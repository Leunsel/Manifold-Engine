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

    // How many fields one Unreal fill writes at most; a fill that stops there says so (Truncated) and can be run
    // again with a higher limit.
    inline constexpr int kUnrealFieldLimit = 6000;

    struct UnrealFill
    {
        std::vector<StructField> Fields;
        // The fill or one of the structures it embeds stopped at the limit; the same fill with a higher limit
        // (Load More) reads on.
        bool Truncated = false;
        // A property chain (of the class or of an embedded structure) broke before its end: the definition is
        // incomplete, and a higher limit does not change that.
        bool Incomplete = false;
        std::vector<UnrealArrayRef> Arrays;
        // The structures that StructProperty fields embed (ChildStructure = the struct's C++ name), with their own
        // fields counted from 0; nested structs inside them are listed here as well.
        std::vector<std::pair<std::string, std::vector<StructField>>> Structs;
        // Names from Structs whose definition is cut (Truncated or Incomplete), so the user can be told which.
        std::vector<std::string> CutStructs;
        bool Handled = false;
    };

    // Adds the structures a fill embeds to the store: a structure of that name that already has fields is kept,
    // and `skip` (the structure being filled) is never replaced. Names in `cut` (the CutStructs of the fill that
    // wrote them) are replaced by a longer definition whose first rows they still are, as after Load More.
    void AddUnrealStructures(StructureStore& store, const UnrealFill& fill, const std::string& skip, const std::vector<std::string>& cut = {});

    using UnrealArrayLookup = std::function<std::optional<std::pair<Target::UnrealField, std::uint64_t>>(std::uint64_t data)>;

    UnrealFill FillFromUnreal(const Target::UnrealRuntime& runtime, std::uint64_t address, const UnrealArrayLookup& arrays, bool customTypes, int limit = kUnrealFieldLimit);
    UnrealFill FillUnrealObject(const Target::UnrealRuntime& runtime, std::uint64_t object, bool customTypes, int limit = kUnrealFieldLimit);
    std::optional<std::pair<std::string, std::uint64_t>> UnrealStructureName(const Target::UnrealRuntime& runtime, std::uint64_t address, bool containing,
        const UnrealArrayLookup& arrays);
}
