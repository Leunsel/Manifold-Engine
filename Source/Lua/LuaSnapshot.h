#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace Manifold::Lua
{
    struct SnapshotNode
    {
        std::string Key;
        std::string Type;
        std::string Value;
        std::string Label;
        std::int64_t Integer = 0;
        bool IsInteger = false;
        bool IsAddress = false;
        bool Cycle = false;
        std::uint8_t Depth = 0;
        std::uint32_t Subtree = 0;
        std::uint32_t Total = 0;
        std::uint32_t Shown = 0;
    };

    struct ValueSnapshot
    {
        std::vector<SnapshotNode> Nodes;
        bool Truncated = false;

        std::size_t Bytes() const;
        std::string Summary(bool hex) const;
    };

    struct SnapshotLimits
    {
        int Depth = 4;
        int Children = 256;
        int Nodes = 4000;
        double LabelSeconds = 0.05;
        int AddressProbes = 64;
    };

    using AddressProbe = std::function<bool(std::uint64_t address)>;

    std::shared_ptr<const ValueSnapshot> TakeSnapshot(lua_State* state, int index, const SnapshotLimits& limits, const AddressProbe& probe = {});
    std::string NodeValueText(const SnapshotNode& node, bool hex);
    std::string QuoteLuaString(std::string_view text, std::size_t limit);
}
