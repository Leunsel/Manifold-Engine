#pragma once

#include "Scan/Scanner.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Scan
{
    struct StaticPointer
    {
        std::uint64_t Address = 0;
        std::uint64_t Value = 0;
        std::string Module;
        std::uint64_t ModuleBase = 0;

        std::uint64_t Offset(std::uint64_t target) const { return target - Value; }
    };

    struct StaticPointerSearch
    {
        std::uint64_t Target = 0;
        std::uint64_t MaxOffset = 0x1000;
        bool SystemModules = false;
        std::size_t Limit = 100000;
    };

    void MatchPointers(std::span<const std::uint8_t> bytes, std::uint64_t base, int pointerSize, std::uint64_t low, std::uint64_t high, std::vector<std::uint64_t>& out);
    std::vector<StaticPointer> FindStaticPointers(const Target::Process& process, const StaticPointerSearch& search, PatternProgress* progress = nullptr, bool* truncated = nullptr);
}
