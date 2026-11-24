#include "UI/TableCleanupWindow.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Table/CheatTable.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include <algorithm>
#include <filesystem>

namespace Manifold::UI
{
    namespace
    {
        const char* KindIcon(Table::CleanupKind kind)
        {
            switch (kind)
            {
            case Table::CleanupKind::Structure: return ICON_FA_SITEMAP;
            case Table::CleanupKind::Symbol: return ICON_FA_TAG;
            case Table::CleanupKind::File: return ICON_FA_FILE;
            case Table::CleanupKind::EmptyGroup: return ICON_FA_FOLDER_OPEN;
            case Table::CleanupKind::DisasmComments: return ICON_FA_COMMENT;
            case Table::CleanupKind::TableComment: return ICON_FA_COMMENT_ALT;
            case Table::CleanupKind::ScriptComments: return ICON_FA_CODE;
            case Table::CleanupKind::FileComments: return ICON_FA_FILE_CODE;
            }
            return "";
        }
    }

    TableCleanupWindow::TableCleanupWindow(Workspace& workspace) : m_workspace(workspace)
    {
        Look();
    }

    // ∑ The open table is written to text the way Save writes it and read back:
    //   the plan and the copy work on that, never on the live records.
    void TableCleanupWindow::Look()
    {
        Engine& engine = m_workspace.Core();
        m_error.clear();
        m_original = Table::WriteTable(engine.Table(), engine.Structures(), engine.TableSymbols(), engine.TableFileNaming());
        Table::LoadedTable loaded;
        if (Status parsed = Table::ParseTable(m_original, loaded); !parsed)
        {
            m_plan = {};
            m_error = parsed.Text;
            return;
        }
        m_plan = Table::PlanCleanup(loaded, m_original.size());
        m_chosen.clear();
        for (const Table::CleanupItem& item : m_plan.Items)
        {
            if (item.Recommended)
                m_chosen.insert(item.Key);
        }
        m_lookedAt = engine.Table().Changes();
        m_measured = false;
    }

    std::vector<Table::CleanupItem> TableCleanupWindow::Chosen() const
    {
        std::vector<Table::CleanupItem> chosen;
        for (const Table::CleanupItem& item : m_plan.Items)
        {
            if (m_chosen.count(item.Key))
                chosen.push_back(item);
        }
        return chosen;
    }

    void TableCleanupWindow::Measure()
    {
        if (m_measured)
            return;
        m_measured = true;
        const Result<std::string> cleaned = Table::CleanTableText(m_original, Chosen());
        m_cleanedBytes = cleaned ? cleaned->size() : m_original.size();
        if (!cleaned)
            m_error = cleaned.Error().Text;
    }

    void TableCleanupWindow::SaveCopy()
    {
        Engine& engine = m_workspace.Core();
        const Result<std::string> cleaned = Table::CleanTableText(m_original, Chosen());
        if (!cleaned)
        {
            m_error = cleaned.Error().Text;
            return;
        }
        const std::filesystem::path current = Utf8ToWide(engine.Table().FilePath);
        const std::string stem = current.empty() ? std::string("table") : WideToUtf8(current.stem().wstring());
        const auto path = m_workspace.SaveFileDialog({ { L"Cheat tables (*.CT)", L"*.CT" } }, stem + "_release.CT", L"CT");
        if (!path)
            return;
        std::error_code error;
        if (!current.empty() && std::filesystem::equivalent(*path, current, error))
        {
            m_error = "That is the open table's own file. Choose another name: the cleanup writes a copy and leaves the table as it is.";
            return;
        }
        if (!Table::WriteFileSafely(*path, *cleaned))
        {
            m_error = "Could not write " + WideToUtf8(path->wstring()) + ".";
            return;
        }
        m_error.clear();
        const std::size_t before = m_original.size();
        engine.Notify(LogLevel::Success, Format("Saved the cleaned copy %s: %s instead of %s (%zu of %zu items left out). The open table is unchanged.",
                                             WideToUtf8(path->filename().wstring()).c_str(), ByteSize(cleaned->size()).c_str(), ByteSize(before).c_str(), m_chosen.size(),
                                             m_plan.Items.size()));
    }

    void TableCleanupWindow::Summary()
    {
        Measure();
        const std::size_t before = m_plan.OriginalBytes;
        const std::size_t after = m_cleanedBytes;
        const double saved = before ? 100.0 * static_cast<double>(before - std::min(before, after)) / static_cast<double>(before) : 0.0;
        Layout::Heading(Format("%s  " ICON_FA_LONG_ARROW_ALT_RIGHT "  %s", ByteSize(before).c_str(), ByteSize(after).c_str()));
        Layout::Muted(Format("%zu of %zu items chosen, %.0f %% smaller. The copy is a new file; the open table and its file stay as they are.", m_chosen.size(),
            m_plan.Items.size(), saved), Layout::Overflow::Wrap);
        if (m_lookedAt != m_workspace.Core().Table().Changes())
            Layout::Colored(ICON_FA_EXCLAMATION_TRIANGLE "  The table changed since this list was made. Look Again before saving.", ManifoldTheme::Warning, Layout::Overflow::Wrap);
        if (!m_error.empty())
            Layout::Colored(m_error, ManifoldTheme::Error, Layout::Overflow::Wrap);
    }

