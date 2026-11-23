#pragma once

#include "imgui.h"

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class Workspace;

    // ∑ The layout audit only sees what is on screen, so a window that nobody
    //   opened at a small size or on its second tab keeps its problems. The sweep
    //   (--layout-sweep=<report>) opens every window by its --open name, one at a
    //   time, at its minimum, a medium and a large size, on every tab of its tab
    //   bars and at several interface scales, collects what the audit finds and
    //   writes one report. It runs in a cloaked main window without platform
    //   windows, so nothing appears on screen and nothing takes the focus.
    class LayoutSweep
    {
    public:
        struct Host
        {
            virtual void SetSweepScale(float scale) = 0;
            virtual void SetSweepMainSize(int width, int height) = 0;

        protected:
            ~Host() = default;
        };

        // An empty list (or one with only "+name" entries, which are added) sweeps every window Workspace::WindowNames()
        // knows.
        LayoutSweep(std::filesystem::path report, std::vector<std::string> windows, std::vector<float> scales);

        void BeforeDraw(Workspace& workspace, Host& host);   // before the workspace draws
        void AfterDraw();                                    // after Layout::Audit::EndFrame
        bool Finished() const { return m_finished; }
        // Writes the report; false when the file could not be written.
        bool Write(std::string* error) const;
        std::size_t ProblemCount() const { return m_found.size(); }

    private:
        enum class Phase
        {
            Start,
            MainSize,
            CloseWindows,
            OpenWindow,
            FindWindows,
            SizeWindows,
            Tab,
            Next,
            Done
        };

        struct Finding
        {
            std::string Root;       // the window it belongs to
            std::string Window;     // the ImGui window (or child) that showed it
            std::set<std::string> Opened;   // the --open names that showed it ("main" for the main window steps)
            std::string Kind;
            std::string Detail;     // the first detail seen
            float Worst = 0.0f;
            std::set<std::string> Contexts;   // scale · size · tab
        };

        void Wait(int frames) { m_waitUntil = ImGui::GetFrameCount() + frames; }
        bool Waiting() const { return ImGui::GetFrameCount() < m_waitUntil; }
        void StartCollecting(std::string context);
        bool ApplySize();   // false while a window still has to leave its dock node

        std::filesystem::path m_report;
        std::vector<std::string> m_windows;
        std::vector<float> m_scales;
        std::vector<std::string> m_skipped;

        Phase m_phase = Phase::Start;
        bool m_finished = false;
        int m_waitUntil = 0;
        std::size_t m_scale = 0;
        std::size_t m_window = 0;
        int m_mainSize = 0;
        int m_size = 0;
        std::vector<ImGuiID> m_before;    // root windows shown before opening
        std::vector<ImGuiID> m_targets;   // root windows the opened name brought up
        struct TabStep
        {
            ImGuiID Bar = 0;
            ImGuiID Tab = 0;
            std::string Name;
        };
        std::vector<TabStep> m_tabs;
        std::size_t m_tab = 0;
        bool m_tabsListed = false;
        std::map<ImGuiID, ImGuiID> m_firstTabs;   // bar → tab selected when the window opened

        bool m_collecting = false;
        int m_collectFrom = 0;
        int m_collectUntil = 0;
        std::string m_context;
        std::string m_opened;
        bool m_mainOnly = false;
        std::map<std::string, Finding> m_found;
        int m_steps = 0;
    };
}
