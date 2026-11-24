#include "Target/AgentLifeProtocol.h"
#include "Target/CrashProtocol.h"

#include <windows.h>

#include <intrin.h>

using namespace Manifold::Target::Crash;

extern "C" __declspec(dllexport) Block ManifoldCrash = {};

std::uint64_t RealTickCount64();
void LifeHold(std::uint32_t part);
void LifeLetGo(std::uint32_t part);
void LifeTouch();

namespace
{
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    Block& g_block = ManifoldCrash;
    Ring* volatile g_ring = nullptr;
    PVOID g_handler = nullptr;
    volatile LONG g_inside = 0;
    volatile LONG g_callLock = 0;

    struct Inside
    {
        Inside() { _InterlockedIncrement(&g_inside); }
        ~Inside() { _InterlockedDecrement(&g_inside); }
    };

    struct Held
    {
        Held()
        {
            for (int spin = 0; _InterlockedCompareExchange(&g_callLock, 1, 0) != 0; ++spin)
            {
                if (spin < 64)
                    _mm_pause();
                else
                    ::SwitchToThread();
            }
        }
        ~Held() { _InterlockedExchange(&g_callLock, 0); }
    };

    // ∑ Only the kinds that end a process when nobody handles them. Breakpoints
    //   and single steps belong to the debuggers and the code filter, C++ and
    //   CLR exceptions are thrown on purpose and caught all the time.
    bool Fatal(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case EXCEPTION_INVALID_DISPOSITION:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case 0xC0000374u:  // heap corruption
        case 0xC00002B4u:  // several float faults (SSE)
        case 0xC00002B5u:  // several float traps (SSE)
            return true;
        default:
            return false;
        }
    }

    std::uint32_t Readable(std::uintptr_t address, std::uint32_t wanted)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!::VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || !(info.Protect & kReadable))
            return 0;
        const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        return static_cast<std::uint32_t>(end - address < wanted ? end - address : wanted);
    }

    void Registers(const CONTEXT& context, std::uint64_t* out)
    {
#if defined(_M_X64)
        out[RegIp] = context.Rip;
        out[RegSp] = context.Rsp;
        out[RegAx] = context.Rax;
        out[RegBx] = context.Rbx;
        out[RegCx] = context.Rcx;
        out[RegDx] = context.Rdx;
        out[RegSi] = context.Rsi;
        out[RegDi] = context.Rdi;
        out[RegBp] = context.Rbp;
        out[RegR8] = context.R8;
        out[RegR9] = context.R9;
        out[RegR10] = context.R10;
        out[RegR11] = context.R11;
        out[RegR12] = context.R12;
        out[RegR13] = context.R13;
        out[RegR14] = context.R14;
        out[RegR15] = context.R15;
#else
        out[RegIp] = context.Eip;
        out[RegSp] = context.Esp;
        out[RegAx] = context.Eax;
        out[RegBx] = context.Ebx;
        out[RegCx] = context.Ecx;
        out[RegDx] = context.Edx;
        out[RegSi] = context.Esi;
        out[RegDi] = context.Edi;
        out[RegBp] = context.Ebp;
        for (std::uint32_t i = RegR8; i < kRegisters; ++i)
            out[i] = 0;
#endif
    }

    // ∑ The stack between the stack pointer and the stack's base is committed,
    //   so it is copied without a check; a stack pointer outside the thread's
    //   own stack (a fiber, a corrupted rsp) leaves the copy empty.
    std::uint32_t CopyStack(std::uintptr_t sp, std::uint64_t* out)
    {
        const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
        const auto base = reinterpret_cast<std::uintptr_t>(tib->StackBase);
        const auto limit = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
        if (sp < limit || sp >= base || (sp & (sizeof(std::uintptr_t) - 1)) != 0)
            return 0;
        const std::uintptr_t words = (base - sp) / sizeof(std::uintptr_t);
        const std::uint32_t count = static_cast<std::uint32_t>(words < kStackWords ? words : kStackWords);
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(sp);
        for (std::uint32_t i = 0; i < count; ++i)
            out[i] = stack[i];
        return count;
    }

    void Note(const EXCEPTION_RECORD& exception, const CONTEXT& context, Ring& ring)
    {
        const std::uint64_t number = static_cast<std::uint64_t>(_InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&ring.Next))) - 1;
        Record& record = ring.Slots[number % kSlots];
        record.Sequence = static_cast<std::uint32_t>(number * 2 + 1);
        _ReadWriteBarrier();
        record.Code = exception.ExceptionCode;
        record.Flags = exception.ExceptionFlags;
        record.ThreadId = ::GetCurrentThreadId();
        record.Tick = RealTickCount64();
        record.Address = reinterpret_cast<std::uintptr_t>(exception.ExceptionAddress);
        record.InfoCount = exception.NumberParameters < 2 ? exception.NumberParameters : 2;
        record.Info[0] = record.InfoCount > 0 ? exception.ExceptionInformation[0] : 0;
        record.Info[1] = record.InfoCount > 1 ? exception.ExceptionInformation[1] : 0;
        Registers(context, record.Registers);
        record.StackCount = CopyStack(static_cast<std::uintptr_t>(record.Registers[RegSp]), record.Stack);
        const auto ip = static_cast<std::uintptr_t>(record.Registers[RegIp]);
        record.ByteCount = Readable(ip, kCodeBytes);
        const auto* code = reinterpret_cast<const std::uint8_t*>(ip);
        for (std::uint32_t i = 0; i < record.ByteCount; ++i)
            record.Bytes[i] = code[i];
        _ReadWriteBarrier();
        ::MemoryBarrier();
        record.Sequence = static_cast<std::uint32_t>(number * 2 + 2);
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        if (!Fatal(info->ExceptionRecord->ExceptionCode))
            return EXCEPTION_CONTINUE_SEARCH;
        const Inside inside;
        if (Ring* ring = g_ring)
            Note(*info->ExceptionRecord, *info->ContextRecord, *ring);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void WaitOutside(DWORD milliseconds)
    {
        const ULONGLONG until = RealTickCount64() + milliseconds;
        while (g_inside != 0 && RealTickCount64() < until)
            ::Sleep(1);
    }

    std::uint32_t Stop()
    {
        if (g_handler)
        {
            ::RemoveVectoredExceptionHandler(g_handler);
            g_handler = nullptr;
        }
        Ring* ring = g_ring;
        g_ring = nullptr;
        g_block.Running = 0;
        WaitOutside(2000);
        if (g_inside != 0)
            return ResultStillInside;
        if (ring)
            ::UnmapViewOfFile(ring);
        return ResultOk;
    }

    std::uint32_t Start()
    {
        if (const std::uint32_t stopped = Stop(); stopped != ResultOk)
            return stopped;
        const HANDLE mapping = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_block.Mapping));
        g_block.Mapping = 0;
        if (!mapping)
            return ResultNoMapping;
        void* view = ::MapViewOfFile(mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(Ring));
        g_block.LastError = view ? 0 : ::GetLastError();
        ::CloseHandle(mapping);
        auto* ring = static_cast<Ring*>(view);
        if (!ring || ring->Magic != kMagic || ring->Version != kVersion)
        {
            if (ring)
                ::UnmapViewOfFile(ring);
            return ResultNoMapping;
        }
        ring->ProcessId = ::GetCurrentProcessId();
        ring->Is64Bit = sizeof(void*) == 8 ? 1u : 0u;
        g_ring = ring;
        // ∑ Last in the chain: an exception a handler before it resolves (the
        //   debuggers' breakpoints, the code filter, a runtime's own null
        //   checks) never reaches it.
        g_handler = ::AddVectoredExceptionHandler(0, &Handler);
        if (!g_handler)
        {
            g_block.LastError = ::GetLastError();
            g_ring = nullptr;
            ::UnmapViewOfFile(ring);
            return ResultNoHandler;
        }
        g_block.Running = 1;
        return ResultOk;
    }

