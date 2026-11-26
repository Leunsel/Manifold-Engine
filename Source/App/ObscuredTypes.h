#pragma once

namespace Manifold::Lua
{
    class LuaEngine;
}

namespace Manifold
{
    // The ACTk value types (Scan/Obscured.h) as custom types: the address list, Structure Dissect and the
    // results show the plain value, and a write puts a new one under the key already in memory.
    void RegisterObscuredTypes(Lua::LuaEngine& lua, int pointerSize);
}
