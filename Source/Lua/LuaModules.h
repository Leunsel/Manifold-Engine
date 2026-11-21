#pragma once

#include <string_view>

struct lua_State;

namespace Manifold::Lua
{
    void RegisterModuleNatives(lua_State* state, int table);
    void TraceAutorunWrites(lua_State* state);
    void NoteAutorunWrite(lua_State* state, std::string_view function, std::string_view path);
}
