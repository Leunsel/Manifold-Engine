#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Layout::Audit
{
    // ∑ The audit finds what the layout rules could not prevent: content that
    // left its cell (raw ImGui calls inside a region), rows whose minimums do
    // not fit, windows narrower than their content needs, and any ImGui
    // window whose content is wider than the window without a horizontal
    // scrollbar (so it is cut off). Off by default; then nothing is recorded.
    enum class Kind
    {
        CellOverflow,
        RowTooNarrow,
        BelowMinimum,
        WindowClipped,
        ColumnClipped
    };

    struct Problem
    {
        Kind What = Kind::CellOverflow;
        ImGuiID Window = 0;
        std::string WindowName;
        ImGuiID Viewport = 0;
        ImRect Rect;      // what was drawn
        ImRect Bounds;    // where it had to stay
        float Amount = 0.0f;
        std::string Detail;
        int FirstFrame = 0;
        int LastFrame = 0;
        unsigned Seen = 0;
    };

    const char* KindName(Kind kind);

    void SetEnabled(bool enabled);
    bool Enabled();
    void SetOverlay(bool overlay);
    bool Overlay();
    void SetReporter(std::function<void(const Problem&)> reporter);   // called once for every new problem

    void Note(Kind kind, const ImRect& rect, const ImRect& bounds, std::string_view detail);
    void NoteWindow(ImGuiWindow* window, Kind kind, const ImRect& rect, const ImRect& bounds, std::string_view detail);
    // Scans every ImGui window, draws the overlay and drops problems not seen for a while. Once per frame, after the
    // interface is built and before ImGui::Render.
    void EndFrame();
    std::vector<Problem> Problems();
    void Clear();
    void Highlight(ImGuiID window, const ImRect& rect);   // the problem list's hover: drawn this frame
}
