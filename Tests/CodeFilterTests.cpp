#include "TestKit.h"

#include "App/CodeFilter.h"
#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/AgentLife.h"
#include "Target/Coverage.h"
#include "Target/CoverageProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "Syscalls/Syscall.h"

namespace Manifold::Tests
{
    namespace
    {
        struct Probe
        {
            long Ready;
            long Trigger;
            long Hammer;
            long Spin;
            long Idle;
            long Action;
            long Wrong;
            long ForeignTrigger;
            long Foreign;
            long Threads;
            long Input;
            long Lock;
            std::int64_t CodeOnly;
        };

        struct Child
        {
            PROCESS_INFORMATION Info{};

            Child() = default;
            Child(const Child&) = delete;
            Child& operator=(const Child&) = delete;

            ~Child() { End(); }

            void End()
            {
                if (!Info.hProcess)
                    return;
                ::TerminateProcess(Info.hProcess, 0);
                ::WaitForSingleObject(Info.hProcess, 3000);
                ::CloseHandle(Info.hProcess);
                ::CloseHandle(Info.hThread);
                Info = {};
            }

            bool Alive() const
            {
                DWORD code = 0;
                return Info.hProcess && ::GetExitCodeProcess(Info.hProcess, &code) && code == STILL_ACTIVE;
            }
        };

        struct Session
        {
            Engine& Core;
            Child& Target;
            std::string Label;
            std::uint64_t ProbeAt = 0;
            std::uint64_t Idle = 0;
            std::uint64_t Action = 0;
            std::uint64_t Leaf = 0;
            std::uint64_t Never = 0;
            std::uint64_t Foreign = 0;
            std::uint64_t Virtual = 0;
            std::map<std::uint64_t, std::uint8_t> Originals;
            Settings* Config = nullptr;
            std::string Module;
        };

