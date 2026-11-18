#include "App/Notifications.h"

#include <algorithm>

namespace Manifold
{
    void NotificationCenter::Post(LogLevel level, std::string text, double now)
    {
        std::lock_guard lock(m_lock);
        const auto same = std::find_if(m_notices.begin(), m_notices.end(), [&](const Notice& notice) { return notice.Level == level && notice.Text == text; });
        if (same != m_notices.end())
        {
            Notice repeated = std::move(*same);
            m_notices.erase(same);
            ++repeated.Count;
            repeated.At = now;
            repeated.Id = m_next++;
            m_notices.push_back(std::move(repeated));
            return;
        }
        Notice notice;
        notice.Level = level;
        notice.Text = std::move(text);
        notice.At = now;
        notice.Id = m_next++;
        m_notices.push_back(std::move(notice));
        Trim();
    }

    void NotificationCenter::Trim()
    {
        while (m_notices.size() > kMaxNotices)
        {
            const auto oldest = std::find_if(m_notices.begin(), m_notices.end(), [](const Notice& notice) { return !notice.Sticky; });
            if (oldest == m_notices.end())
                break;
            m_notices.erase(oldest);
        }
    }

    int NotificationCenter::Show(LogLevel level, std::string text, double now, double progress, bool sticky)
    {
        std::lock_guard lock(m_lock);
        Notice notice;
        notice.Level = level;
        notice.Text = std::move(text);
        notice.At = now;
        notice.Id = m_next++;
        notice.Progress = progress;
        notice.Sticky = sticky;
        const int id = notice.Id;
        m_notices.push_back(std::move(notice));
        Trim();
        return id;
    }

    bool NotificationCenter::Update(int id, LogLevel level, std::string text, double now, double progress, bool sticky)
    {
        std::lock_guard lock(m_lock);
        for (Notice& notice : m_notices)
        {
            if (notice.Id != id)
                continue;
            notice.Level = level;
            notice.Text = std::move(text);
            notice.At = now;
            notice.Progress = progress;
            notice.Sticky = sticky;
            return true;
        }
        return false;
    }

    bool NotificationCenter::Remove(int id)
    {
        std::lock_guard lock(m_lock);
        const auto found = std::find_if(m_notices.begin(), m_notices.end(), [&](const Notice& notice) { return notice.Id == id; });
        if (found == m_notices.end())
            return false;
        m_notices.erase(found);
        return true;
    }

    bool NotificationCenter::Has(int id) const
    {
        std::lock_guard lock(m_lock);
        return std::any_of(m_notices.begin(), m_notices.end(), [&](const Notice& notice) { return notice.Id == id; });
    }

    std::vector<Notice> NotificationCenter::Visible(double now, double lifetime)
    {
        std::lock_guard lock(m_lock);
        m_notices.erase(std::remove_if(m_notices.begin(), m_notices.end(), [&](const Notice& notice) { return !notice.Sticky && now - notice.At > lifetime; }), m_notices.end());
        return { m_notices.begin(), m_notices.end() };
    }

    void NotificationCenter::Expire(int id, double at)
    {
        std::lock_guard lock(m_lock);
        for (Notice& notice : m_notices)
        {
            if (notice.Id == id)
            {
                notice.At = at;
                notice.Sticky = false;
            }
        }
    }

    bool NotificationCenter::Empty() const
    {
        std::lock_guard lock(m_lock);
        return m_notices.empty();
    }
}
