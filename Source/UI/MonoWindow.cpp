#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Target/Mono.h"
#include "UI/AddressActions.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>

namespace Manifold::UI
{
    MonoWindow::MonoWindow(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()) {}

    void MonoWindow::RebuildFilter()
    {
        m_filtered.clear();
        for (int i = 0; i < static_cast<int>(m_classes.size()); ++i)
        {
            if (m_filter.empty() || ContainsNoCase(m_classes[static_cast<std::size_t>(i)].FullName(), m_filter) ||
                ContainsNoCase(m_classes[static_cast<std::size_t>(i)].Image, m_filter))
                m_filtered.push_back(i);
        }
        m_lastFilter = m_filter;
    }

    void MonoWindow::SelectClass(int index)
    {
        m_selected = index;
        m_methods.clear();
        m_fields.clear();
        m_resolved.clear();
        if (index < 0 || index >= static_cast<int>(m_classes.size()))
            return;
        m_selectedName = m_classes[static_cast<std::size_t>(index)].FullName();
        m_methods = m_engine.Mono().MethodsOf(m_selectedName);
        m_fields = m_engine.Mono().FieldsOf(m_selectedName);
    }

    void MonoWindow::Draw()
    {
        if (!m_open)
            return;
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_DICE_D6 "  Mono / .NET###mono", &m_open, ImVec2(820.0f, 520.0f), 0, ImVec2(560.0f, 340.0f)))
        {
            EndToolWindow();
            return;
        }
        if (!m_engine.HasProcess())
        {
            m_workspace.NoProcessState(ICON_FA_DICE_D6, "Open a Mono-based Unity game to read its classes, methods and fields.");
            EndToolWindow();
            return;
        }

        Target::MonoBridge& mono = m_engine.Mono();
        if (!mono.Present())
        {
            std::vector<EmptyAction> actions = { { ICON_FA_PLUG "  Load the Mono runtime", nullptr, true } };
            if (EmptyState(ICON_FA_DICE_D6, "No Mono runtime loaded",
                    "This reads the game's Mono runtime by running a small helper inside it. IL2CPP games are not supported.", actions, true) == 0)
            {
                const Status status = m_engine.EnsureMono();
                if (!status)
                    m_workspace.Report(status);
            }
            EndToolWindow();
            return;
        }

        if (mono.Generation() != m_generation)
        {
            m_generation = mono.Generation();
            m_classes = mono.Classes();
            m_lastFilter = "\x01";
            if (!m_selectedName.empty())
            {
                const auto found = std::find_if(m_classes.begin(), m_classes.end(), [&](const Target::MonoClass& c) { return c.FullName() == m_selectedName; });
                SelectClass(found == m_classes.end() ? -1 : static_cast<int>(found - m_classes.begin()));
            }
        }

        ImGui::AlignTextToFramePadding();
        Muted(mono.Module() + "  ·  " + std::to_string(m_classes.size()) + " classes");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_SYNC "  Reload"))
        {
            mono.Refresh();
            m_generation = ~0ull;
        }
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##monofilter", "filter classes (name or assembly)", m_filter);
        if (m_filter != m_lastFilter)
            RebuildFilter();

        const float gap = Px(8.0f);
        if (m_listWidth <= 0.0f)
            m_listWidth = Px(320.0f);
        const float total = ImGui::GetContentRegionAvail().x;
        m_listWidth = std::clamp(m_listWidth, Px(220.0f), std::max(Px(220.0f), total - Px(260.0f) - gap));
        const float height = ImGui::GetContentRegionAvail().y;

        ImGui::BeginChild("##monoclasses", ImVec2(m_listWidth, height), ImGuiChildFlags_Borders);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filtered.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const int index = m_filtered[static_cast<std::size_t>(row)];
                const Target::MonoClass& info = m_classes[static_cast<std::size_t>(index)];
                ImGui::PushID(index);
                if (ImGui::Selectable(info.FullName().c_str(), m_selected == index))
                    SelectClass(index);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, gap);
        ImGui::BeginChild("##monomembers", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        if (m_selected < 0)
            Muted("Pick a class on the left.");
        else
        {
            CardTitle(ICON_FA_DICE_D6, m_selectedName.c_str());
            MethodsPane();
            ImGui::Spacing();
            FieldsPane();
        }
        ImGui::EndChild();
        EndToolWindow();
    }

    void MonoWindow::MethodsPane()
    {
        SectionLabel((std::to_string(m_methods.size()) + " methods").c_str());
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        const float height = std::max(Px(120.0f), ImGui::GetContentRegionAvail().y * 0.55f);
        if (ImGui::BeginTable("##monomethods", 2, flags, ImVec2(0.0f, height)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableHeadersRow();
            // Resolve (and so JIT-compile) only the methods that are on screen,
            // through the clipper, and cache the result (0 = compile failed) so a
            // class with hundreds of methods does not compile them all at once.
            const auto resolve = [&](const std::string& method) -> std::uint64_t
                {
                    auto it = m_resolved.find(method);
                    if (it == m_resolved.end())
                        it = m_resolved.emplace(method, m_engine.Mono().AddressOf(m_selectedName + ":" + method).value_or(0)).first;
                    return it->second;
                };
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_methods.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const std::string& method = m_methods[static_cast<std::size_t>(i)];
                    const std::uint64_t address = resolve(method);
                    ImGui::TableNextRow();
                    ImGui::PushID(i);
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(method.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && address)
                        m_workspace.OpenMemoryViewer(address);
                    if (ImGui::BeginPopupContextItem())
                    {
                        if (address)
                            AddressMenuItems(m_workspace, address, ShowCode | ShowHex);
                        else
                            ImGui::MenuItem(ICON_FA_CODE "  Show in disassembler", nullptr, false, false);
                        if (ImGui::MenuItem(ICON_FA_CIRCLE "  Set a breakpoint", nullptr, false, address != 0))
                        {
                            Status status = m_engine.EnsureDebugger();
                            if (status)
                                status = m_engine.Debugger().SetBreakpoint(address, Debug::BreakpointType::Execute, 1, false);
                            m_workspace.Report(status);
                        }
                        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the name"))
                            ImGui::SetClipboardText((m_selectedName + ":" + method).c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    PushMono();
                    if (address)
                        ImGui::TextColored(ManifoldTheme::AddressText, "%s", Hex(address).c_str());
                    else
                        Muted("--");
                    PopMono();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void MonoWindow::FieldsPane()
    {
        SectionLabel((std::to_string(m_fields.size()) + " fields").c_str());
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##monofields", 2, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(90.0f));
            ImGui::TableHeadersRow();
            for (const Target::MonoField& field : m_fields)
            {
                ImGui::TableNextRow();
                ImGui::PushID(field.Name.c_str());
                ImGui::TableNextColumn();
                PushMono();
                if (ImGui::Selectable(field.Name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                    ImGui::SetClipboardText(Hex(static_cast<std::uint64_t>(field.Offset)).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(ManifoldTheme::BytesText, "0x%X", field.Offset);
                PopMono();
                ItemTooltip("Offset from the object. Click the name to copy it.");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}
