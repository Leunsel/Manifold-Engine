#include "Target/CoverageProtocol.h"

#include <windows.h>

#include <intrin.h>

using namespace Manifold::Target::Cover;

extern "C" __declspec(dllexport) Block ManifoldCover = {};

std::uint64_t RealTickCount64();

namespace
{
    constexpr std::uintptr_t kPageMask = ~static_cast<std::uintptr_t>(0xFFF);
    constexpr DWORD kSoftwareOriginate = 0x80;
    constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    constexpr int kSystemSlots = 4;

    struct Range
    {
        std::uintptr_t Begin;
        std::uintptr_t End;
    };

    Block& g_block = ManifoldCover;
    PVOID g_handler = nullptr;
    HANDLE g_host = nullptr;
    HANDLE g_wait = nullptr;
    volatile LONG g_inside = 0;
    volatile LONG g_writeLock = 0;
    volatile LONG g_callLock = 0;
    volatile std::uintptr_t g_low = 0;
    volatile std::uintptr_t g_high = 0;
    Range g_system[kSystemSlots] = {};

    struct Inside
    {
        Inside() { _InterlockedIncrement(&g_inside); }
        ~Inside() { _InterlockedDecrement(&g_inside); }
    };

    void Acquire(volatile LONG& lock)
    {
        for (int spin = 0; _InterlockedCompareExchange(&lock, 1, 0) != 0; ++spin)
        {
            if (spin < 64)
                _mm_pause();
            else
                ::SwitchToThread();
        }
    }

    void Let(volatile LONG& lock)
    {
        _InterlockedExchange(&lock, 0);
    }

    struct Held
    {
        volatile LONG& Lock;
        explicit Held(volatile LONG& lock) : Lock(lock) { Acquire(Lock); }
        ~Held() { Let(Lock); }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;
    };

    LONG* StateOf(Entry& entry)
    {
        return reinterpret_cast<LONG*>(const_cast<std::uint32_t*>(&entry.State));
    }

    void SetState(Entry& entry, State state)
    {
        _InterlockedExchange(StateOf(entry), static_cast<LONG>(state));
    }

    bool Exchange(Entry& entry, State from, State to)
    {
        return _InterlockedCompareExchange(StateOf(entry), static_cast<LONG>(to), static_cast<LONG>(from)) == static_cast<LONG>(from);
    }

    Entry* Table()
    {
        return reinterpret_cast<Entry*>(static_cast<std::uintptr_t>(g_block.Table));
    }

    Entry* Find(std::uintptr_t address)
    {
        Entry* table = Table();
        const std::uint32_t count = g_block.Count;
        if (!table || count == 0 || count > kMaxEntries)
            return nullptr;
        std::uint32_t low = 0;
        std::uint32_t high = count;
        while (low < high)
        {
            const std::uint32_t middle = low + (high - low) / 2;
            const std::uintptr_t at = static_cast<std::uintptr_t>(table[middle].Address);
            if (at == address)
                return &table[middle];
            if (at < address)
                low = middle + 1;
            else
                high = middle;
        }
        return nullptr;
    }

