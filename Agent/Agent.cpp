#include "Debug/VehProtocol.h"

#include <windows.h>
#include <tlhelp32.h>

#include <intrin.h>

#include <vector>

using namespace Manifold::Debug::Veh;

namespace
{
    constexpr std::uint64_t kResumeFlag = 0x10000;
    constexpr std::uint64_t kDataSingleStep = 0x80000004;
    constexpr ULONGLONG kActiveWindowMs = 150;
    constexpr DWORD kSweepMs = 20;
    constexpr int kRecentSlots = 1024;
    constexpr int kMaxThreads = 8192;

    struct Recent
    {
        volatile DWORD Thread;
        volatile ULONGLONG Tick;
    };

    Block* g_block = nullptr;
    PVOID g_handler = nullptr;
    DWORD g_applyThreadId = 0;
    HINSTANCE g_module = nullptr;
    volatile LONG g_started = 0;
    volatile LONG g_breaking = 0;
    volatile DWORD g_breakOwner = 0;
    DWORD g_suspended[kMaxThreads];
    int g_suspendedCount = 0;
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

    void ToArray(const CONTEXT& c, std::uint64_t (&r)[Manifold::Debug::Program::kRegisterCount])
    {
        r[0] = c.Rax; r[1] = c.Rbx; r[2] = c.Rcx; r[3] = c.Rdx; r[4] = c.Rsi; r[5] = c.Rdi; r[6] = c.Rbp; r[7] = c.Rsp;
        r[8] = c.R8; r[9] = c.R9; r[10] = c.R10; r[11] = c.R11; r[12] = c.R12; r[13] = c.R13; r[14] = c.R14; r[15] = c.R15;
        r[16] = c.Rip;
        r[17] = c.EFlags;
    }

