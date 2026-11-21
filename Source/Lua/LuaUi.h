#pragma once

struct lua_State;

namespace Manifold::Lua
{
    struct UiNode;

    void RegisterUiNatives(lua_State* state, int natives);
    void ApplyUiProps(lua_State* state, int props, UiNode& node);
}
