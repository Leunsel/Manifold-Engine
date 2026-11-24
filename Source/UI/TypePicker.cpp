#include "UI/TypePicker.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Lua/LuaEngine.h"
#include "Table/AddressList.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::size_t kFilterFrom = 15;

        struct FilterState
        {
            ImGuiID Owner = 0;
            std::string Text;
            int Frames = 0;
        };

        FilterState& Filter()
        {
            static FilterState state;
            return state;
        }

        float TextWidth(std::string_view text)
        {
            return Layout::FitText(ImGui::GetFont(), ImGui::GetFontSize(), text, FLT_MAX).Width;
        }

        bool Matches(const Scan::TypeSection& section, const Scan::TypeEntry& entry, std::string_view filter)
        {
            return Scan::TypeEntryMatches(entry, filter) || ContainsNoCase(section.Title, TrimView(filter));
        }
    }

    std::vector<Scan::CustomTypeInfo> CustomTypes(const Lua::LuaEngine& lua)
    {
        std::vector<Scan::CustomTypeInfo> types;
        for (const std::string& name : lua.CustomTypeNames())
        {
            if (const Lua::CustomType* type = lua.FindCustomType(name))
                types.push_back({ type->Name, type->ByteCount(), type->Float, type->String });
        }
        return types;
    }

    Scan::TypeChoice RecordTypeChoice(const Table::MemoryRecord& record)
    {
        if (!record.CustomTypeName.empty())
            return { Scan::ScanKind::Custom, record.Type, record.CustomTypeName };
        return { Scan::ScanKind::Single, record.Type, {} };
    }

    void SetRecordType(Table::MemoryRecord& record, const Scan::TypeChoice& choice)
    {
        if (choice.Kind == Scan::ScanKind::Custom)
        {
            record.CustomTypeName = choice.Custom;
            return;
        }
        record.CustomTypeName.clear();
        record.Type = choice.Type;
    }

    bool TypeComboItems(const std::vector<Scan::TypeSection>& sections, Scan::TypeChoice& choice, bool keepOpen)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float indent = Px(8.0f);
        float labelWidth = 0.0f;
        float detailWidth = 0.0f;
        std::size_t total = 0;
        for (const Scan::TypeSection& section : sections)
        {
            labelWidth = std::max(labelWidth, TextWidth(section.Title) - indent);
            for (const Scan::TypeEntry& entry : section.Entries)
            {
                labelWidth = std::max(labelWidth, TextWidth(entry.Label));
                if (!entry.Detail.empty())
                    detailWidth = std::max(detailWidth, TextWidth(entry.Detail));
                ++total;
            }
        }
        const float width = indent + labelWidth + (detailWidth > 0.0f ? Px(24.0f) + detailWidth : 0.0f) + style.ItemInnerSpacing.x;

        FilterState& filter = Filter();
        const ImGuiID owner = ImGui::GetID("##typefilter");
        if (ImGui::IsWindowAppearing() || filter.Owner != owner)
        {
            filter.Owner = owner;
            filter.Text.clear();
            filter.Frames = 0;
        }
        ++filter.Frames;
        bool enter = false;
        const float listWidth = std::max(width + style.ScrollbarSize, ImGui::GetContentRegionAvail().x);
        if (total >= kFilterFrom)
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            Layout::Field({}, { .Placement = Layout::LabelPlacement::None, .Width = listWidth });
            enter = InputTextWithHint("##typefilter", ICON_FA_SEARCH "  Filter types", filter.Text, ImGuiInputTextFlags_EnterReturnsTrue);
        }

        const float line = ImGui::GetTextLineHeightWithSpacing();
        float listHeight = 0.0f;
        bool any = false;
        for (const Scan::TypeSection& section : sections)
        {
            const auto shown = std::count_if(section.Entries.begin(), section.Entries.end(), [&](const Scan::TypeEntry& entry) { return Matches(section, entry, filter.Text); });
            if (shown == 0)
                continue;
            if (any)
                listHeight += 1.0f + style.ItemSpacing.y;
            listHeight += ImGui::GetFontSize() * 0.86f + style.ItemSpacing.y + static_cast<float>(shown) * line;
            any = true;
        }
        if (!any)
            listHeight = line;
        listHeight += Px(2.0f) - style.ItemSpacing.y;
        const float maxHeight = line * 16.0f;
        const bool scrolls = listHeight > maxHeight;

        bool changed = false;
        bool first = true;
        const Scan::TypeEntry* firstMatch = nullptr;
        // ∑ The list scrolls in its own child, so the filter field above it stays in place.
        ImGui::BeginChild("##typelist", ImVec2(listWidth, std::min(listHeight, maxHeight)), ImGuiChildFlags_None,
            scrolls ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        for (const Scan::TypeSection& section : sections)
        {
            const bool shown = std::any_of(section.Entries.begin(), section.Entries.end(), [&](const Scan::TypeEntry& entry) { return Matches(section, entry, filter.Text); });
            if (!shown)
                continue;
            MenuSection(section.Title.c_str(), first);
            first = false;
            ImGui::Indent(indent);
            for (const Scan::TypeEntry& entry : section.Entries)
            {
                if (!Matches(section, entry, filter.Text))
                    continue;
                if (!firstMatch)
                    firstMatch = &entry;
                const bool selected = entry.Choice.Same(choice);
                ImGui::PushID(entry.Label.c_str());
                ImGui::PushID(static_cast<int>(entry.Choice.Kind));
                if (ImGui::Selectable(entry.Label.c_str(), selected))
                {
                    choice = entry.Choice;
                    changed = true;
                    if (!keepOpen)
                        ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
                ImGui::PopID();
                // ∑ Not in the appearing frame: the popup has no size yet there, and the scroll ended at the bottom.
                if (selected && filter.Frames == 2)
                    ImGui::SetScrollHereY();
                if (entry.Tooltip)
                    ItemTooltip(entry.Tooltip);
                if (!entry.Detail.empty())
                {
                    const ImVec2 max = ImGui::GetItemRectMax();
                    const ImVec2 min = ImGui::GetItemRectMin();
                    const ImVec2 at(max.x - TextWidth(entry.Detail) - style.ItemInnerSpacing.x, min.y + (max.y - min.y - ImGui::GetFontSize()) * 0.5f);
                    ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ManifoldTheme::Muted), entry.Detail.c_str());
                }
            }
            ImGui::Unindent(indent);
        }
        if (!firstMatch)
            Muted("No type matches");
        ImGui::EndChild();
        if (firstMatch && enter)
        {
            choice = firstMatch->Choice;
            changed = true;
            if (!keepOpen)
                ImGui::CloseCurrentPopup();
        }
        return changed;
    }

    bool TypeCombo(const char* id, Scan::TypeChoice& choice, const std::function<Scan::TypeCatalogOptions()>& options, ImGuiComboFlags flags)
    {
        if (!ImGui::BeginCombo(id, Scan::TypeChoiceLabel(choice).c_str(), flags))
            return false;
        const bool changed = TypeComboItems(Scan::TypeCatalog(options()), choice);
        ImGui::EndCombo();
        return changed;
    }

    const Scan::TypeEntry* TypeMenuItems(const std::vector<Scan::TypeSection>& sections, const Scan::TypeChoice* current,
        const std::function<std::string(const Scan::TypeEntry&)>& detail, bool submenus)
    {
        const Scan::TypeEntry* clicked = nullptr;
        bool first = true;
        for (const Scan::TypeSection& section : sections)
        {
            if (section.Entries.empty())
                continue;
            if (submenus)
            {
                const bool holds = current && std::any_of(section.Entries.begin(), section.Entries.end(), [&](const Scan::TypeEntry& entry) { return entry.Choice.Same(*current); });
                if (!ImGui::BeginMenuEx(section.Title.c_str(), holds ? ICON_FA_CHECK : nullptr))
                    continue;
            }
            else
            {
                MenuSection(section.Title.c_str(), first);
                first = false;
            }
            for (const Scan::TypeEntry& entry : section.Entries)
            {
                const std::string shown = detail ? detail(entry) : entry.Detail;
                ImGui::PushID(static_cast<int>(entry.Choice.Kind));
                if (ImGui::MenuItem(entry.Label.c_str(), shown.empty() ? nullptr : shown.c_str(), current && entry.Choice.Same(*current)))
                    clicked = &entry;
                ImGui::PopID();
                if (entry.Tooltip)
                    ItemTooltip(entry.Tooltip);
            }
            if (submenus)
                ImGui::EndMenu();
        }
        return clicked;
    }
}
