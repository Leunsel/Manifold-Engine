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
        int BackSteps = 8;
        bool WholeProcess = true;
    };

    std::optional<UniqueAob> FindUniqueAob(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, const UniqueAobOptions& options = {});
}