    std::uintptr_t ImageSize(HMODULE module)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
        return nt->OptionalHeader.SizeOfImage;
    }

    // The handler itself runs through these: an int3 in them would trap
    // again inside the handler.
    void FindSystem()
    {
        const wchar_t* names[] = { L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll" };
        int slot = 0;
        for (const wchar_t* name : names)
        {
            if (const HMODULE module = ::GetModuleHandleW(name))
            {
                const auto begin = reinterpret_cast<std::uintptr_t>(module);
                g_system[slot++] = { begin, begin + ImageSize(module) };
            }
        }
        const auto self = static_cast<std::uintptr_t>(g_block.ImageBase);
        g_system[slot++] = { self, self + static_cast<std::uintptr_t>(g_block.ImageSize) };
        while (slot < kSystemSlots)
            g_system[slot++] = {};
    }

    bool InSystem(std::uintptr_t address)
    {
        for (const Range& range : g_system)
        {
            if (address >= range.Begin && address < range.End)
                return true;
        }
        return false;
    }

    bool Executable(std::uintptr_t address, MEMORY_BASIC_INFORMATION& info)
    {
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info))
            return false;
        return info.State == MEM_COMMIT && (info.Protect & kExecutable) != 0 && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
    }

    void Flush(std::uintptr_t begin, std::uintptr_t end)
    {
        ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(begin), end - begin);
    }

    // One byte, under the write lock: two threads that changed the protection
    // of one page at the same time would put the wrong one back.
    bool WriteByte(std::uintptr_t address, std::uint8_t value)
    {
        Held held(g_writeLock);
        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &old))
            return false;
        *reinterpret_cast<volatile std::uint8_t*>(address) = value;
        DWORD ignore = 0;
        ::VirtualProtect(reinterpret_cast<LPVOID>(address), 1, old, &ignore);
        Flush(address, address + 1);
        return true;
    }

    void SetIp(CONTEXT& context, std::uintptr_t address)
    {
#if defined(_M_X64)
        context.Rip = address;
#else
        context.Eip = static_cast<DWORD>(address);
#endif
    }

    std::uintptr_t Ip(const CONTEXT& context)
    {
#if defined(_M_X64)
        return static_cast<std::uintptr_t>(context.Rip);
#else
        return static_cast<std::uintptr_t>(context.Eip);
#endif
    }

    std::uint8_t ByteAt(std::uintptr_t address)
    {
        return *reinterpret_cast<const volatile std::uint8_t*>(address);
    }

    // A thread that trapped on an int3 the agent set, but reached the handler
    // only after the byte was put back (the round ended, or the table changed
    // under it), finds no armed entry. The trap came from the processor, the
    // int3 is gone, so the instruction runs again from its start.
    bool Stale(const EXCEPTION_RECORD& record, const CONTEXT& context, std::uintptr_t at)
    {
        if ((record.ExceptionFlags & kSoftwareOriginate) != 0 || at < g_low || at > g_high)
            return false;
        const std::uintptr_t ip = Ip(context);
        if (ip != at && ip != at + 1)
            return false;
        MEMORY_BASIC_INFORMATION info{};
        return Executable(at, info) && ByteAt(at) != kInt3;
    }

    void Widen(std::uintptr_t low, std::uintptr_t high)
    {
        if (g_low == 0 || low < g_low)
            g_low = low;
        if (high > g_high)
            g_high = high;
        g_block.Low = g_low;
        g_block.High = g_high;
    }

    void WaitOutside(DWORD milliseconds)
    {
        const ULONGLONG until = RealTickCount64() + milliseconds;
        while (g_inside != 0 && RealTickCount64() < until)
            ::Sleep(1);
    }

    std::uint32_t ArmPage(Entry* table, std::uint32_t first, std::uint32_t last)
    {
        MEMORY_BASIC_INFORMATION info{};
        const bool code = Executable(static_cast<std::uintptr_t>(table[first].Address), info);
        std::uint32_t wanted = 0;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        for (std::uint32_t i = first; i < last; ++i)
        {
            Entry& entry = table[i];
            const auto address = static_cast<std::uintptr_t>(entry.Address);
            entry.Skip = SkipNone;
            entry.ThreadId = 0;
            if (InSystem(address))
                entry.Skip = SkipSystem;
            else if (!code)
                entry.Skip = info.State == MEM_COMMIT && (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 ? SkipNotCode : SkipUnreadable;
            else if ((entry.Original = ByteAt(address)) == kInt3)
                entry.Skip = SkipAlreadyInt3;
            if (entry.Skip != SkipNone)
            {
                SetState(entry, StateSkipped);
                continue;
            }
            if (!begin)
                begin = address;
            end = address + 1;
            ++wanted;
        }
        if (!wanted)
            return 0;
        Held held(g_writeLock);
        DWORD old = 0;
        if (!::VirtualProtect(reinterpret_cast<LPVOID>(begin), end - begin, PAGE_EXECUTE_READWRITE, &old))
        {
            for (std::uint32_t i = first; i < last; ++i)
            {
                if (table[i].Skip == SkipNone)
                {
                    table[i].Skip = SkipProtect;
                    SetState(table[i], StateSkipped);
                }
            }
            return 0;
        }
        for (std::uint32_t i = first; i < last; ++i)
        {
            Entry& entry = table[i];
            if (entry.Skip != SkipNone)
                continue;
            SetState(entry, StateArmed);
            *reinterpret_cast<volatile std::uint8_t*>(static_cast<std::uintptr_t>(entry.Address)) = kInt3;
        }
        DWORD ignore = 0;
        ::VirtualProtect(reinterpret_cast<LPVOID>(begin), end - begin, old, &ignore);
        Flush(begin, end);
        return wanted;
    }

    std::uint32_t DisarmPage(Entry* table, std::uint32_t first, std::uint32_t last)
    {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        std::uint32_t taken = 0;
        for (std::uint32_t i = first; i < last; ++i)
        {
            if (!Exchange(table[i], StateArmed, StateReleasing))
                continue;
            const auto address = static_cast<std::uintptr_t>(table[i].Address);
            if (!begin)
                begin = address;
            end = address + 1;
            ++taken;
        }
        if (!taken)
            return 0;
        {
            Held held(g_writeLock);
            DWORD old = 0;
            const bool writable = ::VirtualProtect(reinterpret_cast<LPVOID>(begin), end - begin, PAGE_EXECUTE_READWRITE, &old) != FALSE;
            for (std::uint32_t i = first; i < last; ++i)
            {
                Entry& entry = table[i];
                if (entry.State != StateReleasing)
                    continue;
                if (writable)
                    *reinterpret_cast<volatile std::uint8_t*>(static_cast<std::uintptr_t>(entry.Address)) = entry.Original;
            }
            if (writable)
            {
                DWORD ignore = 0;
                ::VirtualProtect(reinterpret_cast<LPVOID>(begin), end - begin, old, &ignore);
                Flush(begin, end);
            }
        }
        for (std::uint32_t i = first; i < last; ++i)
        {
            if (table[i].State == StateReleasing)
                SetState(table[i], StateReleased);
        }
        return taken;
    }

    template <class Visit>
    void ForPages(Entry* table, std::uint32_t count, Visit visit)
    {
        std::uint32_t first = 0;
        while (first < count)
        {
            const std::uintptr_t page = static_cast<std::uintptr_t>(table[first].Address) & kPageMask;
            std::uint32_t last = first + 1;
            while (last < count && (static_cast<std::uintptr_t>(table[last].Address) & kPageMask) == page)
                ++last;
            visit(first, last);
            first = last;
        }
    }

    void WaitRestored(Entry* table, std::uint32_t count)
    {
        const ULONGLONG until = RealTickCount64() + 2000;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            while ((table[i].State == StateRestoring || table[i].State == StateReleasing) && RealTickCount64() < until)
                ::SwitchToThread();
        }
    }

    void DisarmAll()
    {
        Entry* table = Table();
        const std::uint32_t count = g_block.Count;
        if (g_block.Active && table && count <= kMaxEntries)
        {
            ForPages(table, count, [&](std::uint32_t first, std::uint32_t last) { DisarmPage(table, first, last); });
            WaitRestored(table, count);
        }
        g_block.Active = 0;
        g_block.Armed = 0;
        WaitOutside(2000);
    }

    std::uint32_t Arm()
    {
        DisarmAll();
        Entry* table = Table();
        const std::uint32_t count = g_block.Count;
        g_block.Armed = 0;
        g_block.Skipped = 0;
        if (!table || count == 0 || count > kMaxEntries)
            return count == 0 ? ResultOk : ResultBadTable;
        for (std::uint32_t i = 1; i < count; ++i)
        {
            if (table[i].Address <= table[i - 1].Address)
            {
                for (std::uint32_t j = 0; j < count; ++j)
                {
                    table[j].Skip = SkipUnsorted;
                    SetState(table[j], StateSkipped);
                }
                g_block.Skipped = count;
                return ResultBadTable;
            }
        }
        if (!g_handler)
            return ResultNoHandler;
        FindSystem();
        Widen(static_cast<std::uintptr_t>(table[0].Address), static_cast<std::uintptr_t>(table[count - 1].Address));
        g_block.Active = 1;
        std::uint32_t armed = 0;
        ForPages(table, count, [&](std::uint32_t first, std::uint32_t last) { armed += ArmPage(table, first, last); });
        g_block.Armed = armed;
        g_block.Skipped = count - armed;
        return ResultOk;
    }

    void Unwatch()
    {
        if (g_wait)
            ::UnregisterWaitEx(g_wait, INVALID_HANDLE_VALUE);
        g_wait = nullptr;
        if (g_host)
            ::CloseHandle(g_host);
        g_host = nullptr;
        g_block.Watching = 0;
    }

    void CALLBACK HostGone(PVOID, BOOLEAN)
    {
        Held held(g_callLock);
        g_block.HostLost = 1;
        DisarmAll();
    }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info);

    bool AddHandler()
    {
        if (!g_handler)
            g_handler = ::AddVectoredExceptionHandler(1, &Handler);
        g_block.Handler = g_handler ? 1u : 0u;
        return g_handler != nullptr;
    }

    std::uint32_t RemoveHandler()
    {
        if (!g_handler)
            return ResultOk;
        WaitOutside(2000);
        if (g_inside != 0)
            return ResultStillInside;
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
        g_block.Handler = 0;
        WaitOutside(2000);
        return g_inside == 0 ? ResultOk : ResultStillInside;
    }

