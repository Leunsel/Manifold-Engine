#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Ini
{
    struct Entry
    {
        std::string Key;
        std::string Value;
    };

    struct Section
    {
        std::string Name;
        std::vector<Entry> Entries;
    };

    std::vector<Section> Parse(std::string_view text);
    std::optional<std::string> ReadText(const std::filesystem::path& path);
    bool WriteAtomic(const std::filesystem::path& path, std::string_view text);
}
