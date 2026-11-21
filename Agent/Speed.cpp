#include "Target/SpeedhackProtocol.h"

#include <windows.h>

#include <intrin.h>

using namespace Manifold::Target::Speed;

extern "C" __declspec(dllexport) Block ManifoldSpeed = {};

namespace
{
    constexpr std::uintptr_t kSharedData = 0x7FFE0000;
    constexpr std::uintptr_t kTickMultiplier = kSharedData + 0x4;
    constexpr std::uintptr_t kTickCount = kSharedData + 0x320;
    constexpr int kNestSlots = 32;
    constexpr LONG64 kRebaseAfter = 0x40000000;

    enum ClockId
    {
        ClockCounter = 0,
        ClockTick = 1,
        ClockMultimedia = 2,
        ClockCount = 3
    };

    struct Clock
    {
        volatile LONG64 Base;
        volatile LONG64 Ref;
        volatile LONG Primed;
    };

    struct SystemTime
    {
        ULONG Low;
        LONG High1;
        LONG High2;
    };

    using CounterCall = BOOL(WINAPI*)(LARGE_INTEGER*);
    using TickCall = DWORD(WINAPI*)();

    Block& g_block = ManifoldSpeed;
    Clock g_clocks[ClockCount] = {};
    volatile double g_speed = 1.0;
    volatile LONG g_sequence = 0;
    volatile LONG g_applying = 0;
    volatile LONG g_nested = 0;
    volatile LONG g_nest[kNestSlots] = {};
    HANDLE g_wait = nullptr;
    HANDLE g_host = nullptr;

    struct Inside
    {
        long volatile* Slot;

        Inside()
            : Slot(reinterpret_cast<long volatile*>(&g_block.Inside[((::GetCurrentThreadId() >> 2) % kStripes) * kStripeStep]))
        {
            _InterlockedIncrement(Slot);
        }

        ~Inside() { _InterlockedDecrement(Slot); }
        Inside(const Inside&) = delete;
        Inside& operator=(const Inside&) = delete;
    };

    bool Nested()
    {
        if (g_nested == 0)
            return false;
        const LONG id = static_cast<LONG>(::GetCurrentThreadId());
        for (int i = 0; i < kNestSlots; ++i)
        {
            if (g_nest[i] == id)
                return true;
        }
        return false;
    }

    struct Nest
    {
        int Slot = -1;

        Nest()
        {
            const LONG id = static_cast<LONG>(::GetCurrentThreadId());
            _InterlockedIncrement(&g_nested);
            while (Slot < 0)
            {
                for (int i = 0; i < kNestSlots && Slot < 0; ++i)
                {
                    if (_InterlockedCompareExchange(&g_nest[i], id, 0) == 0)
                        Slot = i;
                }
                if (Slot < 0)
                    ::SwitchToThread();
            }
        }

        ~Nest()
        {
            _InterlockedExchange(&g_nest[Slot], 0);
            _InterlockedDecrement(&g_nested);
        }

        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;
    };

    template <class Call>
    Call Original(Source source)
    {
        return reinterpret_cast<Call>(static_cast<std::uintptr_t>(g_block.Originals[source]));
    }

    LONG64 RealTick()
    {
        const volatile SystemTime* time = reinterpret_cast<const volatile SystemTime*>(kTickCount);
        ULONG low = 0;
        LONG high = 0;
        do
        {
            high = time->High1;
            low = time->Low;
        } while (high != time->High2);
        const std::uint64_t multiplier = *reinterpret_cast<const volatile ULONG*>(kTickMultiplier);
        return static_cast<LONG64>(((low * multiplier) >> 24) + ((static_cast<std::uint64_t>(static_cast<ULONG>(high)) * multiplier) << 8));
    }

    LONG64 RealCounter()
    {
        LARGE_INTEGER value{};
        if (const CounterCall original = Original<CounterCall>(SourceCounter))
            original(&value);
        else
            ::QueryPerformanceCounter(&value);
        return value.QuadPart;
    }

    LONG64 RealMultimedia()
    {
        const TickCall original = Original<TickCall>(SourceMultimedia);
        if (!original)
            return 0;
        const Nest nest;
        return original();
    }

    double Clamped(double speed)
    {
        if (!(speed >= 0.0))
            return speed < 0.0 ? 0.0 : 1.0;
        return speed > kMaxSpeed ? kMaxSpeed : speed;
    }

    LONG64 Delta(int clock, LONG64 real, LONG64 ref)
    {
        if (clock == ClockMultimedia)
            return static_cast<LONG>(static_cast<ULONG>(real) - static_cast<ULONG>(ref));
        return real - ref;
    }

    LONG64 Advance(LONG64 delta, double speed)
    {
        if (delta <= 0)
            return 0;
        if (speed == 1.0)
            return delta;
        return static_cast<LONG64>(static_cast<double>(delta) * speed);
    }

