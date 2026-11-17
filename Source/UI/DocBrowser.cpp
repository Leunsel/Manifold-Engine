#include "UI/DocBrowser.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/DocView.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>

namespace Manifold::UI
{
    namespace
    {
        bool ByName(const Lua::DocPtr& a, const Lua::DocPtr& b)
        {
            const std::string left = Lower(a->Path);
            const std::string right = Lower(b->Path);
            return left != right ? left < right : a->Path < b->Path;
        }

        constexpr const char* kStandardTables[] = { "string", "table", "math", "os", "io", "coroutine", "debug", "utf8" };
    }

    std::string InsertText(const Lua::DocRecord& record)
    {
        const std::string name = record.OwnerIsClass ? record.Name : record.Path;
        return record.Callable() ? name + "(" : name;
    }

    void DocBrowser::Rebuild(const Lua::DocRegistry& registry)
    {
        m_groups.clear();
        Group manifold{ "manifold", {}, false };
        Group modules{ "Modules", {}, false };
        Group lua{ "Lua", {}, false };
        Group globals{ "Lua functions", {}, true };
        Group cheat{ "Cheat Engine functions", {}, true };
        Group classes{ "Cheat Engine classes", {}, false };
        if (const Lua::DocPtr root = registry.Find("manifold"))
            manifold.Items.push_back(root);
        for (const Lua::DocPtr& record : registry.All())
        {
            if (record->Hidden)
                continue;
            if (record->Kind == Lua::DocItemKind::Table && record->Owner.empty() && (record->Origin == Lua::DocOrigin::Script || record->Origin == Lua::DocOrigin::Annotation))
                modules.Items.push_back(record);
            else if (record->Origin == Lua::DocOrigin::Standard && record->Owner.empty() && record->Kind != Lua::DocItemKind::Keyword)
                (record->Kind == Lua::DocItemKind::Table ? lua : globals).Items.push_back(record);
            else if (record->Origin == Lua::DocOrigin::CheatEngine && record->Owner.empty())
                (record->Kind == Lua::DocItemKind::Class ? classes : cheat).Items.push_back(record);
        }
        std::sort(modules.Items.begin(), modules.Items.end(), ByName);
        std::sort(lua.Items.begin(), lua.Items.end(), [](const Lua::DocPtr& a, const Lua::DocPtr& b)
            {
                const auto rank = [](const std::string& path)
                {
                    return static_cast<int>(std::find(std::begin(kStandardTables), std::end(kStandardTables), path) - std::begin(kStandardTables));
                };
                return rank(a->Path) < rank(b->Path);
            });
        std::sort(globals.Items.begin(), globals.Items.end(), ByName);
        std::sort(cheat.Items.begin(), cheat.Items.end(), ByName);
        std::sort(classes.Items.begin(), classes.Items.end(), ByName);
        for (Group* group : { &manifold, &modules, &lua, &globals, &cheat, &classes })
        {
            if (!group->Items.empty())
                m_groups.push_back(std::move(*group));
        }
        m_generation = registry.Generation();
    }

