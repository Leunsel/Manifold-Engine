#pragma once

#include "Core/BackgroundWork.h"

#include <functional>

namespace Manifold
{
    class JobQueue
    {
    public:
        using Finish = std::function<void()>;
        using Job = std::function<Finish()>;

        explicit JobQueue(BackgroundWork& work) : m_work(work) {}

        JobQueue(const JobQueue&) = delete;
        JobQueue& operator=(const JobQueue&) = delete;

        void Post(Job job);
        void Stop();
        bool Busy() const;

    private:
        BackgroundWork& m_work;
    };
}
