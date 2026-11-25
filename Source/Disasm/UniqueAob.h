#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Disasm
{
    class Disassembler;

    struct UniqueAob
    {
        std::string Text;
        std::uint64_t Start = 0;
        int Offset = 0;
        std::vector<std::uint8_t> Bytes;
        std::vector<std::uint8_t> Mask;
    };

    struct UniqueAobOptions
    {
        std::size_t MaxLength = 96;
        // 0: the pattern starts at the address; otherwise it may start at an instruction
        // up to MaxOffset bytes before or after it (the result's Offset says where).
        int BackSteps = 8;
        int MaxOffset = 128;
        bool WholeProcess = true;
        // Unique among executable memory only. For tests in their own process, whose scan
        // buffers hold copies of what they read; getUniqueAOB measures all memory like AOBScan.
        bool CodeOnly = false;
    };

    std::optional<UniqueAob> FindUniqueAob(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, const UniqueAobOptions& options = {});
}
