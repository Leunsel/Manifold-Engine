#pragma once

#include "Core/RuntimeLog.h"
#include "Lua/LuaSnapshot.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Lua
{
    enum class EntryKind : std::uint8_t
    {
        Print,
        Log,
        Input,
        Result,
        Error,
        System
    };

    struct ConsoleSource
    {
        std::string Chunk;
        std::string Function;
        int Line = 0;
    };

    struct ConsoleEntry
    {
        std::uint64_t Id = 0;
        std::uint64_t InputId = 0;
        EntryKind Kind = EntryKind::Print;
        LogLevel Level = LogLevel::Info;
        std::int64_t TimeMs = 0;
        double Elapsed = -1.0;
        std::string Channel;
        std::string Text;
        std::string FieldsText;
        std::string Traceback;
        std::string Code;
        ConsoleSource Source;
        std::shared_ptr<const ValueSnapshot> Fields;
        std::vector<std::shared_ptr<const ValueSnapshot>> Values;
        std::uint32_t Repeat = 1;

        std::size_t Bytes() const;
        std::string PlainText() const;
    };

    struct ConsoleCounts
    {
        std::array<std::size_t, 5> Levels{};
        std::size_t Inputs = 0;
        std::size_t Results = 0;
        std::size_t Errors = 0;
    };

    class ConsoleLog
    {
    public:
        static constexpr std::size_t kMaxEntries = 100000;
        static constexpr std::size_t kMaxBytes = 32u << 20;

        std::uint64_t Add(ConsoleEntry entry);
        void Clear();
        std::size_t Count() const;
        std::uint64_t Dropped() const;
        std::uint64_t Generation() const { return m_generation.load(); }
        std::uint64_t Epoch() const { return m_epoch.load(); }
        std::size_t Bytes() const;
        ConsoleCounts Counts() const;
        void Visit(std::size_t first, std::size_t last, const std::function<void(std::size_t, const ConsoleEntry&)>& visit) const;
        std::vector<ConsoleEntry> Entries() const;
        std::optional<ConsoleEntry> Last() const;
        std::uint64_t FirstId() const;
        bool VisitId(std::uint64_t id, const std::function<void(const ConsoleEntry&)>& visit) const;
        std::vector<std::string> Channels() const;

    private:
        mutable std::mutex m_lock;
        std::deque<ConsoleEntry> m_entries;
        std::size_t m_bytes = 0;
        std::uint64_t m_dropped = 0;
        std::uint64_t m_nextId = 1;
        ConsoleCounts m_counts;
        std::vector<std::string> m_channels;
        std::atomic<std::uint64_t> m_generation{ 1 };
        std::atomic<std::uint64_t> m_epoch{ 0 };
    };

    std::int64_t ConsoleClock();
    std::string ConsoleTime(std::int64_t timeMs, bool milliseconds);
    ConsoleSource ParseSource(std::string_view message, std::string_view* rest = nullptr);
    void SplitTraceback(const std::string& message, std::string& text, std::string& traceback);
    const char* LevelTag(LogLevel level);
    std::string ChunkLabel(std::string_view source, std::string_view shortSource);
}
