#include "UI/LayoutSweep.h"

#include "Core/Text.h"
#include "Layout/Audit.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace Manifold::UI
{
    namespace
    {
        // ∑ a step waits for the window to take its size and for rows to learn their cells' widths (a frame late), then
        //   past the audit's appearing grace (4 frames) and its second sighting, and then collects a few frames
        constexpr int kSettleFrames = 10;
        constexpr int kCollectFrames = 4;

        struct SizeStep
        {
            const char* Name;
            float Width;   // points; 0 = as small as the window allows
            float Height;
        };

        constexpr SizeStep kMainSizes[] = { { "main minimum", 0.0f, 0.0f }, { "main 1180×760 (first start)", 1180.0f, 760.0f }, { "main 1280×800", 1280.0f, 800.0f },
            { "main 1920×1080", 1920.0f, 1080.0f } };
        constexpr SizeStep kToolSizes[] = { { "minimum", 0.0f, 0.0f }, { "medium", 760.0f, 540.0f }, { "large", 1280.0f, 860.0f } };
        // the main window while tool windows are swept: room for the large size
        constexpr float kToolMainWidth = 1600.0f;
        constexpr float kToolMainHeight = 1000.0f;

        // "about" is the About popup, not a tool window; the sweep cannot close it
        bool Sweepable(const std::string& name)
        {
            return name != "about";
        }

        bool IsRoot(const ImGuiWindow* window)
        {
            return window->WasActive && !window->Hidden && !window->ParentWindow &&
                !(window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu));
        }

        std::vector<ImGuiID> ShownRoots()
        {
            std::vector<ImGuiID> roots;
            for (const ImGuiWindow* window : GImGui->Windows)
            {
                if (IsRoot(window))
                    roots.push_back(window->ID);
            }
            return roots;
        }

        // ∑ halfway down (or back to the top) in every scrollable window of the given roots (all when none are given):
        //   some things only show while a list is scrolled, like the address list's pinned group header
        void ScrollWindows(const std::vector<ImGuiID>* roots, bool halfway)
        {
            for (ImGuiWindow* window : GImGui->Windows)
            {
                if (!window->WasActive || window->Hidden || window->ScrollMax.y <= 0.0f)
                    continue;
                const ImGuiWindow* root = window->RootWindow ? window->RootWindow : window;
                if (roots && std::find(roots->begin(), roots->end(), root->ID) == roots->end())
                    continue;
                ImGui::SetScrollY(window, halfway ? std::round(window->ScrollMax.y * 0.5f) : 0.0f);
            }
        }

        std::string ScaleLabel(float scale)
        {
            return std::to_string(static_cast<int>(std::lround(scale * 100.0f))) + " %";
        }

        // the part of a column problem's detail that names the column, so all widths of one column are one finding
        std::string ColumnOf(const std::string& detail)
        {
            const std::size_t open = detail.find('"');
            const std::size_t close = open == std::string::npos ? std::string::npos : detail.find('"', open + 1);
            return close == std::string::npos ? std::string() : detail.substr(open + 1, close - open - 1);
        }
    }

    LayoutSweep::LayoutSweep(std::filesystem::path report, std::vector<std::string> windows, std::vector<float> scales)
        : m_report(std::move(report)), m_windows(std::move(windows)), m_scales(std::move(scales))
    {
        // ∑ "+name" adds to the full list (a window that needs an argument to show its content, "+templates:<Id>")
        std::vector<std::string> added;
        std::erase_if(m_windows, [&added](const std::string& name) {
            if (name.empty() || name.front() != '+')
                return false;
            added.push_back(name.substr(1));
            return true;
        });
        if (m_windows.empty())
            m_windows = Workspace::WindowNames();
        m_windows.insert(m_windows.end(), added.begin(), added.end());
        std::erase_if(m_windows, [this](const std::string& name) {
            if (Sweepable(name))
                return false;
            m_skipped.push_back(name + " (a popup, not a tool window)");
            return true;
        });
        if (m_scales.empty())
            m_scales = { 1.0f, 1.5f };
    }

    void LayoutSweep::StartCollecting(std::string context)
    {
        m_context = std::move(context);
        m_collectFrom = ImGui::GetFrameCount() + kSettleFrames;
        m_collectUntil = m_collectFrom + kCollectFrames;
        m_collecting = true;
        ++m_steps;
    }

    bool LayoutSweep::ApplySize()
    {
        ImGuiContext& g = *GImGui;
        bool docked = false;
        for (const ImGuiID id : m_targets)
        {
            ImGuiWindow* window = ImGui::FindWindowByID(id);
            if (window && window->DockNode)
            {
                ImGui::DockContextQueueUndockWindow(&g, window);
                docked = true;
            }
        }
        if (docked)
            return false;
        const SizeStep& step = kToolSizes[m_size];
        const ImGuiViewport* main = ImGui::GetMainViewport();
        for (const ImGuiID id : m_targets)
        {
            ImGuiWindow* window = ImGui::FindWindowByID(id);
            if (!window)
                continue;
            ImGui::SetWindowPos(window, ImVec2(main->Pos.x + Px(16.0f), main->Pos.y + Px(48.0f)), ImGuiCond_Always);
            // ∑ 1×1 asks for the smallest size: the window's size constraints (BeginToolWindow's minimum) give it its floor
            ImGui::SetWindowSize(window, step.Width > 0.0f ? Px(step.Width, step.Height) : ImVec2(1.0f, 1.0f), ImGuiCond_Always);
        }
        return true;
    }

    void LayoutSweep::BeforeDraw(Workspace& workspace, Host& host)
    {
        if (m_finished)
            return;
        const int frame = ImGui::GetFrameCount();
        if (m_collecting)
        {
            if (frame < m_collectUntil)
                return;
            m_collecting = false;
        }
        if (Waiting())
            return;
        ImGuiContext& g = *GImGui;
        const float scale = m_scale < m_scales.size() ? m_scales[m_scale] : 1.0f;
        switch (m_phase)
        {
        case Phase::Start:
            if (m_scale >= m_scales.size())
            {
                CloseToolWindows();
                m_phase = Phase::Done;
                Wait(2);
                return;
            }
            host.SetSweepScale(scale);
            CloseToolWindows();   // the last scale's last window
            m_mainSize = 0;
            m_phase = Phase::MainSize;
            Wait(3);
            return;

        case Phase::MainSize:
            if (m_scrollNext)
            {
                m_scrollNext = false;
                ScrollWindows(nullptr, true);
                StartCollecting(m_sizeContext + " · scrolled");
                return;
            }
            ScrollWindows(nullptr, false);
            if (m_mainSize < static_cast<int>(std::size(kMainSizes)))
            {
                const SizeStep& step = kMainSizes[m_mainSize++];
                host.SetSweepMainSize(static_cast<int>(std::lround(step.Width * scale)), static_cast<int>(std::lround(step.Height * scale)));
                m_opened = "main";
                m_mainOnly = true;
                m_sizeContext = ScaleLabel(scale) + " · " + step.Name;
                StartCollecting(m_sizeContext);
                m_scrollNext = true;
                return;
            }
            host.SetSweepMainSize(static_cast<int>(std::lround(kToolMainWidth * scale)), static_cast<int>(std::lround(kToolMainHeight * scale)));
            m_mainOnly = false;
            m_window = 0;
            m_phase = Phase::CloseWindows;
            Wait(3);
            return;

        case Phase::CloseWindows:
            if (m_window >= m_windows.size())
            {
                ++m_scale;
                m_phase = Phase::Start;
                return;
            }
            CloseToolWindows();
            m_phase = Phase::OpenWindow;
            Wait(3);
            return;

        case Phase::OpenWindow:
            m_before = ShownRoots();
            m_opened = m_windows[m_window];
            if (!workspace.OpenNamed(m_opened))
            {
                if (m_scale == 0)
                    m_skipped.push_back(m_opened + " (unknown name)");
                m_phase = Phase::Next;
                return;
            }
            m_phase = Phase::FindWindows;
            Wait(4);
            return;

        case Phase::FindWindows:
        {
            m_targets.clear();
            for (const ImGuiID id : ShownRoots())
            {
                if (std::find(m_before.begin(), m_before.end(), id) == m_before.end())
                    m_targets.push_back(id);
            }
            if (m_targets.empty())
            {
                if (m_scale == 0)
                    m_skipped.push_back(m_opened + " (no window appeared)");
                m_phase = Phase::Next;
                return;
            }
            m_size = 0;
            m_phase = Phase::SizeWindows;
            return;
        }

        case Phase::SizeWindows:
            if (m_size >= static_cast<int>(std::size(kToolSizes)))
            {
                m_phase = Phase::Next;
                return;
            }
            if (!ApplySize())
            {
                Wait(2);
                return;
            }
            m_tabs.clear();
            m_tab = 0;
            m_tabsListed = false;
            m_firstTabs.clear();
            m_scrollNext = true;
            m_sizeContext = ScaleLabel(scale) + " · " + kToolSizes[m_size].Name;
            StartCollecting(m_sizeContext);
            m_phase = Phase::Tab;
            return;

        case Phase::Tab:
        {
            if (!m_tabsListed)
            {
                m_tabsListed = true;
                for (int i = 0; i < g.TabBars.GetMapSize(); ++i)
                {
                    ImGuiTabBar* bar = g.TabBars.TryGetMapData(i);
                    if (!bar || (bar->Flags & ImGuiTabBarFlags_DockNode) || !bar->Window || bar->CurrFrameVisible < frame - 1 || bar->Tabs.Size < 2)
                        continue;
                    const ImGuiWindow* root = bar->Window->RootWindow;
                    if (!root || std::find(m_targets.begin(), m_targets.end(), root->ID) == m_targets.end())
                        continue;
                    m_firstTabs[bar->ID] = bar->SelectedTabId;
                    for (ImGuiTabItem& tab : bar->Tabs)
                    {
                        if (tab.ID != bar->SelectedTabId)
                            m_tabs.push_back({ bar->ID, tab.ID, Layout::Audit::VisibleName(ImGui::TabBarGetTabName(bar, &tab)) });
                    }
                }
            }
            if (m_tab < m_tabs.size())
            {
                const TabStep& step = m_tabs[m_tab++];
                ImGuiTabBar* bar = g.TabBars.GetByKey(step.Bar);
                ImGuiTabItem* tab = bar ? ImGui::TabBarFindTabByID(bar, step.Tab) : nullptr;
                if (!tab)
                    return;
                ImGui::TabBarQueueFocus(bar, tab);
                StartCollecting(ScaleLabel(scale) + " · " + kToolSizes[m_size].Name + " · tab " + step.Name);
                return;
            }
            for (const auto& [barId, tabId] : m_firstTabs)
            {
                ImGuiTabBar* bar = g.TabBars.GetByKey(barId);
                if (ImGuiTabItem* tab = bar ? ImGui::TabBarFindTabByID(bar, tabId) : nullptr)
                    ImGui::TabBarQueueFocus(bar, tab);
            }
            if (!m_firstTabs.empty())
            {
                // back on the first tabs before scrolling, so their lists are the ones that scroll
                m_firstTabs.clear();
                Wait(3);
                return;
            }
            if (m_scrollNext)
            {
                m_scrollNext = false;
                ScrollWindows(&m_targets, true);
                StartCollecting(m_sizeContext + " · scrolled");
                return;
            }
            ScrollWindows(&m_targets, false);
            ++m_size;
            m_phase = Phase::SizeWindows;
            Wait(3);
            return;
        }

        case Phase::Next:
            ++m_window;
            m_phase = Phase::CloseWindows;
            return;

        case Phase::Done:
            m_finished = true;
            return;
        }
    }

    void LayoutSweep::AfterDraw()
    {
        if (!m_collecting)
            return;
        const int frame = ImGui::GetFrameCount();
        if (frame < m_collectFrom)
            return;
        for (const Layout::Audit::Problem& problem : Layout::Audit::Problems())
        {
            if (problem.LastFrame != frame)
                continue;
            const ImGuiWindow* window = ImGui::FindWindowByID(problem.Window);
            if (!window)
                continue;
            const ImGuiWindow* root = window->RootWindow ? window->RootWindow : window;
            if (!m_mainOnly && std::find(m_targets.begin(), m_targets.end(), root->ID) == m_targets.end())
                continue;
            const std::string label = problem.Place.empty() ? Layout::Audit::PlaceName(window) : problem.Place;
            const std::string kind = Layout::Audit::KindName(problem.What);
            const std::string column = problem.What == Layout::Audit::Kind::ColumnClipped ? ColumnOf(problem.Detail) : std::string();
            const std::string key = label + '\x1f' + kind + '\x1f' + column;
            Finding& finding = m_found[key];
            if (finding.Kind.empty())
            {
                finding.Root = Layout::Audit::VisibleName(root->Name);
                finding.Window = label;
                finding.Kind = kind;
            }
            finding.Opened.insert(m_opened);
            if (problem.Amount >= finding.Worst || finding.Detail.empty())
            {
                finding.Worst = problem.Amount;
                finding.Detail = problem.Detail;
            }
            finding.Contexts.insert(m_context);
        }
    }

    bool LayoutSweep::Write(std::string* error) const
    {
        std::vector<const Finding*> findings;
        for (const auto& [key, finding] : m_found)
            findings.push_back(&finding);
        std::sort(findings.begin(), findings.end(), [](const Finding* a, const Finding* b) {
            if (a->Root != b->Root)
                return a->Root < b->Root;
            return a->Worst > b->Worst;
        });

        std::string text = "Manifold Engine layout sweep\n";
        text += "Scales:";
        for (const float scale : m_scales)
            text += " " + ScaleLabel(scale);
        text += "\nWindows: main";
        for (const std::string& name : m_windows)
            text += ", " + name;
        text += "\nSteps: " + std::to_string(m_steps) + "\n";
        for (const std::string& skipped : m_skipped)
            text += "Skipped: " + skipped + "\n";
        text += "\n" + std::to_string(findings.size()) + (findings.size() == 1 ? " problem\n" : " problems\n");
        std::string root;
        for (const Finding* finding : findings)
        {
            if (finding->Root != root)
            {
                root = finding->Root;
                text += "\n[" + root + "]\n";
            }
            text += "  " + finding->Window + ": " + finding->Kind + Format(", up to %.0f px", finding->Worst) + "\n";
            text += "    " + finding->Detail + "\n";
            std::string seen;
            for (const std::string& context : finding->Contexts)
                seen += (seen.empty() ? "" : "; ") + context;
            text += "    seen at " + seen + "\n";
            std::string names;
            for (const std::string& name : finding->Opened)
                names += (names.empty() ? "" : ", ") + name;
            text += "    opened as " + names + "\n";
        }

        std::ofstream file(m_report, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        if (!file && error)
            *error = "could not write " + WideToUtf8(m_report.wstring());
        return static_cast<bool>(file);
    }
}
