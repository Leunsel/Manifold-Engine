#include "Lua/LuaConsole.h"

#include "Core/Text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace Manifold::Lua
{
    namespace
    {
        bool SameAs(const ConsoleEntry& a, const ConsoleEntry& b)
        {
            if (a.Kind == EntryKind::Input || a.Kind == EntryKind::Result || !a.Values.empty() || !b.Values.empty() || a.Fields || b.Fields)
                return false;
            return a.Kind == b.Kind && a.Level == b.Level && a.Channel == b.Channel && a.Text == b.Text && a.FieldsText == b.FieldsText && a.Traceback == b.Traceback &&
                   a.Code == b.Code && a.Source.Chunk == b.Source.Chunk && a.Source.Line == b.Source.Line && a.InputId == b.InputId;
        }

        void Tally(ConsoleCounts& counts, const ConsoleEntry& entry, int sign, std::size_t repeats)
        {
            const auto apply = [&](std::size_t& value)
            {
                value = sign > 0 ? value + repeats : value - std::min(value, repeats);
            };
            apply(counts.Levels[static_cast<std::size_t>(entry.Level)]);
            if (entry.Kind == EntryKind::Input)
                apply(counts.Inputs);
            if (entry.Kind == EntryKind::Result)
                apply(counts.Results);
            if (entry.Kind == EntryKind::Error)
                apply(counts.Errors);
        }
    }

    std::size_t ConsoleEntry::Bytes() const
    {
        std::size_t bytes = 64 + Channel.size() + Text.size() + FieldsText.size() + Traceback.size() + Code.size() + Source.Chunk.size() + Source.Function.size();
        if (Fields)
            bytes += Fields->Bytes();
        for (const auto& value : Values)
            bytes += value ? value->Bytes() : 0;
        return bytes;
    }

    std::string ConsoleEntry::PlainText() const
    {
        std::string text;
        if (Kind == EntryKind::Input)
            return "> " + Text;
        if (Kind == EntryKind::Result)
        {
            for (std::size_t i = 0; i < Values.size(); ++i)
                text += (i ? ", " : "") + (Values[i] ? Values[i]->Summary(false) : std::string("nil"));
            return text.empty() ? std::string("(no values)") : text;
        }
        if (Kind == EntryKind::Log || Kind == EntryKind::Error)
            text = std::string(LevelTag(Level)) + " ";
        if (!Channel.empty())
            text += "[" + Channel + "] ";
        text += Text;
        if (!Code.empty())
            text += " (" + Code + ")";
        if (!FieldsText.empty())
            text += "  " + FieldsText;
        if (!Traceback.empty())
            text += "\n" + Traceback;
        return text;
    }

    std::uint64_t ConsoleLog::Add(ConsoleEntry entry)
    {
        std::lock_guard lock(m_lock);
        if (!m_entries.empty() && SameAs(m_entries.back(), entry))
        {
            ConsoleEntry& last = m_entries.back();
            ++last.Repeat;
            last.TimeMs = entry.TimeMs;
            Tally(m_counts, entry, 1, 1);
            ++m_generation;
            return last.Id;
        }
        entry.Id = m_nextId++;
        if (entry.Kind == EntryKind::Input && entry.InputId == 0)
            entry.InputId = entry.Id;
        if (!entry.Channel.empty() && std::find(m_channels.begin(), m_channels.end(), entry.Channel) == m_channels.end() && m_channels.size() < 512)
        {
            m_channels.push_back(entry.Channel);
            std::sort(m_channels.begin(), m_channels.end());
        }
        Tally(m_counts, entry, 1, 1);
        m_bytes += entry.Bytes();
        const std::uint64_t id = entry.Id;
        m_entries.push_back(std::move(entry));
        while (m_entries.size() > 1 && (m_entries.size() > kMaxEntries || m_bytes > kMaxBytes))
        {
            const ConsoleEntry& first = m_entries.front();
            m_bytes -= std::min(m_bytes, first.Bytes());
            Tally(m_counts, first, -1, first.Repeat);
            m_entries.pop_front();
            ++m_dropped;
        }
        ++m_generation;
        return id;
    }

    void ConsoleLog::Clear()
    {
        std::lock_guard lock(m_lock);
        m_entries.clear();
        m_bytes = 0;
        m_dropped = 0;
        m_counts = {};
        m_channels.clear();
        ++m_epoch;
        ++m_generation;
    }

    std::size_t ConsoleLog::Count() const
    {
        std::lock_guard lock(m_lock);
        return m_entries.size();
    }

    std::uint64_t ConsoleLog::Dropped() const
    {
        std::lock_guard lock(m_lock);
        return m_dropped;
    }

    std::size_t ConsoleLog::Bytes() const
    {
        std::lock_guard lock(m_lock);
        return m_bytes;
    }

    ConsoleCounts ConsoleLog::Counts() const
    {
        std::lock_guard lock(m_lock);
        return m_counts;
    }

    void ConsoleLog::Visit(std::size_t first, std::size_t last, const std::function<void(std::size_t, const ConsoleEntry&)>& visit) const
    {
        std::lock_guard lock(m_lock);
        last = std::min(last, m_entries.size());
        for (std::size_t i = first; i < last; ++i)
            visit(i, m_entries[i]);
    }

    std::vector<ConsoleEntry> ConsoleLog::Entries() const
    {
        std::lock_guard lock(m_lock);
        return { m_entries.begin(), m_entries.end() };
    }

    std::optional<ConsoleEntry> ConsoleLog::Last() const
    {
        std::lock_guard lock(m_lock);
        if (m_entries.empty())
            return std::nullopt;
        return m_entries.back();
    }

    std::uint64_t ConsoleLog::FirstId() const
    {
        std::lock_guard lock(m_lock);
        return m_entries.empty() ? m_nextId : m_entries.front().Id;
    }

    bool ConsoleLog::VisitId(std::uint64_t id, const std::function<void(const ConsoleEntry&)>& visit) const
    {
        std::lock_guard lock(m_lock);
        if (m_entries.empty() || id < m_entries.front().Id)
            return false;
        const std::uint64_t index = id - m_entries.front().Id;
        if (index >= m_entries.size())
            return false;
        visit(m_entries[static_cast<std::size_t>(index)]);
        return true;
    }

    std::vector<std::string> ConsoleLog::Channels() const
    {
        std::lock_guard lock(m_lock);
        return m_channels;
    }

    std::int64_t ConsoleClock()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string ConsoleTime(std::int64_t timeMs, bool milliseconds)
    {
        const std::time_t seconds = static_cast<std::time_t>(timeMs / 1000);
        std::tm local{};
        localtime_s(&local, &seconds);
        char text[32] = {};
        if (milliseconds)
            std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(timeMs % 1000));
        else
            std::snprintf(text, sizeof(text), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        return text;
    }

    ConsoleSource ParseSource(std::string_view message, std::string_view* rest)
    {
        ConsoleSource source;
        const std::size_t limit = std::min<std::size_t>(message.find('\n'), message.size());
        for (std::size_t at = message.find(':'); at != std::string_view::npos && at < limit; at = message.find(':', at + 1))
        {
            std::size_t end = at + 1;
            while (end < limit && message[end] >= '0' && message[end] <= '9')
                ++end;
            if (end == at + 1 || end >= limit || message[end] != ':')
                continue;
            const std::string_view chunk = message.substr(0, at);
            if (chunk.empty())
                continue;
            if (chunk.front() != '[' && chunk.find(' ') != std::string_view::npos && chunk.find('\\') == std::string_view::npos && chunk.find('/') == std::string_view::npos)
                continue;
            source.Chunk = std::string(chunk);
            source.Line = std::atoi(std::string(message.substr(at + 1, end - at - 1)).c_str());
            if (rest)
            {
                std::size_t after = end + 1;
                while (after < message.size() && message[after] == ' ')
                    ++after;
                *rest = message.substr(after);
            }
            return source;
        }
        if (rest)
            *rest = message;
        return source;
    }

    void SplitTraceback(const std::string& message, std::string& text, std::string& traceback)
    {
        const std::size_t at = message.find("stack traceback:");
        if (at == std::string::npos)
        {
            text = message;
            traceback.clear();
            return;
        }
        text = message.substr(0, at);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\r'))
            text.pop_back();
        traceback = message.substr(at);
        while (!traceback.empty() && (traceback.back() == '\n' || traceback.back() == '\r'))
            traceback.pop_back();
    }

    std::string ChunkLabel(std::string_view source, std::string_view shortSource)
    {
        if (!source.empty() && source.front() == '@')
        {
            const std::size_t slash = source.find_last_of("\\/");
            return std::string(slash == std::string_view::npos ? source.substr(1) : source.substr(slash + 1));
        }
        if (!source.empty() && source.front() == '=')
            return std::string(source.substr(1));
        return std::string(shortSource);
    }

    const char* LevelTag(LogLevel level)
    {
        constexpr const char* kTags[] = { "DBG", "INF", "OK ", "WRN", "ERR" };
        return kTags[std::clamp(static_cast<int>(level), 0, 4)];
    }
}
