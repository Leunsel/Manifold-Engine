#pragma once

#include <cstdint>

// The agent's lifetime. Manifold Engine loads the agent library once per part
// that starts (the debugger, the code filter, the speedhack, the crash watch)
// and counts every load in HostLoads; the parts tell the agent while they need
// it. When none does and nothing happened for GraceMs, the agent gives every
// load back and unloads itself. It does the same after Manifold Engine ended
// (HostHandle signalled): each part takes its changes out first, and a part
// that cannot (speedhack hooks still in the game's code) keeps it loaded.
namespace Manifold::Target::Life
{
    constexpr std::uint32_t kMagic = 0x4546494Cu;
    constexpr std::uint32_t kVersion = 1;
    constexpr std::uint32_t kDefaultGraceMs = 8000;

    enum Part : std::uint32_t
    {
        PartDebugger = 1,
        PartCover = 2,
        PartSpeed = 4,
        PartCrash = 8
    };

    struct Block
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        volatile std::uint32_t HostLoads;   // written by the host: loads it made
        volatile std::uint32_t Dropped;     // loads the agent gave back
        volatile std::uint32_t Parts;       // the parts that need the agent now
        volatile std::uint32_t Leaving;     // set right before the last FreeLibrary
        volatile std::uint64_t HostHandle;  // the host process, SYNCHRONIZE, in this process
        volatile std::uint64_t Activity;    // tick of the last call into the agent
        volatile std::uint32_t GraceMs;
        volatile std::uint32_t HostGone;
    };

    static_assert(sizeof(Block) == 48, "the block has the same layout in both agents");
}
