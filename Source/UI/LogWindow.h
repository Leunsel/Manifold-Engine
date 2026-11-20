#pragma once

#include "Core/RuntimeLog.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Manifold::UI
{
    class LogWindow : public ToolWindowBase
    {
    public:
        explicit LogWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        struct Row
        {
            std::size_t First = 0;
            std::size_t Last = 0;
            int Repeat = 1;
            float Top = 0.0f;
            float Height = 0.0f;
        };

        void MenuBar();
        void Toolbar();
        void AreaMenu();
        void Refresh();
        void Rebuild();
        void Layout(float width);
        void Table(float height);
        void RowMenu(std::size_t row);
        void Details();
        void ErrorsTab();
        void Select(std::size_t row, bool toggle, bool range);
        void SelectAll();
        void Copy(const std::vector<std::size_t>& rows) const;
        std::vector<std::size_t> SelectedRows() const;
        std::string LineText(const Row& row) const;
        const Row* CurrentRow() const;

        Workspace& m_workspace;
        int m_level = 1;
        std::string m_search;
        std::string m_appliedSearch = "\x01";
        int m_appliedLevel = -1;
        bool m_follow = true;
        bool m_wrap = true;
        bool m_group = true;
        bool m_details = true;
        bool m_dirty = true;
        bool m_focusSearch = false;
        bool m_logTab = true;
        std::uint64_t m_generation = 0;
        std::vector<LogEntry> m_entries;
        std::vector<Row> m_rows;
        std::vector<std::pair<std::string, int>> m_areas;
        std::set<std::string> m_hiddenAreas;
        std::set<std::uint64_t> m_selected;
        std::uint64_t m_current = 0;
        std::uint64_t m_anchor = 0;
        int m_warnings = 0;
        int m_errors = 0;
        float m_layoutWidth = -1.0f;
        float m_messageWidth = 0.0f;
        bool m_layoutWrap = false;
        float m_total = 0.0f;
        float m_detailHeight = 170.0f;
        std::uint64_t m_detailSerial = 0;
        std::string m_detailText;
    };
}
