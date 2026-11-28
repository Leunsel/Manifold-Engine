#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "App/WriteHistory.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Debug/BreakAction.h"
#include "Debug/Trace.h"

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
            // ∑ Work runs once per 1 ms wait, which is 15.6 ms on most machines, and each hit of the Windows
            //   debugger costs debug events on top: up to 50 calls until the value is 7 modulo 50 came close
            //   to 4 s on the 2-core CI runner (failed once). The wait ends at the break, so it costs nothing
            //   when the machine is fast.
            const bool broke = set && WaitForBreak(engine, 0, 15000);
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
            const bool broke = set && WaitForBreak(engine, 0, 15000);  // up to 100 writes, see the condition above
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

    void CheckTraceOneThread(Engine& engine, bool veh)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const char* backend = veh ? "in-process" : "windows";
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        CHECK_MSG(work, backend);
        if (!work)
            return;
        Debug::TraceRequest request;
        request.Address = work;
        request.Count = 300;
        request.StepOver = veh;
        Debug::Tracer& tracer = engine.Tracer();
        const Status started = tracer.Start(engine.Process(), engine.Disassembler(), request);
        CHECK_MSG(started.Ok(), started.Text);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(60000);
        while (started && tracer.Running() && std::chrono::steady_clock::now() < until)
            Wait(engine, 20);
        CHECK_MSG(!tracer.Running(), std::string(backend) + ": the trace did not end, " + tracer.StatusText());
        if (tracer.Running())
            tracer.Stop();
        const std::vector<Debug::TraceStep> steps = tracer.Steps();
        CHECK_MSG(!steps.empty() && steps.front().Address == work, std::string(backend) + ": " + std::to_string(steps.size()) + " steps, " + tracer.StatusText());
        std::vector<std::uint32_t> threads;
        for (const Debug::TraceStep& step : steps)
        {
            if (std::find(threads.begin(), threads.end(), step.ThreadId) == threads.end())
                threads.push_back(step.ThreadId);
        }
        std::string list;
        for (std::uint32_t thread : threads)
            list += " " + std::to_string(thread);
        CHECK_MSG(threads.size() == 1, std::string(backend) + ": a trace of " + std::to_string(steps.size()) + " steps went through " + std::to_string(threads.size()) +
            " threads:" + list);
        CHECK_MSG(!debugger.Broken() && !debugger.HasBreakpoint(work), std::string(backend) + ": the trace left the process stopped or the breakpoint set");
        if (debugger.Broken())
            debugger.Resume(Debug::ResumeMode::Continue);
        Wait(engine, 100);
        CHECK_MSG(engine.Process()->Alive(), std::string(backend) + ": the target died");

        // Work runs every millisecond: a first hit that comes while the trace is still setting its breakpoint is its first step
        int hung = 0;
        int short_ = 0;
        for (int round = 0; round < 40 && engine.Process()->Alive(); ++round)
        {
            Debug::TraceRequest quick;
            quick.Address = work;
            quick.Count = 3;
            quick.StepOver = veh;
            const Status again = tracer.Start(engine.Process(), engine.Disassembler(), quick);
            if (!again)
            {
                CHECK_MSG(false, std::string(backend) + ": trace " + std::to_string(round) + ": " + again.Text);
                break;
            }
            const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (tracer.Running() && std::chrono::steady_clock::now() < limit)
                Wait(engine, 20);
            if (tracer.Running())
            {
                ++hung;
                tracer.Stop();
                continue;
            }
            const std::vector<Debug::TraceStep> quickSteps = tracer.Steps();
            if (quickSteps.size() != 3 || quickSteps.front().Address != work || debugger.HasBreakpoint(work))
                ++short_;
        }
        CHECK_MSG(hung == 0, std::string(backend) + ": " + std::to_string(hung) + " of 40 traces missed the first hit of their own breakpoint and kept waiting");
        CHECK_MSG(short_ == 0, std::string(backend) + ": " + std::to_string(short_) + " of 40 traces ended without 3 steps from Work or left the breakpoint set");
        if (debugger.Broken())
            debugger.Resume(Debug::ResumeMode::Continue);
    }

    void CheckSlotReuse(Engine& engine)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const std::uint64_t shared = engine.Evaluate("TestTarget.g_shared").value_or(0);
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        const std::uint64_t quiet = engine.Evaluate("TestTarget.mfGapHigh").value_or(0);
        const std::uint64_t idle = engine.Evaluate("TestTarget.mfTwin").value_or(0);
        CHECK_MSG(shared && work && quiet && idle, "in-process, slot reuse");
        if (!shared || !work || !quiet || !idle)
            return;

        {
            int hot = 0;
            CHECK(debugger.StartWatch(Debug::WatchKind::Writes, shared, 4, hot).Ok());
            Wait(engine, 300);
            debugger.StopWatch(hot);
            int still = 0;
            CHECK(debugger.StartWatch(Debug::WatchKind::Writes, quiet, 1, still).Ok());
            Wait(engine, 300);
            Debug::Watch before;
            CHECK(debugger.CopyWatch(hot, before));
            CHECK_MSG(!before.Entries.empty(), "in-process, slot reuse: the watch on g_shared saw no write");
            Debug::Watch after;
            CHECK(debugger.CopyWatch(still, after));
            CHECK_MSG(after.Entries.empty(), "in-process, slot reuse: the watch on " + Hex(quiet) + ", which nothing writes, has " + std::to_string(after.Entries.size()) +
                " writers, first " + (after.Entries.empty() ? std::string("-") : Hex(after.Entries.front().Instruction)));
            debugger.RemoveWatch(still);
            debugger.RemoveWatch(hot);
        }

        {
            const std::uint64_t flag = engine.Evaluate("TestTarget.g_faultsOn").value_or(0);
            CHECK_MSG(flag, "in-process, slot reuse: no g_faultsOn");
            int read = 0;
            CHECK(debugger.StartWatch(Debug::WatchKind::Accesses, flag, 4, read).Ok());
            Wait(engine, 300);
            debugger.StopWatch(read);
            int written = 0;
            CHECK(debugger.StartWatch(Debug::WatchKind::Writes, flag, 4, written).Ok());
            Wait(engine, 300);
            Debug::Watch before;
            CHECK(debugger.CopyWatch(read, before));
            CHECK_MSG(!before.Entries.empty(), "in-process, slot reuse: the watch on g_faultsOn saw no access");
            Debug::Watch after;
            CHECK(debugger.CopyWatch(written, after));
            CHECK_MSG(after.Entries.empty(), "in-process, slot reuse: the writes watch on g_faultsOn, which only the workers read, has " + std::to_string(after.Entries.size()) +
                " writers, first " + (after.Entries.empty() ? std::string("-") : Hex(after.Entries.front().Instruction)));
            debugger.RemoveWatch(written);
            debugger.RemoveWatch(read);
        }

        {
            const std::uint64_t serial = debugger.Break().Serial;
            const Status hot = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false, Action(engine, "RCX == 1", {}, true));
            CHECK_MSG(hot.Ok(), hot.Text);
            Wait(engine, 300);
            CHECK(debugger.RemoveBreakpoint(work).Ok());
            const Status set = debugger.SetBreakpoint(idle, Debug::BreakpointType::Execute, 1, false);
            CHECK_MSG(set.Ok(), set.Text);
            const bool broke = set && WaitForBreak(engine, serial, 1000);
            const Debug::BreakInfo info = debugger.Break();
            CHECK_MSG(!broke, "in-process, slot reuse: the breakpoint at " + Hex(idle) + ", which nothing calls, stopped at " + Hex(info.Address) + " for " +
                Hex(info.BreakpointAddress));
            CHECK(debugger.RemoveBreakpoint(idle).Ok());
            if (broke)
                debugger.Resume(Debug::ResumeMode::Continue);
            Wait(engine, 100);
        }
        CHECK_MSG(engine.Process()->Alive(), "in-process, slot reuse: the target died");
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

        bool StartTestTarget(PROCESS_INFORMATION& info, const std::filesystem::path& targetPath = TargetBesideTests(), const std::wstring& arguments = {})
        {
            std::wstring command = L"\"" + targetPath.wstring() + L"\"" + arguments;
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

    // A memory-viewer edit over an inserted int3 keeps the breakpoint armed and records the real
    // bytes; removing the breakpoint afterwards leaves the edited bytes, not the old ones.
    void CheckWritesKeepBreakpoints(Engine& engine)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const Target::Process& process = *engine.Process();
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        CHECK(work != 0);
        if (!work)
            return;
        std::uint8_t clean[4]{};
        CHECK(process.ReadExact(work, clean, sizeof(clean)));
        const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false, Action(engine, "1 == 1", {}, false));
        CHECK_MSG(set.Ok(), set.Text);
        if (!set)
            return;
        const Status written = engine.Writes().Write(process, engine.ProcessGeneration(), work, clean, "test");
        CHECK_MSG(written.Ok(), written.Text);
        const MemoryWrite* entry = engine.Writes().Find(engine.Writes().LastId());
        CHECK(entry && entry->Original.size() == sizeof(clean) && std::equal(entry->Original.begin(), entry->Original.end(), clean));
        Wait(engine, 100);
        // ∑ The breakpoint keeps firing, and while a hit is stepped over the old byte is in
        //   place for a moment: one window (hit, action, continue, step, reinsert) can outlast
        //   any number of back-to-back reads, so the int3 is looked for over time.
        const auto armedWithin = [&](int milliseconds)
        {
            const ULONGLONG until = ::GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
            do
            {
                std::uint8_t raw = 0;
                if (process.ReadExact(work, &raw, 1) && raw == 0xCC)
                    return true;
                ::Sleep(1);
            } while (::GetTickCount64() < until);
            return false;
        };
        CHECK_MSG(armedWithin(500), "the int3 is gone after the write");
        CHECK_MSG(HitsAt(engine, work) > 0, "the breakpoint stopped counting after the write");
        // The registers of the thread cannot be read at the step after a hit: the int3 still goes back.
        {
            const std::uint64_t hitsBefore = HitsAt(engine, work);
            engine.Debugger().SimulateFailedStepReads(3);
            Wait(engine, 300);
            engine.Debugger().SimulateFailedStepReads(0);
            CHECK_MSG(armedWithin(500), "the int3 is gone after a step whose registers could not be read");
            Wait(engine, 200);
            CHECK_MSG(HitsAt(engine, work) > hitsBefore + 3, "the breakpoint stopped counting after a step whose registers could not be read");
            CHECK(process.Alive());
        }
        if (entry)
        {
            std::uint8_t seen[4]{};
            debugger.ReadAround(process, work, seen, sizeof(seen));
            const WriteState state = engine.Writes().StateOf(process, *entry);
            CHECK_MSG(state == WriteState::InMemory, "state " + std::to_string(static_cast<int>(state)) + ", memory without int3 " + HexBytes(seen) + ", written " + HexBytes(entry->Written));
        }
        CHECK(debugger.RemoveBreakpoint(work).Ok());
        Wait(engine, 50);
        std::uint8_t after[4]{};
        CHECK(process.ReadExact(work, after, sizeof(after)) && std::equal(std::begin(after), std::end(after), clean));
        CHECK(process.Alive());
    }

    void CheckStepOut(Engine& engine, bool veh)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const Target::Process& process = *engine.Process();
        const char* backend = veh ? "in-process" : "windows";
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        CHECK(work != 0);
        if (!work)
            return;
        const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
        CHECK_MSG(set.Ok(), set.Text);
        const bool broke = set && WaitForBreak(engine, 0, 4000);
        CHECK_MSG(broke, std::string(backend) + ": no break to step from");
        if (!broke)
        {
            debugger.RemoveBreakpoint(work);
            return;
        }
        debugger.RemoveBreakpoint(work);
        const Debug::BreakInfo at = debugger.Break();
        const std::uint64_t returnAddress = process.ReadPointer(at.Registers.Rsp).value_or(0);

        const auto started = std::chrono::steady_clock::now();
        int steps = 0;
        std::uint64_t serial = at.Serial;
        for (; steps < 20; ++steps)
        {
            debugger.Resume(Debug::ResumeMode::StepInto);
            if (!WaitForBreak(engine, serial, 2000))
                break;
            serial = debugger.Break().Serial;
            if (debugger.Break().ThreadId != at.ThreadId)
                break;
            if (debugger.Break().Address == returnAddress)
                break;
        }
        const double perStep = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / std::max(1, steps + 1);
        std::printf("  %s: %.1f ms per single step\n", backend, perStep);
        CHECK_MSG(perStep < 25.0, std::string(backend) + ": a single step takes " + Format("%.1f", perStep) + " ms");
        CHECK_MSG(debugger.Break().ThreadId == at.ThreadId, std::string(backend) + ": stepping moved to another thread");
        CHECK_MSG(debugger.Break().Address == returnAddress, std::string(backend) + ": stepping out of Work by single steps ended at " + Hex(debugger.Break().Address));

        debugger.Resume(Debug::ResumeMode::Continue);
        const Status again = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
        CHECK_MSG(again.Ok(), again.Text);
        const bool second = again && WaitForBreak(engine, serial, 4000);
        debugger.RemoveBreakpoint(work);
        CHECK_MSG(second, std::string(backend) + ": no second break");
        if (second)
        {
            const Debug::BreakInfo inside = debugger.Break();
            const std::uint64_t expected = process.ReadPointer(inside.Registers.Rsp).value_or(0);
            debugger.Resume(Debug::ResumeMode::RunToReturn);
            const bool returned = WaitForBreak(engine, inside.Serial, 3000);
            CHECK_MSG(returned, std::string(backend) + ": step out never stopped");
            if (returned)
            {
                const Debug::BreakInfo out = debugger.Break();
                CHECK_MSG(out.Address == expected, std::string(backend) + ": step out stopped at " + Hex(out.Address) + ", not at the caller " + Hex(expected));
                CHECK_MSG(out.ThreadId == inside.ThreadId, std::string(backend) + ": step out stopped in another thread");
                CHECK_MSG(out.Registers.Rsp > inside.Registers.Rsp, std::string(backend) + ": step out stopped deeper in the stack");
                CHECK_MSG(out.Reason.find("Returned") != std::string::npos, out.Reason);
            }
        }
        if (debugger.Broken())
            debugger.Resume(Debug::ResumeMode::Continue);
        Wait(engine, 100);
        CHECK_MSG(process.Alive() && SharedMoves(engine, 300), std::string(backend) + ": the target does not run after stepping");
    }

    // Issue #17: Step Into on call rax and jmp rax stops where RAX points, Step Over
    // on call rax after the call. A thread of the target runs the code below in a loop.
    void CheckRegisterBranchSteps(Engine& engine)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const Target::Process& process = *engine.Process();
        const auto page = process.Allocate(0x1000, PAGE_EXECUTE_READWRITE);
        CHECK_MSG(page.Ok(), page ? std::string() : page.Error().Text);
        if (!page)
            return;
        const std::uint64_t code = *page;
        const std::uint8_t bytes[] = {
            0x48, 0x8D, 0x05, 0x29, 0x00, 0x00, 0x00,                                           // 00 lea rax,[code+30]
            0xFF, 0xD0,                                                                         // 07 call rax
            0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00,                                           // 09 lea rax,[code+20]
            0xFF, 0xE0,                                                                         // 10 jmp rax
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // 12
            0xF3, 0x90,                                                                         // 20 pause
            0x8B, 0x05, 0xD8, 0x00, 0x00, 0x00,                                                 // 22 mov eax,[code+100]
            0x85, 0xC0,                                                                         // 28 test eax,eax
            0x75, 0x02,                                                                         // 2A jnz 2E
            0xEB, 0xD2,                                                                         // 2C jmp 00
            0xC3, 0x90,                                                                         // 2E ret (the thread ends)
            0xC3,                                                                               // 30 ret
        };
        const Status written = process.Write(code, bytes, sizeof(bytes));
        CHECK_MSG(written.Ok(), written.Text);
        const Status started = written ? process.CreateThread(code, 0, false) : written;
        CHECK_MSG(started.Ok(), started.Text);
        if (!started)
            return;
        const auto step = [&](std::uint64_t from, Debug::ResumeMode mode, std::uint64_t expected, const std::string& what)
            {
                const Status set = debugger.SetBreakpoint(from, Debug::BreakpointType::Execute, 1, false);
                CHECK_MSG(set.Ok(), set.Text);
                const bool broke = set && WaitForBreak(engine, debugger.Break().Serial, 4000);
                debugger.RemoveBreakpoint(from);
                CHECK_MSG(broke && debugger.Break().Address == from, what + ": no break at " + Hex(from));
                if (!broke)
                    return;
                const Debug::BreakInfo at = debugger.Break();
                debugger.Resume(mode);
                const bool stepped = WaitForBreak(engine, at.Serial, 2000);
                CHECK_MSG(stepped && debugger.Break().Address == expected,
                    what + (stepped ? " stopped at " + Hex(debugger.Break().Address) : std::string(" did not stop")) + ", expected " + Hex(expected));
                if (debugger.Broken())
                    debugger.Resume(Debug::ResumeMode::Continue);
            };
        step(code + 0x07, Debug::ResumeMode::StepOver, code + 0x09, "step over call rax");
        step(code + 0x07, Debug::ResumeMode::StepInto, code + 0x30, "step into call rax");
        step(code + 0x10, Debug::ResumeMode::StepInto, code + 0x20, "step into jmp rax");
        const std::uint32_t stop = 1;
        process.Write(code + 0x100, &stop, sizeof(stop));
        Wait(engine, 100);
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

    void CheckSecondAttach(Engine& engine)
    {
        Debug::Debugger& debugger = engine.Debugger();
        const DWORD pid = Raw(engine.Process()->Id());
        const std::uint64_t work = engine.Evaluate("TestTarget.Work").value_or(0);
        const Status first = engine.EnsureDebugger();
        CHECK_MSG(first.Ok(), "second attach: " + first.Text);
        if (!first || !work)
            return;
        for (int attempt = 1; attempt <= 2; ++attempt)
        {
            const auto opened = Target::Process::Open(MakeProcessId(pid));
            CHECK_MSG(opened.Ok(), opened.Error().Text);
            if (!opened)
                return;
            (*opened)->RefreshModules();
            Debug::Debugger second;
            second.SetBackend(Debug::Backend::Veh);
            const Status attached = second.Attach(*opened);
            CHECK_MSG(!attached.Ok(), "a second in-process debugger attached to the same process");
            CHECK_MSG(attached.Text.find("another program") != std::string::npos, "second attach " + std::to_string(attempt) + ": " + attached.Text);
            CHECK(!second.Attached());
        }
        CHECK_MSG(AgentLoaded(pid), "second attach: the running agent was unloaded");
        const Status set = debugger.SetBreakpoint(work, Debug::BreakpointType::Execute, 1, false);
        CHECK_MSG(set.Ok(), set.Text);
        CHECK_MSG(set && WaitForBreak(engine, 0, 4000), "second attach: the first debugger no longer breaks");
        debugger.Detach();
        CHECK_MSG(SharedMoves(engine, 400), "second attach: the target does not run after detaching");
        Wait(engine, 1500);
        CHECK_MSG(engine.Process()->Alive(), "second attach: the target died after detaching");
        CHECK_MSG(!AgentLoaded(pid), "second attach: the agent is still loaded after the first debugger detached");
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
            const auto released = std::chrono::steady_clock::now();
            bool loaded = true;
            for (int tries = 0; tries < 50 && loaded; ++tries)
            {
                Wait(engine, 100);
                loaded = AgentLoaded(target.dwProcessId);
            }
            std::printf("  %s host exit: agent %s %lld ms after the target ran again\n", how, loaded ? "still loaded" : "unloaded",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - released).count()));
            DWORD code = 0;
            ::GetExitCodeProcess(target.hProcess, &code);
            CHECK_MSG(code == STILL_ACTIVE, std::string(how) + " host exit: the target ended with " + Hex(code, 8));
            CHECK_MSG(!loaded, std::string(how) + " host exit: the agent is still loaded 5 s after the target ran again");
            EndTestTarget(target, code);
        }
    }

    void CheckForeignSingleStep(const std::filesystem::path& targetPath)
    {
        PROCESS_INFORMATION target{};
        if (!StartTestTarget(target, targetPath, L" --trap-flag"))
        {
            CHECK_MSG(false, "could not start the test target for the foreign single step check");
            return;
        }
        {
            Settings settings;
            settings.UseInProcessDebugger = true;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            const std::uint64_t steps = opened ? engine.Evaluate("TestTarget.g_trapSteps").value_or(0) : 0;
            const std::uint64_t dr6 = opened ? engine.Evaluate("TestTarget.g_trapDr6").value_or(0) : 0;
            CHECK_MSG(steps && dr6, "foreign single step: no g_trapSteps in the target");
            const auto count = [&]()
                {
                    long value = -1;
                    engine.Process()->ReadExact(steps, &value, sizeof(value));
                    return value;
                };
            const auto lastDr6 = [&]()
                {
                    long long value = -1;
                    engine.Process()->ReadExact(dr6, &value, sizeof(value));
                    return Hex(static_cast<std::uint64_t>(value));
                };
            const long first = steps ? count() : 0;
            Wait(engine, 300);
            const long alone = steps ? count() : 0;
            CHECK_MSG(alone > first, "foreign single step: the target's own single steps did not arrive without a debugger (" + std::to_string(first) + " -> " +
                std::to_string(alone) + ")");
            const Status attached = steps ? engine.EnsureDebugger() : opened;
            CHECK_MSG(attached.Ok(), attached.Text);
            if (steps && attached)
            {
                CHECK(engine.Debugger().UsingVeh());
                Wait(engine, 200);
                const long before = count();
                const std::string seen = lastDr6();
                Wait(engine, 500);
                const long after = count();
                CHECK_MSG(after > before, "foreign single step: the target's own single steps stopped arriving while the in-process debugger was attached (" +
                    std::to_string(before) + " -> " + std::to_string(after) + ", dr6 without a debugger " + seen + ")");
                const std::uint64_t quiet = engine.Evaluate("TestTarget.mfGapHigh").value_or(0);
                int watch = 0;
                CHECK(quiet && engine.Debugger().StartWatch(Debug::WatchKind::Writes, quiet, 1, watch).Ok());
                Wait(engine, 400);
                const long armed = count();
                Wait(engine, 500);
                const long later = count();
                CHECK_MSG(later > armed, "foreign single step: the target's own single steps stopped arriving while a watch ran (" + std::to_string(armed) + " -> " +
                    std::to_string(later) + ", last dr6 " + lastDr6() + ")");
                engine.Debugger().RemoveWatch(watch);
                engine.Debugger().Detach();
                Wait(engine, 400);
            }
        }
        DWORD code = 0;
        ::GetExitCodeProcess(target.hProcess, &code);
        CHECK_MSG(code == STILL_ACTIVE, "foreign single step: the target ended with " + Hex(code, 8));
        EndTestTarget(target, code);
    }

    void CheckSlotKindChange(const std::filesystem::path& targetPath)
    {
        PROCESS_INFORMATION target{};
        if (!StartTestTarget(target, targetPath, L" --write-probe"))
        {
            CHECK_MSG(false, "could not start the test target for the slot kind change check");
            return;
        }
        {
            Settings settings;
            settings.UseInProcessDebugger = true;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            const std::uint64_t probe = opened ? engine.Evaluate("TestTarget.g_writeProbe").value_or(0) : 0;
            const std::uint64_t ask = opened ? engine.Evaluate("TestTarget.g_writeAsk").value_or(0) : 0;
            const std::uint64_t done = opened ? engine.Evaluate("TestTarget.g_writeDone").value_or(0) : 0;
            const std::uint64_t threadAt = opened ? engine.Evaluate("TestTarget.g_writeThread").value_or(0) : 0;
            const bool found = probe && ask && done && threadAt;
            CHECK_MSG(found, "slot kind change: no write probe in the target");
            const Status attached = found ? engine.EnsureDebugger() : Status::Fail(ErrorCode::DebugNotAttached);
            CHECK_MSG(attached.Ok(), attached.Text);
            DWORD threadId = 0;
            if (attached)
                engine.Process()->ReadExact(threadAt, &threadId, sizeof(threadId));
            HANDLE thread = threadId ? ::OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId) : nullptr;
            CHECK_MSG(!attached || thread, "slot kind change: could not open the probe thread");
            Debug::Debugger& debugger = engine.Debugger();
            long serial = 0;
            const auto write = [&]()
                {
                    ++serial;
                    engine.Process()->Write(ask, &serial, sizeof(serial));
                    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                    while (std::chrono::steady_clock::now() < until)
                    {
                        long seen = 0;
                        engine.Process()->ReadExact(done, &seen, sizeof(seen));
                        if (seen == serial)
                            return true;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    return false;
                };
            const auto held = [&]() -> std::uint64_t
                {
                    alignas(16) CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    bool read = false;
                    if (::SuspendThread(thread) != static_cast<DWORD>(-1))
                    {
                        read = ::GetThreadContext(thread, &context) != FALSE;
                        ::ResumeThread(thread);
                    }
                    const DWORD64 slots[4] = { context.Dr0, context.Dr1, context.Dr2, context.Dr3 };
                    for (int slot = 0; read && slot < 4; ++slot)
                    {
                        if (slots[slot] == probe && (context.Dr7 & (1ull << (slot * 2))) != 0)
                            return (context.Dr7 >> (16 + slot * 4)) & 0xF;
                    }
                    return 0xFF;
                };
            const auto count = [&](int id)
                {
                    Debug::Watch copy;
                    std::uint64_t total = 0;
                    if (debugger.CopyWatch(id, copy))
                    {
                        for (const Debug::WatchEntry& entry : copy.Entries)
                            total += entry.Count;
                    }
                    return total;
                };
            struct Change
            {
                Debug::WatchKind Kind;
                int Size;
                const char* Name;
            };
            const Change changes[] = { { Debug::WatchKind::Accesses, 4, "writes to accesses" }, { Debug::WatchKind::Writes, 8, "size 4 to size 8" } };
            for (const Change& change : changes)
            {
                bool stale = false;
                for (int attempt = 0; thread && attempt < 5 && !stale; ++attempt)
                {
                    int before = 0;
                    CHECK(debugger.StartWatch(Debug::WatchKind::Writes, probe, 4, before).Ok());
                    Wait(engine, 300);
                    const bool first = write();
                    debugger.StopWatch(before);
                    int after = 0;
                    CHECK(debugger.StartWatch(change.Kind, probe, change.Size, after).Ok());
                    const std::uint64_t nibble = held();
                    const bool second = write();
                    Wait(engine, 300);
                    const std::uint64_t now = count(after);
                    debugger.RemoveWatch(after);
                    debugger.RemoveWatch(before);
                    CHECK_MSG(first && second, std::string("slot kind change, ") + change.Name + ": the probe thread did not write");
                    stale = nibble == 0xD;
                    if (stale)
                        CHECK_MSG(now == 1, std::string("slot kind change, ") + change.Name + ": the new watch counted " + std::to_string(now) +
                            " hits for the one write of a thread that still held the old registers");
                }
                CHECK_MSG(!thread || stale, std::string("slot kind change, ") + change.Name + ": the probe thread never still held the old registers when the watch changed");
            }
            if (thread)
                ::CloseHandle(thread);
            if (attached)
            {
                debugger.Detach();
                Wait(engine, 400);
            }
        }
        DWORD code = 0;
        ::GetExitCodeProcess(target.hProcess, &code);
        CHECK_MSG(code == STILL_ACTIVE, "slot kind change: the target ended with " + Hex(code, 8));
        EndTestTarget(target, code);
    }

    void CheckTraceUnderLoad(const std::filesystem::path& targetPath)
    {
        ::SetEnvironmentVariableW(L"MANIFOLD_STRESS_THREADS", L"64");
        ::SetEnvironmentVariableW(L"MANIFOLD_NO_CHURN", L"1");
        ::SetEnvironmentVariableW(L"MANIFOLD_STRESS_SLEEP", L"1");
        PROCESS_INFORMATION target{};
        const bool started = StartTestTarget(target, targetPath, L" --stress");
        ::SetEnvironmentVariableW(L"MANIFOLD_STRESS_THREADS", nullptr);
        ::SetEnvironmentVariableW(L"MANIFOLD_NO_CHURN", nullptr);
        ::SetEnvironmentVariableW(L"MANIFOLD_STRESS_SLEEP", nullptr);
        if (!started)
        {
            CHECK_MSG(false, "could not start the test target for the trace under load");
            return;
        }
        {
            Settings settings;
            settings.UseInProcessDebugger = true;
            Engine engine(settings);
            const Status opened = engine.OpenProcess(MakeProcessId(target.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            const std::uint64_t touch = opened ? engine.Evaluate("TestTarget.Touch").value_or(0) : 0;
            const Status attached = touch ? engine.EnsureDebugger() : Status::Fail(ErrorCode::DebugTraceFailed, "no TestTarget.Touch");
            CHECK_MSG(attached.Ok(), attached.Text);
            Debug::Tracer& tracer = engine.Tracer();
            for (int round = 0; attached && round < 25; ++round)
            {
                Debug::TraceRequest request;
                request.Address = touch;
                request.Count = 40;
                request.StepOver = true;
                const Status traced = tracer.Start(engine.Process(), engine.Disassembler(), request);
                CHECK_MSG(traced.Ok(), traced.Text);
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20000);
                while (traced && tracer.Running() && std::chrono::steady_clock::now() < until)
                    Wait(engine, 20);
                if (tracer.Running())
                    tracer.Stop();
                std::vector<std::uint32_t> threads;
                std::string list;
                int index = 0;
                for (const Debug::TraceStep& step : tracer.Steps())
                {
                    if (std::find(threads.begin(), threads.end(), step.ThreadId) == threads.end())
                    {
                        threads.push_back(step.ThreadId);
                        list += " " + std::to_string(step.ThreadId) + " from step " + std::to_string(index) + " at " + (step.Address == touch ? std::string("Touch") : Hex(step.Address));
                    }
                    ++index;
                }
                CHECK_MSG(threads.size() == 1, "trace under load: trace " + std::to_string(round + 1) + " of " + std::to_string(tracer.Steps().size()) + " steps went through " +
                    std::to_string(threads.size()) + " threads:" + list + ", " + tracer.StatusText());
                if (threads.size() != 1)
                    break;
            }
            if (attached)
            {
                if (engine.Debugger().Broken())
                    engine.Debugger().Resume(Debug::ResumeMode::Continue);
                engine.Debugger().Detach();
                Wait(engine, 400);
            }
        }
        DWORD code = 0;
        ::GetExitCodeProcess(target.hProcess, &code);
        CHECK_MSG(code == STILL_ACTIVE, "trace under load: the target ended with " + Hex(code, 8));
        EndTestTarget(target, code);
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
                CheckStepOut(engine, veh);
                if (veh)
                    CheckHostSilence(engine);
                else
                    CheckWritesKeepBreakpoints(engine);
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
                CheckSlotReuse(engine);
                CheckFpuAndExceptions(engine, true);
                CheckStepOut(engine, true);
                CheckTraceOneThread(engine, true);
                CheckRegisterBranchSteps(engine);
                CheckHostSilence(engine);
                engine.Debugger().Detach();
                Wait(engine, 400);
                CHECK(engine.Process()->Alive());
                CheckDetachPaths(engine);
                CheckSecondAttach(engine);
            }
        }
        DWORD code = 0;
        EndTestTarget(info, code);
        CHECK_MSG(code == STILL_ACTIVE, "the target of the in-process debugger ended with " + Hex(code, 8));
        CheckHostExits(targetPath);
        CheckForeignSingleStep(targetPath);
        CheckSlotKindChange(targetPath);
        CheckTraceUnderLoad(targetPath);
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
