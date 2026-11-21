#include "Templates/TemplateEngine.h"

#include <algorithm>
#include <cstdio>
#include <regex>

namespace Manifold::Templates
{
    namespace
    {
        int CountLines(std::string_view text)
        {
            return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
        }

        bool IsBlank(std::string_view text)
        {
            return std::all_of(text.begin(), text.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
        }

        std::string TrimCopy(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            const std::size_t last = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(first, last - first + 1));
        }

        std::vector<std::string> Pieces(std::string_view text)
        {
            std::vector<std::string> pieces;
            std::size_t start = 0;
            while (true)
            {
                const std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                {
                    pieces.emplace_back(text.substr(start));
                    break;
                }
                pieces.emplace_back(text.substr(start, end - start));
                start = end + 1;
            }
            return pieces;
        }

        bool StandaloneBefore(const std::vector<Node>& nodes, std::size_t index)
        {
            for (std::size_t scan = index; scan-- > 0;)
            {
                const Node& node = nodes[scan];
                if (node.Kind == NodeKind::Code)
                    continue;
                if (node.Kind != NodeKind::Text)
                    return false;
                const std::size_t newline = node.Text.rfind('\n');
                const std::string_view tail = newline == std::string::npos ? std::string_view(node.Text) : std::string_view(node.Text).substr(newline + 1);
                if (!IsBlank(tail))
                    return false;
                if (newline != std::string::npos)
                    return true;
            }
            return true;
        }

        bool StandaloneAfter(const std::vector<Node>& nodes, std::size_t index)
        {
            for (std::size_t scan = index + 1; scan < nodes.size(); ++scan)
            {
                const Node& node = nodes[scan];
                if (node.Kind == NodeKind::Code)
                    continue;
                if (node.Kind != NodeKind::Text)
                    return false;
                const std::size_t newline = node.Text.find('\n');
                const std::string_view head = std::string_view(node.Text).substr(0, newline);
                if (!IsBlank(head))
                    return false;
                if (newline != std::string::npos)
                    return true;
            }
            return true;
        }

        void DropIndentBefore(std::vector<Node>& nodes, std::size_t index)
        {
            for (std::size_t scan = index; scan-- > 0;)
            {
                Node& node = nodes[scan];
                if (node.Kind != NodeKind::Text)
                    continue;
                const std::size_t newline = node.Text.rfind('\n');
                const std::string head = newline == std::string::npos ? std::string() : node.Text.substr(0, newline + 1);
                const bool reachedContent = !head.empty();
                node.Text = head;
                if (reachedContent)
                    return;
            }
        }

        void DropLineAfter(std::vector<Node>& nodes, std::size_t index)
        {
            for (std::size_t scan = index + 1; scan < nodes.size(); ++scan)
            {
                Node& node = nodes[scan];
                if (node.Kind != NodeKind::Text)
                    continue;
                const std::size_t newline = node.Text.find('\n');
                if (newline != std::string::npos)
                {
                    node.Text = node.Text.substr(newline + 1);
                    return;
                }
                node.Text.clear();
            }
        }

        void TrimStandaloneTags(std::vector<Node>& nodes)
        {
            for (std::size_t index = 0; index < nodes.size(); ++index)
            {
                if (nodes[index].Kind == NodeKind::Code && StandaloneBefore(nodes, index) && StandaloneAfter(nodes, index))
                {
                    DropIndentBefore(nodes, index);
                    DropLineAfter(nodes, index);
                }
            }
        }
    }

    int CompiledTemplate::TemplateLine(int generatedLine) const
    {
        if (generatedLine <= 0 || generatedLine >= static_cast<int>(LineMap.size()))
            return 0;
        return LineMap[static_cast<std::size_t>(generatedLine)];
    }

    ParseResult Parse(std::string_view source)
    {
        ParseResult result;
        if (source.starts_with("\xEF\xBB\xBF"))
            source.remove_prefix(3);
        std::size_t position = 0;
        int line = 1;
        while (position < source.size())
        {
            const std::size_t start = std::min(source.find("<<", position), source.find("<%", position));
            if (start == std::string_view::npos)
            {
                result.Nodes.push_back({ NodeKind::Text, std::string(source.substr(position)), line });
                break;
            }
            if (start > position)
            {
                const std::string_view leading = source.substr(position, start - position);
                result.Nodes.push_back({ NodeKind::Text, std::string(leading), line });
                line += CountLines(leading);
            }
            const bool expression = source.compare(start, 2, "<<") == 0;
            const std::string_view close = expression ? ">>" : "%>";
            const std::size_t end = source.find(close, start + 2);
            if (end == std::string_view::npos)
            {
                result.Error = std::string("Unclosed ") + (expression ? "<<" : "<%") + " block (expected " + std::string(close) + ")";
                result.ErrorLine = line;
                result.Nodes.clear();
                return result;
            }
            const std::string_view content = source.substr(start + 2, end - start - 2);
            if (expression)
            {
                std::string trimmed = TrimCopy(content);
                if (trimmed.empty())
                {
                    result.Error = "Empty << >> expression block";
                    result.ErrorLine = line;
                    result.Nodes.clear();
                    return result;
                }
                result.Nodes.push_back({ NodeKind::Expression, std::move(trimmed), line });
            }
            else
            {
                result.Nodes.push_back({ NodeKind::Code, std::string(content), line });
            }
            line += CountLines(source.substr(start, end + 2 - start));
            position = end + 2;
        }
        TrimStandaloneTags(result.Nodes);
        return result;
    }