#if !defined(_M_X64)
#pragma comment(linker, "/EXPORT:ManifoldCrashCall=_ManifoldCrashCall@4")
#endif
}

extern "C" __declspec(dllexport) DWORD WINAPI ManifoldCrashCall(LPVOID parameter)
{
    const auto call = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parameter));
    LifeTouch();
    Held held;
    std::uint32_t result = ResultOk;
    switch (call)
    {
    case CallStart:
        result = Start();
        if (result == ResultOk)
            LifeHold(Manifold::Target::Life::PartCrash);
        break;
    case CallStop:
        result = Stop();
        if (result == ResultOk)
            LifeLetGo(Manifold::Target::Life::PartCrash);
        break;
    default:
        result = ResultUnknownCall;
        break;
    }
    g_block.Result = result;
    g_block.Applied = g_block.Serial;
    return result;
}

// The host ended: nobody reads the ring any more, so the handler goes and the
// agent may unload (called from Agent/Life.cpp).
void CrashHostGone()
{
    Held held;
    if (Stop() == ResultOk)
        LifeLetGo(Manifold::Target::Life::PartCrash);
}

// Under the loader lock nothing is waited for: the handler goes, the view
// stays (a thread may still be inside), which the process's end cleans up.
void CrashDetach()
{
    if (g_handler)
    {
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
    g_ring = nullptr;
}

void CrashAttach()
{
    g_block.Version = kVersion;
    g_block.Magic = kMagic;
}
