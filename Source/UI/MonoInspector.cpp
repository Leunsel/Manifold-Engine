#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/UnityExplorer.h"
#include "UI/AddressActions.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kMaxHistory = 16;

        std::string ShortClass(std::string_view name)
        {
            const std::size_t generic = name.find('<');
            const std::size_t dot = name.substr(0, generic).rfind('.');
            return std::string(dot == std::string_view::npos ? name : name.substr(dot + 1));
        }

        // The text to start editing from: no quotes around a string, the name
        // of an enum value without its number.
        std::string EditText(const Target::MonoShown& shown, const Target::MonoTypeInfo& type)
        {
            std::string text = shown.Text;
            if (type.Code == Target::MonoTypeCode::String && text.size() >= 2 && text.front() == '"' && text.back() == '"')
                return text.substr(1, text.size() - 2);
            if (type.Enum)
            {
                if (const std::size_t open = text.rfind(" ("); open != std::string::npos)
                    return text.substr(0, open);
            }
            return text;
        }

        const char* MemberIcon(Target::MonoMemberKind kind)
        {
            switch (kind)
            {
            case Target::MonoMemberKind::Field: return ICON_FA_TAG;
            case Target::MonoMemberKind::Property: return ICON_FA_COG;
            default: return ICON_FA_BOLT;
            }
        }
    }

    // ---------------------------------------------------------------------
    // Tabs and history

    void MonoWindow::OpenObject(std::uint64_t object, bool newTab, bool fromScan)
    {
        if (!object)
            return;
        Target::UnityExplorer& unity = m_engine.Mono().Unity();
        const int id = unity.Inspect(object, fromScan);
        if (!id)
            return;  // the explorer is off; the status line says so
        if (newTab || m_tabs.empty() || m_activeTab >= m_tabs.size())
        {
            InspectorTab tab;
            tab.Key = m_nextTabKey++;
            m_tabs.push_back(std::move(tab));
            m_activeTab = m_tabs.size() - 1;
        }
        Navigate(m_tabs[m_activeTab], id);
        m_selectTab = static_cast<int>(m_activeTab);
        m_showClass = false;
        m_unityGeneration = 0;
    }

    void MonoWindow::OpenStatics(std::uint64_t klass)
    {
        if (!klass)
            return;
        Target::UnityExplorer& unity = m_engine.Mono().Unity();
        const int id = unity.InspectClass(klass);
        if (!id)
            return;  // the explorer is off; the status line says so
        InspectorTab tab;
        tab.Key = m_nextTabKey++;
        m_tabs.push_back(std::move(tab));
        m_activeTab = m_tabs.size() - 1;
        Navigate(m_tabs[m_activeTab], id);
        m_selectTab = static_cast<int>(m_activeTab);
        m_showClass = false;
        m_unityGeneration = 0;
    }

    void MonoWindow::Navigate(InspectorTab& tab, int id)
    {
        Target::UnityExplorer& unity = m_engine.Mono().Unity();
        // Going somewhere new drops the pages after the current one, and the
        // oldest page once the history is full; each page holds its objects.
        while (static_cast<int>(tab.History.size()) > tab.Position + 1)
        {
            unity.Close(tab.History.back());
            tab.History.pop_back();
        }
        tab.History.push_back(id);
        while (tab.History.size() > kMaxHistory)
        {
            unity.Close(tab.History.front());
            tab.History.erase(tab.History.begin());
        }
        tab.Position = static_cast<int>(tab.History.size()) - 1;
        tab.RefreshedAt = ImGui::GetTime();
    }

    void MonoWindow::CloseTab(std::size_t index)
    {
        if (index >= m_tabs.size())
            return;
        Target::UnityExplorer& unity = m_engine.Mono().Unity();
        for (const int id : m_tabs[index].History)
            unity.Close(id);
        m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
        if (m_activeTab >= m_tabs.size())
            m_activeTab = m_tabs.empty() ? 0 : m_tabs.size() - 1;
    }

    void MonoWindow::CloseAll()
    {
        while (!m_tabs.empty())
            CloseTab(m_tabs.size() - 1);
        m_inspections.clear();
        m_closedAll = true;
    }

    std::string MonoWindow::TabLabel(const InspectorTab& tab) const
    {
        const Target::UnityInspection* inspection = Snapshot(tab.Current());
        if (!inspection)
            return "...";
        std::string label = inspection->Title.empty() ? std::string("?") : ShortClass(inspection->Title);
        if (!inspection->ObjectName.empty())
            label = inspection->ObjectName + " (" + label + ")";
        else if (!inspection->Object)
            label = "static " + label;
        if (label.size() > 40)
            label = label.substr(0, 38) + "..";
        return label;
    }

    bool MonoWindow::ObjectLink(const char* id, const std::string& text, std::uint64_t object)
    {
        ImGui::PushID(id);
        const bool clicked = ImGui::TextLink(text.empty() ? "(unnamed)" : text.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
            OpenObject(object, true);
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem(ICON_FA_EYE "  Inspect"))
                OpenObject(object, false);
            if (ImGui::MenuItem(ICON_FA_EXTERNAL_LINK_ALT "  Inspect in a new tab"))
                OpenObject(object, true);
            ImGui::Separator();
            AddressMenuItems(m_workspace, object, ShowHex | DissectHere | CopyAddress, 8);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (clicked)
            OpenObject(object, false);
        return clicked;
    }

    void MonoWindow::InspectorPane(Target::UnityExplorer& unity)
    {
        if (!unity.Enabled())
        {
            EmptyState(ICON_FA_EYE, "Inspector is off",
                "The inspector reads properties and calls methods on the game's main thread. Turn the explorer on under Scenes or Objects "
                "first - it is off because it can still crash Unity 6 games.",
                {}, false);
            return;
        }
        if (m_tabs.empty())
        {
            EmptyState(ICON_FA_EYE, "Inspector",
                "Pick a GameObject under Scenes, an object under Objects, or a class's static members under Classes. Fields are read live from "
                "the game's memory; properties and methods run on its main thread.",
                {}, false);
            return;
        }
        std::optional<std::size_t> close;
        if (ImGui::BeginTabBar("##inspectortabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
        {
            for (std::size_t i = 0; i < m_tabs.size(); ++i)
            {
                InspectorTab& tab = m_tabs[i];
                bool open = true;
                const ImGuiTabItemFlags flags = m_selectTab == static_cast<int>(i) ? ImGuiTabItemFlags_SetSelected : 0;
                const std::string label = TabLabel(tab) + "###inspect" + std::to_string(tab.Key);
                if (ImGui::BeginTabItem(label.c_str(), &open, flags))
                {
                    m_activeTab = i;
                    if (m_selectTab == static_cast<int>(i))
                        m_selectTab = -1;
                    InspectorPage(unity, tab);
                    ImGui::EndTabItem();
                }
                if (!open)
                    close = i;
            }
            ImGui::EndTabBar();
        }
        if (close)
            CloseTab(*close);
    }

    // ---------------------------------------------------------------------
    // One inspector page

    void MonoWindow::InspectorPage(Target::UnityExplorer& unity, InspectorTab& tab)
    {
        const int id = tab.Current();
        const Target::UnityInspection* inspection = Snapshot(id);
        if (!inspection)
        {
            Spinner(ImGui::GetTextLineHeight() * 0.4f, ManifoldTheme::Accent);
            ImGui::SameLine();
            Muted("Opening...");
            return;
        }
        ImGui::BeginDisabled(tab.Position <= 0);
        if (IconButton(ICON_FA_ARROW_LEFT, "Back"))
            --tab.Position;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(tab.Position + 1 >= static_cast<int>(tab.History.size()));
        if (IconButton(ICON_FA_ARROW_RIGHT, "Forward"))
            ++tab.Position;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (IconButton(ICON_FA_SYNC, "Read the values again (properties run their getters on the game's main thread)"))
        {
            unity.Refresh(id, true);
            tab.RefreshedAt = ImGui::GetTime();
        }
        // An object the game destroyed and collected (a scene change) no longer
        // holds its class: stop refreshing it and stop reading its memory at
        // once, and let one refresh record it as gone.
        const bool gone = inspection->Gone || (inspection->Object && inspection->Klass && unity.ClassOf(inspection->Object) != inspection->Klass);
        if (gone)
        {
            tab.Auto = false;
            if (!inspection->Gone && !inspection->Loading)
                unity.Refresh(id, false);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(gone);
        ImGui::Checkbox("Auto", &tab.Auto);
        ImGui::EndDisabled();
        ItemTooltip("Reads properties and the GameObject again about ten times a second (or as fast as the game answers). Fields always update live.");
        // The getters of one refresh run as a batch in a single visit to the
        // game's main thread, so a short interval costs the game little.
        constexpr double kAutoRefreshSeconds = 0.1;
        if (tab.Auto && !inspection->Loading && ImGui::GetTime() - tab.RefreshedAt > kAutoRefreshSeconds)
        {
            unity.Refresh(id, true);
            tab.RefreshedAt = ImGui::GetTime();
        }
        // Fixed slots, so nothing in this row comes and goes while Auto runs:
        // the time of the last refresh stays up during the next one (and is
        // updated twice a second, not ten times), and the spinner shows only
        // for a slow refresh, at a fixed place after it.
        const double now = ImGui::GetTime();
        if (inspection->RoundTrips > 0 && (tab.ShownAt < 0.0 || now - tab.ShownAt > 0.5 || !tab.Auto))
        {
            tab.ShownMs = inspection->RefreshMs;
            tab.ShownAt = now;
        }
        else if (inspection->RoundTrips == 0 && !inspection->Loading)
            tab.ShownMs = -1.0;    // nothing ran in the game (e.g. found by a scan)
        ImGui::SameLine();
        const float slotX = ImGui::GetCursorPosX();
        if (tab.ShownMs >= 0.0)
        {
            ImGui::TextColored(ManifoldTheme::Muted, "%.0f ms", tab.ShownMs);
            char detail[220];
            std::snprintf(detail, sizeof(detail),
                "The last refresh took %.1f ms: %d calls on the game's main thread in %d round trips, %d of them waited for the game's next frame.",
                inspection->RefreshMs, inspection->Calls, inspection->RoundTrips, inspection->FrameWaits);
            ItemTooltip(detail);
        }
        else
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
        if (inspection->Loading && now - tab.RefreshedAt > 0.3)
        {
            ImGui::SameLine(slotX + ImGui::CalcTextSize("00000 ms").x);
            Spinner(ImGui::GetTextLineHeight() * 0.4f, ManifoldTheme::Accent);
        }

        const Target::MonoClassLayout* layout = inspection->Layout.get();
        const std::string className = layout ? layout->Name : inspection->Title;
        CardTitle(inspection->Object ? ICON_FA_CUBE : ICON_FA_CUBES, className.c_str(), inspection->Object ? std::string_view() : std::string_view("static members"));
        if (inspection->Object)
        {
            if (!inspection->ObjectName.empty())
            {
                ImGui::TextUnformatted(("\"" + inspection->ObjectName + "\"").c_str());
                PushMono();
                const float addressWidth = ImGui::CalcTextSize(Hex(inspection->Object).c_str()).x;
                PopMono();
                FlowNext(addressWidth);
            }
            PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AddressText);
            ImGui::Selectable(Hex(inspection->Object).c_str(), false, ImGuiSelectableFlags_None, ImVec2(ImGui::CalcTextSize(Hex(inspection->Object).c_str()).x, 0.0f));
            ImGui::PopStyleColor();
            PopMono();
            if (ImGui::BeginPopupContextItem("##objectaddress"))
            {
                AddressMenuItems(m_workspace, inspection->Object, ShowHex | DissectHere | CopyAddress, 8);
                ImGui::EndPopup();
            }
            ItemTooltip("Right-click: show in memory, Structure Dissect (it names Mono classes), copy");
        }
        if (layout && layout->Chain.size() > 1)
        {
            std::vector<std::string> parts;
            for (std::size_t i = 1; i < layout->Chain.size() && i < 8; ++i)
            {
                if (layout->Chain[i] == "System.Object")
                    break;
                parts.push_back(ShortClass(layout->Chain[i]));
            }
            if (!parts.empty())
            {
                const std::string derives = "derives from " + Join(parts, " > ");
                if (inspection->Object)
                    FlowNext(ImGui::CalcTextSize(derives.c_str()).x);
                Muted(derives);
            }
        }
        if (inspection->FromScan)
            TextWithError(ICON_FA_INFO_CIRCLE "  Found by a memory scan: only fields are shown, nothing is called on it.", ManifoldTheme::Label);
        if (!inspection->Error.empty())
            TextWithError(ICON_FA_EXCLAMATION_TRIANGLE "  " + inspection->Error, ManifoldTheme::Warning);
        if (gone)
        {
            if (!inspection->Gone)
                TextWithError(ICON_FA_EXCLAMATION_TRIANGLE "  This object is gone: the game destroyed it, so its memory is no longer read.", ManifoldTheme::Warning);
            return;
        }

        ImGui::BeginChild("##inspectorbody", ImVec2(0.0f, 0.0f));
        if (inspection->GameObject)
            GameObjectSection(unity, *inspection);
        if (inspection->Owner)
        {
            ImGui::AlignTextToFramePadding();
            Muted("GameObject");
            ImGui::SameLine();
            ObjectLink("##owner", inspection->OwnerName, inspection->Owner);
            if (inspection->Enabled)
            {
                ImGui::SameLine(0.0f, Px(16.0f));
                bool enabled = *inspection->Enabled;
                if (ImGui::Checkbox("Enabled", &enabled))
                    unity.SetEnabled(inspection->Object, enabled, id);
            }
        }
        if (inspection->Transform && ImGui::CollapsingHeader(ICON_FA_ARROWS_ALT "  Transform", ImGuiTreeNodeFlags_DefaultOpen))
            TransformRows(unity, *inspection->Transform, id);
        if (layout && layout->Rank > 0 && inspection->Object)
            ElementsTable(unity, *inspection);
        if (layout)
            MembersTable(unity, *inspection);
        ImGui::EndChild();
    }

    void MonoWindow::GameObjectSection(Target::UnityExplorer& unity, const Target::UnityInspection& inspection)
    {
        const Target::UnityGameObject& go = *inspection.GameObject;
        const int id = inspection.Id;
        bool active = go.ActiveSelf;
        if (ImGui::Checkbox("##goactive", &active))
            unity.SetActive(go.Object, active, id);
        ItemTooltip("GameObject.SetActive");
        ImGui::SameLine();
        std::string& name = m_nameEdits[id];
        ImGui::SetNextItemWidth(std::min(Px(320.0f), ImGui::GetContentRegionAvail().x));
        const bool renamed = InputText("##goname", name, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!ImGui::IsItemActive() && !renamed)
            name = go.Name;
        if (renamed)
            unity.SetName(go.Object, name, id);
        ItemTooltip("The object's name; Enter renames it in the game");
        std::string facts = "Layer " + std::to_string(go.Layer);
        if (!go.Tag.empty())
            facts += "  ·  Tag " + go.Tag;
        if (!go.Scene.empty())
            facts += "  ·  Scene " + go.Scene;
        if (go.ActiveSelf && !go.ActiveInHierarchy)
            facts += "  ·  inactive through a parent";
        Muted(facts);
        if (go.Parent)
        {
            ImGui::AlignTextToFramePadding();
            Muted("Parent");
            ImGui::SameLine();
            ObjectLink("##parent", go.ParentName, go.Parent);
        }
        if (go.Transform.Read && ImGui::CollapsingHeader(ICON_FA_ARROWS_ALT "  Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushID("##gotransform");
            TransformRows(unity, go.Transform, id);
            ImGui::PopID();
        }
        const std::string components = ICON_FA_PUZZLE_PIECE "  Components (" + std::to_string(go.Components.size()) + ")###components";
        if (ImGui::CollapsingHeader(components.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (std::size_t i = 0; i < go.Components.size(); ++i)
            {
                const Target::UnityComponent& component = go.Components[i];
                ImGui::PushID(static_cast<int>(i));
                if (component.HasEnabled)
                {
                    bool enabled = component.Enabled;
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(2.0f), 0.0f));
                    if (ImGui::Checkbox("##enabled", &enabled))
                        unity.SetEnabled(component.Object, enabled, id);
                    ImGui::PopStyleVar();
                    ItemTooltip("Behaviour.enabled");
                    ImGui::SameLine();
                }
                else
                {
                    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight() - Px(4.0f), 0.0f));
                    ImGui::SameLine();
                }
                ObjectLink("##component", ShortClass(component.Class), component.Object);
                if (component.Class.find('.') != std::string::npos)
                {
                    ImGui::SameLine();
                    Muted(component.Class);
                }
                ImGui::PopID();
            }
        }
        const std::string children = ICON_FA_SITEMAP "  Children (" + std::to_string(go.ChildCount) + ")###children";
        if (go.ChildCount > 0 && ImGui::CollapsingHeader(children.c_str()))
        {
            for (std::size_t i = 0; i < go.Children.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                ObjectLink("##child", go.Children[i].Name, go.Children[i].Object);
                ImGui::PopID();
            }
            if (go.ChildCount > static_cast<int>(go.Children.size()))
                Muted("... and " + std::to_string(go.ChildCount - static_cast<int>(go.Children.size())) + " more (expand them in the scene tree)");
        }
    }

    void MonoWindow::TransformRows(Target::UnityExplorer& unity, const Target::UnityTransform& transform, int refresh)
    {
        static const char* const kRows[] = { "Local position", "Local rotation", "Local scale" };
        const float* values[] = { transform.LocalPosition, transform.LocalEuler, transform.LocalScale };
        const float label = ImGui::CalcTextSize("Local rotation").x + Px(16.0f);
        for (int row = 0; row < 3; ++row)
        {
            ImGui::PushID(row);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kRows[row]);
            ImGui::SameLine(label);
            const bool mine = m_dragTransform == transform.Object && m_dragRow == row;
            float shown[3] = { values[row][0], values[row][1], values[row][2] };
            if (mine)
                std::copy(m_dragValue, m_dragValue + 3, shown);
            else
            {
                // Engine noise such as -1.3e-07 would read "-0.000" and its sign
                // flip between refreshes; anything that rounds to zero shows 0.
                for (float& value : shown)
                {
                    if (std::fabs(value) < 0.0005f)
                        value = 0.0f;
                }
            }
            // Never narrower than three readable numbers (~60 px each).
            ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x, Px(180.0f), Px(360.0f)));
            if (ImGui::DragFloat3("##vector", shown, row == 1 ? 0.5f : 0.05f, 0.0f, 0.0f, "%.3f"))
            {
                m_dragTransform = transform.Object;
                m_dragRow = row;
                std::copy(shown, shown + 3, m_dragValue);
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                unity.SetVector(transform.Object, row, m_dragValue, refresh);
                m_dragTransform = 0;
                m_dragRow = -1;
            }
            else if (mine && !ImGui::IsItemActive() && !ImGui::IsItemDeactivated())
            {
                m_dragTransform = 0;
                m_dragRow = -1;
            }
            ItemTooltip("Drag, or double-click to type; the game gets the value when you let go");
            ImGui::PopID();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("World position");
        ImGui::SameLine(label);
        const auto clean = [](float value) { return std::fabs(value) < 0.0005f ? 0.0f : value; };
        Muted(Format("%.3f, %.3f, %.3f", clean(transform.Position[0]), clean(transform.Position[1]), clean(transform.Position[2])));
    }

    void MonoWindow::ElementsTable(Target::UnityExplorer& unity, const Target::UnityInspection& inspection)
    {
        const Target::MonoClassLayout& layout = *inspection.Layout;
        const std::shared_ptr<Target::Process> process = m_engine.Process();
        if (!process)
            return;
        std::uint64_t length = 0;
        process->ReadExact(inspection.Object + 0x18, &length, sizeof(length));
        const int count = static_cast<int>(std::min<std::uint64_t>(length, 1000000));
        const int size = std::max(1, layout.ElementSize);
        const std::string header = ICON_FA_LIST "  Elements (" + std::to_string(length) + ")###elements";
        if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            return;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        const float height = std::min(Px(260.0f), (ImGui::GetTextLineHeightWithSpacing() + Px(2.0f)) * static_cast<float>(count + 1) + Px(8.0f));
        if (!ImGui::BeginTable("##elements", 2, flags, ImVec2(0.0f, std::max(height, Px(60.0f)))))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
        const std::string valueColumn = "Value (" + Target::UnityExplorer::ShortTypeName(layout.Element) + ")";
        ImGui::TableSetupColumn(valueColumn.c_str(), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(count);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                PushMono();
                ImGui::TextColored(ManifoldTheme::Label, "%d", i);
                PopMono();
                ImGui::TableNextColumn();
                const std::uint64_t at = inspection.Object + 0x20 + static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(size);
                Target::MonoShown shown;
                if (process->ReadExact(at, bytes.data(), bytes.size()))
                    shown = unity.Format(layout.Element, bytes.data(), bytes.size());
                else
                    shown.Text = "??";
                if (shown.Reference)
                    ObjectLink("##element", shown.Text, shown.Reference);
                else
                    ImGui::TextUnformatted(shown.Text.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    void MonoWindow::ValueCell(Target::UnityExplorer& unity, const Target::UnityInspection& inspection, int member, const Target::MonoShown& shown, bool editable)
    {
        const Target::MonoMember& info = inspection.Layout->Members[static_cast<std::size_t>(member)];
        const bool editing = m_editInspection == inspection.Id && m_editMember == member;
        if (editing)
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (m_editFocus)
            {
                ImGui::SetKeyboardFocusHere();
                m_editFocus = false;
            }
            const bool submitted = InputText("##edit", m_editText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (submitted)
            {
                unity.SetMember(inspection.Id, member, m_editText);
                m_editMember = -1;
            }
            else if (ImGui::IsItemDeactivated())
                m_editMember = -1;
            ItemTooltip("Enter writes the value, Escape keeps the old one");
            return;
        }
        const int code = info.Type.ValueCode ? info.Type.ValueCode : info.Type.Code;
        if (editable && code == Target::MonoTypeCode::Boolean && !info.Type.Enum && (shown.Text == "true" || shown.Text == "false"))
        {
            bool value = shown.Text == "true";
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(2.0f), 0.0f));
            if (ImGui::Checkbox("##bool", &value))
                unity.SetMember(inspection.Id, member, value ? "true" : "false");
            ImGui::PopStyleVar();
            ImGui::SameLine();
            Muted(shown.Text);
            return;
        }
        if (shown.Reference)
        {
            ObjectLink("##ref", shown.Text, shown.Reference);
            if (editable)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_PEN "##set"))
                {
                    m_editInspection = inspection.Id;
                    m_editMember = member;
                    m_editText = Hex(shown.Reference);
                    m_editFocus = true;
                }
                ItemTooltip("Type null, or the address of another object");
            }
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, shown.Null || shown.Pending ? ManifoldTheme::Muted : (code == Target::MonoTypeCode::String ? ManifoldTheme::CodeString : ManifoldTheme::Text));
        const bool clicked = ImGui::Selectable(shown.Text.empty() ? " " : shown.Text.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
        ImGui::PopStyleColor();
        if (editable && clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            m_editInspection = inspection.Id;
            m_editMember = member;
            m_editText = EditText(shown, info.Type);
            m_editFocus = true;
        }
        if (editable)
            ItemTooltip("Double-click to change it");
        if (ImGui::BeginPopupContextItem("##valuemenu"))
        {
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the value"))
                ImGui::SetClipboardText(shown.Text.c_str());
            if (editable && ImGui::MenuItem(ICON_FA_PEN "  Change..."))
            {
                m_editInspection = inspection.Id;
                m_editMember = member;
                m_editText = EditText(shown, info.Type);
                m_editFocus = true;
            }
            if (info.Kind == Target::MonoMemberKind::Field && !info.Literal && (info.Static || inspection.Object))
            {
                const std::uint64_t address = info.Static ? info.StaticData + static_cast<std::uint64_t>(std::max(0, info.Offset))
                                                          : inspection.Object + static_cast<std::uint64_t>(info.Offset);
                if (address && (!info.Static || info.StaticData))
                {
                    ImGui::Separator();
                    AddressMenuItems(m_workspace, address, ShowHex | WatchAccesses | WatchWrites | AddToList | CopyAddress,
                        std::clamp(info.Type.ValueType ? info.Type.Size : 8, 1, 8));
                }
            }
            ImGui::EndPopup();
        }
    }

    void MonoWindow::MembersTable(Target::UnityExplorer& unity, const Target::UnityInspection& inspection)
    {
        const Target::MonoClassLayout& layout = *inspection.Layout;
        ImGui::Spacing();
        // The switches wrap under the filter in a narrow inspector instead of
        // running off its edge.
        ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * 0.4f, Px(140.0f), Px(220.0f)));
        InputTextWithHint("##memberfilter", "filter members", m_memberFilter);
        FlowNext(CheckboxWidth("Fields"));
        ImGui::Checkbox("Fields", &m_showFields);
        FlowNext(CheckboxWidth("Properties"));
        ImGui::Checkbox("Properties", &m_showProperties);
        FlowNext(CheckboxWidth("Methods"));
        ImGui::Checkbox("Methods", &m_showMethods);
        FlowNext(CheckboxWidth("Inherited"));
        ImGui::Checkbox("Inherited", &m_showInherited);
        ItemTooltip("Also members of the base classes (MonoBehaviour, Component, ...)");

        std::vector<int> rows;
        for (int i = 0; i < static_cast<int>(layout.Members.size()); ++i)
        {
            const Target::MonoMember& member = layout.Members[static_cast<std::size_t>(i)];
            if ((member.Kind == Target::MonoMemberKind::Field && !m_showFields) || (member.Kind == Target::MonoMemberKind::Property && !m_showProperties) ||
                (member.Kind == Target::MonoMemberKind::Method && !m_showMethods))
                continue;
            if (!m_showInherited && member.Declaring != layout.Name)
                continue;
            if (!inspection.Object && !member.Static)
                continue;
            if (!m_memberFilter.empty() && !ContainsNoCase(member.Name, m_memberFilter))
                continue;
            rows.push_back(i);
        }
        if (rows.empty())
        {
            Muted(inspection.Object ? "No members to show." : "This class has no static members to show.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        const float height = std::max(Px(160.0f), ImGui::GetContentRegionAvail().y);
        if (!ImGui::BeginTable("##members", 3, flags, ImVec2(0.0f, height)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const int index = rows[static_cast<std::size_t>(row)];
                const Target::MonoMember& member = layout.Members[static_cast<std::size_t>(index)];
                ImGui::TableNextRow();
                ImGui::PushID(index);
                ImGui::TableNextColumn();
                const bool inherited = member.Declaring != layout.Name;
                ImGui::PushStyleColor(ImGuiCol_Text, inherited ? ManifoldTheme::Label : ManifoldTheme::Text);
                ImGui::TextUnformatted((std::string(MemberIcon(member.Kind)) + "  " + (member.Static ? "static " : "") + member.Name).c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    std::string tip = member.Declaring;
                    if (member.Kind == Target::MonoMemberKind::Field)
                        tip += member.Literal ? "\nconstant" : "\noffset 0x" + Hex(static_cast<std::uint64_t>(member.Offset)) + (member.Static ? " in the static data" : "");
                    else if (member.Kind == Target::MonoMemberKind::Property)
                        tip += std::string("\n") + (member.Getter ? "get" : "") + (member.Getter && member.Setter ? " · " : "") + (member.Setter ? "set" : "");
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::TableNextColumn();
                if (member.Kind == Target::MonoMemberKind::Method)
                    TextFitted(Target::UnityExplorer::ShortTypeName(member.Type) + " " + Target::UnityExplorer::Signature(member), ImGui::GetContentRegionAvail().x, ManifoldTheme::CodeKeyword);
                else
                    TextFitted(Target::UnityExplorer::ShortTypeName(member.Type), ImGui::GetContentRegionAvail().x, ManifoldTheme::CodeKeyword);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", member.Type.Name.c_str());
                ImGui::TableNextColumn();
                const auto value = inspection.Values.find(index);
                switch (member.Kind)
                {
                case Target::MonoMemberKind::Field:
                {
                    const Target::MonoShown shown = unity.ReadField(member, inspection.Object);
                    const bool simple = member.Type.ValueType;
                    const bool editable = shown.Editable && !member.Literal && (member.Static || inspection.Object) && (simple || !inspection.FromScan);
                    ValueCell(unity, inspection, index, shown, editable);
                    break;
                }
                case Target::MonoMemberKind::Property:
                {
                    if (value != inspection.Values.end() && value->second.Read)
                        ValueCell(unity, inspection, index, unity.Shown(member, value->second), member.Setter != 0 && !inspection.FromScan);
                    else if (value != inspection.Values.end() && !value->second.Error.empty())
                    {
                        TextFitted(ICON_FA_EXCLAMATION_TRIANGLE "  " + value->second.Error, ImGui::GetContentRegionAvail().x, ManifoldTheme::Warning);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", value->second.Error.c_str());
                    }
                    else if (!member.Getter)
                        Muted("set only");
                    else if (inspection.FromScan)
                        Muted("not read");
                    else if (inspection.Loading)
                        Muted("...");
                    else if (ImGui::SmallButton("Get"))
                        unity.Refresh(inspection.Id, true);
                    break;
                }
                case Target::MonoMemberKind::Method:
                {
                    ImGui::BeginDisabled(inspection.FromScan);
                    if (member.Params.empty())
                    {
                        if (ImGui::SmallButton(ICON_FA_PLAY "  Call"))
                            unity.InvokeMethod(inspection.Id, index, {});
                        ItemTooltip("Runs the method on the game's main thread");
                    }
                    else
                    {
                        if (ImGui::SmallButton(ICON_FA_PLAY "  Call..."))
                        {
                            m_callInspection = inspection.Id;
                            m_callMember = index;
                            m_callArgs.assign(member.Params.size(), std::string());
                            ImGui::OpenPopup("##call");
                        }
                        ItemTooltip("Asks for the arguments, then runs the method on the game's main thread");
                    }
                    ImGui::EndDisabled();
                    if (ImGui::BeginPopup("##call"))
                    {
                        if (m_callInspection == inspection.Id && m_callMember == index)
                        {
                            SectionLabel((member.Name + Target::UnityExplorer::Signature(member)).c_str());
                            for (std::size_t p = 0; p < member.Params.size() && p < m_callArgs.size(); ++p)
                            {
                                ImGui::PushID(static_cast<int>(p));
                                const std::string name = p < member.ParamNames.size() && !member.ParamNames[p].empty() ? member.ParamNames[p] : "arg" + std::to_string(p + 1);
                                ImGui::SetNextItemWidth(Px(220.0f));
                                InputTextWithHint("##arg", Target::UnityExplorer::ShortTypeName(member.Params[p]).c_str(), m_callArgs[p]);
                                ImGui::SameLine();
                                ImGui::TextUnformatted(name.c_str());
                                ImGui::PopID();
                            }
                            Muted("Numbers, true/false, enum names, \"text\", null, or an object's address.");
                            if (AccentButton(ICON_FA_PLAY "  Call"))
                            {
                                unity.InvokeMethod(inspection.Id, index, m_callArgs);
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        ImGui::EndPopup();
                    }
                    if (value != inspection.Values.end())
                    {
                        ImGui::SameLine();
                        if (value->second.Read)
                        {
                            const Target::MonoShown shown = unity.Shown(member, value->second);
                            if (shown.Reference)
                                ObjectLink("##result", shown.Text, shown.Reference);
                            else
                                TextFitted(ICON_FA_ARROW_RIGHT "  " + shown.Text, ImGui::GetContentRegionAvail().x, ManifoldTheme::Text);
                        }
                        else
                        {
                            TextFitted(ICON_FA_EXCLAMATION_TRIANGLE "  " + value->second.Error, ImGui::GetContentRegionAvail().x, ManifoldTheme::Warning);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("%s", value->second.Error.c_str());
                        }
                    }
                    break;
                }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}