#if !defined(_M_X64)
#pragma comment(linker, "/EXPORT:ManifoldCoverCall=_ManifoldCoverCall@4")
#endif
}

LONG CoverFilter(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    if (record.ExceptionCode != static_cast<DWORD>(EXCEPTION_BREAKPOINT))
        return EXCEPTION_CONTINUE_SEARCH;
    const Inside inside;
    const auto at = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    CONTEXT& context = *info->ContextRecord;
    if (g_block.Active)
    {
        if (Entry* entry = Find(at))
        {
            for (;;)
            {
                const std::uint32_t state = entry->State;
                if (state == StateArmed)
                {
                    if (!Exchange(*entry, StateArmed, StateRestoring))
                        continue;
                    entry->ThreadId = ::GetCurrentThreadId();
                    WriteByte(at, entry->Original);
                    SetState(*entry, StateHit);
                    _InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_block.Hits));
                    break;
                }
                if (state != StateRestoring && state != StateReleasing)
                    break;
                ::SwitchToThread();
            }
            const std::uint32_t settled = entry->State;
            if ((settled == StateHit || settled == StateReleased) && ByteAt(at) != kInt3)
            {
                SetIp(context, at);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    if (Stale(record, context, at))
    {
        SetIp(context, at);
        _InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_block.Stale));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    _InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_block.Foreign));
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace
{
    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        return CoverFilter(info);
    }
}

