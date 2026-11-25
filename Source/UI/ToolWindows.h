#pragma once

#include "UI/ProcessIcon.h"
#include "UI/RebaseMenu.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class ProcessPicker : public ToolWindow
    {
    public:
        explicit ProcessPicker(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show();

    private:
        void Refresh();

        Workspace& m_workspace;
        bool m_open = true;
        std::vector<Target::ProcessEntry> m_list;
        std::string m_filter;
        bool m_windowsOnly = false;
        std::uint32_t m_selected = 0;
        std::string m_selectedRow;
        bool m_focusFilter = true;
        bool m_scrollToSelected = false;
        std::string m_lastFilter = "\x01";
        bool m_lastMode = false;
        std::set<std::string> m_expanded;
        IconCache m_icons;
    };

    class SymbolsWindow : public ToolWindowBase
    {
    public:
        explicit SymbolsWindow(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        Workspace& m_workspace;
        std::string m_name;
        std::string m_address;
        std::string m_error;
        std::string m_filter;
        [[maybe_unused]] int m_tab = 0;
        std::string m_module;
        RebasePrompt m_rebase;
    };

    class RegionsWindow : public ToolWindowBase
    {
    public:
        explicit RegionsWindow(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; m_dirty = true; }

    private:
        Workspace& m_workspace;
        bool m_dirty = true;
        bool m_committedOnly = true;
        std::string m_filter;
        std::vector<Target::RegionInfo> m_regions;
    };
}
