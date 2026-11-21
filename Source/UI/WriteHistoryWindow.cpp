#include "UI/WriteHistoryWindow.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <ctime>
#include <vector>

namespace Manifold::UI
{
    namespace
    {
        std::string TimeText(std::chrono::system_clock::time_point at)
        {
            const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
            std::tm local{};
            localtime_s(&local, &seconds);
            char text[16] = "";
            std::strftime(text, sizeof(text), "%H:%M:%S", &local);
            return text;
        }

        std::string BytesPreview(const std::vector<std::uint8_t>& bytes)
        {
            constexpr std::size_t kShown = 12;
            if (bytes.size() <= kShown)
                return HexBytes(bytes);
            return HexBytes(std::span<const std::uint8_t>(bytes.data(), kShown)) + " ... (" + std::to_string(bytes.size()) + " bytes)";
        }

        const char* StateText(WriteState state)
        {
            switch (state)
            {
            case WriteState::InMemory: return "in memory";
            case WriteState::Undone: return "undone";
            case WriteState::ChangedSince: return "changed since";
            default: return "unreadable";
            }
        }

        ImVec4 StateColor(WriteState state)
        {
            switch (state)
            {
            case WriteState::InMemory: return ManifoldTheme::Success;
            case WriteState::Undone: return ManifoldTheme::Muted;
            case WriteState::ChangedSince: return ManifoldTheme::Warning;
            default: return ManifoldTheme::Error;
            }
        }

        bool CodeEdit(const MemoryWrite& write)
        {
            return write.Source == "Assemble" || write.Source == "NOP" || write.Source == "Restore";
        }
    }

    WriteState WriteHistoryWindow::StateOf(const MemoryWrite& write)
    {
        Engine& engine = m_workspace.Core();
        KnownState& known = m_states[write.Id];
        const double now = ImGui::GetTime();
        const double interval = 0.5 + static_cast<double>(write.Written.size()) / (8.0 * 1024.0 * 1024.0);
        if (known.At < 0.0 || now - known.At >= interval)
        {
            known.State = engine.Writes().StateOf(*engine.Process(), write);
            known.At = now;
        }
        return known.State;
    }

