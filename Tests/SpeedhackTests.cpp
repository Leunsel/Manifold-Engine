#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Target/PatchScan.h"
#include "Target/Speedhack.h"
#include "Target/SpeedhackProtocol.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
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
            std::uint64_t Sequence;
            std::int64_t Counter;
            std::int64_t Frequency;
            std::uint64_t Tick64;
            std::uint64_t Samples;
            std::int64_t Backwards;
            std::uint32_t Tick;
            std::uint32_t Multimedia;
            std::uint32_t Ready;
            std::uint32_t Threads;
            std::uint32_t BackClock;
            std::uint32_t BackThread;
            std::int64_t BackBefore;
            std::int64_t BackNow;
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

        struct Rates
        {
            double Counter = 0.0;
            double Tick = 0.0;
            double Tick64 = 0.0;
            double Multimedia = 0.0;
        };

        struct Session
        {
            Engine& Core;
            const Child& Target;
            std::uint64_t ProbeAt = 0;
            std::string Label;
            std::map<std::uint64_t, std::vector<std::uint8_t>> Originals;
        };

        bool Start(const std::filesystem::path& path, const std::wstring& arguments, Child& child)
        {
            STARTUPINFOW startup{ sizeof(startup) };
            std::wstring command = L"\"" + path.wstring() + L"\" " + arguments;
            return ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.Info) != FALSE;
        }

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void Pause(int milliseconds)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }

        std::optional<Probe> ReadProbe(const Target::Process& process, std::uint64_t address)
        {
            for (int attempt = 0; attempt < 4000; ++attempt)
            {
                Probe probe{};
                std::uint64_t again = 0;
                if (!process.ReadExact(address, &probe, sizeof(probe)))
                    return std::nullopt;
                if (probe.Ready && (probe.Sequence & 1) == 0 && process.ReadExact(address, &again, sizeof(again)) && again == probe.Sequence)
                    return probe;
            }
            return std::nullopt;
        }

        std::int64_t Backwards(const Session& session)
        {
            const auto probe = ReadProbe(*session.Core.Process(), session.ProbeAt);
            return probe ? probe->Backwards : -1;
        }

        // The first clock that went back, as the target saw it.
        std::string BackText(const Session& session)
        {
            const auto probe = ReadProbe(*session.Core.Process(), session.ProbeAt);
            if (!probe || !probe->BackClock)
                return {};
            static constexpr const char* kNames[] = { "QueryPerformanceCounter", "GetTickCount64", "GetTickCount", "timeGetTime" };
            std::string clock;
            for (int bit = 0; bit < 4; ++bit)
            {
                if (probe->BackClock & (1u << bit))
                    clock += (clock.empty() ? "" : "+") + std::string(kNames[bit]);
            }
            return Format(" (first: %s on the %s thread, %lld then %lld)", clock.c_str(), probe->BackThread ? "publishing" : "other",
                static_cast<long long>(probe->BackBefore), static_cast<long long>(probe->BackNow));
        }

        Rates Measure(const Session& session, int milliseconds)
        {
            const Target::Process& process = *session.Core.Process();
            const auto first = ReadProbe(process, session.ProbeAt);
            const double started = Now();
            Pause(milliseconds);
            const auto second = ReadProbe(process, session.ProbeAt);
            const double elapsed = Now() - started;
            Rates rates;
            if (!first || !second || elapsed <= 0.0 || first->Frequency <= 0)
                return rates;
            rates.Counter = static_cast<double>(second->Counter - first->Counter) / static_cast<double>(first->Frequency) / elapsed;
            rates.Tick = static_cast<double>(static_cast<std::int32_t>(second->Tick - first->Tick)) / 1000.0 / elapsed;
            rates.Tick64 = static_cast<double>(static_cast<std::int64_t>(second->Tick64 - first->Tick64)) / 1000.0 / elapsed;
            rates.Multimedia = static_cast<double>(static_cast<std::int32_t>(second->Multimedia - first->Multimedia)) / 1000.0 / elapsed;
            return rates;
        }

        void CheckRates(const Session& session, double expected, const char* what)
        {
            const Rates rates = Measure(session, 700);
            const double fine = std::max(0.05 * expected, 0.02);
            const double coarse = std::max(0.10 * expected, 0.05);
            const std::string text = Format("%s %s: counter %.3f, tick %.3f, tick64 %.3f, multimedia %.3f, expected %.3f", session.Label.c_str(), what, rates.Counter, rates.Tick,
                rates.Tick64, rates.Multimedia, expected);
            CHECK_MSG(std::fabs(rates.Counter - expected) <= fine, text);
            CHECK_MSG(std::fabs(rates.Tick - expected) <= coarse, text);
            CHECK_MSG(std::fabs(rates.Tick64 - expected) <= coarse, text);
            CHECK_MSG(std::fabs(rates.Multimedia - expected) <= coarse, text);
        }

        bool PatchedOnDisk(const Target::Process& process, std::uint64_t address, bool& known)
        {
            known = false;
            const auto module = process.ModuleAt(address);
            if (!module)
                return false;
            std::vector<Target::CodePatch> patches;
            const Target::ModulePatchSummary summary = Target::ScanModuleForPatches(process, *module, patches);
            if (!summary.Problem.Ok())
                return false;
            known = true;
            for (const Target::CodePatch& patch : patches)
            {
                if (patch.Address < address + Target::Speed::kPatchBytes && address < patch.Address + patch.Size)
                    return true;
            }
            return false;
        }

        struct BlockState
        {
            std::uint32_t Serial = 0;
            std::uint32_t Applied = 0;
            double Speed = 0.0;
            bool Read = false;
        };

        BlockState ReadBlockState(Session& session)
        {
            BlockState state;
            session.Core.Process()->RefreshModules();
            const Target::Process& process = *session.Core.Process();
            const std::uint64_t block = process.FindExport(session.Core.Speed().AgentName(), "ManifoldSpeed");
            state.Read = block && process.ReadExact(block + offsetof(Target::Speed::Block, Serial), &state.Serial, sizeof(state.Serial)) &&
                process.ReadExact(block + offsetof(Target::Speed::Block, Applied), &state.Applied, sizeof(state.Applied)) &&
                process.ReadExact(block + offsetof(Target::Speed::Block, Speed), &state.Speed, sizeof(state.Speed));
            return state;
        }

        bool AgentLoaded(Session& session)
        {
            session.Core.Process()->RefreshModules();
            return session.Core.Process()->FindModule(session.Core.Speed().AgentName()).has_value();
        }

        bool WaitAgentGone(Session& session, int milliseconds)
        {
            const double until = Now() + milliseconds / 1000.0;
            while (Now() < until)
            {
                if (!AgentLoaded(session))
                    return true;
                Pause(50);
            }
            return !AgentLoaded(session);
        }

        void CheckRestored(Session& session, const std::vector<Target::SpeedhackHook>& hooks, const char* what)
        {
            const Target::Process& process = *session.Core.Process();
            for (const Target::SpeedhackHook& hook : hooks)
            {
                std::uint8_t now[Target::Speed::kPatchBytes] = {};
                const bool read = process.ReadExact(hook.Address, now, sizeof(now));
                const auto known = session.Originals.find(hook.Address);
                const std::vector<std::uint8_t>& original = known != session.Originals.end() ? known->second : hook.Original;
                CHECK_MSG(read && original.size() == sizeof(now) && std::memcmp(now, original.data(), sizeof(now)) == 0,
                    session.Label + " " + what + ": " + hook.Name + " at " + Hex(hook.Address) + " is " + HexBytes(now));
                if (const auto region = process.RegionAt(hook.Page))
                    CHECK_MSG(region->State == MEM_FREE, session.Label + " " + what + ": the hook page " + Hex(hook.Page) + " is still there");
            }
        }

        void CheckSpeeds(Session& session)
        {
            Engine& engine = session.Core;
            Target::Speedhack& speedhack = engine.Speed();
            CHECK(!speedhack.Installed() && !speedhack.Enabled() && speedhack.Speed() == 1.0);
            CheckRates(session, 1.0, "untouched");

            const Status doubled = speedhack.SetSpeed(2.0);
            CHECK_MSG(doubled.Ok(), session.Label + ": " + doubled.Text);
            if (!doubled)
                return;
            const std::vector<Target::SpeedhackHook> hooks = speedhack.Hooks();
            std::set<std::uint32_t> sources;
            for (const Target::SpeedhackHook& hook : hooks)
            {
                sources.insert(hook.Source);
                session.Originals[hook.Address] = hook.Original;
                std::uint8_t now[Target::Speed::kPatchBytes] = {};
                CHECK(engine.Process()->ReadExact(hook.Address, now, sizeof(now)) && now[0] == 0xE9);
                bool known = false;
                const bool patched = PatchedOnDisk(*engine.Process(), hook.Address, known);
                if (known)
                    CHECK_MSG(patched, session.Label + ": " + hook.Name + " does not differ from its file while hooked");
                CHECK(speedhack.Owns(hook.Address, 1) && speedhack.Owns(hook.Address + 4, 8) && !speedhack.Owns(hook.Address + 5, 4));
            }
            CHECK_MSG(sources.size() == Target::Speed::SourceCount, session.Label + ": " + std::to_string(sources.size()) + " of 4 time sources hooked");
            CHECK(speedhack.Installed() && speedhack.Enabled() && speedhack.Speed() == 2.0);
            CheckRates(session, 2.0, "speed 2");

            CHECK(speedhack.SetSpeed(0.5).Ok());
            CheckRates(session, 0.5, "speed 0.5");
            CHECK(speedhack.SetSpeed(0.0).Ok());
            CheckRates(session, 0.0, "speed 0");
            CHECK(speedhack.SetSpeed(5000.0).Ok() && speedhack.Speed() == Target::Speedhack::kMaxSpeed);
            CHECK(!speedhack.SetSpeed(std::nan("")).Ok());

            const double choices[] = { 0.0, 0.25, 1.0, 3.0, 10.0, 50.0, 0.5, 2.0 };
            for (int i = 0; i < 160; ++i)
            {
                CHECK(speedhack.SetSpeed(choices[(i * 7 + i / 8) % 8]).Ok());
                Pause(3);
            }
            CHECK(speedhack.SetSpeed(1.0).Ok());
            CheckRates(session, 1.0, "speed 1 after changes");
            CHECK_MSG(Backwards(session) == 0, session.Label + ": a clock ran backwards " + std::to_string(Backwards(session)) + " times during speed changes" + BackText(session));

            CHECK(speedhack.SetSpeed(3.0).Ok());
            Pause(100);
            CHECK(speedhack.Disable().Ok());
            CHECK(speedhack.Installed() && !speedhack.Enabled() && speedhack.ClockShifted() && speedhack.Speed() == 1.0);
            const BlockState settled = ReadBlockState(session);
            CHECK_MSG(settled.Read && settled.Applied == settled.Serial && settled.Speed == 1.0,
                session.Label + Format(": Disable returned before the agent applied speed 1 (serial %u, applied %u, speed %.3f)", settled.Serial, settled.Applied, settled.Speed));
            CheckRates(session, 1.0, "disabled after a shift");
            CHECK_MSG(Backwards(session) == 0, session.Label + ": a clock ran backwards when the speedhack was turned off" + BackText(session));

            CHECK(speedhack.Remove().Ok());
            CHECK(!speedhack.Installed() && speedhack.Hooks().empty());
            CheckRestored(session, hooks, "removed");
            for (const Target::SpeedhackHook& hook : hooks)
            {
                bool known = false;
                const bool patched = PatchedOnDisk(*engine.Process(), hook.Address, known);
                if (known)
                    CHECK_MSG(!patched, session.Label + ": " + hook.Name + " still differs from its file after the removal");
            }
            CHECK_MSG(WaitAgentGone(session, 3000), session.Label + ": the agent library stayed loaded after the removal");
            CHECK(session.Target.Alive());
            CheckRates(session, 1.0, "removed");
        }

        void CheckCycles(Session& session)
        {
            Target::Speedhack& speedhack = session.Core.Speed();
            const std::int64_t before = Backwards(session);
            std::vector<Target::SpeedhackHook> hooks;
            int clean = 0;
            for (int i = 0; i < 20; ++i)
            {
                if (!speedhack.SetSpeed(1.0))
                    break;
                hooks = speedhack.Hooks();
                Pause(10);
                if (speedhack.Disable() && !speedhack.Installed())
                    ++clean;
            }
            CHECK_MSG(clean == 20, session.Label + ": " + std::to_string(clean) + " of 20 on/off rounds at speed 1 left the process clean");
            CHECK_MSG(Backwards(session) == before, session.Label + ": a clock ran backwards while the speedhack went on and off at speed 1" + BackText(session));
            CheckRestored(session, hooks, "after 20 rounds at speed 1");

            int removed = 0;
            for (int i = 0; i < 12; ++i)
            {
                if (!speedhack.SetSpeed(i % 2 ? 2.0 : 0.5))
                    break;
                hooks = speedhack.Hooks();
                Pause(15);
                if (speedhack.Remove() && !speedhack.Installed())
                    ++removed;
            }
            CHECK_MSG(removed == 12, session.Label + ": " + std::to_string(removed) + " of 12 set/remove rounds worked");
            CheckRestored(session, hooks, "after 12 set/remove rounds");
            CHECK_MSG(WaitAgentGone(session, 3000), session.Label + ": the agent library stayed loaded after the rounds");
            CHECK(session.Target.Alive());
        }

        void CheckDebugger(Session& session)
        {
            Engine& engine = session.Core;
            Target::Speedhack& speedhack = engine.Speed();
            CHECK(speedhack.SetSpeed(2.0).Ok());
            const std::vector<Target::SpeedhackHook> hooks = speedhack.Hooks();
            Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok() && engine.Debugger().UsingVeh(), session.Label + ": " + attached.Text);
            CheckRates(session, 2.0, "debugger attached to a running speedhack");
            engine.Debugger().Detach();
            Pause(300);
            CHECK_MSG(AgentLoaded(session), session.Label + ": the debugger's detach unloaded the agent under the speedhack");
            CheckRates(session, 2.0, "debugger detached again");
            CHECK(speedhack.Remove().Ok());
            CheckRestored(session, hooks, "removed after the debugger");
            CHECK_MSG(WaitAgentGone(session, 3000), session.Label + ": the agent library stayed loaded");

            attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok(), session.Label + ": " + attached.Text);
            CHECK(speedhack.SetSpeed(0.5).Ok());
            CheckRates(session, 0.5, "speedhack under an attached debugger");
            CHECK(speedhack.Remove().Ok());
            CheckRestored(session, speedhack.Hooks(), "removed under the debugger");
            CHECK_MSG(AgentLoaded(session) && engine.Debugger().Attached(), session.Label + ": removing the speedhack took the debugger's agent away");
            engine.Debugger().Detach();
            CHECK_MSG(WaitAgentGone(session, 5000), session.Label + ": the agent library stayed loaded after the debugger left");
            CHECK(session.Target.Alive());
        }

        // The in-process debugger's agent shares the library with the speedhack
        // and times the host's heartbeat itself. With the hooked GetTickCount64
        // it took 1.5 s of silence at speed 1000 for 25 minutes and stopped
        // itself (and at speed 0 it never noticed a host that was gone).
        void CheckDebuggerClock(Session& session)
        {
            Engine& engine = session.Core;
            Target::Speedhack& speedhack = engine.Speed();
            CHECK(speedhack.SetSpeed(Target::Speedhack::kMaxSpeed).Ok());
            const std::vector<Target::SpeedhackHook> hooks = speedhack.Hooks();
            const Status attached = engine.EnsureDebugger();
            CHECK_MSG(attached.Ok() && engine.Debugger().UsingVeh(), session.Label + ": " + attached.Text);
            if (attached)
            {
                Debug::Debugger& debugger = engine.Debugger();
                int id = 0;
                const Status started = debugger.StartWatch(Debug::WatchKind::Writes, session.ProbeAt + offsetof(Probe, Threads), 4, id);
                CHECK_MSG(started.Ok(), session.Label + ": " + started.Text);
                debugger.PauseHeartbeat(true);
                Pause(1500);
                debugger.PauseHeartbeat(false);
                Pause(1500);
                Debug::Watch watch;
                CHECK_MSG(started && debugger.CopyWatch(id, watch) && watch.Running,
                    session.Label + ": the in-process debugger stopped itself at speed 1000 after 1.5 s without a heartbeat");
                if (started)
                    debugger.RemoveWatch(id);
                debugger.Detach();
            }
            CHECK(speedhack.Remove().Ok());
            CheckRestored(session, hooks, "removed after the debugger clock check");
            CHECK_MSG(WaitAgentGone(session, 5000), session.Label + ": the agent library stayed loaded after the debugger clock check");
            CHECK(session.Target.Alive());
        }

        void CheckLua(Session& session)
        {
            Engine& engine = session.Core;
            std::string result;
            const Status status = engine.Lua().RunWithResult(R"(
                if speedhack_getSpeed() ~= 1 then return "speed before" end
                if speedhack_setSpeed(2) ~= true or speedhack_getSpeed() ~= 2 then return "setSpeed" end
                if manifold.speed.get() ~= 2 or not manifold.speed.isEnabled() then return "get" end
                local status = manifold.speed.status()
                if not status.enabled or not status.installed or status.speed ~= 2 or #status.hooks < 4 or status.hooks[1].address == 0 then return "status" end
                if manifold.speed.set(0.5) ~= true or speedhack_getSpeed() ~= 0.5 then return "set" end
                if manifold.speed.disable() ~= true or manifold.speed.get() ~= 1 or manifold.speed.isEnabled() then return "disable" end
                if manifold.speed.remove() ~= true or manifold.speed.status().installed then return "remove" end
                return "ok"
            )", "speedhack", result);
            CHECK_MSG(status.Ok() && result == "ok", session.Label + " Lua: " + (status.Ok() ? result : status.Text));
            CHECK(!engine.Speed().Installed());
        }

        void CheckHostLoss(Session& session, const std::filesystem::path& tests)
        {
            Engine& engine = session.Core;
            const DWORD pid = session.Target.Info.dwProcessId;
            const std::int64_t before = Backwards(session);
            Child host;
            CHECK(Start(tests, L"--speedhack-host " + std::to_wstring(pid) + L" 3", host));
            bool fast = false;
            for (int attempt = 0; attempt < 30 && !fast && host.Alive(); ++attempt)
                fast = Measure(session, 300).Counter > 2.5;
            CHECK_MSG(fast, session.Label + ": the second program did not set speed 3");
            host.End();
            Pause(500);
            CHECK_MSG(session.Target.Alive(), session.Label + ": the target died with the program that set its speed");
            CheckRates(session, 1.0, "after the program that set the speed was killed");
            CHECK_MSG(Backwards(session) == before, session.Label + ": a clock ran backwards when the program that set the speed was killed" + BackText(session));

            engine.CloseProcess();
            const Status opened = engine.OpenProcess(MakeProcessId(pid));
            CHECK_MSG(opened.Ok(), opened.Text);
            if (!opened)
                return;
            Target::Speedhack& speedhack = engine.Speed();
            CHECK_MSG(speedhack.Installed() && !speedhack.Enabled(), session.Label + ": the hooks of the killed program were not taken over");
            const std::vector<Target::SpeedhackHook> hooks = speedhack.Hooks();
            CHECK(hooks.size() >= 4);
            CHECK(speedhack.SetSpeed(2.0).Ok());
            CheckRates(session, 2.0, "taken over");
            CHECK(speedhack.Remove().Ok());
            CheckRestored(session, hooks, "taken over and removed");
            CHECK_MSG(WaitAgentGone(session, 3000), session.Label + ": the agent library stayed loaded after the takeover");
            CHECK(session.Target.Alive());
        }

        void CheckTarget(const std::filesystem::path& path, const std::filesystem::path& tests, const char* label)
        {
            Child target;
            if (!Start(path, L"--speed", target))
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
            const double until = Now() + 8.0;
            while (Now() < until && target.Alive() && !session.ProbeAt)
            {
                Pause(100);
                if (!engine.HasProcess() && !engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)))
                    continue;
                engine.Process()->RefreshModules();
                const std::uint64_t probe = engine.Process()->FindExport(module, "g_speedProbe");
                if (probe && ReadProbe(*engine.Process(), probe))
                    session.ProbeAt = probe;
            }
            CHECK_MSG(session.ProbeAt != 0, std::string(label) + ": the speed probe of the test target did not start");
            if (!session.ProbeAt)
                return;
            CheckSpeeds(session);
            CheckCycles(session);
            CheckDebugger(session);
            CheckDebuggerClock(session);
            CheckLua(session);
            CheckHostLoss(session, tests);
            engine.CloseProcess();
            CHECK_MSG(target.Alive(), std::string(label) + ": the target ended during the speedhack tests");
        }

        int RunSpeedhackTests(const std::filesystem::path& target)
        {
            const int before = Totals().Failures;
            InitSysCalls();
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            {
                Settings settings;
                Engine engine(settings);
                const Status none = engine.Speed().SetSpeed(2.0);
                CHECK(none.Code == ErrorCode::NoProcess && !engine.Speed().Enabled() && engine.Speed().Speed() == 1.0);
                CHECK(engine.Speed().Disable().Ok() && engine.Speed().Remove().Ok());
                CHECK(Target::Speedhack::Text(2.0) == "2" && Target::Speedhack::Text(0.25) == "0.25");
            }
            CheckTarget(target, self, "64-bit");
            CheckTarget(target.parent_path() / L"TestTarget32.exe", self, "32-bit");
            return Totals().Failures - before;
        }

        int RunSpeedhackHost(const std::vector<std::string>& arguments)
        {
            if (arguments.size() < 3)
                return 2;
            InitSysCalls();
            Settings settings;
            Engine engine(settings);
            if (!engine.OpenProcess(MakeProcessId(static_cast<std::uint32_t>(std::strtoul(arguments[1].c_str(), nullptr, 10)))))
                return 3;
            if (!engine.Speed().SetSpeed(std::strtod(arguments[2].c_str(), nullptr)))
                return 4;
            std::printf("ready\n");
            std::fflush(stdout);
            for (;;)
                ::Sleep(1000);
        }
    }

    MANIFOLD_MODE("--speedhack-host", &RunSpeedhackHost);

    MANIFOLD_SUITE("speedhack", "Speedhack", 126, [](const SuiteContext& context) { return RunSpeedhackTests(context.Target); });
}
