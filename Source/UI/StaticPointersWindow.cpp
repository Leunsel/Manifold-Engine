#include "UI/StaticPointersWindow.h"

#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Target/Process.h"
#include "UI/AddressActions.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui_internal.h"

#include <algorithm>
#include <chrono>

namespace Manifold::UI
{
    StaticPointersWindow::StaticPointersWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    StaticPointersWindow::~StaticPointersWindow() = default;

    void StaticPointersWindow::Search(std::uint64_t address)
    {
        Show();
        m_address = Hex(address);
        Start();
    }

    void StaticPointersWindow::Start()
    {
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() || m_task.Running())
            return;
        Status error;
        const auto target = engine.Evaluate(Trim(m_address), &error);
        if (!target)
        {
            m_error = error.Text.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, Trim(m_address)) : error.Text;
            return;
        }
        const auto maxOffset = ParseHex(Trim(m_maxOffset));
        if (!maxOffset || *maxOffset > 0x1000000)
        {
            m_error = "The largest offset must be a hex number up to 1000000.";
            return;
        }
        m_error.clear();
        engine.Process()->RefreshModules();
        m_process = engine.Process();
        m_target = *target;
        m_result.reset();
        m_selected = -1;
        Scan::StaticPointerSearch search;
        search.Target = *target;
        search.MaxOffset = *maxOffset;
        search.SystemModules = m_system;
        std::shared_ptr<Target::Process> process = m_process;
        m_task.Start(engine.Work(), "Finding static pointers", [process, search](Scan::PatternProgress& progress)
            {
                const auto started = std::chrono::steady_clock::now();
                Outcome outcome;
                outcome.Hits = Scan::FindStaticPointers(*process, search, &progress, &outcome.Truncated);
                outcome.Cancelled = progress.Cancel;
                outcome.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                return outcome;
            });
    }

    void StaticPointersWindow::Collect()
    {
        // ∑ The hits are addresses in the process they were found in: once another
        //   one is open, its module names, Add, Dissect and the memory viewer would
        //   all apply them to the wrong game, so they go, with a line saying why.
        Engine& engine = m_workspace.Core();
        if (m_process && (!engine.HasProcess() || engine.Process() != m_process))
        {
            if (m_result || m_task.Running())
                m_error = "The results were for " + m_process->Name() + ", which is no longer the open process.";
            if (m_task.Running())
                m_task.Cancel();
            m_result.reset();
            m_process.reset();
            m_selected = -1;
        }
        std::optional<Outcome> outcome = m_task.Collect();
        if (!outcome || !m_process)
            return;
        m_result = std::move(outcome);
        LOG_INFO("Scan", Grouped(m_result->Hits.size()) + " static pointers to " + Hex(m_target) + (m_result->Cancelled ? " (cancelled)" : ""));
    }

    std::string StaticPointersWindow::PathText(const Scan::StaticPointer& hit) const
    {
        const std::uint64_t offset = hit.Offset(m_target);
        return "[" + m_workspace.Core().Symbols().ModuleOffset(hit.Address) + "]" + (offset ? "+" + Hex(offset) : "");
    }

    void StaticPointersWindow::AddToList(const Scan::StaticPointer& hit)
    {
        Engine& engine = m_workspace.Core();
        Table::MemoryRecord& record = engine.Table().AddValue("Static pointer " + PathText(hit), engine.Symbols().ModuleOffset(hit.Address), Scan::ValueType::FourBytes);
        record.Offsets = { Hex(hit.Offset(m_target)) };
        engine.RefreshValues(true);
    }

    void StaticPointersWindow::Results()
    {
        Engine& engine = m_workspace.Core();
        if (m_result->Hits.empty())
        {
            EmptyState(ICON_FA_THUMBTACK, "No static pointers", "No writable module memory holds a pointer to " + Hex(m_target) + " or up to " + Trim(m_maxOffset) + " bytes before it.", {}, false);
            return;
        }
        if (!BeginDataTable("##statichits", { { "Static address", 1.4f }, { "Offset", 0.5f }, { "Points to", 1.0f }, { "Now", 1.0f } }))
            return;
        const bool live = engine.HasProcess() && m_process == engine.Process();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_result->Hits.size()));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const Scan::StaticPointer& hit = m_result->Hits[static_cast<std::size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(row);
                PushMono();
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
                if (ImGui::Selectable(engine.Symbols().ModuleOffset(hit.Address).c_str(), row == m_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = row;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        AddToList(hit);
                }
                ImGui::PopStyleColor();
                PopMono();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s\nDouble-click adds it to the address list.", PathText(hit).c_str());
                if (ImGui::BeginPopupContextItem("##statichitmenu"))
                {
                    m_selected = row;
                    if (ImGui::MenuItem(ICON_FA_PLUS "  Add the Pointer to the Address List"))
                        AddToList(hit);
                    if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect the Structure It Points To"))
                        m_workspace.OpenStructure(hit.Value);
                    if (ImGui::MenuItem(ICON_FA_CODE "  Show the Static Address in the Memory Viewer"))
                        m_workspace.RevealInMemoryViewer(hit.Address, static_cast<std::uint64_t>(m_process ? m_process->PointerSize() : 8), false, true);
                    ImGui::Separator();
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Pointer Path"))
                        ImGui::SetClipboardText(PathText(hit).c_str());
                    if (ImGui::MenuItem(ICON_FA_LIST "  Copy All Pointer Paths"))
                    {
                        std::string text;
                        for (const Scan::StaticPointer& each : m_result->Hits)
                            text += PathText(each) + "\r\n";
                        ImGui::SetClipboardText(text.c_str());
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                PushMono();
                const std::uint64_t offset = hit.Offset(m_target);
                ImGui::TextUnformatted(offset ? ("+" + Hex(offset)).c_str() : "+0");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(AddressText(engine, hit.Value).c_str());
                ImGui::TableNextColumn();
                std::optional<std::uint64_t> now;
                if (live)
                    now = m_process->ReadPointer(hit.Address);
                if (!now)
                    ImGui::TextDisabled("unreadable");
                else if (*now == hit.Value)
                    Muted("unchanged");
                else
                    TextWithError(Hex(*now), ManifoldTheme::Warning);
                PopMono();
            }
        }
        ImGui::PopItemFlag();
        ImGui::EndTable();
    }

    void StaticPointersWindow::Draw()
    {
        Collect();
        if (!BeginWindow(ICON_FA_THUMBTACK "  Find Static Pointers###staticpointers", ImVec2(760.0f, 520.0f), 0, ImVec2(460.0f, 280.0f)))
            return;
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess() && !m_task.Running() && !m_result)
        {
            m_workspace.NoProcessState(ICON_FA_THUMBTACK, "Open a process to find static pointers.");
            EndToolWindow();
            return;
        }
        Muted("Looks in the writable sections of the loaded modules for pointers to the address or to a structure start up to the largest offset before it.");
        const bool running = m_task.Running();
        bool enter = false;
        // ∑ the three settings share a line while it is wide enough and wrap onto the next one in a narrow window
        Layout::RowOptions line;
        line.Wrap = true;
        line.Gap = Layout::Space::L;
        Layout::Row settings("##staticsettings", { Layout::Size::Fill().Min(240.0f).Max(320.0f), Layout::Size::Fixed(190.0f), Layout::Size::Fit() }, line);
        settings.Cell([&] {
            Layout::FieldOptions field;
            field.Placement = Layout::LabelPlacement::Left;
            Layout::Field("Address", field);
            enter |= InputTextWithHint("##staticaddress", "address or expression", m_address, ImGuiInputTextFlags_EnterReturnsTrue);
        });
        settings.Cell([&] {
            Layout::FieldOptions field;
            field.Placement = Layout::LabelPlacement::Left;
            Layout::Field("Largest offset", field);
            enter |= InputTextWithHint("##staticoffset", "1000", m_maxOffset, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsHexadecimal);
        });
        settings.Cell([&] { ImGui::Checkbox("System modules", &m_system); });
        settings.End();
        if (!m_error.empty())
            TextWithError(m_error, ManifoldTheme::Error);
        ImGui::BeginDisabled(running || !engine.HasProcess());
        if (AccentButton(ICON_FA_SEARCH "  Find") || (enter && !running))
        {
            Start();
            if (enter && m_error.empty())
                ImGui::ClearActiveID();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, Px(12.0f));
        if (running)
        {
            const Scan::PatternProgress& progress = m_task.State();
            const std::uint64_t total = std::max<std::uint64_t>(1, progress.Total.load());
            if (TaskStatus("Searching" + Format("  %d%%", static_cast<int>(std::min<std::uint64_t>(100, progress.Done.load() * 100 / total)))))
                m_task.Cancel();
        }
        else if (m_result)
        {
            ImGui::AlignTextToFramePadding();
            const std::size_t count = m_result->Hits.size();
            Muted(Grouped(count) + (count == 1 ? " static pointer" : " static pointers") + (m_result->Truncated ? " (stopped at the limit)" : m_result->Cancelled ? " (cancelled)" : "") + "  ·  " + Hex(m_target) + "  ·  " +
                  Format("%.2f s", m_result->Seconds));
        }
        ImGui::Spacing();
        if (m_result)
            Results();
        EndToolWindow();
    }
}
