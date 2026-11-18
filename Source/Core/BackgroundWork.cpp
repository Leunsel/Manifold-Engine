#include "Core/BackgroundWork.h"

#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include <algorithm>
#include <exception>

namespace Manifold
{
    void WorkProgress::SetText(std::string text)
    {
        std::lock_guard lock(m_lock);
        m_text = std::move(text);
    }

    std::string WorkProgress::Text() const
    {
        std::lock_guard lock(m_lock);
        return m_text;
    }

    void WorkProgress::SetSteps(std::int64_t done, std::int64_t total)
    {
        m_total = total;
        m_done = done;
    }

    double WorkProgress::Fraction() const
    {
        const std::int64_t total = m_total.load();
        if (total <= 0)
            return -1.0;
        return std::clamp(static_cast<double>(m_done.load()) / static_cast<double>(total), 0.0, 1.0);
    }

    BackgroundWork::~BackgroundWork()
    {
        Stop();
    }

    int BackgroundWork::Start(WorkOptions options, Job job)
    {
        std::lock_guard lock(m_lock);
        if (m_stopped)
            return 0;
        auto entry = std::make_unique<Entry>();
        entry->Id = m_next++;
        entry->Options = std::move(options);
        Entry* raw = entry.get();
        const int id = raw->Id;
        const bool queued = raw->Options.Serial && std::any_of(m_entries.begin(), m_entries.end(), [raw](const auto& other)
            {
                return other->Options.Serial && other->Options.Group == raw->Options.Group && !other->Done;
            });
        m_entries.push_back(std::move(entry));
        if (queued)
            raw->Waiting = std::move(job);
        else
            Launch(raw, std::move(job));
        return id;
    }

    void BackgroundWork::Launch(Entry* entry, Job job)
    {
        entry->Launched = true;
        entry->Started = std::chrono::steady_clock::now();
        entry->Thread = std::thread([this, entry, job = std::move(job), name = Utf8ToWide("Work: " + entry->Options.Title)]()
            {
                SetThreadName(name.c_str());
                Finish finish;
                try
                {
                    finish = job(*entry->Progress);
                }
                catch (const std::exception& error)
                {
                    LOG_ERROR("Work", entry->Options.Title + " failed: " + error.what());
                }
                std::lock_guard done(m_lock);
                entry->Finished = std::move(finish);
                entry->Done = true;
                if (entry->Options.Serial && !m_stopped)
                    LaunchNextSerial(entry->Options.Group);
            });
    }

    void BackgroundWork::LaunchNextSerial(const std::string& group)
    {
        for (const auto& entry : m_entries)
        {
            if (entry->Options.Serial && entry->Options.Group == group && !entry->Launched)
            {
                Launch(entry.get(), std::move(entry->Waiting));
                return;
            }
        }
    }

    void BackgroundWork::Cancel(int id)
    {
        std::lock_guard lock(m_lock);
        for (const auto& entry : m_entries)
        {
            if (entry->Id == id)
                entry->Progress->Cancel();
        }
    }

    void BackgroundWork::CancelGroup(std::string_view group)
    {
        std::lock_guard lock(m_lock);
        for (const auto& entry : m_entries)
        {
            if (entry->Options.Group == group)
                entry->Progress->Cancel();
        }
    }

    std::unique_ptr<BackgroundWork::Entry> BackgroundWork::Take(Entry* entry)
    {
        std::lock_guard lock(m_lock);
        const auto found = std::find_if(m_entries.begin(), m_entries.end(), [entry](const auto& item) { return item.get() == entry; });
        if (found == m_entries.end())
            return nullptr;
        std::unique_ptr<Entry> taken = std::move(*found);
        m_entries.erase(found);
        return taken;
    }

    void BackgroundWork::Settle(std::unique_ptr<Entry> entry, bool run)
    {
        if (!entry)
            return;
        if (entry->Thread.joinable())
            entry->Thread.join();
        Finish finish = std::move(entry->Finished);
        entry.reset();
        if (run && finish)
            finish();
    }

    void BackgroundWork::Complete(int id)
    {
        Entry* target = nullptr;
        {
            std::lock_guard lock(m_lock);
            for (const auto& entry : m_entries)
            {
                if (entry->Id == id)
                    target = entry.get();
            }
        }
        if (!target)
            return;
        if (target->Thread.joinable())
            target->Thread.join();
        Settle(Take(target), true);
    }

    void BackgroundWork::CompleteGroup(std::string_view group)
    {
        for (;;)
        {
            const int id = Find(group);
            if (!id)
                return;
            Complete(id);
        }
    }

    bool BackgroundWork::Running(int id) const
    {
        std::lock_guard lock(m_lock);
        return std::any_of(m_entries.begin(), m_entries.end(), [id](const auto& entry) { return entry->Id == id; });
    }

    bool BackgroundWork::Busy() const
    {
        std::lock_guard lock(m_lock);
        return !m_entries.empty();
    }

    bool BackgroundWork::Busy(std::string_view group) const
    {
        return Find(group) != 0;
    }

    int BackgroundWork::Find(std::string_view group) const
    {
        std::lock_guard lock(m_lock);
        for (const auto& entry : m_entries)
        {
            if (entry->Options.Group == group)
                return entry->Id;
        }
        return 0;
    }

    bool BackgroundWork::Visible() const
    {
        std::lock_guard lock(m_lock);
        return std::any_of(m_entries.begin(), m_entries.end(), [](const auto& entry) { return !entry->Options.Quiet; });
    }

    std::vector<WorkInfo> BackgroundWork::List() const
    {
        std::vector<WorkInfo> list;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_lock);
        for (const auto& entry : m_entries)
        {
            if (entry->Options.Quiet)
                continue;
            WorkInfo info;
            info.Id = entry->Id;
            info.Title = entry->Options.Title;
            info.Group = entry->Options.Group;
            info.Text = entry->Progress->Text();
            info.Fraction = entry->Progress->Fraction();
            info.Done = entry->Progress->Done();
            info.Total = entry->Progress->Total();
            info.Seconds = std::chrono::duration<double>(now - entry->Started).count();
            info.Cancellable = entry->Options.Cancellable;
            info.Cancelling = entry->Progress->Cancelled();
            list.push_back(std::move(info));
        }
        return list;
    }

    std::optional<WorkInfo> BackgroundWork::Info(std::string_view group) const
    {
        for (WorkInfo& info : List())
        {
            if (info.Group == group)
                return std::move(info);
        }
        return std::nullopt;
    }

    std::shared_ptr<WorkProgress> BackgroundWork::ProgressOf(int id) const
    {
        std::lock_guard lock(m_lock);
        for (const auto& entry : m_entries)
        {
            if (entry->Id == id)
                return entry->Progress;
        }
        return nullptr;
    }

    void BackgroundWork::Drain()
    {
        std::vector<Entry*> finished;
        {
            std::lock_guard lock(m_lock);
            for (const auto& entry : m_entries)
            {
                if (entry->Done)
                    finished.push_back(entry.get());
            }
        }
        for (Entry* entry : finished)
            Settle(Take(entry), true);
    }

    void BackgroundWork::Stop()
    {
        std::vector<Entry*> all;
        {
            std::lock_guard lock(m_lock);
            m_stopped = true;
            for (const auto& entry : m_entries)
            {
                entry->Progress->Cancel();
                all.push_back(entry.get());
            }
        }
        for (Entry* entry : all)
            Settle(Take(entry), false);
    }
}
