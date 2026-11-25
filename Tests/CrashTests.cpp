#include "TestKit.h"

#include "App/CrashReport.h"
#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/CrashProtocol.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

#include "Syscalls/Syscall.h"

namespace Manifold::Tests
{
    namespace
    {
        // ---- Attribution from made-up records: every rule, no process ----

        ScriptFootprint Footprint(std::string label, std::uint64_t begin, std::uint64_t end, std::uint64_t on, std::uint64_t off = 0)
        {
            ScriptFootprint footprint;
            footprint.Label = std::move(label);
            footprint.RecordId = 7;
            footprint.Ranges.push_back({ begin, end, ScriptRangeKind::Allocation, "newmem" });
            footprint.OnTick = on;
            footprint.OffTick = off;
            return footprint;
        }

        Target::CrashRecord Record(std::uint32_t code, std::uint64_t ip, std::uint64_t tick, std::uint64_t accessed = 0, std::uint64_t access = 0)
        {
            Target::CrashRecord record;
            record.Code = code;
            record.Address = ip;
            record.Tick = tick;
            record.ThreadId = 42;
            record.Registers[Target::Crash::RegIp] = ip;
            record.Registers[Target::Crash::RegSp] = 0x9000;
            if (code == 0xC0000005u)
                record.Info = { access, accessed };
            return record;
        }

        CrashContext Context(std::uint32_t exitCode, std::uint64_t endTick, std::vector<ScriptFootprint> footprints)
        {
            CrashContext context;
            context.ExitCode = exitCode;
            context.EndTick = endTick;
            context.Process = "Game.exe";
            context.Footprints = std::move(footprints);
            context.Describe = [](std::uint64_t address) { return address >= 0x400000 && address < 0x500000 ? Format("Game.exe+%llX", static_cast<unsigned long long>(address - 0x400000)) : Hex(address); };
            return context;
        }

        Target::CrashCapture Capture(std::vector<Target::CrashRecord> records, bool is64Bit = true)
        {
            Target::CrashCapture capture;
            capture.Records = std::move(records);
            capture.Is64Bit = is64Bit;
            capture.Watching = true;
            return capture;
        }