    void DocBrowser::Leaf(const Lua::DocPtr& record, const std::string& label, const std::function<void(const std::string&)>& insert)
    {
        const bool selected = m_selected == record;
        ImGui::PushID(record.get());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float indent = ImGui::CalcTextSize(ICON_FA_CUBE).x + Px(6.0f);
        if (ImGui::Selectable(("##leaf" + record->Path).c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
        {
            m_selected = record;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && insert)
                insert(InsertText(*record));
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(at, ImGui::GetColorU32(DocKindColor(record->Kind)), DocKindIcon(record->Kind));
        const std::string text = label + (record->IsDeprecated ? "  (deprecated)" : "");
        draw->AddText(ImVec2(at.x + indent, at.y), ImGui::GetColorU32(record->IsDeprecated ? ManifoldTheme::Muted : ManifoldTheme::Text), text.c_str());
        if (!record->Summary.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetItemTooltip("%s", record->Summary.c_str());
        ImGui::PopID();
    }

    void DocBrowser::Branch(const Lua::DocRegistry& registry, const Lua::DocPtr& record, const std::function<void(const std::string&)>& insert, int depth)
    {
        const std::string label = depth == 0 ? record->Path : record->Name;
        const bool open = ImGui::TreeNodeEx(record.get(), ImGuiTreeNodeFlags_SpanAvailWidth | (m_selected == record ? ImGuiTreeNodeFlags_Selected : 0), "%s  %s",
            DocKindIcon(record->Kind), label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            m_selected = record;
        if (!open)
            return;
        std::vector<Lua::DocPtr> members = record->Kind == Lua::DocItemKind::Class ? registry.ClassMembers(record->Path) : registry.Members(record->Path);
        if (record->Origin != Lua::DocOrigin::Manifold)
            std::sort(members.begin(), members.end(), ByName);
        for (const Lua::DocPtr& member : members)
        {
            if (member->Hidden)
                continue;
            if ((member->Kind == Lua::DocItemKind::Table || member->Kind == Lua::DocItemKind::Class) && depth < 4)
                Branch(registry, member, insert, depth + 1);
            else
                Leaf(member, member->Name, insert);
        }
        ImGui::TreePop();
    }

    void DocBrowser::Draw(const Lua::DocRegistry& registry, ImVec2 size, const std::function<void(const std::string&)>& insert)
    {
        if (registry.Generation() != m_generation)
            Rebuild(registry);
        ImGui::BeginChild("##docbrowser", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemWidth(-1.0f);
        if (m_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSearch = false;
        }
        InputTextWithHint("##docsearch", ICON_FA_SEARCH " Search the documentation", m_search);
        const float available = ImGui::GetContentRegionAvail().y;
        float tree = std::floor(available * m_treeShare);
        float detail = available - tree - Px(6.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::BeginChild("##doctree", ImVec2(0.0f, tree), ImGuiChildFlags_None);
        const std::string query(TrimView(m_search));
        if (!query.empty())
        {
            if (query != m_resultsQuery || registry.Generation() != m_resultsGeneration)
            {
                m_results = registry.Search(query, 300);
                m_resultsQuery = query;
                m_resultsGeneration = registry.Generation();
            }
            if (m_results.empty())
                Muted("Nothing matches.");
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_results.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const Lua::DocPtr& record = m_results[static_cast<std::size_t>(i)];
                    Leaf(record, record->Path, insert);
                }
            }
        }
        else
        {
            for (const Group& group : m_groups)
            {
                if (group.Items.size() == 1 && group.Title == group.Items.front()->Path)
                {
                    Branch(registry, group.Items.front(), insert, 0);
                    continue;
                }
                if (!ImGui::TreeNodeEx(group.Title.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  %s  (%zu)", group.Flat ? ICON_FA_LIST : ICON_FA_FOLDER,
                        group.Title.c_str(), group.Items.size()))
                    continue;
                if (group.Flat)
                {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(group.Items.size()));
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const Lua::DocPtr& record = group.Items[static_cast<std::size_t>(i)];
                            Leaf(record, record->Path, insert);
                        }
                    }
                }
                else
                {
                    for (const Lua::DocPtr& record : group.Items)
                        Branch(registry, record, insert, 0);
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();
        ImGui::SetCursorPos(start);
        if (Splitter("##docsplit", false, Px(6.0f), &tree, &detail, Px(80.0f), Px(80.0f)))
            m_treeShare = tree / std::max(1.0f, available);
        ImGui::SetCursorPos(ImVec2(start.x, start.y + tree + Px(6.0f)));
        ImGui::BeginChild("##docdetail", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        if (m_selected)
        {
            const float width = ImGui::GetContentRegionAvail().x - Px(4.0f);
            if (insert && ImGui::SmallButton(ICON_FA_PLUS "  Insert"))
                insert(InsertText(*m_selected));
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_COPY "  Copy"))
            {
                const std::string signature = m_selected->Callable() && !m_selected->Signatures.empty() ? Lua::FormatCall(*m_selected, 0, true) : m_selected->Path;
                ImGui::SetClipboardText(signature.c_str());
            }
            ImGui::Spacing();
            DocViewOptions options;
            options.Width = std::max(Px(120.0f), width);
            DocView(*m_selected, options);
        }
        else
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextColored(ManifoldTheme::Muted, "Pick an entry to read its documentation. A double click inserts it into the input.");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }
}
