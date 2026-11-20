#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Asm
{
    struct AsmCommandParam
    {
        const char* Name;
        const char* Type;
        bool Optional;
        const char* Text;
    };

    struct AsmCommandDoc
    {
        const char* Name;
        std::vector<AsmCommandParam> Params;
        const char* Text;
    };

    struct AsmCompletion
    {
        std::string Name;
        std::string Kind;
        std::string Signature;
        std::string Text;
    };

    struct AsmAssistInput
    {
        bool Is64Bit = true;
        std::vector<std::string> Modules;
        std::vector<std::string> Symbols;
    };

    enum class AsmPlace
    {
        None,
        LineStart,
        Directive,
        Section,
        Operand,
        Argument
    };

    struct AsmWord
    {
        AsmPlace Place = AsmPlace::None;
        std::size_t Start = 0;
        std::string Prefix;
        std::string Command;
        int Argument = 0;
    };

    std::span<const AsmCommandDoc> AsmCommands();
    const AsmCommandDoc* FindAsmCommand(std::string_view name);
    bool InLuaBlock(std::string_view text, std::size_t cursor);
    AsmWord AsmWordAt(std::string_view text, std::size_t cursor);
    std::vector<std::string> ScriptNames(std::string_view text);
    std::vector<AsmCompletion> CompleteAsm(std::string_view text, std::size_t cursor, const AsmAssistInput& input);
}
