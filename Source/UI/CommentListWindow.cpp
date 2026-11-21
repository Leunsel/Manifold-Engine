#include "UI/CommentListWindow.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kUndoDepth = 50;

        std::string CountText(std::size_t count, const char* one, const char* many)
        {
            return std::to_string(count) + " " + (count == 1 ? one : many);
        }
    }

    std::vector<std::string> CommentListWindow::SelectedIn(const std::vector<std::string>& keys) const
    {
        std::vector<std::string> selected;
        for (const std::string& key : keys)
        {
            if (m_selected.count(key))
                selected.push_back(key);
        }
        return selected;
    }

    void CommentListWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_COMMENTS "  Comment List###commentlist", ImVec2(760.0f, 420.0f), ImGuiWindowFlags_MenuBar, ImVec2(440.0f, 220.0f)))
            return;
        Engine& engine = m_workspace.Core();
        Table::AddressList& table = engine.Table();
        // ∑ Undo puts notes back into the table they came from; after New, Open or a restore that table is gone.
        if (table.Identity() != m_tableIdentity)
        {
            m_tableIdentity = table.Identity();
            m_undo.clear();
            m_selected.clear();
            m_anchor.clear();
        }
        const std::vector<std::string> keys = Table::SortedNoteKeys(table, m_filter);
        const std::vector<std::string> selected = SelectedIn(keys);
        std::size_t total = 0;
        for (const auto& [key, note] : table.DisasmNotes)
        {
            if (!note.Comment.empty() || !note.Header.empty())
                ++total;
        }

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Comments"))
            {
                if (ImGui::MenuItem(ICON_FA_CODE "  Go to in the Disassembler", "Enter", false, selected.size() == 1))
                    Reveal(selected.front());
                if (ImGui::MenuItem(ICON_FA_EDIT "  Edit Comment...", nullptr, false, selected.size() == 1))
                {
                    m_editKey = selected.front();
                    m_editHeader = false;
                    m_editOpen = true;
                }
                if (ImGui::MenuItem(ICON_FA_HEADING "  Edit Header...", nullptr, false, selected.size() == 1))
                {
                    m_editKey = selected.front();
                    m_editHeader = true;
                    m_editOpen = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_CHECK_DOUBLE "  Select All", "Ctrl+A", false, !keys.empty()))
                    m_selected.insert(keys.begin(), keys.end());
                if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, !selected.empty()))
                {
                    std::string text;
                    for (const std::string& key : selected)
                    {
                        const Table::AddressList::DisasmNote& note = table.DisasmNotes[key];
                        text += key + "\t" + note.Comment + "\t" + note.Header + "\n";
                    }
                    ImGui::SetClipboardText(text.c_str());
                }
                ItemTooltip("Address, comment and header of each selected entry, separated by tabs.");
                ImGui::Separator();
                const std::string undo = ICON_FA_UNDO_ALT "  Undo Remove" + (m_undo.empty() ? std::string() : " (" + std::to_string(m_undo.back().size()) + ")");
                if (ImGui::MenuItem(undo.c_str(), "Ctrl+Z", false, !m_undo.empty()))
                    Undo();
                if (DangerMenuItem(ICON_FA_TRASH "  Remove", "Delete", !selected.empty()))
                    Remove(selected);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        ImGui::SetNextItemWidth(std::min(Px(320.0f), ImGui::GetContentRegionAvail().x));
        InputTextWithHint("##filter", ICON_FA_SEARCH "  Filter by address, comment or header", m_filter);
        ImGui::SameLine();
        std::string summary = keys.size() == total ? CountText(total, "comment", "comments") + " in this table"
                                                   : Format("%zu of %zu shown", keys.size(), total);
        if (!selected.empty())
            summary += ", " + std::to_string(selected.size()) + " selected";
        Muted(summary);
        if (!m_undo.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_UNDO_ALT "  Undo Remove"))
                Undo();
            ItemTooltip("Puts the last removed comments back (Ctrl+Z).");
        }

        if (total == 0)
        {
            EmptyState(ICON_FA_COMMENTS, "No comments yet",
                "Comments you put on lines of the disassembler (Ctrl+Shift+C in the memory viewer) show up here. They are saved with the table.", {}, false);
        }
        else if (keys.empty())
        {
            Muted("Nothing matches the filter.");
        }
        else if (BeginDataTable("##comments", { { "Address", 1.3f }, { "Now At", 0.9f }, { "Comment", 2.2f }, { "Header", 1.2f } }))
        {
            ClippedRows(static_cast<int>(keys.size()), [&](int index) { Row(keys[static_cast<std::size_t>(index)], keys, index); });
            ImGui::EndTable();
        }
        Keys(keys);
        EditDialog();
        EndToolWindow();
    }

    void CommentListWindow::Row(const std::string& key, const std::vector<std::string>& keys, int index)
    {
        Engine& engine = m_workspace.Core();
        const Table::AddressList::DisasmNote& note = engine.Table().DisasmNotes[key];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        PushMono();
        const bool isSelected = m_selected.count(key) > 0;
        if (ImGui::Selectable(("##row" + key).c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
            Click(keys, index);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            Reveal(key);
        if (ImGui::BeginPopupContextItem("##commentmenu"))
        {
            if (!m_selected.count(key))
            {
                m_selected = { key };
                m_anchor = key;
            }
            RowMenu(key);
            ImGui::EndPopup();
        }
        ImGui::SameLine(0.0f, 0.0f);
        TextFitted(key, ImGui::GetContentRegionAvail().x, ManifoldTheme::AddressText);
        ImGui::TableNextColumn();
        if (engine.HasProcess())
        {
            if (const auto address = engine.Evaluate(key))
                TextFitted(Hex(*address, engine.Process()->Is64Bit() ? 12 : 8), ImGui::GetContentRegionAvail().x, ManifoldTheme::Muted);
            else
                ImGui::TextDisabled("not loaded");
        }
        else
            ImGui::TextDisabled("-");
        PopMono();
        ImGui::TableNextColumn();
        TextFitted(note.Comment, ImGui::GetContentRegionAvail().x, ManifoldTheme::CodeUserComment);
        if (!note.Comment.empty() && ImGui::IsItemHovered() && ImGui::CalcTextSize(note.Comment.c_str()).x > ImGui::GetColumnWidth())
            ShowTooltip([&]() { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f); ImGui::TextUnformatted(note.Comment.c_str()); ImGui::PopTextWrapPos(); });
        ImGui::TableNextColumn();
        TextFitted(note.Header, ImGui::GetContentRegionAvail().x, ManifoldTheme::Accent);
    }

    void CommentListWindow::RowMenu(const std::string& key)
    {
        Engine& engine = m_workspace.Core();
        const std::vector<std::string> selected(m_selected.begin(), m_selected.end());
        if (ImGui::MenuItem(ICON_FA_CODE "  Go to in the Disassembler", "Enter", false, selected.size() == 1))
            Reveal(key);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_EDIT "  Edit Comment..."))
        {
            m_editKey = key;
            m_editHeader = false;
            m_editOpen = true;
        }
        if (ImGui::MenuItem(ICON_FA_HEADING "  Edit Header..."))
        {
            m_editKey = key;
            m_editHeader = true;
            m_editOpen = true;
        }
        ItemTooltip("A header is a title above the line, like Cheat Engine's. It is saved with the table.");
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Address"))
            ImGui::SetClipboardText(key.c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Comment", nullptr, false, !engine.Table().DisasmNotes[key].Comment.empty()))
            ImGui::SetClipboardText(engine.Table().DisasmNotes[key].Comment.c_str());
        ImGui::Separator();
        const std::string remove = selected.size() > 1 ? ICON_FA_TRASH "  Remove " + std::to_string(selected.size()) + " Comments" : std::string(ICON_FA_TRASH "  Remove");
        if (DangerMenuItem(remove.c_str(), "Delete"))
            Remove(selected);
    }

    void CommentListWindow::Click(const std::vector<std::string>& keys, int index)
    {
        const std::string& key = keys[static_cast<std::size_t>(index)];
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && !m_anchor.empty())
        {
            const auto anchor = std::find(keys.begin(), keys.end(), m_anchor);
            if (anchor != keys.end())
            {
                const int from = static_cast<int>(anchor - keys.begin());
                if (!io.KeyCtrl)
                    m_selected.clear();
                for (int i = std::min(from, index); i <= std::max(from, index); ++i)
                    m_selected.insert(keys[static_cast<std::size_t>(i)]);
                return;
            }
        }
        if (io.KeyCtrl)
        {
            if (!m_selected.erase(key))
                m_selected.insert(key);
        }
        else
            m_selected = { key };
        m_anchor = key;
    }

    void CommentListWindow::Reveal(const std::string& key)
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess())
        {
            m_workspace.Report(Status::Fail(ErrorCode::NoProcess, "the comment is at " + key));
            return;
        }
        Status error;
        if (const auto address = engine.Evaluate(key, &error))
            m_workspace.RevealInMemoryViewer(*address, 1, true, true);
        else
            m_workspace.Report(error);
    }

    void CommentListWindow::Remove(std::vector<std::string> keys)
    {
        Engine& engine = m_workspace.Core();
        std::vector<Table::RemovedNote> removed = Table::RemoveNotes(engine.Table(), keys);
        if (removed.empty())
            return;
        for (const Table::RemovedNote& entry : removed)
            m_selected.erase(entry.Key);
        engine.Notify(LogLevel::Info, "Removed " + CountText(removed.size(), "comment", "comments") + "; Ctrl+Z in the comment list puts " +
            (removed.size() == 1 ? "it" : "them") + " back");
        m_undo.push_back(std::move(removed));
        if (m_undo.size() > kUndoDepth)
            m_undo.erase(m_undo.begin());
    }

    void CommentListWindow::Undo()
    {
        if (m_undo.empty())
            return;
        Engine& engine = m_workspace.Core();
        const std::vector<Table::RemovedNote> last = std::move(m_undo.back());
        m_undo.pop_back();
        const std::size_t restored = Table::RestoreNotes(engine.Table(), last);
        m_selected.clear();
        for (const Table::RemovedNote& entry : last)
            m_selected.insert(entry.Key);
        if (restored < last.size())
            engine.Notify(LogLevel::Warning, Format("Put back %zu of %zu comments; the others have new comments since", restored, last.size()));
    }

    void CommentListWindow::Keys(const std::vector<std::string>& keys)
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
            return;
        const ImGuiIO& io = ImGui::GetIO();
        const std::vector<std::string> selected = SelectedIn(keys);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selected.empty())
            Remove(selected);
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            Undo();
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            m_selected.insert(keys.begin(), keys.end());
        else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !selected.empty())
        {
            std::string text;
            for (const std::string& key : selected)
            {
                const Table::AddressList::DisasmNote& note = m_workspace.Core().Table().DisasmNotes[key];
                text += key + "\t" + note.Comment + "\t" + note.Header + "\n";
            }
            ImGui::SetClipboardText(text.c_str());
        }
        else if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) && selected.size() == 1)
            Reveal(selected.front());
    }

    void CommentListWindow::EditDialog()
    {
        Engine& engine = m_workspace.Core();
        if (m_editOpen && !ImGui::IsPopupOpen("##editnote"))
        {
            const auto found = engine.Table().DisasmNotes.find(m_editKey);
            m_editText = found == engine.Table().DisasmNotes.end() ? std::string() : (m_editHeader ? found->second.Header : found->second.Comment);
            ImGui::OpenPopup("##editnote");
        }
        if (!ImGui::BeginPopup("##editnote"))
        {
            m_editOpen = false;
            return;
        }
        ImGui::TextUnformatted(m_editHeader ? "Header above" : "Comment at");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_editKey.c_str());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(Px(360.0f));
        const bool enter = InputText("##notetext", m_editText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const DialogChoice choice = DialogButtons("Save", true, false);
        if (enter || choice == DialogChoice::Ok)
        {
            Table::AddressList& table = engine.Table();
            Table::AddressList::DisasmNote& note = table.DisasmNotes[m_editKey];
            (m_editHeader ? note.Header : note.Comment) = Trim(m_editText);
            if (note.Comment.empty() && note.Header.empty())
                table.DisasmNotes.erase(m_editKey);
            table.Touch();
            m_editOpen = false;
            ImGui::CloseCurrentPopup();
        }
        else if (choice == DialogChoice::Cancel)
            m_editOpen = false;
        ImGui::EndPopup();
    }
}
