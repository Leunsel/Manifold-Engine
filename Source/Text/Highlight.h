#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Manifold::Text
{
    enum class Language : std::uint8_t
    {
        AutoAssembler,
        Lua,
        Plain
    };

    enum class Token : std::uint8_t
    {
        Text,
        Comment,
        Keyword,
        Number,
        String,
        Call,
        Register,
        Section,
        Label,
        Mnemonic
    };

    enum class Block : std::uint8_t
    {
        None,
        Brace,
        Slash,
        Lua,
        LuaComment,
        LuaString
    };

    struct LineState
    {
        Block Kind = Block::None;
        std::uint8_t Level = 0;

        bool operator==(const LineState&) const = default;
    };

    struct TokenSpan
    {
        std::size_t Start = 0;
        std::size_t End = 0;
        Token Kind = Token::Text;
    };

    LineState InitialState(Language language);
    void HighlightLine(Language language, std::string_view line, std::size_t offset, LineState& state, std::vector<TokenSpan>& spans);
    std::vector<TokenSpan> Highlight(Language language, std::string_view text);

    bool IsLuaKeyword(std::string_view word);
    bool IsAaCommand(std::string_view lowerWord);
    bool IsRegister(std::string_view lowerWord);
}
