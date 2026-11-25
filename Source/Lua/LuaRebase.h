#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterRebaseNatives(lua_State* state, int natives);
}
