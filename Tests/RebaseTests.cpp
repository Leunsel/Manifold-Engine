#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/Process.h"
#include "Target/Rebase.h"
#include "Target/Symbols.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace Manifold::Tests
{
    namespace
    {
        using Target::RebaseRule;
        using Target::Rebases;

        void Rules()
        {
            CHECK(Rebases::ParseRule("image") == (RebaseRule{ true, 0 }));
            CHECK(Rebases::ParseRule(" Image ") == (RebaseRule{ true, 0 }));
            CHECK(Rebases::ParseRule("0x140000000") == (RebaseRule{ false, 0x140000000ull }));
            CHECK(Rebases::ParseRule("140000000") == (RebaseRule{ false, 0x140000000ull }));
            CHECK(Rebases::ParseRule("0") == (RebaseRule{ false, 0 }));
            CHECK(!Rebases::ParseRule(""));
            CHECK(!Rebases::ParseRule("-10"));
            CHECK(!Rebases::ParseRule("zz"));
            CHECK(Rebases::RuleText({ true, 0 }) == "image");
            CHECK(Rebases::ParseRule(Rebases::RuleText({ false, 0x10000000ull })) == (RebaseRule{ false, 0x10000000ull }));

            Rebases rebases;
            CHECK(!rebases.Any());
            rebases.Load({ { "Game.exe", "image" }, { "engine.dll", "0x180000000" }, { "broken.dll", "nope" } });
            CHECK(rebases.Any() && rebases.Entries().size() == 2);
            CHECK(rebases.Rule("GAME.EXE") == (RebaseRule{ true, 0 }));
            CHECK(!rebases.Rule("broken.dll"));
            rebases.Set("game.exe", std::nullopt);
            CHECK(!rebases.Rule("Game.exe") && rebases.Entries().size() == 1);
            const auto saved = rebases.Saved();
            CHECK(saved.size() == 1 && saved.count("engine.dll") == 1 && saved.at("engine.dll") == "0x180000000");

            Settings written;
            written.Rebases = { { "engine.dll", "0x180000000" }, { "Game.exe", "image" } };
            const std::filesystem::path file = std::filesystem::temp_directory_path() / ("manifold-rebase-" + std::to_string(::GetCurrentProcessId()) + ".ini");
            CHECK(written.Save(file).Ok());
            Settings read;
            CHECK(read.Load(file));
            CHECK_MSG(read.Rebases == written.Rebases, "rules read back: " + std::to_string(read.Rebases.size()));
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
        }

        void Live()
        {
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened.Ok())
                return;
            const Target::Process& process = *engine.Process();
            Target::Symbols& symbols = engine.Symbols();
            const auto ntdll = process.FindModule("ntdll.dll");
            CHECK(ntdll.has_value());
            if (!ntdll)
                return;

            // ntdll always asks for a base the loader never gives it; the file holds the asked one, memory the given one.
            const auto image = symbols.Rebase().ImageBase(process, *ntdll);
            const auto inMemory = [&]() -> std::uint64_t
                {
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(ntdll->Base);
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(ntdll->Base + static_cast<std::uint64_t>(dos->e_lfanew));
                    return nt->OptionalHeader.ImageBase;
                }();
            CHECK(image.has_value());
            if (!image)
                return;
            std::printf("  ntdll.dll at %s, image base %s (header in memory says %s)\n", Hex(ntdll->Base).c_str(), Hex(*image).c_str(), Hex(inMemory).c_str());
            CHECK(*image != ntdll->Base);

            const std::uint64_t inside = ntdll->Base + 0x1234;
            CHECK(!symbols.Rebased(inside));
            engine.SetRebase("NTDLL.DLL", RebaseRule{ true, 0 });
            CHECK(settings.Rebases.size() == 1 && settings.Rebases.begin()->second == "image");
            CHECK(symbols.Rebased(inside) == *image + 0x1234);
            CHECK(symbols.AddressText(inside, Target::AddressStyle::Rebased) == Hex(*image + 0x1234));
            const auto back = symbols.FromRebased(*image + 0x1234);
            CHECK(back && back->Address == inside && EqualsNoCase(back->Module.Name, "ntdll.dll") && back->ShownBase == *image);
            CHECK(!symbols.FromRebased(*image + ntdll->Size));
            CHECK(symbols.Rebased(ntdll->Base + ntdll->Size - 1) == *image + ntdll->Size - 1);
            int local = 0;
            CHECK(!symbols.Rebased(reinterpret_cast<std::uint64_t>(&local)));
            // Only the screen changes: symbol expressions still read the real addresses.
            CHECK(engine.Evaluate("ntdll.dll+1234") == inside);

            engine.SetRebase("ntdll.dll", RebaseRule{ false, 0 });
            CHECK(symbols.Rebased(inside) == 0x1234ull);
            const auto rva = symbols.FromRebased(0x1234);
            CHECK(rva && rva->Address == inside);

            const std::string base = std::to_string(ntdll->Base);
            const std::string script =
                "assert(manifold.rebase.set('ntdll.dll', 'image'))\n"
                "local entry = assert(manifold.rebase.get('NTDLL.dll'))\n"
                "assert(entry.rule == 'image' and entry.base == manifold.rebase.imageBase('ntdll.dll'), tostring(entry.base))\n"
                "assert(manifold.rebase.shown(" + base + " + 0x10) == entry.base + 0x10)\n"
                "assert(manifold.rebase.real(entry.base + 0x10) == " + base + " + 0x10)\n"
                "assert(manifold.rebase.real(0x10) == 0x10)\n"
                "assert(manifold.rebase.shown('ntdll.dll+20') == entry.base + 0x20)\n"
                "assert(#manifold.rebase.list() == 1 and manifold.rebase.list()[1].module == 'ntdll.dll')\n"
                "assert(manifold.rebase.set('ntdll.dll', 0x10000000) and manifold.rebase.get('ntdll.dll').base == 0x10000000)\n"
                "assert(not pcall(manifold.rebase.set, 'ntdll.dll', 'preferred'))\n"
                "assert(not pcall(manifold.rebase.set, '', 'image'))\n"
                "assert(not pcall(manifold.rebase.set, 'ntdll.dll', -1))\n"
                "local none, problem = manifold.rebase.imageBase('no-such-module.dll')\n"
                "assert(none == nil and problem.code == 'ME-1016', tostring(problem and problem.code))\n"
                "assert(manifold.rebase.clear('ntdll.dll') and manifold.rebase.get('ntdll.dll') == nil)\n"
                "assert(#manifold.rebase.list() == 0 and manifold.apiVersion >= 1)\n";
            const Status status = engine.Lua().Run(script, "rebase api");
            CHECK_MSG(status.Ok(), status.Text);
            CHECK(settings.Rebases.empty());
            CHECK(!symbols.Rebased(inside));

            // Rules outlive the process: they are kept by name for the next one.
            engine.SetRebase("ntdll.dll", RebaseRule{ true, 0 });
            engine.CloseProcess();
            CHECK(symbols.Rebase().Rule("ntdll.dll") == (RebaseRule{ true, 0 }));
            CHECK(!symbols.Rebased(inside));
            engine.SetRebase("ntdll.dll", std::nullopt);
        }
    }

    int RunRebaseTests()
    {
        const int before = Totals().Failures;
        Rules();
        Live();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("rebase", "Module rebase", 132, [](const SuiteContext&) { return RunRebaseTests(); });
}
