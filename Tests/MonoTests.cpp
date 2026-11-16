#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/Mono.h"
#include "Target/RemoteCall.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    void CheckMono(Engine& engine)
    {
        const Target::Process& process = *engine.Process();
        const std::string module = process.Name();

        // The remote-call worker on its own: a pure function must round-trip.
        {
            Target::RemoteCall call;
            const Status started = call.Start(engine.Process());
            CHECK_MSG(started.Ok(), started.Text);
            if (started)
            {
                const std::uint64_t attach = process.FindExport(module, "mono_thread_attach");
                std::uint64_t result = 0;
                CHECK(attach && call.Call(attach, 0xABCDEF12u, result).Ok());
                CHECK_EQ(result, 0xABCDEF12ull);
                const std::uint64_t root = process.FindExport(module, "mono_get_root_domain");
                CHECK(root && call.Call(root, 0, result).Ok() && result != 0);
            }
        }

        const Status mono = engine.EnsureMono();
        CHECK_MSG(mono.Ok(), mono.Text);
        if (!mono)
            return;
        const std::vector<Target::MonoClass> classes = engine.Mono().Classes();
        CHECK_EQ(classes.size(), 2u);
        const auto player = std::find_if(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.Name == "Player"; });
        CHECK(player != classes.end());
        if (player != classes.end())
        {
            CHECK_EQ(player->Namespace, std::string("Game"));
            CHECK_EQ(player->Image, std::string("Assembly-CSharp"));
        }
        const std::vector<std::string> playerMethods = engine.Mono().MethodsOf("Player");
        CHECK(playerMethods.size() == 2 && playerMethods[0] == "TakeDamage" && playerMethods[1] == "Heal");
        const std::vector<Target::MonoField> playerFields = engine.Mono().FieldsOf("Player");
        CHECK_MSG(playerFields.size() == 2, "static field should be skipped, got " + std::to_string(playerFields.size()));
        if (playerFields.size() == 2)
        {
            CHECK(playerFields[0].Name == "health" && playerFields[0].Offset == 0x10 && playerFields[0].TypeCode == 0x08);
            CHECK(playerFields[1].Name == "ammo" && playerFields[1].Offset == 0x1C && playerFields[1].TypeCode == 0x08);
        }
        // Class-from-address, the Structure Dissect lookup.
        const auto object = engine.Evaluate("TestTarget.g_fakePlayer");
        CHECK(object.has_value());
        if (object)
        {
            const auto className = engine.Mono().ClassNameAt(*object);
            CHECK_MSG(className && *className == "Game.Player", className ? *className : "nil");
            CHECK(!engine.Mono().ClassNameAt(*object + 0x100000).has_value());
        }
        CHECK(std::any_of(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.Name == "GameManager"; }));
        const std::vector<std::string> managerMethods = engine.Mono().MethodsOf("GameManager");
        CHECK(managerMethods.size() == 1 && managerMethods[0] == "Update");

        const auto take = engine.Mono().AddressOf("Player:TakeDamage");
        const auto heal = engine.Mono().AddressOf("Game.Player:Heal");
        CHECK_MSG(take && *take, "Player:TakeDamage did not resolve");
        CHECK_MSG(heal && *heal, "Game.Player:Heal did not resolve");
        if (take)
        {
            const auto main = process.MainModule();
            CHECK_MSG(main && *take >= main->Base && *take < main->Base + main->Size, "the JIT address is not in the module");
        }
        CHECK(!engine.Mono().AddressOf("Nope:Missing").has_value());
        CHECK(engine.Mono().FieldOffset("Player", "ammo") == 0x1C);
        CHECK(engine.Mono().FieldOffset("Game.Player", "health") == 0x10);

        // The combined symbol provider resolves Mono names too.
        Status error;
        const auto viaSymbols = engine.Evaluate("Player:TakeDamage", &error);
        CHECK_MSG(viaSymbols && viaSymbols == take, error.Text);

        // The Lua API over the same bridge.
        std::string luaResult;
        const Status lua = engine.Lua().RunWithResult(
            "if not mono_isPresent() then return 'absent' end "
            "local classes = mono_enumClasses() "
            "local methods = mono_class_getMethods('Player') "
            "return tostring(#classes) .. ',' .. tostring(#methods) .. ',' .. "
            "string.format('%X', mono_getAddress('Player', 'TakeDamage') or 0) .. ',' .. "
            "tostring(mono_class_getFieldOffset('Game.Player', 'ammo'))",
            "mono lua", luaResult);
        CHECK_MSG(lua.Ok(), lua.Text);
        if (lua)
        {
            const std::string expected = "2,2," + Hex(*take) + ",28";
            CHECK_EQ(luaResult, expected);
        }
    }

    // Attaches to an already-running process and lists its Mono runtime. Only
    // our own pages are written; the game memory is read, not changed.
    int RunMonoDump(const std::vector<std::string>& arguments)
    {
        std::string name = "CloverPit";
        std::uint32_t pid = 0;
        std::uint64_t object = 0;
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--process=", 0) == 0)
                name = argument.substr(10);
            else if (argument.rfind("--pid=", 0) == 0)
                pid = static_cast<std::uint32_t>(ParseDecimal(argument.substr(6)).value_or(0));
            else if (argument.rfind("--object=", 0) == 0)
                object = ParseHex(argument.substr(9)).value_or(0);
        }
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Settings settings;
        Engine engine(settings);
        const Status opened = pid ? engine.OpenProcess(pid) : engine.OpenProcessByName(name);
        if (!opened)
        {
            std::printf("could not open %s: %s\n", pid ? std::to_string(pid).c_str() : name.c_str(), opened.Text.c_str());
            return 2;
        }
        std::printf("opened %s (pid %lu, %s)\n", engine.Process()->Name().c_str(), static_cast<unsigned long>(engine.Process()->Id()),
            engine.Process()->Is64Bit() ? "64-bit" : "32-bit");
        const auto start = std::chrono::steady_clock::now();
        const Status mono = engine.EnsureMono();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!mono)
        {
            std::printf("no Mono: %s\n", mono.Text.c_str());
            return 1;
        }
        const std::vector<Target::MonoClass> classes = engine.Mono().Classes();
        std::printf("Mono in %s: %zu classes in %.2fs (members load on demand)\n", engine.Mono().Module().c_str(), classes.size(), seconds);
        const std::size_t show = std::min<std::size_t>(classes.size(), 18);
        for (std::size_t i = 0; i < show; ++i)
        {
            const Target::MonoClass& c = classes[i];
            const std::size_t methods = engine.Mono().MethodsOf(c.FullName()).size();
            const std::size_t fields = engine.Mono().FieldsOf(c.FullName()).size();
            std::printf("  %-44s  %3zu methods  %3zu fields\n", c.FullName().c_str(), methods, fields);
        }
        if (classes.size() > show)
            std::printf("  ... and %zu more\n", classes.size() - show);

        // Prove address resolution on the first class that has a method, and a field offset.
        for (const Target::MonoClass& c : classes)
        {
            const std::vector<std::string> classMethods = engine.Mono().MethodsOf(c.FullName());
            if (classMethods.empty())
                continue;
            const std::string full = c.FullName() + ":" + classMethods.front();
            const auto address = engine.Mono().AddressOf(full);
            std::printf("resolve %s -> %s", full.c_str(), address ? Hex(*address).c_str() : "nil");
            if (address)
            {
                const auto module = engine.Process()->ModuleAt(*address);
                std::printf("  (%s)", module ? module->Name.c_str() : "not in a module");
            }
            std::printf("\n");
            const std::vector<Target::MonoField> classFields = engine.Mono().FieldsOf(c.FullName());
            if (!classFields.empty())
                std::printf("field %s:%s offset 0x%X\n", c.FullName().c_str(), classFields.front().Name.c_str(),
                    engine.Mono().FieldOffset(c.FullName(), classFields.front().Name).value_or(-1));
            break;
        }
        if (object)
        {
            const auto vtable = engine.Process()->ReadPointer(object);
            const auto klass = vtable ? engine.Process()->ReadPointer(*vtable) : std::nullopt;
            const auto className = engine.Mono().ClassNameAt(object);
            std::printf("object %s: vtable %s, klass %s -> class %s\n", Hex(object).c_str(),
                vtable ? Hex(*vtable).c_str() : "unreadable", klass ? Hex(*klass).c_str() : "unreadable",
                className ? className->c_str() : "not resolved");
            if (className)
            {
                for (const Target::MonoField& field : engine.Mono().FieldsOf(*className))
                    std::printf("  +0x%-4X %s\n", field.Offset, field.Name.c_str());
            }
        }
        std::printf("done; detaching\n");
        return 0;
    }

    int RunMonoCheck(const std::vector<std::string>& arguments)
    {
        (void)arguments;
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::filesystem::path targetPath = std::filesystem::path(self).parent_path() / L"TestTarget.exe";
        std::wstring command = L"\"" + targetPath.wstring() + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION info{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        {
            std::printf("could not start %ls\n", targetPath.c_str());
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        const int before = Totals().Failures;
        {
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(info.dwProcessId);
            CHECK_MSG(opened.Ok(), opened.Text);
            if (opened)
                CheckMono(engine);
        }
        ::TerminateProcess(info.hProcess, 0);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        const int failures = Totals().Failures - before;
        std::printf("mono check: %d checks, %d failed\n", Totals().Checks, failures);
        return failures == 0 ? 0 : 1;
    }
}
