#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Manifold
{
    class WorkProgress
    {
    public:
        void SetText(std::string text);
        std::string Text() const;
        void SetSteps(std::int64_t done, std::int64_t total);
        void Step(std::int64_t count = 1) { m_done += count; }
        std::int64_t Done() const { return m_done.load(); }
        std::int64_t Total() const { return m_total.load(); }
        double Fraction() const;
        void Cancel() { m_cancel = true; }
        bool Cancelled() const { return m_cancel.load(); }

    private:
        mutable std::mutex m_lock;
        std::string m_text;
        std::atomic<std::int64_t> m_done{ 0 };
        std::atomic<std::int64_t> m_total{ 0 };
        std::atomic<bool> m_cancel{ false };
    };

    struct WorkOptions
    {
        std::string Title;
        std::string Group;
        bool Cancellable = true;
        bool Quiet = false;
        bool Serial = false;
    };

    struct WorkInfo
    {
        int Id = 0;
        std::string Title;
        std::string Group;
        std::string Text;
        double Fraction = -1.0;
        std::int64_t Done = 0;
        std::int64_t Total = 0;
        double Seconds = 0.0;
        bool Cancellable = false;
        bool Cancelling = false;
    };

    class BackgroundWork
    {
    public:
        using Finish = std::function<void()>;
        using Job = std::function<Finish(WorkProgress&)>;

        BackgroundWork() = default;
        ~BackgroundWork();

        BackgroundWork(const BackgroundWork&) = delete;
        BackgroundWork& operator=(const BackgroundWork&) = delete;

        int Start(WorkOptions options, Job job);
        void Cancel(int id);
        void CancelGroup(std::string_view group);
        void Complete(int id);
        void CompleteGroup(std::string_view group);
        bool Running(int id) const;
        bool Busy() const;
        bool Busy(std::string_view group) const;
        int Find(std::string_view group) const;
        bool Visible() const;
        std::vector<WorkInfo> List() const;
        std::optional<WorkInfo> Info(std::string_view group) const;
        std::shared_ptr<WorkProgress> ProgressOf(int id) const;
        void Drain();
        void Stop();

    private:
        struct Entry
        {
            int Id = 0;
            WorkOptions Options;
            std::shared_ptr<WorkProgress> Progress = std::make_shared<WorkProgress>();
            std::chrono::steady_clock::time_point Started = std::chrono::steady_clock::now();
            std::thread Thread;
            Job Waiting;
            bool Launched = false;
            bool Done = false;
            Finish Finished;
        };

        void Launch(Entry* entry, Job job);
        void LaunchNextSerial(const std::string& group);
        void Settle(std::unique_ptr<Entry> entry, bool run);
        std::unique_ptr<Entry> Take(Entry* entry);

        mutable std::mutex m_lock;
        std::vector<std::unique_ptr<Entry>> m_entries;
        int m_next = 1;
        bool m_stopped = false;
    };
}
