#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Manifold
{
    struct DecodedImage
    {
        int Width = 0;
        int Height = 0;
        std::vector<std::uint8_t> Rgba;
    };

    std::optional<DecodedImage> DecodeImage(std::span<const std::uint8_t> bytes);
}
