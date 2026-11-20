#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Debug/BreakAction.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace Manifold::Tests
{
    namespace
    {
        void Wait(Engine& engine, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        bool WaitForBreak(Engine& engine, std::uint64_t afterSerial, int milliseconds)
        {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
            while (std::chrono::steady_clock::now() < until)
            {
                const Debug::BreakInfo info = engine.Debugger().Break();
                if (info.Broken && info.Serial != afterSerial)
                    return true;
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }

        std::uint64_t HitsAt(Engine& engine, std::uint64_t address)
        {
            for (const Debug::Breakpoint& breakpoint : engine.Debugger().Breakpoints())
            {
                if (breakpoint.Address == address)
                    return breakpoint.Hits;
            }
            return 0;
        }

        Debug::BreakAction Action(Engine& engine, const std::string& condition, const std::string& changes, bool stop)
        {
            Debug::BreakAction action;
            action.Condition = condition;
            action.Changes = changes;
            action.Break = stop;
            const Debug::SymbolResolver resolve = [&engine](std::string_view text) { return engine.Evaluate(std::string(text)); };
            const Status compiled = Debug::CompileAction(action, engine.Process()->Is64Bit(), resolve);
            CHECK_MSG(compiled.Ok(), compiled.Text);
            return action;
        }
    }

    void CheckBreakActions(Engine& engine, bool veh)
    {
        const Target::Process& process = *engine.Process();
        Debug::Debugger& debugger = engine.Debugger();
        const char* backend = veh ? "in-process" : "windows";
        const std::uint64_t shared = engine.Evaluate("TestTarget.g_shared").value_or(0);
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        const std::uint64_t damage = engine.Evaluate("TestTarget.Damage").value_or(0);
        const std::uint64_t world = process.ReadPointer(engine.Evaluate("TestTarget.g_world").value_or(0)).value_or(0);
        const std::uint64_t hero = world ? process.ReadPointer(world + 8).value_or(0) : 0;
        CHECK_MSG(shared && work && damage && hero, backend);
        if (!shared || !work || !damage || !hero)
            return;

        {
            const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false, Action(engine, "EDX == 2 or readInteger(RCX) < 0", {}, true));
            CHECK_MSG(set.Ok(), set.Text);
            Wait(engine, 2300);
            CHECK_MSG(!debugger.Broken(), std::string(backend) + ": stopped although the condition never holds");
            CHECK_EQ(HitsAt(engine, damage), 0ull);
            CHECK(debugger.RemoveBreakpoint(damage).Ok());
            if (debugger.Broken())
                debugger.Resume(Debug::ResumeMode::Continue);
        }

        {
            const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false, Action(engine, "readInteger(RCX) % 50 == 7", {}, true));
            CHECK_MSG(set.Ok(), set.Text);
            const bool broke = set && WaitForBreak(engine, 0, 4000);
            CHECK_MSG(broke, std::string(backend) + ": the condition never held");
            if (broke)
            {
                const Debug::BreakInfo info = debugger.Break();
                std::int32_t value = -1;
                process.ReadExact(info.Registers.Rcx, &value, sizeof(value));
                CHECK_MSG(info.Address == work && info.Registers.Rcx == shared, std::string(backend) + " " + Hex(info.Address));
                const int drift = ((value % 50) - 7 + 50) % 50;
                CHECK_MSG(veh ? drift < 25 : drift == 0, std::string(backend) + ": stopped with g_shared = " + std::to_string(value) + ", hits " + std::to_string(HitsAt(engine, work)));
                CHECK_MSG(HitsAt(engine, work) >= 1 || veh, backend);
            }
            CHECK(debugger.RemoveBreakpoint(work).Ok());
            if (broke)
                debugger.Resume(Debug::ResumeMode::Continue);
            Wait(engine, 100);
        }

        {
            const Status set = debugger.SetBreakpoint(damage, Debug::BreakpointType::Execute, 1, false, Action(engine, {}, "EDX = 0", false));
            CHECK_MSG(set.Ok(), set.Text);
            const auto armed = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
            while (set && HitsAt(engine, damage) == 0 && std::chrono::steady_clock::now() < armed)
                Wait(engine, 20);
            CHECK_MSG(HitsAt(engine, damage) >= 1, std::string(backend) + ": the register-changing breakpoint was never hit");
            std::int32_t before = 0;
            process.ReadExact(hero, &before, sizeof(before));
            Wait(engine, 2700);
            std::int32_t after = 0;
            process.ReadExact(hero, &after, sizeof(after));
            CHECK_MSG(after == before, std::string(backend) + ": health went from " + std::to_string(before) + " to " + std::to_string(after));
            CHECK_MSG(!debugger.Broken(), std::string(backend) + ": a breakpoint that only changes registers stopped the process");
            CHECK_MSG(HitsAt(engine, damage) >= 2, std::string(backend) + ": " + std::to_string(HitsAt(engine, damage)) + " hits");
            CHECK(debugger.RemoveBreakpoint(damage).Ok());
            if (debugger.Broken())
                debugger.Resume(Debug::ResumeMode::Continue);
            Wait(engine, 1200);
            std::int32_t later = 0;
            process.ReadExact(hero, &later, sizeof(later));
            CHECK_MSG(later < after, std::string(backend) + ": Damage still did nothing after the breakpoint was removed");
        }

        {
            const Status set = debugger.SetBreakpoint(shared, Debug::BreakpointType::Write, 4, true,
                Action(engine, "readInteger(0x" + Hex(shared) + ") % 100 == 0", {}, true));
            CHECK_MSG(set.Ok(), set.Text);
            const bool broke = set && WaitForBreak(engine, 0, 4000);
            CHECK_MSG(broke, std::string(backend) + ": the data breakpoint did not stop");
            if (broke)
            {
                const Debug::BreakInfo info = debugger.Break();
                std::int32_t value = -1;
                process.ReadExact(shared, &value, sizeof(value));
                CHECK_MSG(info.Reason.find("Write") != std::string::npos, std::string(backend) + ": " + info.Reason);
                CHECK_MSG(info.Address > work && info.Address < work + 0x40, std::string(backend) + ": stopped at " + Hex(info.Address));
                CHECK_MSG(veh ? value % 100 < 50 : value % 100 == 0, std::string(backend) + ": g_shared = " + std::to_string(value) + ", hits " + std::to_string(HitsAt(engine, shared)));
            }
            CHECK(debugger.RemoveBreakpoint(shared).Ok());
            if (broke)
                debugger.Resume(Debug::ResumeMode::Continue);
            Wait(engine, 100);
        }
        CHECK_MSG(process.Alive(), std::string(backend) + ": the target died");
    }

    void CheckWatchesOnOneInstruction(Engine& engine, bool veh)
    {
        const Target::Process& process = *engine.Process();
        Debug::Debugger& debugger = engine.Debugger();
        const char* backend = veh ? "in-process" : "windows";
        const std::uint64_t damage = engine.Evaluate("TestTarget.Damage").value_or(0);
        const std::uint64_t world = process.ReadPointer(engine.Evaluate("TestTarget.g_world").value_or(0)).value_or(0);
        const std::uint64_t health = world ? process.ReadPointer(world + 8).value_or(0) : 0;
        CHECK_MSG(damage && health, backend);
        if (!damage || !health)
            return;
        const Disasm::Instruction first = engine.Disassembler().Read(process, damage, nullptr);
        CHECK_MSG(first.Memory.Written && first.Memory.Size == 4, std::string(backend) + ": Damage starts with " + first.Text());

        const auto hits = [&debugger](int id)
            {
                Debug::Watch copy;
                return debugger.CopyWatch(id, copy) ? copy.Hits : 0ull;
            };
        const auto countAt = [&debugger](int id, std::uint64_t instruction)
            {
                Debug::Watch copy;
                std::uint64_t count = 0;
                if (debugger.CopyWatch(id, copy))
                {
                    for (const Debug::WatchEntry& entry : copy.Entries)
                        count += entry.Instruction == instruction ? entry.Count : 0;
                }
                return count;
            };
        const auto callAfter = [&](int calls, std::uint64_t seen)
            {
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
                while (hits(calls) <= seen && std::chrono::steady_clock::now() < until)
                    Wait(engine, 20);
                Wait(engine, 150);
                return hits(calls) > seen;
            };

        struct Run
        {
            Debug::WatchKind Kind;
            bool LostBits;
        };
        std::vector<Run> runs = { { Debug::WatchKind::Writes, false } };
        if (!veh)
            runs.insert(runs.end(), { { Debug::WatchKind::Writes, true }, { Debug::WatchKind::Accesses, false }, { Debug::WatchKind::Accesses, true } });
        for (const Run& run : runs)
        {
            const std::string label = std::string(backend) + ", " + Debug::WatchKindName(run.Kind) + (run.LostBits ? ", step without data bits" : "");
            debugger.SimulateMissingStepBits(run.LostBits);
            int calls = 0;
            int data = 0;
            CHECK_MSG(debugger.StartWatch(Debug::WatchKind::Calls, damage, 1, calls).Ok(), label);
            CHECK_MSG(debugger.StartWatch(run.Kind, health, 4, data).Ok(), label);
            CHECK_MSG(callAfter(calls, 0), label + ": Damage was not called");
            const std::uint64_t dataBefore = countAt(data, damage);
            const std::uint64_t callsBefore = hits(calls);
            CHECK_MSG(callAfter(calls, callsBefore), label + ": Damage was not called again");
            const std::uint64_t dataAfter = countAt(data, damage);
            const std::uint64_t callsAfter = hits(calls);
            CHECK_MSG(callsAfter > callsBefore && dataAfter - dataBefore == callsAfter - callsBefore,
                label + ": " + std::to_string(dataAfter - dataBefore) + " hits at Damage for " + std::to_string(callsAfter - callsBefore) + " calls");
            if (run.Kind == Debug::WatchKind::Writes)
            {
                Debug::Watch watch;
                CHECK(debugger.CopyWatch(data, watch));
                CHECK_MSG(watch.Entries.size() == 1 && watch.Entries.front().Instruction == damage,
                    label + ": " + (watch.Entries.empty() ? std::string("no write recorded") : std::to_string(watch.Entries.size()) + " writers, first " + Hex(watch.Entries.front().Instruction)));
            }
            debugger.RemoveWatch(data);
            debugger.RemoveWatch(calls);
        }

        if (!veh)
        {
            debugger.SimulateMissingStepBits(true);
            int calls = 0;
            CHECK(debugger.StartWatch(Debug::WatchKind::Calls, damage, 1, calls).Ok());
            const std::uint64_t serial = debugger.Break().Serial;
            const Status set = debugger.SetBreakpoint(health, Debug::BreakpointType::Write, 4, true);
            CHECK_MSG(set.Ok(), set.Text);
            const bool broke = set && WaitForBreak(engine, serial, 4000);
            CHECK_MSG(broke, "windows, step without data bits: the write breakpoint did not stop");
            if (broke)
            {
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(info.Reason.find("Write") != std::string::npos && info.BreakpointAddress == health, info.Reason);
                CHECK_MSG(info.Address == first.Next(), "windows, step without data bits: stopped at " + Hex(info.Address));
            }
            CHECK(debugger.RemoveBreakpoint(health).Ok());
            if (broke)
                debugger.Resume(Debug::ResumeMode::Continue);
            debugger.RemoveWatch(calls);
        }
        debugger.SimulateMissingStepBits(false);
        Wait(engine, 100);
        CHECK_MSG(process.Alive(), std::string(backend) + ": the target died");
    }

    void CheckHostSilence(Engine& engine)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const Target::Process& process = *engine.Process();
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        const std::uint64_t shared = engine.Evaluate("TestTarget.g_shared").value_or(0);
        CHECK(work && shared);
        if (!work || !shared)
            return;
        const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
        CHECK_MSG(set.Ok(), set.Text);
        const bool broke = set && WaitForBreak(engine, 0, 4000);
        CHECK_MSG(broke, "in-process: no break to test the heartbeat with");
        if (!broke)
        {
            debugger.RemoveBreakpoint(work);
            return;
        }
        std::int32_t first = 0;
        process.ReadExact(shared, &first, sizeof(first));
        debugger.PauseHeartbeat(true);
        Wait(engine, 2000);
        std::int32_t held = 0;
        process.ReadExact(shared, &held, sizeof(held));
        CHECK_MSG(held == first && debugger.Broken(), "in-process: the break ended before Manifold went silent long enough");
        Wait(engine, 3500);
        std::int32_t later = 0;
        process.ReadExact(shared, &later, sizeof(later));
        CHECK_MSG(later != first, "in-process: the target stayed stopped after Manifold went silent");
        CHECK_MSG(!debugger.Broken(), "in-process: still shown as stopped after the agent let go");
        debugger.PauseHeartbeat(false);
        CHECK(process.Alive());
    }

    namespace
    {
        bool ThreadWatches(HANDLE thread, std::uint64_t address)
        {
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (!::GetThreadContext(thread, &context))
                return false;
            const DWORD64 slots[4] = { context.Dr0, context.Dr1, context.Dr2, context.Dr3 };
            for (int slot = 0; slot < 4; ++slot)
            {
                if (slots[slot] == address && (context.Dr7 & (1ull << (slot * 2))) != 0)
                    return true;
            }
            return false;
        }

        std::filesystem::path TargetBesideTests()
        {
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            return std::filesystem::path(self).parent_path() / L"TestTarget.exe";
        }

        bool StartTestTarget(PROCESS_INFORMATION& info, const std::filesystem::path& targetPath = TargetBesideTests())
        {
            std::wstring command = L"\"" + targetPath.wstring() + L"\"";
            STARTUPINFOW startup{ sizeof(startup) };
            if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
            {
                std::printf("could not start %ls\n", targetPath.c_str());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            return true;
        }

        void EndTestTarget(PROCESS_INFORMATION& info, DWORD& code)
        {
            ::GetExitCodeProcess(info.hProcess, &code);
            ::TerminateProcess(info.hProcess, 0);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
        }
    }

    namespace
    {
        bool AgentLoaded(DWORD processId)
        {
            HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            bool found = false;
            if (::Module32FirstW(snapshot, &entry))
            {
                do
                {
                    found = found || _wcsicmp(entry.szModule, L"ManifoldAgent.dll") == 0 || _wcsicmp(entry.szModule, L"ManifoldAgent32.dll") == 0;
                } while (::Module32NextW(snapshot, &entry));
            }
            ::CloseHandle(snapshot);
            return found;
        }

        std::int32_t ReadShared(Engine& engine)
        {
            std::int32_t value = 0;
            engine.Process()->ReadExact(engine.Evaluate("TestTarget.g_shared").value_or(0), &value, sizeof(value));
            return value;
        }

        bool SharedMoves(Engine& engine, int milliseconds)
        {
            const std::int32_t before = ReadShared(engine);
            Wait(engine, milliseconds);
            return ReadShared(engine) != before;
        }

        void SetFaults(Engine& engine, bool on)
        {
            const std::int32_t value = on ? 1 : 0;
            const std::uint64_t address = engine.Evaluate("TestTarget.g_faultsOn").value_or(0);
            const Status written = engine.Process()->Write(address, &value, sizeof(value));
            std::int32_t back = -1;
            engine.Process()->ReadExact(address, &back, sizeof(back));
            CHECK_MSG(written.Ok() && back == value, "g_faultsOn at " + Hex(address) + " reads " + std::to_string(back) + " " + written.Text);
        }
    }

    void CheckFpuAndExceptions(Engine& engine, bool veh)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const Target::Process& process = *engine.Process();
        const char* backend = veh ? "in-process" : "windows";
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        const std::uint64_t fault = engine.Evaluate("TestTarget.Fault").value_or(0);
        const std::uint64_t handled = engine.Evaluate("TestTarget.g_faultsHandled").value_or(0);
        CHECK_MSG(work && fault && handled, backend);
        if (!work || !fault || !handled)
            return;

        for (const double value : { 0.0, -0.0, 1.0, -2.5, 3.141592653589793, 1e-310, 6.02214076e23, -1e300 })
        {
            std::uint8_t ten[10] = {};
            Debug::DoubleToExtended(value, ten);
            const double back = Debug::ExtendedToDouble(ten);
            CHECK_MSG(back == value && std::signbit(back) == std::signbit(value), Format("%.17g came back as %.17g", value, back));
        }
        {
            const std::uint8_t one[10] = { 0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0x3F };
            CHECK_EQ(Debug::ExtendedToDouble(one), 1.0);
            const std::uint8_t infinity[10] = { 0, 0, 0, 0, 0, 0, 0, 0x80, 0xFF, 0xFF };
            CHECK_EQ(Debug::ExtendedText(infinity), std::string("-Inf"));
        }

        {
            const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
            CHECK_MSG(set.Ok(), set.Text);
            const bool broke = set && WaitForBreak(engine, 0, 4000);
            CHECK_MSG(broke, std::string(backend) + ": no break to read the FPU at");
            if (broke)
            {
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(info.Fpu.Valid, std::string(backend) + ": the break came without x87 registers");
                CHECK_MSG(info.Fpu.Control() == 0x027F, std::string(backend) + ": x87 control word " + Hex(info.Fpu.Control(), 4));
                CHECK_MSG(!info.Exception.Raised, backend);
            }
            CHECK(debugger.RemoveBreakpoint(work).Ok());
            if (broke)
                debugger.Resume(Debug::ResumeMode::Continue);
            Wait(engine, 100);
        }

        std::int32_t handledBefore = 0;
        process.ReadExact(handled, &handledBefore, sizeof(handledBefore));
        debugger.SetExceptionBreak(Debug::ExceptionBreak::InRegion, work, work + 1);
        SetFaults(engine, true);
        Wait(engine, 1500);
        std::int32_t handledOutside = 0;
        process.ReadExact(handled, &handledOutside, sizeof(handledOutside));
        CHECK_MSG(!debugger.Broken(), std::string(backend) + ": stopped on an exception outside the region");
        CHECK_MSG(handledOutside > handledBefore, std::string(backend) + ": the program did not handle its own exceptions (" + std::to_string(handledBefore) + " -> " + std::to_string(handledOutside) + ")");
        std::printf("  exception check %s: handled %d -> %d\n", backend, static_cast<int>(handledBefore), static_cast<int>(handledOutside));
        std::fflush(stdout);
        if (debugger.Broken())
            debugger.Resume(Debug::ResumeMode::Continue);

        debugger.SetExceptionBreak(Debug::ExceptionBreak::InRegion, fault, fault + 0x100);
        const std::uint64_t serial = debugger.Break().Serial;
        const bool broke = WaitForBreak(engine, serial, 4000);
        CHECK_MSG(broke, std::string(backend) + ": no stop on the access violation inside the region");
        if (broke)
        {
            const Debug::BreakInfo info = debugger.Break();
            CHECK_MSG(info.Exception.Raised && info.Exception.Code == 0xC0000005, std::string(backend) + ": stopped for " + info.Reason);
            CHECK_MSG(info.Exception.ParamCount == 2 && info.Exception.Params[0] == 1 && info.Exception.Params[1] == 0x10,
                std::string(backend) + ": " + Debug::DescribeException(info.Exception));
            CHECK_MSG(info.Address >= fault && info.Address < fault + 0x100, std::string(backend) + " " + Hex(info.Address));
            CHECK_MSG(info.Reason.find("writing 10") != std::string::npos, info.Reason);
            debugger.SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
            std::int32_t handledAtBreak = 0;
            process.ReadExact(handled, &handledAtBreak, sizeof(handledAtBreak));
            debugger.Resume(Debug::ResumeMode::Continue);
            Wait(engine, 600);
            std::int32_t handledAfter = 0;
            process.ReadExact(handled, &handledAfter, sizeof(handledAfter));
            CHECK_MSG(handledAfter > handledAtBreak, std::string(backend) + ": after Run the program did not get its exception");
        }

        debugger.SetExceptionBreak(Debug::ExceptionBreak::Always, 0, 0);
        const bool again = WaitForBreak(engine, debugger.Break().Serial, 4000);
        CHECK_MSG(again, std::string(backend) + ": no stop with Always");
        if (again)
        {
            Debug::BreakInfo info = debugger.Break();
            debugger.SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
            const Disasm::Instruction faulting = engine.Disassembler().Read(process, info.Registers.Rip, nullptr);
            CHECK(faulting.Valid);
            if (faulting.Valid && info.Exception.Code == 0xC0000005)
            {
                Disasm::RegisterValues skip = info.Registers;
                skip.Rip = faulting.Next();
                debugger.SetRegisters(skip);
            }
            debugger.Resume(Debug::ResumeMode::Continue);
        }
        SetFaults(engine, false);
        debugger.SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
        Wait(engine, 300);
        if (debugger.Broken())
            debugger.Resume(Debug::ResumeMode::Continue);
        CHECK_MSG(process.Alive(), std::string(backend) + ": the target died after the exception checks");
        CHECK_MSG(SharedMoves(engine, 300), std::string(backend) + ": the target stopped running after the exception checks");
    }

    void CheckDetachPaths(Engine& engine)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const DWORD pid = Raw(engine.Process()->Id());
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        const std::uint64_t shared = engine.Evaluate("TestTarget.g_shared").value_or(0);
        CHECK(work && shared);
        if (!work || !shared)
            return;

        for (int round = 1; round <= 3; ++round)
        {
            const std::string label = "detach round " + std::to_string(round);
            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok(), label + ": " + attached.Text);
            if (!attached)
                return;
            if (round == 1)
            {
                const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
                CHECK_MSG(set.Ok(), set.Text);
                CHECK_MSG(set && WaitForBreak(engine, 0, 4000), label + ": no break before detaching");
            }
            else if (round == 2)
            {
                int writes = 0;
                const Status started = debugger.StartWatch(Debug::WatchKind::Writes, shared, 4, writes);
                CHECK_MSG(started.Ok(), started.Text);
                Wait(engine, 500);
            }
            else
            {
                const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
                CHECK_MSG(set.Ok(), set.Text);
                CHECK_MSG(set && WaitForBreak(engine, 0, 4000), label + ": no break before stepping");
                debugger.Resume(Debug::ResumeMode::StepInto);
            }
            const auto started = std::chrono::steady_clock::now();
            debugger.Detach();
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            CHECK_MSG(took < 5000, label + ": detaching took " + std::to_string(took) + " ms");
            CHECK_MSG(!debugger.Attached(), label);
            CHECK_MSG(SharedMoves(engine, 400), label + ": the target does not run after detaching");
            Wait(engine, 1500);
            CHECK_MSG(engine.Process()->Alive(), label + ": the target died after detaching");
            CHECK_MSG(!AgentLoaded(pid), label + ": the agent is still loaded after detaching");
        }
    }

    int RunVehHost(const std::vector<std::string>& arguments)
    {
        DWORD pid = 0;
        std::string how = "normal";
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--pid=", 0) == 0)
                pid = static_cast<DWORD>(std::strtoul(argument.c_str() + 6, nullptr, 10));
            if (argument.rfind("--exit=", 0) == 0)
                how = argument.substr(7);
        }
        Settings settings;
        settings.UseInProcessDebugger = true;
        Engine engine(settings);
        if (!engine.OpenProcess(MakeProcessId(pid)) || !engine.EnsureDebugger())
            return 3;
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        if (!work || !engine.Debugger().SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false) || !WaitForBreak(engine, 0, 4000))
            return 4;
        if (how == "kill")
            ::TerminateProcess(::GetCurrentProcess(), 9);
        return 0;
    }

    void CheckHostExits(const std::filesystem::path& targetPath)
    {
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        for (const char* how : { "normal", "kill" })
        {
            PROCESS_INFORMATION target{};
            if (!StartTestTarget(target, targetPath))
            {
                CHECK_MSG(false, "could not start the test target for the host exit check");
                return;
            }
            std::wstring command = L"\"" + std::wstring(self) + L"\" --veh-host --pid=" + std::to_wstring(target.dwProcessId) + L" --exit=" + Utf8ToWide(how);
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION host{};
            const bool started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &host) != FALSE;
            CHECK_MSG(started, std::string("could not start the host for ") + how);
            DWORD hostCode = 0;
            if (started)
            {
                ::WaitForSingleObject(host.hProcess, 30000);
                ::GetExitCodeProcess(host.hProcess, &hostCode);
                ::CloseHandle(host.hThread);
                ::CloseHandle(host.hProcess);
            }
            CHECK_MSG(hostCode == 0 || (std::string(how) == "kill" && hostCode == 9), std::string(how) + ": the host ended with " + std::to_string(hostCode));
            const std::uint64_t until = ::GetTickCount64() + (std::string(how) == "kill" ? 8000 : 2000);
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            bool moving = false;
            while (opened && !moving && ::GetTickCount64() < until)
                moving = SharedMoves(engine, 200);
            CHECK_MSG(moving, std::string(how) + " host exit: the target did not run again");
            Wait(engine, 1200);
            DWORD code = 0;
            ::GetExitCodeProcess(target.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, std::string(how) + " host exit: the target ended with " + Hex(code, 8));
            CHECK_MSG(!AgentLoaded(target.dwProcessId), std::string(how) + " host exit: the agent is still loaded");
            EndTestTarget(target, code);
        }
    }

    int RunVehDetachStress(const std::vector<std::string>& arguments)
    {
        int rounds = 20;
        bool breakFirst = false;
        std::string module = "TestTarget";
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--rounds=", 0) == 0)
                rounds = std::max(1, std::atoi(argument.c_str() + 9));
            breakFirst = breakFirst || argument == "--break";
            if (argument == "--32")
                module = "TestTarget32";
        }
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::filesystem::path targetPath = std::filesystem::path(self).parent_path() / Utf8ToWide(module + ".exe");
        int died = 0;
        int stuck = 0;
        int loaded = 0;
        for (int round = 1; round <= rounds; ++round)
        {
            std::wstring command = L"\"" + targetPath.wstring() + L"\" --stress";
            STARTUPINFOW startup{ sizeof(startup) };
            PROCESS_INFORMATION info{};
            if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
                return 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            {
                Settings settings;
                settings.UseInProcessDebugger = true;
                Engine engine(settings);
                if (!engine.OpenProcess(MakeProcessId(info.dwProcessId)) || !engine.EnsureDebugger())
                    return 3;
                const std::uint64_t stress = engine.Evaluate(module + ".g_stress").value_or(0);
                int watch = 0;
                engine.Debugger().StartWatch(Debug::WatchKind::Writes, stress, 8, watch);
                if (breakFirst)
                {
                    const std::uint64_t touch = engine.Evaluate(module + ".Touch").value_or(0);
                    engine.Debugger().SetBreakpoint(touch, Debug::BreakpointType::Execute, 1, false);
                    WaitForBreak(engine, 0, 3000);
                }
                Wait(engine, 300);
                engine.Debugger().Detach();
                Wait(engine, 1500);
            }
            DWORD code = 0;
            ::GetExitCodeProcess(info.hProcess, &code);
            const bool alive = code == STILL_ACTIVE;
            const bool agent = alive && AgentLoaded(info.dwProcessId);
            died += alive ? 0 : 1;
            loaded += agent ? 1 : 0;
            if (!alive || agent)
                std::printf("  round %d: %s%s\n", round, alive ? "alive" : ("ended " + Hex(code, 8)).c_str(), agent ? ", agent still loaded" : "");
            ::TerminateProcess(info.hProcess, 0);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            std::fflush(stdout);
        }
        std::printf("detach under load: %d rounds, target died %d, agent left loaded %d, stuck %d\n", rounds, died, loaded, stuck);
        return died == 0 ? 0 : 1;
    }

    void CheckAttachArmsMainThread(Engine& engine, HANDLE mainThread, std::uint64_t address)
    {
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        const bool pinned = ::GetProcessAffinityMask(::GetCurrentProcess(), &processMask, &systemMask) &&
                            ::SetProcessAffinityMask(::GetCurrentProcess(), processMask & (0 - processMask));
        CHECK_MSG(pinned, "could not run the attach check on one processor");
        for (int round = 1; round <= 3; ++round)
        {
            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok(), attached.Text);
            if (!attached)
                break;
            int writes = 0;
            const Status started = engine.Debugger().StartWatch(Debug::WatchKind::Writes, address, 4, writes);
            CHECK_MSG(started.Ok(), started.Text);
            CHECK_MSG(ThreadWatches(mainThread, address), "attach round " + std::to_string(round) + ": a watch started right after attaching left the main thread out");
            engine.Debugger().RemoveWatch(writes);
            engine.Debugger().Detach();
        }
        if (pinned)
            ::SetProcessAffinityMask(::GetCurrentProcess(), processMask);
    }

    int RunWatchArmCheck(const std::vector<std::string>& arguments)
    {
        int rounds = 20;
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--rounds=", 0) == 0)
                rounds = std::max(1, std::atoi(argument.c_str() + 9));
            if (argument == "--one-cpu")
                ::SetProcessAffinityMask(::GetCurrentProcess(), 1);
        }
        PROCESS_INFORMATION info{};
        if (!StartTestTarget(info))
            return 2;
        int unarmed = 0;
        int stillUnarmed = 0;
        int unrecorded = 0;
        {
            Settings settings;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            const Target::Process& process = *engine.Process();
            const std::uint64_t world = process.ReadPointer(engine.Evaluate("TestTarget.g_world").value_or(0)).value_or(0);
            const std::uint64_t health = world ? process.ReadPointer(world + 8).value_or(0) : 0;
            CHECK(health != 0);
            for (int round = 0; round < rounds && opened && health; ++round)
            {
                const Status attached = engine.EnsureDebugger();
                int writes = 0;
                const Status started = attached ? engine.Debugger().StartWatch(Debug::WatchKind::Writes, health, 4, writes) : attached;
                CHECK_MSG(started.Ok(), started.Text);
                const bool armed = ThreadWatches(info.hThread, health);
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
                Debug::Watch watch;
                while (std::chrono::steady_clock::now() < until && !(engine.Debugger().CopyWatch(writes, watch) && !watch.Entries.empty()))
                    Wait(engine, 20);
                const bool recorded = engine.Debugger().CopyWatch(writes, watch) && !watch.Entries.empty();
                const bool armedLater = ThreadWatches(info.hThread, health);
                unarmed += armed ? 0 : 1;
                stillUnarmed += armedLater ? 0 : 1;
                unrecorded += recorded ? 0 : 1;
                if (!armed || !armedLater || !recorded)
                    std::printf("  round %d: main thread %s right after the watch started, %s after the wait, %s\n", round + 1, armed ? "armed" : "NOT armed",
                        armedLater ? "armed" : "NOT armed", recorded ? "write recorded" : "NO write recorded");
                engine.Debugger().RemoveWatch(writes);
                engine.Debugger().Detach();
                Wait(engine, 50);
            }
            CHECK(engine.Process()->Alive());
        }
        DWORD code = 0;
        EndTestTarget(info, code);
        std::printf("watch arming: %d rounds, main thread not armed right after StartWatch in %d, still not armed 2.5 s later in %d, no write recorded in %d\n", rounds,
            unarmed, stillUnarmed, unrecorded);
        return unarmed == 0 && stillUnarmed == 0 && unrecorded == 0 && Totals().Failures == 0 ? 0 : 1;
    }

    int RunDebuggerCheck(const std::vector<std::string>& arguments)
    {
        bool veh = false;
        for (const std::string& argument : arguments)
            veh = veh || argument == "--veh";
        PROCESS_INFORMATION info{};
        if (!StartTestTarget(info))
            return 2;
        const int before = Totals().Failures;
        {
            Settings settings;
            settings.UseInProcessDebugger = veh;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            const Status attached = opened ? engine.EnsureDebugger() : opened;
            CHECK_MSG(attached.Ok(), attached.Text);
            if (attached)
            {
                CheckBreakActions(engine, veh);
                CheckWatchesOnOneInstruction(engine, veh);
                CheckFpuAndExceptions(engine, veh);
                if (veh)
                    CheckHostSilence(engine);
                engine.Debugger().Detach();
                Wait(engine, 400);
                CHECK(engine.Process()->Alive());
            }
        }
        DWORD code = 0;
        EndTestTarget(info, code);
        const int failures = Totals().Failures - before;
        std::printf("debugger check (%s): %d checks, %d failed, target %s\n", veh ? "in-process" : "windows", Totals().Checks, failures,
            code == STILL_ACTIVE ? "was alive" : ("had ended " + Hex(code, 8)).c_str());
        return failures == 0 ? 0 : 1;
    }

    int RunInProcessTests(const std::filesystem::path& targetPath)
    {
        const int before = Totals().Failures;
        PROCESS_INFORMATION info{};
        if (!StartTestTarget(info, targetPath))
        {
            Skip("the test target " + targetPath.string() + " could not be started");
            return 0;
        }
        {
            Settings settings;
            settings.UseInProcessDebugger = true;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            const Status attached = opened ? engine.EnsureDebugger() : opened;
            CHECK_MSG(attached.Ok(), attached.Text);
            if (attached)
            {
                CHECK(engine.Debugger().UsingVeh());
                CheckBreakActions(engine, true);
                CheckWatchesOnOneInstruction(engine, true);
                CheckFpuAndExceptions(engine, true);
                CheckHostSilence(engine);
                engine.Debugger().Detach();
                Wait(engine, 400);
                CHECK(engine.Process()->Alive());
                CheckDetachPaths(engine);
            }
        }
        DWORD code = 0;
        EndTestTarget(info, code);
        CHECK_MSG(code == STILL_ACTIVE, "the target of the in-process debugger ended with " + Hex(code, 8));
        CheckHostExits(targetPath);
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("inprocess", "In-process debugger (64-bit)", 122, [](const SuiteContext& context)
        {
            std::printf("  target %ls\n", context.Target.c_str());
            return RunInProcessTests(context.Target);
        });

    MANIFOLD_MODE("--debugger-check", &RunDebuggerCheck);
    MANIFOLD_MODE("--watch-arm-check", &RunWatchArmCheck);
    MANIFOLD_MODE("--veh-host", &RunVehHost);
    MANIFOLD_MODE("--veh-detach-stress", &RunVehDetachStress);
}
