#include "Debug/VehProtocol.h"
#include "Target/AgentLifeProtocol.h"

#include <windows.h>
#include <tlhelp32.h>

#include <intrin.h>

#include <vector>

using namespace Manifold::Debug::Veh;

void SpeedAttach(HINSTANCE instance);
void CoverAttach(HINSTANCE instance);
void CoverDetach();
LONG CoverFilter(EXCEPTION_POINTERS* info);
void CrashAttach();
void CrashDetach();
void LifeAttach(HINSTANCE instance);
void LifeHold(std::uint32_t part);
void LifeLetGo(std::uint32_t part);
void LifeTouch();

// The speedhack (Speed.cpp) hooks GetTickCount64 in this very process, so the
// agent's own waits and timeouts read the real tick count from the shared user
// data instead: at speed 0 they would never end, at speed 100 the host would
// count as gone after 40 ms. It works whether or not the hooks are installed.
std::uint64_t RealTickCount64();

namespace
{
    constexpr std::uint64_t kResumeFlag = 0x10000;
    constexpr std::uint64_t kDataSingleStep = 0x80000004;
    constexpr DWORD kWx86SingleStep = 0x4000001E;
    constexpr ULONGLONG kActiveWindowMs = 150;
    constexpr DWORD kSweepMs = 20;
    constexpr int kRecentSlots = 1024;
    constexpr int kMaxThreads = 8192;

    struct Recent
    {
        volatile DWORD Thread;
        volatile ULONGLONG Tick;
    };

    struct HostWatch
    {
        std::uint64_t Beat = 0;
        ULONGLONG Seen = 0;
    };

    bool HostGone(Block* block, HostWatch& watch)
    {
        const std::uint64_t beat = block->HostBeat;
        const ULONGLONG now = RealTickCount64();
        if (beat == 0 || beat != watch.Beat || watch.Seen == 0)
        {
            watch.Beat = beat;
            watch.Seen = now;
            return false;
        }
        return now - watch.Seen > kHostSilenceMs;
    }

    void LetGo(Block* block)
    {
        block->HostLost = block->HostLost + 1;
        block->Stop = 1;
    }

    HANDLE EventOf(std::uint64_t value)
    {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    }

    void SignalBreak(Block* block)
    {
        if (block->BreakEvent)
            ::SetEvent(EventOf(block->BreakEvent));
    }

    // The host sets the resume event right after the command, so the thread
    // goes on at once instead of after the next 15 ms scheduler tick that a
    // Sleep(2) would wait for.
    void WaitForResume(Block* block)
    {
        HostWatch host;
        const HANDLE resume = EventOf(block->ResumeEvent);
        while (block->ResumeCmd == ResumeNone && !block->Stop)
        {
            if (HostGone(block, host))
            {
                LetGo(block);
                break;
            }
            if (resume)
                ::WaitForSingleObject(resume, 20);
            else
                ::Sleep(2);
        }
    }

    constexpr int kMaxArmed = 32;
    constexpr DWORD kPrintException = 0x40010006;
    constexpr DWORD kPrintExceptionWide = 0x4001000A;
    constexpr DWORD kThreadNameException = 0x406D1388;
    constexpr DWORD kControlC = 0x40010005;
    constexpr DWORD kControlBreak = 0x40010008;

    Block* volatile g_block = nullptr;
    PVOID g_handler = nullptr;
    volatile LONG g_inside = 0;
    std::uint64_t g_armed[kMaxArmed] = {};
    volatile LONG g_armedCount = 0;
    DWORD g_applyThreadId = 0;
    HINSTANCE g_module = nullptr;
    volatile LONG g_started = 0;
    volatile LONG g_breaking = 0;
    volatile DWORD g_breakOwner = 0;
    DWORD g_suspended[kMaxThreads];
    int g_suspendedCount = 0;
    volatile LONG g_kept = 0;
    volatile ULONGLONG g_keptSince = 0;
    constexpr ULONGLONG kKeepMs = 300;
    Recent g_recent[kRecentSlots] = {};

    long volatile* LockOf(Block* block)
    {
        return reinterpret_cast<long volatile*>(&block->Lock);
    }

    std::uint64_t LengthBits(std::uint32_t size)
    {
        switch (size)
        {
        case 2: return 1;
        case 8: return 2;
        case 4: return 3;
        default: return 0;
        }
    }

    std::uint64_t TypeBits(std::uint32_t mode)
    {
        switch (mode)
        {
        case ModeWrite:
        case ModeBreakWrite: return 1;
        case ModeAccess:
        case ModeBreakAccess: return 3;
        default: return 0;
        }
    }

    bool Breaks(std::uint32_t mode)
    {
        return mode == ModeBreak || mode == ModeBreakWrite || mode == ModeBreakAccess;
    }

    struct SelfReader
    {
        bool operator()(std::uint64_t address, void* out, std::uint32_t size) const
        {
            SIZE_T done = 0;
            return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) && done == size;
        }
    };

