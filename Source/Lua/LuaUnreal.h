#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterUnrealNatives(lua_State* state, int natives);
}
