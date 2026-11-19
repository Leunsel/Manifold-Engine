#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace Manifold
{
    class CheckedMutex
    {
    public:
        void lock()
        {
            m_mutex.lock();
            if (m_depth++ == 0)
                m_owner.store(std::this_thread::get_id());
        }

        bool try_lock()
        {
            if (!m_mutex.try_lock())
                return false;
            if (m_depth++ == 0)
                m_owner.store(std::this_thread::get_id());
            return true;
        }

        void unlock()
        {
            if (--m_depth == 0)
                m_owner.store(std::thread::id());
            m_mutex.unlock();
        }

        bool HeldHere() const { return m_owner.load() == std::this_thread::get_id(); }

    private:
        std::recursive_mutex m_mutex;
        std::atomic<std::thread::id> m_owner{};
        int m_depth = 0;
    };

    void NoteUnheldLock(const char* file, int line);
    std::uint64_t UnheldLockNotes();
}

#define MANIFOLD_ASSERT_HELD(mutex) \
    do \
    { \
        if (!(mutex).HeldHere()) \
            ::Manifold::NoteUnheldLock(__FILE__, __LINE__); \
    } while (false)
