#pragma once

#include <cstdint>

namespace Manifold::Target::Speed
{
    constexpr std::uint32_t kMagic = 0x4450534Du;
    constexpr std::uint32_t kVersion = 1;
    constexpr int kMaxHooks = 8;
    constexpr int kPatchBytes = 5;
    constexpr int kStripes = 16;
    constexpr int kStripeStep = 16;
    constexpr double kMaxSpeed = 1000.0;

    enum Source : std::uint32_t
    {
        SourceCounter = 0,
        SourceTick = 1,
        SourceTick64 = 2,
        SourceMultimedia = 3,
        SourceCount = 4
    };

    enum Call : std::uint32_t
    {
        CallWatch = 1,
        CallRelease = 2,
        CallApply = 3
    };

    struct Hook
    {
        std::uint64_t Target;
        std::uint64_t Page;
        std::uint32_t Source;
        std::uint32_t Pad;
        std::uint8_t Original[8];
    };

    struct Block
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        std::uint64_t Entries[SourceCount];
        volatile std::uint64_t Originals[SourceCount];
        volatile double Wanted;
        volatile std::uint32_t Serial;
        volatile std::uint32_t Applied;
        volatile double Speed;
        volatile std::uint32_t Shifted;
        volatile std::uint32_t HostLost;
        volatile std::uint64_t HostHandle;
        volatile std::uint32_t Watching;
        volatile std::uint32_t HookCount;
        Hook Hooks[kMaxHooks];
        std::uint64_t ImageBase;
        std::uint64_t ImageSize;
        volatile std::int32_t Inside[kStripes * kStripeStep];
    };

    static_assert(sizeof(Hook) == 32);
    static_assert(sizeof(Block) == 8 + 32 + 32 + 8 + 8 + 8 + 8 + 8 + 8 + kMaxHooks * 32 + 16 + kStripes * kStripeStep * 4, "the 32-bit and the 64-bit agent share this layout");
}