    void WriteHistoryWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_HISTORY "  Write History###writehistory", ImVec2(820.0f, 420.0f), ImGuiWindowFlags_MenuBar, ImVec2(520.0f, 240.0f)))
            return;
        Engine& engine = m_workspace.Core();
        const WriteHistory& history = engine.Writes();
        const std::uint64_t generation = engine.ProcessGeneration();
        std::vector<const MemoryWrite*> rows;
        if (engine.HasProcess())
        {
            for (auto it = history.Entries().rbegin(); it != history.Entries().rend(); ++it)
            {
                if (it->Generation == generation)
                    rows.push_back(&*it);
            }
        }
        const std::size_t undoable = engine.HasProcess() ? history.Undoable(generation) : 0;
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Edits"))
            {
                if (ImGui::MenuItem(ICON_FA_UNDO_ALT "  Undo All", nullptr, false, undoable > 0))
                    UndoAll();
                ItemTooltip("Writes back the bytes from before each edit, newest first.");
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Clear the List", nullptr, false, !rows.empty()))
                {
                    engine.Writes().Clear(generation);
                    rows.clear();
                }
                ItemTooltip("Forgets the edits; the memory stays as it is.");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (!engine.HasProcess())
        {
            m_workspace.NoProcessState(ICON_FA_HISTORY, "Open a process to see the memory edits made in it.");
            EndToolWindow();
            return;
        }

        if (history.Revision() != m_statesRevision)
        {
            m_states.clear();
            m_statesRevision = history.Revision();
        }
        const std::size_t dropped = history.Dropped(generation);
        std::string summary = rows.empty() ? std::string("No edits in this process yet")
                                           : Format("%zu %s in this process, %zu not undone", rows.size(), rows.size() == 1 ? "edit" : "edits", undoable);
        if (dropped > 0)
            summary += Format(", %zu older %s left the list and cannot be undone here", dropped, dropped == 1 ? "one" : "ones");
        Muted(summary);

        if (rows.empty())
        {
            EmptyState(ICON_FA_HISTORY, "No memory edits yet",
                "Edits made in the memory viewer show up here: typing in the hex view, assembling, NOP, paste, fill and loading a file. Each one can be undone and redone.",
                {}, false);
            EndToolWindow();
            return;
        }
        if (BeginDataTable("##writes", { { "Time", 1.0f, 64.0f }, { "Address", 1.6f }, { "Source", 1.0f, 86.0f }, { "Before", 1.0f }, { "After", 1.0f }, { "State", 1.0f, 100.0f }, { "", 1.0f, 64.0f } }))
        {
            ClippedRows(static_cast<int>(rows.size()), [&](int index)
                {
                    const MemoryWrite& write = *rows[static_cast<std::size_t>(index)];
                    Row(write, StateOf(write), history.BlockedBy(write).has_value());
                });
            ImGui::EndTable();
        }
        EndToolWindow();
    }

    void WriteHistoryWindow::Row(const MemoryWrite& write, WriteState state, bool blocked)
    {
        Engine& engine = m_workspace.Core();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        PushMono();
        const std::string time = TimeText(write.At);
        if (ImGui::Selectable(time.c_str(), m_selected == write.Id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            m_selected = write.Id;
        PopMono();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            Reveal(write, CodeEdit(write));
        if (ImGui::BeginPopupContextItem("##writemenu"))
        {
            m_selected = write.Id;
            RowMenu(write, blocked);
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        TextFitted(engine.Describe(write.Address), ImGui::GetContentRegionAvail().x, ManifoldTheme::AddressText);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(write.Source.c_str());
        PushMono();
        ImGui::TableNextColumn();
        TextFitted(BytesPreview(write.Original), ImGui::GetContentRegionAvail().x, ManifoldTheme::Muted);
        ImGui::TableNextColumn();
        TextFitted(BytesPreview(write.Written), ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
        PopMono();
        ImGui::TableNextColumn();
        ImGui::TextColored(StateColor(state), "%s", StateText(state));
        if (state == WriteState::ChangedSince)
            ItemTooltip("The game or a script wrote to these bytes after the edit. Undo still writes back the bytes from before it.");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(blocked);
        if (ImGui::SmallButton(write.Undone ? "Redo" : "Undo"))
            Toggle(write);
        ImGui::EndDisabled();
        if (blocked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Another edit overlaps these bytes and comes first.");
    }

    void WriteHistoryWindow::RowMenu(const MemoryWrite& write, bool blocked)
    {
        Engine& engine = m_workspace.Core();
        if (ImGui::MenuItem(write.Undone ? ICON_FA_REDO "  Redo" : ICON_FA_UNDO_ALT "  Undo", nullptr, false, !blocked))
            Toggle(write);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CODE "  Show in the Disassembler"))
            Reveal(write, true);
        if (ImGui::MenuItem(ICON_FA_TH "  Show in the Hex View"))
            Reveal(write, false);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Address", engine.Describe(write.Address).c_str()))
            ImGui::SetClipboardText(engine.Describe(write.Address).c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Bytes Before"))
            ImGui::SetClipboardText(HexBytes(write.Original).c_str());
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Bytes After"))
            ImGui::SetClipboardText(HexBytes(write.Written).c_str());
    }

    void WriteHistoryWindow::Toggle(const MemoryWrite& write)
    {
        Engine& engine = m_workspace.Core();
        const int id = write.Id;
        const bool redo = write.Undone;
        const std::uint64_t address = write.Address;
        const Status status = redo ? engine.Writes().Redo(*engine.Process(), engine.ProcessGeneration(), id)
                                   : engine.Writes().Undo(*engine.Process(), engine.ProcessGeneration(), id);
        if (status)
            engine.Notify(LogLevel::Info, std::string(redo ? "Wrote the edit at " : "Restored the bytes at ") + engine.Describe(address) + " again");
        m_workspace.Report(status);
    }

    void WriteHistoryWindow::UndoAll()
    {
        Engine& engine = m_workspace.Core();
        int restored = 0;
        while (const auto id = engine.Writes().LastUndoable(engine.ProcessGeneration()))
        {
            const Status status = engine.Writes().Undo(*engine.Process(), engine.ProcessGeneration(), *id);
            if (!status)
            {
                m_workspace.Report(status);
                break;
            }
            ++restored;
        }
        if (restored > 0)
            engine.Notify(LogLevel::Info, Format("Undid %d memory %s", restored, restored == 1 ? "edit" : "edits"));
    }

    void WriteHistoryWindow::Reveal(const MemoryWrite& write, bool code)
    {
        m_workspace.RevealInMemoryViewer(write.Address, write.Written.size(), code, true);
    }
}
