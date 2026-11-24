#pragma once

#include "App/Themes.h"
#include "UI/ToolWindowBase.h"
#include "UI/Widgets.h"

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
        static constexpr int PageCount = 7;

        static const char* PageIcon(int page);
        static const char* PageTitle(int page);
        static const char* PageSubtitle(int page);

        explicit SettingsWindow(Workspace& workspace);
        void Draw() override;
        void Show(int page = -1)
        {
            m_open = true;
            m_bringToFront = true;
            if (page >= 0 && page < PageCount)
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
        bool ThemeGallery(SettingsList& list, Settings& settings);
        void ThemeCardMenu(const Themes::CustomTheme& theme, Settings& settings);
        void ThemeNameDialog(Settings& settings);
        void OpenThemeName(int action, const std::string& source, const std::string& name);
        void SelectTheme(Settings& settings, const std::string& custom, ThemeFamily family);
        void FinishThemeEdit(Settings& settings);
        void ImportTheme(Settings& settings);

        Workspace& m_workspace;
        int m_page = 0;
        bool m_scrollTop = false;
        bool m_allImGuiColors = false;
        bool m_themeReset = false;
        bool m_themeColorsEdited = false;  // colours or sizes changed this frame
        Dialog m_themeNameDialog;
        int m_themeNaming = 0;             // ThemeNaming in the .cpp
        std::string m_themeNameSource;     // the custom theme's id the dialog works on
        std::string m_themeName;
        std::string m_themeNotice;
        double m_themesScannedAt = -100.0;
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
