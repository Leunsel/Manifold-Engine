#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterRttiNatives(lua_State* state, int natives);
}
