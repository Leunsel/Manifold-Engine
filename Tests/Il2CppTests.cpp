#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/RemoteCall.h"
#include "Target/UnityExplorer.h"

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
    void SettleMono(Engine& engine, int milliseconds);

    namespace
    {
        // TestTarget.exe --il2cpp: its il2cpp_* exports answer and its fake
        // Mono has no root domain, so the bridge has to pick IL2CPP by itself.
        struct Il2CppTarget
        {
            PROCESS_INFORMATION Info{};

            ~Il2CppTarget()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }

            bool Start(const std::filesystem::path& path)
            {
                SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
                HANDLE read = nullptr;
                HANDLE write = nullptr;
                if (!::CreatePipe(&read, &write, &security, 0))
                    return false;
                ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOW startup{ sizeof(startup) };
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write;
                startup.hStdError = write;
                std::wstring command = L"\"" + path.wstring() + L"\" --il2cpp";
                const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &Info);
                ::CloseHandle(write);
                std::string text;
                char buffer[512];
                DWORD got = 0;
                while (started && text.find("shoot at") == std::string::npos)
                {
                    if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                        break;
                    text.append(buffer, got);
                }
                ::CloseHandle(read);
                return started && text.find("shoot at") != std::string::npos;
            }
        };

        std::uint64_t ReadValue(const Target::Process& process, std::uint64_t address)
        {
            std::uint64_t value = 0;
            process.ReadExact(address, &value, sizeof(value));
            return value;
        }

        std::int32_t ReadInt(const Target::Process& process, std::uint64_t address)
        {
            std::int32_t value = 0;
            process.ReadExact(address, &value, sizeof(value));
            return value;
        }

        const Target::MonoMethod* Method(const Target::MonoClass& info, const std::string& name, int params)
        {
            for (const Target::MonoMethod& method : info.Methods)
            {
                if (method.Name == name && (params < 0 || method.ParamCount == params))
                    return &method;
            }
            return nullptr;
        }
    }

    void CheckIl2Cpp(Engine& engine)
    {
        const Target::Process& process = *engine.Process();
        const std::string module = process.Name();
        const auto main = process.MainModule();
        CHECK(main.has_value());
        if (!main)
            return;
        const auto inModule = [&](std::uint64_t address) { return address >= main->Base && address < main->Base + main->Size; };
        const std::uint64_t player = engine.Evaluate("TestTarget.g_il2cppPlayer").value_or(0);
        CHECK(player != 0);

        // The drain stub on its own: one call walks a whole iterator, with the
        // 8 bytes at an offset of each item or a peek function next to it.
        {
            Target::RemoteCall call;
            const Status started = call.Start(engine.Process());
            CHECK_MSG(started.Ok(), started.Text);
            const std::uint64_t methods = process.FindExport(module, "il2cpp_class_get_methods");
            const std::uint64_t fields = process.FindExport(module, "il2cpp_class_get_fields");
            const std::uint64_t pointer = process.FindExport(module, "fake_method_pointer");
            const std::uint64_t klass = ReadValue(process, player);
            std::vector<Target::RemoteCall::DrainItem> byOffset;
            std::vector<Target::RemoteCall::DrainItem> byFunction;
            std::vector<Target::RemoteCall::DrainItem> plain;
            CHECK(started && call.Drain(methods, klass, byOffset, 0).Ok());
            CHECK(started && call.Drain(methods, klass, byFunction, Target::RemoteCall::kNoPeek, pointer).Ok());
            CHECK(started && call.Drain(fields, klass, plain).Ok());
            CHECK_EQ(byOffset.size(), 4u);
            CHECK_EQ(byFunction.size(), 4u);
            for (std::size_t i = 0; i < std::min(byOffset.size(), byFunction.size()); ++i)
            {
                CHECK(byOffset[i].Item == byFunction[i].Item);
                CHECK(byOffset[i].Peek != 0 && byOffset[i].Peek == byFunction[i].Peek && inModule(byOffset[i].Peek));
                CHECK_EQ(ReadValue(process, byOffset[i].Item), byOffset[i].Peek);
            }
            CHECK_EQ(plain.size(), 4u);
            CHECK(std::all_of(plain.begin(), plain.end(), [](const Target::RemoteCall::DrainItem& item) { return item.Item != 0 && item.Peek == 0; }));
            std::vector<Target::RemoteCall::DrainItem> limited;
            CHECK(started && call.Drain(methods, klass, limited, 0, 0, 2).Ok());
            CHECK_EQ(limited.size(), 2u);
            call.Stop();
        }

        const std::int32_t detachedBefore = ReadInt(process, engine.Evaluate("TestTarget.g_il2cppDetached").value_or(0));
        const Status loaded = engine.EnsureMono();
        CHECK_MSG(loaded.Ok(), loaded.Text);
        if (!loaded)
            return;
        Target::MonoBridge& bridge = engine.Mono();
        CHECK(bridge.IsIl2Cpp());
        CHECK(bridge.Runtime() == Target::ManagedRuntime::Il2Cpp);
        CHECK_EQ(bridge.Module(), module);
        CHECK(ReadInt(process, engine.Evaluate("TestTarget.g_il2cppAttached").value_or(0)) >= 1);

        // Classes: "<Module>" is left out, image names lose their ".dll".
        const std::vector<Target::MonoClass> classes = bridge.Classes();
        CHECK_EQ(classes.size(), 5u);
        const auto found = std::find_if(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.FullName() == "Game.Player"; });
        CHECK(found != classes.end());
        if (found != classes.end())
            CHECK_EQ(found->Image, std::string("Assembly-CSharp"));
        CHECK(std::none_of(classes.begin(), classes.end(), [](const Target::MonoClass& c) { return c.Name == "<Module>"; }));

        // Members: parents, parameter counts, overloads, code addresses
        // (MethodInfo::methodPointer at offset 0), instance fields only.
        std::uint64_t take = 0;
        std::uint64_t getter = 0;
        if (const auto details = bridge.ClassDetails("Game.Player"))
        {
            CHECK_EQ(details->Parent, std::string("Game.Actor"));
            CHECK(!details->Generic);
            CHECK_EQ(details->Methods.size(), 4u);
            const Target::MonoMethod* takeDamage = Method(*details, "TakeDamage", 1);
            const Target::MonoMethod* heal = Method(*details, "Heal", 1);
            const Target::MonoMethod* healMore = Method(*details, "Heal", 2);
            const Target::MonoMethod* health = Method(*details, "get_Health", 0);
            CHECK(takeDamage && heal && healMore && health);
            if (takeDamage && heal && healMore && health)
            {
                take = takeDamage->Address;
                getter = health->Address;
                CHECK(inModule(take) && inModule(heal->Address) && inModule(healMore->Address) && inModule(getter));
                CHECK(heal->Address != healMore->Address);
                CHECK_EQ(ReadValue(process, takeDamage->Handle), take);
            }
            CHECK_EQ(details->Fields.size(), 3u);
            if (details->Fields.size() == 3)
            {
                CHECK(details->Fields[0].Name == "health" && details->Fields[0].Offset == 0x18 && details->Fields[0].TypeCode == 0x08);
                CHECK(details->Fields[1].Name == "ammo" && details->Fields[1].Offset == 0x1C && details->Fields[1].TypeCode == 0x08);
                CHECK(details->Fields[2].Name == "speed" && details->Fields[2].Offset == 0x20 && details->Fields[2].TypeCode == 0x0C);
            }
        }
        else
        {
            CHECK_MSG(false, "Game.Player has no details");
        }
        if (const auto list = bridge.ClassDetails("List`1"))
        {
            CHECK(list->Generic);
            CHECK(list->Fields.empty());
            CHECK_EQ(list->Parent, std::string("System.Object"));
            const Target::MonoMethod* add = Method(*list, "Add", 1);
            CHECK(add && add->Address == 0);
        }
        else
        {
            CHECK_MSG(false, "List`1 has no details");
        }
        if (const auto manager = bridge.ClassDetails("GameManager"))
        {
            const Target::MonoMethod* level = Method(*manager, "get_Level", 0);
            CHECK(level && level->Address == getter && getter != 0);
            const Target::MonoMethod* onEvent = Method(*manager, "OnEvent", 1);
            CHECK(onEvent && onEvent->Address == 0);
        }
        const std::vector<std::string> names = bridge.MethodsOf("Player");
        CHECK(names.size() == 4 && names[0] == "TakeDamage" && names[1] == "Heal" && names[3] == "get_Health");
        CHECK(bridge.FieldOffset("Player", "ammo") == 0x1C);
        CHECK(bridge.FieldOffset("Game.Player", "speed") == 0x20);
        CHECK(!bridge.FieldOffset("Player", "s_count").has_value());

        // Name -> address, also with the image in front, and in expressions
        // where the dash of Assembly-CSharp is part of the name.
        CHECK(take != 0 && bridge.AddressOf("Player:TakeDamage") == take);
        CHECK(take != 0 && bridge.AddressOf("Assembly-CSharp:Player:TakeDamage") == take);
        CHECK(take != 0 && bridge.AddressOf("Assembly-CSharp.dll:Game.Player:TakeDamage") == take);
        CHECK(!bridge.AddressOf("mscorlib:Player:TakeDamage").has_value());
        CHECK(!bridge.AddressOf("GameManager:OnEvent").has_value());
        CHECK(!bridge.AddressOf("Nope:Missing").has_value());
        Status error;
        CHECK_MSG(engine.Evaluate("Player:TakeDamage", &error) == take, error.Text);
        CHECK_MSG(engine.Evaluate("Assembly-CSharp:Player:TakeDamage", &error) == take, error.Text);
        CHECK_MSG(engine.Evaluate("Assembly-CSharp:Game.Player:TakeDamage+4", &error) == take + 4, error.Text);
        CHECK_MSG(engine.Evaluate("\"Assembly-CSharp:Player:TakeDamage\"+4", &error) == take + 4, error.Text);
        CHECK(engine.Evaluate("TestTarget.exe+10-4") == main->Base + 0xC);

        // Address -> name, from the sorted table of method code, both in the
        // bridge and through the symbol provider (it beats a nearby export).
        if (take)
        {
            const auto atStart = bridge.MethodNameAt(take);
            CHECK_MSG(atStart && *atStart == "Game.Player:TakeDamage", atStart ? *atStart : "nil");
            const auto inside = bridge.MethodNameAt(take + 1);
            CHECK_MSG(inside && *inside == "Game.Player:TakeDamage", inside ? *inside : "nil");
            CHECK(bridge.MethodStartNameAt(take) == std::optional<std::string>("Game.Player:TakeDamage"));
            CHECK(!bridge.MethodStartNameAt(take + 1).has_value());
            const auto range = bridge.MethodRange(take + 1);
            CHECK(range && range->first == take && range->second > take + 1);
            CHECK_EQ(engine.Describe(take), std::string("Game.Player:TakeDamage"));
            CHECK_EQ(engine.Describe(take + 1), std::string("Game.Player:TakeDamage+1"));
            CHECK_EQ(bridge.MethodsSharingCode(take), 1u);
        }
        if (getter)
        {
            CHECK_EQ(bridge.MethodsSharingCode(getter), 2u);
            const auto shared = bridge.MethodNameAt(getter);
            CHECK_MSG(shared && *shared == "Game.Player:get_Health", shared ? *shared : "nil");
        }
        CHECK(!bridge.MethodNameAt(0).has_value());
        CHECK(!bridge.MethodNameAt(main->Base).has_value());
        CHECK(!bridge.MethodNameAt(main->Base + main->Size + 0x1000).has_value());
        const std::uint64_t domainGet = process.FindExport(module, "il2cpp_domain_get");
        CHECK_EQ(engine.Describe(domainGet), std::string("TestTarget.il2cpp_domain_get"));

        // Structure Dissect's lookups: the object points straight at its
        // class; a generic instance no image lists is named by its shape;
        // anything else is left alone.
        if (player)
        {
            CHECK(bridge.ClassNameAt(player) == std::optional<std::string>("Game.Player"));
            const auto inside = bridge.ClassAt(player + 0x18);
            CHECK_MSG(inside && inside->Base == player && inside->Name == "Game.Player", inside ? inside->Name + " at " + Hex(inside->Base) : "nil");
            CHECK(!bridge.ClassAt(player + 0x100000).has_value());
            CHECK(!bridge.ClassNameAt(player + 8).has_value());
        }
        const std::size_t before = bridge.ClassCount();
        const std::uint64_t list = engine.Evaluate("TestTarget.g_il2cppList").value_or(0);
        const auto listName = bridge.ClassNameAt(list);
        CHECK_MSG(listName && *listName == "System.Collections.Generic.List`1", listName ? *listName : "nil");
        CHECK_EQ(bridge.ClassCount(), before + 1);
        CHECK(bridge.FieldsOf("System.Collections.Generic.List`1").empty());
        CHECK(!bridge.ClassNameAt(engine.Evaluate("TestTarget.g_il2cppWild").value_or(0)).has_value());
        CHECK(!bridge.ClassNameAt(engine.Evaluate("TestTarget.g_il2cppNotClass").value_or(0)).has_value());
        CHECK(!bridge.ClassNameAt(engine.Evaluate("TestTarget.g_il2cppStray").value_or(0)).has_value());

        // The Unity explorer is Mono only and refuses, even switched on.
        Target::UnityExplorer& unity = bridge.Unity();
        unity.SetEnabled(true);
        unity.ReadScenes();
        CHECK_MSG(unity.State().Error.find("IL2CPP") != std::string::npos, unity.State().Error);
        CHECK(!unity.State().Hooked);
        unity.SetEnabled(false);

        // The mono_* Lua functions serve IL2CPP too, as in Cheat Engine.
        std::string luaResult;
        const Status lua = engine.Lua().RunWithResult(
            "local methods = mono_class_getMethods('Actor') "
            "return tostring(mono_isPresent()) .. ',' .. tostring(mono_isIl2Cpp()) .. ',' .. tostring(#mono_enumClasses()) .. ',' .. "
            "tostring(#mono_class_getMethods('Game.Player')) .. ',' .. tostring(methods and #methods) .. ',' .. "
            "string.format('%X', mono_getAddress('Player', 'TakeDamage') or 0) .. ',' .. tostring(mono_class_getFieldOffset('Game.Player', 'speed')) .. ',' .. "
            "string.format('%X', getAddress('Assembly-CSharp:Player:TakeDamage'))",
            "il2cpp lua", luaResult);
        CHECK_MSG(lua.Ok(), lua.Text);
        if (lua && take)
            CHECK_EQ(luaResult, "true,true,6,4,0," + Hex(take) + ",32," + Hex(take));

        // Loading in the background, a reload (which drops classes found only
        // by their shape) and Shutdown, which takes the worker off IL2CPP's
        // thread list.
        engine.StopMono();
        CHECK(!engine.MonoPresent());
        CHECK(!bridge.IsIl2Cpp());
        CHECK_EQ(ReadInt(process, engine.Evaluate("TestTarget.g_il2cppDetached").value_or(0)), detachedBefore + 1);
        CHECK(!engine.Evaluate("Player:TakeDamage").has_value());
        engine.LoadMono(true);
        CHECK(engine.MonoLoading());
        SettleMono(engine, 5000);
        CHECK(!engine.MonoLoading());
        CHECK(engine.MonoPresent() && bridge.IsIl2Cpp());
        CHECK_EQ(bridge.ClassCount(), 5u);
        CHECK(take != 0 && engine.Evaluate("Player:TakeDamage") == take);
        bridge.ClassNameAt(list);
        const std::uint64_t generation = bridge.Generation();
        engine.ReloadMono(true);
        SettleMono(engine, 5000);
        CHECK(bridge.Generation() != generation);
        CHECK_EQ(bridge.ClassCount(), 5u);
        CHECK(take != 0 && bridge.MethodNameAt(take) == std::optional<std::string>("Game.Player:TakeDamage"));
        engine.StopMono();
    }

    void CheckIl2CppTarget(const std::filesystem::path& targetPath)
    {
        Il2CppTarget target;
        const bool started = target.Start(targetPath);
        CHECK_MSG(started, "could not start " + WideToUtf8(targetPath.wstring()) + " --il2cpp");
        if (!started)
            return;
        Settings settings;
        Engine engine(settings);
        const Status opened = engine.OpenProcess(target.Info.dwProcessId);
        CHECK_MSG(opened.Ok(), opened.Text);
        if (opened)
            CheckIl2Cpp(engine);
        engine.CloseProcess();
    }

    int RunIl2CppCheck(const std::vector<std::string>&)
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        const int before = Totals().Failures;
        CheckIl2CppTarget(std::filesystem::path(self).parent_path() / L"TestTarget.exe");
        const int failures = Totals().Failures - before;
        std::printf("il2cpp check: %d checks, %d failed\n", Totals().Checks, failures);
        return failures == 0 ? 0 : 1;
    }
}
