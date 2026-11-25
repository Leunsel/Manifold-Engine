#include "Target/AgentLifeProtocol.h"

#include <windows.h>

#include <intrin.h>

using namespace Manifold::Target::Life;

extern "C" __declspec(dllexport) Block ManifoldLife = {};

std::uint64_t RealTickCount64();
void CrashHostGone();

namespace
{
    constexpr DWORD kLoopMs = 250;

    Block& g_life = ManifoldLife;
    HMODULE g_self = nullptr;
    HANDLE g_host = nullptr;
    std::uint64_t g_hostValue = 0;
    volatile LONG g_lock = 0;

    struct Held
    {
        Held()
        {
            while (_InterlockedCompareExchange(&g_lock, 1, 0) != 0)
                ::SwitchToThread();
        }
        ~Held() { _InterlockedExchange(&g_lock, 0); }
    };

    // The host process ended: what still has to go before the agent can.
    void HostEnded()
    {
        g_life.HostGone = 1;
        CrashHostGone();
    }

    void WatchHost()
    {
        const std::uint64_t value = g_life.HostHandle;
        if (value != g_hostValue)
        {
            if (g_host)
                ::CloseHandle(g_host);
            g_host = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
            g_hostValue = value;
            g_life.HostGone = 0;
        }
        if (g_host && !g_life.HostGone && ::WaitForSingleObject(g_host, 0) == WAIT_OBJECT_0)
            HostEnded();
    }

    // ∑ Every load the host made is given back: all but the last with
    //   FreeLibrary (the library stays), the last with FreeLibraryAndExitThread
    //   from this thread, which is the only way a library unloads itself. A
    //   load the host made but had not counted yet keeps the library in place,
    //   and the counts still agree once the host writes it.
    DWORD WINAPI LifeLoop(LPVOID)
    {
        for (;;)
        {
            ::Sleep(kLoopMs);
            WatchHost();
            const std::uint32_t grace = g_life.GraceMs ? g_life.GraceMs : kDefaultGraceMs;
            {
                Held held;
                // ∑ Activity is read before the clock: the host writes its own tick
                //   there, and a clock read first could be older than that tick, so
                //   the difference would wrap and look like a long idle time. A
                //   tick that is not behind the clock counts as activity now.
                // one 64-bit read also in the 32-bit agent: two halves read across the 2^32 ms carry
                // (49.7 days of uptime) could look 49 days old and unload the agent under a new load
                const std::uint64_t activity = static_cast<std::uint64_t>(_InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&g_life.Activity), 0, 0));
                const std::uint64_t now = RealTickCount64();
                if (g_life.Parts != 0 || activity >= now || now - activity < grace)
                    continue;
                const std::uint32_t loads = g_life.HostLoads;
                const std::uint32_t dropped = g_life.Dropped;
                if (loads <= dropped)
                    continue;  // nothing the host loaded is left to give back
                const std::uint32_t outstanding = loads - dropped;
                for (std::uint32_t i = 1; i < outstanding; ++i)
                {
                    ::FreeLibrary(g_self);
                    g_life.Dropped = g_life.Dropped + 1;
                }
                g_life.Dropped = g_life.Dropped + 1;
                g_life.Leaving = 1;
            }
            if (g_host)
                ::CloseHandle(g_host);
            g_host = nullptr;
            ::FreeLibraryAndExitThread(g_self, 0);
        }
    }
}

void LifeTouch()
{
    ::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_life.Activity), static_cast<LONG64>(RealTickCount64()));
}

void LifeHold(std::uint32_t part)
{
    Held held;
    g_life.Parts = g_life.Parts | part;
    g_life.Leaving = 0;
    LifeTouch();
}

void LifeLetGo(std::uint32_t part)
{
    Held held;
    g_life.Parts = g_life.Parts & ~part;
    LifeTouch();
}

void LifeAttach(HINSTANCE instance)
{
    g_self = instance;
    g_life.GraceMs = kDefaultGraceMs;
    g_life.Activity = RealTickCount64();
    g_life.Version = kVersion;
    g_life.Magic = kMagic;
    // runs once DllMain returned and the loader lock is free
    if (HANDLE thread = ::CreateThread(nullptr, 0, &LifeLoop, nullptr, 0, nullptr))
        ::CloseHandle(thread);
}
