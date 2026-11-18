#include "Core/Handle.h"

#include <windows.h>

namespace Manifold
{
    void UniqueHandle::Reset(HANDLE handle) noexcept
    {
        const HANDLE previous = std::exchange(m_handle, Usable(handle) ? handle : nullptr);
        if (previous && previous != m_handle)
            ::CloseHandle(previous);
    }
}
