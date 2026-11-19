#pragma once

#include "Target/UnityExplorer.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class MonoWindow : public ToolWindowBase
    {
    public:
        explicit MonoWindow(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }
        // Picks a class in the Classes tab once the class list is there.
        void ShowClass(std::string className) { m_pendingClass = std::move(className); }

    private:
        struct InspectorTab
        {
            std::vector<int> History;     // inspection ids, oldest first
            int Position = -1;
            bool Auto = false;
            double RefreshedAt = 0.0;
            double ShownMs = -1.0;         // the refresh time on screen, updated twice a second
            double ShownAt = -1.0;
            std::uint64_t Key = 0;
            int Current() const { return Position >= 0 && Position < static_cast<int>(History.size()) ? History[static_cast<std::size_t>(Position)] : 0; }
        };

        void RebuildFilter();
        void SelectClass(int index);
        void SyncUnity(Target::UnityExplorer& unity);
        bool ExplorerGate(Target::UnityExplorer& unity, const char* icon, const char* what);
        bool MenuBar();
        void MenuStatus(Target::MonoBridge& mono);
        void StatusLine();
        void ScenesTab(Target::UnityExplorer& unity);
        void SceneNode(Target::UnityExplorer& unity, int node);
        void ObjectsTab(Target::UnityExplorer& unity);
        void ClassesTab();
        void ClassPane();
        void MethodsPane();
        void FieldsPane();
        void InspectorPane(Target::UnityExplorer& unity);
        void InspectorPage(Target::UnityExplorer& unity, InspectorTab& tab);
        void GameObjectSection(Target::UnityExplorer& unity, const Target::UnityInspection& inspection);
        void TransformRows(Target::UnityExplorer& unity, const Target::UnityTransform& transform, int refresh);
        void MembersTable(Target::UnityExplorer& unity, const Target::UnityInspection& inspection);
        void ElementsTable(Target::UnityExplorer& unity, const Target::UnityInspection& inspection);
        void ValueCell(Target::UnityExplorer& unity, const Target::UnityInspection& inspection, int member, const Target::MonoShown& shown, bool editable);
        bool ObjectLink(const char* id, const std::string& text, std::uint64_t object);
        void OpenObject(std::uint64_t object, bool newTab, bool fromScan = false);
        void OpenStatics(std::uint64_t klass);
        void Navigate(InspectorTab& tab, int id);
        void CloseTab(std::size_t index);
        void CloseAll();
        std::string TabLabel(const InspectorTab& tab) const;
        const Target::UnityInspection* Snapshot(int id) const;

        Workspace& m_workspace;
        Engine& m_engine;
        std::uint64_t m_generation = ~0ull;
        std::vector<Target::MonoClass> m_classes;
        std::vector<int> m_filtered;
        std::string m_filter;
        std::string m_lastFilter = "\x01";
        int m_selected = -1;
        std::string m_selectedName;
        std::vector<Target::MonoMethod> m_methods;
        std::vector<Target::MonoField> m_fields;
        std::string m_parent;
        bool m_generic = false;
        std::string m_pendingClass;
        std::map<std::size_t, std::uint64_t> m_resolved;
        float m_listWidth = 0.0f;

        int m_leftTab = 0;               // 0 scenes, 1 objects, 2 classes
        int m_requestLeftTab = -1;
        bool m_showClass = false;        // the right side shows the picked class
        std::vector<InspectorTab> m_tabs;
        std::size_t m_activeTab = 0;
        int m_selectTab = -1;
        std::uint64_t m_nextTabKey = 1;
        std::uint64_t m_unityGeneration = 0;
        std::uint64_t m_monoGeneration = ~0ull;
        Target::UnityStatus m_status;
        double m_busySince = -1.0;         // when the current explorer job started, -1 while idle
        Target::UnityTree m_tree;
        Target::UnitySearchResult m_search;
        std::map<int, Target::UnityInspection> m_inspections;
        std::string m_treeFilter;
        std::uint64_t m_selectedObject = 0;
        std::string m_searchClass;
        std::string m_searchName;
        bool m_searchHidden = false;
        bool m_suggest = false;
        bool m_suggestHovered = false;
        std::string m_memberFilter;
        bool m_showFields = true;
        bool m_showProperties = true;
        bool m_showMethods = true;
        bool m_showInherited = true;
        int m_editInspection = 0;
        int m_editMember = -1;
        std::string m_editText;
        bool m_editFocus = false;
        int m_callInspection = 0;
        int m_callMember = -1;
        std::vector<std::string> m_callArgs;
        std::map<int, std::string> m_nameEdits;
        std::uint64_t m_dragTransform = 0;
        int m_dragRow = -1;
        float m_dragValue[3] = {};
        bool m_closedAll = false;
    };
}
