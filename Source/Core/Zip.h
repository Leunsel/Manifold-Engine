#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Manifold::Zip
{
    struct Entry
    {
        std::string Name;   // as the archive has it, with '/' between folders ('\' is turned into '/')
        std::vector<std::uint8_t> Data;
        bool Folder = false;
    };

    // Reads every entry of a zip file held in memory: stored and deflated
    // entries, sizes from the central directory, each checked against its
    // CRC-32. No Zip64, no encryption, no spanned archives: the release zips
    // need none of it. Fails as a whole on the first entry it cannot read.
    std::optional<std::vector<Entry>> Read(std::span<const std::uint8_t> archive, std::string* error = nullptr, std::size_t limit = 512u * 1024u * 1024u);

    std::uint32_t Crc32(std::span<const std::uint8_t> data);
}
