#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterUiNatives(lua_State* state, int natives);
}
