#include "Target/ManagedDumper.h"

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint8_t kImage[1] = { 0 };
    }

    std::span<const std::uint8_t> ManagedDumperImage()
    {
        return std::span<const std::uint8_t>(kImage, 0);
    }
}
