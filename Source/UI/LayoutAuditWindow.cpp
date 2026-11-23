#include "UI/LayoutAuditWindow.h"

#include "App/Engine.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "UI/Widgets.h"

#include <cstdio>
#include <string>

namespace Manifold::UI
{
    LayoutAuditWindow::LayoutAuditWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void LayoutAuditWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_RULER_COMBINED "  Layout Problems###layoutproblems", ImVec2(720.0f, 420.0f), 0, ImVec2(420.0f, 220.0f)))
            return;
        Settings& settings = m_workspace.Core().Options();
        const std::vector<Layout::Audit::Problem> problems = Layout::Audit::Problems();
        {
            Layout::Row bar("##bar", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fill(), Layout::Size::Fit() }, Layout::Toolbar());
            bar.Cell([&] {
                if (ToggleSwitch("##checks", &settings.LayoutChecks))
                {
                    Layout::Audit::SetEnabled(settings.LayoutChecks);
                    m_workspace.Core().SaveOptionsSoon();
                }
            });
            bar.Cell([&] { Layout::Text("Layout Checks"); });
            bar.Cell([&] {
                bool overlay = Layout::Audit::Overlay();
                if (ToggleSwitch("##overlay", &overlay))
                    Layout::Audit::SetOverlay(overlay);
            });
            bar.Cell([&] { Layout::Text("Outline on Screen"); });
            bar.Cell([&] {
                Layout::Muted(Layout::Audit::Enabled() ? std::to_string(problems.size()) + (problems.size() == 1 ? " problem" : " problems") + " in the last frames"
                                                       : "Checks are off");
            });
            bar.Cell([&] {
                if (Layout::Button(ICON_FA_ERASER "  Clear"))
                    Layout::Audit::Clear();
            });
        }
        ImGui::Spacing();
        if (!Layout::Audit::Enabled() || problems.empty())
        {
            EmptyState(ICON_FA_RULER_COMBINED, Layout::Audit::Enabled() ? "No Layout Problems" : "Layout Checks Are Off",
                Layout::Audit::Enabled() ? "Nothing left its place in the last frames. Resize windows to look for more." :
                                           "Turn them on to find content that is cut off, leaves its cell or does not fit its row.",
                {}, false);
            EndToolWindow();
            return;
        }
        if (BeginDataTable("##problems", { { "Window", 1.2f }, { "Problem", 1.0f }, { "Detail", 2.0f }, { "Frames", 0.0f, 64.0f } }))
        {
            ClippedRows(static_cast<int>(problems.size()), [&](int index) {
                const Layout::Audit::Problem& problem = problems[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::size_t hidden = problem.WindowName.find("###");
                const std::string name = hidden == std::string::npos ? problem.WindowName : problem.WindowName.substr(0, hidden);
                Layout::RowSelectable("##row", false);
                if (ImGui::IsItemHovered())
                    Layout::Audit::Highlight(problem.Window, problem.Rect);
                Layout::Text(name);
                ImGui::TableNextColumn();
                Layout::Colored(Layout::Audit::KindName(problem.What), problem.What == Layout::Audit::Kind::WindowClipped || problem.What == Layout::Audit::Kind::ColumnClipped ? ManifoldTheme::Warning : ManifoldTheme::Error);
                ImGui::TableNextColumn();
                Layout::Muted(problem.Detail);
                ImGui::TableNextColumn();
                Layout::Text(std::to_string(problem.Seen));
            });
            ImGui::EndTable();
        }
        EndToolWindow();
    }
}
