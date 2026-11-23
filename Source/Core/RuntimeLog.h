#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    enum class LogLevel
    {
        Debug,
        Info,
        Success,
        Warning,
        Error
    };

    struct LogEntry
    {
        std::string Time;
        LogLevel Level = LogLevel::Info;
        std::string Area;
        std::string Trace;
        std::string Message;
        std::string Full;
        int Milliseconds = 0;
        double Clock = 0.0;
        std::uint32_t Thread = 0;
        bool MainThread = false;
        std::uint64_t Serial = 0;

        const std::string& Text() const { return Full.empty() ? Message : Full; }
    };

    namespace RuntimeLog
    {
        inline constexpr std::size_t kMaxEntries = 4000;

        const char* LevelName(LogLevel level);
        bool EntryMatches(const LogEntry& entry, LogLevel lowest, std::string_view text);
        std::string TraceText(const char* file, const char* function, int line);

        void OpenFile(const std::filesystem::path& path);
        std::filesystem::path FilePath();
        void SetVerbose(bool verbose);
        bool Verbose();
        void Write(LogLevel level, std::string_view area, const char* file, const char* function, int line, std::string_view message);
        // message is the one line the list shows; details (any number of lines) is what the details
        // pane shows and what goes into the log file, indented under the entry.
        void WriteDetailed(LogLevel level, std::string_view area, const char* file, const char* function, int line, std::string_view message, std::string_view details);
        void Clear();
        std::vector<LogEntry> Entries();
        std::optional<LogEntry> Last();
        std::uint64_t Generation();
        std::size_t ErrorsSince(std::uint64_t generation);
    }
}

#define MLOG(level, area, message) ::Manifold::RuntimeLog::Write(::Manifold::LogLevel::level, (area), __FILE__, __FUNCTION__, __LINE__, (message))
#define LOG_DEBUG(area, message) MLOG(Debug, area, message)
#define LOG_INFO(area, message) MLOG(Info, area, message)
#define LOG_SUCCESS(area, message) MLOG(Success, area, message)
#define LOG_WARN(area, message) MLOG(Warning, area, message)
#define LOG_ERROR(area, message) MLOG(Error, area, message)
