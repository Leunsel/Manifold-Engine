#include "Lua/LuaEngine.h"

#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "Lua/LuaApi.h"
#include "Target/Process.h"

#include "lua.hpp"

#include <chrono>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kTracebackKey = "Manifold.LastTraceback";

        double Clock()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        int KeepTraceback(lua_State* state)
        {
            luaL_traceback(state, state, nullptr, 1);
            lua_setfield(state, LUA_REGISTRYINDEX, kTracebackKey);
            lua_settop(state, 1);
            return 1;
        }

        int ToText(lua_State* state)
        {
            luaL_tolstring(state, 1, nullptr);
            return 1;
        }

        std::string SafeText(lua_State* state, int index)
        {
            index = lua_absindex(state, index);
            if (lua_type(state, index) == LUA_TSTRING)
                return lua_tostring(state, index);
            const int top = lua_gettop(state);
            lua_pushcfunction(state, &ToText);
            lua_pushvalue(state, index);
            std::string text = lua_pcall(state, 1, 1, 0) == LUA_OK && lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : std::string("(error object is a ") + luaL_typename(state, index) + " value)";
            lua_settop(state, top);
            return text;
        }

        std::string RawString(lua_State* state, int index, const char* key)
        {
            lua_pushstring(state, key);
            lua_rawget(state, index);
            std::string text = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : std::string();
            lua_pop(state, 1);
            return text;
        }
    }

    std::shared_ptr<const ValueSnapshot> LuaEngine::Snapshot(lua_State* state, int index)
    {
        SnapshotLimits limits;
        AddressProbe probe;
        if (const std::shared_ptr<Target::Process> process = m_engine.Process(); process && process->Alive())
            probe = [process](std::uint64_t address) { return process->IsReadable(address); };
        return TakeSnapshot(state, index, limits, probe);
    }

    ConsoleEntry LuaEngine::ErrorEntry(lua_State* state, int index, std::string traceback, std::uint64_t inputId)
    {
        ConsoleEntry entry;
        entry.Kind = EntryKind::Error;
        entry.Level = LogLevel::Error;
        entry.InputId = inputId;
        index = lua_absindex(state, index);
        if (lua_type(state, index) == LUA_TTABLE && !RawString(state, index, "message").empty())
        {
            entry.Text = RawString(state, index, "message");
            entry.Code = RawString(state, index, "code");
            const std::string where = RawString(state, index, "where");
            if (const std::size_t colon = where.rfind(':'); colon != std::string::npos && colon + 1 < where.size())
            {
                entry.Source.Chunk = where.substr(0, colon);
                entry.Source.Line = std::atoi(where.c_str() + colon + 1);
            }
            entry.Traceback = RawString(state, index, "traceback");
        }
        else
        {
            std::string message;
            SplitTraceback(SafeText(state, index), message, entry.Traceback);
            std::string_view rest;
            entry.Source = ParseSource(message, &rest);
            entry.Text = std::string(rest);
        }
        if (entry.Traceback.empty())
            entry.Traceback = std::move(traceback);
        return entry;
    }

    bool LuaEngine::Incomplete(lua_State* state, const std::string& code)
    {
        const int top = lua_gettop(state);
        const std::string expression = "return " + code;
        if (luaL_loadbuffer(state, expression.data(), expression.size(), "=input") == LUA_OK)
        {
            lua_settop(state, top);
            return false;
        }
        lua_settop(state, top);
        bool incomplete = false;
        if (luaL_loadbuffer(state, code.data(), code.size(), "=input") != LUA_OK)
        {
            const std::string message = lua_tostring(state, -1) ? lua_tostring(state, -1) : "";
            incomplete = message.size() >= 5 && message.compare(message.size() - 5, 5, "<eof>") == 0;
        }
        lua_settop(state, top);
        return incomplete;
    }

    std::uint64_t LuaEngine::Evaluate(const std::string& code, std::function<void(const Status&)> done, int* task)
    {
        const std::uint64_t number = ++m_inputCounter;
        const std::string chunk = "[" + std::to_string(number) + "]";
        RememberChunk(chunk, code);
        ConsoleEntry input;
        input.Kind = EntryKind::Input;
        input.Text = code;
        input.Source.Chunk = chunk;
        const std::uint64_t inputId = AddEntry(std::move(input));

        const std::string name = "=" + chunk;
        std::string body = "return " + code;
        if (luaL_loadbuffer(m_state, body.data(), body.size(), name.c_str()) != LUA_OK)
            body = code;
        else
            body = "local function values(...) return ... end return values(" + code + "\n)";
        lua_pop(m_state, 1);
        const double started = Clock();
        const auto results = [this, inputId, started](lua_State* thread, int count)
        {
            if (count <= 0)
                return;
            ConsoleEntry entry;
            entry.Kind = EntryKind::Result;
            entry.InputId = inputId;
            entry.Elapsed = Clock() - started;
            const int first = lua_gettop(thread) - count + 1;
            for (int i = 0; i < count; ++i)
                entry.Values.push_back(Snapshot(thread, first + i));
            AddEntry(std::move(entry));
        };

        if (!m_engine.Async())
        {
            const int top = lua_gettop(m_state);
            lua_pushcfunction(m_state, &KeepTraceback);
            if (luaL_loadbuffer(m_state, body.data(), body.size(), name.c_str()) != LUA_OK)
            {
                const std::string message = SafeText(m_state, -1);
                ConsoleEntry entry = ErrorEntry(m_state, -1, {}, inputId);
                entry.Code = ErrorName(ErrorCode::LuaSyntax);
                AddEntry(std::move(entry));
                lua_settop(m_state, top);
                if (done)
                    done(Status::Fail(ErrorCode::LuaSyntax, message));
                return inputId;
            }
            if (lua_pcall(m_state, 0, LUA_MULTRET, top + 1) != LUA_OK)
            {
                const int error = lua_gettop(m_state);
                lua_getfield(m_state, LUA_REGISTRYINDEX, kTracebackKey);
                std::string traceback = lua_type(m_state, -1) == LUA_TSTRING ? lua_tostring(m_state, -1) : std::string();
                lua_pop(m_state, 1);
                const std::string message = SafeText(m_state, error);
                AddEntry(ErrorEntry(m_state, error, std::move(traceback), inputId));
                lua_settop(m_state, top);
                if (done)
                    done(Status::Fail(ErrorCode::LuaRuntime, message.substr(0, message.find('\n'))));
                return inputId;
            }
            results(m_state, lua_gettop(m_state) - top - 1);
            lua_settop(m_state, top);
            if (done)
                done(Status::Success());
            return inputId;
        }
        const int launched = StartTask(body, chunk, [done](const Status& status, const std::string&)
            {
                if (done)
                    done(status);
            }, {}, {}, results, inputId);
        if (task)
            *task = launched;
        return inputId;
    }
}
