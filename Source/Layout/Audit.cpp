#include "Layout/Audit.h"

#include "Layout/Region.h"
#include "Layout/Rules.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>

namespace Manifold::Layout::Audit
{
    namespace
    {
        struct State
        {
            bool Enabled = false;
            bool Overlay = true;
            std::function<void(const Problem&)> Reporter;
            std::map<std::uint64_t, Problem> Problems;
            std::map<ImGuiID, int> Appeared;   // root window → the frame it last appeared in
            ImGuiID HighlightWindow = 0;
            ImRect HighlightRect;
            int HighlightFrame = -10;
        };

        State& Get()
        {
            static State state;
            return state;
        }

        constexpr int kForgetAfter = 90;
        // ∑ a problem counts from its second frame: a row learns its cells'
        // widths in the frame they first appear in, and a window's content
        // size follows a frame late, so a first frame alone is not a problem
        constexpr unsigned kSeenToCount = 2;
        // ∑ a window that just appeared is still finding its size (a script
        // window takes its content's minimum width a frame or two later)
        constexpr int kAppearingFrames = 4;

        bool Settling(State& state, const ImGuiWindow* window, int frame)
        {
            const ImGuiWindow* root = window->RootWindow ? window->RootWindow : window;
            if (root->Appearing || window->Appearing)
                state.Appeared[root->ID] = frame;
            const auto known = state.Appeared.find(root->ID);
            return known != state.Appeared.end() && frame - known->second < kAppearingFrames;
        }

        // ∑ content coordinates (window position and scroll taken out), so a
        // problem keeps its key while the window moves or scrolls
        std::uint64_t KeyOf(const ImGuiWindow* window, Kind kind, const ImRect& bounds)
        {
            const int x = static_cast<int>(std::floor((bounds.Min.x - window->Pos.x + window->Scroll.x) / 8.0f));
            const int y = static_cast<int>(std::floor((bounds.Min.y - window->Pos.y + window->Scroll.y) / 8.0f));
            const int w = static_cast<int>(std::floor(bounds.GetWidth() / 8.0f));
            const int parts[4] = { static_cast<int>(kind), x, y, w };
            return (static_cast<std::uint64_t>(window->ID) << 32) ^ ImHashData(parts, sizeof(parts), 0);
        }
    }

    const char* KindName(Kind kind)
    {
        switch (kind)
        {
        case Kind::CellOverflow:
            return "Content Leaves Its Cell";
        case Kind::RowTooNarrow:
            return "Row Too Narrow";
        case Kind::BelowMinimum:
            return "Below Minimum Size";
        case Kind::WindowClipped:
            return "Content Cut Off";
        case Kind::ColumnClipped:
            return "Column Cuts Its Content";
        }
        return "Layout";
    }

    void SetEnabled(bool enabled)
    {
        State& state = Get();
        state.Enabled = enabled;
        if (!enabled)
            state.Problems.clear();
    }

    bool Enabled()
    {
        return Get().Enabled;
    }

    void SetOverlay(bool overlay)
    {
        Get().Overlay = overlay;
    }

    bool Overlay()
    {
        return Get().Overlay;
    }

    void SetReporter(std::function<void(const Problem&)> reporter)
    {
        Get().Reporter = std::move(reporter);
    }

    void NoteWindow(ImGuiWindow* window, Kind kind, const ImRect& rect, const ImRect& bounds, std::string_view detail)
    {
        State& state = Get();
        if (!state.Enabled || !window)
            return;
        const int frame = ImGui::GetFrameCount();
        if (Settling(state, window, frame))
            return;
        const std::uint64_t key = KeyOf(window, kind, bounds);
        const auto [found, added] = state.Problems.try_emplace(key);
        Problem& problem = found->second;
        if (added)
        {
            problem.What = kind;
            problem.Window = window->ID;
            problem.WindowName = window->RootWindow ? window->RootWindow->Name : window->Name;
            problem.FirstFrame = frame;
        }
        problem.Viewport = window->Viewport ? window->Viewport->ID : 0;
        problem.Rect = rect;
        problem.Bounds = bounds;
        problem.Amount = std::max(0.0f, rect.GetWidth() - bounds.GetWidth());
        problem.Detail.assign(detail);
        const bool again = problem.LastFrame != frame;
        if (again || added)
            ++problem.Seen;
        problem.LastFrame = frame;
        if ((again || added) && problem.Seen == kSeenToCount && state.Reporter)
            state.Reporter(problem);
    }

    void Note(Kind kind, const ImRect& rect, const ImRect& bounds, std::string_view detail)
    {
        if (Get().Enabled && GImGui)
            NoteWindow(GImGui->CurrentWindow, kind, rect, bounds, detail);
    }

