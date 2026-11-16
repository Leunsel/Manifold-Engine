#include "Core/JobQueue.h"

#include "Core/RuntimeLog.h"

#include <exception>
#include <string>

namespace Manifold
{
    JobQueue::JobQueue() : m_thread(&JobQueue::Work, this)
    {
    }

    JobQueue::~JobQueue()
    {
        Stop();
    }

    void JobQueue::Post(Job job)
    {
        {
            std::lock_guard lock(m_lock);
            if (m_stop)
                return;
            m_jobs.push_back(std::move(job));
            ++m_pending;
        }
        m_wake.notify_one();
    }

    void JobQueue::Drain()
    {
        std::deque<Finish> ready;
        {
            std::lock_guard lock(m_lock);
            ready.swap(m_finished);
        }
        for (Finish& finish : ready)
            finish();
    }

    void JobQueue::Stop()
    {
        {
            std::lock_guard lock(m_lock);
            m_stop = true;
            m_jobs.clear();
            m_finished.clear();
        }
        m_wake.notify_all();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
            m_thread.join();
    }

    void JobQueue::Work()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(m_lock);
                m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
                if (m_stop)
                    return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            Finish finish;
            try
            {
                finish = job();
            }
            catch (const std::exception& error)
            {
                LOG_ERROR("Jobs", std::string("A background job failed: ") + error.what());
            }
            {
                std::lock_guard lock(m_lock);
                if (finish && !m_stop)
                    m_finished.push_back(std::move(finish));
            }
            --m_pending;
        }
    }
}
