#pragma once

struct lua_State;

namespace Manifold::Lua
{
    // The natives behind manifold.dotnet (Manifold.lua defines the schema).
    void RegisterDotNetNatives(lua_State* state, int natives);
}
