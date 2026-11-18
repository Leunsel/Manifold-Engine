#include "Table/ManagedStructures.h"

#include <algorithm>

namespace Manifold::Table
{
    Scan::ValueType ManagedFieldType(int typeCode, int& size, FieldDisplay& display)
    {
        using V = Scan::ValueType;
        display = FieldDisplay::Unsigned;
        switch (typeCode)
        {
        case 0x04: size = 1; display = FieldDisplay::Signed; return V::Byte;
        case 0x02: case 0x05: size = 1; return V::Byte;
        case 0x06: size = 2; display = FieldDisplay::Signed; return V::TwoBytes;
        case 0x03: case 0x07: size = 2; return V::TwoBytes;
        case 0x08: size = 4; display = FieldDisplay::Signed; return V::FourBytes;
        case 0x09: size = 4; return V::FourBytes;
        case 0x0a: case 0x18: size = 8; display = FieldDisplay::Signed; return V::EightBytes;
        case 0x0b: case 0x19: size = 8; return V::EightBytes;
        case 0x0c: size = 4; return V::Float;
        case 0x0d: size = 8; return V::Double;
        default: size = 8; return V::Pointer;
        }
    }

    bool StoredInline(const Target::MonoField& field)
    {
        return field.ValueSize > 0 && field.EnumCode == 0 && field.TypeHandle && !field.TypeName.empty();
    }

    void CollectManagedLayouts(Target::MonoBridge& mono, const std::vector<Target::MonoField>& fields, ManagedLayouts& nested, int depth)
    {
        if (depth >= 6)
            return;
        for (const Target::MonoField& field : fields)
        {
            if (!StoredInline(field) || nested.count(field.TypeName))
                continue;
            std::vector<Target::MonoField> inner = mono.TypedFieldsOf(field.TypeHandle);
            nested[field.TypeName] = inner;
            CollectManagedLayouts(mono, inner, nested, depth + 1);
        }
    }

    void FillFromManaged(StructDefinition& target, std::vector<Target::MonoField> fields, int shift, const ManagedLayouts& nested)
    {
        std::stable_sort(fields.begin(), fields.end(), [](const Target::MonoField& a, const Target::MonoField& b) { return a.Offset < b.Offset; });
        target.Fields.clear();
        for (std::size_t index = 0; index < fields.size(); ++index)
        {
            const Target::MonoField& field = fields[index];
            StructField member;
            member.Offset = field.Offset - shift;
            member.Name = field.Name;
            if (field.EnumCode)
            {
                member.Type = ManagedFieldType(field.EnumCode, member.Size, member.Display);
            }
            else if (StoredInline(field))
            {
                member.Type = Scan::ValueType::ByteArray;
                member.Size = field.ValueSize;
                if (const auto found = nested.find(field.TypeName); found != nested.end() && !found->second.empty())
                    member.ChildStructure = field.TypeName;
            }
            else if (field.TypeCode == 0x11)
            {
                const int gap = index + 1 < fields.size() ? fields[index + 1].Offset - field.Offset : 8;
                member.Type = Scan::ValueType::ByteArray;
                member.Size = std::clamp(gap, 1, 256);
            }
            else
            {
                member.Type = ManagedFieldType(field.TypeCode, member.Size, member.Display);
            }
            target.Fields.push_back(std::move(member));
        }
    }

    void AddManagedLayouts(StructureStore& store, const ManagedLayouts& nested, int shift, const std::string& skip)
    {
        for (const auto& [name, inner] : nested)
        {
            if (inner.empty() || name == skip)
                continue;
            StructDefinition* existing = store.Find(name);
            if (existing && !existing->Fields.empty())
                continue;
            StructDefinition& child = existing ? *existing : store.Add(name);
            FillFromManaged(child, inner, shift, nested);
        }
    }
}
