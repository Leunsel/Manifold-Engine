#include "Text/LuaContext.h"

#include "Core/Text.h"
#include "Text/Highlight.h"

#include <algorithm>

namespace Manifold::Text
{
    namespace
    {
        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool IsSeparator(std::string_view text, std::size_t at)
        {
            return (text[at] == '.' || text[at] == ':') && !(at > 0 && text[at - 1] == '.');
        }
    }

    bool IsIdentifierChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '_';
    }

    bool InCode(std::string_view text, std::size_t cursor)
    {
        cursor = std::min(cursor, text.size());
        std::size_t lineStart = cursor;
        while (lineStart > 0 && text[lineStart - 1] != '\n')
            --lineStart;
        char quote = 0;
        for (std::size_t i = lineStart; i < cursor; ++i)
        {
            const char c = text[i];
            if (quote)
            {
                if (c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (c == '-' && i + 1 < cursor && text[i + 1] == '-')
                return false;
        }
        return quote == 0;
    }

    std::size_t ChainStart(std::string_view text, std::size_t end)
    {
        std::size_t position = std::min(end, text.size());
        while (true)
        {
            const std::size_t identifierEnd = position;
            while (position > 0 && IsIdentifierChar(text[position - 1]))
                --position;
            if (position == identifierEnd)
                return std::string::npos;
            if (position > 1 && text[position - 1] == '.' && text[position - 2] != '.')
            {
                --position;
                continue;
            }
            return position;
        }
    }

    WordContext WordAt(std::string_view text, std::size_t cursor)
    {
        WordContext context;
        cursor = std::min(cursor, text.size());
        if (!InCode(text, cursor))
            return context;
        std::size_t start = cursor;
        while (start > 0 && IsIdentifierChar(text[start - 1]))
            --start;
        if (start < cursor && IsDigit(text[start]))
            return context;
        context.Start = start;
        context.Prefix = std::string(text.substr(start, cursor - start));
        if (start > 0 && IsSeparator(text, start - 1))
        {
            const std::size_t chainStart = ChainStart(text, start - 1);
            if (chainStart == std::string::npos || IsDigit(text[chainStart]))
                return WordContext{};
            context.Chain = std::string(text.substr(chainStart, start - 1 - chainStart));
            context.Method = text[start - 1] == ':';
            context.Member = true;
        }
        context.Valid = true;
        return context;
    }

    CallContext CallAt(std::string_view text, std::size_t cursor)
    {
        CallContext context;
        cursor = std::min(cursor, text.size());
        std::size_t from = cursor > 2000 ? cursor - 2000 : 0;
        while (from > 0 && text[from - 1] != '\n')
            --from;
        struct Open
        {
            std::size_t At;
            char Kind;
            int Commas;
        };
        std::vector<Open> stack;
        char quote = 0;
        for (std::size_t i = from; i < cursor; ++i)
        {
            const char c = text[i];
            if (quote)
            {
                if (c == '\\')
                    ++i;
                else if (c == quote || c == '\n')
                    quote = 0;
                continue;
            }
            if (c == '-' && i + 1 < cursor && text[i + 1] == '-')
            {
                while (i < cursor && text[i] != '\n')
                    ++i;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (c == '(' || c == '{' || c == '[')
                stack.push_back({ i, c, 0 });
            else if ((c == ')' || c == '}' || c == ']') && !stack.empty())
                stack.pop_back();
            else if (c == ',' && !stack.empty())
                ++stack.back().Commas;
        }
        if (quote || stack.empty() || stack.back().Kind != '(')
            return context;
        std::size_t end = stack.back().At;
        while (end > 0 && text[end - 1] == ' ')
            --end;
        std::size_t start = end;
        while (start > 0 && IsIdentifierChar(text[start - 1]))
            --start;
        if (start == end || IsDigit(text[start]))
            return context;
        std::string chain(text.substr(start, end - start));
        if (start > 1 && IsSeparator(text, start - 1))
        {
            const std::size_t chainStart = ChainStart(text, start - 1);
            if (chainStart == std::string::npos)
                return context;
            context.Method = text[start - 1] == ':';
            chain = std::string(text.substr(chainStart, start - 1 - chainStart)) + "." + chain;
            start = chainStart;
        }
        std::size_t before = start;
        while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t'))
            --before;
        if (before >= 8 && text.compare(before - 8, 8, "function") == 0 && (before == 8 || !IsIdentifierChar(text[before - 9])))
            return context;
        if (IsLuaKeyword(chain))
            return context;
        context.Valid = true;
        context.Chain = std::move(chain);
        context.Argument = stack.back().Commas;
        context.Paren = stack.back().At;
        return context;
    }

    ChainContext ChainAt(std::string_view text, std::size_t position)
    {
        ChainContext context;
        if (position >= text.size() || !IsIdentifierChar(text[position]))
            return context;
        std::size_t start = position;
        while (start > 0 && IsIdentifierChar(text[start - 1]))
            --start;
        std::size_t end = position;
        while (end < text.size() && IsIdentifierChar(text[end]))
            ++end;
        if (IsDigit(text[start]) || !InCode(text, start))
            return context;
        context.Name = std::string(text.substr(start, end - start));
        if (IsLuaKeyword(context.Name))
            return ChainContext{};
        context.Chain = context.Name;
        context.Start = start;
        context.End = end;
        if (start > 1 && IsSeparator(text, start - 1))
        {
            const std::size_t chainStart = ChainStart(text, start - 1);
            if (chainStart == std::string::npos || IsDigit(text[chainStart]))
                return ChainContext{};
            context.Method = text[start - 1] == ':';
            context.Chain = std::string(text.substr(chainStart, start - 1 - chainStart)) + "." + context.Name;
            context.Start = chainStart;
        }
        context.Valid = true;
        return context;
    }

    std::string AssignedCall(std::string_view text, std::size_t before, std::string_view name)
    {
        if (name.empty())
            return {};
        before = std::min(before, text.size());
        const auto blank = [](char c) { return c == ' ' || c == '\t'; };
        std::string found;
        for (std::size_t at = text.find(name); at != std::string_view::npos && at < before; at = text.find(name, at + 1))
        {
            const std::size_t end = at + name.size();
            if ((at > 0 && (IsIdentifierChar(text[at - 1]) || text[at - 1] == '.' || text[at - 1] == ':')) || (end < text.size() && IsIdentifierChar(text[end])))
                continue;
            if (!InCode(text, at))
                continue;
            std::size_t i = end;
            while (i < text.size() && blank(text[i]))
                ++i;
            if (i >= text.size() || text[i] != '=' || (i + 1 < text.size() && text[i + 1] == '='))
                continue;
            ++i;
            while (i < text.size() && blank(text[i]))
                ++i;
            const std::size_t chainStart = i;
            while (i < text.size() && (IsIdentifierChar(text[i]) || text[i] == '.' || text[i] == ':'))
                ++i;
            const std::size_t chainEnd = i;
            while (i < text.size() && blank(text[i]))
                ++i;
            if (chainEnd == chainStart || i >= text.size() || (text[i] != '(' && text[i] != '"' && text[i] != '\'' && text[i] != '{') || i >= before)
                continue;
            found = std::string(text.substr(chainStart, chainEnd - chainStart));
        }
        return found;
    }

    std::vector<BufferFunction> BufferFunctions(std::string_view text)
    {
        std::vector<BufferFunction> result;
        for (std::size_t at = text.find("function"); at != std::string::npos; at = text.find("function", at + 8))
        {
            if ((at > 0 && IsIdentifierChar(text[at - 1])) || (at + 8 < text.size() && IsIdentifierChar(text[at + 8])))
                continue;
            std::size_t i = at + 8;
            while (i < text.size() && text[i] == ' ')
                ++i;
            std::string name;
            if (i < text.size() && text[i] == '(')
            {
                std::size_t back = at;
                while (back > 0 && text[back - 1] == ' ')
                    --back;
                if (back > 0 && text[back - 1] == '=')
                {
                    --back;
                    while (back > 0 && text[back - 1] == ' ')
                        --back;
                    std::size_t nameStart = back;
                    while (nameStart > 0 && (IsIdentifierChar(text[nameStart - 1]) || text[nameStart - 1] == '.'))
                        --nameStart;
                    name = std::string(text.substr(nameStart, back - nameStart));
                }
            }
            else
            {
                const std::size_t nameStart = i;
                while (i < text.size() && (IsIdentifierChar(text[i]) || text[i] == '.' || text[i] == ':'))
                    ++i;
                name = std::string(text.substr(nameStart, i - nameStart));
                while (i < text.size() && text[i] == ' ')
                    ++i;
            }
            if (name.empty() || i >= text.size() || text[i] != '(')
                continue;
            const std::size_t close = text.find(')', i);
            if (close == std::string::npos)
                continue;
            const std::size_t separator = name.find_last_of(".:");
            std::string params = Trim(text.substr(i + 1, close - i - 1));
            if (separator != std::string::npos && name[separator] == ':')
                params = params.empty() ? "self" : "self, " + params;
            result.push_back({ std::move(name), std::move(params), at });
        }
        return result;
    }

    int FuzzyScore(std::string_view name, std::string_view query)
    {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (query.empty())
            return 1;
        if (query.size() > name.size())
            return -1;
        if (name.compare(0, query.size(), query) == 0)
            return 3000 - static_cast<int>(name.size());
        bool prefix = true;
        for (std::size_t i = 0; i < query.size() && prefix; ++i)
            prefix = lower(name[i]) == lower(query[i]);
        if (prefix)
            return 2000 - static_cast<int>(name.size());
        if (query.size() < 2)
            return -1;
        const auto boundaryAt = [&](std::size_t at)
        {
            return at == 0 || name[at - 1] == '_' || (name[at] >= 'A' && name[at] <= 'Z' && !(name[at - 1] >= 'A' && name[at - 1] <= 'Z'));
        };
        int score = 1000;
        std::size_t at = 0;
        std::size_t previous = std::string_view::npos;
        for (const char wanted : query)
        {
            while (at < name.size() && (lower(name[at]) != lower(wanted) || (previous == std::string_view::npos && !boundaryAt(at))))
                ++at;
            if (at >= name.size())
                return -1;
            const bool boundary = boundaryAt(at);
            if (boundary)
                score += 20;
            if (previous != std::string_view::npos && at == previous + 1)
                score += 10;
            else if (previous != std::string_view::npos)
                score -= static_cast<int>(at - previous);
            previous = at;
            ++at;
        }
        if (lower(name[0]) != lower(query[0]))
            score -= 50;
        return score - static_cast<int>(name.size());
    }

    std::vector<std::string> SplitParameters(std::string_view params)
    {
        std::vector<std::string> parts;
        int depth = 0;
        std::size_t start = 0;
        char quote = 0;
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            const char c = params[i];
            if (quote)
            {
                if (c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (c == '(' || c == '{' || c == '[')
                ++depth;
            else if (c == ')' || c == '}' || c == ']')
                --depth;
            else if (c == ',' && depth == 0)
            {
                parts.push_back(Trim(params.substr(start, i - start)));
                start = i + 1;
            }
        }
        if (!TrimView(params.substr(start)).empty())
            parts.push_back(Trim(params.substr(start)));
        return parts;
    }
}
