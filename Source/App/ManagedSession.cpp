#include "App/ManagedSession.h"

#include "Core/Text.h"
#include "Target/UnityExplorer.h"

#include <chrono>
#include <optional>

#include <windows.h>

namespace Manifold
{
    Status ManagedSession::Ensure()
    {
        m_work.CompleteGroup(kWorkGroup);
        if (m_target.Mono().Present())
            return Status::Success();
        if (!m_target.Process())
            return Status::Fail(ErrorCode::NoProcess);
        const Status started = m_target.Mono().Start(m_target.Process());
        if (started)
            m_target.RefreshSymbolProvider();
        return started;
    }

    Status ManagedSession::ReloadNow()
    {
        m_work.CompleteGroup(kWorkGroup);
        if (!m_target.Mono().Present())
            return Ensure();
        return m_target.Mono().Refresh();
    }

    void ManagedSession::Load(bool fromScript)
    {
        if (m_target.Mono().Present() || Loading())
            return;
        if (!m_target.Process())
        {
            if (!fromScript)
                Notify(Status::Fail(ErrorCode::NoProcess));
            return;
        }
        const std::shared_ptr<Target::Process> process = m_target.Process();
        const std::uint64_t generation = m_target.Generation();
        const bool il2cpp = Target::MonoBridge::DetectRuntime(*process) == Target::ManagedRuntime::Il2Cpp;
        m_work.Start({ .Title = il2cpp ? "Loading the IL2CPP runtime" : "Loading the Mono runtime", .Group = kWorkGroup }, [this, process, generation, fromScript](WorkProgress& progress) -> BackgroundWork::Finish
            {
                const Status status = m_target.Mono().Start(process, &progress);
                const bool cancelled = progress.Cancelled();
                return [this, status, cancelled, generation, fromScript]() { Finish(status, cancelled, true, fromScript, generation); };
            });
    }

    void ManagedSession::Reload(bool fromScript)
    {
        if (Loading())
            return;
        if (!m_target.Mono().Present())
        {
            Load(fromScript);
            return;
        }
        const std::uint64_t generation = m_target.Generation();
        m_work.Start({ .Title = m_target.Mono().IsIl2Cpp() ? "Reloading the IL2CPP classes" : "Reloading the Mono classes", .Group = kWorkGroup }, [this, generation, fromScript](WorkProgress& progress) -> BackgroundWork::Finish
            {
                const Status status = m_target.Mono().Refresh(&progress);
                const bool cancelled = progress.Cancelled();
                return [this, status, cancelled, generation, fromScript]() { Finish(status, cancelled, false, fromScript, generation); };
            });
    }

    void ManagedSession::Stop(bool fromScript)
    {
        CancelWork();
        if (!m_target.Mono().Present())
            return;
        const std::string runtime = Target::MonoBridge::RuntimeName(m_target.Mono().Runtime());
        NoteExplorer(false);
        m_target.Mono().Stop();
        m_target.RefreshSymbolProvider();
        LOG_INFO("Mono", "Shut down the " + runtime + " helper in the game");
        if (!fromScript)
            m_notify(LogLevel::Success, "Shut down " + runtime + ": the helper in the game has ended");
    }

    void ManagedSession::CancelWork()
    {
        m_work.CancelGroup(kWorkGroup);
        m_work.CompleteGroup(kWorkGroup);
    }

    void ManagedSession::Finish(const Status& status, bool cancelled, bool starting, bool fromScript, std::uint64_t generation)
    {
        if (generation != m_target.Generation())
            return;
        if (status)
        {
            if (starting)
                m_target.RefreshSymbolProvider();
            if (starting && m_settings.AutoExplorer && !m_target.Mono().IsIl2Cpp() && !m_target.Mono().Unity().Enabled())
            {
                m_target.Mono().Unity().SetEnabled(true);
                LOG_INFO("Unity", "Turned the scene explorer on by itself (Settings > Debugger > Start scene explorer automatically)");
            }
            if (!fromScript)
                m_notify(LogLevel::Success, Format("%s %zu %s classes", starting ? "Loaded" : "Reloaded", m_target.Mono().ClassCount(), Target::MonoBridge::RuntimeName(m_target.Mono().Runtime())));
            return;
        }
        if (cancelled)
        {
            LOG_INFO("Mono", starting ? "Loading the managed runtime was cancelled" : "Reloading the classes was cancelled; the earlier list stays");
            return;
        }
        if (fromScript)
            LOG_WARN("Mono", status.Text);
        else
            Notify(status);
    }

    void ManagedSession::Notify(const Status& status)
    {
        if (!status.Ok())
            m_notify(LogLevel::Error, status.Text);
    }

    void ManagedSession::NoteExplorer(bool exited)
    {
        const std::shared_ptr<Target::Process> process = m_target.Process();
        if (!process || !m_target.Mono().Present())
            return;
        const std::optional<Target::UnityStatus> unity = m_target.Mono().UnityState();
        if (!unity || !unity->Hooked)
            return;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - unity->HookedSince).count();
        if (!exited)
        {
            if (process->Alive())
                LOG_INFO("Unity", Format("The scene explorer's agent ran %.0f s in %s (%llu frames) until it was taken out", seconds, process->Name().c_str(),
                    static_cast<unsigned long long>(unity->CallbackRuns)));
            return;
        }
        DWORD code = 0;
        std::optional<std::uint32_t> exitCode;
        if (::GetExitCodeProcess(process->Handle(), &code) && code != STILL_ACTIVE)
            exitCode = static_cast<std::uint32_t>(code);
        const Target::ExplorerExitNote note = Target::UnityExplorer::ExitNote(*unity, process->Name(), exitCode, seconds);
        if (!note.Crashed)
        {
            LOG_INFO("Unity", note.Text);
            return;
        }
        LOG_WARN("Unity", note.Text);
        m_notify(LogLevel::Warning, process->Name() + " crashed while the scene explorer was active. The log has the details.");
    }
}
