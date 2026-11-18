#pragma once

#include "Core/RuntimeLog.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Manifold
{
    struct Notice
    {
        LogLevel Level = LogLevel::Info;
        std::string Text;
        double At = 0.0;
        int Id = 0;
        int Count = 1;
        double Progress = -1.0;
        bool Sticky = false;
    };

    class NotificationCenter
    {
    public:
        void Post(LogLevel level, std::string text, double now);
        int Show(LogLevel level, std::string text, double now, double progress, bool sticky);
        bool Update(int id, LogLevel level, std::string text, double now, double progress, bool sticky);
        bool Remove(int id);
        bool Has(int id) const;
        std::vector<Notice> Visible(double now, double lifetime);
        void Expire(int id, double at);
        bool Empty() const;

    private:
        static constexpr std::size_t kMaxNotices = 6;

        void Trim();

        mutable std::mutex m_lock;
        std::deque<Notice> m_notices;
        int m_next = 1;
    };
}
