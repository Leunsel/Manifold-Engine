#include "Lua/LuaArgs.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

namespace Manifold::Lua
{
    Engine& Owner(lua_State* state)
    {
        return EngineOf(state).Owner();
    }

    std::string Key(lua_State* state, int index)
    {
        return lua_type(state, index) == LUA_TSTRING ? Lower(lua_tostring(state, index)) : std::string();
    }

    std::string TextArgument(lua_State* state, int index)
    {
        std::size_t length = 0;
        const char* text = luaL_tolstring(state, index, &length);
        std::string result(text, length);
        lua_pop(state, 1);
        return result;
    }

    std::uint64_t AddressArgument(lua_State* state, int index)
    {
        if (lua_type(state, index) == LUA_TNUMBER)
        {
            const auto value = ExactInteger(state, index);
            if (!value)
                luaL_argerror(state, index, "the address is not a whole number");
            return static_cast<std::uint64_t>(value.value_or(0));
        }
        const char* text = luaL_checkstring(state, index);
        Status error;
        const auto value = Owner(state).Evaluate(text, &error);
        if (!value)
            luaL_error(state, "%s", error.Text.c_str());
        return value.value_or(0);
    }

    std::optional<std::uint64_t> ResolveAddress(lua_State* state, int index, Status& error)
    {
        const int type = lua_type(state, index);
        if (type == LUA_TNUMBER)
        {
            const auto value = ExactInteger(state, index);
            if (!value)
            {
                error = Status::Fail(ErrorCode::LuaBadArgument, "the address is not a whole number");
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(*value);
        }
        if (type != LUA_TSTRING)
        {
            error = Status::Fail(ErrorCode::LuaBadArgument, "the address must be a number or an address expression");
            return std::nullopt;
        }
        const char* text = lua_tostring(state, index);
        const auto value = Owner(state).Evaluate(text, &error);
        if (!value && error.Ok())
            error = Status::Fail(ErrorCode::ExprSyntax, text);
        return value;
    }

    std::optional<std::uint64_t> ResolveAddress(lua_State* state, int index)
    {
        Status ignored;
        return ResolveAddress(state, index, ignored);
    }

    std::vector<std::uint8_t> ByteTable(lua_State* state, int index)
    {
        std::vector<std::uint8_t> bytes;
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, index));
        bytes.reserve(static_cast<std::size_t>(count));
        for (lua_Integer i = 1; i <= count; ++i)
        {
            lua_rawgeti(state, index, i);
            bytes.push_back(static_cast<std::uint8_t>(lua_tointeger(state, -1)));
            lua_pop(state, 1);
        }
        return bytes;
    }

    std::vector<std::uint8_t> BytesFrom(lua_State* state, int first)
    {
        std::vector<std::uint8_t> bytes;
        const int top = lua_gettop(state);
        for (int i = first; i <= top; ++i)
        {
            if (lua_istable(state, i))
            {
                const std::vector<std::uint8_t> part = ByteTable(state, i);
                bytes.insert(bytes.end(), part.begin(), part.end());
            }
            else
            {
                bytes.push_back(static_cast<std::uint8_t>(luaL_checkinteger(state, i)));
            }
        }
        return bytes;
    }

    void NoteOutsideAction(lua_State* state, std::string_view function, std::string_view what)
    {
        constexpr std::size_t kShown = 400;
        std::string text(function);
        text += ": ";
        text += what.substr(0, kShown);
        if (what.size() > kShown)
            text += "...";
        luaL_where(state, 1);
        std::string where = lua_tostring(state, -1) ? lua_tostring(state, -1) : "";
        lua_pop(state, 1);
        while (!where.empty() && (where.back() == ' ' || where.back() == ':'))
            where.pop_back();
        if (!where.empty())
            text += "  (" + where + ")";
        LOG_WARN("Script", text);
    }

    std::string FailureMessage(const Status& status)
    {
        const std::string suffix = " (" + ErrorName(status.Code) + ")";
        std::string text = status.Text;
        if (text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
            text.resize(text.size() - suffix.size());
        return text;
    }

    int PushFailure(lua_State* state, const Status& status)
    {
        lua_pushnil(state);
        lua_pushstring(state, ErrorName(status.Code).c_str());
        lua_pushstring(state, FailureMessage(status).c_str());
        return 3;
    }

    int PushFailure(lua_State* state, ErrorCode code, std::string_view detail)
    {
        return PushFailure(state, Status::Fail(code, detail));
    }
}
