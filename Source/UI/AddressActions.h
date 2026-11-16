#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    class Workspace;

    enum AddressAction : unsigned
    {
        ShowCode = 1u << 0,
        ShowHex = 1u << 1,
        DissectHere = 1u << 2,
        WatchAccesses = 1u << 3,
        WatchWrites = 1u << 4,
        AddToList = 1u << 5,
        CopyAddress = 1u << 6,
        CopyModuleOffset = 1u << 7,
        CodeActions = ShowCode | ShowHex | CopyAddress | CopyModuleOffset,
        DataActions = ShowHex | ShowCode | DissectHere | WatchAccesses | WatchWrites | AddToList | CopyAddress | CopyModuleOffset
    };

    std::string AddressText(Engine& engine, std::uint64_t address);
    bool AddressMenuItems(Workspace& workspace, std::uint64_t address, unsigned actions, int size = 4);
    bool AddressCell(Workspace& workspace, std::uint64_t address, bool selected, unsigned actions, bool code = true, const std::function<void()>& extra = {});

    struct AddressField
    {
        std::string Text;
        std::optional<std::uint64_t> Value;
        std::string Error;
        std::string Evaluated;
        std::uint64_t Generation = 0;
        double At = -1.0;
    };

    const std::optional<std::uint64_t>& Resolve(Engine& engine, AddressField& field);
    bool AddressInput(const char* id, Engine& engine, AddressField& field, float width, const char* hint = "address");
}
