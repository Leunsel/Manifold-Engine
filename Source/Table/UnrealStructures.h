#pragma once

#include "Table/Structures.h"
#include "Target/Unreal.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Manifold::Table
{
    struct UnrealArrayRef
    {
        std::uint64_t Data = 0;
        Target::UnrealField Field;
        std::uint64_t Header = 0;
    };

    struct UnrealFill
    {
        std::vector<StructField> Fields;
        std::vector<UnrealArrayRef> Arrays;
        bool Handled = false;
    };

    using UnrealArrayLookup = std::function<std::optional<std::pair<Target::UnrealField, std::uint64_t>>(std::uint64_t data)>;

    UnrealFill FillFromUnreal(const Target::UnrealRuntime& runtime, std::uint64_t address, const UnrealArrayLookup& arrays, bool customTypes);
    UnrealFill FillUnrealObject(const Target::UnrealRuntime& runtime, std::uint64_t object, bool customTypes);
    std::optional<std::pair<std::string, std::uint64_t>> UnrealStructureName(const Target::UnrealRuntime& runtime, std::uint64_t address, bool containing,
        const UnrealArrayLookup& arrays);
}
