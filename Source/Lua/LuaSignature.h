#pragma once

struct lua_State;

namespace Manifold::Lua
{
    void RegisterSignatureNatives(lua_State* state, int natives);
}
