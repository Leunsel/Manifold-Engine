#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <utility>

namespace Manifold::UI
{
    template <class Result, class Progress>
    class BackgroundTask
    {
    public:
        BackgroundTask() = default;
        ~BackgroundTask()
        {
            Cancel();
            if (m_future.valid())
                m_future.wait();
        }

        BackgroundTask(const BackgroundTask&) = delete;
        BackgroundTask& operator=(const BackgroundTask&) = delete;

        template <class Work>
        bool Start(Work&& work)
        {
            if (Running())
                return false;
            m_progress = std::make_unique<Progress>();
            m_started = std::chrono::steady_clock::now();
            Progress* progress = m_progress.get();
            m_future = std::async(std::launch::async, [progress, work = std::forward<Work>(work)]() mutable { return work(*progress); });
            return true;
        }

        bool Running() const { return m_future.valid(); }
        Progress& State() { return *m_progress; }
        const Progress& State() const { return *m_progress; }

        void Cancel()
        {
            if (m_progress && Running())
                m_progress->Cancel = true;
        }

        std::optional<Result> Collect()
        {
            if (!m_future.valid() || m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return std::nullopt;
            m_seconds = Elapsed();
            return m_future.get();
        }

        double Elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(); }
        double Seconds() const { return m_seconds; }

    private:
        std::unique_ptr<Progress> m_progress;
        std::future<Result> m_future;
        std::chrono::steady_clock::time_point m_started{};
        double m_seconds = 0.0;
    };
}
