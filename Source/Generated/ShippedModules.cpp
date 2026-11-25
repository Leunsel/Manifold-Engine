#include "Lua/AutorunModules.h"

#include <array>

namespace Manifold::Autorun
{
    namespace
    {
        constexpr std::array<ShippedModule, 1> kShipped = {
            ShippedModule{ "manifold-godot", "528415bafad814d1996e26664df865cda40d6389b44e2f3d6caf5fbcc5f38fe2" },
        };
    }

    std::span<const ShippedModule> ShippedModules()
    {
        return std::span<const ShippedModule>(kShipped.data(), 1);
    }
}
