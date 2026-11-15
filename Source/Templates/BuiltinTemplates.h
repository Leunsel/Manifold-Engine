#pragma once

#include <span>
#include <string>
#include <string_view>

namespace Manifold::Templates
{
    struct BuiltinFile
    {
        const char* Path;
        std::span<const std::string_view> Chunks;

        std::string Text() const;
    };

    std::span<const BuiltinFile> BuiltinFiles();
}
