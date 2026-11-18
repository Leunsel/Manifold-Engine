#pragma once

#include "Table/Structures.h"
#include "Target/Mono.h"

#include <map>
#include <string>
#include <vector>

namespace Manifold::Table
{
    using ManagedLayouts = std::map<std::string, std::vector<Target::MonoField>>;

    Scan::ValueType ManagedFieldType(int typeCode, int& size, FieldDisplay& display);
    bool StoredInline(const Target::MonoField& field);
    void CollectManagedLayouts(Target::MonoBridge& mono, const std::vector<Target::MonoField>& fields, ManagedLayouts& nested, int depth = 0);
    void FillFromManaged(StructDefinition& target, std::vector<Target::MonoField> fields, int shift, const ManagedLayouts& nested);
    void AddManagedLayouts(StructureStore& store, const ManagedLayouts& nested, int shift, const std::string& skip);
}
