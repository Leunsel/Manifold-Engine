#include "Core/RuntimeLog.h"

#include "Core/MainThread.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <share.h>
#include <system_error>
#include <utility>

#include <windows.h>

namespace Manifold::RuntimeLog
{
    namespace
    {
        std::mutex g_lock;
        std::deque<LogEntry> g_entries;
        std::uint64_t g_generation = 0;
        std::FILE* g_file = nullptr;
        std::filesystem::path g_path;
        bool g_verbose = false;
        std::deque<std::uint64_t> g_errorGenerations;
        std::uint64_t g_serial = 0;
        const std::chrono::steady_clock::time_point g_started = std::chrono::steady_clock::now();

        std::string TimeText(int& milliseconds)
        {
            SYSTEMTIME local{};
            ::GetLocalTime(&local);
            milliseconds = local.wMilliseconds;
            char text[16] = {};
            std::snprintf(text, sizeof(text), "%02u:%02u:%02u", local.wHour, local.wMinute, local.wSecond);
            return text;
        }

        std::string OneLine(std::string_view message)
        {
            std::string line(message);
            for (char& c : line)
            {
                if (static_cast<unsigned char>(c) < 0x20)
                    c = ' ';
            }
            return line;
        }
    }

    const char* LevelName(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Success: return "OK";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "INFO";
        }
    }

    bool EntryMatches(const LogEntry& entry, LogLevel lowest, std::string_view text)
    {
        if (static_cast<int>(entry.Level) < static_cast<int>(lowest))
            return false;
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        const auto contains = [&](std::string_view where)
            {
                return std::search(where.begin(), where.end(), text.begin(), text.end(), [&](char a, char b) { return lower(a) == lower(b); }) != where.end();
            };
        return text.empty() || contains(entry.Text()) || contains(entry.Trace) || contains(entry.Area);
    }

    std::string TraceText(const char* file, const char* function, int line)
    {
        const std::string_view path = file ? file : "unknown";
        const std::string_view name = function ? function : "unknown";
        const auto slash = path.find_last_of("\\/");
        const auto scope = name.rfind("::");
        std::string trace(slash == std::string_view::npos ? path : path.substr(slash + 1));
        trace += ':';
        trace += scope == std::string_view::npos ? name : name.substr(scope + 2);
        trace += ':';
        trace += std::to_string(line);
        return trace;
    }

    void OpenFile(const std::filesystem::path& path)
    {
        std::lock_guard lock(g_lock);
        if (g_file)
        {
            std::fclose(g_file);
            g_file = nullptr;
        }
        g_path = path;
        if (path.empty())
            return;

        std::error_code ignored;
        if (path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), ignored);
        std::filesystem::rename(path, std::filesystem::path(path).replace_extension(L".previous.log"), ignored);
        g_file = ::_wfsopen(path.c_str(), L"a", _SH_DENYNO);
    }

    std::filesystem::path FilePath()
    {
        std::lock_guard lock(g_lock);
        return g_path;
    }

    void SetVerbose(bool verbose)
    {
        std::lock_guard lock(g_lock);
        g_verbose = verbose;
    }

    bool Verbose()
    {
        std::lock_guard lock(g_lock);
        return g_verbose;
    }

    void Write(LogLevel level, std::string_view area, const char* file, const char* function, int line, std::string_view message)
    {
        WriteDetailed(level, area, file, function, line, message, {});
    }

    void WriteDetailed(LogLevel level, std::string_view area, const char* file, const char* function, int line, std::string_view message, std::string_view details)
    {
        LogEntry entry;
        entry.Time = TimeText(entry.Milliseconds);
        entry.Clock = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count();
        entry.Thread = ::GetCurrentThreadId();
        entry.MainThread = OnMainThread();
        entry.Level = level;
        entry.Area = std::string(area);
        entry.Trace = TraceText(file, function, line);
        entry.Message = OneLine(message);
        entry.Detailed = !details.empty();
        if (entry.Detailed)
            entry.Full = std::string(details);
        else if (entry.Message != message)
            entry.Full = std::string(message);

        std::lock_guard lock(g_lock);
        if (level == LogLevel::Debug && !g_verbose)
            return;
        entry.Serial = ++g_serial;
        if (g_file)
        {
            std::fprintf(g_file, "[%s.%03d] [%-5s] [%s] [%s] %s\n", entry.Time.c_str(), entry.Milliseconds, LevelName(level), entry.Area.c_str(), entry.Trace.c_str(), entry.Message.c_str());
            // ∑ the details go into the file too, indented under their entry, so a file read later
            //   holds what the Log window's details pane shows
            if (!details.empty())
            {
                std::size_t at = 0;
                while (at <= details.size())
                {
                    const std::size_t end = std::min(details.find('\n', at), details.size());
                    std::string_view part = details.substr(at, end - at);
                    if (!part.empty() && part.back() == '\r')
                        part.remove_suffix(1);
                    std::fprintf(g_file, "    %.*s\n", static_cast<int>(part.size()), part.data());
                    at = end + 1;
                }
            }
            std::fflush(g_file);
        }
        g_entries.push_back(std::move(entry));
        if (g_entries.size() > kMaxEntries)
            g_entries.pop_front();
        ++g_generation;
        if (level == LogLevel::Error)
        {
            g_errorGenerations.push_back(g_generation);
            if (g_errorGenerations.size() > kMaxEntries)
                g_errorGenerations.pop_front();
        }
    }

    void Clear()
    {
        std::lock_guard lock(g_lock);
        g_entries.clear();
        g_errorGenerations.clear();
        ++g_generation;
    }

    std::vector<LogEntry> Entries()
    {
        std::lock_guard lock(g_lock);
        return { g_entries.begin(), g_entries.end() };
    }

    std::optional<LogEntry> Last()
    {
        std::lock_guard lock(g_lock);
        if (g_entries.empty())
            return std::nullopt;
        return g_entries.back();
    }

    std::uint64_t Generation()
    {
        std::lock_guard lock(g_lock);
        return g_generation;
    }

    std::size_t ErrorsSince(std::uint64_t generation)
    {
        std::lock_guard lock(g_lock);
        return static_cast<std::size_t>(std::count_if(g_errorGenerations.begin(), g_errorGenerations.end(), [&](std::uint64_t at) { return at > generation; }));
    }
}
