#pragma once

struct lua_State;

namespace Manifold::Lua
{
    // uiTheme* natives for manifold.ui.theme
    void RegisterThemeNatives(lua_State* state, int natives);
}
