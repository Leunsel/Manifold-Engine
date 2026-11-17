#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Target/Mono.h"
#include "Target/UnityExplorer.h"
#include "UI/AddressActions.h"
#include "UI/Loading.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <optional>

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

    void MonoWindow::SyncUnity(Target::UnityExplorer& unity)
    {
        if (unity.Generation() == m_unityGeneration)
            return;
        m_unityGeneration = unity.Generation();
        m_status = unity.State();
        m_tree = unity.Tree();
        m_search = unity.SearchResult();
        std::map<int, Target::UnityInspection> snapshots;
        for (const InspectorTab& tab : m_tabs)
        {
            for (const int id : tab.History)
            {
                if (const auto inspection = unity.Inspection(id))
                    snapshots.emplace(id, *inspection);
            }
        }
        m_inspections = std::move(snapshots);
    }

    const Target::UnityInspection* MonoWindow::Snapshot(int id) const
    {
        const auto found = m_inspections.find(id);
        return found == m_inspections.end() ? nullptr : &found->second;
    }

    void MonoWindow::Draw()
    {
        if (!m_open)
        {
            if (!m_closedAll)
                CloseAll();
            return;
        }
        m_closedAll = false;
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_DICE_D6 "  Mono / .NET###mono", &m_open, ImVec2(1040.0f, 620.0f), 0, ImVec2(620.0f, 360.0f)))
        {
            EndToolWindow();
            return;
        }
        if (!m_engine.HasProcess())
        {
            m_workspace.NoProcessState(ICON_FA_DICE_D6, "Open a Mono-based Unity game to explore its scenes, objects and classes.");
            EndToolWindow();
            return;
        }

        Target::MonoBridge& mono = m_engine.Mono();
        const std::optional<WorkInfo> work = m_engine.Work().Info("mono");
        if (!mono.Present())
        {
            if (!m_tabs.empty())
            {
                m_tabs.clear();
                m_inspections.clear();
            }
            if (work)
            {
                if (LoadingCard(ICON_FA_DICE_D6, *work, "You can keep working while the classes are read."))
                    m_engine.CancelWork(work->Id);
                EndToolWindow();
                return;
            }
            std::vector<EmptyAction> actions = { { ICON_FA_PLUG "  Load the Mono runtime", nullptr, true } };
            if (EmptyState(ICON_FA_DICE_D6, "No Mono runtime loaded",
                    "This reads the game's Mono runtime by running a small helper inside it. IL2CPP games are not supported.", actions, true) == 0)
                m_engine.LoadMono();
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
        Target::UnityExplorer& unity = mono.Unity();
        SyncUnity(unity);

        // Toolbar: what is loaded, the explorer's state, reload and shut down.
        // Items wrap onto a second line instead of running off a narrow window.
        ImGui::AlignTextToFramePadding();
        Muted(mono.Module() + "  ·  " + std::to_string(m_classes.size()) + " classes" +
            (m_status.UnityVersion.empty() ? std::string() : "  ·  Unity " + m_status.UnityVersion));
        if (work)
        {
            ImGui::SameLine();
            if (LoadingLine(*work))
                m_engine.CancelWork(work->Id);
        }
        else
        {
            FlowNext(SmallButtonWidth(ICON_FA_SYNC "  Reload"));
            if (ImGui::SmallButton(ICON_FA_SYNC "  Reload"))
                m_engine.ReloadMono();
            ItemTooltip("Reads the class list again, for assemblies the game loaded later");
            FlowNext(SmallButtonWidth(ICON_FA_POWER_OFF "  Shut down"));
            if (ImGui::SmallButton(ICON_FA_POWER_OFF "  Shut down"))
            {
                CloseAll();
                m_engine.StopMono();
                EndToolWindow();
                return;
            }
            ItemTooltip("Ends the helper in the game and takes the explorer's per-frame callback out again");
        }
        StatusLine(unity);

        // The window's minimum (620 px, held even when docked) fits both panes
        // at their minimums, so neither can be squeezed below a usable width.
        const float gap = Px(8.0f);
        const float minList = Px(250.0f);
        const float minInspector = Px(340.0f);
        if (m_listWidth <= 0.0f)
            m_listWidth = Px(360.0f);
        const float total = ImGui::GetContentRegionAvail().x;
        m_listWidth = std::clamp(m_listWidth, minList, std::max(minList, total - minInspector - gap));
        const float height = ImGui::GetContentRegionAvail().y;
        const ImVec2 start = ImGui::GetCursorPos();

        ImGui::BeginChild("##monoleft", ImVec2(m_listWidth, height), ImGuiChildFlags_Borders);
        if (ImGui::BeginTabBar("##monolefttabs"))
        {
            const auto tab = [&](const char* label, int index)
                {
                    ImGuiTabItemFlags flags = 0;
                    if (m_requestLeftTab == index)
                        flags |= ImGuiTabItemFlags_SetSelected;
                    if (!ImGui::BeginTabItem(label, nullptr, flags))
                        return false;
                    if (m_requestLeftTab == index)
                        m_requestLeftTab = -1;
                    if (m_leftTab != index)
                    {
                        m_leftTab = index;
                        m_showClass = index == 2 && m_selected >= 0;
                    }
                    return true;
                };
            if (tab(ICON_FA_SITEMAP "  Scenes", 0))
            {
                ScenesTab(unity);
                ImGui::EndTabItem();
            }
            if (tab(ICON_FA_SEARCH "  Objects", 1))
            {
                ObjectsTab(unity);
                ImGui::EndTabItem();
            }
            if (tab(ICON_FA_CUBES "  Classes", 2))
            {
                ClassesTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        float left = m_listWidth;
        float right = total - left - gap;
        ImGui::SetCursorPos(start);
        if (Splitter("##monosplit", true, gap, &left, &right, minList, minInspector, height))
            m_listWidth = left;
        ImGui::SetCursorPos(ImVec2(start.x + m_listWidth + gap, start.y));
        ImGui::BeginChild("##monoright", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        if (m_showClass && m_leftTab == 2 && m_selected >= 0)
            ClassPane();
        else
            InspectorPane(unity);
        ImGui::EndChild();
        EndToolWindow();
    }

    void MonoWindow::StatusLine(Target::UnityExplorer& unity)
    {
        // The running request goes LAST on the line and only once it has run
        // for a moment: with Auto, the inspector refreshes ten times a second,
        // and a spinner, task text and Stop button popping in front of the
        // other items made the whole line jump each time.
        const double now = ImGui::GetTime();
        if (!m_status.Busy)
            m_busySince = -1.0;
        else if (m_busySince < 0.0)
            m_busySince = now;
        const bool showBusy = m_status.Busy && now - m_busySince > 0.4;
        const char* const explorerOn = ICON_FA_EXCLAMATION_TRIANGLE " explorer on";
        const char* const keepLabel = "Keep running in the background";
        if (unity.Enabled())
        {
            FlowNext(ImGui::CalcTextSize(explorerOn).x);
            ImGui::TextColored(ManifoldTheme::Warning, "%s", explorerOn);
            ItemTooltip("Code runs inside the game while this is on. It can crash Unity 6 games; Shut down takes it back out.");
        }
        if (m_status.Hooked)
        {
            FlowNext(CheckboxWidth(keepLabel));
            const bool background = m_status.RunInBackground.value_or(false);
            bool keep = background;
            if (ImGui::Checkbox(keepLabel, &keep))
                unity.SetRunInBackground(keep);
            ItemTooltip("Sets Application.runInBackground, so the game keeps updating while Manifold is in front. Most requests need the game to run. "
                        "Shut down sets it back.");
        }
        if (showBusy)
        {
            // Spinner, task and Stop stay together when they wrap.
            const std::string task = m_status.Task.empty() ? std::string("Working") : m_status.Task;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            FlowNext(ImGui::GetTextLineHeight() * 0.5f + spacing + ImGui::CalcTextSize(task.c_str()).x + spacing + SmallButtonWidth(ICON_FA_STOP "  Stop"));
            Spinner(ImGui::GetTextLineHeight() * 0.5f, ManifoldTheme::Accent);
            ImGui::SameLine();
            Muted(task);
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_STOP "  Stop"))
                unity.Cancel();
            ItemTooltip("Stops the running request");
        }
        if (!m_status.Error.empty())
            TextWithError(ICON_FA_EXCLAMATION_TRIANGLE "  " + m_status.Error, ManifoldTheme::Warning);
    }

    // ---------------------------------------------------------------------
    // Scenes

    bool MonoWindow::ExplorerGate(Target::UnityExplorer& unity, const char* icon, const char* what)
    {
        if (unity.Enabled())
            return true;
        // Honest about the risk: this part runs code inside the game, and on
        // Unity 6 with the current Mono build that is still known to crash
        // games. The class list and Structure Dissect do not come through here.
        std::vector<EmptyAction> actions = { { ICON_FA_PLAY "  Turn the explorer on anyway", nullptr, true } };
        const int chose = EmptyState(icon, what,
            "This runs code inside the game to reach its main thread. On Unity 6 that is known to crash games after a few seconds - "
            "the cause is not fixed yet, so only use it on a game you can afford to lose, and save first.\n\n"
            "The class list, method addresses and Structure Dissect's Mono lookup do not use this and stay safe.",
            actions, false);
        if (chose == 0)
            unity.SetEnabled(true);
        return false;
    }

    void MonoWindow::ScenesTab(Target::UnityExplorer& unity)
    {
        if (!ExplorerGate(unity, ICON_FA_SITEMAP, "Scene explorer is off"))
            return;
        if (!m_tree.Read)
        {
            std::vector<EmptyAction> actions = { { ICON_FA_SITEMAP "  Read the scenes", nullptr, true } };
            if (EmptyState(ICON_FA_SITEMAP, "Scene explorer",
                    "Lists the loaded scenes and their GameObjects. Unity calls must run on the game's main thread, so this hangs a small callback on "
                    "Application.onBeforeRender in the game; Shut down takes it out again.",
                    actions, false) == 0)
                unity.ReadScenes();
            return;
        }
        if (IconButton(ICON_FA_SYNC, "Read the scenes again"))
            unity.ReadScenes();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##treefilter", "filter loaded objects", m_treeFilter);

        ImGui::BeginChild("##scenetree", ImVec2(0.0f, 0.0f));
        if (!m_treeFilter.empty())
        {
            std::vector<int> matches;
            for (int i = 0; i < static_cast<int>(m_tree.Nodes.size()); ++i)
            {
                if (ContainsNoCase(m_tree.Nodes[static_cast<std::size_t>(i)].Name, m_treeFilter))
                    matches.push_back(i);
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(matches.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const Target::UnityNode& node = m_tree.Nodes[static_cast<std::size_t>(matches[static_cast<std::size_t>(row)])];
                    ImGui::PushID(row);
                    if (ImGui::Selectable(node.Name.c_str(), node.Object == m_selectedObject))
                    {
                        m_selectedObject = node.Object;
                        OpenObject(node.Object, false);
                    }
                    ImGui::PopID();
                }
            }
            if (matches.empty())
                Muted("No loaded object matches. Only expanded parts of the tree are searched.");
            ImGui::EndChild();
            return;
        }
        for (int s = 0; s < static_cast<int>(m_tree.Scenes.size()); ++s)
        {
            const Target::UnityScene& scene = m_tree.Scenes[static_cast<std::size_t>(s)];
            ImGui::PushID(s);
            std::string label = scene.Name.empty() ? std::string("(unnamed scene)") : scene.Name;
            if (scene.RootCount >= 0)
                label += "  (" + std::to_string(scene.RootCount) + ")";
            if (scene.Active)
                label += "  · active";
            ImGui::PushStyleColor(ImGuiCol_Text, scene.DontDestroy ? ManifoldTheme::Label : ManifoldTheme::Accent);
            const bool open = ImGui::TreeNodeEx("##scene", ImGuiTreeNodeFlags_SpanAvailWidth | (scene.DontDestroy ? 0 : ImGuiTreeNodeFlags_DefaultOpen),
                "%s %s", ICON_FA_LAYER_GROUP, label.c_str());
            ImGui::PopStyleColor();
            if (scene.DontDestroy)
                ItemTooltip("Objects kept across scene loads. Unity does not list this scene; the first look moves an empty GameObject there, "
                            "reads its scene and destroys it again in the same frame.");
            if (open)
            {
                if (!scene.RootsLoaded && !scene.Loading)
                    unity.ExpandScene(s);
                if (scene.Loading && scene.Roots.empty())
                {
                    Spinner(ImGui::GetTextLineHeight() * 0.4f, ManifoldTheme::Accent);
                    ImGui::SameLine();
                    Muted("Reading...");
                }
                for (const int root : scene.Roots)
                    SceneNode(unity, root);
                if (scene.RootsLoaded && scene.Roots.empty())
                    Muted("No objects.");
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void MonoWindow::SceneNode(Target::UnityExplorer& unity, int index)
    {
        if (index < 0 || index >= static_cast<int>(m_tree.Nodes.size()))
            return;
        const Target::UnityNode& node = m_tree.Nodes[static_cast<std::size_t>(index)];
        ImGui::PushID(index);
        bool active = node.Active;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(2.0f), 0.0f));
        if (ImGui::Checkbox("##active", &active))
            unity.SetActive(node.Object, active, 0);
        ImGui::PopStyleVar();
        ItemTooltip(active ? "Active: click to deactivate (GameObject.SetActive)" : "Inactive: click to activate");
        ImGui::SameLine(0.0f, Px(4.0f));
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.ChildCount == 0)
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (node.Object == m_selectedObject)
            flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::PushStyleColor(ImGuiCol_Text, node.Active ? ManifoldTheme::Text : ManifoldTheme::Muted);
        const std::string label = node.ChildCount > 0 ? node.Name + "  (" + std::to_string(node.ChildCount) + ")" : node.Name;
        const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        {
            m_selectedObject = node.Object;
            OpenObject(node.Object, false);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
            OpenObject(node.Object, true);
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem(ICON_FA_EYE "  Inspect"))
                OpenObject(node.Object, false);
            if (ImGui::MenuItem(ICON_FA_EXTERNAL_LINK_ALT "  Inspect in a new tab"))
                OpenObject(node.Object, true);
            if (ImGui::MenuItem(node.Active ? ICON_FA_EYE_SLASH "  Deactivate" : ICON_FA_EYE "  Activate"))
                unity.SetActive(node.Object, !node.Active, 0);
            ImGui::Separator();
            AddressMenuItems(m_workspace, node.Object, ShowHex | DissectHere | CopyAddress, 8);
            ImGui::EndPopup();
        }
        if (open && node.ChildCount > 0)
        {
            if (!node.ChildrenLoaded && !node.Loading)
                unity.ExpandNode(index);
            if (node.Loading && node.Children.empty())
            {
                Spinner(ImGui::GetTextLineHeight() * 0.4f, ManifoldTheme::Accent);
                ImGui::SameLine();
                Muted("Reading...");
            }
            for (const int child : node.Children)
                SceneNode(unity, child);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    // ---------------------------------------------------------------------
    // Object search

    void MonoWindow::ObjectsTab(Target::UnityExplorer& unity)
    {
        if (!ExplorerGate(unity, ICON_FA_SEARCH, "Object search is off"))
            return;
        const float label = ImGui::CalcTextSize("Name").x + Px(14.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Class");
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-1.0f);
        const bool entered = InputTextWithHint("##searchclass", "UnityEngine.Camera, a MonoBehaviour, any class", m_searchClass, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemActive())
            m_suggest = true;
        const ImVec2 under(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
        const float width = ImGui::GetItemRectSize().x;
        bool start = entered;
        if (m_suggest && m_searchClass.size() >= 2)
        {
            // Matching class names under the field, the closest first.
            std::vector<int> matches;
            for (int i = 0; i < static_cast<int>(m_classes.size()) && matches.size() < 400; ++i)
            {
                if (ContainsNoCase(m_classes[static_cast<std::size_t>(i)].FullName(), m_searchClass))
                    matches.push_back(i);
            }
            std::stable_sort(matches.begin(), matches.end(), [&](int a, int b)
                {
                    const auto rank = [&](int i)
                        {
                            const Target::MonoClass& c = m_classes[static_cast<std::size_t>(i)];
                            if (EqualsNoCase(c.Name, m_searchClass) || EqualsNoCase(c.FullName(), m_searchClass))
                                return 0;
                            return StartsWithNoCase(c.Name, m_searchClass) ? 1 : 2;
                        };
                    return rank(a) < rank(b);
                });
            if (!matches.empty() && !(matches.size() == 1 && EqualsNoCase(m_classes[static_cast<std::size_t>(matches[0])].FullName(), m_searchClass)))
            {
                ImGui::SetNextWindowPos(under);
                ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(std::max(width, Px(360.0f)), Px(260.0f)));
                if (ImGui::Begin("##classsuggest", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_Tooltip))
                {
                    for (std::size_t i = 0; i < std::min<std::size_t>(matches.size(), 30); ++i)
                    {
                        const Target::MonoClass& c = m_classes[static_cast<std::size_t>(matches[i])];
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(c.FullName().c_str()))
                        {
                            m_searchClass = c.FullName();
                            m_suggest = false;
                        }
                        ImGui::SameLine();
                        Muted(c.Image);
                        ImGui::PopID();
                    }
                }
                ImGui::End();
            }
        }
        if (!ImGui::IsAnyItemActive() && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
            m_suggest = false;
        if (entered)
            m_suggest = false;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Name");
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-1.0f);
        start = InputTextWithHint("##searchname", "part of the object's name (Unity objects)", m_searchName, ImGuiInputTextFlags_EnterReturnsTrue) || start;
        ImGui::Checkbox("Inactive objects and assets too", &m_searchHidden);
        ItemTooltip("Unity objects: Resources.FindObjectsOfTypeAll instead of Object.FindObjectsOfType. Other classes are always found by a memory scan.");
        ImGui::SameLine();
        ImGui::BeginDisabled(m_searchClass.empty());
        start = AccentButton(ICON_FA_SEARCH "  Search") || start;
        ImGui::EndDisabled();
        if (start && !m_searchClass.empty())
        {
            m_suggest = false;
            unity.Search({ m_searchClass, m_searchName, m_searchHidden });
        }

        ImGui::Separator();
        if (m_search.Running)
        {
            Spinner(ImGui::GetTextLineHeight() * 0.4f, ManifoldTheme::Accent);
            ImGui::SameLine();
            Muted("Searching for " + m_search.Request.ClassName + "... " + std::to_string(m_search.Hits.size()) + " so far");
        }
        else if (!m_search.Error.empty())
            TextWithError(m_search.Error, ManifoldTheme::Error);
        else if (m_search.Done)
        {
            std::string summary = std::to_string(m_search.Hits.size()) + (m_search.Hits.size() == 1 ? " object" : " objects");
            if (m_search.Total > m_search.Hits.size())
                summary += " of " + std::to_string(m_search.Total);
            Muted(summary);
            if (m_search.HeapScan)
            {
                ImGui::SameLine();
                Muted("· found by a memory scan");
                ItemTooltip("Not a Unity object, so its instances were found by their class pointer in the game's memory. Some may be dead objects "
                            "the garbage collector has not reused yet; such hits only show fields, no properties or calls.");
            }
        }
        else
            Muted("Pick a class and search for its live objects.");

        if (m_search.Hits.empty())
            return;
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##searchhits", 3, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_search.Hits.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const Target::UnityHit& hit = m_search.Hits[static_cast<std::size_t>(row)];
                    ImGui::TableNextRow();
                    ImGui::PushID(row);
                    ImGui::TableNextColumn();
                    const std::string name = hit.Name.empty() ? "#" + std::to_string(row + 1) : hit.Name;
                    if (ImGui::Selectable(name.c_str(), hit.Object == m_selectedObject, ImGuiSelectableFlags_SpanAllColumns))
                    {
                        m_selectedObject = hit.Object;
                        OpenObject(hit.Object, false, m_search.HeapScan);
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
                        OpenObject(hit.Object, true, m_search.HeapScan);
                    if (ImGui::BeginPopupContextItem())
                    {
                        if (ImGui::MenuItem(ICON_FA_EXTERNAL_LINK_ALT "  Inspect in a new tab"))
                            OpenObject(hit.Object, true, m_search.HeapScan);
                        ImGui::Separator();
                        AddressMenuItems(m_workspace, hit.Object, ShowHex | DissectHere | CopyAddress, 8);
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    TextFitted(hit.Class, ImGui::GetContentRegionAvail().x, ManifoldTheme::Label);
                    ImGui::TableNextColumn();
                    PushMono();
                    ImGui::TextColored(ManifoldTheme::AddressText, "%s", Hex(hit.Object).c_str());
                    PopMono();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    // ---------------------------------------------------------------------
    // Classes

    void MonoWindow::ClassesTab()
    {
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##monofilter", "filter classes (name or assembly)", m_filter);
        if (m_filter != m_lastFilter)
            RebuildFilter();
        ImGui::BeginChild("##monoclasses", ImVec2(0.0f, 0.0f));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filtered.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const int index = m_filtered[static_cast<std::size_t>(row)];
                const Target::MonoClass& info = m_classes[static_cast<std::size_t>(index)];
                ImGui::PushID(index);
                if (ImGui::Selectable(info.FullName().c_str(), m_selected == index && m_showClass))
                {
                    SelectClass(index);
                    m_showClass = true;
                }
                if (ImGui::BeginPopupContextItem())
                {
                    const bool live = m_engine.Mono().Unity().Enabled();
                    if (ImGui::MenuItem(ICON_FA_EYE "  Static members", nullptr, false, live))
                        OpenStatics(info.Handle);
                    if (!live)
                        ItemTooltip("Needs the explorer, which is off: turn it on under Scenes or Objects");
                    if (ImGui::MenuItem(ICON_FA_SEARCH "  Find objects of this class", nullptr, false, live))
                    {
                        m_searchClass = info.FullName();
                        m_requestLeftTab = 1;
                        m_engine.Mono().Unity().Search({ m_searchClass, std::string(), m_searchHidden });
                    }
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the name"))
                        ImGui::SetClipboardText(info.FullName().c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void MonoWindow::ClassPane()
    {
        CardTitle(ICON_FA_DICE_D6, m_selectedName.c_str());
        ImGui::SameLine();
        const Target::MonoClass* info = m_selected >= 0 && m_selected < static_cast<int>(m_classes.size()) ? &m_classes[static_cast<std::size_t>(m_selected)] : nullptr;
        if (info)
        {
            if (ImGui::SmallButton(ICON_FA_EYE "  Static members"))
                OpenStatics(info->Handle);
            ItemTooltip("Opens the class in the inspector: static fields and properties with their values");
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_SEARCH "  Find objects"))
            {
                m_searchClass = info->FullName();
                m_requestLeftTab = 1;
                m_engine.Mono().Unity().Search({ m_searchClass, std::string(), m_searchHidden });
            }
            ItemTooltip("Lists the live objects of this class");
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_TIMES "##closeclass"))
                m_showClass = false;
            ItemTooltip("Back to the inspector");
        }
        MethodsPane();
        ImGui::Spacing();
        FieldsPane();
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
