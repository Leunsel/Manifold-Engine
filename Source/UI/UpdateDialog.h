#pragma once

#include "App/Updates.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    // Help > Check for Updates and the check at start. A newer release opens
    // a dialog with its notes; Install downloads the zip, checks its SHA-256,
    // swaps the program files and offers to restart. The first time a version
    // is offered the dialog has no Skip (owner rule, see Updates::NoticeFor).
    class UpdateDialog
    {
    public:
        using Restart = std::function<void(const std::string& from)>;

        UpdateDialog(Engine& engine, Restart restart);
        ~UpdateDialog();

        UpdateDialog(const UpdateDialog&) = delete;
        UpdateDialog& operator=(const UpdateDialog&) = delete;

        void CheckAtStart();
        void CheckNow();
        bool Checking() const { return m_checking; }
        // the newer version the last check found, also after Later
        const std::optional<Updates::Release>& Available() const { return m_available; }
        void OpenAvailable();
        void Draw();

    private:
        enum class Phase
        {
            Hidden,
            Offer,
            Downloading,
            Installed,
            Failed,
        };

        void Check(bool manual);
        void Show(const Updates::Release& release, bool skippable);
        void Install();
        void Close();
        void DrawNotes(const std::string& notes);
        void DrawButtons();

        Engine& m_engine;
        Restart m_restart;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        std::optional<Updates::Release> m_available;
        std::optional<Updates::Release> m_shown;
        std::optional<std::string> m_blocker;
        Phase m_phase = Phase::Hidden;
        bool m_skippable = false;
        bool m_checking = false;
        bool m_pendingOpen = false;
        int m_work = 0;
        std::string m_error;
        std::string m_result;
        std::string m_installed;   // the version this run already put in place
    };
}
