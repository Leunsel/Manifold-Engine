#include "Core/RuntimeLog.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <share.h>
#include <system_error>
#include <utility>

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

        std::string TimeText()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            char text[16] = "??:??:??";
            if (::localtime_s(&local, &now) == 0)
                std::strftime(text, sizeof(text), "%H:%M:%S", &local);
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
        return text.empty() || contains(entry.Message) || contains(entry.Trace) || contains(entry.Area);
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
        LogEntry entry;
        entry.Time = TimeText();
        entry.Level = level;
        entry.Area = std::string(area);
        entry.Trace = TraceText(file, function, line);
        entry.Message = OneLine(message);

        std::lock_guard lock(g_lock);
        if (level == LogLevel::Debug && !g_verbose)
            return;
        if (g_file)
        {
            std::fprintf(g_file, "[%s] [%-5s] [%s] [%s] %s\n", entry.Time.c_str(), LevelName(level), entry.Area.c_str(), entry.Trace.c_str(), entry.Message.c_str());
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
