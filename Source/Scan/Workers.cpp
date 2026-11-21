#include "Scan/Workers.h"

#include <algorithm>

#include <windows.h>

namespace Manifold::Scan
{
    int WorkerCount(int wanted)
    {
        if (wanted > 0)
            return std::clamp(wanted, 1, 64);
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        return static_cast<int>(std::clamp(cores / 2u, 2u, 8u));
    }

    void PreferFastCores()
    {
        THREAD_POWER_THROTTLING_STATE state{};
        state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
        ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
    }
}
