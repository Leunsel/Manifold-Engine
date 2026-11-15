#include "Disasm/AssemblySearch.h"

#include "Core/Text.h"
#include "Target/Symbols.h"

#include "Zydis.h"

#include <algorithm>
#include <chrono>
#include <cctype>

namespace Manifold::Disasm
{
    namespace
    {
        constexpr std::uint64_t kChunk = 0x100000;
        constexpr std::uint64_t kPage = 0x1000;

        const char* const kPrefixes[] = { "lock", "rep", "repe", "repz", "repne", "repnz", "xacquire", "xrelease", "bnd", "notrack" };

        bool IsPrefix(std::string_view word)
        {
            return std::any_of(std::begin(kPrefixes), std::end(kPrefixes), [&](const char* prefix) { return word == prefix; });
        }

        bool TightBefore(char c)
        {
            return c == ',' || c == ']' || c == '+' || c == '-' || c == ':';
        }

        bool TightAfter(char c)
        {
            return c == ',' || c == '[' || c == '+' || c == '-' || c == ':';
        }

        std::string FirstMnemonic(std::string_view normalized)
        {
            std::size_t at = 0;
            while (at < normalized.size())
            {
                const std::size_t end = std::min(normalized.find(' ', at), normalized.size());
                const std::string_view word = normalized.substr(at, end - at);
                if (!IsPrefix(word))
                {
                    if (word.find_first_of("*?") != std::string_view::npos)
                        return {};
                    return std::string(word);
                }
                at = end + 1;
            }
            return {};
        }
    }

    std::string NormalizeAsm(std::string_view text, bool keepSize)
    {
        std::string lower;
        lower.reserve(text.size());
        for (char c : text)
            lower += c == '\t' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!keepSize)
        {
            static const char* const kSizes[] = { "zmmword ptr", "ymmword ptr", "xmmword ptr", "tbyte ptr", "fword ptr", "qword ptr", "dword ptr", "word ptr", "byte ptr" };
            for (const char* size : kSizes)
                lower = ReplaceAll(lower, size, "");
        }
        lower = ReplaceAll(lower, "0x", "");
        std::string out;
        out.reserve(lower.size());
        for (char c : lower)
        {
            if (c == ' ')
            {
                if (out.empty() || out.back() == ' ' || TightAfter(out.back()))
                    continue;
                out += ' ';
                continue;
            }
            if (TightBefore(c) && !out.empty() && out.back() == ' ')
                out.pop_back();
            out += c;
        }
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    bool WildcardMatch(std::string_view pattern, std::string_view text)
    {
        std::size_t p = 0;
        std::size_t t = 0;
        std::size_t star = std::string_view::npos;
        std::size_t mark = 0;
        while (t < text.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
            {
                ++p;
                ++t;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++;
                mark = t;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1;
                t = ++mark;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
            ++p;
        return p == pattern.size();
    }

    std::optional<AsmPattern> ParseAsmPattern(std::string_view text)
    {
        AsmPattern pattern;
        std::string line;
        const auto flush = [&]()
            {
                const std::string trimmed = Trim(line);
                line.clear();
                if (trimmed.empty())
                    return;
                AsmPatternLine entry;
                entry.KeepSize = ContainsNoCase(trimmed, "ptr");
                entry.Text = NormalizeAsm(trimmed, entry.KeepSize);
                entry.Mnemonic = FirstMnemonic(entry.Text);
                pattern.Lines.push_back(std::move(entry));
            };
        for (char c : text)
        {
            if (c == '\n' || c == '\r' || c == ';')
                flush();
            else
                line += c;
        }
        flush();
        if (pattern.Lines.empty())
            return std::nullopt;
        return pattern;
    }

    AsmSearchResult SearchAssembly(const Target::Process& process, const Disassembler& disassembler, const Target::Symbols* symbols,
                                   const std::vector<CodeSpan>& spans, const AsmPattern& pattern, AsmSearchProgress* progress)
    {
        const auto started = std::chrono::steady_clock::now();
        AsmSearchResult result;
        if (pattern.Lines.empty())
            return result;
        std::uint64_t total = 0;
        for (const CodeSpan& span : spans)
            total += span.End - span.Start;
        if (progress)
        {
            progress->Done = 0;
            progress->Total = total;
        }
        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, process.Is64Bit() ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32, process.Is64Bit() ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
        ZydisDecodedInstruction instruction{};
        const std::size_t lookahead = ZYDIS_MAX_INSTRUCTION_LENGTH * pattern.Lines.size();
        const AsmPatternLine& first = pattern.Lines.front();
        std::vector<std::uint8_t> buffer(kChunk);
        std::uint64_t done = 0;
        const auto matches = [&](std::size_t line, const Instruction& decoded)
            {
                const AsmPatternLine& entry = pattern.Lines[line];
                return decoded.Valid && WildcardMatch(entry.Text, NormalizeAsm(decoded.Text(), entry.KeepSize));
            };
        for (const CodeSpan& span : spans)
        {
            std::uint64_t at = span.Start;
            while (at < span.End && !result.Cancelled && !result.Truncated)
            {
                if (progress && progress->Cancel.load())
                {
                    result.Cancelled = true;
                    break;
                }
                const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, span.End - at));
                const std::size_t got = process.Read(at, buffer.data(), wanted);
                if (got == 0)
                {
                    const std::uint64_t next = std::min(span.End, (at & ~(kPage - 1)) + kPage);
                    done += next - at;
                    at = next;
                    continue;
                }
                const bool last = at + got >= span.End;
                std::size_t position = 0;
                while (position < got)
                {
                    const std::size_t available = got - position;
                    if (available < lookahead && !last)
                        break;
                    if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, buffer.data() + position, available, &instruction)))
                    {
                        ++position;
                        continue;
                    }
                    ++result.Instructions;
                    const std::uint64_t address = at + position;
                    const char* mnemonic = ZydisMnemonicGetString(instruction.mnemonic);
                    if (first.Mnemonic.empty() || (mnemonic && first.Mnemonic == mnemonic))
                    {
                        const Instruction decoded = disassembler.Decode(address, buffer.data() + position, available, symbols);
                        if (matches(0, decoded))
                        {
                            AsmMatch match;
                            match.Address = address;
                            match.Lines.push_back(decoded.Text());
                            std::size_t next = position + decoded.Length;
                            bool all = true;
                            for (std::size_t line = 1; line < pattern.Lines.size() && all; ++line)
                            {
                                if (next >= got)
                                {
                                    all = false;
                                    break;
                                }
                                const Instruction following = disassembler.Decode(at + next, buffer.data() + next, got - next, symbols);
                                all = matches(line, following);
                                match.Lines.push_back(following.Text());
                                next += following.Length ? following.Length : 1;
                            }
                            if (all)
                            {
                                result.Matches.push_back(std::move(match));
                                if (result.Matches.size() >= kMaxAsmMatches)
                                {
                                    result.Truncated = true;
                                    break;
                                }
                            }
                        }
                    }
                    position += instruction.length;
                }
                if (position == 0)
                    position = 1;
                at += position;
                done += position;
                if (progress)
                    progress->Done = done;
            }
            if (result.Cancelled || result.Truncated)
                break;
        }
        result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return result;
    }
}
