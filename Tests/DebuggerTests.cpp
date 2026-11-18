#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Debug/BreakAction.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

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

    int RunDebuggerCheck(const std::vector<std::string>& arguments)
    {
        bool veh = false;
        for (const std::string& argument : arguments)
            veh = veh || argument == "--veh";
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
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
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
                if (veh)
                    CheckHostSilence(engine);
                engine.Debugger().Detach();
                Wait(engine, 400);
                CHECK(engine.Process()->Alive());
            }
        }
        DWORD code = 0;
        ::GetExitCodeProcess(info.hProcess, &code);
        ::TerminateProcess(info.hProcess, 0);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        const int failures = Totals().Failures - before;
        std::printf("debugger check (%s): %d checks, %d failed, target %s\n", veh ? "in-process" : "windows", Totals().Checks, failures,
            code == STILL_ACTIVE ? "was alive" : ("had ended " + Hex(code, 8)).c_str());
        return failures == 0 ? 0 : 1;
    }
}
