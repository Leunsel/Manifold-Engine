#include "App/Engine.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaEngine.h"
#include "Target/Mono.h"

#include <optional>
#include <string>

#include "lua.hpp"

namespace Manifold::Lua
{
    namespace
    {

        bool Cooperative(lua_State* state)
        {
            const Engine& engine = Owner(state);
            return engine.Async() && Manifold::OnMainThread() && lua_isyieldable(state) && EngineOf(state).IsCooperative(state);
        }

        template <lua_CFunction Body>
        int AfterMonoWork(lua_State* state, int, lua_KContext)
        {
            if (Owner(state).Managed().Loading())
                return lua_yieldk(state, 0, 0, &AfterMonoWork<Body>);
            return Body(state);
        }

        template <lua_CFunction Body>
        int WithMono(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.MonoPresent() || engine.Managed().Loading())
            {
                if (Cooperative(state))
                {
                    engine.Managed().Load(true);
                    if (engine.Managed().Loading())
                        return lua_yieldk(state, 0, 0, &AfterMonoWork<Body>);
                }
                else
                    engine.Managed().Ensure();
            }
            return Body(state);
        }

        // All of these report "not Mono" by returning false/nil rather than
        // raising, so a table on a non-Mono game degrades quietly.
        int MonoInitializeBody(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).MonoPresent() ? 1 : 0);
            return 1;
        }

        int MonoIsPresent(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).MonoPresent() ? 1 : 0);
            return 1;
        }

        // Whether the game runs IL2CPP: the same mono_* functions serve both,
        // as in Cheat Engine, but IL2CPP code is compiled ahead of time and
        // lives in GameAssembly.dll. Like mono_isPresent it starts nothing:
        // without an attached runtime the module list answers.
        int MonoIsIl2Cpp(lua_State* state)
        {
            Engine& engine = Owner(state);
            bool il2cpp = false;
            if (engine.MonoPresent())
                il2cpp = engine.Mono().IsIl2Cpp();
            else if (engine.Process())
                il2cpp = Target::MonoBridge::DetectRuntime(*engine.Process()) == Target::ManagedRuntime::Il2Cpp;
            lua_pushboolean(state, il2cpp ? 1 : 0);
            return 1;
        }

        int MonoRefreshBody(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).MonoPresent() ? 1 : 0);
            return 1;
        }

        int MonoRefresh(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (Cooperative(state) && engine.MonoPresent())
            {
                engine.Managed().Reload(true);
                if (engine.Managed().Loading())
                    return lua_yieldk(state, 0, 0, &AfterMonoWork<MonoRefreshBody>);
                return MonoRefreshBody(state);
            }
            if (!engine.MonoPresent())
                return WithMono<MonoRefreshBody>(state);
            lua_pushboolean(state, engine.Managed().ReloadNow().Ok() ? 1 : 0);
            return 1;
        }

        int MonoEnumClassesBody(lua_State* state)
        {
            Engine& engine = Owner(state);
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

        int MonoClassGetMethodsBody(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = luaL_checkstring(state, 1);
            // The members load on demand here, so a class nobody opened before
            // still lists its methods.
            const std::optional<Target::MonoClass> info = engine.Mono().ClassDetails(name);
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

        int MonoClassGetFieldOffsetBody(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string klass = luaL_checkstring(state, 1);
            const std::string field = luaL_checkstring(state, 2);
            const auto offset = engine.Mono().FieldOffset(klass, field);
            if (!offset)
                lua_pushnil(state);
            else
                lua_pushinteger(state, *offset);
            return 1;
        }

        int MonoShowDissector(lua_State* state)
        {
            UiRequest request{ UiRequestKind::Mono };
            if (lua_gettop(state) >= 1 && lua_type(state, 1) == LUA_TSTRING)
                request.Text = lua_tostring(state, 1);
            Owner(state).Request(std::move(request));
            return 0;
        }

        int MonoShutdown(lua_State* state)
        {
            Owner(state).Managed().Stop(true);
            return 0;
        }

        int MonoGetAddressBody(lua_State* state)
        {
            Engine& engine = Owner(state);
            std::string name = luaL_checkstring(state, 1);
            if (lua_gettop(state) >= 2 && lua_isstring(state, 2))
                name += ":" + std::string(lua_tostring(state, 2));
            const auto address = engine.Mono().AddressOf(name);
            if (!address)
                lua_pushnil(state);
            else
                lua_pushinteger(state, static_cast<lua_Integer>(*address));
            return 1;
        }

        const struct { const char* Name; lua_CFunction Call; } kMonoFunctions[] = {
            { "mono_initialize", &WithMono<MonoInitializeBody> },
            { "mono_isPresent", &MonoIsPresent },
            { "mono_isIl2Cpp", &MonoIsIl2Cpp },
            { "mono_refresh", &MonoRefresh },
            { "mono_enumClasses", &WithMono<MonoEnumClassesBody> },
            { "mono_class_getMethods", &WithMono<MonoClassGetMethodsBody> },
            { "mono_class_getFieldOffset", &WithMono<MonoClassGetFieldOffsetBody> },
            { "mono_getAddress", &WithMono<MonoGetAddressBody> },
            { "mono_showDissector", &MonoShowDissector },
            { "mono_shutdown", &MonoShutdown },
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
