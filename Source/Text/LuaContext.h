#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Text
{
    struct WordContext
    {
        bool Valid = false;
        std::size_t Start = 0;
        std::string Prefix;
        std::string Chain;
        bool Method = false;
        bool Member = false;
    };

    struct CallContext
    {
        bool Valid = false;
        std::string Chain;
        bool Method = false;
        int Argument = 0;
        std::size_t Paren = 0;
    };

    struct ChainContext
    {
        bool Valid = false;
        std::size_t Start = 0;
        std::size_t End = 0;
        std::string Chain;
        std::string Name;
        bool Method = false;
    };

    struct BufferFunction
    {
        std::string Name;
        std::string Params;
        std::size_t Offset = 0;
    };

    bool IsIdentifierChar(char c);
    bool InCode(std::string_view text, std::size_t cursor);
    std::size_t ChainStart(std::string_view text, std::size_t end);
    WordContext WordAt(std::string_view text, std::size_t cursor);
    CallContext CallAt(std::string_view text, std::size_t cursor);
    ChainContext ChainAt(std::string_view text, std::size_t position);
    std::vector<BufferFunction> BufferFunctions(std::string_view text);
    std::string AssignedCall(std::string_view text, std::size_t before, std::string_view name);
    std::vector<std::string> SplitParameters(std::string_view params);
    int FuzzyScore(std::string_view name, std::string_view query);
}
