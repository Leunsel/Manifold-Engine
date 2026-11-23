#include "Layout/Rules.h"

#include <cmath>
#include <vector>

namespace Manifold::Layout
{
    namespace
    {
        std::vector<Rules>& Stack()
        {
            static std::vector<Rules> stack;
            return stack;
        }
    }

    Rules& Defaults()
    {
        static Rules rules;
        return rules;
    }

    const Rules& Current()
    {
        const std::vector<Rules>& stack = Stack();
        return stack.empty() ? Defaults() : stack.back();
    }

    RulesScope::RulesScope(const Rules& rules)
    {
        Stack().push_back(rules);
    }

    RulesScope::~RulesScope()
    {
        Stack().pop_back();
    }

    float Scale()
    {
        const float scale = ImGui::GetCurrentContext() ? ImGui::GetStyle().FontScaleDpi : 1.0f;
        return scale > 0.0f ? scale : 1.0f;
    }

    float Pt(float points)
    {
        return points * Scale();
    }

    ImVec2 Pt(ImVec2 points)
    {
        const float scale = Scale();
        return ImVec2(std::round(points.x * scale), std::round(points.y * scale));
    }

    float Gap(Space space)
    {
        const int index = static_cast<int>(space);
        return Pt(Current().Spacing[index < 0 || index > 5 ? 3 : index]);
    }

    float MinTextWidth()
    {
        // "Wi" stands for an average glyph pair; the ellipsis comes on top
        const float glyph = ImGui::CalcTextSize("Wi").x * 0.5f;
        return std::round(glyph * Current().MinTextChars + ImGui::CalcTextSize("...").x);
    }
}
