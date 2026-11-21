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
        Plain,
        Json,
        Template
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
        Mnemonic,
        Delimiter,
        Variable
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

    // Template lines keep two states: Kind/Level for the Auto Assembler text
    // around the tags, Inner/InnerLevel for the Lua inside a tag that spans
    // lines; Tag says which tag is open at the start of the line.
    enum class Tag : std::uint8_t
    {
        None,
        Code,
        Expression
    };

    struct LineState
    {
        Block Kind = Block::None;
        std::uint8_t Level = 0;
        Tag Open = Tag::None;
        Block Inner = Block::None;
        std::uint8_t InnerLevel = 0;

        bool operator==(const LineState&) const = default;
    };

    struct TokenSpan
    {
        std::size_t Start = 0;
        std::size_t End = 0;
        Token Kind = Token::Text;
        bool Embedded = false;
    };

    LineState InitialState(Language language);
    void HighlightLine(Language language, std::string_view line, std::size_t offset, LineState& state, std::vector<TokenSpan>& spans);
    std::vector<TokenSpan> Highlight(Language language, std::string_view text);

    bool IsLuaKeyword(std::string_view word);
    bool IsAaCommand(std::string_view lowerWord);
    bool IsRegister(std::string_view lowerWord);
}
