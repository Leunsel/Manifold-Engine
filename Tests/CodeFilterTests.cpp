#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Target/Coverage.h"
#include "Target/CoverageProtocol.h"

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
            std::map<std::uint64_t, std::uint8_t> Originals;
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
            const long calls = Field(session, &Probe::Action) - before;
            const auto counts = coverage.Counts();
            std::printf("  %s: 300 rounds in %.2f s while the targets ran %ld times, %llu hits, %llu stale traps\n", session.Label.c_str(), seconds, calls,
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
            engine.Process()->RefreshModules();
            const std::uint64_t blockAt = engine.Process()->FindExport(engine.CodeCoverage().AgentName(), "ManifoldCover");
            Target::Cover::Block block{};
            SIZE_T got = 0;
            const bool read = blockAt && ::ReadProcessMemory(session.Target.Info.hProcess, reinterpret_cast<LPCVOID>(blockAt), &block, sizeof(block), &got) && got == sizeof(block);
            CHECK_MSG(read && block.HostLost == 1 && block.Active == 0 && block.Handler == 1, session.Label + ": the library did not note the lost program");

            engine.CloseProcess();
            const Status opened = engine.OpenProcess(MakeProcessId(pid));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            Target::Coverage& coverage = engine.CodeCoverage();
            CHECK_MSG(coverage.Installed(), session.Label + ": the library of the killed program was not taken over");
            const auto counts = coverage.Counts();
            CHECK_MSG(counts && counts->Watching && !counts->HostLost && !counts->Active, session.Label + ": the taken-over library does not watch this program");
            CHECK(coverage.Release().Ok());
            CHECK_MSG(WaitAgentGone(session, 5000), session.Label + ": the agent library stayed loaded after the takeover");
            CHECK(session.Target.Alive());
        }

        std::uint64_t Export(Engine& engine, const std::string& module, const char* name)
        {
            return engine.Process()->FindExport(module, name);
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
            const std::string module = WideToUtf8(path.filename().wstring());
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
            CheckRelease(session);
            CheckHostLoss(session, tests);
            engine.CloseProcess();
            CHECK_MSG(target.Alive(), std::string(label) + ": the target ended during the code filter tests");
            CHECK(AllOriginal(session));
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