        void CheckAttribution()
        {
            constexpr std::uint32_t kAccess = 0xC0000005u;
            ScriptFootprint ammo = Footprint("Infinite Ammo", 0x10000, 0x11000, 1000);
            ammo.Labels = { { 0x10000, "newmem" }, { 0x10010, "code" } };
            ammo.Ranges.push_back({ 0x401000, 0x401005, ScriptRangeKind::Patch, {} });

            {
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x10015, 5000, 0, 0) }), Context(kAccess, 5100, { ammo }));
                CHECK(report.Crashed && report.Exact && report.Fatal && report.Blames.size() == 1);
                CHECK_MSG(!report.Blames.empty() && report.Blames[0].Link == CrashLink::InCode && report.Blames[0].Where == "code+5" && report.Blames[0].Label == "Infinite Ammo",
                    report.Headline);
                CHECK_MSG(report.Headline == "Game.exe crashed in the code of \"Infinite Ammo\" (code+5): access violation reading 0000000000000000.", report.Headline);
                CHECK(report.Details.find("rip=0000000000010015") != std::string::npos && report.Details.find("Scripts on at the time: \"Infinite Ammo\"") != std::string::npos);
                CHECK(report.Blames[0].RecordId == 7 && report.SecondsBeforeEnd > 0.09 && report.SecondsBeforeEnd < 0.11);
            }
            {
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x401002, 5000) }), Context(kAccess, 5100, { ammo }));
                CHECK(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::InPatch);
            }
            {
                const CrashReport report = AttributeCrash(Capture({ Record(0xC000001Du, 0x401007, 5000) }), Context(0xC000001Du, 5100, { ammo }));
                CHECK_MSG(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::BehindPatch, report.Headline);
                CHECK(report.Headline.find("illegal instruction") != std::string::npos && report.Headline.find("jump back") != std::string::npos);
            }
            {
                const ScriptFootprint off = Footprint("Old Script", 0x20000, 0x21000, 1000, 3000);
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x20004, 5000, 0x20004, 8) }), Context(kAccess, 5100, { off }));
                CHECK_MSG(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::RanFreed, report.Headline);
                CHECK(report.Headline.find("executing 0000000000020004") != std::string::npos && report.SwitchedOn.empty() && report.LastSwitched == "Old Script");
            }
            {
                // The same memory came back as another script's allocation: the one on at the time is meant.
                const ScriptFootprint first = Footprint("First", 0x30000, 0x31000, 1000, 2000);
                const ScriptFootprint second = Footprint("Second", 0x30000, 0x31000, 2500);
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x30020, 5000) }), Context(kAccess, 5100, { first, second }));
                CHECK(report.Crashed && !report.Blames.empty() && report.Blames[0].Label == "Second" && report.Blames[0].Link == CrashLink::InCode);
            }
            {
                Target::CrashRecord inGame = Record(kAccess, 0x402000, 5000, 0x10, 0);
                inGame.Stack = { 0x1234, 0x7777, 0x10030, 0x402100 };
                const CrashReport report = AttributeCrash(Capture({ inGame }), Context(kAccess, 5100, { ammo }));
                CHECK_MSG(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::OnStack && report.Blames[0].Where == "code+20", report.Headline);
                CHECK(report.Headline.find("Game.exe+2000, called from the code of \"Infinite Ammo\"") != std::string::npos);
                CHECK(report.Details.find("[sp+10] \"Infinite Ammo\" code+20") != std::string::npos && report.Details.find("[sp+18] Game.exe+2100") != std::string::npos);
            }
            {
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x402000, 5000, 0x10800, 1) }), Context(kAccess, 5100, { ammo }));
                CHECK(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::Accessed && report.Blames[0].Where == "code+7F0");
            }
            {
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x10012, 4200), Record(kAccess, 0x402000, 5000) }), Context(kAccess, 5100, { ammo }));
                CHECK(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::Earlier);
                CHECK(report.Fatal && report.Fatal->Address == 0x402000 && report.Details.find("Other exceptions in the last 30 s: 1") != std::string::npos);
            }
            {
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x402000, 5000) }), Context(kAccess, 5100, { ammo }));
                CHECK_MSG(report.Crashed && report.Blames.empty() && report.Headline.find("No script's code was involved; the last script switched was \"Infinite Ammo\"") != std::string::npos,
                    report.Headline);
            }
            {
                // A game that ends itself (its crash handler, exit code 3) right after an exception in a script.
                const CrashReport scripted = AttributeCrash(Capture({ Record(kAccess, 0x10015, 5000) }), Context(3, 6000, { ammo }));
                CHECK(scripted.Crashed && !scripted.Exact && !scripted.Blames.empty());
                const CrashReport unrelated = AttributeCrash(Capture({ Record(kAccess, 0x402000, 5000) }), Context(3, 6000, { ammo }));
                CHECK(!unrelated.Crashed);
                const CrashReport normal = AttributeCrash(Capture({ Record(kAccess, 0x10015, 5000) }), Context(0, 60000, { ammo }));
                CHECK(!normal.Crashed);
            }
            {
                const CrashReport old = AttributeCrash(Capture({ Record(kAccess, 0x10015, 1000) }), Context(kAccess, 50000, { ammo }));
                CHECK(old.Crashed && !old.Fatal && old.Blames.empty());
                Target::CrashCapture unwatched;
                const CrashReport blind = AttributeCrash(unwatched, Context(0xC00000FDu, 5000, {}));
                CHECK_MSG(blind.Crashed && !blind.Watched && blind.Headline == "Game.exe ended with stack overflow (C00000FD); no crash watch was in the game, so where it happened is not known.",
                    blind.Headline);
            }
            {
                // The owner's case: push eax without its pop, the crash comes later in the game's own code.
                ScriptFootprint damage = Footprint("Damage : Multiplier ()->", 0x60000, 0x61000, 3000);
                damage.Ranges.push_back({ 0x4C98C0, 0x4C98C5, ScriptRangeKind::Patch, {} });
                damage.Warnings.push_back({ 5, "the stack is 4 bytes off at the jump back in line 9: push eax in line 5 has no matching pop. The game's code after it will read the wrong values and likely crash." });
                const CrashReport report = AttributeCrash(Capture({ Record(kAccess, 0x4C98EF, 5000, 0x2F24, 0) }, false), Context(kAccess, 9400, { damage }));
                CHECK_MSG(report.Crashed && !report.Blames.empty() && report.Blames[0].Link == CrashLink::Unbalanced && report.Blames.size() == 1, report.Headline);
                CHECK_MSG(report.Headline == "Game.exe crashed in Game.exe+C98EF: access violation reading 00002F24. \"Damage : Multiplier ()->\" is the likely cause: "
                                             "the stack is 4 bytes off at the jump back in line 9: push eax in line 5 has no matching pop.",
                    report.Headline);
                CHECK(report.Details.find("its hook leaves the stack unbalanced") != std::string::npos);
                damage.Warnings.clear();
                const CrashReport nearby = AttributeCrash(Capture({ Record(kAccess, 0x4C98EF, 5000, 0x2F24, 0) }, false), Context(kAccess, 9400, { damage }));
                CHECK_MSG(nearby.Crashed && !nearby.Blames.empty() && nearby.Blames[0].Link == CrashLink::NearPatch && nearby.Blames[0].Where == "2F bytes behind its patch at Game.exe+C98C0",
                    nearby.Headline);
                CHECK(nearby.Headline.find("That is 2F bytes behind its patch at Game.exe+C98C0 of \"Damage : Multiplier ()->\"") != std::string::npos);
                const CrashReport distant = AttributeCrash(Capture({ Record(kAccess, 0x4CA0EF, 5000, 0x2F24, 0) }, false), Context(kAccess, 9400, { damage }));
                CHECK(distant.Crashed && distant.Blames.empty());
            }
            {
                Target::CrashRecord small = Record(kAccess, 0x10015, 5000, 0x44, 1);
                small.Stack = { 0x10020 };
                const CrashReport report = AttributeCrash(Capture({ small }, false), Context(kAccess, 5100, { ammo }));
                CHECK(report.Details.find("eip=00010015") != std::string::npos && report.Details.find("r8=") == std::string::npos);
                CHECK(report.Headline.find("writing 00000044") != std::string::npos);
            }
        }

        void CheckLedger()
        {
            CrashLedger ledger;
            Asm::AaResult result;
            result.Writes.push_back({ 0x10000, std::vector<std::uint8_t>(32, 0x90), 3 });
            result.Writes.push_back({ 0x401000, { 0xE9, 0, 0, 0, 0 }, 7 });
            result.Writes.push_back({ 0x401005, { 0x90 }, 8 });
            result.Writes.push_back({ 0x402000, { 0x90, 0x90 }, 9 });
            result.Symbols = { { "newmem", 0x10000 }, { "code", 0x10010 }, { "INJECT", 0x401000 } };
            Asm::AaMemory before;
            Asm::AaMemory after;
            after.Allocations.push_back({ "newmem", 0x10000, 0x1000, 0x10000, false });
            ledger.Note("Infinite Ammo", 7, 1, Asm::AaSection::Enable, result, before, after, 1000);
            std::vector<ScriptFootprint> footprints = ledger.Footprints();
            CHECK(footprints.size() == 1);
            if (footprints.size() != 1)
                return;
            const ScriptFootprint& noted = footprints.front();
            CHECK(noted.Label == "Infinite Ammo" && noted.RecordId == 7 && noted.OnTick == 1000 && noted.OffTick == 0);
            CHECK_MSG(noted.Ranges.size() == 3, std::to_string(noted.Ranges.size()));
            CHECK(noted.Ranges.size() == 3 && noted.Ranges[1].Kind == ScriptRangeKind::Patch && noted.Ranges[1].Begin == 0x401000 && noted.Ranges[1].End == 0x401006);
            CHECK(noted.Labels.size() == 2 && noted.Labels.count(0x10010) && !noted.Labels.count(0x401000));

            ledger.Note("Infinite Ammo", 7, 1, Asm::AaSection::Enable, result, after, after, 1500);
            CHECK(ledger.Footprints().size() == 1 && ledger.Footprints().front().OnTick == 1500);
            // One record's Lua running a second hook script (other places): a footprint of its own.
            Asm::AaResult second;
            second.Writes.push_back({ 0x20000, std::vector<std::uint8_t>(16, 0x90), 3 });
            second.Writes.push_back({ 0x403000, { 0xE9, 0, 0, 0, 0 }, 7 });
            Asm::AaMemory secondMemory;
            secondMemory.Allocations.push_back({ "mem2", 0x20000, 0x1000, 0x20000, false });
            ledger.Note("Infinite Ammo", 7, 2, Asm::AaSection::Enable, second, before, secondMemory, 1600);
            CHECK(ledger.Footprints().size() == 2);
            ledger.Note("Infinite Ammo", 7, 1, Asm::AaSection::Disable, Asm::AaResult{}, after, before, 2000);
            footprints = ledger.Footprints();
            CHECK(footprints.size() == 2 && footprints.front().OffTick == 2000 && footprints.back().OffTick == 0);
            ledger.Note("Infinite Ammo", 7, 1, Asm::AaSection::Enable, result, before, after, 3000);
            CHECK(ledger.Footprints().size() == 3 && ledger.Footprints().back().OffTick == 0);
            Asm::AaResult failed;
            failed.Error = Status::Fail(ErrorCode::LuaRuntime, "no");
            failed.Writes = result.Writes;
            ledger.Note("Broken", 9, 3, Asm::AaSection::Enable, failed, before, after, 3500);
            CHECK(ledger.Footprints().size() == 3);
            // A run that puts the patched bytes back switches the script off whatever its name, and
            // leaves no footprint of its own: Lua's autoAssemble(enable) / autoAssemble(disable) pair
            // (two texts), a released orphan (no record), a script edited while it was on.
            {
                CrashLedger pairs;
                Asm::AaResult enable;
                enable.Writes.push_back({ 0x30000, std::vector<std::uint8_t>(8, 0x90), 2 });
                enable.Writes.push_back({ 0x405000, { 0xE9, 1, 2, 3, 4 }, 4 });
                Asm::AaMemory allocated;
                allocated.Allocations.push_back({ "luamem", 0x30000, 0x1000, 0x30000, false });
                pairs.Note("autoAssemble", 0, 10, Asm::AaSection::Whole, enable, before, allocated, 100);
                Asm::AaResult disable;
                disable.Writes.push_back({ 0x405000, { 0x8B, 0x45, 0x08, 0x89, 0x01 }, 2 });
                pairs.Note("autoAssemble", 0, 11, Asm::AaSection::Whole, disable, allocated, before, 200);
                const std::vector<ScriptFootprint> lua = pairs.Footprints();
                CHECK_MSG(lua.size() == 1 && lua.front().OffTick == 200, Format("%zu footprints after the Lua pair", lua.size()));
                pairs.Note("Hook", 5, 20, Asm::AaSection::Enable, enable, before, allocated, 300);
                pairs.Note("orphan", 0, 21, Asm::AaSection::Disable, disable, allocated, before, 400);
                const std::vector<ScriptFootprint> orphan = pairs.Footprints();
                CHECK(orphan.size() == 2 && orphan.back().Label == "Hook" && orphan.back().OffTick == 400);
            }
            CHECK(ScriptLabel("autoAssemble from \"Damage : Multiplier ()->\"", "lua") == "Damage : Multiplier ()->");
            CHECK(ScriptLabel("\"Infinite Ammo\"", "table") == "Infinite Ammo" && ScriptLabel("", "window") == "Auto Assembler window" && ScriptLabel("autoAssemble", "lua") == "autoAssemble");
            ledger.Reset();
            CHECK(ledger.Footprints().empty());
        }

        // ---- A real crash in the test target ----

        struct Child
        {
            PROCESS_INFORMATION Info{};

            Child() = default;
            Child(const Child&) = delete;
            Child& operator=(const Child&) = delete;
            ~Child()
            {
                if (!Info.hProcess)
                    return;
                ::TerminateProcess(Info.hProcess, 0);
                ::WaitForSingleObject(Info.hProcess, 3000);
                ::CloseHandle(Info.hProcess);
                ::CloseHandle(Info.hThread);
            }
        };

        bool WaitFor(int milliseconds, const std::function<bool()>& done)
        {
            const ULONGLONG until = ::GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
            for (;;)
            {
                if (done())
                    return true;
                if (::GetTickCount64() >= until)
                    return false;
                ::Sleep(5);
            }
        }

        bool Start(const std::filesystem::path& path, Child& child)
        {
            STARTUPINFOW startup{ sizeof(startup) };
            std::wstring command = L"\"" + path.wstring() + L"\" --crash";
            return ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.Info) != FALSE;
        }

        enum class Way
        {
            InScript,
            CalledFromScript,
            QuietEnd
        };

        std::string CrashScript(bool is64Bit, Way way, std::uint64_t crashRead)
        {
            std::string body;
            if (way == Way::InScript)
                body = is64Bit ? "  sub rsp,28\nboom:\n  xor eax,eax\n  mov [rax],eax\n  add rsp,28\n  ret\n" : "boom:\n  xor eax,eax\n  mov [eax],eax\n  ret 4\n";
            else
                body = is64Bit ? "  sub rsp,28\n  xor ecx,ecx\n  mov rax," + Hex(crashRead) + "\n  call rax\nback:\n  add rsp,28\n  ret\n"
                               : "  push 0\n  mov eax," + Hex(crashRead) + "\n  call eax\nback:\n  add esp,4\n  ret 4\n";
            return "[ENABLE]\nalloc(crashcave,$1000)\nlabel(" + std::string(way == Way::InScript ? "boom" : "back") + ")\ncrashcave:\n" + body + "createthread(crashcave)\n[DISABLE]\ndealloc(crashcave)\n";
        }

        void Pump(Engine& engine, int milliseconds, const std::function<bool()>& done)
        {
            WaitFor(milliseconds, [&]
                {
                    engine.Tick();
                    return done();
                });
        }

        void CheckLive(const std::filesystem::path& path, const char* label, Way way)
        {
            Child target;
            if (!Start(path, target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.CrashWatch = true;
            Engine engine(settings);
            const bool opened = WaitFor(5000, [&] { return engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)).Ok(); });
            CHECK_MSG(opened, std::string(label) + ": the test target could not be opened");
            if (!opened)
                return;
            std::uint64_t crashRead = 0;
            WaitFor(3000, [&]
                {
                    engine.Process()->RefreshModules();
                    crashRead = engine.Process()->FindExport(path.filename().string(), "CrashRead");
                    return crashRead != 0;
                });
            CHECK(crashRead != 0);
            const bool is64Bit = engine.Process()->Is64Bit();
            Table::AddressList& list = engine.Table();
            Table::MemoryRecord& harmless = list.AddScript("Harmless Patch", "[ENABLE]\nalloc(quietcave,$100)\nquietcave:\n  ret\n[DISABLE]\ndealloc(quietcave)\n");
            const Status on = engine.SetActive(harmless, true);
            CHECK_MSG(on.Ok(), on.Text);
            Pump(engine, 5000, [&] { return engine.Crashes().Running(); });
            CHECK_MSG(engine.Crashes().Running(), std::string(label) + ": the crash watch did not start after a script wrote code");
            {
                std::string started;
                for (const LogEntry& entry : RuntimeLog::Entries())
                {
                    if (entry.Message.starts_with("Watching "))
                        started = entry.Message;
                }
                std::printf("  %s: %s\n", label, started.c_str());
            }
            {
                std::string result;
                const Status scripts = engine.Lua().RunWithResult(
                    "local s = manifold.crash.scripts() if manifold.crash.watching() and #s == 1 and s[1].script == 'Harmless Patch' and s[1].on and s[1].ranges[1].kind == 'alloc' then return 'ok' end return 'bad'",
                    "crash", result);
                CHECK_MSG(scripts.Ok() && result == "ok", std::string(label) + " Lua: " + (scripts.Ok() ? result : scripts.Text));
            }
            if (way == Way::QuietEnd)
            {
                ::TerminateProcess(target.Info.hProcess, 0);
                Pump(engine, 5000, [&] { return !engine.HasProcess(); });
                CHECK(!engine.HasProcess() && !engine.LastCrash());
                return;
            }
            const std::string name = way == Way::InScript ? "Null Write" : "Bad Call";
            Table::MemoryRecord& crasher = list.AddScript(name, CrashScript(is64Bit, way, crashRead));
            const Status crashed = engine.SetActive(crasher, true);
            CHECK_MSG(crashed.Ok(), std::string(label) + ": " + crashed.Text);
            Pump(engine, 8000, [&] { return !engine.HasProcess(); });
            CHECK_MSG(!engine.HasProcess(), std::string(label) + ": the test target did not crash");
            const std::optional<CrashReport> report = engine.LastCrash();
            CHECK_MSG(report.has_value(), std::string(label) + ": no crash report");
            if (!report)
                return;
            std::printf("  %s: %s\n", label, report->Headline.c_str());
            CHECK(report->Crashed && report->Exact && report->ExitCode == 0xC0000005u && report->Fatal && report->Fatal->Code == 0xC0000005u);
            CHECK_MSG(!report->Blames.empty() && report->Blames.front().Label == name, report->Headline);
            if (report->Blames.empty())
                return;
            if (way == Way::InScript)
            {
                CHECK_MSG(report->Blames.front().Link == CrashLink::InCode && report->Blames.front().Where.starts_with("boom"), report->Blames.front().Where);
                CHECK(report->Fatal->Info.size() == 2 && report->Fatal->Info[0] == 1 && report->Fatal->Info[1] == 0);
                CHECK_MSG(report->Details.find("mov ") != std::string::npos, report->Details);
            }
            else
            {
                CHECK_MSG(report->Blames.front().Link == CrashLink::OnStack && report->Blames.front().Where.starts_with("back"), report->Blames.front().Where);
                CHECK_MSG(report->Headline.find("CrashRead") != std::string::npos, report->Headline);
            }
            CHECK(std::find(report->SwitchedOn.begin(), report->SwitchedOn.end(), "Harmless Patch") != report->SwitchedOn.end());
            std::string result;
            const Status lua = engine.Lua().RunWithResult(
                "local c = manifold.crash.last() if c and c.exitCode == 0xC0000005 and c.exact and c.scripts[1].script == '" + name + "' and type(c.headline) == 'string' then return c.scripts[1].link end return 'bad'",
                "crash", result);
            CHECK_MSG(lua.Ok() && result == (way == Way::InScript ? "code" : "stack"), std::string(label) + " Lua: " + (lua.Ok() ? result : lua.Text));
        }

        // The watch starts on a worker: closing the process while it starts drops that start
        // without marking the watch as failed, and the next process gets one again.
        void CheckStartRace(const std::filesystem::path& path, const char* label)
        {
            Child target;
            if (!Start(path, target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.CrashWatch = true;
            Engine engine(settings);
            const ProcessId id = MakeProcessId(target.Info.dwProcessId);
            const bool opened = WaitFor(5000, [&] { return engine.OpenProcess(id).Ok(); });
            CHECK_MSG(opened, std::string(label) + ": the test target could not be opened");
            if (!opened)
                return;
            Table::MemoryRecord& code = engine.Table().AddScript("Writes Code", "[ENABLE]\nalloc(racecave,$100)\nracecave:\n  ret\n[DISABLE]\ndealloc(racecave)\n");
            CHECK(engine.SetActive(code, true).Ok());
            engine.Tick();  // hands the start to a worker
            engine.CloseProcess();
            Pump(engine, 500, [] { return false; });
            CHECK(!engine.Crashes().Running());
            CHECK(engine.OpenProcess(id).Ok());
            Table::MemoryRecord& again = engine.Table().AddScript("Writes Code Again", "[ENABLE]\nalloc(racecave2,$100)\nracecave2:\n  ret\n[DISABLE]\ndealloc(racecave2)\n");
            CHECK(engine.SetActive(again, true).Ok());
            Pump(engine, 5000, [&] { return engine.Crashes().Running(); });
            CHECK_MSG(engine.Crashes().Running(), std::string(label) + ": after a start dropped by closing the process, the watch did not start for the next one");
        }

        // The agent is shared: the speedhack loads it, the crash watch finds it there and
        // starts. Removing the speedhack must not pull the library out from under the
        // watch, and closing the process afterwards must leave the game running.
        void CheckSharedAgent(const std::filesystem::path& path, const char* label)
        {
            Child target;
            if (!Start(path, target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.CrashWatch = true;
            Engine engine(settings);
            const bool opened = WaitFor(5000, [&] { return engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)).Ok(); });
            CHECK_MSG(opened, std::string(label) + ": the test target could not be opened");
            if (!opened)
                return;
            Target::Process& process = *engine.Process();
            const std::string agent = process.Is64Bit() ? "ManifoldAgent.dll" : "ManifoldAgent32.dll";
            const Status sped = engine.Speed().SetSpeed(2.0);
            CHECK_MSG(sped.Ok(), std::string(label) + ": " + sped.Text);
            const Status watching = engine.Crashes().Start();
            CHECK_MSG(watching.Ok() && engine.Crashes().Running(), std::string(label) + ": " + watching.Text);
            const Status removed = engine.Speed().Remove();
            CHECK_MSG(removed.Ok(), std::string(label) + ": " + removed.Text);
            process.RefreshModules();
            const bool loaded = process.FindModule(agent).has_value();
            CHECK_MSG(loaded, std::string(label) + ": removing the speedhack unloaded the agent while the crash watch still used it");
            const std::uint64_t block = loaded ? process.FindExport(agent, "ManifoldCrash") : 0;
            Target::Crash::Block shared{};
            CHECK_MSG(block && process.ReadExact(block, &shared, sizeof(shared)) && shared.Running == 1, std::string(label) + ": the crash watch stopped with the speedhack");
            engine.CloseProcess();
            ::Sleep(400);
            DWORD code = 0;
            CHECK_MSG(::GetExitCodeProcess(target.Info.hProcess, &code) && code == STILL_ACTIVE,
                std::string(label) + Format(": the game ended (%08X) when the crash watch was stopped after the speedhack", static_cast<unsigned>(code)));
        }

        // ---- The Auto Assembler's stack balance check ----

        bool Logged(std::string_view text)
        {
            const std::vector<LogEntry> entries = RuntimeLog::Entries();
            return std::any_of(entries.begin(), entries.end(), [&](const LogEntry& entry) { return entry.Message.find(text) != std::string::npos; });
        }

        int LineOf(const std::string& script, std::string_view needle)
        {
            int line = 1;
            std::size_t at = 0;
            while (at < script.size())
            {
                const std::size_t end = std::min(script.find('\n', at), script.size());
                if (std::string_view(script).substr(at, end - at).find(needle) != std::string_view::npos)
                    return line;
                at = end + 1;
                ++line;
            }
            return 0;
        }

        void CheckStackWarnings(const std::filesystem::path& path, const char* label)
        {
            Child target;
            if (!Start(path, target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.CrashWatch = false;
            Engine engine(settings);
            const bool opened = WaitFor(5000, [&] { return engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)).Ok(); });
            CHECK_MSG(opened, std::string(label) + ": the test target could not be opened");
            if (!opened)
                return;
            const bool is64Bit = engine.Process()->Is64Bit();
            const std::string ax = is64Bit ? "rax" : "eax";
            const std::string bp = is64Bit ? "rbp" : "ebp";
            const std::string sp = is64Bit ? "rsp" : "esp";
            Table::AddressList& list = engine.Table();
            // The "game code" hooks patch: `site` is five nops, `framed` starts a function with push ebp; mov ebp,esp; both end with ret.
            Table::MemoryRecord& site = list.AddScript("Site", std::string("[ENABLE]\nalloc(site,$100)\nalloc(framed,$100)\nregistersymbol(site)\nregistersymbol(framed)\n") +
                "site:\ndb 90 90 90 90 90 C3\nframed:\ndb " + (is64Bit ? "55 48 8B EC 90" : "55 8B EC 90 90") +
                " C3\n[DISABLE]\nunregistersymbol(site)\nunregistersymbol(framed)\ndealloc(site)\ndealloc(framed)\n");
            const Status placed = engine.SetActive(site, true);
            CHECK_MSG(placed.Ok(), placed.Text);
            const auto hook = [&](const std::string& body, const std::string& labels = {}, const std::string& at = "site")
            {
                return "[ENABLE]\nalloc(cave,$100," + at + ")\nlabel(return)\n" + labels + "cave:\n" + body + "  jmp return\n" + at + ":\n  jmp cave\nreturn:\n[DISABLE]\ndealloc(cave)\n";
            };
            const auto warnings = [&](const std::string& script)
            {
                Asm::AaMemory memory;
                const ScriptCheck check = engine.CheckScript(script, Asm::AaSection::Enable, memory);
                CHECK_MSG(check.Result.Ok(), std::string(label) + ": " + check.Result.Error.Text);
                return check.Result.Warnings;
            };

            const std::string open = hook("  push " + ax + "\n  mov " + ax + ",1\n");
            const auto unbalanced = warnings(open);
            CHECK_MSG(unbalanced.size() == 1, std::string(label) + Format(": %zu warnings for a push without its pop", unbalanced.size()));
            if (!unbalanced.empty())
            {
                CHECK_MSG(unbalanced[0].Line == LineOf(open, "push") && unbalanced[0].Text.find("push " + ax) != std::string::npos &&
                              unbalanced[0].Text.find(Format("%d bytes off at the jump back", is64Bit ? 8 : 4)) != std::string::npos,
                    unbalanced[0].Text);
            }
            CHECK(warnings(hook("  push " + ax + "\n  pop " + ax + "\n")).empty());
            CHECK(warnings(hook(is64Bit ? "  pushfq\n  popfq\n" : "  pushfd\n  pushad\n  popad\n  popfd\n")).empty());
            CHECK(warnings(hook("  sub " + sp + ",20\n  add " + sp + ",20\n")).empty());
            // The patch replaced push ebp; mov ebp,esp: the cave runs them again, so it leaves one word on the stack, rightly.
            CHECK_MSG(warnings(hook("  push " + bp + "\n  mov " + bp + "," + sp + "\n", {}, "framed")).empty(), std::string(label) + ": the original push ebp counted as a mistake");
            const auto dropped = warnings(hook("  nop\n", {}, "framed"));
            CHECK_MSG(dropped.size() == 1 && dropped[0].Text.find("takes more off the stack") != std::string::npos, std::string(label) + ": a cave that drops the original push was not noticed");
            const std::string branch = hook("  test eax,eax\n  je skip\n  push " + ax + "\nskip:\n", "label(skip)\n");
            const auto branched = warnings(branch);
            CHECK_MSG(branched.size() == 1 && branched[0].Line == LineOf(branch, "push"), std::string(label) + ": the push on one branch was not noticed");
            const std::string called = "[ENABLE]\nalloc(cave,$100,site)\ncave:\n  push " + ax + "\n  ret\nsite:\n  call cave\n[DISABLE]\ndealloc(cave)\n";
            const auto calledWarnings = warnings(called);
            CHECK_MSG(calledWarnings.size() == 1 && calledWarnings[0].Text.find("at the ret") != std::string::npos, std::string(label) + ": a call hook that returns with a push open was not noticed");
            CHECK(warnings("[ENABLE]\nalloc(cave,$100,site)\ncave:\n  push " + ax + "\n  pop " + ax + "\n  ret\nsite:\n  call cave\n[DISABLE]\ndealloc(cave)\n").empty());

            Table::MemoryRecord& bad = list.AddScript("Bad Hook", open);
            const Status on = engine.SetActive(bad, true);
            CHECK_MSG(on.Ok(), on.Text);
            CHECK_MSG(Logged("\"Bad Hook\": The stack is"), std::string(label) + ": switching the hook on gave no notice");
            const std::vector<ScriptFootprint> footprints = engine.ScriptLedger().Footprints();
            CHECK(std::any_of(footprints.begin(), footprints.end(), [](const ScriptFootprint& footprint) { return footprint.Label == "Bad Hook" && footprint.Warnings.size() == 1; }));
            CHECK(engine.SetActive(bad, false).Ok() && engine.SetActive(site, false).Ok());

            // What the disassembler shows as data: each db/dw/dd/dq line of an allocation, the empty rest
            // of it while it is zero, never a readmem (the original code of a hook).
            Table::MemoryRecord& data = list.AddScript("Data Rows", "[ENABLE]\nalloc(datacave,$100)\nlabel(value ratio)\nregistersymbol(datacave value ratio)\n"
                                                                    "datacave:\n  mov eax,[value]\n  ret\nvalue:\n  dd 0\nratio:\n  dd (float)3.00\n  db 1 2 3\n"
                                                                    "  readmem(datacave,2)\n[DISABLE]\nunregistersymbol(datacave value ratio)\ndealloc(datacave)\n");
            const Status dataOn = engine.SetActive(data, true);
            CHECK_MSG(dataOn.Ok(), dataOn.Text);
            const std::uint64_t cave = engine.Evaluate("datacave").value_or(0);
            const std::uint64_t value = engine.Evaluate("value").value_or(0);
            const std::uint64_t ratio = engine.Evaluate("ratio").value_or(0);
            CHECK(cave && value > cave && ratio == value + 4);
            const CrashLedger& ledger = engine.ScriptLedger();
            CHECK(!ledger.DataAt(cave).has_value());
            const auto valueSpan = ledger.DataAt(value + 1);
            CHECK_MSG(valueSpan && valueSpan->Begin == value && valueSpan->End == value + 4 && !valueSpan->ZeroOnly, std::string(label) + ": the dd line is not one data span");
            const auto ratioSpan = ledger.DataAt(ratio);
            CHECK(ratioSpan && ratioSpan->Begin == ratio && ratioSpan->End == ratio + 4);
            const auto bytesSpan = ledger.DataAt(ratio + 4);
            CHECK(bytesSpan && bytesSpan->Begin == ratio + 4 && bytesSpan->End == ratio + 7);
            CHECK_MSG(!ledger.DataAt(ratio + 7).has_value() && !ledger.DataAt(ratio + 8).has_value(), std::string(label) + ": the readmem bytes count as data");
            const auto rest = ledger.DataAt(ratio + 9);
            CHECK_MSG(rest && rest->ZeroOnly && rest->Begin == ratio + 9 && rest->End == cave + 0x100, std::string(label) + ": the empty rest of the allocation is not a zero span");
            CHECK(engine.SetActive(data, false).Ok() && !ledger.DataAt(value).has_value());
        }

        // ---- CE's "break when inside specific regions" with the scripts' memory ----

        void CheckDebuggerStop(const std::filesystem::path& path, const char* label, bool veh)
        {
            Child target;
            if (!Start(path, target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.CrashWatch = false;
            settings.UseInProcessDebugger = veh;
            settings.BreakOnException = 2;
            settings.BreakInScriptMemory = true;
            Engine engine(settings);
            const bool opened = WaitFor(5000, [&] { return engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)).Ok(); });
            CHECK_MSG(opened, std::string(label) + ": the test target could not be opened");
            if (!opened)
                return;
            const Status debugging = engine.EnsureDebugger();
            CHECK_MSG(debugging.Ok(), std::string(label) + ": " + debugging.Text);
            if (!debugging)
                return;
            const bool is64Bit = engine.Process()->Is64Bit();
            Table::AddressList& list = engine.Table();
            // ∑ The code goes in first and runs later from a second script, as in a game: the region is known when the exception comes.
            const std::string body = is64Bit ? "  sub rsp,28\nboom:\n  xor eax,eax\n  mov [rax],eax\n  add rsp,28\n  ret\n" : "boom:\n  xor eax,eax\n  mov [eax],eax\n  ret 4\n";
            Table::MemoryRecord& code = list.AddScript("Null Write",
                "[ENABLE]\nalloc(stopcave,$1000)\nlabel(boom)\nregistersymbol(stopcave)\nstopcave:\n" + body + "[DISABLE]\nunregistersymbol(stopcave)\ndealloc(stopcave)\n");
            const Status on = engine.SetActive(code, true);
            CHECK_MSG(on.Ok(), on.Text);
            Pump(engine, 300, [] { return false; });
            const std::uint64_t serial = engine.Debugger().Break().Serial;
            Table::MemoryRecord& trigger = list.AddScript("Trigger", "[ENABLE]\ncreatethread(stopcave)\n[DISABLE]\n");
            CHECK(engine.SetActive(trigger, true).Ok());
            Pump(engine, 6000, [&] { const Debug::BreakInfo info = engine.Debugger().Break(); return info.Broken && info.Serial != serial; });
            const Debug::BreakInfo info = engine.Debugger().Break();
            CHECK_MSG(info.Broken && info.Exception.Raised && info.Exception.Code == 0xC0000005u, std::string(label) + ": the debugger did not stop at the exception in the script");
            Pump(engine, 300, [] { return false; });
            CHECK_MSG(Logged("Stopped in the code of \"Null Write\" (boom+2)"), std::string(label) + ": no notice named the script");
            CHECK(engine.HasProcess() && engine.Process()->Alive());

            settings.BreakInScriptMemory = false;
            CHECK(engine.ApplyExceptionBreak().Code == ErrorCode::DebugBadExpression);
            ::TerminateProcess(target.Info.hProcess, 0);
            Pump(engine, 5000, [&] { return !engine.HasProcess(); });
        }

        void CheckRegions()
        {
            std::vector<ScriptFootprint> footprints = { Footprint("A", 0x1000, 0x2000, 10), Footprint("B", 0x1800, 0x3000, 10), Footprint("Off", 0x9000, 0xA000, 10, 20) };
            footprints[0].Ranges.push_back({ 0x5000, 0x5005, ScriptRangeKind::Patch, {} });
            footprints[1].Ranges.push_back({ 0x5100, 0x5105, ScriptRangeKind::Patch, {} });
            const auto all = ScriptRegions(footprints, 64);
            CHECK(all.size() == 3 && all[0].first == 0x1000 && all[0].second == 0x3000 && all[1].first == 0x5000 && all[2].first == 0x5100);
            const auto squeezed = ScriptRegions(footprints, 2);
            CHECK(squeezed.size() == 2 && squeezed[1].first == 0x5000 && squeezed[1].second == 0x5105);
            const auto at = ScriptAt(footprints, 0x5002, 15, {});
            CHECK(at && at->Label == "A" && at->Link == CrashLink::InPatch);
            const auto freed = ScriptAt(footprints, 0x9004, 30, {});
            CHECK(freed && freed->Label == "Off" && freed->Link == CrashLink::RanFreed && freed->Where == "newmem+4");
            CHECK(!ScriptAt(footprints, 0x8000, 30, {}));
        }

        int RunCrashTests(const std::filesystem::path& target)
        {
            const int before = Totals().Failures;
            CheckAttribution();
            CheckLedger();
            CheckRegions();
            InitSysCalls();
            for (const auto& [path, label] : { std::pair{ target, "64-bit" }, std::pair{ target.parent_path() / L"TestTarget32.exe", "32-bit" } })
            {
                CheckLive(path, (std::string(label) + " in script").c_str(), Way::InScript);
                CheckLive(path, (std::string(label) + " called from script").c_str(), Way::CalledFromScript);
                CheckLive(path, (std::string(label) + " quiet end").c_str(), Way::QuietEnd);
                CheckStackWarnings(path, (std::string(label) + " stack check").c_str());
                CheckStartRace(path, (std::string(label) + " start race").c_str());
                CheckSharedAgent(path, (std::string(label) + " shared agent").c_str());
                CheckDebuggerStop(path, (std::string(label) + " Windows debugger").c_str(), false);
                CheckDebuggerStop(path, (std::string(label) + " VEH debugger").c_str(), true);
            }
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("crash", "Crash attribution", 128, [](const SuiteContext& context) { return RunCrashTests(context.Target); });
}
