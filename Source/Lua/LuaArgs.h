#pragma once

#include "Core/Errors.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace Manifold
{
    class Engine;
}

namespace Manifold::Lua
{
    Engine& Owner(lua_State* state);
    std::string Key(lua_State* state, int index);
    std::string TextArgument(lua_State* state, int index);

    std::uint64_t AddressArgument(lua_State* state, int index);
    std::optional<std::uint64_t> ResolveAddress(lua_State* state, int index, Status& error);
    std::optional<std::uint64_t> ResolveAddress(lua_State* state, int index);

    std::vector<std::uint8_t> ByteTable(lua_State* state, int index);
    std::vector<std::uint8_t> BytesFrom(lua_State* state, int first);

    void NoteOutsideAction(lua_State* state, std::string_view function, std::string_view what);

    std::string FailureMessage(const Status& status);
    int PushFailure(lua_State* state, const Status& status);
    int PushFailure(lua_State* state, ErrorCode code, std::string_view detail = {});
}
