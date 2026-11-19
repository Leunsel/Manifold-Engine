#include "Core/CheckedMutex.h"

#include "Core/RuntimeLog.h"

#include <set>
#include <string>

#include <windows.h>

namespace Manifold
{
    namespace
    {
        std::mutex& ReportedLock()
        {
            static std::mutex lock;
            return lock;
        }

        std::set<std::string>& Reported()
        {
            static std::set<std::string> sites;
            return sites;
        }

        std::atomic<std::uint64_t> g_notes{ 0 };
    }

    void NoteUnheldLock(const char* file, int line)
    {
        ++g_notes;
        const std::string site = std::string(file) + ":" + std::to_string(line);
        {
            std::lock_guard lock(ReportedLock());
            if (!Reported().insert(site).second)
                return;
        }
        LOG_ERROR("Threads", "Code that needs its lock ran without it on thread " + std::to_string(::GetCurrentThreadId()) + " at " + site);
#ifdef _DEBUG
        if (::IsDebuggerPresent())
            ::DebugBreak();
#endif
    }

    std::uint64_t UnheldLockNotes()
    {
        return g_notes.load();
    }
}
