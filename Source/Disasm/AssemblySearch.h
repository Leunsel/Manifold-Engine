#pragma once

#include "Disasm/CodeDissect.h"
#include "Disasm/Disassembler.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Symbols;
}

namespace Manifold::Disasm
{
    struct AsmPatternLine
    {
        std::string Text;
        std::string Mnemonic;
        bool KeepSize = false;
    };

    struct AsmPattern
    {
        std::vector<AsmPatternLine> Lines;
    };

    struct AsmMatch
    {
        std::uint64_t Address = 0;
        std::vector<std::string> Lines;
    };

    struct AsmSearchProgress
    {
        std::atomic<std::uint64_t> Done{ 0 };
        std::atomic<std::uint64_t> Total{ 0 };
        std::atomic<bool> Cancel{ false };
    };

    struct AsmSearchResult
    {
        std::vector<AsmMatch> Matches;
        std::uint64_t Instructions = 0;
        bool Truncated = false;
        bool Cancelled = false;
        double Seconds = 0.0;
    };

    constexpr std::size_t kMaxAsmMatches = 5000;

    std::optional<AsmPattern> ParseAsmPattern(std::string_view text);
    std::string NormalizeAsm(std::string_view text, bool keepSize);
    bool WildcardMatch(std::string_view pattern, std::string_view text);
    AsmSearchResult SearchAssembly(const Target::Process& process, const Disassembler& disassembler, const Target::Symbols* symbols,
                                   const std::vector<CodeSpan>& spans, const AsmPattern& pattern, AsmSearchProgress* progress = nullptr);
}
