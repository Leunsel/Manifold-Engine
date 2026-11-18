#pragma once

#include <cstdint>
#include <utility>

typedef void* HANDLE;

namespace Manifold
{
    class UniqueHandle
    {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) noexcept : m_handle(Usable(handle) ? handle : nullptr) {}
        ~UniqueHandle() { Reset(); }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;
        UniqueHandle(UniqueHandle&& other) noexcept : m_handle(other.Release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
                Reset(other.Release());
            return *this;
        }

        HANDLE Get() const noexcept { return m_handle; }
        explicit operator bool() const noexcept { return m_handle != nullptr; }
        HANDLE Release() noexcept { return std::exchange(m_handle, nullptr); }
        void Reset(HANDLE handle = nullptr) noexcept;

        static bool Usable(HANDLE handle) noexcept { return handle && handle != reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1)); }

    private:
        HANDLE m_handle = nullptr;
    };
}
