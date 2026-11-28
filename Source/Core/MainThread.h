#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace Manifold
{
    void BindMainThread();
    bool OnMainThread();
    void NoteOffMainThread(const char* file, int line);
    void SetThreadName(const wchar_t* name);

    class MainThreadQueue
    {
    public:
        void Post(std::function<void()> work);
        void RunAndWait(const std::function<void()>& work);
        std::size_t Drain();
        bool Empty() const;
        bool WaitForWork(std::chrono::milliseconds timeout) const;

    private:
        mutable std::mutex m_lock;
        mutable std::condition_variable m_posted;
        std::deque<std::function<void()>> m_work;
    };
}

#define MANIFOLD_ASSERT_MAIN() \
    do \
    { \
        if (!::Manifold::OnMainThread()) \
            ::Manifold::NoteOffMainThread(__FILE__, __LINE__); \
    } while (false)
