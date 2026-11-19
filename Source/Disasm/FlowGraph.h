#pragma once

#include "Disasm/Disassembler.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace Manifold::Disasm
{
    enum class EdgeKind
    {
        Next,
        Jump,
        Taken,
        NotTaken
    };

    struct FlowEdge
    {
        int From = 0;
        int To = 0;
        EdgeKind Kind = EdgeKind::Next;
        bool Back = false;
    };

    struct FlowBlock
    {
        std::uint64_t Start = 0;
        std::uint64_t End = 0;
        std::vector<Instruction> Lines;
        bool Returns = false;
        bool Indirect = false;
        bool Stops = false;
        bool Truncated = false;
        bool LoopHead = false;
        std::vector<std::uint64_t> Calls;
        std::vector<std::uint64_t> Exits;
    };

    struct FlowGraph
    {
        std::uint64_t Entry = 0;
        std::vector<FlowBlock> Blocks;
        std::vector<FlowEdge> Edges;
        int Instructions = 0;
        bool Truncated = false;

        int BlockAt(std::uint64_t address) const;
        int BlockStarting(std::uint64_t address) const;
    };

    struct FlowLimits
    {
        int MaxInstructions = 6000;
        int MaxBlocks = 800;
        std::uint64_t Reach = 0x80000;
        std::uint64_t StopAt = 0;
    };

    using OtherFunction = std::function<bool(std::uint64_t target, EdgeKind kind)>;

    FlowGraph BuildFlowGraph(const Disassembler& disassembler, const Target::Process& process, std::uint64_t entry, const Target::Symbols* symbols,
        const CodeHints* hints, const OtherFunction& otherFunction, const FlowLimits& limits = {});
}
