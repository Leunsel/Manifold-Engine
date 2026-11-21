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

    inline constexpr int kMaxImageSide = 16384;
    inline constexpr std::uint64_t kMaxImagePixels = 64ull * 1024ull * 1024ull;

    // Refuses images wider or higher than kMaxImageSide or with more than maxPixels pixels before anything is allocated.
    std::optional<DecodedImage> DecodeImage(std::span<const std::uint8_t> bytes, std::uint64_t maxPixels = kMaxImagePixels);
}