#if defined(_M_X64)
    std::uint64_t InstructionPointer(const CONTEXT& c)
    {
        return c.Rip;
    }

    std::uint64_t ReturnAddress(const CONTEXT& c)
    {
        return *reinterpret_cast<const std::uint64_t*>(c.Rsp);
    }

    void CaptureRegs(Registers& r, const CONTEXT& c)
    {
        r.Rax = c.Rax; r.Rbx = c.Rbx; r.Rcx = c.Rcx; r.Rdx = c.Rdx;
        r.Rsi = c.Rsi; r.Rdi = c.Rdi; r.Rbp = c.Rbp; r.Rsp = c.Rsp;
        r.R8 = c.R8; r.R9 = c.R9; r.R10 = c.R10; r.R11 = c.R11;
        r.R12 = c.R12; r.R13 = c.R13; r.R14 = c.R14; r.R15 = c.R15;
        r.Rip = c.Rip;
        r.Flags = c.EFlags;
        r.GsBase = reinterpret_cast<std::uint64_t>(NtCurrentTeb());
    }

    void ApplyRegs(CONTEXT& c, const Registers& r)
    {
        c.Rax = r.Rax; c.Rbx = r.Rbx; c.Rcx = r.Rcx; c.Rdx = r.Rdx;
        c.Rsi = r.Rsi; c.Rdi = r.Rdi; c.Rbp = r.Rbp; c.Rsp = r.Rsp;
        c.R8 = r.R8; c.R9 = r.R9; c.R10 = r.R10; c.R11 = r.R11;
        c.R12 = r.R12; c.R13 = r.R13; c.R14 = r.R14; c.R15 = r.R15;
        c.Rip = r.Rip;
        c.EFlags = static_cast<DWORD>(r.Flags);
    }

    void SetDebugRegisters(CONTEXT& c, const std::uint64_t (&addresses)[4], std::uint64_t dr7)
    {
        c.Dr0 = addresses[0];
        c.Dr1 = addresses[1];
        c.Dr2 = addresses[2];
        c.Dr3 = addresses[3];
        c.Dr6 = 0;
        c.Dr7 = dr7;
    }

    constexpr DWORD kFullContext = CONTEXT_FULL;
    constexpr int kVectorCount = 16;

    bool HasVectors(const CONTEXT& c)
    {
        return (c.ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT;
    }

    unsigned char* VectorAt(CONTEXT& c, int index)
    {
        return reinterpret_cast<unsigned char*>(&c.FltSave.XmmRegisters[index]);
    }

    const unsigned char* VectorAt(const CONTEXT& c, int index)
    {
        return reinterpret_cast<const unsigned char*>(&c.FltSave.XmmRegisters[index]);
    }

    std::uint32_t Mxcsr(const CONTEXT& c)
    {
        return c.MxCsr;
    }

    unsigned char* FpuArea(CONTEXT& c)
    {
        return reinterpret_cast<unsigned char*>(&c.FltSave);
    }

    const unsigned char* FpuArea(const CONTEXT& c)
    {
        return reinterpret_cast<const unsigned char*>(&c.FltSave);
    }

    std::uint64_t DebugAddress(const CONTEXT& c, int slot)
    {
        const DWORD64 slots[4] = { c.Dr0, c.Dr1, c.Dr2, c.Dr3 };
        return slots[slot];
    }

    void SetMxcsr(CONTEXT& c, std::uint32_t value)
    {
        c.MxCsr = value;
        c.FltSave.MxCsr = value;
    }
#else
    constexpr int kFxsaveMxcsr = 24;
    constexpr int kFxsaveXmm = 160;

    std::uint64_t InstructionPointer(const CONTEXT& c)
    {
        return c.Eip;
    }

    std::uint64_t ReturnAddress(const CONTEXT& c)
    {
        return *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(c.Esp));
    }

    void CaptureRegs(Registers& r, const CONTEXT& c)
    {
        r = {};
        r.Rax = c.Eax; r.Rbx = c.Ebx; r.Rcx = c.Ecx; r.Rdx = c.Edx;
        r.Rsi = c.Esi; r.Rdi = c.Edi; r.Rbp = c.Ebp; r.Rsp = c.Esp;
        r.Rip = c.Eip;
        r.Flags = c.EFlags;
        r.GsBase = reinterpret_cast<std::uintptr_t>(NtCurrentTeb());
    }

    void ApplyRegs(CONTEXT& c, const Registers& r)
    {
        c.Eax = static_cast<DWORD>(r.Rax); c.Ebx = static_cast<DWORD>(r.Rbx); c.Ecx = static_cast<DWORD>(r.Rcx); c.Edx = static_cast<DWORD>(r.Rdx);
        c.Esi = static_cast<DWORD>(r.Rsi); c.Edi = static_cast<DWORD>(r.Rdi); c.Ebp = static_cast<DWORD>(r.Rbp); c.Esp = static_cast<DWORD>(r.Rsp);
        c.Eip = static_cast<DWORD>(r.Rip);
        c.EFlags = static_cast<DWORD>(r.Flags);
    }

    void SetDebugRegisters(CONTEXT& c, const std::uint64_t (&addresses)[4], std::uint64_t dr7)
    {
        c.Dr0 = static_cast<DWORD>(addresses[0]);
        c.Dr1 = static_cast<DWORD>(addresses[1]);
        c.Dr2 = static_cast<DWORD>(addresses[2]);
        c.Dr3 = static_cast<DWORD>(addresses[3]);
        c.Dr6 = 0;
        c.Dr7 = static_cast<DWORD>(dr7);
    }

    constexpr DWORD kFullContext = CONTEXT_FULL | CONTEXT_EXTENDED_REGISTERS;
    constexpr int kVectorCount = 8;

    bool HasVectors(const CONTEXT& c)
    {
        return (c.ContextFlags & CONTEXT_EXTENDED_REGISTERS) == CONTEXT_EXTENDED_REGISTERS;
    }

    unsigned char* VectorAt(CONTEXT& c, int index)
    {
        return c.ExtendedRegisters + kFxsaveXmm + index * 16;
    }

    const unsigned char* VectorAt(const CONTEXT& c, int index)
    {
        return c.ExtendedRegisters + kFxsaveXmm + index * 16;
    }

    std::uint32_t Mxcsr(const CONTEXT& c)
    {
        std::uint32_t value = 0;
        __movsb(reinterpret_cast<unsigned char*>(&value), c.ExtendedRegisters + kFxsaveMxcsr, sizeof(value));
        return value;
    }

    void SetMxcsr(CONTEXT& c, std::uint32_t value)
    {
        __movsb(c.ExtendedRegisters + kFxsaveMxcsr, reinterpret_cast<const unsigned char*>(&value), sizeof(value));
    }

    unsigned char* FpuArea(CONTEXT& c)
    {
        return c.ExtendedRegisters;
    }

    const unsigned char* FpuArea(const CONTEXT& c)
    {
        return c.ExtendedRegisters;
    }

    std::uint64_t DebugAddress(const CONTEXT& c, int slot)
    {
        const DWORD slots[4] = { c.Dr0, c.Dr1, c.Dr2, c.Dr3 };
        return slots[slot];
    }

