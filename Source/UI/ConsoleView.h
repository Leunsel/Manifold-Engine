#pragma once

#include "Lua/LuaConsole.h"

#include "imgui.h"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Manifold::UI
{
    struct ConsoleActions
    {
        std::function<void(std::uint64_t)> OpenMemory;
        std::function<void(std::uint64_t)> Dissect;
        std::function<void(std::uint64_t)> AddAddress;
        std::function<void(const std::string&)> Recall;
        std::function<void(const std::string&, int)> GotoSource;
        std::function<void()> Clear;
    };

    struct ConsoleFilter
    {
        std::array<bool, 5> Levels{ true, true, true, true, true };
        std::string Channel;
        std::string Search;
        bool Wrap = true;
        bool Time = true;
        bool Hex = false;
    };

    class ConsoleView
    {
    public:
        void DrawFilterBar(const Lua::ConsoleLog& log, ConsoleFilter& filter, bool& changed);
        void Draw(const Lua::ConsoleLog& log, const ConsoleFilter& filter, ImVec2 size, const ConsoleActions& actions);
        void FocusSearch() { m_focusSearch = true; }
        void Follow() { m_follow = true; }
        bool& Following() { return m_follow; }
        void Invalidate() { m_built = false; }

    private:
        enum class RowKind : std::uint8_t
        {
            Head,
            Text,
            Code,
            Toggle,
            Traceback,
            Node
        };

        struct Row
        {
            std::uint64_t Id = 0;
            RowKind Kind = RowKind::Head;
            std::uint8_t Value = 0;
            std::uint32_t Start = 0;
            std::uint32_t End = 0;
        };

        void Rebuild(const Lua::ConsoleLog& log, const ConsoleFilter& filter);
        void Append(const Lua::ConsoleEntry& entry, const ConsoleFilter& filter);
        bool Passes(const Lua::ConsoleEntry& entry, const ConsoleFilter& filter) const;
        void WrapText(std::uint64_t id, RowKind kind, std::uint8_t value, std::string_view text, std::uint32_t columns, bool head);
        void AddTree(std::uint64_t id, std::uint8_t value, const Lua::ValueSnapshot& snapshot, bool includeRoot);
        void DrawRow(const Row& row, const Lua::ConsoleEntry& entry, const ConsoleFilter& filter, const ConsoleActions& actions, float width, bool continues);
        bool Expanded(std::uint64_t id, std::uint32_t key) const;
        void Toggle(std::uint64_t id, std::uint32_t key);
        const Lua::ValueSnapshot* ValueOf(const Lua::ConsoleEntry& entry, std::uint8_t value) const;
        float MessageOffset(bool time) const;
        float RowStride() const;

        std::vector<Row> m_rows;
        std::set<std::pair<std::uint64_t, std::uint32_t>> m_expanded;
        bool m_built = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_epoch = ~0ull;
        std::uint64_t m_dropped = 0;
        std::uint64_t m_lastId = 0;
        std::uint32_t m_lastRepeat = 0;
        std::uint32_t m_columns = 0;
        std::uint32_t m_maxColumns = 0;
        float m_glyph = 7.0f;
        float m_iconColumn = 16.0f;
        ConsoleFilter m_builtFilter;
        void Select(std::uint64_t id, bool toggle, bool range);
        std::vector<std::uint64_t> ShownIds() const;
        std::string Copy(const Lua::ConsoleLog& log, const std::vector<std::uint64_t>& ids) const;

        std::uint64_t m_selected = 0;
        std::uint64_t m_anchor = 0;
        std::set<std::uint64_t> m_selection;
        bool m_follow = true;
        bool m_focusSearch = false;
        std::uint64_t m_menuId = 0;
        std::uint64_t m_menuAddress = 0;
        bool m_menuHasAddress = false;
        std::string m_menuValue;
    };
}
