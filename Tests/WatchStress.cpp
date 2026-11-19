#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

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
        std::string Option(const std::vector<std::string>& arguments, const std::string& name, const std::string& fallback = {})
        {
            const std::string prefix = "--" + name + "=";
            for (const std::string& argument : arguments)
            {
                if (argument.rfind(prefix, 0) == 0)
                    return argument.substr(prefix.size());
            }
            return fallback;
        }

        bool Flag(const std::vector<std::string>& arguments, const char* name)
        {
            for (const std::string& argument : arguments)
            {
                if (argument == name)
                    return true;
            }
            return false;
        }
    }

    int RunWatchStress(const std::vector<std::string>& arguments)
    {
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::filesystem::path targetPath = std::filesystem::path(self).parent_path() / L"TestTarget.exe";
        if (const std::string given = Option(arguments, "target"); !given.empty())
            targetPath = Utf8ToWide(given);
        const int seconds = static_cast<int>(ParseDecimal(Option(arguments, "seconds", "20")).value_or(20));
        const bool hidden = Flag(arguments, "--hidden");
        const bool hiddenLate = Flag(arguments, "--hidden-late");
        const bool hiddenMain = Flag(arguments, "--hidden-main");
        const bool toggle = Flag(arguments, "--toggle");
        const bool writes = Option(arguments, "kind") == "writes";

        const bool solo = Flag(arguments, "--solo");
        std::wstring command = L"\"" + targetPath.wstring() + (solo ? L"\" --solo" : L"\" --stress") + (hidden ? L" --hidden" : L"") +
                               (hiddenLate ? L" --hidden-late" : L"") + (hiddenMain ? L" --hidden-main" : L"");
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION info{};
        if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
        {
            std::printf("could not start %ls\n", targetPath.c_str());
            return 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        RuntimeLog::SetVerbose(true);
        Settings settings;
        settings.UseInProcessDebugger = Flag(arguments, "--veh");
        Engine engine(settings);
        int result = 0;
        const Status opened = engine.OpenProcess(MakeProcessId(info.dwProcessId));
        if (!opened)
        {
            std::printf("open failed: %s\n", opened.Text.c_str());
            result = 2;
        }
        if (Flag(arguments, "--break"))
        {
            Status started = engine.EnsureDebugger();
            const std::uint64_t touch = opened ? engine.Evaluate("TestTarget.Touch").value_or(0) : 0;
            std::printf("breakpoint on TestTarget.Touch = %s\n", Hex(touch).c_str());
            if (started && touch)
                started = engine.Debugger().SetBreakpoint(touch, Debug::BreakpointType::Execute, 1, true);
            if (!started)
            {
                std::printf("set breakpoint failed: %s\n", started.Text.c_str());
                ::TerminateProcess(info.hProcess, 0);
                return 2;
            }
            if (touch && engine.HasProcess())
            {
                std::uint8_t code[16] = {};
                engine.Process()->ReadExact(touch, code, sizeof(code));
                const std::uint8_t raw0 = code[0];
                engine.Debugger().Unpatch(touch, code, sizeof(code));
                std::printf("byte at breakpoint: memory %02X, shown-after-unpatch %02X (%s)\n", raw0, code[0], code[0] == raw0 ? "unchanged" : "BLANKED");
            }
            auto waitBroken = [&](std::uint64_t afterSerial) -> bool
            {
                for (int i = 0; i < 150; ++i)
                {
                    const Debug::BreakInfo b = engine.Debugger().Break();
                    if (b.Broken && b.Serial != afterSerial)
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                return false;
            };
            if (!waitBroken(0))
            {
                std::printf("never broke\n");
                ::TerminateProcess(info.hProcess, 0);
                return 1;
            }
            Debug::BreakInfo b = engine.Debugger().Break();
            std::printf("BROKE at %s thread %u  rcx=%s rdx=%s rsp=%s  xmm registers %d\n", Hex(b.Address).c_str(), b.ThreadId, Hex(b.Registers.Rcx).c_str(),
                Hex(b.Registers.Rdx).c_str(), Hex(b.Registers.Rsp).c_str(), b.Vectors.Count);
            Debug::VectorRegisters vectors = b.Vectors;
            for (std::size_t i = 0; i < 16; ++i)
                vectors.Xmm[5][i] = static_cast<std::uint8_t>(0x11 + i);
            engine.Debugger().SetVectors(vectors);
            {
                const std::uint64_t serial = b.Serial;
                const std::uint32_t thread = b.ThreadId;
                engine.Debugger().Resume(Debug::ResumeMode::StepInto);
                if (waitBroken(serial))
                {
                    b = engine.Debugger().Break();
                    if (b.ThreadId != thread)
                        std::printf("xmm5 after one step: thread %u broke first, not comparable\n", b.ThreadId);
                    else
                        std::printf("xmm5 after one step: %s\n", b.Vectors.Count > 0 && b.Vectors.Xmm[5] == vectors.Xmm[5] ? "kept (OK)" : "LOST");
                }
            }
            for (int k = 0; k < 3; ++k)
            {
                const std::uint64_t serial = b.Serial;
                engine.Debugger().Resume(Debug::ResumeMode::Continue);
                if (!waitBroken(serial)) { std::printf("continue %d: no rebreak\n", k); break; }
                b = engine.Debugger().Break();
                std::printf("continue %d -> broke again at %s thread %u\n", k, Hex(b.Address).c_str(), b.ThreadId);
            }
            for (int st = 0; st < 6; ++st)
            {
                const std::uint64_t rip0 = b.Address;
                const std::uint64_t serial = b.Serial;
                engine.Debugger().Resume(Debug::ResumeMode::StepInto);
                if (!waitBroken(serial)) { std::printf("step %d: no break\n", st); break; }
                b = engine.Debugger().Break();
                const long long delta = static_cast<long long>(b.Address) - static_cast<long long>(rip0);
                std::printf("step %d: %s -> %s  (delta %+lld)\n", st, Hex(rip0).c_str(), Hex(b.Address).c_str(), delta);
            }
            engine.Debugger().RemoveBreakpoint(touch);
            engine.Debugger().Resume(Debug::ResumeMode::Continue);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            engine.Debugger().Detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            DWORD code = STILL_ACTIVE;
            ::GetExitCodeProcess(info.hProcess, &code);
            std::printf("after break test: target %s\n", code == STILL_ACTIVE ? "alive" : ("ended " + Hex(code, 8)).c_str());
            ::TerminateProcess(info.hProcess, 0);
            ::CloseHandle(info.hProcess);
            ::CloseHandle(info.hThread);
            return code == STILL_ACTIVE ? 0 : 1;
        }
        const std::uint64_t address = opened ? engine.Evaluate("TestTarget.g_stress").value_or(0) : 0;
        int watch = 0;
        if (address)
        {
            std::printf("watching %s at %s with %s, %d s%s\n", writes ? "writes" : "accesses", Hex(address).c_str(), hiddenMain ? "a hidden main thread" : hiddenLate ? "a thread that hides after 3 s" : hidden ? "a hidden thread" : "no hidden thread", seconds,
                toggle ? ", toggling" : "");
            Status started = engine.EnsureDebugger();
            if (started)
                started = engine.Debugger().StartWatch(writes ? Debug::WatchKind::Writes : Debug::WatchKind::Accesses, address, 8, watch);
            if (!started)
            {
                std::printf("watch failed: %s\n", started.Text.c_str());
                result = 2;
            }
        }
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        int tick = 0;
        while (result == 0 && std::chrono::steady_clock::now() < end)
        {
            for (int i = 0; i < 50; ++i)
            {
                engine.Tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ++tick;
            DWORD code = STILL_ACTIVE;
            ::GetExitCodeProcess(info.hProcess, &code);
            Debug::Watch copy;
            const bool have = engine.Debugger().CopyWatch(watch, copy);
            std::printf("%2d s: target %s, %llu hits, %zu instructions, %d hidden%s\n", tick, code == STILL_ACTIVE ? "alive" : ("ended " + Hex(code, 8)).c_str(),
                have ? static_cast<unsigned long long>(copy.Hits) : 0ull, have ? copy.Entries.size() : 0, engine.Debugger().HiddenThreads(),
                engine.Debugger().MainThreadHidden() ? " incl. main" : "");
            std::fflush(stdout);
            if (code != STILL_ACTIVE)
            {
                result = 1;
                break;
            }
            if (toggle && tick % 3 == 0)
            {
                engine.Debugger().RemoveWatch(watch);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                engine.Debugger().StartWatch(writes ? Debug::WatchKind::Writes : Debug::WatchKind::Accesses, address, 8, watch);
            }
        }
        if (result == 0)
        {
            engine.Debugger().RemoveWatch(watch);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            engine.Debugger().Detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            DWORD code = STILL_ACTIVE;
            ::GetExitCodeProcess(info.hProcess, &code);
            std::printf("after detach: target %s\n", code == STILL_ACTIVE ? "alive" : ("ended " + Hex(code, 8)).c_str());
            if (code != STILL_ACTIVE)
                result = 1;
        }
        int shown = 0;
        for (const LogEntry& entry : RuntimeLog::Entries())
        {
            if (entry.Area == "Debugger" && (entry.Message.find("passed to") != std::string::npos || entry.Message.find("ended") != std::string::npos ||
                entry.Message.find("not the debugger") != std::string::npos || entry.Message.find("hides") != std::string::npos || entry.Level >= LogLevel::Warning) && shown++ < 25)
                std::printf("log: %s\n", entry.Message.c_str());
        }
        ::TerminateProcess(info.hProcess, 0);
        ::CloseHandle(info.hProcess);
        ::CloseHandle(info.hThread);
        std::printf("%s\n", result == 0 ? "target survived" : "target died");
        return result;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_MODE("--watch-stress", &RunWatchStress);
}
