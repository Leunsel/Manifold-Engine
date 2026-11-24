#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    enum class PaletteKind : std::uint8_t
    {
        Command,
        Window,
        Setting,
        Table,
        Record
    };

    const char* PaletteKindName(PaletteKind kind);

    // One thing the command palette can do. Key stays the same between
    // sessions ("command:OpenTable", "table:<path>", "record:<id>") and is
    // what the recent list remembers.
    struct PaletteItem
    {
        PaletteKind Kind = PaletteKind::Command;
        std::string Key;
        std::string Title;
        std::string Detail;
        std::string Shortcut;
        const char* Icon = "";
        int Order = 0;
    };

    struct PaletteMatch
    {
        std::size_t Index = 0;
        int Score = 0;
        std::vector<int> Highlight;  // the characters of the title the query matched
    };

    struct PaletteQuery
    {
        std::string Text;
        std::optional<PaletteKind> Only;  // ">" commands (and windows, settings), "@" records, "#" tables
        bool CommandsOnly = false;
    };

    PaletteQuery ParsePaletteQuery(std::string_view text);

    // How well a query matches a text, as a fuzzy subsequence: every
    // character of the query in order, case ignored. Characters at the start
    // of a word and runs of them count more, gaps less. -1 when it does not
    // match. The matched positions go to `positions`.
    int FuzzyScore(std::string_view query, std::string_view text, std::vector<int>* positions = nullptr);

    // The items for a query, best first, at most `limit`. Every word of the
    // query must match the title or the detail. Without a query the recent
    // items come first, then the rest in their order (records only when asked
    // for with "@", they can be thousands).
    std::vector<PaletteMatch> RankPalette(std::span<const PaletteItem> items, std::string_view query, std::span<const std::string> recent, std::size_t limit);

    // Puts a key in front of the recent list.
    void RememberPaletteKey(std::vector<std::string>& recent, const std::string& key, std::size_t keep = 16);
    std::vector<std::string> SplitPaletteRecent(std::string_view text);
    std::string JoinPaletteRecent(const std::vector<std::string>& recent);
}
