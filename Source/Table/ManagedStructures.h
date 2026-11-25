#pragma once

#include "Table/Structures.h"
#include "Target/Mono.h"

#include <map>
#include <string>
#include <vector>

namespace Manifold::Table
{
    using ManagedLayouts = std::map<std::string, std::vector<Target::MonoField>>;

    // pointerSize is the target's (4 in a 32-bit game): references and IntPtr fields are that wide.
    Scan::ValueType ManagedFieldType(int typeCode, int pointerSize, int& size, FieldDisplay& display);
    bool StoredInline(const Target::MonoField& field);
    void CollectManagedLayouts(Target::MonoBridge& mono, const std::vector<Target::MonoField>& fields, ManagedLayouts& nested, int depth = 0);
    void FillFromManaged(StructDefinition& target, std::vector<Target::MonoField> fields, int shift, const ManagedLayouts& nested, int pointerSize);
    // The fields of an array object: bounds, max_length and up to maxElements
    // elements, typed like a field of the element type.
    std::vector<Target::MonoField> ManagedArrayFields(const Target::MonoArrayLayout& array, int pointerSize, int maxElements);
    // "Game.Item[7]" for an array of 7 Game.Item.
    std::string ManagedArrayName(const Target::MonoArrayLayout& array);
    void AddManagedLayouts(StructureStore& store, const ManagedLayouts& nested, int shift, const std::string& skip, int pointerSize);
}
