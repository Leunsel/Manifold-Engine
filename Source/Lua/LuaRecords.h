#pragma once

struct lua_State;

namespace Manifold::Lua
{
    // natives behind manifold.record: the cheat table's memory records
    void RegisterRecordNatives(lua_State* state, int natives);
}
