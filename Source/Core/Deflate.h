#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Manifold::Deflate
{
    inline constexpr std::size_t kMaxInflated = 256u * 1024u * 1024u;
    inline constexpr std::size_t kMaxRatio = 1032;

    std::optional<std::vector<std::uint8_t>> Compress(std::span<const std::uint8_t> data);
    std::optional<std::vector<std::uint8_t>> Decompress(std::span<const std::uint8_t> packed, std::size_t limit = kMaxInflated);
    std::optional<std::vector<std::uint8_t>> DecodePng(std::span<const std::uint8_t> file, int& width, int& height);
}
