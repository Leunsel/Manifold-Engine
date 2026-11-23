#pragma once

#include "Layout/Size.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace Manifold::Layout
{
    // ∑ A region is a cell of the layout: a rectangle content has to stay in.
    // While it is open the window's work rect ends at the region's right
    // edge, so every ImGui widget inside (fields with -FLT_MIN, progress bars,
    // tables, child windows, wrapped text, selectables) takes the region's
    // width instead of the window's, and a clip rect keeps drawing and mouse
    // hovering inside it. Components report what they need (natural and
    // minimum width) to the innermost region; a closing region passes its
    // totals on to its parent, so a window learns its content's minimum.
    // Reports on one line add up (with the gap between them), lines take the
    // widest: the minimum never depends on how wide the region is now, so a
    // window cannot grow by measuring itself.
    struct RegionOptions
    {
        float Width = 0.0f;       // pixels; 0 or less: what is available
        bool Clip = true;
        bool FrameLine = false;   // text in it sits on a framed widget's line (rows)
        ImGuiID Id = 0;           // where the measure is kept for the next frame (0: not kept)
        bool ReportToParent = true; // false: the owner reports for it (a row reports its cells' sum itself)
    };

    class Region
    {
    public:
        explicit Region(const RegionOptions& options = {});
        ~Region();
        Region(const Region&) = delete;
        Region& operator=(const Region&) = delete;

        static Region* Current();

        // an item at x, y (screen, the top of its line) that would like natural pixels and can live with minimum
        void Report(float x, float y, float natural, float minimum);
        float Left() const { return m_left; }
        float Right() const { return m_right; }
        bool FrameLine() const { return m_frameLine; }
        bool Reported() const { return m_reported; }
        Measure Reports() const { return { m_natural, m_minimum }; }
        ImVec2 Size() const { return m_size; }   // after End
        void End();

    private:
        ImGuiWindow* m_window = nullptr;
        Region* m_parent = nullptr;
        ImGuiID m_id = 0;
        float m_left = 0.0f;
        float m_right = 0.0f;
        float m_workMax = 0.0f;
        float m_contentMax = 0.0f;
        bool m_clip = true;
        bool m_frameLine = false;
        bool m_toParent = true;
        bool m_reported = false;
        bool m_ended = false;
        float m_natural = 0.0f;
        float m_minimum = 0.0f;
        float m_top = 0.0f;
        float m_lineY = -1.0e30f;
        float m_lineNatural = 0.0f;
        float m_lineMinimum = 0.0f;
        ImVec2 m_size;
    };

    // width from the cursor to the innermost bound (region, table cell or window)
    float Avail();
    // reports an item starting at the cursor to the innermost region, if any
    void Report(float natural, float minimum);
    // ∑ while one lives, reports are dropped: a component that reported its whole self (a field with its label) draws
    // its parts with other components without counting them twice
    class QuietReports
    {
    public:
        QuietReports();
        ~QuietReports();
        QuietReports(const QuietReports&) = delete;
        QuietReports& operator=(const QuietReports&) = delete;
    };
    // whether content here has a width it must keep to; false in auto-sizing windows and auto-fitting table columns,
    // where text keeps its own width so the window or column can grow to it
    bool Bounded();
    bool FrameLine();
    // ∑ whether the window's width follows its content: an auto-sizing window or tooltip, or a child window sized by
    // its content across (ImGui marks a child that only grows downwards as auto-sizing too, #9355)
    bool AutoSizedWidth(const ImGuiWindow* window);
    // what the region kept under id measured last frame
    Measure Measured(ImGuiID id);
    // ∑ declares the content of an auto-fitting window or column as this wide, without taking more room now
    void WantWidth(float x, float width);

    // Called by the host once per frame around building the interface: checks every region was closed.
    void NewFrame();
}
