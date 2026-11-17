#pragma once

#include "App/Diagnostics.h"
#include "Core/Profiler.h"
#include "UI/Workspace.h"

#include "imgui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Manifold::UI
{
    std::vector<Diagnostics::InfoGroup> InterfaceInfo();
    std::string DiagnosticsReport(Workspace& workspace);
    bool FrameReadout(std::string& text);

    class ProfilerWindow : public ToolWindow
    {
    public:
        enum Tab
        {
            OverviewTab,
            BreakdownTab,
            ThreadsTab,
            ChecksTab,
            InfoTab,
            TabCount
        };

        explicit ProfilerWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show(int tab = -1);

    private:
        struct EasedTop
        {
            double Shown = 0.0;
            double Target = 0.0;
            double Previous = 0.0;
            double ChangedAt = -100.0;
            double LowerSince = -1.0;
            double SeenAt = -100.0;

            double Follow(double needed, double now);
            float Fade(double now) const;
        };

        void Toolbar();
        void Overview();
        void Breakdown();
        void ThreadList();
        void ChecksList();
        void AppInfo();
        void Tiles();
        void FrameGraph(float height);
        void Legend();
        bool ProfilerOffCard();
        void RunAction(Diagnostics::Action action);
        void SaveReport();
        void RefreshChecks(bool force);
        void RefreshTree();
        int SelectedAgo() const;
        ImVec4 ZoneColor(int node) const;

        Workspace& m_workspace;
        bool m_open = true;
        bool m_bringToFront = true;
        int m_tab = OverviewTab;
        int m_requestedTab = -1;
        bool m_pinned = false;
        std::uint64_t m_pinnedFrame = 0;
        bool m_wholeFrame = false;
        bool m_onlyIssues = false;
        bool m_hideTiny = true;
        double m_sampledAt = -100.0;
        double m_threadsAt = -100.0;
        double m_checkedAt = -100.0;
        double m_infoAt = -100.0;
        double m_copiedAt = -100.0;
        std::vector<Diagnostics::Check> m_checks;
        std::vector<Diagnostics::InfoGroup> m_info;
        std::vector<Profiler::ZoneView> m_tree;
        std::vector<int> m_topLevel;
        std::vector<int> m_topOf;
        std::vector<float> m_scratch;
        EasedTop m_graphTop;
        std::vector<EasedTop> m_sparkTops;
    };
}
