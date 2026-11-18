#pragma once

#include "App/Sounds.h"
#include "Core/BackgroundWork.h"
#include "Core/RuntimeLog.h"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Manifold::UI
{
    template <class Result, class Progress>
    class BackgroundTask
    {
    public:
        BackgroundTask() = default;
        ~BackgroundTask() { Cancel(); }

        BackgroundTask(const BackgroundTask&) = delete;
        BackgroundTask& operator=(const BackgroundTask&) = delete;

        template <class Work>
        bool Start(BackgroundWork& work, std::string title, Work&& job)
        {
            if (Running())
                return false;
            auto shared = std::make_shared<Shared>();
            WorkOptions options;
            options.Title = title;
            options.Group = "task";
            options.Quiet = true;
            const int id = work.Start(std::move(options), [shared, title, job = std::forward<Work>(job)](WorkProgress&) mutable -> BackgroundWork::Finish
                {
                    try
                    {
                        Result result = job(shared->State);
                        return [shared, result = std::move(result)]() mutable { shared->Value.emplace(std::move(result)); };
                    }
                    catch (const std::exception& error)
                    {
                        LOG_ERROR("Work", title + " failed: " + error.what());
                    }
                    return [shared]() { shared->Failed = true; };
                });
            if (!id)
                return false;
            m_shared = std::move(shared);
            m_running = true;
            m_started = std::chrono::steady_clock::now();
            return true;
        }

        bool Running() const { return m_running; }
        Progress& State() { return m_shared->State; }
        const Progress& State() const { return m_shared->State; }

        void Cancel()
        {
            if (m_shared && m_running)
                m_shared->State.Cancel = true;
        }

        std::optional<Result> Collect()
        {
            if (!m_running || (!m_shared->Value && !m_shared->Failed))
                return std::nullopt;
            m_running = false;
            m_seconds = Elapsed();
            if (!m_shared->Value)
                return std::nullopt;
            if (m_seconds >= kLongSeconds && !m_shared->State.Cancel)
                Sounds::Play(Sounds::Cue::Success);
            std::optional<Result> result = std::move(m_shared->Value);
            m_shared->Value.reset();
            return result;
        }

        double Elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(); }
        double Seconds() const { return m_seconds; }

    private:
        static constexpr double kLongSeconds = 2.0;

        struct Shared
        {
            Progress State;
            std::optional<Result> Value;
            bool Failed = false;
        };

        std::shared_ptr<Shared> m_shared;
        bool m_running = false;
        std::chrono::steady_clock::time_point m_started{};
        double m_seconds = 0.0;
    };
}
