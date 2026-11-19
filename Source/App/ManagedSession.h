#pragma once

#include "App/Settings.h"
#include "App/TargetSession.h"
#include "Core/BackgroundWork.h"
#include "Core/Errors.h"
#include "Core/RuntimeLog.h"

#include <cstdint>
#include <functional>
#include <string>

namespace Manifold
{
    class ManagedSession
    {
    public:
        using Notifier = std::function<void(LogLevel level, std::string text)>;

        ManagedSession(TargetSession& target, BackgroundWork& work, const Settings& settings, Notifier notify)
            : m_target(target), m_work(work), m_settings(settings), m_notify(std::move(notify))
        {
        }

        ManagedSession(const ManagedSession&) = delete;
        ManagedSession& operator=(const ManagedSession&) = delete;

        Status Ensure();
        Status ReloadNow();
        void Load(bool fromScript = false);
        void Reload(bool fromScript = false);
        void Stop(bool fromScript = false);
        void CancelWork();
        bool Loading() const { return m_work.Busy(kWorkGroup); }
        int WorkId() const { return m_work.Find(kWorkGroup); }
        void NoteExplorer(bool exited);

    private:
        static constexpr const char* kWorkGroup = "mono";

        void Finish(const Status& status, bool cancelled, bool starting, bool fromScript, std::uint64_t generation);
        void Notify(const Status& status);

        TargetSession& m_target;
        BackgroundWork& m_work;
        const Settings& m_settings;
        Notifier m_notify;
    };
}
