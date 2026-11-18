#include "Core/JobQueue.h"

namespace Manifold
{
    namespace
    {
        constexpr const char* kJobGroup = "jobs";
    }

    void JobQueue::Post(Job job)
    {
        WorkOptions options;
        options.Title = "Background job";
        options.Group = kJobGroup;
        options.Cancellable = false;
        options.Quiet = true;
        options.Serial = true;
        m_work.Start(std::move(options), [job = std::move(job)](WorkProgress&) -> BackgroundWork::Finish { return job(); });
    }

    void JobQueue::Stop()
    {
        m_work.CancelGroup(kJobGroup);
    }

    bool JobQueue::Busy() const
    {
        return m_work.Busy(kJobGroup);
    }
}
