#include "App/Engine.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaEngine.h"
#include "Target/Mono.h"

#include <string>

#include "lua.hpp"

namespace Manifold::Lua
{
    namespace
    {
        Engine& Owner(lua_State* state) { return EngineOf(state).Owner(); }

        // All of these report "not Mono" by returning false/nil rather than
        // raising, so a table on a non-Mono game degrades quietly.
        int MonoInitialize(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).EnsureMono().Ok() ? 1 : 0);
            return 1;
        }

        int MonoIsPresent(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).MonoPresent() ? 1 : 0);
            return 1;
        }

        int MonoRefresh(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.MonoPresent() && !engine.EnsureMono())
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            lua_pushboolean(state, engine.Mono().Refresh().Ok() ? 1 : 0);
            return 1;
        }

        int MonoEnumClasses(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.MonoPresent())
                engine.EnsureMono();
            const std::vector<Target::MonoClass> classes = engine.Mono().Classes();
            lua_createtable(state, static_cast<int>(classes.size()), 0);
            int index = 1;
            for (const Target::MonoClass& info : classes)
            {
                lua_createtable(state, 0, 4);
                lua_pushstring(state, info.Namespace.c_str());
                lua_setfield(state, -2, "namespace");
                lua_pushstring(state, info.Name.c_str());
                lua_setfield(state, -2, "name");
                lua_pushstring(state, info.FullName().c_str());
                lua_setfield(state, -2, "fullname");
                lua_pushstring(state, info.Image.c_str());
                lua_setfield(state, -2, "image");
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        const Target::MonoClass* FindClass(Engine& engine, const std::string& name, std::vector<Target::MonoClass>& storage)
        {
            if (!engine.MonoPresent())
                engine.EnsureMono();
            storage = engine.Mono().Classes();
            for (const Target::MonoClass& info : storage)
            {
                if (info.Name == name || info.FullName() == name)
                    return &info;
            }
            return nullptr;
        }

        int MonoClassGetMethods(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = luaL_checkstring(state, 1);
            std::vector<Target::MonoClass> storage;
            const Target::MonoClass* info = FindClass(engine, name, storage);
            if (!info)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_createtable(state, static_cast<int>(info->Methods.size()), 0);
            int index = 1;
            for (const Target::MonoMethod& method : info->Methods)
            {
                lua_pushstring(state, method.Name.c_str());
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int MonoClassGetFieldOffset(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string klass = luaL_checkstring(state, 1);
            const std::string field = luaL_checkstring(state, 2);
            if (!engine.MonoPresent())
                engine.EnsureMono();
            const auto offset = engine.Mono().FieldOffset(klass, field);
            if (!offset)
                lua_pushnil(state);
            else
                lua_pushinteger(state, *offset);
            return 1;
        }

        int MonoGetAddress(lua_State* state)
        {
            Engine& engine = Owner(state);
            std::string name = luaL_checkstring(state, 1);
            if (lua_gettop(state) >= 2 && lua_isstring(state, 2))
                name += ":" + std::string(lua_tostring(state, 2));
            if (!engine.MonoPresent())
                engine.EnsureMono();
            const auto address = engine.Mono().AddressOf(name);
            if (!address)
                lua_pushnil(state);
            else
                lua_pushinteger(state, static_cast<lua_Integer>(*address));
            return 1;
        }

        const struct { const char* Name; lua_CFunction Call; } kMonoFunctions[] = {
            { "mono_initialize", &MonoInitialize },
            { "mono_isPresent", &MonoIsPresent },
            { "mono_refresh", &MonoRefresh },
            { "mono_enumClasses", &MonoEnumClasses },
            { "mono_class_getMethods", &MonoClassGetMethods },
            { "mono_class_getFieldOffset", &MonoClassGetFieldOffset },
            { "mono_getAddress", &MonoGetAddress },
        };
    }

    void RegisterMonoApi(LuaEngine&, lua_State* state)
    {
        for (const auto& function : kMonoFunctions)
        {
            lua_pushcfunction(state, function.Call);
            lua_setglobal(state, function.Name);
            NoteFunction(function.Name);
        }
    }
}
