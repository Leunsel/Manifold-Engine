#include "Disasm/OpcodeConvert.h"

#include "Core/Text.h"
#include "Disasm/Disassembler.h"

#include <algorithm>
#include <cctype>

namespace Manifold::Disasm
{
    namespace
    {
        int Digit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        bool Separator(char c)
        {
            return std::isspace(static_cast<unsigned char>(c)) != 0 || c == ',' || c == ';' || c == '{' || c == '}' || c == '[' || c == ']' || c == '(' || c == ')' ||
                   c == '"' || c == '\'';
        }

        std::string_view StripComment(std::string_view line)
        {
            const std::size_t comment = line.find("//");
            return TrimView(comment == std::string_view::npos ? line : line.substr(0, comment));
        }
    }

    std::optional<std::vector<std::uint8_t>> ParseByteText(std::string_view text, std::string* error)
    {
        std::vector<std::uint8_t> bytes;
        std::size_t at = 0;
        while (at < text.size())
        {
            if (Separator(text[at]))
            {
                ++at;
                continue;
            }
            std::size_t end = at;
            while (end < text.size() && !Separator(text[end]))
                ++end;
            std::string_view token = text.substr(at, end - at);
            at = end;
            // \x48\x8B is one token of several bytes; 0x48 and 48h one byte each. Every \x or 0x
            // starts a group of its own, so \x1\x2 is 01 02 like \x01\x02 (the last group was not
            // padded and the token failed as an odd digit count), and a prefix with no digits
            // after it (a lone 0x, \x\x41) is an error instead of nothing.
            struct Group
            {
                std::string Digits;
                bool Prefixed = false;
            };
            std::vector<Group> groups(1);
            for (std::size_t i = 0; i < token.size(); ++i)
            {
                const char c = token[i];
                if ((c == '\\' || c == '0') && i + 1 < token.size() && (token[i + 1] == 'x' || token[i + 1] == 'X'))
                {
                    if (groups.back().Prefixed || !groups.back().Digits.empty())
                        groups.emplace_back();
                    groups.back().Prefixed = true;
                    ++i;
                    continue;
                }
                if ((c == 'h' || c == 'H') && i + 1 == token.size() && !groups.back().Digits.empty())
                    continue;
                if (Digit(c) < 0)
                {
                    if (error)
                        *error = "\"" + std::string(token) + "\" is not a hex byte";
                    return std::nullopt;
                }
                groups.back().Digits += c;
            }
            for (Group& group : groups)
            {
                std::string& digits = group.Digits;
                if (digits.empty())
                {
                    if (!group.Prefixed)
                        continue;
                    if (error)
                        *error = "\"" + std::string(token) + "\" has a \\x or 0x without hex digits after it";
                    return std::nullopt;
                }
                if (digits.size() == 1)
                    digits.insert(digits.begin(), '0');
                if (digits.size() % 2 == 1)
                {
                    if (error)
                        *error = "\"" + std::string(token) + "\" has an odd number of hex digits";
                    return std::nullopt;
                }
                for (std::size_t i = 0; i < digits.size(); i += 2)
                    bytes.push_back(static_cast<std::uint8_t>(Digit(digits[i]) * 16 + Digit(digits[i + 1])));
            }
        }
        if (error)
            error->clear();
        return bytes;
    }

    std::string FormatBytes(std::span<const std::uint8_t> bytes, ByteStyle style)
    {
        static const char* kDigits = "0123456789ABCDEF";
        std::string text;
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            const char high = kDigits[bytes[i] >> 4];
            const char low = kDigits[bytes[i] & 15];
            switch (style)
            {
            case ByteStyle::Spaced:
                if (i)
                    text += ' ';
                break;
            case ByteStyle::Compact: break;
            case ByteStyle::Escaped: text += "\\x"; break;
            case ByteStyle::CArray: text += i ? ", 0x" : "0x"; break;
            }
            text += high;
            text += low;
        }
        return text;
    }

    std::vector<ConvertedLine> DecodeBytes(std::span<const std::uint8_t> bytes, std::uint64_t address, bool is64Bit, const Target::Symbols* symbols)
    {
        std::vector<ConvertedLine> lines;
        const Disassembler disassembler(is64Bit);
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const std::uint64_t at = address + offset;
            const Instruction instruction = disassembler.Decode(at, bytes.data() + offset, bytes.size() - offset, symbols);
            ConvertedLine line;
            line.Address = at;
            if (instruction.Valid && instruction.Length > 0 && offset + instruction.Length <= bytes.size())
            {
                line.Bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + instruction.Length));
                line.Text = instruction.Text();
                offset += instruction.Length;
            }
            else
            {
                line.Bytes.push_back(bytes[offset]);
                line.Text = "db " + FormatBytes(std::span<const std::uint8_t>(&bytes[offset], 1), ByteStyle::Spaced);
                line.Error = bytes.size() - offset < 15 ? "No instruction starts here, or it needs more bytes" : "No instruction starts here";
                ++offset;
            }
            lines.push_back(std::move(line));
        }
        return lines;
    }

    std::vector<ConvertedLine> AssembleText(std::string_view text, std::uint64_t address, bool is64Bit, const Asm::SymbolResolver& resolve)
    {
        std::vector<ConvertedLine> lines;
        const Disassembler disassembler(is64Bit);
        std::uint64_t at = address;
        int number = 0;
        std::size_t start = 0;
        while (start <= text.size())
        {
            const std::size_t end = std::min(text.find('\n', start), text.size());
            const std::string_view line = StripComment(text.substr(start, end - start));
            ++number;
            start = end + 1;
            if (line.empty())
                continue;
            ConvertedLine converted;
            converted.Address = at;
            converted.SourceLine = number;
            Asm::AssembleOptions options;
            options.Is64Bit = is64Bit;
            options.Address = at;
            options.Resolve = resolve;
            options.DefaultToDword = true;
            const Asm::AssembleResult result = Asm::Assemble(line, options);
            if (result.Ok && !result.Bytes.empty())
            {
                converted.Bytes = result.Bytes;
                const Instruction decoded = disassembler.Decode(at, result.Bytes.data(), result.Bytes.size());
                converted.Text = decoded.Valid && decoded.Length == result.Bytes.size() ? decoded.Text() : std::string(line);
                at += result.Bytes.size();
            }
            else
            {
                converted.Text = std::string(line);
                converted.Error = result.Message.empty() ? std::string(Asm::ErrorName(result.Error)) : result.Message;
            }
            lines.push_back(std::move(converted));
        }
        return lines;
    }
}
