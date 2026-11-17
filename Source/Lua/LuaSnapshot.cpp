#include "Lua/LuaSnapshot.h"

#include "Core/Text.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace Manifold::Lua
{
    namespace
    {
        constexpr int kCollect = 2048;

        thread_local double t_deadline = 0.0;

        double Now()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void DeadlineHook(lua_State* state, lua_Debug*)
        {
            if (Now() > t_deadline)
                luaL_error(state, "took too long");
        }

        int ToText(lua_State* state)
        {
            luaL_tolstring(state, 1, nullptr);
            return 1;
        }

        int IndexField(lua_State* state)
        {
            lua_settop(state, 2);
            lua_gettable(state, 1);
            return 1;
        }

        struct KeyRef
        {
            int Slot = 0;
            int Kind = 2;
            lua_Integer Integer = 0;
            std::string Text;
        };

        std::string KeyText(lua_State* state, int index)
        {
            switch (lua_type(state, index))
            {
            case LUA_TNUMBER:
            {
                if (lua_isinteger(state, index))
                    return "[" + std::to_string(lua_tointeger(state, index)) + "]";
                char text[64] = {};
                std::snprintf(text, sizeof(text), "[%.14g]", lua_tonumber(state, index));
                return text;
            }
            case LUA_TSTRING:
            {
                std::size_t length = 0;
                const char* raw = lua_tolstring(state, index, &length);
                const std::string_view name(raw, length);
                const bool identifier = !name.empty() && !(name[0] >= '0' && name[0] <= '9') &&
                                        std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
                return identifier ? std::string(name) : "[" + QuoteLuaString(name, 60) + "]";
            }
            case LUA_TBOOLEAN:
                return lua_toboolean(state, index) ? "[true]" : "[false]";
            default:
            {
                char text[64] = {};
                std::snprintf(text, sizeof(text), "[%s: %p]", luaL_typename(state, index), lua_topointer(state, index));
                return text;
            }
            }
        }

        class Builder
        {
        public:
            Builder(lua_State* state, const SnapshotLimits& limits, const AddressProbe& probe, ValueSnapshot& out) : m_state(state), m_limits(limits), m_probe(probe), m_out(out) {}

            void Add(int index, std::string key, int depth)
            {
                index = lua_absindex(m_state, index);
                if (static_cast<int>(m_out.Nodes.size()) >= m_limits.Nodes)
                {
                    m_out.Truncated = true;
                    return;
                }
                const std::size_t at = m_out.Nodes.size();
                m_out.Nodes.emplace_back();
                SnapshotNode node;
                node.Key = std::move(key);
                node.Depth = static_cast<std::uint8_t>(std::min(depth, 255));
                node.Type = luaL_typename(m_state, index);
                switch (lua_type(m_state, index))
                {
                case LUA_TNIL:
                    node.Value = "nil";
                    break;
                case LUA_TBOOLEAN:
                    node.Value = lua_toboolean(m_state, index) ? "true" : "false";
                    break;
                case LUA_TNUMBER:
                    if (lua_isinteger(m_state, index))
                    {
                        node.IsInteger = true;
                        node.Integer = static_cast<std::int64_t>(lua_tointeger(m_state, index));
                        node.Value = std::to_string(node.Integer);
                        if (m_probe && m_probes < m_limits.AddressProbes && node.Integer >= 0x10000 && node.Integer <= 0x7FFFFFFFFFFFll)
                        {
                            ++m_probes;
                            node.IsAddress = m_probe(static_cast<std::uint64_t>(node.Integer));
                        }
                    }
                    else
                    {
                        char text[64] = {};
                        std::snprintf(text, sizeof(text), "%.14g", lua_tonumber(m_state, index));
                        node.Value = text;
                    }
                    break;
                case LUA_TSTRING:
                {
                    std::size_t length = 0;
                    const char* raw = lua_tolstring(m_state, index, &length);
                    node.Value = QuoteLuaString(std::string_view(raw, length), 300);
                    break;
                }
                case LUA_TFUNCTION:
                {
                    lua_Debug info{};
                    lua_pushvalue(m_state, index);
                    if (lua_getinfo(m_state, ">S", &info) && info.what && std::string_view(info.what) != "C")
                        node.Value = "function " + std::string(info.short_src) + ":" + std::to_string(info.linedefined);
                    else
                        node.Value = "function (C)";
                    break;
                }
                case LUA_TTHREAD:
                    node.Value = "thread";
                    break;
                case LUA_TUSERDATA:
                case LUA_TLIGHTUSERDATA:
                    node.Label = Label(index, true);
                    node.Value = node.Label.empty() ? std::string("userdata") : node.Label;
                    break;
                case LUA_TTABLE:
                    Table(index, depth, node, at);
                    break;
                default:
                    node.Value = node.Type;
                    break;
                }
                node.Subtree = static_cast<std::uint32_t>(m_out.Nodes.size() - at - 1);
                m_out.Nodes[at] = std::move(node);
            }

        private:
            std::string Label(int index, bool userdata)
            {
                index = lua_absindex(m_state, index);
                std::string label;
                const int top = lua_gettop(m_state);
                if (lua_getmetatable(m_state, index))
                {
                    if (lua_getfield(m_state, -1, "__name") == LUA_TSTRING)
                        label = lua_tostring(m_state, -1);
                    lua_pop(m_state, 1);
                    const bool text = lua_getfield(m_state, -1, "__tostring") != LUA_TNIL;
                    lua_pop(m_state, 1);
                    if (text)
                    {
                        std::string shown;
                        if (Protected(&ToText, index, shown))
                            label = shown;
                    }
                }
                lua_settop(m_state, top);
                if (userdata && label.empty())
                {
                    lua_pushcfunction(m_state, &IndexField);
                    lua_pushvalue(m_state, index);
                    lua_pushliteral(m_state, "ClassName");
                    if (Guarded(2) && lua_type(m_state, -1) == LUA_TSTRING)
                        label = lua_tostring(m_state, -1);
                    lua_settop(m_state, top);
                }
                if (label.size() > 120)
                    label = label.substr(0, 117) + "...";
                return label;
            }

            bool Guarded(int arguments)
            {
                const lua_Hook hook = lua_gethook(m_state);
                const int mask = lua_gethookmask(m_state);
                const int count = lua_gethookcount(m_state);
                const double previous = t_deadline;
                t_deadline = Now() + m_limits.LabelSeconds;
                lua_sethook(m_state, &DeadlineHook, LUA_MASKCOUNT, 500);
                const int status = lua_pcall(m_state, arguments, 1, 0);
                lua_sethook(m_state, hook, mask, count);
                t_deadline = previous;
                return status == LUA_OK;
            }

            bool Protected(lua_CFunction function, int index, std::string& text)
            {
                const int top = lua_gettop(m_state);
                lua_pushcfunction(m_state, function);
                lua_pushvalue(m_state, index);
                const bool ok = Guarded(1) && lua_type(m_state, -1) == LUA_TSTRING;
                if (ok)
                    text = lua_tostring(m_state, -1);
                lua_settop(m_state, top);
                return ok;
            }

            void Table(int index, int depth, SnapshotNode& node, std::size_t self)
            {
                (void)self;
                lua_checkstack(m_state, 16);
                const void* pointer = lua_topointer(m_state, index);
                node.Label = Label(index, false);
                if (std::find(m_path.begin(), m_path.end(), pointer) != m_path.end())
                {
                    node.Cycle = true;
                    node.Value = node.Label.empty() ? std::string("<cycle>") : node.Label + " <cycle>";
                    return;
                }
                const int top = lua_gettop(m_state);
                int source = index;
                if (!HasEntries(index) && lua_getmetatable(m_state, index))
                {
                    if (lua_getfield(m_state, -1, "__index") == LUA_TTABLE && HasEntries(lua_gettop(m_state)))
                        source = lua_gettop(m_state);
                }
                lua_newtable(m_state);
                const int keys = lua_gettop(m_state);
                std::vector<KeyRef> refs;
                std::uint32_t count = 0;
                lua_pushnil(m_state);
                while (lua_next(m_state, source))
                {
                    ++count;
                    lua_pop(m_state, 1);
                    if (static_cast<int>(refs.size()) < kCollect)
                    {
                        KeyRef ref;
                        ref.Slot = static_cast<int>(refs.size()) + 1;
                        if (lua_type(m_state, -1) == LUA_TNUMBER && lua_isinteger(m_state, -1))
                        {
                            ref.Kind = 0;
                            ref.Integer = lua_tointeger(m_state, -1);
                        }
                        else if (lua_type(m_state, -1) == LUA_TSTRING)
                        {
                            ref.Kind = 1;
                            ref.Text = lua_tostring(m_state, -1);
                        }
                        lua_pushvalue(m_state, -1);
                        lua_rawseti(m_state, keys, ref.Slot);
                        refs.push_back(std::move(ref));
                    }
                }
                node.Total = count;
                node.Value = node.Label.empty() ? "{" + std::to_string(count) + (count == 1 ? " entry}" : " entries}") : node.Label;
                if (depth < m_limits.Depth && count > 0)
                {
                    std::sort(refs.begin(), refs.end(), [](const KeyRef& a, const KeyRef& b)
                        {
                            if (a.Kind != b.Kind)
                                return a.Kind < b.Kind;
                            if (a.Kind == 0)
                                return a.Integer < b.Integer;
                            if (a.Kind == 1)
                                return a.Text < b.Text;
                            return a.Slot < b.Slot;
                        });
                    const std::size_t shown = std::min<std::size_t>(refs.size(), static_cast<std::size_t>(std::max(0, m_limits.Children)));
                    m_path.push_back(pointer);
                    for (std::size_t i = 0; i < shown; ++i)
                    {
                        if (static_cast<int>(m_out.Nodes.size()) >= m_limits.Nodes)
                        {
                            m_out.Truncated = true;
                            break;
                        }
                        lua_rawgeti(m_state, keys, refs[i].Slot);
                        const int key = lua_gettop(m_state);
                        lua_pushvalue(m_state, key);
                        lua_rawget(m_state, source);
                        Add(-1, KeyText(m_state, key), depth + 1);
                        lua_settop(m_state, key - 1);
                        ++node.Shown;
                    }
                    m_path.pop_back();
                }
                lua_settop(m_state, top);
            }

            bool HasEntries(int index)
            {
                lua_pushnil(m_state);
                if (lua_next(m_state, index))
                {
                    lua_pop(m_state, 2);
                    return true;
                }
                return false;
            }

            lua_State* m_state;
            const SnapshotLimits& m_limits;
            const AddressProbe& m_probe;
            ValueSnapshot& m_out;
            std::vector<const void*> m_path;
            int m_probes = 0;
        };
    }

    std::size_t ValueSnapshot::Bytes() const
    {
        std::size_t bytes = sizeof(ValueSnapshot);
        for (const SnapshotNode& node : Nodes)
            bytes += sizeof(SnapshotNode) + node.Key.size() + node.Type.size() + node.Value.size() + node.Label.size();
        return bytes;
    }

    std::string ValueSnapshot::Summary(bool hex) const
    {
        return Nodes.empty() ? std::string("nil") : NodeValueText(Nodes.front(), hex);
    }

    std::string NodeValueText(const SnapshotNode& node, bool hex)
    {
        if (node.IsInteger && (hex || node.IsAddress))
            return node.Integer < 0 ? "-0x" + Hex(static_cast<std::uint64_t>(-(node.Integer + 1)) + 1) : "0x" + Hex(static_cast<std::uint64_t>(node.Integer));
        return node.Value;
    }

    std::string QuoteLuaString(std::string_view text, std::size_t limit)
    {
        std::string out = "\"";
        std::size_t shown = 0;
        for (const char c : text)
        {
            if (shown >= limit)
            {
                out += "\xE2\x80\xA6\" (" + std::to_string(text.size()) + " bytes)";
                return out;
            }
            switch (c)
            {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char code[8] = {};
                    std::snprintf(code, sizeof(code), "\\%u", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += code;
                }
                else
                {
                    out += c;
                }
                break;
            }
            ++shown;
        }
        return out + "\"";
    }

    std::shared_ptr<const ValueSnapshot> TakeSnapshot(lua_State* state, int index, const SnapshotLimits& limits, const AddressProbe& probe)
    {
        auto snapshot = std::make_shared<ValueSnapshot>();
        index = lua_absindex(state, index);
        const int top = lua_gettop(state);
        lua_checkstack(state, 32);
        Builder builder(state, limits, probe, *snapshot);
        builder.Add(index, std::string(), 0);
        lua_settop(state, top);
        return snapshot;
    }
}
