#pragma once

#include "Asm/Assembler.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Symbols;
}

namespace Manifold::Disasm
{
    // One instruction of the opcode converter, in either direction.
    struct ConvertedLine
    {
        std::uint64_t Address = 0;
        std::vector<std::uint8_t> Bytes;
        std::string Text;
        std::string Error;
        int SourceLine = 0;  // 1-based line of the instruction text; 0 for bytes
    };

    enum class ByteStyle
    {
        Spaced,   // 48 8B 05
        Compact,  // 488B05
        Escaped,  // \x48\x8B\x05
        CArray    // 0x48, 0x8B, 0x05
    };

    // Bytes as people paste them: "48 8B 05", "488B05", "0x48, 0x8B",
    // "\x48\x8B", "{ 0x48 }" and the like. A single digit is one byte (also
    // each \x or 0x group: \x1\x2 is 01 02); an odd run of more digits and a
    // \x or 0x without digits are errors, named in `error`.
    std::optional<std::vector<std::uint8_t>> ParseByteText(std::string_view text, std::string* error = nullptr);
    std::string FormatBytes(std::span<const std::uint8_t> bytes, ByteStyle style);

    // Decodes the bytes one instruction after the other from `address`; a byte
    // that starts no instruction becomes a "db" line with an error.
    std::vector<ConvertedLine> DecodeBytes(std::span<const std::uint8_t> bytes, std::uint64_t address, bool is64Bit, const Target::Symbols* symbols = nullptr);
    // Assembles one instruction per line ("//" starts a comment), each at the
    // address after the one before; a failed line keeps the address.
    std::vector<ConvertedLine> AssembleText(std::string_view text, std::uint64_t address, bool is64Bit, const Asm::SymbolResolver& resolve = {});
}
