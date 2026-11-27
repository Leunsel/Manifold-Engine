#include "Target/AgentLife.h"

#include "Core/Handle.h"
#include "Core/Text.h"
#include "Target/AgentLifeProtocol.h"
#include "Target/Process.h"

#include <atomic>
#include <cstddef>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        std::atomic<std::uint32_t> g_grace{ Life::kDefaultGraceMs };
        std::function<void(std::uint64_t)> g_loadProbe;
    }

    void SetAgentLoadProbe(std::function<void(std::uint64_t)> probe)
    {
        g_loadProbe = std::move(probe);
    }

    std::string AgentFileName(bool is64Bit)
    {
        return is64Bit ? "ManifoldAgent.dll" : "ManifoldAgent32.dll";
    }

    std::filesystem::path AgentFilePath(bool is64Bit)
    {
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        return std::filesystem::path(self).parent_path() / Utf8ToWide(AgentFileName(is64Bit));
    }

    void SetAgentGrace(std::uint32_t milliseconds)
    {
        g_grace = milliseconds;
    }

    std::uint32_t AgentGrace()
    {
        return g_grace;
    }

    Status LoadAgent(const Process& process)
    {
        const bool is64Bit = process.Is64Bit();
        std::filesystem::path path = AgentFilePath(is64Bit);
        const std::string name = AgentFileName(is64Bit);
        std::error_code ignore;
        if (!std::filesystem::exists(path, ignore))
            return Status::Fail(ErrorCode::InjectFailed, name + " was not found next to the program");
        // ∑ The agent gives back every counted load once it was idle for its grace
        //   time, so a load must never become visible with an old Activity: the
        //   agent would free it with the others and the part's first call would
        //   land in unmapped memory. Activity is touched before the inject (an
        //   agent that is loaded already keeps its lifetime thread), and again
        //   right before and after the count.
        const auto touch = [&process](std::uint64_t block)
        {
            const std::uint64_t now = ::GetTickCount64();
            process.Write(block + offsetof(Life::Block, Activity), &now, sizeof(now));
        };
        if (!Process::DryRun())
        {
            // the module list may be older than the agent's last unload: touch only a block that is still there
            Life::Block early{};
            const_cast<Process&>(process).RefreshModules();
            const std::uint64_t at = process.FindExport(name, "ManifoldLife");
            if (at && process.ReadExact(at, &early, sizeof(early)) && early.Magic == Life::kMagic && early.Version == Life::kVersion)
            {
                touch(at);
                if (const auto loaded = process.ModuleAt(at); loaded && !loaded->Path.empty())
                    path = loaded->Path;
            }
        }
        if (Status loaded = process.InjectLibrary(path.wstring()); !loaded)
            return loaded;
        if (Process::DryRun())
            return Status::Success();
        const std::uint64_t block = process.FindExport(name, "ManifoldLife");
        Life::Block life{};
        if (!block || !process.ReadExact(block, &life, sizeof(life)) || life.Magic != Life::kMagic || life.Version != Life::kVersion)
            return Status::Success();  // an agent from before the lifetime: it stays loaded, as it always did
        const std::uint32_t grace = g_grace;
        process.Write(block + offsetof(Life::Block, GraceMs), &grace, sizeof(grace));
        touch(block);
        const std::uint32_t loads = life.HostLoads + 1;
        process.Write(block + offsetof(Life::Block, HostLoads), &loads, sizeof(loads));
        if (g_loadProbe)
            g_loadProbe(block);
        touch(block);
        // the agent watches this program's handle; one is enough for the session
        if (life.HostHandle == 0 || life.HostGone)
        {
            const UniqueHandle target(::OpenProcess(PROCESS_DUP_HANDLE, FALSE, Raw(process.Id())));
            HANDLE remote = nullptr;
            if (target && ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), target.Get(), &remote, SYNCHRONIZE, FALSE, 0))
            {
                const std::uint64_t value = reinterpret_cast<std::uintptr_t>(remote);
                process.Write(block + offsetof(Life::Block, HostHandle), &value, sizeof(value));
            }
        }
        return Status::Success();
    }
}
