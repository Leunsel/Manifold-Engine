#pragma once

#include <cstdint>

namespace Manifold::Debug::Veh
{
    constexpr std::uint32_t kMagic = 0x56454830u;
    constexpr std::uint32_t kVersion = 1;
    constexpr int kMaxWatches = 4;
    constexpr int kMaxEntries = 256;

    struct Registers
    {
        std::uint64_t Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp;
        std::uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
        std::uint64_t Rip, Flags, GsBase;
    };

    struct Vectors
    {
        std::uint8_t Xmm[16][16];
        std::uint32_t Mxcsr;
        std::uint32_t Count;
    };

    struct Entry
    {
        std::uint64_t Instruction;
        std::uint64_t Accessed;
        std::uint64_t Count;
        std::uint32_t ThreadId;
        std::uint32_t Pad;
        Registers Regs;
    };

    struct Watch
    {
        std::uint32_t Active;
        std::uint32_t Mode;
        std::uint64_t Address;
        std::uint32_t Size;
        std::uint32_t Slot;
        volatile std::uint32_t Clear;
        std::uint32_t Pad;
        std::uint64_t Overflow;
        std::uint64_t EntryCount;
        Entry Entries[kMaxEntries];
    };

    enum Mode : std::uint32_t
    {
        ModeAccess = 0,
        ModeWrite = 1,
        ModeInstruction = 2,
        ModeCalls = 3,
        ModeBreak = 4
    };

    enum Resume : std::uint32_t
    {
        ResumeNone = 0,
        ResumeContinue = 1,
        ResumeStep = 2
    };

    struct Block
    {
        std::uint32_t Magic;
        std::uint32_t Version;
        volatile std::uint32_t Command;
        volatile std::uint32_t Applied;
        volatile std::uint32_t Running;
        volatile std::uint32_t Stop;
        volatile std::uint32_t AppliedThreads;
        volatile std::uint32_t LastError;
        volatile std::int32_t Lock;
        std::uint32_t Reserved;
        volatile std::uint64_t Hits;
        volatile std::uint32_t Broken;
        volatile std::uint32_t ResumeCmd;
        volatile std::uint32_t BrokenThread;
        volatile std::uint32_t RegsDirty;
        volatile std::uint32_t BreakNow;
        volatile std::uint32_t Pad3;
        volatile std::uint64_t BreakSerial;
        volatile std::uint64_t BreakAddress;
        Registers BreakRegs;
        Registers NewRegs;
        Watch Watches[kMaxWatches];
        Vectors BreakXmm;
        Vectors NewXmm;
        volatile std::uint32_t XmmDirty;
        std::uint32_t Pad4;
    };
}
