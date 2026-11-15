#include "TestKit.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Core/RuntimeLog.h"
#include "Lua/LuaEngine.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <thread>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        struct TargetProgram
        {
            PROCESS_INFORMATION Info{};
            std::map<std::string, std::uint64_t> Addresses;

            ~TargetProgram()
            {
                if (Info.hProcess)
                {
                    ::TerminateProcess(Info.hProcess, 0);
                    ::WaitForSingleObject(Info.hProcess, 3000);
                    ::CloseHandle(Info.hProcess);
                    ::CloseHandle(Info.hThread);
                }
            }
        };

        bool Launch(const std::filesystem::path& path, TargetProgram& target)
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
            std::wstring command = L"\"" + path.wstring() + L"\"";
            const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &target.Info);
            ::CloseHandle(write);
            if (!started)
            {
                ::CloseHandle(read);
                return false;
            }
            std::string text;
            char buffer[512];
            DWORD got = 0;
            while (text.find("shoot at") == std::string::npos || text.find('\n', text.find("shoot at")) == std::string::npos)
            {
                if (!::ReadFile(read, buffer, sizeof(buffer), &got, nullptr) || got == 0)
                    break;
                text.append(buffer, got);
            }
            ::CloseHandle(read);
            const auto take = [&](const char* label)
                {
                    const std::size_t at = text.find(label);
                    if (at == std::string::npos)
                        return;
                    const std::size_t start = at + std::strlen(label);
                    std::size_t end = start;
                    while (end < text.size() && IsHexDigit(text[end]))
                        ++end;
                    if (const auto value = ParseHex(text.substr(start, end - start)))
                        target.Addresses[label] = *value;
                };
            for (const char* label : { "g_world at ", "hero at ", "health at ", "damage at ", "readhealth at ", "shoot at " })
                take(label);
            return target.Addresses.size() == 6;
        }

        bool WaitScan(Engine& engine)
        {
            for (int i = 0; i < 600; ++i)
            {
                if (engine.Scanner().Poll())
                    return engine.Scanner().LastError().Ok();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return false;
        }

        bool Contains(Engine& engine, std::uint64_t address)
        {
            const std::uint64_t count = std::min<std::uint64_t>(engine.Scanner().Count(), 5'000'000);
            Scan::ScanRecord record;
            for (std::uint64_t i = 0; i < count; ++i)
            {
                if (engine.Scanner().Record(i, record) && record.Address == address)
                    return true;
            }
            return false;
        }

        void Pause(Engine& engine, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
    }

    int RunIntegrationTests(const std::filesystem::path& targetPath)
    {
        const int before = Totals().Failures;
        TargetProgram target;
        if (!Launch(targetPath, target))
        {
            std::printf("  SKIP the test target %ls could not be started\n", targetPath.c_str());
            return 0;
        }
        const std::uint64_t world = target.Addresses["g_world at "];
        const std::uint64_t hero = target.Addresses["hero at "];
        const std::uint64_t health = target.Addresses["health at "];
        const std::uint64_t damage = target.Addresses["damage at "];
        CHECK_EQ(hero, health);

        RuntimeLog::SetVerbose(true);
        Settings settings;
        settings.ScanMapped = false;
        Engine engine(settings);
        const Status opened = engine.OpenProcess(target.Info.dwProcessId);
        CHECK_MSG(opened.Ok(), opened.Text);
        if (!opened)
            return Totals().Failures - before;
        const Target::Process& process = *engine.Process();
        CHECK(process.Is64Bit());

        CHECK_EQ(engine.Evaluate("[" + Hex(world) + "]+8").value_or(0) != 0, true);
        CHECK_EQ(engine.Evaluate("[[" + Hex(world) + "]+8]").value_or(0), hero);
        const auto module = process.FindModule("TestTarget.exe");
        CHECK(module.has_value());
        if (module)
        {
            CHECK_EQ(engine.Evaluate("TestTarget.exe").value_or(0), module->Base);
            CHECK_EQ(engine.Evaluate("\"TestTarget.exe\"+10").value_or(0), module->Base + 0x10);
            CHECK_EQ(engine.Symbols().ModuleOffset(damage), "\"TestTarget.exe\"+" + Hex(damage - module->Base));
        }
        const auto sleep = engine.Evaluate("kernel32.Sleep");
        CHECK(sleep.has_value());
        if (sleep)
        {
            const std::string described = engine.Describe(*sleep);
            CHECK_MSG(described.find("Sleep") != std::string::npos, described);
        }
        Status unknown;
        CHECK(!engine.Evaluate("no_such_symbol_here", &unknown) && unknown.Code == ErrorCode::ExprUnknownSymbol);

        const Disasm::Instruction first = engine.Disassembler().Read(process, damage, &engine.Symbols());
        CHECK_MSG(first.Valid && first.Mnemonic == "sub" && first.Memory.Present && first.Memory.Written, first.Text());

        const auto healthNow = process.ReadValue<std::int32_t>(health);
        CHECK(healthNow.has_value());
        Scan::ScanOptions options;
        options.Type = Scan::ValueType::FourBytes;
        options.Mode = Scan::Compare::Exact;
        options.Value = std::to_string(healthNow.value_or(0));
        options.Mapped = false;
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK_MSG(Contains(engine, health), "exact scan for " + options.Value + " found " + std::to_string(engine.Scanner().Count()));
        const std::uint64_t firstCount = engine.Scanner().Count();
        Pause(engine, 1300);
        options.Mode = Scan::Compare::Decreased;
        CHECK(engine.Scanner().Start(engine.Process(), options, false).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, health));
        CHECK(engine.Scanner().Count() < firstCount);
        engine.Scanner().Undo();
        CHECK_EQ(engine.Scanner().Count(), firstCount);
        engine.Scanner().Reset();

        options = {};
        options.Type = Scan::ValueType::Float;
        options.Mode = Scan::Compare::Exact;
        options.Value = "3.5";
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, hero + 4));
        engine.Scanner().Reset();

        options = {};
        options.Type = Scan::ValueType::String;
        options.Value = "manifoldhero";
        options.CaseSensitive = false;
        options.Alignment = 1;
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, hero + 16));
        engine.Scanner().Reset();

        options = {};
        options.Type = Scan::ValueType::FourBytes;
        options.Mode = Scan::Compare::Unknown;
        options.Start = hero & ~0xFFFFull;
        options.Stop = (hero & ~0xFFFFull) + 0x100000;
        CHECK(engine.Scanner().Start(engine.Process(), options, true).Ok());
        CHECK(WaitScan(engine));
        CHECK(engine.Scanner().IsSnapshot());
        Pause(engine, 1100);
        options.Mode = Scan::Compare::Changed;
        CHECK(engine.Scanner().Start(engine.Process(), options, false).Ok());
        CHECK(WaitScan(engine));
        CHECK(Contains(engine, hero + 0x20));
        engine.Scanner().Reset();

        Table::MemoryRecord& pointer = engine.Table().AddValue("Health by pointer", Hex(world), Scan::ValueType::FourBytes);
        pointer.Offsets = { "8", "0" };
        engine.ResolveRecord(pointer);
        CHECK(pointer.AddressOk && pointer.ResolvedAddress == health);
        Table::MemoryRecord& ammo = engine.Table().AddValue("Ammo", Hex(hero + 0x20), Scan::ValueType::FourBytes);
        CHECK(engine.WriteRecord(ammo, "25").Ok());
        CHECK(engine.SetActive(ammo, true).Ok());
        Pause(engine, 1200);
        const std::int32_t frozen = process.ReadValue<std::int32_t>(hero + 0x20).value_or(-1);
        CHECK_MSG(frozen == 25 || frozen == 24, std::to_string(frozen));
        CHECK(engine.SetActive(ammo, false).Ok());

        std::uint8_t original[5]{};
        CHECK(process.ReadExact(damage, original, sizeof(original)));
        CHECK(engine.Symbols().Register("DAMAGE", damage, "test").Ok());
        Table::MemoryRecord& script = engine.Table().AddScript("Heal instead of damage",
            "[ENABLE]\nalloc(newmem,$100,DAMAGE)\nlabel(code)\nnewmem:\ncode:\n  add [rcx],edx\n  ret\nDAMAGE:\n  jmp newmem\nregistersymbol(code)\n"
            "[DISABLE]\nDAMAGE:\n  db " + HexBytes(original) + "\nunregistersymbol(code)\ndealloc(newmem)\n");
        const Status enabled = engine.SetActive(script, true);
        CHECK_MSG(enabled.Ok(), enabled.Text);
        CHECK(engine.Symbols().FindRegistered("code").has_value());
        const Disasm::Instruction jump = engine.Disassembler().Read(process, damage, nullptr);
        CHECK_MSG(jump.Kind == Disasm::Flow::Jump && jump.Length == 5, jump.Text());
        const std::int32_t healedFrom = process.ReadValue<std::int32_t>(health).value_or(0);
        Pause(engine, 2300);
        CHECK_MSG(process.ReadValue<std::int32_t>(health).value_or(0) > healedFrom, "health did not rise while the script was active");
        const Status disabled = engine.SetActive(script, false);
        CHECK_MSG(disabled.Ok(), disabled.Text);
        std::uint8_t restored[5]{};
        CHECK(process.ReadExact(damage, restored, sizeof(restored)) && std::memcmp(restored, original, sizeof(original)) == 0);
        CHECK(!engine.Symbols().FindRegistered("code").has_value());
        CHECK(script.ScriptMemory.Allocations.empty());

        std::string luaResult;
        Status lua = engine.Lua().RunWithResult("registerSymbol('hero', " + std::to_string(hero) + ")\n"
                                                "writeFloat('hero+4', 7.25)\n"
                                                "local list = getAddressList()\n"
                                                "local record = list.createMemoryRecord()\n"
                                                "record.Description = 'From Lua'\n"
                                                "record.Address = 'hero+8'\n"
                                                "record.Type = vtDouble\n"
                                                "local hits = AOBScan('4D 61 6E 69 66 6F 6C 64 48 65 72 6F')\n"
                                                "return string.format('%.2f|%s|%d|%s', readFloat('hero+4'), record.Value, hits and hits.Count or 0, getNameFromAddress(getAddress('TestTarget.exe')))",
            "test", luaResult);
        CHECK_MSG(lua.Ok(), lua.Text);
        const auto parts = Split(luaResult, '|', true);
        CHECK_MSG(parts.size() == 4 && parts[0] == "7.25" && parts[1] == "1234.5" && parts[2] != "0" && parts[3] == "TestTarget.exe", luaResult);

        lua = engine.Lua().RunWithResult("local s = createStructure('guess test')\n"
                                         "s.autoGuess(getAddress('hero'), 0, 16)\n"
                                         "local e = s.getElementByOffset(4)\n"
                                         "if s.Count < 3 or not e or e.Vartype ~= vtSingle then return 'guess ' .. s.Count end\n"
                                         "local count = s.Count\n"
                                         "s.autoGuess(getAddress('hero'), 4, 4)\n"
                                         "if s.Count ~= count then return 'guessing again changed the count' end\n"
                                         "local b = createStructure('guess offset')\n"
                                         "b.autoGuess(getAddress('hero'), 4, 4)\n"
                                         "if b.Count ~= 1 or b.getElement(0).Offset ~= 4 then return 'offset ' .. b.Count end\n"
                                         "s.destroy()\n"
                                         "b.destroy()\n"
                                         "return 'ok'",
            "autoGuess", luaResult);
        CHECK_MSG(lua.Ok() && luaResult == "ok", lua.Ok() ? luaResult : lua.Text);

        lua = engine.Lua().Run("local t = createTimer(nil, false)\nt.Interval = 50\nticks = 0\nt.OnTimer = function(sender) ticks = ticks + 1 if ticks >= 3 then sender.destroy() end end\nt.Enabled = true\n"
                               "createThread(function() sleep(100) threadDone = true end)", "timers");
        CHECK_MSG(lua.Ok(), lua.Text);
        Pause(engine, 600);
        lua = engine.Lua().RunWithResult("return tostring(ticks) .. '|' .. tostring(threadDone)", "timers", luaResult);
        CHECK_MSG(lua.Ok() && luaResult == "3|true", luaResult);

        lua = engine.Lua().Run("error('expected failure')", "failing");
        CHECK(!lua.Ok() && lua.Code == ErrorCode::LuaRuntime);
        lua = engine.Lua().Run("this is not lua", "broken");
        CHECK(!lua.Ok() && lua.Code == ErrorCode::LuaSyntax);

        CHECK_EQ(engine.Evaluate("TestTarget.g_world").value_or(0), world);
        CHECK_EQ(engine.Evaluate("TestTarget.Damage").value_or(0), damage);
        std::filesystem::path table = targetPath.parent_path();
        for (int i = 0; i < 6 && !std::filesystem::exists(table / "Tests" / "Target" / "TestTarget.CT"); ++i)
            table = table.parent_path();
        table = table / "Tests" / "Target" / "TestTarget.CT";
        if (std::filesystem::exists(table))
        {
            settings.TableScripts = TableScriptPolicy::Never;
            const Status loaded = engine.LoadTable(table, false);
            CHECK_MSG(loaded.Ok(), loaded.Text);
            Table::MemoryRecord* healthRecord = engine.Table().FindByDescription("Health");
            CHECK(healthRecord != nullptr);
            if (healthRecord)
            {
                engine.ResolveRecord(*healthRecord);
                CHECK(healthRecord->AddressOk && healthRecord->ResolvedAddress == health);
                CHECK(healthRecord->Hotkeys.size() == 1);
            }
            Table::MemoryRecord* nameRecord = engine.Table().FindByDescription("Name");
            if (nameRecord)
                CHECK_EQ(engine.FormatRecord(*nameRecord, engine.ReadRecord(*nameRecord)), "ManifoldHero");
            Table::MemoryRecord* ammoRecord = engine.Table().FindByDescription("Ammo");
            if (ammoRecord)
            {
                CHECK(engine.WriteRecord(*ammoRecord, "Full").Ok());
                CHECK_EQ(process.ReadValue<std::int32_t>(hero + 0x20).value_or(0), 30);
            }
            Table::MemoryRecord* heal = engine.Table().FindByDescription("Heal instead of damage");
            CHECK(heal != nullptr);
            if (heal)
            {
                const Status on = engine.SetActive(*heal, true);
                CHECK_MSG(on.Ok(), on.Text);
                const Status off = engine.SetActive(*heal, false);
                CHECK_MSG(off.Ok(), off.Text);
                std::uint8_t back[3]{};
                CHECK(process.ReadExact(damage, back, 3) && back[0] == 0x29 && back[1] == 0x11 && back[2] == 0xC3);
            }
            Table::MemoryRecord* luaRecord = engine.Table().FindByDescription("Lua: print the health");
            if (luaRecord)
            {
                const Status on = engine.SetActive(*luaRecord, true);
                CHECK_MSG(on.Ok(), on.Text);
                const auto output = engine.Lua().Output();
                CHECK(!output.empty() && output.back().Text.rfind("Health is ", 0) == 0);
                CHECK(engine.SetActive(*luaRecord, false).Ok());
            }
            const std::string saved = Table::WriteTable(engine.Table(), engine.Structures(), engine.TableSymbols());
            Table::LoadedTable again;
            CHECK(Table::ParseTable(saved, again).Ok() && again.Records.size() == 3);
        }
        else
        {
            std::printf("  SKIP TestTarget.CT not found\n");
        }

        const Status attached = engine.EnsureDebugger();
        CHECK_MSG(attached.Ok(), attached.Text);
        if (attached)
        {
            int writes = 0;
            CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Writes, health, 4, writes).Ok());
            int calls = 0;
            CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Calls, damage, 1, calls).Ok());
            int accesses = 0;
            CHECK(engine.Debugger().StartWatch(Debug::WatchKind::InstructionAccesses, target.Addresses["readhealth at "], 1, accesses).Ok());
            Pause(engine, 2600);
            Debug::Watch watch;
            CHECK(engine.Debugger().CopyWatch(writes, watch));
            CHECK_MSG(!watch.Entries.empty() && watch.Entries.front().Instruction == damage, watch.Entries.empty() ? "no write recorded" : Hex(watch.Entries.front().Instruction));
            CHECK(engine.Debugger().CopyWatch(calls, watch));
            CHECK_MSG(!watch.Entries.empty() && module && module->Contains(watch.Entries.front().Instruction), "no caller recorded");
            if (!watch.Entries.empty())
            {
                const Disasm::Instruction call = engine.Disassembler().Read(process, watch.Entries.front().Instruction, nullptr);
                CHECK_MSG(call.Kind == Disasm::Flow::Call && call.Target == damage, call.Text());
            }
            CHECK(engine.Debugger().CopyWatch(accesses, watch));
            CHECK_MSG(!watch.Entries.empty() && watch.Entries.front().Accessed == health, watch.Entries.empty() ? "no access recorded" : Hex(watch.Entries.front().Accessed));

            engine.Debugger().RemoveWatch(writes);
            engine.Debugger().RemoveWatch(calls);
            engine.Debugger().RemoveWatch(accesses);
            CHECK(engine.Debugger().SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false).Ok());
            bool broke = false;
            for (int i = 0; i < 100 && !broke; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                broke = engine.Debugger().Broken();
            }
            CHECK(broke);
            if (broke)
            {
                const Debug::BreakInfo info = engine.Debugger().Break();
                CHECK_EQ(info.Address, damage);
                CHECK_EQ(info.Registers.Rcx, hero);
                engine.Debugger().Resume(Debug::ResumeMode::StepInto);
                bool stepped = false;
                for (int i = 0; i < 100 && !stepped; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    const Debug::BreakInfo next = engine.Debugger().Break();
                    stepped = next.Broken && next.Serial != info.Serial;
                }
                CHECK(stepped);
                CHECK_EQ(engine.Debugger().Break().Address, first.Next());
                CHECK(engine.Debugger().RemoveBreakpoint(damage).Ok());
                engine.Debugger().Resume(Debug::ResumeMode::Continue);
            }
            std::uint8_t afterBreakpoints = 0;
            CHECK(process.ReadExact(damage, &afterBreakpoints, 1) && afterBreakpoints == original[0]);

            const std::uint64_t shared = engine.Evaluate("TestTarget.g_shared").value_or(0);
            const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
            CHECK(shared != 0 && work != 0);
            std::uint64_t sharedHits = 0;
            for (int round = 0; round < 6 && shared && work; ++round)
            {
                int sharedWrites = 0;
                int sharedAccesses = 0;
                int workCalls = 0;
                CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Writes, shared, 4, sharedWrites).Ok());
                CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Calls, work, 1, workCalls).Ok());
                if (round % 2 == 0)
                    CHECK(engine.Debugger().StartWatch(Debug::WatchKind::Accesses, health, 4, sharedAccesses).Ok());
                Pause(engine, 350);
                Debug::Watch copy;
                if (engine.Debugger().CopyWatch(sharedWrites, copy))
                    sharedHits += copy.Hits;
                engine.Debugger().RemoveWatch(sharedWrites);
                engine.Debugger().RemoveWatch(workCalls);
                if (sharedAccesses)
                    engine.Debugger().RemoveWatch(sharedAccesses);
                Pause(engine, 60);
            }
            CHECK_MSG(sharedHits > 20, std::to_string(sharedHits) + " writes seen");
            CHECK_MSG(process.Alive(), "the target died while watches started and stopped");
            if (!process.Alive())
            {
                for (const LogEntry& entry : RuntimeLog::Entries())
                {
                    if (entry.Area == "Debugger")
                        std::printf("    log: %s\n", entry.Message.c_str());
                }
            }
            engine.Debugger().Detach();
            CHECK(!engine.Debugger().Attached());
            Pause(engine, 600);
            CHECK(process.Alive());
        }

        {
            std::string answer;
            const Status run = engine.Lua().RunWithResult(R"LUA(
                local script = "[ENABLE]\nalloc(compatblock,$100)\nlabel(compatvalue other)\ncompatblock:\ncompatvalue:\n  dd 5\nother:\n  dd 6\nregistersymbol(compatvalue other)\n[DISABLE]\nunregistersymbol(*)\ndealloc(*)\n"
                local ok, info = autoAssemble(script)
                if not ok then return "enable: " .. tostring(info) end
                local address = getAddressSafe("compatvalue")
                if not address or readInteger(address) ~= 5 or readInteger(getAddressSafe("other")) ~= 6 then return "values" end
                if type(info.allocs) ~= "table" or not info.allocs.compatblock then return "allocs" end
                if autoAssemble(script, false, info) ~= true then return "disable" end
                if getAddressSafe("compatvalue") ~= nil or getAddressSafe("other") ~= nil then return "still registered" end
                if readInteger(address) ~= nil then return "not freed" end
                return "ok")LUA", "integration", answer);
            CHECK_MSG(run.Ok() && answer == "ok", run.Ok() ? answer : run.Text);
        }

        CHECK(engine.Symbols().Register("compatkeep", 0x1234, "test").Ok());
        CHECK(engine.OpenProcess(target.Info.dwProcessId).Ok());
        CHECK(engine.Symbols().FindRegistered("compatkeep").has_value());
        CHECK(engine.Process().get() == &process);

        {
            const auto stored = process.ReadValue<std::uint64_t>(world);
            CHECK(stored.has_value());
            Target::Process::SetDryRun(true);
            const std::uint64_t marker = 0xDEADBEEFCAFEF00Dull;
            CHECK(process.Write(world, &marker, sizeof(marker)).Ok());
            CHECK_EQ(process.ReadValue<std::uint64_t>(world).value_or(0), marker);
            std::uint64_t raw = 0;
            SIZE_T done = 0;
            ::ReadProcessMemory(process.Handle(), reinterpret_cast<LPCVOID>(world), &raw, sizeof(raw), &done);
            CHECK_EQ(raw, stored.value_or(1));
            Status allocated;
            const std::uint64_t fake = process.Allocate(0x100, PAGE_EXECUTE_READWRITE, world, allocated);
            CHECK(allocated.Ok() && fake != 0);
            const auto region = process.RegionAt(fake);
            CHECK(!region || region->State != MEM_COMMIT);
            CHECK(process.Free(fake).Ok());
            Target::Process::SetDryRun(false);
            CHECK_EQ(process.ReadValue<std::uint64_t>(world).value_or(0), stored.value_or(1));
        }
        {
            engine.SetAsync(true);
            const auto pump = [&](const std::function<bool()>& done)
                {
                    const auto start = std::chrono::steady_clock::now();
                    while (!done() && std::chrono::steady_clock::now() - start < std::chrono::seconds(15))
                    {
                        engine.Tick();
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    return done();
                };

            bool finished = false;
            Status result;
            std::string output;
            const int task = engine.Lua().StartTask("local s = 0 for i = 1, 3000000 do s = s + i end return tostring(s)", "async sum",
                [&](const Status& status, const std::string& value)
                {
                    result = status;
                    output = value;
                    finished = true;
                });
            CHECK(task > 0);
            int slices = 0;
            while (!finished && slices < 20000)
            {
                engine.Lua().RunTasks(0.002);
                ++slices;
            }
            CHECK(finished);
            CHECK_MSG(result.Ok(), result.Text);
            CHECK_EQ(output, std::string("4500001500000"));
            CHECK_MSG(slices > 1, "the task ran in one go instead of in slices");

            bool stopped = false;
            const int endless = engine.Lua().StartTask("while true do end", "async endless", [&](const Status& status, const std::string&) { stopped = !status.Ok(); });
            engine.Lua().RunTasks(0.002);
            CHECK(engine.Lua().TaskAlive(endless));
            engine.Lua().StopTask(endless);
            for (int i = 0; i < 100 && engine.Lua().TaskAlive(endless); ++i)
                engine.Lua().RunTasks(0.002);
            CHECK(!engine.Lua().TaskAlive(endless));
            CHECK(stopped);

            Table::MemoryRecord& asyncScript = engine.Table().AddScript("async script",
                "[ENABLE]\n{$lua}\nasyncFlag = (asyncFlag or 0) + 1\n{$asm}\nalloc(asyncmem,16)\nregistersymbol(asyncmem)\n[DISABLE]\nunregistersymbol(asyncmem)\ndealloc(asyncmem)\n");
            const int scriptId = asyncScript.Id;
            engine.RequestActive(asyncScript, true);
            CHECK(engine.ActivationPending(scriptId));
            CHECK(!asyncScript.Active);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK(!engine.ActivationPending(scriptId));
            CHECK_MSG(asyncScript.Active, asyncScript.LastError);
            CHECK(engine.Symbols().FindRegistered("asyncmem").has_value());
            std::string flag;
            CHECK(engine.Lua().RunWithResult("return tostring(asyncFlag)", "async flag", flag).Ok());
            CHECK_EQ(flag, std::string("1"));

            Table::MemoryRecord& group = engine.Table().AddGroup("async group");
            group.Options.ActivateChildren = true;
            group.Options.DeactivateChildren = true;
            Table::MemoryRecord& child = engine.Table().AddScript("async child", "[ENABLE]\n{$lua}\nasyncChild = 'on'\n{$asm}\n[DISABLE]\n{$lua}\nasyncChild = 'off'\n{$asm}\n", &group);
            const int childId = child.Id;
            engine.RequestActive(group, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK(group.Active);
            CHECK(engine.Table().FindById(childId) && engine.Table().FindById(childId)->Active);

            bool waited = false;
            Status waitResult;
            engine.Lua().StartTask(
                "local list = getAddressList()\n"
                "local record = list:getMemoryRecordByDescription('async script')\n"
                "record.Active = false\n"
                "asyncAfter = tostring(record.Active)\n"
                "return asyncAfter",
                "async wait", [&](const Status& status, const std::string& value)
                {
                    waitResult = status;
                    waited = value == "false";
                });
            CHECK(pump([&]() { return waited || !waitResult.Ok(); }));
            CHECK_MSG(waitResult.Ok(), waitResult.Text);
            CHECK_MSG(waited, "memrec.Active did not wait for the switch");
            CHECK(!engine.Symbols().FindRegistered("asyncmem").has_value());

            engine.RequestActive(group, false);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK(!group.Active);
            std::string childState;
            CHECK(engine.Lua().RunWithResult("return asyncChild", "async child", childState).Ok());
            CHECK_EQ(childState, std::string("off"));

            const auto orderScript = [](const std::string& name)
                {
                    return "[ENABLE]\n{$lua}\nasyncOrder = (asyncOrder or '') .. '+" + name + "'\n{$asm}\n[DISABLE]\n{$lua}\nasyncOrder = (asyncOrder or '') .. '-" + name + "'\n{$asm}\n";
                };
            Table::MemoryRecord& tree = engine.Table().AddScript("order parent", orderScript("P"));
            tree.Options.ActivateChildren = true;
            tree.Options.DeactivateChildren = true;
            engine.Table().AddScript("order a", orderScript("A"), &tree);
            engine.Table().AddScript("order b", orderScript("B"), &tree);
            engine.Table().AddScript("order c", orderScript("C"), &tree);
            const auto order = [&]()
                {
                    std::string value;
                    engine.Lua().RunWithResult("return asyncOrder", "order", value);
                    return value;
                };
            engine.Lua().Run("asyncOrder = ''", "order reset");
            engine.RequestActive(tree, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            engine.RequestActive(tree, false);
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));

            engine.Lua().Run("asyncOrder = ''", "order reset");
            engine.RequestActive(tree, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            engine.RequestDeactivateAll();
            CHECK(pump([&]() { return !engine.Working(); }));
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));

            engine.Lua().Run("asyncOrder = ''", "order reset");
            engine.RequestActive(tree, true);
            CHECK(pump([&]() { return !engine.Working(); }));
            Table::MemoryRecord& restore = engine.Table().AddScript("restore state",
                "[ENABLE]\n{$lua}\nif syntaxcheck then return end\n"
                "local list = getAddressList()\n"
                "for i = list.Count - 1, 0, -1 do\n"
                "  local mr = list:getMemoryRecord(i)\n"
                "  if mr and mr.Active then mr.Active = false end\n"
                "end\n"
                "restoreSeen = tostring(list:getMemoryRecordByDescription('order parent').Active)\n"
                "{$asm}\n[DISABLE]\n");
            const int restoreId = restore.Id;
            engine.RequestActive(restore, true);
            CHECK_MSG(pump([&]() { return !engine.Working(); }), "a script that switches other records from its own Lua never finished");
            CHECK(!engine.ActivationPending(restoreId));
            CHECK_MSG(restore.Active, restore.LastError);
            CHECK(!tree.Active);
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));
            std::string restoreSeen;
            CHECK(engine.Lua().RunWithResult("return restoreSeen", "restore seen", restoreSeen).Ok());
            CHECK_EQ(restoreSeen, std::string("false"));
            engine.SetAsync(false);

            engine.Lua().Run("asyncOrder = ''", "order reset");
            CHECK(engine.SetActive(tree, true).Ok());
            CHECK(engine.SetActive(tree, false).Ok());
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));

            engine.Lua().Run("asyncOrder = ''", "order reset");
            CHECK(engine.SetActive(tree, true).Ok());
            CHECK(engine.DeactivateAll().empty());
            CHECK_EQ(order(), std::string("+P+A+B+C-C-B-A-P"));
        }
        engine.CloseProcess();
        return Totals().Failures - before;
    }
}
