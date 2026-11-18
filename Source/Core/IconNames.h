#pragma once

#include <span>
#include <string_view>

namespace Manifold
{
    struct IconName
    {
        std::string_view Name;
        std::string_view Glyph;
    };

    std::span<const IconName> IconNames();
    std::string_view IconGlyph(std::string_view name);
}
