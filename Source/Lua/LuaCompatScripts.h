#pragma once

#include <span>
#include <string>
#include <string_view>

namespace Manifold::Lua
{
    struct CompatScript
    {
        const char* Name;
        std::span<const std::string_view> Chunks;

        std::string Text() const;
    };

    std::span<const CompatScript> CompatScripts();
    std::span<const CompatScript> ManifoldScripts();
}
