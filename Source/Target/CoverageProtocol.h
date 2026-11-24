#pragma once

#include <cstdint>

// The code filter: one-shot int3 breakpoints the agent sets and takes back
// inside the process. The host writes a table of entries sorted by address
// into memory it allocated, the agent arms them page by page; the first thread
// that runs into one restores the byte, marks the entry hit and runs on.
namespace Manifold::Target::Cover
{
    constexpr std::uint32_t kMagic = 0x564F434Du;
    constexpr std::uint32_t kVersion = 2;
    constexpr std::uint32_t kMaxEntries = 1u << 21;
    constexpr std::uint8_t kInt3 = 0xCC;

    enum State : std::uint32_t
    {
        StateIdle = 0,
        StateArmed = 1,
        StateRestoring = 2,
        StateHit = 3,
        StateReleased = 4,
        StateSkipped = 5,
        StateReleasing = 6
    };

    enum Skip : std::uint32_t
    {
        SkipNone = 0,
        SkipAlreadyInt3 = 1,
        SkipUnreadable = 2,
        SkipProtect = 3,
        SkipSystem = 4,
        SkipUnsorted = 5,
        SkipNotCode = 6
    };

    enum Result : std::uint32_t
    {
        ResultOk = 0,
        ResultBadTable = 1,
        ResultNoHandler = 2,
        ResultUnknownCall = 3,
        ResultStillInside = 4,
        ResultStillArmed = 5
    };

    enum Call : std::uint32_t
    {
        CallWatch = 1,
        CallArm = 2,
        CallDisarm = 3,
        CallRelease = 4,
        CallReleaseRange = 5
    };

    struct Entry
    {
        std::uint64_t Address;
        volatile std::uint32_t State;
        volatile std::uint32_t ThreadId;
        std::uint8_t Original;
        std::uint8_t Skip;
        std::uint16_t Pad;
        std::uint32_t Pad2;
    };

    struct Block
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        std::uint64_t Table;
        std::uint32_t Count;
        volatile std::uint32_t Active;
        volatile std::uint64_t Hits;
        volatile std::uint64_t Foreign;
        volatile std::uint64_t Stale;
        volatile std::uint32_t Armed;
        volatile std::uint32_t Skipped;
        volatile std::uint32_t Handler;
        volatile std::uint32_t Result;
        std::uint64_t HostHandle;
        volatile std::uint32_t Watching;
        volatile std::uint32_t HostLost;
        volatile std::uint32_t Serial;
        volatile std::uint32_t Applied;
        std::uint64_t ImageBase;
        std::uint64_t ImageSize;
        std::uint64_t Low;
        std::uint64_t High;
        std::uint64_t RangeBegin;
        std::uint64_t RangeEnd;
    };

    static_assert(sizeof(Entry) == 24);
    static_assert(sizeof(Block) == 136);
}