// Unwatch waits for a running HostGone, which takes the call lock, so it runs
// before the lock is held.
extern "C" __declspec(dllexport) DWORD WINAPI ManifoldCoverCall(LPVOID parameter)
{
    const auto call = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parameter));
    if (call == CallWatch || call == CallRelease)
        Unwatch();
    Held held(g_callLock);
    std::uint32_t result = ResultOk;
    switch (call)
    {
    case CallWatch:
        g_host = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_block.HostHandle));
        g_block.HostLost = 0;
        if (g_host && ::RegisterWaitForSingleObject(&g_wait, g_host, &HostGone, nullptr, INFINITE, WT_EXECUTEONLYONCE))
            g_block.Watching = 1;
        result = g_block.Watching ? ResultOk : ResultNoHandler;
        break;
    case CallArm:
        result = AddHandler() ? Arm() : ResultNoHandler;
        break;
    case CallDisarm:
        DisarmAll();
        break;
    case CallRelease:
        DisarmAll();
        result = RemoveHandler();
        break;
    default:
        result = ResultUnknownCall;
        break;
    }
    g_block.Result = result;
    g_block.Applied = g_block.Serial;
    return result;
}

// Unwatch would wait for the wait callback, which cannot run while the loader
// lock is held, so a library that goes away without CallRelease only puts the
// bytes back and leaves its wait registered.
void CoverDetach()
{
    if (g_block.Active)
        DisarmAll();
    if (g_handler)
    {
        ::RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
}

void CoverAttach(HINSTANCE instance)
{
    g_block.ImageBase = reinterpret_cast<std::uintptr_t>(instance);
    g_block.ImageSize = ImageSize(instance);
    g_block.Version = kVersion;
    g_block.Magic = kMagic;
}