        bool Start(const std::filesystem::path& path, const std::wstring& arguments, Child& child, bool pipe = false, HANDLE* output = nullptr)
        {
            STARTUPINFOW startup{ sizeof(startup) };
            HANDLE read = nullptr;
            HANDLE write = nullptr;
            if (pipe)
            {
                SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
                if (!::CreatePipe(&read, &write, &inherit, 0))
                    return false;
                ::SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdOutput = write;
                startup.hStdError = write;
            }
            std::wstring command = L"\"" + path.wstring() + L"\" " + arguments;
            const bool started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, pipe, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.Info) != FALSE;
            if (write)
                ::CloseHandle(write);
            if (output)
                *output = read;
            else if (read)
                ::CloseHandle(read);
            return started;
        }

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void Pause(int milliseconds)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }

        bool WaitFor(int milliseconds, const std::function<bool()>& done)
        {
            const double until = Now() + milliseconds / 1000.0;
            for (;;)
            {
                if (done())
                    return true;
                if (Now() >= until)
                    return false;
                Pause(5);
            }
        }

        std::optional<Probe> ReadProbe(const Session& session)
        {
            Probe probe{};
            SIZE_T got = 0;
            if (!::ReadProcessMemory(session.Target.Info.hProcess, reinterpret_cast<LPCVOID>(session.ProbeAt), &probe, sizeof(probe), &got) || got != sizeof(probe))
                return std::nullopt;
            return probe;
        }

        void SetField(const Session& session, std::size_t offset, long value)
        {
            SIZE_T written = 0;
            ::WriteProcessMemory(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.ProbeAt + offset), &value, sizeof(value), &written);
        }

        long Field(const Session& session, long Probe::* field)
        {
            const auto probe = ReadProbe(session);
            return probe ? (*probe).*field : -1;
        }

        // The bytes as the process has them, past anything in the program that
        // could show the originals instead.
        int RawByte(const Session& session, std::uint64_t address)
        {
            std::uint8_t value = 0;
            SIZE_T got = 0;
            if (!::ReadProcessMemory(session.Target.Info.hProcess, reinterpret_cast<LPCVOID>(address), &value, 1, &got) || got != 1)
                return -1;
            return value;
        }

        std::optional<Target::CoverEntry> EntryAt(Session& session, std::uint64_t address)
        {
            const auto entries = session.Core.CodeCoverage().Entries();
            if (!entries)
                return std::nullopt;
            for (const Target::CoverEntry& entry : *entries)
            {
                if (entry.Address == address)
                    return entry;
            }
            return std::nullopt;
        }

        bool StateIs(Session& session, std::uint64_t address, Target::CoverState state)
        {
            const auto entry = EntryAt(session, address);
            return entry && entry->State == state;
        }

        bool AllOriginal(Session& session)
        {
            for (const auto& [address, original] : session.Originals)
            {
                if (RawByte(session, address) != original)
                    return false;
            }
            return true;
        }

        bool AgentLoaded(Session& session)
        {
            session.Core.Process()->RefreshModules();
            return session.Core.Process()->FindModule(session.Core.CodeCoverage().AgentName()).has_value();
        }

        bool WaitAgentGone(Session& session, int milliseconds)
        {
            return WaitFor(milliseconds, [&] { return !AgentLoaded(session); });
        }

        void Trigger(Session& session)
        {
            SetField(session, offsetof(Probe, Trigger), Field(session, &Probe::Trigger) + 1);
        }

        void CheckRound(Session& session)
        {
            Target::Coverage& coverage = session.Core.CodeCoverage();
            const std::uint64_t all[] = { session.Never, session.Action, session.Idle, session.Leaf };
            const Status armed = coverage.Arm(all);
            CHECK_MSG(armed.Ok(), session.Label + ": " + armed.Text);
            if (!armed)
                return;
            const auto counts = coverage.Counts();
            CHECK_MSG(counts && counts->Armed == 4 && counts->Skipped == 0 && counts->Active, session.Label + ": not all four functions were armed");
            CHECK_MSG(counts && counts->Watching, session.Label + ": the library does not watch the program");
            CHECK_MSG(WaitFor(2000, [&] { return StateIs(session, session.Idle, Target::CoverState::Hit); }), session.Label + ": the function that runs all the time was not hit");
            CHECK_MSG(RawByte(session, session.Idle) == session.Originals[session.Idle], session.Label + ": the hit function did not get its byte back");
            CHECK_MSG(StateIs(session, session.Action, Target::CoverState::Armed) && RawByte(session, session.Action) == Target::Cover::kInt3,
                session.Label + ": the action function is not armed before the action");
            CHECK_MSG(StateIs(session, session.Never, Target::CoverState::Armed), session.Label + ": the function that never runs is not armed");
            const auto hit = EntryAt(session, session.Idle);
            CHECK_MSG(hit && hit->ThreadId != 0 && hit->Original == session.Originals[session.Idle], session.Label + ": the hit entry has no thread or the wrong original byte");

            const long before = Field(session, &Probe::Action);
            SetField(session, offsetof(Probe, Spin), 1);
            Pause(50);
            Trigger(session);
            const bool ran = WaitFor(3000, [&] { return Field(session, &Probe::Action) >= before + 8; });
            SetField(session, offsetof(Probe, Spin), 0);
            CHECK_MSG(ran, session.Label + Format(": eight threads at one int3 did not all run on (%ld of 8)", Field(session, &Probe::Action) - before));
            CHECK_MSG(StateIs(session, session.Action, Target::CoverState::Hit) && StateIs(session, session.Leaf, Target::CoverState::Hit),
                session.Label + ": the action and the function it calls were not both hit");
            CHECK_MSG(StateIs(session, session.Never, Target::CoverState::Armed), session.Label + ": the function that never runs was hit");
            CHECK_MSG(Field(session, &Probe::Wrong) == 0, session.Label + ": a function gave a wrong result after its int3");

            const long foreign = Field(session, &Probe::Foreign);
            SetField(session, offsetof(Probe, ForeignTrigger), Field(session, &Probe::ForeignTrigger) + 1);
            CHECK_MSG(WaitFor(2000, [&] { return Field(session, &Probe::Foreign) == foreign + 1; }), session.Label + ": the program's own int3 did not reach its own handler");

            const auto hits = coverage.Hits();
            CHECK_MSG(hits && hits->size() == 3, session.Label + Format(": %zu hits, 3 expected", hits ? hits->size() : 0));
            const Status disarmed = coverage.Disarm();
            CHECK_MSG(disarmed.Ok(), session.Label + ": " + disarmed.Text);
            CHECK_MSG(StateIs(session, session.Never, Target::CoverState::Released), session.Label + ": the function that never ran was not released");
            CHECK_MSG(AllOriginal(session), session.Label + ": a byte differs after the round");
            const auto after = coverage.Counts();
            CHECK_MSG(after && !after->Active && after->Armed == 0, session.Label + ": the library still counts as armed");
        }

        void CheckNextRound(Session& session)
        {
            Target::Coverage& coverage = session.Core.CodeCoverage();
            const std::uint64_t action[] = { session.Action };
            CHECK(coverage.Arm(action).Ok());
            Trigger(session);
            CHECK_MSG(WaitFor(2000, [&] { return StateIs(session, session.Action, Target::CoverState::Hit); }), session.Label + ": the action was not hit in the second round");
            CHECK(coverage.Arm(action).Ok());
            CHECK_MSG(StateIs(session, session.Action, Target::CoverState::Armed) && RawByte(session, session.Action) == Target::Cover::kInt3, session.Label + ": a new round did not arm the action again");
            const std::uint64_t never[] = { session.Never };
            CHECK(coverage.Arm(never).Ok());
            CHECK_MSG(RawByte(session, session.Action) == session.Originals[session.Action], session.Label + ": a new round did not take back the int3 of the round before");
            CHECK(coverage.Disarm().Ok());
            CHECK_MSG(AllOriginal(session), session.Label + ": a byte differs after the second round");
        }

        // Rounds that start and end while eight threads run through the armed
        // functions all the time: threads reach the handler after the byte is
        // back, or while another thread restores it.
        void CheckStress(Session& session)
        {
            Target::Coverage& coverage = session.Core.CodeCoverage();
            const std::uint64_t both[] = { session.Action, session.Leaf };
            SetField(session, offsetof(Probe, Hammer), 1);
            const long before = Field(session, &Probe::Action);
            int failed = 0;
            const double started = Now();
            for (int round = 0; round < 300; ++round)
            {
                if (!coverage.Arm(both))
                    ++failed;
                if (round % 3 == 0 && !coverage.Disarm())
                    ++failed;
            }
            const double seconds = Now() - started;
            CHECK(coverage.Disarm().Ok());
            SetField(session, offsetof(Probe, Hammer), 0);
            // the target's counter is a 32-bit long that wraps after a few seconds on a fast PC
            const unsigned long calls = static_cast<unsigned long>(Field(session, &Probe::Action)) - static_cast<unsigned long>(before);
            const auto counts = coverage.Counts();
            std::printf("  %s: 300 rounds in %.2f s while the targets ran %lu times, %llu hits, %llu stale traps\n", session.Label.c_str(), seconds, calls,
                counts ? static_cast<unsigned long long>(counts->Hits) : 0ull, counts ? static_cast<unsigned long long>(counts->Stale) : 0ull);
            CHECK_MSG(failed == 0, session.Label + Format(": %d rounds failed", failed));
            CHECK_MSG(calls > 300, session.Label + ": the threads did not run during the rounds");
            CHECK_MSG(Field(session, &Probe::Wrong) == 0, session.Label + ": a result came out wrong during the rounds");
            CHECK_MSG(session.Target.Alive(), session.Label + ": the target died during the rounds");
            CHECK_MSG(AllOriginal(session), session.Label + ": a byte differs after the rounds");
        }

        void CheckSkips(Session& session)
        {
            Target::Coverage& coverage = session.Core.CodeCoverage();
            const Target::Process& process = *session.Core.Process();
            const std::uint64_t system = process.FindExport("ntdll.dll", "NtQueryInformationProcess");
            const std::uint64_t kernel = process.FindExport("kernelbase.dll", "VirtualProtect");
            const std::uint64_t agent = process.FindExport(coverage.AgentName(), "ManifoldCoverCall");
            const std::uint64_t data = session.ProbeAt;
            CHECK(system && kernel && agent);
            const int systemByte = RawByte(session, system);
            const int kernelByte = RawByte(session, kernel);
            const std::uint64_t all[] = { system, kernel, agent, data, session.Never };
            CHECK(coverage.Arm(all).Ok());
            const auto skip = [&](std::uint64_t address) { const auto entry = EntryAt(session, address); return entry ? entry->Skip : Target::CoverSkip::None; };
            CHECK_MSG(skip(system) == Target::CoverSkip::System && skip(kernel) == Target::CoverSkip::System && skip(agent) == Target::CoverSkip::System,
                session.Label + ": system or agent code was not skipped");
            CHECK_MSG(skip(data) == Target::CoverSkip::NotCode, session.Label + ": data was not skipped as not executable");
            CHECK_MSG(RawByte(session, system) == systemByte && RawByte(session, kernel) == kernelByte, session.Label + ": a system byte changed");
            const auto counts = coverage.Counts();
            CHECK_MSG(counts && counts->Armed == 1 && counts->Skipped == 4, session.Label + ": the armed and skipped counts are wrong");
            CHECK(coverage.Disarm().Ok());
            CHECK(AllOriginal(session));
            CHECK(std::string(Target::CoverSkipText(Target::CoverSkip::NotCode)) == "not executable memory");
        }

        // With "break on all exceptions" the in-process debugger must not stop
        // at the filter's int3s, whichever handler runs first.
        void CheckDebugger(Session& session)
        {
            Engine& engine = session.Core;
            Target::Coverage& coverage = engine.CodeCoverage();
            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok() && engine.Debugger().UsingVeh(), session.Label + ": " + attached.Text);
            if (!attached)
                return;
            engine.Debugger().SetExceptionBreak(Debug::ExceptionBreak::Always, 0, 0);
            const std::uint64_t action[] = { session.Action, session.Leaf };
            CHECK(coverage.Arm(action).Ok());
            Trigger(session);
            CHECK_MSG(WaitFor(2000, [&] { return StateIs(session, session.Action, Target::CoverState::Hit) && StateIs(session, session.Leaf, Target::CoverState::Hit); }),
                session.Label + ": the action was not hit with the debugger attached");
            Pause(200);
            CHECK_MSG(!engine.Debugger().Broken(), session.Label + ": the debugger stopped at an int3 of the code filter");
            if (engine.Debugger().Broken())
                engine.Debugger().Resume(Debug::ResumeMode::Continue);
            CHECK(coverage.Disarm().Ok());
            engine.Debugger().SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
            engine.Debugger().Detach();
            CHECK(AllOriginal(session));
            CHECK(session.Target.Alive());
        }

        // The in-process debugger stops at the program's own int3 (still in its code) and the user
        // goes on with "handled": the thread must go past the int3, not trap on it again forever,
        // and the program's own handler must not see it.
        void CheckHandledInt3(Session& session)
        {
            Engine& engine = session.Core;
            Debug::Debugger& debugger = engine.Debugger();
            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok() && debugger.UsingVeh(), session.Label + ": " + attached.Text);
            if (!attached)
                return;
            const auto brokeAfter = [&](std::uint64_t serial)
            {
                return WaitFor(3000, [&] { engine.Tick(); const Debug::BreakInfo info = debugger.Break(); return info.Broken && info.Serial != serial; });
            };
            debugger.SetExceptionBreak(Debug::ExceptionBreak::InRegion, session.Foreign, session.Foreign + 16);
            const long foreign = Field(session, &Probe::Foreign);
            for (int round = 0; round < 2; ++round)
            {
                const std::uint64_t serial = debugger.Break().Serial;
                SetField(session, offsetof(Probe, ForeignTrigger), Field(session, &Probe::ForeignTrigger) + 1);
                const bool broke = brokeAfter(serial);
                CHECK_MSG(broke, session.Label + ": the debugger did not stop at the program's own int3");
                if (!broke)
                    break;
                const Debug::BreakInfo info = debugger.Break();
                CHECK_MSG(info.Exception.Raised && info.Exception.Code == 0x80000003 && info.Address >= session.Foreign && info.Address < session.Foreign + 16,
                    session.Label + ": stopped for " + info.Reason);
                debugger.ResumeHandled();
                const bool again = brokeAfter(info.Serial);
                CHECK_MSG(!again, session.Label + ": going on handled from an int3 that is still in the code trapped on it again");
                if (again)
                    debugger.Resume(Debug::ResumeMode::Continue);
            }
            CHECK_MSG(Field(session, &Probe::Foreign) == foreign, session.Label + ": the program's handler saw an int3 the user had handled");
            debugger.SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
            if (debugger.Broken())
                debugger.Resume(Debug::ResumeMode::Continue);
            const long before = Field(session, &Probe::Foreign);
            SetField(session, offsetof(Probe, ForeignTrigger), Field(session, &Probe::ForeignTrigger) + 1);
            CHECK_MSG(WaitFor(2000, [&] { return Field(session, &Probe::Foreign) == before + 1; }), session.Label + ": the program's own int3 does not reach its handler after the debugger");
            debugger.Detach();
            CHECK(session.Target.Alive());
        }

        // Someone else writes over an armed int3 without going through Manifold (an overlay
        // hooking the function, a write the release before it timed out on): ending the round
        // must leave their byte, not put the original back into the middle of their code.
        void CheckForeignWrite(Session& session)
        {
            Target::Coverage& coverage = session.Core.CodeCoverage();
            const std::uint64_t never[] = { session.Never };
            CHECK(coverage.Arm(never).Ok() && RawByte(session, session.Never) == Target::Cover::kInt3);
            const std::uint8_t theirs = 0xE9;
            SIZE_T written = 0;
            DWORD old = 0;
            ::VirtualProtectEx(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.Never), 1, PAGE_EXECUTE_READWRITE, &old);
            ::WriteProcessMemory(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.Never), &theirs, 1, &written);
            ::VirtualProtectEx(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.Never), 1, old, &old);
            CHECK(coverage.Disarm().Ok());
            CHECK_MSG(RawByte(session, session.Never) == theirs, session.Label + ": ending the round wrote the original byte over someone else's write");
            const std::uint8_t original = session.Originals[session.Never];
            ::VirtualProtectEx(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.Never), 1, PAGE_EXECUTE_READWRITE, &old);
            ::WriteProcessMemory(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.Never), &original, 1, &written);
            ::VirtualProtectEx(session.Target.Info.hProcess, reinterpret_cast<LPVOID>(session.Never), 1, old, &old);
            CHECK(AllOriginal(session));
        }

        void CheckRelease(Session& session)
        {
            Target::Coverage& coverage = session.Core.CodeCoverage();
            const std::uint64_t never[] = { session.Never };
            CHECK(coverage.Arm(never).Ok());
            CHECK(RawByte(session, session.Never) == Target::Cover::kInt3);
            const Status released = coverage.Release();
            CHECK_MSG(released.Ok(), session.Label + ": " + released.Text);
            CHECK_MSG(!coverage.Installed() && AllOriginal(session), session.Label + ": release left a byte or the library behind");
            CHECK_MSG(WaitAgentGone(session, 5000), session.Label + ": the agent library stayed loaded after the release");
            CHECK(session.Target.Alive());
        }

        // A second program arms the target and is killed: the library sees
        // its handle signalled and takes every int3 back.
        void CheckHostLoss(Session& session, const std::filesystem::path& tests)
        {
            Engine& engine = session.Core;
            const DWORD pid = session.Target.Info.dwProcessId;
            Child host;
            HANDLE output = nullptr;
            CHECK(Start(tests, L"--codefilter-host " + std::to_wstring(pid) + L" " + std::to_wstring(session.Never) + L" " + std::to_wstring(session.Action), host, true, &output));
            std::string text;
            const bool ready = WaitFor(8000, [&]
                {
                    DWORD available = 0;
                    if (output && ::PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available)
                    {
                        char buffer[256] = {};
                        DWORD got = 0;
                        if (::ReadFile(output, buffer, sizeof(buffer) - 1, &got, nullptr))
                            text.append(buffer, got);
                    }
                    return text.find("ready") != std::string::npos || !host.Alive();
                });
            if (output)
                ::CloseHandle(output);
            CHECK_MSG(ready && host.Alive(), session.Label + ": the second program did not arm the target: " + text);
            CHECK_MSG(RawByte(session, session.Never) == Target::Cover::kInt3 && RawByte(session, session.Action) == Target::Cover::kInt3, session.Label + ": the second program's int3s are not there");
            host.End();
            CHECK_MSG(WaitFor(3000, [&] { return AllOriginal(session); }), session.Label + ": the int3s stayed after the program that set them was killed");
            Trigger(session);
            Pause(300);
            CHECK_MSG(session.Target.Alive() && Field(session, &Probe::Wrong) == 0, session.Label + ": the target did not run on after the program was killed");
            // ∑ With its handler out and no other part of the agent in use, the library unloads
            //   by itself (Agent/Life.cpp); there is nothing left to take over.
            CHECK_MSG(WaitAgentGone(session, 5000), session.Label + ": the library stayed after the program that loaded it was killed");

            engine.CloseProcess();
            const Status opened = engine.OpenProcess(MakeProcessId(pid));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            Target::Coverage& coverage = engine.CodeCoverage();
            CHECK(!coverage.Installed());
            const std::uint64_t never[] = { session.Never };
            CHECK(coverage.Arm(never).Ok() && RawByte(session, session.Never) == Target::Cover::kInt3);
            const auto counts = coverage.Counts();
            CHECK_MSG(counts && counts->Watching && !counts->HostLost && counts->Active, session.Label + ": the library loaded anew does not watch this program");
            CHECK(coverage.Release().Ok() && AllOriginal(session));
            CHECK_MSG(WaitAgentGone(session, 5000), session.Label + ": the agent library stayed loaded after the release");
            CHECK(session.Target.Alive());
        }

        // Every reader sees the original bytes while the int3s are in; a write
        // over one first takes it back, so ending the round keeps the write.
        void CheckShadow(Session& session)
        {
            Engine& engine = session.Core;
            Target::Coverage& coverage = engine.CodeCoverage();
            const Target::Process& process = *engine.Process();
            const std::uint64_t both[] = { session.Never, session.Action };
            const Disasm::Instruction before = engine.Disassembler().Read(process, session.Never);
            CHECK(coverage.Arm(both).Ok());
            CHECK(RawByte(session, session.Never) == Target::Cover::kInt3);
            std::uint8_t shown[4] = {};
            CHECK_MSG(process.ReadExact(session.Never, shown, sizeof(shown)) && shown[0] == session.Originals[session.Never], session.Label + ": a read shows the int3");
            std::uint8_t raw = 0;
            CHECK_MSG(process.ReadRaw(session.Never, &raw, 1) && raw == Target::Cover::kInt3, session.Label + ": the raw read does not show the int3");
            std::vector<std::uint8_t> span(64);
            CHECK(process.Read(session.Never - 16, span.data(), span.size()) == span.size() && span[16] == session.Originals[session.Never]);
            const Disasm::Instruction during = engine.Disassembler().Read(process, session.Never);
            CHECK_MSG(during.Valid && during.Text() == before.Text(), session.Label + ": the disassembler sees " + during.Text() + " instead of " + before.Text());
            CHECK(coverage.Owns(session.Never) && !coverage.Owns(session.Never + 1));

            const std::uint8_t patch = 0xC3;
            const Status written = process.Write(session.Never, &patch, 1);
            CHECK_MSG(written.Ok(), session.Label + ": " + written.Text);
            CHECK_MSG(StateIs(session, session.Never, Target::CoverState::Released) && RawByte(session, session.Never) == patch,
                session.Label + ": the write over an armed byte did not release it first");
            CHECK_MSG(StateIs(session, session.Action, Target::CoverState::Armed), session.Label + ": the write released more than its own bytes");
            CHECK(!coverage.Owns(session.Never));
            CHECK(coverage.Disarm().Ok());
            CHECK_MSG(RawByte(session, session.Never) == patch, session.Label + ": ending the round undid a write");
            CHECK(process.Write(session.Never, &session.Originals[session.Never], 1).Ok());
            CHECK(AllOriginal(session));
        }

        // The Windows debugger sees every int3 first and passes the filter's
        // on to the process, also with "break on all exceptions".
        void CheckWindowsDebugger(Session& session)
        {
            Engine& engine = session.Core;
            Target::Coverage& coverage = engine.CodeCoverage();
            session.Config->UseInProcessDebugger = false;
            const Status attached = engine.EnsureDebugger();
            session.Config->UseInProcessDebugger = true;
            CHECK_MSG(attached.Ok() && !engine.Debugger().UsingVeh(), session.Label + ": " + attached.Text);
            if (!attached)
                return;
            engine.Debugger().SetExceptionBreak(Debug::ExceptionBreak::Always, 0, 0);
            const std::uint64_t action[] = { session.Action, session.Leaf, session.Never };
            CHECK(coverage.Arm(action).Ok());
            const long before = Field(session, &Probe::Action);
            Trigger(session);
            CHECK_MSG(WaitFor(3000, [&] { return StateIs(session, session.Action, Target::CoverState::Hit) && StateIs(session, session.Leaf, Target::CoverState::Hit); }),
                session.Label + ": the action was not hit under the Windows debugger");
            CHECK(WaitFor(3000, [&] { return Field(session, &Probe::Action) >= before + 8; }));
            Pause(200);
            CHECK_MSG(!engine.Debugger().Broken(), session.Label + ": the Windows debugger stopped at an int3 of the code filter");
            if (engine.Debugger().Broken())
                engine.Debugger().Resume(Debug::ResumeMode::Continue);
            CHECK(coverage.Disarm().Ok());
            engine.Debugger().SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
            engine.Debugger().Detach();
            CHECK(Field(session, &Probe::Wrong) == 0);
            CHECK(AllOriginal(session));
            CHECK(session.Target.Alive());
        }

        bool Has(const std::vector<CodeCandidate>& candidates, std::uint64_t address)
        {
            return std::any_of(candidates.begin(), candidates.end(), [&](const CodeCandidate& candidate) { return candidate.Address == address; });
        }

        void CheckFilter(Session& session)
        {
            Engine& engine = session.Core;
            CodeFilter& filter = engine.Filter();
            CHECK(filter.Start(FilterRound::FilterOut).Code == ErrorCode::CodeFilterFailed);
            // Switch tables in the code section, collected as the owner had it in Isaac (no call
            // targets, so no function bounds on 32-bit): data, never candidates.
            {
                CodeFilterOptions tables;
                tables.Modules = { session.Module };
                tables.Calls = false;
                const Result<CodeFilterReport> prepared = filter.Prepare(tables);
                CHECK_MSG(prepared.Ok(), session.Label + ": " + (prepared ? std::string() : prepared.Error().Text));
                const std::vector<CodeCandidate> all = filter.Candidates();
                const Target::Process& process = *engine.Process();
                const auto indices = process.ReadPointer(process.FindExport(session.Module, "g_coverIndexTablePointer"));
                const auto region = indices ? process.RegionAt(*indices) : std::nullopt;
                CHECK_MSG(region && region->Executable(), session.Label + ": the test target's index table is not in its code section");
                CHECK_MSG(indices && !Has(all, *indices), session.Label + ": a case index table in the code section is a candidate: " + (indices ? Hex(*indices) : std::string()));
                // the bytes behind CoverReadCodeOnly's absolute address: only code names them
                const std::uint64_t reader = process.FindExport(session.Module, "CoverReadCodeOnly");
                const auto codeOnly = static_cast<std::uint64_t>(ReadProbe(session).value_or(Probe{}).CodeOnly);
                CHECK_MSG(reader && codeOnly && !Has(all, codeOnly), session.Label + ": bytes that only an instruction names became a candidate: " + Hex(codeOnly));
            }
            CodeFilterOptions options;
            options.Modules = { session.Module };
            options.Calls = true;
            const Result<CodeFilterReport> report = filter.Prepare(options);
            CHECK_MSG(report.Ok(), session.Label + ": " + report.Error().Text);
            if (!report)
                return;
            const bool is64Bit = engine.Process()->Is64Bit();
            const CodeModuleCount& count = report->Modules.front();
            CHECK(report->Modules.size() == 1 && report->Total == filter.Remaining() && report->Total > 50);
            CHECK_MSG(is64Bit ? count.Unwind > 50 : count.Unwind == 0, session.Label + ": unwind entries miscounted");
            CHECK(count.Exports >= 5 && count.Calls > 20 && count.Pointers > 5);
            std::printf("  %s: %zu functions (%zu unwind, %zu exports, %zu calls, %zu pointers, %zu rejected) in %.2f s\n", session.Label.c_str(), report->Total, count.Unwind,
                count.Exports, count.Calls, count.Pointers, count.Rejected, report->Seconds);
            const std::vector<CodeCandidate> all = filter.Candidates();
            for (const std::uint64_t address : { session.Idle, session.Action, session.Leaf, session.Never })
                CHECK_MSG(Has(all, address), session.Label + ": a test function is not a candidate: " + Hex(address));
            CHECK(!Has(all, session.ProbeAt));
            const auto area = std::find_if(all.begin(), all.end(), [&](const CodeCandidate& candidate) { return candidate.Address == session.Virtual; });
            CHECK_MSG(area != all.end() && (area->Sources & SourcePointer), session.Label + ": the virtual leaf method is not a candidate from its vtable");
            const auto leaf = std::find_if(all.begin(), all.end(), [&](const CodeCandidate& candidate) { return candidate.Address == session.Leaf; });
            CHECK_MSG(leaf != all.end() && (leaf->Sources & SourceExport) && (leaf->Sources & SourceCall),
                session.Label + ": the leaf's sources are " + (leaf != all.end() ? CodeSourceText(leaf->Sources) : std::string("none")));

            for (int round = 0; round < 2; ++round)
            {
                const Status started = filter.Start(FilterRound::FilterOut);
                CHECK_MSG(started.Ok(), session.Label + ": " + started.Text);
                CHECK(filter.Running() && filter.RunningKind() == FilterRound::FilterOut);
                CHECK(filter.Start(FilterRound::KeepOnly).Code == ErrorCode::CodeFilterFailed);
                Pause(1200);
                if (round == 0)
                    CHECK_MSG(filter.LiveHits() > 0, session.Label + ": nothing ran during the first round");
                CHECK(filter.Stop().Ok());
            }
            const std::vector<CodeCandidate> quiet = filter.Candidates();
            CHECK_MSG(!Has(quiet, session.Idle) && Has(quiet, session.Action) && Has(quiet, session.Never), session.Label + ": filtering out what runs on its own went wrong");
            const std::size_t beforeKeep = filter.Remaining();

            const long calls = Field(session, &Probe::Action);
            CHECK(filter.Start(FilterRound::KeepOnly).Ok());
            Trigger(session);
            CHECK(WaitFor(2000, [&] { return Field(session, &Probe::Action) >= calls + 8; }));
            CHECK(filter.Stop().Ok());
            const std::vector<CodeCandidate> kept = filter.Candidates();
            std::printf("  %s: %zu after two Filter Out rounds, %zu after Keep Only\n", session.Label.c_str(), beforeKeep, kept.size());
            CHECK_MSG(Has(kept, session.Action) && Has(kept, session.Leaf) && !Has(kept, session.Never) && !Has(kept, session.Idle), session.Label + ": Keep Only did not keep the action");
            CHECK_MSG(Has(kept, session.Virtual), session.Label + ": Keep Only did not keep the virtual method the action calls");
            CHECK_MSG(kept.size() <= 12, session.Label + Format(": %zu functions are left, the action should stand out", kept.size()));
            const std::vector<RoundRecord> history = filter.History();
            CHECK(history.size() == 3 && history.back().Kind == FilterRound::KeepOnly && history.back().After == kept.size() && history.back().Before == beforeKeep);
            CHECK(filter.Undo() && filter.Remaining() == beforeKeep && filter.History().size() == 2);
            CHECK(filter.Start(FilterRound::KeepOnly).Ok());
            CHECK(filter.Cancel().Ok() && !filter.Running() && filter.Remaining() == beforeKeep);

            const Result<std::size_t> tooMany = filter.Refine(4);
            CHECK_MSG(!tooMany && tooMany.Error().Code == ErrorCode::CodeFilterFailed, session.Label + ": refine took more functions than allowed");
            const long again = Field(session, &Probe::Action);
            CHECK(filter.Start(FilterRound::KeepOnly).Ok());
            Trigger(session);
            CHECK(WaitFor(2000, [&] { return Field(session, &Probe::Action) >= again + 8; }));
            CHECK(filter.Stop().Ok());
            const std::size_t functions = filter.Remaining();
            CHECK(filter.Start(FilterRound::Refine).Code == ErrorCode::CodeFilterFailed);
            const Result<std::size_t> refined = filter.Refine();
            CHECK_MSG(refined.Ok(), session.Label + ": " + (refined ? std::string() : refined.Error().Text));
            const std::vector<CodeCandidate> blocks = filter.Candidates();
            std::printf("  %s: %zu functions refined into %zu blocks\n", session.Label.c_str(), functions, blocks.size());
            CHECK_MSG(refined && *refined == blocks.size() && blocks.size() > functions, session.Label + ": the action's branch gave no block of its own");
            CHECK_MSG(std::all_of(blocks.begin(), blocks.end(), [](const CodeCandidate& candidate) { return candidate.Function != 0; }), session.Label + ": a block has no function");
            CHECK(Has(blocks, session.Action) && Has(blocks, session.Leaf));
            CHECK(filter.History().back().Kind == FilterRound::Refine);
            const Result<std::size_t> twice = filter.Refine();
            CHECK(!twice && twice.Error().Code == ErrorCode::CodeFilterFailed);
            const long blockRound = Field(session, &Probe::Action);
            CHECK(filter.Start(FilterRound::KeepOnly).Ok());
            Trigger(session);
            CHECK(WaitFor(2000, [&] { return Field(session, &Probe::Action) >= blockRound + 8; }));
            CHECK(filter.Stop().Ok());
            const std::vector<CodeCandidate> ranBlocks = filter.Candidates();
            CHECK_MSG(Has(ranBlocks, session.Action) && Has(ranBlocks, session.Leaf), session.Label + ": the blocks of the action did not run");
            CHECK_MSG(ranBlocks.size() < blocks.size(), session.Label + ": the block of the branch that is never taken was kept");
            CHECK(filter.Undo() && filter.Undo() && filter.Remaining() == functions);
            CHECK(AllOriginal(session));

            filter.Reset();
            CHECK(!filter.Prepared() && filter.Remaining() == 0);
            CHECK(AllOriginal(session));
            CHECK(Field(session, &Probe::Wrong) == 0 && session.Target.Alive());
        }

        // Without the pointer source the virtual leaf method is out of reach:
        // no call names it, it exports nothing and (64-bit) has no unwind entry.
        void CheckWithoutPointers(Session& session)
        {
            CodeFilter& filter = session.Core.Filter();
            CodeFilterOptions options;
            options.Modules = { session.Module };
            options.Calls = true;
            options.Pointers = false;
            const Result<CodeFilterReport> report = filter.Prepare(options);
            CHECK(report.Ok() && report->Modules.front().Pointers == 0);
            CHECK_MSG(!Has(filter.Candidates(), session.Virtual), session.Label + ": the virtual method was found without the pointer source");
            filter.Reset();
        }

        // An address list as a user writes it: a BOM, comments, empty lines,
        // hex with and without 0x, module+offset, a symbol, a second column,
        // a duplicate and lines that are not code.
        void CheckImport(Session& session)
        {
            Engine& engine = session.Core;
            CodeFilter& filter = engine.Filter();
            const std::string text = "\xEF\xBB\xBF# found with the code filter\r\n\r\n" + engine.Symbols().ModuleOffset(session.Action) + "\r\n" + "0x" + Hex(session.Leaf) + "\r\n" +
                engine.Describe(session.Never) + "\r\n" + Hex(session.Idle) + "\tthe idle loop\r\n" + "; a note\n" + "// another note\n" + Hex(session.Leaf) + "\n" +
                "not an address at all\n" + Hex(session.ProbeAt) + "\n0\n";
            const Result<CodeImportReport> report = filter.Import(text);
            CHECK_MSG(report.Ok(), session.Label + " import: " + (report ? std::string() : report.Error().Text));
            if (!report)
                return;
            std::printf("  %s: imported %zu of %zu lines (%zu duplicates, %zu rejected) in %.3f s\n", session.Label.c_str(), report->Imported, report->Lines, report->Duplicates,
                report->Rejected, report->Seconds);
            CHECK_MSG(report->Lines == 8 && report->Imported == 4 && report->Duplicates == 1 && report->Rejected == 3 && report->Total == 4,
                session.Label + Format(": import counted %zu lines, %zu imported, %zu duplicates, %zu rejected", report->Lines, report->Imported, report->Duplicates, report->Rejected));
            CHECK(report->Problems.size() == 3 && report->Problems.front().starts_with("line 10,"));
            const std::vector<CodeCandidate> imported = filter.Candidates();
            CHECK(filter.Prepared() && filter.History().empty() && imported.size() == 4);
            for (const std::uint64_t address : { session.Action, session.Leaf, session.Never, session.Idle })
                CHECK_MSG(Has(imported, address), session.Label + ": the import missed " + Hex(address));
            CHECK(std::all_of(imported.begin(), imported.end(), [](const CodeCandidate& candidate) { return candidate.Sources == SourceImport && candidate.Function == 0; }));
            CHECK(CodeSourceText(SourceImport) == "Import");

            const long calls = Field(session, &Probe::Action);
            CHECK(filter.Start(FilterRound::KeepOnly).Ok());
            Trigger(session);
            CHECK(WaitFor(2000, [&] { return Field(session, &Probe::Action) >= calls + 8; }));
            CHECK(filter.Stop().Ok());
            const std::vector<CodeCandidate> kept = filter.Candidates();
            CHECK_MSG(Has(kept, session.Action) && Has(kept, session.Leaf) && !Has(kept, session.Never), session.Label + ": a round on imported addresses went wrong");
            const Result<CodeImportReport> late = filter.Import(Hex(session.Never), ImportMode::Add);
            CHECK(!late && late.Error().Code == ErrorCode::CodeFilterFailed);

            const std::string exported = filter.ExportText();
            CHECK(exported.starts_with("# Manifold Engine code filter: " + std::to_string(kept.size()) + " addresses"));
            const Result<CodeImportReport> back = filter.Import(exported);
            CHECK(back.Ok() && back->Imported == kept.size() && back->Rejected == 0 && filter.History().empty());
            CHECK(back.Ok() && filter.Candidates().size() == kept.size() && Has(filter.Candidates(), session.Action));

            const Result<CodeImportReport> nothing = filter.Import("# only a comment\n\n");
            CHECK(!nothing && nothing.Error().Code == ErrorCode::CodeFilterFailed && filter.Remaining() == kept.size());
            const Result<CodeImportReport> noCode = filter.Import(Hex(session.ProbeAt) + "\n");
            CHECK(!noCode && noCode.Error().Text.find("not in executable memory") != std::string::npos);

            CodeFilterOptions options;
            options.Modules = { session.Module };
            options.Calls = true;
            options.Pointers = false;
            const Result<CodeFilterReport> prepared = filter.Prepare(options);
            CHECK(prepared.Ok() && !Has(filter.Candidates(), session.Virtual));
            const std::size_t before = filter.Remaining();
            const Result<CodeImportReport> added = filter.Import(Hex(session.Virtual) + "\n" + Hex(session.Action) + "\n", ImportMode::Add);
            CHECK_MSG(added.Ok() && added->Imported == 1 && added->Duplicates == 1 && added->Total == before + 1, session.Label + ": adding to the collected list went wrong");
            const std::vector<CodeCandidate> merged = filter.Candidates();
            CHECK(merged.size() == before + 1 && std::is_sorted(merged.begin(), merged.end(), [](const CodeCandidate& a, const CodeCandidate& b) { return a.Address < b.Address; }));
            const auto virtualAt = std::find_if(merged.begin(), merged.end(), [&](const CodeCandidate& candidate) { return candidate.Address == session.Virtual; });
            CHECK(virtualAt != merged.end() && virtualAt->Sources == SourceImport);
            const auto actionAt = std::find_if(merged.begin(), merged.end(), [&](const CodeCandidate& candidate) { return candidate.Address == session.Action; });
            CHECK(actionAt != merged.end() && !(actionAt->Sources & SourceImport));
            CHECK(filter.Report().Total == before + 1);
            filter.Reset();
            CHECK(AllOriginal(session));
        }

        void CheckLua(Session& session)
        {
            Engine& engine = session.Core;
            std::string result;
            const std::string script = R"(
                local cf = manifold.codefilter
                if not manifold.has("codefilter.prepare") then return "api" end
                if cf.status().prepared then return "prepared before" end
                local none, failure = cf.start("filterOut")
                if none ~= nil or failure.code ~= "ME-1020" then return "start before prepare" end
                local report, err = cf.prepare({ modules = { ")" + session.Module + R"(" }, calls = true })
                if not report then return "prepare: " .. tostring(err) end
                if report.total < 50 or #report.modules ~= 1 or report.modules[1].functions ~= report.total then return "report" end
                local status = cf.status()
                if not status.prepared or status.running or status.remaining ~= report.total or status.total ~= report.total then return "status" end
                local list = cf.candidates(5)
                if #list ~= 5 or type(list[1].address) ~= "number" or #list[1].sources < 1 or type(list[1].name) ~= "string" then return "candidates" end
                if pcall(cf.start, "sideways") then return "misuse" end
                if cf.start("filterOut") ~= true or not cf.status().running or cf.status().kind ~= "filterOut" then return "start" end
                local record = cf.stop()
                if not record or record.kind ~= "filterOut" or record.before < record.after or record.after + record.hits + record.skipped ~= record.before then return "stop" end
                if #cf.history() ~= 1 then return "history" end
                local tooMany, refineError = cf.refine(1)
                if tooMany ~= nil or refineError.code ~= "ME-1020" then return "refine limit" end
                if cf.start("keepOnly") ~= true or cf.cancel() ~= true or cf.status().running then return "cancel" end
                if cf.undo() ~= true or cf.status().remaining ~= record.before - record.skipped or #cf.history() ~= 0 then return "undo" end
                if cf.undo() ~= false then return "undo twice" end
                if cf.reset() ~= true or cf.status().prepared then return "reset" end
                local action = )" + std::to_string(session.Action) + R"(
                local imported, importError = cf.import({ action, string.format("0x%X", action), "nowhere at all" })
                if not imported then return "import: " .. tostring(importError) end
                if imported.imported ~= 1 or imported.duplicates ~= 1 or imported.rejected ~= 1 or #imported.problems ~= 1 or imported.total ~= 1 then return "import counts" end
                local only = cf.candidates()
                if #only ~= 1 or only[1].address ~= action or only[1].sources[1] ~= "import" then return "import candidates" end
                local text = cf.export()
                if type(text) ~= "string" or not text:find("^# Manifold Engine code filter: 1 addresses") then return "export" end
                local again = cf.import(text)
                if not again or again.imported ~= 1 or again.rejected ~= 0 then return "import export" end
                if pcall(cf.import, { {} }) then return "import misuse" end
                local empty, emptyError = cf.import("")
                if empty ~= nil or emptyError.code ~= "ME-1020" then return "import empty" end
                if cf.reset() ~= true then return "reset after import" end
                return "ok"
            )";
            const Status status = engine.Lua().RunWithResult(script, "codefilter", result);
            CHECK_MSG(status.Ok() && result == "ok", session.Label + " Lua: " + (status.Ok() ? result : status.Text));
            CHECK(!engine.Filter().Prepared());
            CHECK(AllOriginal(session));
        }

        std::uint64_t Export(Engine& engine, const std::string& module, const char* name)
        {
            return engine.Process()->FindExport(module, name);
        }

        // The code turns unwritable while an int3 is in (here Arbitrary Code Guard, which the
        // target switches on for good): the int3 cannot go back, so the round must stay active,
        // the handler and the library must stay for its trap, and the table must not be replaced.
        // Last on purpose: the target keeps the int3 and the guard until it ends.
        void CheckStuckInt3(Session& session)
        {
            Engine& engine = session.Core;
            const Status opened = engine.OpenProcess(MakeProcessId(session.Target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            Target::Coverage& coverage = engine.CodeCoverage();
            const std::uint64_t never[] = { session.Never };
            CHECK(coverage.Arm(never).Ok() && RawByte(session, session.Never) == Target::Cover::kInt3);
            SetField(session, offsetof(Probe, Lock), 1);
            const bool locked = WaitFor(3000, [&] { return Field(session, &Probe::Lock) >= 2; });
            if (!locked || Field(session, &Probe::Lock) != 2)
            {
                Skip(session.Label + ": the target could not switch on Arbitrary Code Guard");
                coverage.Release();
                return;
            }
            const Status disarmed = coverage.Disarm();
            CHECK_MSG(!disarmed.Ok() && disarmed.Text.find("could not be taken out") != std::string::npos,
                session.Label + ": a round whose int3 could not go back counted as ended: " + disarmed.Text);
            CHECK_MSG(RawByte(session, session.Never) == Target::Cover::kInt3, session.Label + ": the int3 is gone although the code could not be written");
            const auto counts = coverage.Counts();
            CHECK_MSG(counts && counts->Active && counts->Armed == 1, session.Label + ": the library gave up the round of an int3 still in the code");
            const std::uint64_t action[] = { session.Action };
            CHECK_MSG(!coverage.Arm(action).Ok() && StateIs(session, session.Never, Target::CoverState::Armed),
                session.Label + ": a new round replaced the table while an int3 of the old one was still in the code");
            CHECK_MSG(!coverage.Release().Ok() && coverage.Installed(), session.Label + ": release counted as done with an int3 still in the code");
            Pause(static_cast<int>(Target::AgentGrace()) + 800);
            CHECK_MSG(AgentLoaded(session), session.Label + ": the library unloaded while its int3 was still in the code");
            CHECK(session.Target.Alive());
            engine.CloseProcess();
        }

        void CheckTarget(const std::filesystem::path& path, const std::filesystem::path& tests, const char* label)
        {
            Child target;
            if (!Start(path, L"--codefilter", target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.UseInProcessDebugger = true;
            Engine engine(settings);
            Session session{ engine, target };
            session.Label = label;
            session.Config = &settings;
            const std::string module = WideToUtf8(path.filename().wstring());
            session.Module = module;
            const bool found = WaitFor(8000, [&]
                {
                    if (!target.Alive())
                        return true;
                    if (!engine.HasProcess() && !engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)))
                        return false;
                    engine.Process()->RefreshModules();
                    session.ProbeAt = Export(engine, module, "g_coverProbe");
                    return session.ProbeAt && Field(session, &Probe::Ready) == 1 && Field(session, &Probe::Threads) == 8;
                });
            CHECK_MSG(found && target.Alive() && session.ProbeAt, std::string(label) + ": the code filter probe of the test target did not start");
            if (!found || !session.ProbeAt || !target.Alive())
                return;
            session.Idle = Export(engine, module, "CoverIdle");
            session.Action = Export(engine, module, "CoverAction");
            session.Leaf = Export(engine, module, "CoverLeaf");
            session.Never = Export(engine, module, "CoverNever");
            session.Foreign = Export(engine, module, "CoverForeign");
            CHECK(session.Idle && session.Action && session.Leaf && session.Never && session.Foreign);
            {
                const Target::Process& process = *engine.Process();
                const auto shape = process.ReadPointer(Export(engine, module, "g_coverShape"));
                const auto vtable = shape ? process.ReadPointer(*shape) : std::nullopt;
                const auto area = vtable ? process.ReadPointer(*vtable) : std::nullopt;
                session.Virtual = area.value_or(0);
                CHECK_MSG(session.Virtual != 0 && engine.Process()->ModuleAt(session.Virtual).has_value(), std::string(label) + ": the virtual method of the test target was not found");
            }
            for (const std::uint64_t address : { session.Idle, session.Action, session.Leaf, session.Never })
            {
                const int value = RawByte(session, address);
                CHECK(value >= 0 && value != Target::Cover::kInt3);
                session.Originals[address] = static_cast<std::uint8_t>(value);
            }
            CheckRound(session);
            CheckNextRound(session);
            CheckStress(session);
            CheckSkips(session);
            CheckDebugger(session);
            CheckHandledInt3(session);
            CheckShadow(session);
            CheckWindowsDebugger(session);
            CheckFilter(session);
            CheckWithoutPointers(session);
            CheckImport(session);
            CheckLua(session);
            CheckForeignWrite(session);
            CheckRelease(session);
            CheckHostLoss(session, tests);
            engine.CloseProcess();
            CHECK_MSG(target.Alive(), std::string(label) + ": the target ended during the code filter tests");
            CHECK(AllOriginal(session));
            CheckStuckInt3(session);
        }

        int RunCodeFilterTests(const std::filesystem::path& target)
        {
            const int before = Totals().Failures;
            InitSysCalls();
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            {
                Settings settings;
                Engine engine(settings);
                const std::uint64_t none[] = { 0x1000 };
                CHECK(engine.CodeCoverage().Arm(none).Code == ErrorCode::NoProcess);
                CHECK(engine.CodeCoverage().Disarm().Ok() && engine.CodeCoverage().Release().Ok());
                CHECK(!engine.CodeCoverage().Installed() && !engine.CodeCoverage().Armed());
            }
            CheckTarget(target, self, "64-bit");
            CheckTarget(target.parent_path() / L"TestTarget32.exe", self, "32-bit");
            return Totals().Failures - before;
        }

        int RunCodeFilterHost(const std::vector<std::string>& arguments)
        {
            if (arguments.size() < 3)
                return 2;
            InitSysCalls();
            Settings settings;
            Engine engine(settings);
            if (!engine.OpenProcess(MakeProcessId(static_cast<std::uint32_t>(std::strtoul(arguments[1].c_str(), nullptr, 10)))))
                return 3;
            std::vector<std::uint64_t> addresses;
            for (std::size_t i = 2; i < arguments.size(); ++i)
                addresses.push_back(std::strtoull(arguments[i].c_str(), nullptr, 10));
            if (const Status armed = engine.CodeCoverage().Arm(addresses); !armed)
            {
                std::printf("failed: %s\n", armed.Text.c_str());
                std::fflush(stdout);
                return 4;
            }
            std::printf("ready\n");
            std::fflush(stdout);
            for (;;)
                ::Sleep(1000);
        }
    }

    MANIFOLD_MODE("--codefilter-host", &RunCodeFilterHost);

    MANIFOLD_SUITE("codefilter", "Code filter", 127, [](const SuiteContext& context) { return RunCodeFilterTests(context.Target); });
}
