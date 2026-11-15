#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Templates
{
    enum class NodeKind
    {
        Text,
        Expression,
        Code
    };

    struct Node
    {
        NodeKind Kind = NodeKind::Text;
        std::string Text;
        int Line = 1;
    };

    struct ParseResult
    {
        std::vector<Node> Nodes;
        std::string Error;
        int ErrorLine = 0;

        bool Ok() const { return Error.empty(); }
    };

    struct CompiledTemplate
    {
        std::string Chunk;
        std::vector<int> LineMap;
        std::string Source;

        int TemplateLine(int generatedLine) const;
    };

    ParseResult Parse(std::string_view source);
    CompiledTemplate Compile(const std::vector<Node>& nodes, const std::string& source);
    std::string QuoteLua(std::string_view text);
    std::vector<std::string> IncludedNames(const std::vector<Node>& nodes);
}
