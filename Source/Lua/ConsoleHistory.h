#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Lua
{
    class ConsoleHistory
    {
    public:
        explicit ConsoleHistory(std::filesystem::path file = {}, std::size_t limit = 1000);

        void Load();
        bool Save() const;
        void Add(std::string text);
        void Clear();
        const std::vector<std::string>& Items() const { return m_items; }

        std::optional<std::string> Previous(const std::string& draft);
        std::optional<std::string> Next();
        void Reset();
        bool Browsing() const { return m_cursor >= 0; }

        static std::string Escape(std::string_view text);
        static std::string Unescape(std::string_view text);

    private:
        std::filesystem::path m_file;
        std::size_t m_limit;
        std::vector<std::string> m_items;
        int m_cursor = -1;
        std::string m_draft;
    };
}
