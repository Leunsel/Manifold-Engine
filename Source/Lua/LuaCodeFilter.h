#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterCodeFilterNatives(lua_State* state, int natives);
}