    void Apply(bool rebase)
    {
        if (_InterlockedCompareExchange(&g_applying, 1, 0) != 0)
            return;
        const std::uint32_t serial = g_block.Serial;
        const bool changed = serial != g_block.Applied;
        if (changed || rebase)
        {
            const double speed = g_speed;
            const double wanted = changed ? Clamped(g_block.Wanted) : speed;
            const bool live[ClockCount] = { true, true, g_block.Originals[SourceMultimedia] != 0 };
            LONG64 real[ClockCount] = {};
            bool shifted = false;
            _InterlockedIncrement(&g_sequence);
            real[ClockCounter] = RealCounter();
            real[ClockTick] = RealTick();
            if (live[ClockMultimedia])
                real[ClockMultimedia] = RealMultimedia();
            for (int i = 0; i < ClockCount; ++i)
            {
                Clock& clock = g_clocks[i];
                if (!live[i])
                    continue;
                if (!clock.Primed)
                {
                    const LONG64 start = i == ClockMultimedia ? static_cast<LONG64>(static_cast<ULONG>(real[i])) : real[i];
                    clock.Base = start;
                    clock.Ref = start;
                    clock.Primed = 1;
                    continue;
                }
                const LONG64 ref = clock.Ref;
                const LONG64 delta = Delta(i, real[i], ref);
                if (delta > 0)
                {
                    clock.Base = clock.Base + Advance(delta, speed);
                    clock.Ref = ref + delta;
                }
                shifted = shifted || clock.Base != clock.Ref;
            }
            g_speed = wanted;
            _InterlockedIncrement(&g_sequence);
            g_block.Speed = wanted;
            g_block.Shifted = shifted ? 1u : 0u;
            g_block.Applied = serial;
        }
        _InterlockedExchange(&g_applying, 0);
    }

    template <class Read>
    LONG64 Scaled(int clock, Read read)
    {
        const Clock& state = g_clocks[clock];
        for (;;)
        {
            if (g_block.Serial != g_block.Applied)
                Apply(false);
            if (!state.Primed)
            {
                Apply(true);
                if (!state.Primed)
                    _mm_pause();
                continue;
            }
            const LONG sequence = g_sequence;
            if (sequence & 1)
            {
                _mm_pause();
                continue;
            }
            const LONG64 real = read();
            const LONG64 base = state.Base;
            const LONG64 ref = state.Ref;
            const double speed = g_speed;
            if (g_sequence != sequence)
                continue;
            const LONG64 delta = Delta(clock, real, ref);
            if (clock == ClockMultimedia && delta > kRebaseAfter)
            {
                Apply(true);
                continue;
            }
            return base + Advance(delta, speed);
        }
    }

    BOOL WINAPI CounterHook(LARGE_INTEGER* out)
    {
        const Inside inside;
        const CounterCall original = Original<CounterCall>(SourceCounter);
        if (!original)
            return FALSE;
        if (!out || Nested())
            return original(out);
        BOOL ok = TRUE;
        const LONG64 scaled = Scaled(ClockCounter, [&]()
            {
                LARGE_INTEGER real{};
                ok = original(&real);
                return real.QuadPart;
            });
        out->QuadPart = scaled;
        return ok;
    }

    DWORD WINAPI TickHook()
    {
        const Inside inside;
        return static_cast<DWORD>(Nested() ? RealTick() : Scaled(ClockTick, &RealTick));
    }

    ULONGLONG WINAPI Tick64Hook()
    {
        const Inside inside;
        return static_cast<ULONGLONG>(Nested() ? RealTick() : Scaled(ClockTick, &RealTick));
    }

    DWORD WINAPI MultimediaHook()
    {
        const Inside inside;
        return static_cast<DWORD>(Nested() ? RealMultimedia() : Scaled(ClockMultimedia, &RealMultimedia));
    }

    void CALLBACK HostGone(PVOID, BOOLEAN)
    {
        g_block.HostLost = 1;
        g_block.Wanted = 1.0;
        _InterlockedIncrement(reinterpret_cast<long volatile*>(&g_block.Serial));
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

    void Reset()
    {
        for (Clock& clock : g_clocks)
            clock.Primed = 0;
        g_speed = 1.0;
        g_block.Wanted = 1.0;
        g_block.Speed = 1.0;
        g_block.Shifted = 0;
        g_block.HostLost = 0;
        g_block.Applied = g_block.Serial;
    }

#if !defined(_M_X64)
#pragma comment(linker, "/EXPORT:ManifoldSpeedCall=_ManifoldSpeedCall@4")
#endif
}

extern "C" __declspec(dllexport) DWORD WINAPI ManifoldSpeedCall(LPVOID parameter)
{
    switch (static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(parameter)))
    {
    case CallWatch:
        Unwatch();
        g_host = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_block.HostHandle));
        g_block.HostLost = 0;
        if (g_host && ::RegisterWaitForSingleObject(&g_wait, g_host, &HostGone, nullptr, INFINITE, WT_EXECUTEONLYONCE))
            g_block.Watching = 1;
        return g_block.Watching ? 0u : 1u;
    case CallRelease:
        Unwatch();
        if (g_block.HookCount == 0)
            Reset();
        return 0;
    case CallApply:
        Apply(false);
        return 0;
    default:
        return 2;
    }
}

void SpeedAttach(HINSTANCE instance)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(instance);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(instance) + dos->e_lfanew);
    g_block.Entries[SourceCounter] = reinterpret_cast<std::uintptr_t>(&CounterHook);
    g_block.Entries[SourceTick] = reinterpret_cast<std::uintptr_t>(&TickHook);
    g_block.Entries[SourceTick64] = reinterpret_cast<std::uintptr_t>(&Tick64Hook);
    g_block.Entries[SourceMultimedia] = reinterpret_cast<std::uintptr_t>(&MultimediaHook);
    g_block.ImageBase = reinterpret_cast<std::uintptr_t>(instance);
    g_block.ImageSize = nt->OptionalHeader.SizeOfImage;
    g_block.Wanted = 1.0;
    g_block.Speed = 1.0;
    g_block.Version = kVersion;
    g_block.Magic = kMagic;
}
