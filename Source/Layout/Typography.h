#pragma once

#include "Layout/Containers.h"

#include "imgui.h"

#include <cstddef>
#include <string_view>

namespace Manifold::Layout
{
    // ∑ What text does when it is wider than its place. Ellipsis is the
    // default everywhere: the text ends in "..." and its tooltip shows all of
    // it. Wrap breaks it into lines at the place's width, Clip cuts it at the
    // edge (for columns of figures), Visible keeps its own width and is the
    // explicit way out (it is what a place without a bound gets anyway: an
    // auto-sizing window or an auto-fitting table column).
    enum class Overflow
    {
        Ellipsis,
        Wrap,
        Clip,
        Visible
    };

    struct TextStyle
    {
        ImVec4 Color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);   // alpha 0: the current text colour
        ImFont* Font = nullptr;
        float FontSize = 0.0f;                           // pixels; 0: the font's own
        Layout::Overflow Mode = Layout::Overflow::Ellipsis;
        Layout::Align Justify = Layout::Align::Start;
        float MaxWidth = 0.0f;                           // pixels; 0: the place's width
        bool Tooltip = true;                             // shows the whole text when it was shortened
        bool FrameLine = false;                          // sits on a framed widget's line (true in row cells anyway)
    };

    struct TextFit
    {
        std::size_t Bytes = 0;   // how much of the text is shown before the ellipsis
        float Width = 0.0f;      // what that and the ellipsis take
        bool Truncated = false;
    };

    // Pure measure (no drawing): how much of a single line fits into width with an ellipsis. Cuts at UTF-8 boundaries.
    TextFit FitText(ImFont* font, float size, std::string_view text, float width);

    // Returns true when the text was shortened.
    bool Text(std::string_view text, const TextStyle& style = {});
    bool Muted(std::string_view text, Overflow overflow = Overflow::Ellipsis);
    bool Label(std::string_view text, Overflow overflow = Overflow::Ellipsis);
    bool Heading(std::string_view text, Overflow overflow = Overflow::Ellipsis);
    bool Colored(std::string_view text, ImVec4 color, Overflow overflow = Overflow::Ellipsis);
    bool Wrapped(std::string_view text, ImVec4 color = ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    // the text's tooltip helper: wraps at the rules' tooltip width
    void TextTooltip(std::string_view text);
}