#pragma comment(linker, "/EXPORT:ManifoldAgentStart=_ManifoldAgentStart@4")
#endif

    void ToArray(const CONTEXT& c, std::uint64_t (&r)[Manifold::Debug::Program::kRegisterCount])
    {
        Registers captured{};
        CaptureRegs(captured, c);
        r[0] = captured.Rax; r[1] = captured.Rbx; r[2] = captured.Rcx; r[3] = captured.Rdx;
        r[4] = captured.Rsi; r[5] = captured.Rdi; r[6] = captured.Rbp; r[7] = captured.Rsp;
        r[8] = captured.R8; r[9] = captured.R9; r[10] = captured.R10; r[11] = captured.R11;
        r[12] = captured.R12; r[13] = captured.R13; r[14] = captured.R14; r[15] = captured.R15;
        r[16] = captured.Rip;
        r[17] = captured.Flags;
    }

    void FromArray(CONTEXT& c, const std::uint64_t (&r)[Manifold::Debug::Program::kRegisterCount])
    {
        Registers changed{};
        changed.Rax = r[0]; changed.Rbx = r[1]; changed.Rcx = r[2]; changed.Rdx = r[3];
        changed.Rsi = r[4]; changed.Rdi = r[5]; changed.Rbp = r[6]; changed.Rsp = r[7];
        changed.R8 = r[8]; changed.R9 = r[9]; changed.R10 = r[10]; changed.R11 = r[11];
        changed.R12 = r[12]; changed.R13 = r[13]; changed.R14 = r[14]; changed.R15 = r[15];
        changed.Rip = r[16];
        changed.Flags = r[17];
        ApplyRegs(c, changed);
    }

    // The condition and the register changes run here, inside the hit, so a
    // breakpoint whose condition fails costs one exception and never stops the
    // other threads.
    bool RunAction(Action& action, CONTEXT& context, DWORD threadId)
    {
        if (action.ThreadId != 0 && action.ThreadId != threadId)
            return false;
        _InterlockedIncrement64(reinterpret_cast<long long volatile*>(&action.Seen));
        std::uint64_t registers[Manifold::Debug::Program::kRegisterCount];
        ToArray(context, registers);
        SelfReader reader;
        const std::uint32_t length = action.ConditionLength <= static_cast<std::uint32_t>(Manifold::Debug::Program::kMaxCode) ? action.ConditionLength : 0u;
        if (!Manifold::Debug::Program::Holds(action.Condition, length, registers, threadId, reader))
            return false;
        _InterlockedIncrement64(reinterpret_cast<long long volatile*>(&action.Hits));
        if (action.ChangeCount > 0 && Manifold::Debug::Program::Apply(action.Changes, static_cast<int>(action.ChangeCount), registers, threadId, reader) > 0)
            FromArray(context, registers);
        return action.Break != 0;
    }

    void BuildRegisters(Block* block, std::uint64_t (&addresses)[4], std::uint64_t& dr7)
    {
        dr7 = 0;
        for (int slot = 0; slot < kMaxWatches; ++slot)
        {
            addresses[slot] = 0;
            const Watch& watch = block->Watches[slot];
            if (!watch.Active)
                continue;
            const bool execute = watch.Mode == ModeInstruction || watch.Mode == ModeCalls || watch.Mode == ModeBreak;
            const bool data = !execute;
            addresses[slot] = watch.Address;
            dr7 |= 1ull << (slot * 2);
            dr7 |= (data ? TypeBits(watch.Mode) : 0ull) << (16 + slot * 4);
            dr7 |= (data ? LengthBits(watch.Size) : 0ull) << (18 + slot * 4);
        }
        if (dr7 != 0)
            dr7 |= (1ull << 8) | (1ull << 9);
    }

    bool Covers(std::uint64_t held, std::uint64_t armed, int slot)
    {
        const std::uint64_t lengths[4] = { 1, 2, 8, 4 };
        const std::uint64_t heldType = (held >> (16 + slot * 4)) & 3;
        const std::uint64_t armedType = (armed >> (16 + slot * 4)) & 3;
        const std::uint64_t heldLength = lengths[(held >> (18 + slot * 4)) & 3];
        const std::uint64_t armedLength = lengths[(armed >> (18 + slot * 4)) & 3];
        return (heldType == armedType || (heldType == 1 && armedType == 3)) && heldLength <= armedLength;
    }

    void MarkActive(DWORD threadId)
    {
        Recent& slot = g_recent[threadId % kRecentSlots];
        slot.Thread = threadId;
        slot.Tick = RealTickCount64();
    }

    bool RecentlyActive(DWORD threadId, ULONGLONG now)
    {
        const Recent& slot = g_recent[threadId % kRecentSlots];
        return slot.Thread == threadId && now - slot.Tick < kActiveWindowMs;
    }

    void Record(Block* block, int slot, const CONTEXT& context)
    {
        Watch& watch = block->Watches[slot];
        const std::uint64_t ip = InstructionPointer(context);
        const std::uint64_t caller = watch.Mode == ModeCalls ? ReturnAddress(context) : 0;
        while (_InterlockedCompareExchange(LockOf(block), 1, 0) != 0)
            _mm_pause();
        std::uint64_t count = watch.EntryCount;
        Entry* entries = watch.Entries;
        Entry* hit = nullptr;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            if (entries[i].Instruction == ip && entries[i].Accessed == caller)
            {
                hit = &entries[i];
                break;
            }
        }
        if (!hit && count < static_cast<std::uint64_t>(kMaxEntries))
        {
            hit = &entries[count];
            hit->Instruction = ip;
            hit->Accessed = caller;
            hit->Count = 0;
            hit->ThreadId = ::GetCurrentThreadId();
            hit->Pad = 0;
            CaptureRegs(hit->Regs, context);
            watch.EntryCount = count + 1;
        }
        if (hit)
            ++hit->Count;
        else
            ++watch.Overflow;
        _InterlockedExchange(LockOf(block), 0);
    }

    void CaptureVectors(Vectors& v, const CONTEXT& c)
    {
        if (!HasVectors(c))
        {
            v.Count = 0;
            return;
        }
        for (int i = 0; i < kVectorCount; ++i)
            __movsb(v.Xmm[i], VectorAt(c, i), 16);
        v.Mxcsr = Mxcsr(c);
        v.Count = kVectorCount;
    }

    void ApplyVectors(CONTEXT& c, const Vectors& v)
    {
        if (!HasVectors(c) || v.Count != static_cast<std::uint32_t>(kVectorCount))
            return;
        for (int i = 0; i < kVectorCount; ++i)
            __movsb(VectorAt(c, i), v.Xmm[i], 16);
        SetMxcsr(c, v.Mxcsr);
    }

    void CaptureFpu(Block* block, const CONTEXT& c)
    {
        if (!HasVectors(c))
        {
            block->FpuValid = 0;
            return;
        }
        __movsb(block->BreakFpu, FpuArea(c), kFpuBytes);
        block->FpuValid = 1;
    }

    // Only the x87 part goes back: control, status and tag words, the opcode
    // and pointers, and the eight stack registers. MXCSR and the XMM registers
    // between them belong to ApplyVectors.
    void ApplyFpu(CONTEXT& c, const std::uint8_t (&fpu)[kFpuBytes])
    {
        if (!HasVectors(c))
            return;
        unsigned char* area = FpuArea(c);
        __movsb(area, fpu, 24);
        __movsb(area + 32, fpu + 32, kFpuBytes - 32);
    }

    void RememberArmed(std::uint64_t address)
    {
        if (!address)
            return;
        const LONG count = g_armedCount;
        for (LONG i = 0; i < count; ++i)
        {
            if (g_armed[i] == address)
                return;
        }
        if (count < kMaxArmed)
        {
            g_armed[count] = address;
            g_armedCount = count + 1;
        }
    }

    bool WasArmed(std::uint64_t address)
    {
        const LONG count = g_armedCount;
        for (LONG i = 0; i < count; ++i)
        {
            if (g_armed[i] == address)
                return true;
        }
        return false;
    }

    bool Unexpected(DWORD code)
    {
        switch (code)
        {
        case kPrintException:
        case kPrintExceptionWide:
        case kThreadNameException:
        case kControlC:
        case kControlBreak:
        case static_cast<DWORD>(EXCEPTION_SINGLE_STEP):
        case kWx86SingleStep:
            return false;
        default:
            return true;
        }
    }

    bool BreaksOnException(Block* block, const EXCEPTION_RECORD& record)
    {
        const std::uint32_t mode = block->ExceptionMode;
        if (mode == ExceptionNever || !Unexpected(record.ExceptionCode))
            return false;
        if (mode == ExceptionAlways)
            return true;
        const std::uint64_t at = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        if (mode != ExceptionInRegion)
            return false;
        if (at >= block->ExceptionStart && at < block->ExceptionStop)
            return true;
        const std::uint32_t count = block->RegionCount < static_cast<std::uint32_t>(kMaxRegions) ? block->RegionCount : static_cast<std::uint32_t>(kMaxRegions);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (at >= block->Regions[i][0] && at < block->Regions[i][1])
                return true;
        }
        return false;
    }

    struct Inside
    {
        Inside() { _InterlockedIncrement(&g_inside); }
        ~Inside() { _InterlockedDecrement(&g_inside); }
        Inside(const Inside&) = delete;
        Inside& operator=(const Inside&) = delete;
    };

    void SuspendOthers()
    {
        const DWORD self = ::GetCurrentProcessId();
        const DWORD me = ::GetCurrentThreadId();
        g_suspendedCount = 0;
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (::Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != self || entry.th32ThreadID == me || entry.th32ThreadID == g_applyThreadId)
                    continue;
                HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (!thread)
                    continue;
                if (::SuspendThread(thread) != static_cast<DWORD>(-1) && g_suspendedCount < kMaxThreads)
                    g_suspended[g_suspendedCount++] = entry.th32ThreadID;
                ::CloseHandle(thread);
            } while (::Thread32Next(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
    }

    void ResumeOthers()
    {
        for (int i = 0; i < g_suspendedCount; ++i)
        {
            HANDLE thread = ::OpenThread(THREAD_SUSPEND_RESUME, FALSE, g_suspended[i]);
            if (thread)
            {
                ::ResumeThread(thread);
                ::CloseHandle(thread);
            }
        }
        g_suspendedCount = 0;
    }

    struct BreakCause
    {
        std::uint32_t Code = 0;
        std::uint32_t ParamCount = 0;
        std::uint64_t Params[2] = {};
    };

    // Returns whether the host changed registers while the thread stood here.
    bool EnterBreak(Block* block, CONTEXT& context, std::uint64_t address, std::uint32_t slot, const BreakCause& cause = {})
    {
        for (int attempt = 0; _InterlockedCompareExchange(&g_breaking, 1, 0) != 0; ++attempt)
        {
            for (int wait = 0; wait < 50 && g_breaking && !block->Broken && !block->Stop; ++wait)
                ::Sleep(1);
            const bool stopped = block->Broken != 0;
            while (block->Broken && !block->Stop)
                ::Sleep(1);
            // Only a real stop of another thread swallows this hit; the apply
            // thread also holds the flag for a moment to let kept threads go.
            if (stopped || block->Stop || attempt >= 3)
                return false;
            if (slot < static_cast<std::uint32_t>(kMaxWatches) && !block->Watches[slot].Active)
                return false;
            if (slot < static_cast<std::uint32_t>(kMaxWatches) && block->Actions[slot].ThreadId != 0 && block->Actions[slot].ThreadId != ::GetCurrentThreadId())
                return false;
            if (slot == kSlotException && block->ExceptionMode == ExceptionNever)
                return false;
        }
        g_breakOwner = ::GetCurrentThreadId();
        CaptureRegs(block->BreakRegs, context);
        CaptureVectors(block->BreakXmm, context);
        CaptureFpu(block, context);
        block->XmmDirty = 0;
        block->FpuDirty = 0;
        block->RegsDirty = 0;
        block->BreakCode = cause.Code;
        block->BreakParamCount = cause.ParamCount;
        block->BreakParams[0] = cause.Params[0];
        block->BreakParams[1] = cause.Params[1];
        block->BreakAddress = address;
        block->BreakSlot = slot;
        block->BrokenThread = ::GetCurrentThreadId();
        block->ResumeCmd = ResumeNone;
        if (!_InterlockedExchange(&g_kept, 0))
            SuspendOthers();
        block->BreakSerial = block->BreakSerial + 1;
        block->Broken = 1;
        SignalBreak(block);
        WaitForResume(block);
        bool changed = false;
        if (block->RegsDirty)
        {
            ApplyRegs(context, block->NewRegs);
            block->RegsDirty = 0;
            changed = true;
        }
        if (block->XmmDirty)
        {
            ApplyVectors(context, block->NewXmm);
            block->XmmDirty = 0;
            changed = true;
        }
        if (block->FpuDirty)
        {
            ApplyFpu(context, block->NewFpu);
            block->FpuDirty = 0;
            changed = true;
        }
        const bool handled = block->ExceptionHandled != 0;
        block->ExceptionHandled = 0;
        // A single step may leave the other threads stopped: one instruction
        // cannot wait on them, and skipping the thread snapshot makes a step
        // take well under a millisecond. The apply loop lets them go if the
        // step does not come back within kKeepMs (a syscall that waits).
        const bool keep = block->KeepSuspended != 0 && !block->Stop;
        block->KeepSuspended = 0;
        block->ResumeCmd = ResumeNone;
        block->Broken = 0;
        if (keep)
        {
            g_keptSince = RealTickCount64();
            _InterlockedExchange(&g_kept, 1);
        }
        else
            ResumeOthers();
        g_breakOwner = 0;
        _InterlockedExchange(&g_breaking, 0);
        return changed || handled;
    }

    // After the agent stopped, a thread whose debug registers could not be
    // cleared may still trap on an address the agent armed. Such a trap is
    // taken here and the thread disarmed; anything else goes on to the process.
    LONG Passive(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != static_cast<DWORD>(EXCEPTION_SINGLE_STEP) && code != static_cast<DWORD>(kDataSingleStep) && code != kWx86SingleStep)
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT& context = *info->ContextRecord;
        const std::uint64_t dr6 = context.Dr6;
        bool ours = false;
        for (int slot = 0; slot < 4 && !ours; ++slot)
            ours = (dr6 & (1ull << slot)) != 0 && WasArmed(DebugAddress(context, slot));
        if (!ours)
            return EXCEPTION_CONTINUE_SEARCH;
        const std::uint64_t none[4] = {};
        SetDebugRegisters(context, none, 0);
        context.EFlags |= kResumeFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    LONG ExceptionBreak(Block* block, EXCEPTION_POINTERS* info)
    {
        const EXCEPTION_RECORD& record = *info->ExceptionRecord;
        BreakCause cause;
        cause.Code = record.ExceptionCode;
        cause.ParamCount = record.NumberParameters < 2 ? record.NumberParameters : 2;
        for (std::uint32_t i = 0; i < cause.ParamCount; ++i)
            cause.Params[i] = record.ExceptionInformation[i];
        const std::uint64_t at = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        CONTEXT& context = *info->ContextRecord;
        if (!EnterBreak(block, context, at, kSlotException, cause))
            return EXCEPTION_CONTINUE_SEARCH;
        // ∑ An int3 that is still in the code would trap again at once if the
        //   thread went on from it, so going on means going past it, as a
        //   debugger does. An instruction pointer the user moved stays.
        if (record.ExceptionCode == static_cast<DWORD>(EXCEPTION_BREAKPOINT) && InstructionPointer(context) == at)
        {
            std::uint8_t byte = 0;
            SIZE_T done = 0;
            if (::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), &byte, 1, &done) && done == 1 && byte == 0xCC)
            {
#if defined(_M_X64)
                context.Rip = at + 1;
#else
                context.Eip = static_cast<DWORD>(at + 1);
#endif
            }
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // The code filter's int3s come first: with "break on all exceptions" this
    // handler would otherwise stop at every one when it runs before the
    // filter's own handler.
    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode == static_cast<DWORD>(EXCEPTION_BREAKPOINT) && CoverFilter(info) == EXCEPTION_CONTINUE_EXECUTION)
            return EXCEPTION_CONTINUE_EXECUTION;
        const Inside inside;
        Block* block = g_block;
        if (!block)
            return Passive(info);
        if (block->Magic != kMagic)
            return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != static_cast<DWORD>(EXCEPTION_SINGLE_STEP) && code != static_cast<DWORD>(kDataSingleStep) && code != kWx86SingleStep)
        {
            const DWORD self = ::GetCurrentThreadId();
            if (block->Stop || self == g_applyThreadId || self == g_breakOwner || !BreaksOnException(block, *info->ExceptionRecord))
                return EXCEPTION_CONTINUE_SEARCH;
            return ExceptionBreak(block, info);
        }
        CONTEXT& context = *info->ContextRecord;
        const std::uint64_t dr6 = context.Dr6;
        if ((dr6 & 0xF) == 0 && ((context.Dr7 & 0xFF) == 0 || (dr6 & 0x4000) != 0))
            return EXCEPTION_CONTINUE_SEARCH;
        const DWORD tid = ::GetCurrentThreadId();
        MarkActive(tid);
        // A thread that traps while another one is stopping the process waits
        // here instead of running on until SuspendOthers reaches it. The wait
        // is short on purpose: this thread may hold a lock (the heap's) that
        // the stopping thread needs for its thread snapshot.
        for (int wait = 0; wait < 50 && g_breaking && g_breakOwner != tid && !block->Broken && !block->Stop; ++wait)
            ::Sleep(1);
        bool breakHit = false;
        std::uint64_t breakAddr = 0;
        std::uint32_t breakSlot = 0;
        std::uint64_t addresses[4] = {};
        std::uint64_t dr7 = 0;
        BuildRegisters(block, addresses, dr7);
        for (int slot = 0; slot < kMaxWatches; ++slot)
        {
            const Watch& watch = block->Watches[slot];
            if (!watch.Active)
                continue;
            if ((dr6 & (1ull << slot)) && DebugAddress(context, slot) == watch.Address && Covers(context.Dr7, dr7, slot))
            {
                if (Breaks(watch.Mode))
                {
                    const std::uint64_t at = InstructionPointer(context);
                    if (RunAction(block->Actions[slot], context, tid) && !breakHit && !block->Stop)
                    {
                        breakHit = true;
                        breakAddr = at;
                        breakSlot = static_cast<std::uint32_t>(slot);
                    }
                }
                else
                {
                    Record(block, slot, context);
                    _InterlockedIncrement64(reinterpret_cast<long long volatile*>(&block->Hits));
                }
            }
        }
        if (breakHit)
            EnterBreak(block, context, breakAddr, breakSlot);
        BuildRegisters(block, addresses, dr7);
        SetDebugRegisters(context, addresses, dr7);
        context.EFlags |= kResumeFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Returns false only when the thread is still there and its debug
    // registers did not read back as written.
    bool ArmThread(Block* block, DWORD threadId, const std::uint64_t (&addresses)[4], std::uint64_t dr7, bool verify)
    {
        HANDLE thread = ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
        if (!thread)
            return true;
        bool armed = true;
        if (::SuspendThread(thread) != static_cast<DWORD>(-1))
        {
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            SetDebugRegisters(context, addresses, dr7);
            if (::SetThreadContext(thread, &context))
            {
                block->AppliedThreads = block->AppliedThreads + 1;
                if (dr7 != 0)
                    MarkActive(threadId);
            }
            if (verify)
            {
                alignas(16) CONTEXT check{};
                check.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                armed = ::GetThreadContext(thread, &check) && (check.Dr7 & 0xFF) == (dr7 & 0xFF);
            }
            ::ResumeThread(thread);
        }
        ::CloseHandle(thread);
        return armed;
    }

    int Sweep(Block* block, const std::uint64_t (&addresses)[4], std::uint64_t dr7, bool all, bool verify = false)
    {
        const DWORD self = ::GetCurrentProcessId();
        const ULONGLONG now = RealTickCount64();
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return -1;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        block->AppliedThreads = 0;
        int failed = 0;
        if (::Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != self || entry.th32ThreadID == g_applyThreadId)
                    continue;
                if (!all && RecentlyActive(entry.th32ThreadID, now))
                    continue;
                if (!ArmThread(block, entry.th32ThreadID, addresses, dr7, verify))
                    ++failed;
            } while (::Thread32Next(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
        return failed;
    }

    bool WaitUntilOutside(ULONGLONG milliseconds)
    {
        const ULONGLONG until = RealTickCount64() + milliseconds;
        while (g_inside != 0)
        {
            if (RealTickCount64() > until)
                return false;
            ::Sleep(1);
        }
        return true;
    }

    // Every thread must read back with its debug registers off before the
    // handler goes: a thread that still traps after that has nobody to take
    // its single step and the process dies (STATUS_SINGLE_STEP, no WER).
    // When that cannot be proven, the handler stays as a passive one and the
    // module stays loaded; when a thread is still inside the handler, the
    // module stays loaded too, so no thread runs code that is gone.
    bool Release(Block* block)
    {
        for (int slot = 0; slot < kMaxWatches; ++slot)
        {
            RememberArmed(block->Watches[slot].Address);
            block->Watches[slot].Active = 0;
        }
        const ULONGLONG until = RealTickCount64() + 3000;
        while (g_breaking && RealTickCount64() < until)
            ::Sleep(1);
        if (_InterlockedExchange(&g_kept, 0))
            ResumeOthers();
        const std::uint64_t none[4] = {};
        bool disarmed = false;
        for (int pass = 0; pass < 40 && !disarmed; ++pass)
        {
            disarmed = Sweep(block, none, 0, true, true) == 0;
            ::Sleep(15);
        }
        if (disarmed)
            disarmed = Sweep(block, none, 0, true, true) == 0;
        g_block = nullptr;
        const bool outside = WaitUntilOutside(3000);
        bool unload = false;
        if (disarmed && outside && g_handler)
        {
            ::RemoveVectoredExceptionHandler(g_handler);
            g_handler = nullptr;
            unload = WaitUntilOutside(2000);
        }
        if (block->BreakEvent)
            ::CloseHandle(EventOf(block->BreakEvent));
        if (block->ResumeEvent)
            ::CloseHandle(EventOf(block->ResumeEvent));
        block->BreakEvent = 0;
        block->ResumeEvent = 0;
        block->AppliedThreads = 0;
        block->Running = 0;
        return unload;
    }

    DWORD WINAPI ApplyLoop(LPVOID)
    {
        g_applyThreadId = ::GetCurrentThreadId();
        Block* block = g_block;
        std::uint32_t lastClear[kMaxWatches] = {};
        HostWatch host;
        while (block && !block->Stop)
        {
            if (!block->Broken && HostGone(block, host))
            {
                LetGo(block);
                break;
            }
            for (int slot = 0; slot < kMaxWatches; ++slot)
            {
                if (block->Watches[slot].Clear != lastClear[slot])
                {
                    while (_InterlockedCompareExchange(LockOf(block), 1, 0) != 0)
                        _mm_pause();
                    block->Watches[slot].EntryCount = 0;
                    block->Watches[slot].Overflow = 0;
                    _InterlockedExchange(LockOf(block), 0);
                    lastClear[slot] = block->Watches[slot].Clear;
                }
            }
            if (g_kept && !block->Broken && RealTickCount64() - g_keptSince > kKeepMs && _InterlockedCompareExchange(&g_breaking, 1, 0) == 0)
            {
                if (_InterlockedExchange(&g_kept, 0))
                    ResumeOthers();
                _InterlockedExchange(&g_breaking, 0);
            }
            if (block->BreakNow && !block->Broken && _InterlockedCompareExchange(&g_breaking, 1, 0) == 0)
            {
                g_breakOwner = ::GetCurrentThreadId();
                block->BreakNow = 0;
                if (_InterlockedExchange(&g_kept, 0))
                    ResumeOthers();
                SuspendOthers();
                const DWORD target = g_suspendedCount > 0 ? g_suspended[0] : 0;
                HANDLE thread = target ? ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, target) : nullptr;
                alignas(16) CONTEXT ctx{};
                ctx.ContextFlags = kFullContext;
                if (thread && ::GetThreadContext(thread, &ctx))
                {
                    CaptureRegs(block->BreakRegs, ctx);
                    CaptureVectors(block->BreakXmm, ctx);
                    CaptureFpu(block, ctx);
                    block->XmmDirty = 0;
                    block->FpuDirty = 0;
                    block->RegsDirty = 0;
                    block->BreakCode = 0;
                    block->BreakParamCount = 0;
                    block->BreakRegs.GsBase = 0;
                    block->BreakAddress = InstructionPointer(ctx);
                    block->BreakSlot = kSlotBreakNow;
                    block->BrokenThread = target;
                    block->ResumeCmd = ResumeNone;
                    block->BreakSerial = block->BreakSerial + 1;
                    block->Broken = 1;
                    SignalBreak(block);
                    WaitForResume(block);
                    block->KeepSuspended = 0;
                    const bool registers = block->RegsDirty != 0;
                    const bool vectors = block->XmmDirty != 0;
                    const bool fpu = block->FpuDirty != 0;
                    if (registers)
                        ApplyRegs(ctx, block->NewRegs);
                    if (vectors)
                        ApplyVectors(ctx, block->NewXmm);
                    if (fpu)
                        ApplyFpu(ctx, block->NewFpu);
                    if (registers || vectors || fpu)
                        ::SetThreadContext(thread, &ctx);
                    block->RegsDirty = 0;
                    block->XmmDirty = 0;
                    block->FpuDirty = 0;
                    block->ExceptionHandled = 0;
                    block->ResumeCmd = ResumeNone;
                    block->Broken = 0;
                }
                if (thread)
                    ::CloseHandle(thread);
                ResumeOthers();
                g_breakOwner = 0;
                _InterlockedExchange(&g_breaking, 0);
            }
            if (!block->Broken)
            {
                std::uint64_t addresses[4] = {};
                std::uint64_t dr7 = 0;
                BuildRegisters(block, addresses, dr7);
                for (int slot = 0; slot < 4; ++slot)
                    RememberArmed(addresses[slot]);
                const std::uint32_t command = block->Command;
                if (dr7 != 0 || block->Applied != command)
                    Sweep(block, addresses, dr7, false);
                block->Applied = command;
            }
            ::Sleep(kSweepMs);
        }
        // ∑ A clean release lets go of the agent; the agent's lifetime unloads it once no
        //   part needs it (Agent/Life.cpp). An unclean one keeps it, so no thread runs
        //   code that is gone.
        const bool unload = block && Release(block);
        _InterlockedExchange(&g_started, 0);
        if (unload)
            LifeLetGo(Manifold::Target::Life::PartDebugger);
        return 0;
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI ManifoldAgentStart(LPVOID parameter)
{
    Block* block = static_cast<Block*>(parameter);
    if (!block || block->Magic != kMagic || block->Version != kVersion)
        return 1;
    if (_InterlockedCompareExchange(&g_started, 1, 0) != 0)
    {
        block->LastError = ERROR_BUSY;
        return 2;
    }
    block->Stop = 0;
    block->AppliedThreads = 0;
    for (int i = 0; i < kRecentSlots; ++i)
    {
        g_recent[i].Thread = 0;
        g_recent[i].Tick = 0;
    }
    g_block = block;
    const bool addedHandler = !g_handler;
    if (!g_handler)
        g_handler = ::AddVectoredExceptionHandler(1, &Handler);
    if (!g_handler)
    {
        g_block = nullptr;
        block->LastError = ::GetLastError();
        block->Running = 0;
        _InterlockedExchange(&g_started, 0);
        return 3;
    }
    DWORD applyId = 0;
    HANDLE applyThread = ::CreateThread(nullptr, 0, &ApplyLoop, nullptr, 0, &applyId);
    if (!applyThread)
    {
        block->LastError = ::GetLastError();
        g_block = nullptr;
        if (addedHandler)
        {
            ::RemoveVectoredExceptionHandler(g_handler);
            g_handler = nullptr;
        }
        block->Running = 0;
        _InterlockedExchange(&g_started, 0);
        return 4;
    }
    ::CloseHandle(applyThread);
    block->Running = 1;
    LifeHold(Manifold::Target::Life::PartDebugger);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        ::DisableThreadLibraryCalls(instance);
        SpeedAttach(instance);
        CoverAttach(instance);
        CrashAttach();
        LifeAttach(instance);
    }
    else if (reason == DLL_PROCESS_DETACH && !reserved)
    {
        CoverDetach();
        CrashDetach();
    }
    return TRUE;
}
