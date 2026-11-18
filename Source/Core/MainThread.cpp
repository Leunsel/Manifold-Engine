#include "Core/MainThread.h"

#include "Core/RuntimeLog.h"

#include <atomic>
#include <future>
#include <set>
#include <string>
#include <thread>

#include <windows.h>

namespace Manifold
{
    namespace
    {
        std::atomic<DWORD> g_mainThread{ 0 };

        std::mutex& ReportedLock()
        {
            static std::mutex lock;
            return lock;
        }

        std::set<std::string>& Reported()
        {
            static std::set<std::string> sites;
            return sites;
        }
    }

    void BindMainThread()
    {
        g_mainThread = ::GetCurrentThreadId();
        SetThreadName(L"Main");
    }

    bool OnMainThread()
    {
        const DWORD main = g_mainThread.load();
        return main == 0 || main == ::GetCurrentThreadId();
    }

    void NoteOffMainThread(const char* file, int line)
    {
        const std::string site = std::string(file) + ":" + std::to_string(line);
        {
            std::lock_guard lock(ReportedLock());
            if (!Reported().insert(site).second)
                return;
        }
        LOG_ERROR("Threads", "Main-thread code ran on thread " + std::to_string(::GetCurrentThreadId()) + " at " + site);
#ifdef _DEBUG
        if (::IsDebuggerPresent())
            ::DebugBreak();
#endif
    }

    void SetThreadName(const wchar_t* name)
    {
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static const SetDescription set = []() -> SetDescription
            {
                const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
                return kernel32 ? reinterpret_cast<SetDescription>(reinterpret_cast<void*>(::GetProcAddress(kernel32, "SetThreadDescription"))) : nullptr;
            }();
        if (set && name)
            set(::GetCurrentThread(), name);
    }

    void MainThreadQueue::Post(std::function<void()> work)
    {
        std::lock_guard lock(m_lock);
        m_work.push_back(std::move(work));
    }

    void MainThreadQueue::RunAndWait(const std::function<void()>& work)
    {
        if (OnMainThread())
        {
            work();
            return;
        }
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        Post([&]()
            {
                work();
                finished.set_value();
            });
        done.wait();
    }

    std::size_t MainThreadQueue::Drain()
    {
        MANIFOLD_ASSERT_MAIN();
        std::size_t ran = 0;
        for (;;)
        {
            std::function<void()> work;
            {
                std::lock_guard lock(m_lock);
                if (m_work.empty())
                    return ran;
                work = std::move(m_work.front());
                m_work.pop_front();
            }
            work();
            ++ran;
        }
    }

    bool MainThreadQueue::Empty() const
    {
        std::lock_guard lock(m_lock);
        return m_work.empty();
    }
}
