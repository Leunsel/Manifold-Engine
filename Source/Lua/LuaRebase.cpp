#include "Lua/LuaRebase.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaArgs.h"
#include "Target/Process.h"

#include "lua.hpp"

#include <string>

namespace Manifold::Lua
{
    namespace
    {
        std::shared_ptr<Target::Process> Opened(lua_State* state, Status& error)
        {
            const auto& process = Owner(state).Process();
            if (!process)
                error = Status::Fail(ErrorCode::NoProcess);
            else if (!process->Alive())
                error = Status::Fail(ErrorCode::ProcessExited);
            else
                return process;
            return nullptr;
        }

        void PushEntry(lua_State* state, Engine& engine, const Target::RebaseEntry& entry)
        {
            lua_createtable(state, 0, 3);
            lua_pushstring(state, entry.Module.c_str());
            lua_setfield(state, -2, "module");
            if (entry.Rule.ImageBase)
                lua_pushstring(state, "image");
            else
                lua_pushinteger(state, static_cast<lua_Integer>(entry.Rule.Base));
            lua_setfield(state, -2, "rule");
            const auto& process = engine.Process();
            if (process && process->Alive())
            {
                if (const auto module = process->FindModule(entry.Module))
                {
                    if (const auto base = engine.Symbols().Rebase().ShownBase(*process, *module))
                    {
                        lua_pushinteger(state, static_cast<lua_Integer>(*base));
                        lua_setfield(state, -2, "base");
                    }
                }
            }
        }

        int Set(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string module = luaL_checkstring(state, 1);
            if (lua_isnoneornil(state, 2))
                engine.SetRebase(module, std::nullopt);
            else if (lua_type(state, 2) == LUA_TSTRING)
                engine.SetRebase(module, Target::RebaseRule{ true, 0 });
            else
                engine.SetRebase(module, Target::RebaseRule{ false, static_cast<std::uint64_t>(luaL_checkinteger(state, 2)) });
            lua_pushboolean(state, 1);
            return 1;
        }

        int Get(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string module = luaL_checkstring(state, 1);
            const auto rule = engine.Symbols().Rebase().Rule(module);
            if (!rule)
            {
                lua_pushnil(state);
                return 1;
            }
            std::string name = module;
            for (const Target::RebaseEntry& entry : engine.Symbols().Rebase().Entries())
            {
                if (EqualsNoCase(entry.Module, module))
                    name = entry.Module;
            }
            PushEntry(state, engine, { name, *rule });
            return 1;
        }

        int List(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::vector<Target::RebaseEntry> entries = engine.Symbols().Rebase().Entries();
            lua_createtable(state, static_cast<int>(entries.size()), 0);
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                PushEntry(state, engine, entries[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int Shown(lua_State* state)
        {
            Status error;
            if (!Opened(state, error))
                return PushFailure(state, error);
            const auto address = ResolveAddress(state, 1, error);
            if (!address)
                return PushFailure(state, error);
            lua_pushinteger(state, static_cast<lua_Integer>(Owner(state).Symbols().Rebased(*address).value_or(*address)));
            return 1;
        }

        int Real(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const auto shown = static_cast<std::uint64_t>(luaL_checkinteger(state, 1));
            const auto hit = Owner(state).Symbols().FromRebased(shown);
            lua_pushinteger(state, static_cast<lua_Integer>(hit ? hit->Address : shown));
            return 1;
        }

        int ImageBase(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const std::string name = luaL_checkstring(state, 1);
            const auto module = process->FindModule(name);
            if (!module)
                return PushFailure(state, ErrorCode::ModuleNotFound, name);
            const auto base = Owner(state).Symbols().Rebase().ImageBase(*process, *module);
            if (!base)
                return PushFailure(state, ErrorCode::ReadFailed, name + " PE header");
            lua_pushinteger(state, static_cast<lua_Integer>(*base));
            return 1;
        }
    }

    void RegisterRebaseNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "rebaseSet", &Set },
            { "rebaseGet", &Get },
            { "rebaseList", &List },
            { "rebaseShown", &Shown },
            { "rebaseReal", &Real },
            { "rebaseImageBase", &ImageBase },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
