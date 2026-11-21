#include "Lua/AutorunModules.h"

#include <array>

namespace Manifold::Autorun
{
    namespace
    {
        constexpr std::array<ShippedModule, 2> kShipped = {
            ShippedModule{ "manifold-godot", "ce1cc44c488a29f992d0420b8444bc78fddb4ca7974fcd19b9a9f9b470970fcd" },
            ShippedModule{ "manifold-sigmaker", "a113a7a07848a7d612a6478052c1bea48c519ed81382ae796ce1fa25f71bf95b" },
        };
    }

    std::span<const ShippedModule> ShippedModules()
    {
        return std::span<const ShippedModule>(kShipped.data(), 2);
    }
}
