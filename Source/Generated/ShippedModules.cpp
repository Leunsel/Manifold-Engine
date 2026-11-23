#include "Lua/AutorunModules.h"

#include <array>

namespace Manifold::Autorun
{
    namespace
    {
        constexpr std::array<ShippedModule, 1> kShipped = {
            ShippedModule{ "manifold-godot", "ce1cc44c488a29f992d0420b8444bc78fddb4ca7974fcd19b9a9f9b470970fcd" },
        };
    }

    std::span<const ShippedModule> ShippedModules()
    {
        return std::span<const ShippedModule>(kShipped.data(), 1);
    }
}
