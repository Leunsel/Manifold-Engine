#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Target/AgentLife.h"
#include "Target/AgentLifeProtocol.h"
#include "Target/Process.h"

#include <cstdio>
#include <cstdlib>
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
        constexpr std::uint32_t kGrace = 400;

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

        bool Launch(const std::filesystem::path& path, const std::wstring& arguments, Child& child)
        {
            STARTUPINFOW startup{ sizeof(startup) };
            std::wstring command = L"\"" + path.wstring() + L"\" " + arguments;
            return ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.Info) != FALSE;
        }

        bool WaitFor(int milliseconds, const std::function<bool()>& done)
        {
            const ULONGLONG until = ::GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
            for (;;)
            {
                if (done())
                    return true;
                if (::GetTickCount64() >= until)
                    return false;
                ::Sleep(20);
            }
        }

        // The agent in the target as a fresh look would see it, without loading anything.
        struct Look
        {
            bool Loaded = false;
            Target::Life::Block Life{};
        };

        Look LookAt(DWORD pid)
        {
            Look look;
            Result<std::shared_ptr<Target::Process>> process = Target::Process::Open(MakeProcessId(pid));
            if (!process)
                return look;
            Target::Process& target = **process;
            target.RefreshModules();
            const std::string name = Target::AgentFileName(target.Is64Bit());
            look.Loaded = target.FindModule(name).has_value();
            if (look.Loaded)
            {
                const std::uint64_t block = target.FindExport(name, "ManifoldLife");
                if (block)
                    target.ReadExact(block, &look.Life, sizeof(look.Life));
            }
            return look;
        }

        bool Unloads(DWORD pid, int milliseconds)
        {
            return WaitFor(milliseconds, [&] { return !LookAt(pid).Loaded; });
        }

        enum class Part
        {
            Crash,
            Speed,
            Cover
        };

        const char* PartName(Part part)
        {
            switch (part)
            {
            case Part::Crash: return "crash watch";
            case Part::Speed: return "speedhack";
            case Part::Cover: return "code filter";
            }
            return "";
        }

        Status StartPart(Engine& engine, Part part)
        {
            switch (part)
            {
            case Part::Crash: return engine.Crashes().Start();
            case Part::Speed: return engine.Speed().SetSpeed(2.0);
            case Part::Cover:
            {
                const std::uint64_t address = engine.Process()->FindExport(engine.Process()->Name(), "CoverIdle");
                const std::uint64_t addresses[] = { address };
                return engine.CodeCoverage().Arm(addresses);
            }
            }
            return Status::Success();
        }

        // Each part, switched on and off again while Manifold runs: the agent leaves once none needs it.
        void CheckNormalEnd(const std::filesystem::path& path, const char* label, Part part)
        {
            const std::string what = std::string(label) + " " + PartName(part);
            Child target;
            if (!Launch(path, part == Part::Cover ? L"--codefilter" : L"--crash", target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Settings settings;
            settings.CrashWatch = false;
            {
                Engine engine(settings);
                const bool opened = WaitFor(5000, [&] { return engine.OpenProcess(MakeProcessId(target.Info.dwProcessId)).Ok(); });
                CHECK_MSG(opened, what + ": the test target could not be opened");
                if (!opened)
                    return;
                WaitFor(3000, [&]
                    {
                        engine.Process()->RefreshModules();
                        return part != Part::Cover || engine.Process()->FindExport(engine.Process()->Name(), "CoverIdle") != 0;
                    });
                const Status started = StartPart(engine, part);
                CHECK_MSG(started.Ok(), what + ": " + started.Text);
                const Look on = LookAt(target.Info.dwProcessId);
                CHECK_MSG(on.Loaded && on.Life.Magic == Target::Life::kMagic && on.Life.Parts != 0 && on.Life.HostLoads >= 1,
                    what + Format(": the agent does not know the part (parts %u, loads %u)", on.Life.Parts, on.Life.HostLoads));
                ::Sleep(kGrace * 2);
                CHECK_MSG(LookAt(target.Info.dwProcessId).Loaded, what + ": the agent left while the part still needed it");
                if (part == Part::Speed)
                    CHECK(engine.Speed().Remove().Ok());
                engine.CloseProcess();
            }
            CHECK_MSG(Unloads(target.Info.dwProcessId, static_cast<int>(kGrace) + 3000), what + ": the agent stayed after Manifold let go of it");
            CHECK_MSG(target.Alive(), what + ": the game ended when the agent unloaded");
        }

        // Manifold ends without letting go (a crash, Task Manager): the agent takes its parts
        // out by itself and unloads, unless speedhack hooks still point into it.
        void CheckHostDeath(const std::filesystem::path& path, const std::filesystem::path& tests, const char* label, Part part)
        {
            const std::string what = std::string(label) + " " + PartName(part) + " without Manifold";
            Child target;
            if (!Launch(path, part == Part::Cover ? L"--codefilter" : L"--crash", target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            ::Sleep(300);
            Child host;
            const std::wstring arguments = L"--agent-host " + std::to_wstring(target.Info.dwProcessId) + L" " + std::to_wstring(static_cast<int>(part));
            CHECK(Launch(tests, arguments, host));
            const bool held = WaitFor(8000, [&] { const Look look = LookAt(target.Info.dwProcessId); return look.Loaded && look.Life.Parts != 0; });
            CHECK_MSG(held, what + ": the host did not start the part");
            if (!held)
                return;
            host.End();
            if (part == Part::Speed)
            {
                ::Sleep(kGrace + 1500);
                const Look look = LookAt(target.Info.dwProcessId);
                CHECK_MSG(look.Loaded && (look.Life.Parts & Target::Life::PartSpeed), what + ": the agent left although speedhack hooks still point into it");
                CHECK(look.Life.HostGone == 1);
            }
            else
            {
                CHECK_MSG(Unloads(target.Info.dwProcessId, static_cast<int>(kGrace) + 6000), what + ": the agent stayed after Manifold ended");
            }
            CHECK_MSG(target.Alive(), what + ": the game ended");
        }

        // A load the host counts must already carry a fresh Activity: the agent gives back every
        // counted load once it looks idle, and a part's first call would then land in unmapped memory.
        void CheckCountedLoad(const std::filesystem::path& path, const char* label)
        {
            const std::string what = std::string(label) + " counted load";
            Child target;
            if (!Launch(path, L"--crash", target))
            {
                Skip("the test target " + path.string() + " could not be started");
                return;
            }
            Result<std::shared_ptr<Target::Process>> opened = Target::Process::Open(MakeProcessId(target.Info.dwProcessId));
            CHECK_MSG(opened.Ok(), what + ": the test target could not be opened");
            if (!opened)
                return;
            Target::Process& process = **opened;
            const bool started = WaitFor(5000, [&] { process.RefreshModules(); return process.FindExport("kernel32.dll", "LoadLibraryW") != 0; });
            CHECK_MSG(started, what + ": the test target did not finish starting");
            if (!started)
                return;
            int probes = 0;
            int stale = 0;
            std::uint64_t before = 0;
            Target::SetAgentLoadProbe([&](std::uint64_t block)
                {
                    Target::Life::Block life{};
                    ++probes;
                    if (!process.ReadExact(block, &life, sizeof(life)) || life.Activity < before)
                        ++stale;
                });
            for (int round = 0; round < 3; ++round)
            {
                before = ::GetTickCount64();
                const Status loaded = Target::LoadAgent(process);
                CHECK_MSG(loaded.Ok(), what + ": " + loaded.Text);
                ::Sleep(kGrace / 2);
            }
            Target::SetAgentLoadProbe({});
            CHECK_MSG(probes == 3 && stale == 0, what + Format(": %d of %d counted loads carried an old Activity", stale, probes));
            const Look look = LookAt(target.Info.dwProcessId);
            CHECK_MSG(look.Life.HostLoads == 3, what + Format(": the agent counts %u loads", look.Life.HostLoads));
            CHECK_MSG(Unloads(target.Info.dwProcessId, static_cast<int>(kGrace) + 3000), what + ": the agent stayed although no part needs it");
            CHECK_MSG(target.Alive(), what + ": the game ended when the agent unloaded");
        }

        int RunAgentHost(const std::vector<std::string>& arguments)
        {
            if (arguments.size() < 3)
                return 2;
            InitSysCalls();
            Target::SetAgentGrace(kGrace);
            Settings settings;
            settings.CrashWatch = false;
            Engine engine(settings);
            if (!engine.OpenProcess(MakeProcessId(static_cast<std::uint32_t>(std::strtoul(arguments[1].c_str(), nullptr, 10)))))
                return 3;
            engine.Process()->RefreshModules();
            if (!StartPart(engine, static_cast<Part>(std::atoi(arguments[2].c_str()))))
                return 4;
            for (;;)
                ::Sleep(1000);
        }

        int RunAgentLifeTests(const std::filesystem::path& target)
        {
            const int before = Totals().Failures;
            InitSysCalls();
            Target::SetAgentGrace(kGrace);
            wchar_t self[MAX_PATH] = L"";
            ::GetModuleFileNameW(nullptr, self, MAX_PATH);
            for (const auto& [path, label] : { std::pair{ target, "64-bit" }, std::pair{ target.parent_path() / L"TestTarget32.exe", "32-bit" } })
            {
                CheckCountedLoad(path, label);
                for (const Part part : { Part::Crash, Part::Speed, Part::Cover })
                {
                    CheckNormalEnd(path, label, part);
                    CheckHostDeath(path, self, label, part);
                }
            }
            return Totals().Failures - before;
        }
    }

    MANIFOLD_MODE("--agent-host", &RunAgentHost);

    MANIFOLD_SUITE("agentlife", "Agent lifetime", 129, [](const SuiteContext& context) { return RunAgentLifeTests(context.Target); });
}
