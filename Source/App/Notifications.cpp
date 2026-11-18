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
        while (m_notices.size() > kMaxNotices)
            m_notices.pop_front();
    }

    std::vector<Notice> NotificationCenter::Visible(double now, double lifetime)
    {
        std::lock_guard lock(m_lock);
        m_notices.erase(std::remove_if(m_notices.begin(), m_notices.end(), [&](const Notice& notice) { return now - notice.At > lifetime; }), m_notices.end());
        return { m_notices.begin(), m_notices.end() };
    }

    void NotificationCenter::Expire(int id, double at)
    {
        std::lock_guard lock(m_lock);
        for (Notice& notice : m_notices)
        {
            if (notice.Id == id)
                notice.At = at;
        }
    }

    bool NotificationCenter::Empty() const
    {
        std::lock_guard lock(m_lock);
        return m_notices.empty();
    }
}
