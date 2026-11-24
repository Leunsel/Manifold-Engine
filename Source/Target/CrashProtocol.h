#pragma once

#include <cstdint>

// The crash watch: the agent notes every exception of a kind that ends a
// process (access violations, illegal instructions, divisions by zero, stack
// overflows) in a ring that lives in memory Manifold Engine owns. The host
// creates the section and hands the agent a handle to it, so the ring is still
// readable after the process died, and the host matches the last records
// against the code the scripts put into the game.
namespace Manifold::Target::Crash
{
    constexpr std::uint32_t kMagic = 0x48535243u;
    constexpr std::uint32_t kVersion = 1;
    constexpr std::uint32_t kSlots = 32;
    constexpr std::uint32_t kStackWords = 64;
    constexpr std::uint32_t kCodeBytes = 16;
    constexpr std::uint32_t kRegisters = 17;

    // Registers[] in this order; 32-bit processes fill the first nine.
    enum Register : std::uint32_t
    {
        RegIp = 0,
        RegSp,
        RegAx,
        RegBx,
        RegCx,
        RegDx,
        RegSi,
        RegDi,
        RegBp,
        RegR8,
        RegR9,
        RegR10,
        RegR11,
        RegR12,
        RegR13,
        RegR14,
        RegR15
    };

    enum Call : std::uint32_t
    {
        CallStart = 1,
        CallStop = 2
    };

    enum Result : std::uint32_t
    {
        ResultOk = 0,
        ResultNoMapping = 1,
        ResultNoHandler = 2,
        ResultUnknownCall = 3,
        ResultStillInside = 4
    };

    // Sequence is odd while the agent writes the record and even once it is
    // complete (2 * number + 2), so a record cut off by the process's end is
    // recognised.
    struct Record
    {
        volatile std::uint32_t Sequence;
        std::uint32_t Code;
        std::uint32_t Flags;
        std::uint32_t ThreadId;
        std::uint64_t Tick;
        std::uint64_t Address;
        std::uint64_t Info[2];
        std::uint32_t InfoCount;
        std::uint32_t StackCount;
        std::uint64_t Registers[kRegisters];
        std::uint64_t Stack[kStackWords];
        std::uint8_t Bytes[kCodeBytes];
        std::uint32_t ByteCount;
        std::uint32_t Pad;
    };

    struct Ring
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        volatile std::uint64_t Next;
        volatile std::uint64_t Seen;
        std::uint32_t ProcessId;
        std::uint32_t Is64Bit;
        Record Slots[kSlots];
    };

    // The agent's exported block: the host writes Mapping (the section's
    // handle in the process) and Serial, then calls ManifoldCrashCall.
    struct Block
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        std::uint64_t Mapping;
        volatile std::uint32_t Running;
        volatile std::uint32_t Result;
        volatile std::uint32_t Serial;
        volatile std::uint32_t Applied;
        volatile std::uint32_t LastError;
        std::uint32_t Pad;
    };

    static_assert(sizeof(Record) == 728, "the record has the same layout in both agents");
    static_assert(sizeof(Block) == 40, "the block has the same layout in both agents");
}