    std::string QuoteLua(std::string_view text)
    {
        std::string quoted = "\"";
        quoted.reserve(text.size() + 2);
        for (const char c : text)
        {
            const unsigned char byte = static_cast<unsigned char>(c);
            switch (c)
            {
            case '\\': quoted += "\\\\"; break;
            case '"': quoted += "\\\""; break;
            case '\n': quoted += "\\n"; break;
            case '\r': quoted += "\\r"; break;
            case '\t': quoted += "\\t"; break;
            default:
                if (byte < 0x20 || byte == 0x7F)
                {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\%03u", static_cast<unsigned>(byte));
                    quoted += escaped;
                }
                else
                    quoted += c;
            }
        }
        quoted += '"';
        return quoted;
    }

    CompiledTemplate Compile(const std::vector<Node>& nodes, const std::string& source)
    {
        CompiledTemplate compiled;
        compiled.Source = source;
        std::vector<std::string> lines = { "local _ENV = ...", "local _ret = {}" };
        compiled.LineMap.assign(3, 0);
        auto emit = [&](std::string code, int templateLine)
        {
            lines.push_back(std::move(code));
            compiled.LineMap.push_back(templateLine);
        };
        for (const Node& node : nodes)
        {
            if (node.Kind == NodeKind::Text)
            {
                if (!node.Text.empty())
                    emit("_ret[#_ret + 1] = " + QuoteLua(node.Text), node.Line);
            }
            else if (node.Kind == NodeKind::Expression)
            {
                const std::vector<std::string> pieces = Pieces(node.Text);
                if (pieces.size() <= 1)
                {
                    emit("_ret[#_ret + 1] = _safe((" + node.Text + "), " + std::to_string(node.Line) + ", " + QuoteLua(node.Text) + ")", node.Line);
                }
                else
                {
                    std::string flat;
                    for (const std::string& piece : pieces)
                        flat += (flat.empty() ? "" : " ") + TrimCopy(piece);
                    emit("_ret[#_ret + 1] = _safe((" + pieces.front(), node.Line);
                    for (std::size_t index = 1; index + 1 < pieces.size(); ++index)
                        emit(pieces[index], node.Line + static_cast<int>(index));
                    emit(pieces.back() + "), " + std::to_string(node.Line) + ", " + QuoteLua(flat) + ")", node.Line + static_cast<int>(pieces.size()) - 1);
                }
            }
            else
            {
                int offset = 0;
                for (const std::string& piece : Pieces(node.Text))
                    emit(piece, node.Line + offset++);
            }
        }
        emit("return table.concat(_ret)", nodes.empty() ? 1 : nodes.back().Line);
        for (std::size_t index = 0; index < lines.size(); ++index)
        {
            compiled.Chunk += lines[index];
            if (index + 1 < lines.size())
                compiled.Chunk += '\n';
        }
        return compiled;
    }

    bool InTag(std::string_view source, std::size_t cursor)
    {
        cursor = std::min(cursor, source.size());
        std::size_t position = 0;
        while (position < cursor)
        {
            const std::size_t start = std::min(source.find("<<", position), source.find("<%", position));
            if (start == std::string_view::npos || start + 2 > cursor)
                return false;
            const bool expression = source.compare(start, 2, "<<") == 0;
            const std::size_t end = source.find(expression ? ">>" : "%>", start + 2);
            if (end == std::string_view::npos || end >= cursor)
                return true;
            position = end + 2;
        }
        return false;
    }

    std::vector<std::string> IncludedNames(const std::vector<Node>& nodes)
    {
        static const std::regex pattern(R"(include\s*\(\s*["']([^"']+)["'])");
        std::vector<std::string> names;
        for (const Node& node : nodes)
        {
            if (node.Kind == NodeKind::Text)
                continue;
            for (auto it = std::sregex_iterator(node.Text.begin(), node.Text.end(), pattern); it != std::sregex_iterator(); ++it)
                names.push_back((*it)[1].str());
        }
        return names;
    }
}
