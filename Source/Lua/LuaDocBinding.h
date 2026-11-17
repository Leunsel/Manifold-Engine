#pragma once

#include "Lua/DocRegistry.h"

#include <memory>
#include <string>
#include <string_view>

struct lua_State;

namespace Manifold::Lua
{
    class LuaEngine;

    std::shared_ptr<const DocRegistry> BuiltinDocs();

    void PushIndexed(lua_State* state, int object, std::string_view key);
    bool PushChain(lua_State* state, std::string_view chain);
    std::string ClassNameOf(lua_State* state, int index);

    void BindDoc(lua_State* state, int index, std::string_view path);
    std::string BoundDocPath(lua_State* state, int index);
    DocPtr AnnotatedDoc(LuaEngine& engine, lua_State* state, int index);
    DocPtr ReflectedDoc(lua_State* state, int index, std::string_view path);
    DocPtr DocOfValue(LuaEngine& engine, lua_State* state, int index, std::string_view path, bool reflect);

    void PushDocEntry(lua_State* state, const DocRecord& record);
    void RegisterDocNatives(lua_State* state, int natives);
}
