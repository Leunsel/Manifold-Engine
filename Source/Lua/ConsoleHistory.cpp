#include "Lua/ConsoleHistory.h"

#include <algorithm>
#include <fstream>

namespace Manifold::Lua
{
    ConsoleHistory::ConsoleHistory(std::filesystem::path file, std::size_t limit) : m_file(std::move(file)), m_limit(std::max<std::size_t>(limit, 1)) {}

    std::string ConsoleHistory::Escape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            if (c == '\\')
                out += "\\\\";
            else if (c == '\n')
                out += "\\n";
            else if (c != '\r')
                out += c;
        }
        return out;
    }

    std::string ConsoleHistory::Unescape(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\\' && i + 1 < text.size())
            {
                const char next = text[++i];
                out += next == 'n' ? '\n' : next;
                continue;
            }
            out += text[i];
        }
        return out;
    }

    void ConsoleHistory::Load()
    {
        m_items.clear();
        Reset();
        if (m_file.empty())
            return;
        std::ifstream file(m_file, std::ios::binary);
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                m_items.push_back(Unescape(line));
        }
        if (m_items.size() > m_limit)
            m_items.erase(m_items.begin(), m_items.begin() + static_cast<std::ptrdiff_t>(m_items.size() - m_limit));
    }

    bool ConsoleHistory::Save() const
    {
        if (m_file.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(m_file.parent_path(), error);
        const std::filesystem::path temporary = m_file.wstring() + L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            for (const std::string& item : m_items)
                file << Escape(item) << "\n";
            if (!file)
                return false;
        }
        std::filesystem::rename(temporary, m_file, error);
        return !error;
    }

    void ConsoleHistory::Add(std::string text)
    {
        Reset();
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        if (text.empty())
            return;
        m_items.erase(std::remove(m_items.begin(), m_items.end(), text), m_items.end());
        m_items.push_back(std::move(text));
        if (m_items.size() > m_limit)
            m_items.erase(m_items.begin());
    }

    void ConsoleHistory::Clear()
    {
        m_items.clear();
        Reset();
    }

    std::optional<std::string> ConsoleHistory::Previous(const std::string& draft)
    {
        if (m_items.empty())
            return std::nullopt;
        if (m_cursor < 0)
        {
            m_draft = draft;
            m_cursor = static_cast<int>(m_items.size()) - 1;
            return m_items[static_cast<std::size_t>(m_cursor)];
        }
        if (m_cursor == 0)
            return std::nullopt;
        --m_cursor;
        return m_items[static_cast<std::size_t>(m_cursor)];
    }

    std::optional<std::string> ConsoleHistory::Next()
    {
        if (m_cursor < 0)
            return std::nullopt;
        if (m_cursor + 1 >= static_cast<int>(m_items.size()))
        {
            m_cursor = -1;
            return m_draft;
        }
        ++m_cursor;
        return m_items[static_cast<std::size_t>(m_cursor)];
    }

    void ConsoleHistory::Reset()
    {
        m_cursor = -1;
        m_draft.clear();
    }
}
