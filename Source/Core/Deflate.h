#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Manifold::Deflate
{
    std::optional<std::vector<std::uint8_t>> Compress(std::span<const std::uint8_t> data);
    std::optional<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> packed);
    std::optional<std::vector<std::uint8_t>> DecodePng(std::span<const std::uint8_t> file, int& width, int& height);
}
