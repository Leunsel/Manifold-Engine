#pragma once

#include "imgui.h"

namespace Manifold::Layout
{
    // ∑ The one place for the measures every component uses. Values are in
    // points; components read them through the scaled accessors, so a
    // monitor's DPI never has to be thought of at the call site.
    enum class Space
    {
        None,
        Xs,
        S,
        M,
        L,
        Xl
    };

    struct Rules
    {
        float Spacing[6] = { 0.0f, 2.0f, 4.0f, 8.0f, 12.0f, 16.0f };
        ImVec2 CardPadding = ImVec2(10.0f, 8.0f);
        ImVec2 PanelPadding = ImVec2(8.0f, 6.0f);
        float CardRounding = 6.0f;
        float MinFieldWidth = 80.0f;
        float MinTextChars = 3.0f;             // a text never gets narrower than this many average glyphs plus the ellipsis
        ImVec2 MinCard = ImVec2(120.0f, 40.0f);
        float LabelColumnShare = 0.45f;        // a label column takes at most this share of the line
        float NarrowField = 120.0f;            // below this much room beside a label the label goes above its field
        float TooltipWidthEm = 32.0f;
        float KeyCapPadding = 5.0f;
        ImFont* Bold = nullptr;
        ImFont* Mono = nullptr;
    };

    Rules& Defaults();
    const Rules& Current();

    class RulesScope
    {
    public:
        explicit RulesScope(const Rules& rules);
        ~RulesScope();
        RulesScope(const RulesScope&) = delete;
        RulesScope& operator=(const RulesScope&) = delete;
    };

    float Scale();
    float Pt(float points);
    ImVec2 Pt(ImVec2 points);
    float Gap(Space space);
    float MinTextWidth();
}