    void TableCleanupWindow::List()
    {
        Layout::Row tools("##cleanuptools", { Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fit(), Layout::Size::Fill(), Layout::Size::Fit() }, Layout::Toolbar());
        tools.Cell([&] {
            if (Layout::Button("Recommended##cleanuprec", Layout::ButtonKind::Ghost))
            {
                m_chosen.clear();
                for (const Table::CleanupItem& item : m_plan.Items)
                {
                    if (item.Recommended)
                        m_chosen.insert(item.Key);
                }
                m_measured = false;
            }
        });
        tools.Cell([&] {
            if (Layout::Button("All##cleanupall", Layout::ButtonKind::Ghost))
            {
                for (const Table::CleanupItem& item : m_plan.Items)
                    m_chosen.insert(item.Key);
                m_measured = false;
            }
        });
        tools.Cell([&] {
            if (Layout::Button("None##cleanupnone", Layout::ButtonKind::Ghost))
            {
                m_chosen.clear();
                m_measured = false;
            }
        });
        tools.Skip();
        tools.Cell([&] {
            Layout::ButtonStyle style;
            style.Kind = Layout::ButtonKind::Ghost;
            style.Tooltip = "Reads the table again, with what changed since.";
            if (Layout::Button(ICON_FA_SYNC "  Look Again##cleanuplook", style))
                Look();
        });
        tools.End();

        if (!BeginDataTable("##cleanupitems", { { "", 0.25f }, { "Item", 2.2f }, { "Size", 0.6f }, { "Why", 3.4f } }, ImVec2(0.0f, -Px(46.0f))))
            return;
        Table::CleanupKind shownKind = static_cast<Table::CleanupKind>(-1);
        for (std::size_t i = 0; i < m_plan.Items.size(); ++i)
        {
            const Table::CleanupItem& item = m_plan.Items[i];
            ImGui::PushID(static_cast<int>(i));
            if (item.Kind != shownKind)
            {
                shownKind = item.Kind;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                Layout::Text(std::string(KindIcon(item.Kind)) + "  " + Table::CleanupKindName(item.Kind), { .Color = ManifoldTheme::Label });
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = m_chosen.count(item.Key) > 0;
            if (ImGui::Checkbox("##cleanupitem", &on))
            {
                if (on)
                    m_chosen.insert(item.Key);
                else
                    m_chosen.erase(item.Key);
                m_measured = false;
            }
            ImGui::TableNextColumn();
            Layout::Text(item.Name, { .Font = item.Kind == Table::CleanupKind::Symbol || item.Kind == Table::CleanupKind::File ? Fonts::Mono() : nullptr });
            ImGui::TableNextColumn();
            Layout::Muted(ByteSize(item.Bytes));
            ImGui::TableNextColumn();
            Layout::Muted(item.Reason);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                Layout::TextTooltip(item.Reason);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void TableCleanupWindow::Footer()
    {
        Layout::Row bar("##cleanupsave", { Layout::Size::Fill(), Layout::Size::Fit() }, Layout::Toolbar());
        bar.Cell([&] { Layout::Muted("Check a structure or file you load by a name built at run time before you leave it out.", Layout::Overflow::Ellipsis); });
        bar.Cell([&] {
            Layout::ButtonStyle style;
            style.Kind = Layout::ButtonKind::Accent;
            if (Layout::Button(ICON_FA_SAVE "  Save Cleaned Copy...##cleanupsavebutton", style))
                SaveCopy();
        });
        bar.End();
    }

    void TableCleanupWindow::Draw()
    {
        if (!m_open)
            return;
        if (!BeginWindow(ICON_FA_BROOM "  Clean Up for Release###tablecleanup", ImVec2(860.0f, 560.0f), 0, ImVec2(520.0f, 320.0f)))
            return;
        if (m_plan.Items.empty())
        {
            if (!m_error.empty())
                Layout::Colored(m_error, ManifoldTheme::Error, Layout::Overflow::Wrap);
            const int clicked = EmptyState(ICON_FA_BROOM, "Nothing to clean up", "Every structure, symbol, table file and group of this table is used by something, and it has no comments to leave out.",
                { { ICON_FA_SYNC "  Look Again" } }, false);
            if (clicked == 0)
                Look();
            EndToolWindow();
            return;
        }
        Summary();
        Layout::Spacing(Layout::Space::S);
        List();
        Footer();
        EndToolWindow();
    }
}
