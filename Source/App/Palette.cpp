#include "App/Palette.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace Manifold
{
    namespace
    {
        constexpr int kMatch = 16;
        constexpr int kRun = 24;
        constexpr int kWordStart = 20;
        constexpr int kTextStart = 12;
        constexpr int kGap = 2;
        constexpr int kLeading = 1;
        constexpr int kMaxLeading = 12;
        constexpr int kPrefix = 40;
        constexpr int kExact = 100;
        constexpr int kRecent = 40;

        char Fold(char c)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool Separator(char c)
        {
            return !std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80;
        }

        bool WordStart(std::string_view text, std::size_t at)
        {
            if (at == 0)
                return true;
            const char before = text[at - 1];
            const char here = text[at];
            if (Separator(before) && !Separator(here))
                return true;
            // camelCase and digits after letters: "OpenCodeFilter", "Win64"
            if (std::islower(static_cast<unsigned char>(before)) && std::isupper(static_cast<unsigned char>(here)))
                return true;
            return std::isalpha(static_cast<unsigned char>(before)) && std::isdigit(static_cast<unsigned char>(here));
        }

        // The greedy match from one start, preferring word starts for each next
        // character when one is close.
        int ScoreFrom(std::string_view query, std::string_view text, std::size_t start, std::vector<int>& positions)
        {
            positions.clear();
            int score = 0;
            std::size_t at = start;
            std::size_t previous = std::string_view::npos;
            for (std::size_t q = 0; q < query.size(); ++q)
            {
                const char wanted = Fold(query[q]);
                std::size_t found = std::string_view::npos;
                // a word start further on beats a plain match right here, unless this one continues a run
                for (std::size_t i = at; i < text.size(); ++i)
                {
                    if (Fold(text[i]) != wanted)
                        continue;
                    if (found == std::string_view::npos)
                        found = i;
                    if (previous != std::string_view::npos && i == previous + 1)
                    {
                        found = i;
                        break;
                    }
                    if (WordStart(text, i))
                    {
                        if (found == i || i - found <= 8)
                            found = i;
                        break;
                    }
                }
                if (found == std::string_view::npos)
                    return -1;
                score += kMatch;
                if (WordStart(text, found))
                    score += kWordStart;
                if (found == 0)
                    score += kTextStart;
                if (previous != std::string_view::npos)
                {
                    if (found == previous + 1)
                        score += kRun;
                    else
                        score -= static_cast<int>(std::min<std::size_t>(found - previous - 1, 20)) * kGap;
                }
                else
                {
                    score -= static_cast<int>(std::min<std::size_t>(found, kMaxLeading)) * kLeading;
                }
                positions.push_back(static_cast<int>(found));
                previous = found;
                at = found + 1;
            }
            return score;
        }

        std::vector<std::string_view> Words(std::string_view text)
        {
            std::vector<std::string_view> words;
            std::size_t at = 0;
            while (at < text.size())
            {
                while (at < text.size() && text[at] == ' ')
                    ++at;
                const std::size_t end = std::min(text.find(' ', at), text.size());
                if (end > at)
                    words.push_back(text.substr(at, end - at));
                at = end;
            }
            return words;
        }

        bool ContainsFolded(std::string_view text, std::string_view word)
        {
            if (word.empty() || word.size() > text.size())
                return word.empty();
            for (std::size_t at = 0; at + word.size() <= text.size(); ++at)
            {
                std::size_t i = 0;
                while (i < word.size() && Fold(text[at + i]) == Fold(word[i]))
                    ++i;
                if (i == word.size())
                    return true;
            }
            return false;
        }

                bool Wanted(const PaletteItem& item, const PaletteQuery& query)
        {
            if (query.Only)
                return item.Kind == *query.Only;
            if (query.CommandsOnly)
                return item.Kind == PaletteKind::Command || item.Kind == PaletteKind::Window || item.Kind == PaletteKind::Setting;
            return true;
        }
    }

    const char* PaletteKindName(PaletteKind kind)
    {
        switch (kind)
        {
        case PaletteKind::Command: return "Command";
        case PaletteKind::Window: return "Window";
        case PaletteKind::Setting: return "Settings";
        case PaletteKind::Table: return "Table";
        case PaletteKind::Record: return "Record";
        }
        return "";
    }

    PaletteQuery ParsePaletteQuery(std::string_view text)
    {
        PaletteQuery query;
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);
        if (!text.empty() && text.front() == '>')
        {
            query.CommandsOnly = true;
            text.remove_prefix(1);
        }
        else if (!text.empty() && text.front() == '@')
        {
            query.Only = PaletteKind::Record;
            text.remove_prefix(1);
        }
        else if (!text.empty() && text.front() == '#')
        {
            query.Only = PaletteKind::Table;
            text.remove_prefix(1);
        }
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);
        while (!text.empty() && text.back() == ' ')
            text.remove_suffix(1);
        query.Text = std::string(text);
        return query;
    }

    // ∑ Every start of the query's first character is tried, so "fil" in
    //   "Find Static Pointers ... Code Filter" lands on "Filter", not on the
    //   F of "Find" with the rest strewn behind it.
    int FuzzyScore(std::string_view query, std::string_view text, std::vector<int>* positions)
    {
        if (query.empty())
            return 0;
        if (query.size() > text.size())
            return -1;
        int best = -1;
        std::vector<int> bestPositions;
        std::vector<int> tried;
        const char first = Fold(query.front());
        for (std::size_t start = 0; start < text.size(); ++start)
        {
            if (Fold(text[start]) != first)
                continue;
            const int score = ScoreFrom(query, text, start, tried);
            if (score > best)
            {
                best = score;
                bestPositions = tried;
            }
        }
        if (best < 0)
            return -1;
        if (text.size() >= query.size())
        {
            bool prefix = true;
            for (std::size_t i = 0; i < query.size() && prefix; ++i)
                prefix = Fold(query[i]) == Fold(text[i]);
            if (prefix)
                best += query.size() == text.size() ? kExact : kPrefix;
        }
        if (positions)
            *positions = std::move(bestPositions);
        return best;
    }

    std::vector<PaletteMatch> RankPalette(std::span<const PaletteItem> items, std::string_view text, std::span<const std::string> recent, std::size_t limit)
    {
        const PaletteQuery query = ParsePaletteQuery(text);
        const std::vector<std::string_view> words = Words(query.Text);
        const auto recentRank = [&](const PaletteItem& item) -> int
        {
            for (std::size_t i = 0; i < recent.size(); ++i)
            {
                if (recent[i] == item.Key)
                    return static_cast<int>(i);
            }
            return -1;
        };
        std::vector<PaletteMatch> matches;
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const PaletteItem& item = items[index];
            if (!Wanted(item, query))
                continue;
            PaletteMatch match;
            match.Index = index;
            const int rank = recentRank(item);
            if (words.empty())
            {
                if (item.Kind == PaletteKind::Record && query.Only != PaletteKind::Record)
                    continue;
                match.Score = rank >= 0 ? 100000 - rank : 0;
                matches.push_back(std::move(match));
                continue;
            }
            bool all = true;
            int score = 0;
            for (const std::string_view word : words)
            {
                std::vector<int> positions;
                const int inTitle = FuzzyScore(word, item.Title, &positions);
                // ∑ The detail only counts as a whole word part ("breakpoints" in
                //   a settings page's line): spread letters would match most lines.
                const int inDetail = inTitle < 0 && ContainsFolded(item.Detail, word) ? FuzzyScore(word, item.Detail) : -1;
                if (inTitle < 0 && inDetail < 0)
                {
                    all = false;
                    break;
                }
                if (inTitle >= 0)
                {
                    score += inTitle;
                    match.Highlight.insert(match.Highlight.end(), positions.begin(), positions.end());
                }
                else
                {
                    score += inDetail / 3;
                }
            }
            if (!all)
                continue;
            if (rank >= 0)
                score += kRecent - static_cast<int>(std::min<std::size_t>(rank, 19)) * 2;
            score -= static_cast<int>(item.Title.size() / 8);
            match.Score = score;
            std::sort(match.Highlight.begin(), match.Highlight.end());
            match.Highlight.erase(std::unique(match.Highlight.begin(), match.Highlight.end()), match.Highlight.end());
            matches.push_back(std::move(match));
        }
        std::stable_sort(matches.begin(), matches.end(), [&](const PaletteMatch& a, const PaletteMatch& b)
            {
                if (a.Score != b.Score)
                    return a.Score > b.Score;
                const PaletteItem& left = items[a.Index];
                const PaletteItem& right = items[b.Index];
                if (left.Kind != right.Kind)
                    return left.Kind < right.Kind;
                return left.Order < right.Order;
            });
        if (matches.size() > limit)
            matches.resize(limit);
        return matches;
    }

    void RememberPaletteKey(std::vector<std::string>& recent, const std::string& key, std::size_t keep)
    {
        recent.erase(std::remove(recent.begin(), recent.end(), key), recent.end());
        recent.insert(recent.begin(), key);
        if (recent.size() > keep)
            recent.resize(keep);
    }

    std::vector<std::string> SplitPaletteRecent(std::string_view text)
    {
        std::vector<std::string> keys;
        std::size_t at = 0;
        while (at < text.size())
        {
            const std::size_t end = std::min(text.find('|', at), text.size());
            if (end > at)
                keys.emplace_back(text.substr(at, end - at));
            at = end + 1;
        }
        return keys;
    }

    std::string JoinPaletteRecent(const std::vector<std::string>& recent)
    {
        std::string text;
        for (const std::string& key : recent)
        {
            if (key.find('|') != std::string::npos)
                continue;
            if (!text.empty())
                text += '|';
            text += key;
        }
        return text;
    }
}