    void FromArray(CONTEXT& c, const std::uint64_t (&r)[Manifold::Debug::Program::kRegisterCount])
    {
        c.Rax = r[0]; c.Rbx = r[1]; c.Rcx = r[2]; c.Rdx = r[3]; c.Rsi = r[4]; c.Rdi = r[5]; c.Rbp = r[6]; c.Rsp = r[7];
        c.R8 = r[8]; c.R9 = r[9]; c.R10 = r[10]; c.R11 = r[11]; c.R12 = r[12]; c.R13 = r[13]; c.R14 = r[14]; c.R15 = r[15];
        c.Rip = r[16];
        c.EFlags = static_cast<DWORD>(r[17]);
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

    void MarkActive(DWORD threadId)
    {
        Recent& slot = g_recent[threadId % kRecentSlots];
        slot.Thread = threadId;
        slot.Tick = ::GetTickCount64();
    }

    bool RecentlyActive(DWORD threadId, ULONGLONG now)
    {
        const Recent& slot = g_recent[threadId % kRecentSlots];
        return slot.Thread == threadId && now - slot.Tick < kActiveWindowMs;
    }

    void Record(Block* block, int slot, const CONTEXT& context)
    {
        Watch& watch = block->Watches[slot];
        const std::uint64_t ip = context.Rip;
        while (_InterlockedCompareExchange(LockOf(block), 1, 0) != 0)
            _mm_pause();
        std::uint64_t count = watch.EntryCount;
        Entry* entries = watch.Entries;
        Entry* hit = nullptr;
        for (std::uint64_t i = 0; i < count; ++i)
        {
            if (entries[i].Instruction == ip)
            {
                hit = &entries[i];
                break;
            }
        }
        if (!hit && count < static_cast<std::uint64_t>(kMaxEntries))
        {
            hit = &entries[count];
            hit->Instruction = ip;
            hit->Accessed = 0;
            hit->Count = 0;
            hit->ThreadId = ::GetCurrentThreadId();
            hit->Pad = 0;
            Registers& r = hit->Regs;
            r.Rax = context.Rax; r.Rbx = context.Rbx; r.Rcx = context.Rcx; r.Rdx = context.Rdx;
            r.Rsi = context.Rsi; r.Rdi = context.Rdi; r.Rbp = context.Rbp; r.Rsp = context.Rsp;
            r.R8 = context.R8; r.R9 = context.R9; r.R10 = context.R10; r.R11 = context.R11;
            r.R12 = context.R12; r.R13 = context.R13; r.R14 = context.R14; r.R15 = context.R15;
            r.Rip = context.Rip;
            r.Flags = context.EFlags;
            r.GsBase = reinterpret_cast<std::uint64_t>(NtCurrentTeb());
            watch.EntryCount = count + 1;
        }
        if (hit)
            ++hit->Count;
        else
            ++watch.Overflow;
        _InterlockedExchange(LockOf(block), 0);
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

    void CaptureVectors(Vectors& v, const CONTEXT& c)
    {
        if ((c.ContextFlags & CONTEXT_FLOATING_POINT) != CONTEXT_FLOATING_POINT)
        {
            v.Count = 0;
            return;
        }
        for (int i = 0; i < 16; ++i)
            __movsb(v.Xmm[i], reinterpret_cast<const unsigned char*>(&c.FltSave.XmmRegisters[i]), 16);
        v.Mxcsr = c.MxCsr;
        v.Count = 16;
    }

    void ApplyVectors(CONTEXT& c, const Vectors& v)
    {
        if ((c.ContextFlags & CONTEXT_FLOATING_POINT) != CONTEXT_FLOATING_POINT || v.Count != 16)
            return;
        for (int i = 0; i < 16; ++i)
            __movsb(reinterpret_cast<unsigned char*>(&c.FltSave.XmmRegisters[i]), v.Xmm[i], 16);
        c.MxCsr = v.Mxcsr;
        c.FltSave.MxCsr = v.Mxcsr;
    }

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

    void EnterBreak(Block* block, CONTEXT& context, std::uint64_t address, std::uint32_t slot)
    {
        if (_InterlockedCompareExchange(&g_breaking, 1, 0) != 0)
        {
            for (int wait = 0; wait < 50 && g_breaking && !block->Broken && !block->Stop; ++wait)
                ::Sleep(1);
            while (block->Broken && !block->Stop)
                ::Sleep(1);
            return;
        }
        g_breakOwner = ::GetCurrentThreadId();
        CaptureRegs(block->BreakRegs, context);
        CaptureVectors(block->BreakXmm, context);
        block->XmmDirty = 0;
        block->BreakAddress = address;
        block->BreakSlot = slot;
        block->BrokenThread = ::GetCurrentThreadId();
        block->ResumeCmd = ResumeNone;
        SuspendOthers();
        block->BreakSerial = block->BreakSerial + 1;
        block->Broken = 1;
        while (block->ResumeCmd == ResumeNone && !block->Stop)
            ::Sleep(2);
        if (block->RegsDirty)
        {
            ApplyRegs(context, block->NewRegs);
            block->RegsDirty = 0;
        }
        if (block->XmmDirty)
        {
            ApplyVectors(context, block->NewXmm);
            block->XmmDirty = 0;
        }
        block->ResumeCmd = ResumeNone;
        block->Broken = 0;
        ResumeOthers();
        g_breakOwner = 0;
        _InterlockedExchange(&g_breaking, 0);
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        Block* block = g_block;
        if (!block || block->Magic != kMagic)
            return EXCEPTION_CONTINUE_SEARCH;
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code != static_cast<DWORD>(EXCEPTION_SINGLE_STEP) && code != static_cast<DWORD>(kDataSingleStep))
            return EXCEPTION_CONTINUE_SEARCH;
        CONTEXT& context = *info->ContextRecord;
        const std::uint64_t dr6 = context.Dr6;
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
        for (int slot = 0; slot < kMaxWatches; ++slot)
        {
            const Watch& watch = block->Watches[slot];
            if (!watch.Active)
                continue;
            if (dr6 & (1ull << slot))
            {
                if (Breaks(watch.Mode))
                {
                    const std::uint64_t at = context.Rip;
                    if (RunAction(block->Actions[slot], context, tid) && !breakHit)
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
        std::uint64_t addresses[4] = {};
        std::uint64_t dr7 = 0;
        BuildRegisters(block, addresses, dr7);
        context.Dr0 = addresses[0];
        context.Dr1 = addresses[1];
        context.Dr2 = addresses[2];
        context.Dr3 = addresses[3];
        context.Dr7 = dr7;
        context.Dr6 = 0;
        context.EFlags |= kResumeFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ArmThread(DWORD threadId, const std::uint64_t (&addresses)[4], std::uint64_t dr7)
    {
        HANDLE thread = ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
        if (!thread)
            return;
        if (::SuspendThread(thread) != static_cast<DWORD>(-1))
        {
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            context.Dr0 = addresses[0];
            context.Dr1 = addresses[1];
            context.Dr2 = addresses[2];
            context.Dr3 = addresses[3];
            context.Dr6 = 0;
            context.Dr7 = dr7;
            if (::SetThreadContext(thread, &context))
            {
                g_block->AppliedThreads = g_block->AppliedThreads + 1;
                if (dr7 != 0)
                    MarkActive(threadId);
            }
            ::ResumeThread(thread);
        }
        ::CloseHandle(thread);
    }

    void Sweep(const std::uint64_t (&addresses)[4], std::uint64_t dr7, bool all)
    {
        const DWORD self = ::GetCurrentProcessId();
        const ULONGLONG now = ::GetTickCount64();
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        g_block->AppliedThreads = 0;
        if (::Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != self || entry.th32ThreadID == g_applyThreadId)
                    continue;
                if (!all && RecentlyActive(entry.th32ThreadID, now))
                    continue;
                ArmThread(entry.th32ThreadID, addresses, dr7);
            } while (::Thread32Next(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
    }

    DWORD WINAPI ApplyLoop(LPVOID)
    {
        g_applyThreadId = ::GetCurrentThreadId();
        Block* block = g_block;
        std::uint32_t lastClear[kMaxWatches] = {};
        while (block && !block->Stop)
        {
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
            if (block->BreakNow && !block->Broken && _InterlockedCompareExchange(&g_breaking, 1, 0) == 0)
            {
                g_breakOwner = ::GetCurrentThreadId();
                block->BreakNow = 0;
                SuspendOthers();
                const DWORD target = g_suspendedCount > 0 ? g_suspended[0] : 0;
                HANDLE thread = target ? ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, target) : nullptr;
                alignas(16) CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                if (thread && ::GetThreadContext(thread, &ctx))
                {
                    CaptureRegs(block->BreakRegs, ctx);
                    CaptureVectors(block->BreakXmm, ctx);
                    block->XmmDirty = 0;
                    block->BreakRegs.GsBase = 0;
                    block->BreakAddress = ctx.Rip;
                    block->BreakSlot = 0xFFFFFFFFu;
                    block->BrokenThread = target;
                    block->ResumeCmd = ResumeNone;
                    block->BreakSerial = block->BreakSerial + 1;
                    block->Broken = 1;
                    while (block->ResumeCmd == ResumeNone && !block->Stop)
                        ::Sleep(2);
                    const bool registers = block->RegsDirty != 0;
                    const bool vectors = block->XmmDirty != 0;
                    if (registers)
                        ApplyRegs(ctx, block->NewRegs);
                    if (vectors)
                        ApplyVectors(ctx, block->NewXmm);
                    if (registers || vectors)
                        ::SetThreadContext(thread, &ctx);
                    block->RegsDirty = 0;
                    block->XmmDirty = 0;
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
                Sweep(addresses, dr7, false);
                block->Applied = block->Command;
            }
            ::Sleep(kSweepMs);
        }
        if (block)
        {
            for (int slot = 0; slot < kMaxWatches; ++slot)
                block->Watches[slot].Active = 0;
            std::uint64_t none[4] = {};
            for (int pass = 0; pass < 6; ++pass)
            {
                Sweep(none, 0, true);
                ::Sleep(15);
            }
            if (g_handler)
            {
                ::RemoveVectoredExceptionHandler(g_handler);
                g_handler = nullptr;
            }
            block->AppliedThreads = 0;
            block->Running = 0;
        }
        _InterlockedExchange(&g_started, 0);
        if (g_module)
            ::FreeLibraryAndExitThread(g_module, 0);
        return 0;
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI ManifoldAgentStart(LPVOID parameter)
{
    Block* block = static_cast<Block*>(parameter);
    if (!block || block->Magic != kMagic || block->Version != kVersion)
        return 1;
    if (_InterlockedCompareExchange(&g_started, 1, 0) != 0)
        return 2;
    g_block = block;
    block->Stop = 0;
    block->AppliedThreads = 0;
    for (int i = 0; i < kRecentSlots; ++i)
    {
        g_recent[i].Thread = 0;
        g_recent[i].Tick = 0;
    }
    g_handler = ::AddVectoredExceptionHandler(1, &Handler);
    if (!g_handler)
    {
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
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
        block->Running = 0;
        _InterlockedExchange(&g_started, 0);
        return 4;
    }
    ::CloseHandle(applyThread);
    block->Running = 1;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        ::DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
