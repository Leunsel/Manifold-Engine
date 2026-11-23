#pragma once

#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Manifold::UI
{
    class SettingsList;

    class SettingsWindow : public ToolWindowBase
    {
    public:
        static constexpr int ThemePage = 1;
        static constexpr int SignaturesPage = 5;
        static constexpr int KeyboardPage = 6;

        explicit SettingsWindow(Workspace& workspace);
        void Draw() override;
        void Show(int page = -1)
        {
            m_open = true;
            m_bringToFront = true;
            if (page >= 0)
            {
                m_page = page;
                m_search.clear();
                m_scrollTop = true;
            }
        }

    private:
        void Navigation(float width, float height);
        void PageHeader(Settings& settings);
        bool GeneralPage(SettingsList& list, Settings& settings);
        bool AppearancePage(SettingsList& list, Settings& settings);
        bool ScannerPage(SettingsList& list, Settings& settings);
        bool ScriptsPage(SettingsList& list, Settings& settings);
        bool DebuggerPage(SettingsList& list, Settings& settings);
        bool SignatureOptionsPage(SettingsList& list, Settings& settings);
        bool ShortcutsPage(SettingsList& list, Settings& settings);

        Workspace& m_workspace;
        int m_page = 0;
        bool m_scrollTop = false;
        bool m_allImGuiColors = false;
        bool m_themeReset = false;
        bool m_shortcutsReset = false;
        int m_captureCommand = -1;
        int m_captureSlot = 0;
        std::string m_search;
        std::string m_autoAttach;
        std::string m_saveError;
        std::optional<std::uint64_t> m_seenSaves;
        double m_savedAt = -100.0;
    };
}
