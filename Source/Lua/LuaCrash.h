#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterCrashNatives(lua_State* state, int natives);
}
