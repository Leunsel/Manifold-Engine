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
    };

    class NotificationCenter
    {
    public:
        void Post(LogLevel level, std::string text, double now);
        std::vector<Notice> Visible(double now, double lifetime);
        void Expire(int id, double at);
        bool Empty() const;

    private:
        static constexpr std::size_t kMaxNotices = 6;

        mutable std::mutex m_lock;
        std::deque<Notice> m_notices;
        int m_next = 1;
    };
}