    void EndFrame()
    {
        State& state = Get();
        if (!state.Enabled || !GImGui)
            return;
        ImGuiContext& g = *GImGui;
        const int frame = ImGui::GetFrameCount();
        for (ImGuiWindow* window : g.Windows)
        {
            if (!window->WasActive || window->Hidden || window->Collapsed || Settling(state, window, frame))
                continue;
            if (AutoSizedWidth(window) || (window->Flags & ImGuiWindowFlags_HorizontalScrollbar))
                continue;
            if (window->ScrollbarX || window->ContentSizeExplicit.x > 0.0f)
                continue;
            const float visible = window->ContentRegionRect.GetWidth();
            const float content = window->ContentSize.x;
            if (visible <= 0.0f || content <= visible + 1.0f)
                continue;
            const ImRect inner = window->InnerRect;
            const ImRect bounds(window->ContentRegionRect.Min.x, inner.Min.y, window->ContentRegionRect.Max.x, inner.Max.y);
            const ImRect drawn(bounds.Min.x, inner.Min.y, bounds.Min.x + content, inner.Max.y);
            char detail[96];
            std::snprintf(detail, sizeof(detail), "content is %.0f px wide, the window shows %.0f px", content, visible);
            NoteWindow(window, Kind::WindowClipped, drawn, bounds, detail);
        }
        // ∑ a table cell clips its content at the column's edge, so text
        // written with raw calls into a narrow column is cut without a trace
        // in the window's content size; the column's measured content shows it
        for (int i = 0; i < g.Tables.GetMapSize(); ++i)
        {
            ImGuiTable* table = g.Tables.TryGetMapData(i);
            if (!table || table->LastFrameActive != frame || !table->InnerWindow)
                continue;
            ImGuiWindow* window = table->InnerWindow;
            if (!window->WasActive || window->Hidden || window->Collapsed || Settling(state, window, frame))
                continue;
            for (int n = 0; n < table->ColumnsCount; ++n)
            {
                const ImGuiTableColumn& column = table->Columns[n];
                if (!column.IsEnabled || !column.IsVisibleX || (column.Flags & ImGuiTableColumnFlags_NoClip) || column.WidthGiven <= 0.0f)
                    continue;
                const float content = std::max(column.ContentMaxXFrozen, column.ContentMaxXUnfrozen);
                const float edge = column.ClipRect.Max.x;
                if (content <= edge + 1.0f)
                    continue;
                const ImRect bounds(column.ClipRect.Min.x, table->InnerRect.Min.y, edge, table->InnerRect.Max.y);
                const ImRect drawn(bounds.Min.x, bounds.Min.y, content, bounds.Max.y);
                const char* name = ImGui::TableGetColumnName(table, n);
                char detail[160];
                std::snprintf(detail, sizeof(detail), "column \"%s\" holds %.0f px, its content needs %.0f px", name && *name ? name : "#", edge - column.WorkMinX,
                    content - column.WorkMinX);
                NoteWindow(window, Kind::ColumnClipped, drawn, bounds, detail);
            }
        }
        for (auto it = state.Problems.begin(); it != state.Problems.end();)
        {
            if (it->second.LastFrame < frame - kForgetAfter)
                it = state.Problems.erase(it);
            else
                ++it;
        }
        if (!state.Overlay)
            return;
        const ImU32 red = IM_COL32(240, 70, 60, 220);
        const ImU32 amber = IM_COL32(245, 180, 70, 160);
        for (const auto& [key, problem] : state.Problems)
        {
            if (problem.LastFrame < frame - 1 || problem.Seen < kSeenToCount)
                continue;
            ImGuiViewport* viewport = ImGui::FindViewportByID(problem.Viewport);
            ImDrawList* draw = ImGui::GetForegroundDrawList(viewport ? viewport : ImGui::GetMainViewport());
            draw->AddRect(problem.Bounds.Min, problem.Bounds.Max, amber, 0.0f, 0, 1.0f);
            draw->AddRect(problem.Rect.Min, problem.Rect.Max, red, 0.0f, 0, 1.5f);
        }
        if (state.HighlightFrame >= frame - 1)
        {
            ImGuiWindow* window = ImGui::FindWindowByID(state.HighlightWindow);
            ImDrawList* draw = ImGui::GetForegroundDrawList(window && window->Viewport ? window->Viewport : ImGui::GetMainViewport());
            draw->AddRectFilled(state.HighlightRect.Min, state.HighlightRect.Max, IM_COL32(240, 70, 60, 60));
            draw->AddRect(state.HighlightRect.Min, state.HighlightRect.Max, red, 0.0f, 0, 2.5f);
        }
    }

    std::vector<Problem> Problems()
    {
        std::vector<Problem> list;
        for (const auto& [key, problem] : Get().Problems)
        {
            if (problem.Seen >= kSeenToCount)
                list.push_back(problem);
        }
        std::sort(list.begin(), list.end(), [](const Problem& a, const Problem& b) {
            if (a.WindowName != b.WindowName)
                return a.WindowName < b.WindowName;
            if (a.What != b.What)
                return a.What < b.What;
            return a.FirstFrame < b.FirstFrame;
        });
        return list;
    }

    void Clear()
    {
        Get().Problems.clear();
    }

    void Highlight(ImGuiID window, const ImRect& rect)
    {
        State& state = Get();
        state.HighlightWindow = window;
        state.HighlightRect = rect;
        state.HighlightFrame = GImGui ? ImGui::GetFrameCount() : 0;
    }
}
