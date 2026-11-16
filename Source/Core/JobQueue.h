#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Manifold
{
    class JobQueue
    {
    public:
        using Finish = std::function<void()>;
        using Job = std::function<Finish()>;

        JobQueue();
        ~JobQueue();

        JobQueue(const JobQueue&) = delete;
        JobQueue& operator=(const JobQueue&) = delete;

        void Post(Job job);
        void Drain();
        void Stop();
        bool Busy() const { return m_pending.load() > 0; }

    private:
        void Work();

        std::mutex m_lock;
        std::condition_variable m_wake;
        std::deque<Job> m_jobs;
        std::deque<Finish> m_finished;
        std::atomic<int> m_pending{ 0 };
        bool m_stop = false;
        std::thread m_thread;
    };
}
