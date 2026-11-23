#pragma once

#include "Disasm/FlowGraph.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace Manifold::Disasm
{
    enum class FunctionSource
    {
        Unwind,
        Managed,
        Padding,
        Guess
    };

    struct FunctionSpan
    {
        std::uint64_t Start = 0;
        std::uint64_t End = 0;
        std::uint64_t Last = 0;
        FunctionSource Source = FunctionSource::Guess;
    };

    struct FunctionClues
    {
        std::function<std::optional<FunctionSpan>(std::uint64_t address)> Exact;
        std::function<bool(std::uint64_t address)> KnownStart;
        std::uint64_t Floor = 0;
        std::uint64_t Ceiling = 0;
        bool FloorIsBoundary = false;
    };

    std::optional<FunctionSpan> FindFunction(const Disassembler& disassembler, const Target::Process& process, std::uint64_t address, const CodeHints* hints,
        const FunctionClues& clues);
    std::uint64_t CodeExtent(const Disassembler& disassembler, const Target::Process& process, std::uint64_t start, std::uint64_t limit, const CodeHints* hints);
    // Where the constants a JIT stores behind a method's code begin: the lowest
    // address inside [start, end) that the method's own instructions read as
    // data. Nothing when the method reads nothing of its own range.
    std::optional<std::uint64_t> DataTail(const Disassembler& disassembler, const Target::Process& process, std::uint64_t start, std::uint64_t end,
        const CodeHints* hints);
    const char* FunctionSourceText(FunctionSource source);
}
