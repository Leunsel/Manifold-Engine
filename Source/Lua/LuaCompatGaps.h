#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace Manifold::Lua
{
    class LuaEngine;

    enum class GapKind : std::uint8_t
    {
        NoEffect,
        Unavailable
    };

    enum class GapResult : std::uint8_t
    {
        Nothing,
        Nil,
        False,
        True,
        Zero,
        EmptyTable,
        Id,
        Script
    };

    struct GapReason
    {
        const char* Title;
        const char* Text;
    };

    struct CompatGap
    {
        const char* Name;
        GapKind Kind;
        GapResult Result;
        const GapReason* Reason;
        bool Quiet = false;
    };

    std::span<const CompatGap> CompatGaps();
    const CompatGap* FindCompatGap(std::string_view name);
    std::string CompatGapNote(const CompatGap& gap);
    std::string CompatGapsMarkdown();
    std::span<const char* const> CheatEngineMethodNames();

    void RegisterCompatGaps(LuaEngine& engine, lua_State* state);
    void RegisterCompatSystem(LuaEngine& engine, lua_State* state);

    std::optional<std::vector<std::uint8_t>> StreamData(lua_State* state, int index);
    bool SetStreamData(lua_State* state, int index, std::vector<std::uint8_t> data);
    bool LookupErrors(lua_State* state);
}
