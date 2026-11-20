#pragma once

#include "Table/Structures.h"
#include "Target/Rtti.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Manifold::Table
{
    inline constexpr int kRttiGuessSize = 0x1000;
    inline constexpr int kRttiMemberLimit = 0x200;
    inline constexpr int kRttiMemberDepth = 3;

    struct RttiName
    {
        std::string Name;
        std::uint64_t Start = 0;
        bool Object = false;
    };

    struct RttiFill
    {
        bool Handled = false;
        bool Object = false;
        int Labelled = 0;
        std::vector<StructField> Fields;
        std::vector<StructDefinition> Children;
    };

    std::set<std::string> StructureNames(const StructureStore& store);
    std::optional<RttiName> RttiStructureName(Target::RttiRuntime& runtime, std::uint64_t address);
    RttiFill FillRttiObject(Target::RttiRuntime& runtime, const Target::RttiObject& object, int size, const std::set<std::string>& existing);
    RttiFill FillFromRtti(Target::RttiRuntime& runtime, std::uint64_t address, int size, const std::set<std::string>& existing);
    RttiFill LabelFromRtti(Target::RttiRuntime& runtime, std::vector<StructField> fields, std::uint64_t address, const std::set<std::string>& existing);
    void ApplyRttiFill(StructureStore& store, StructDefinition& definition, RttiFill fill);
}
