#include "Table/ManagedStructures.h"

#include <algorithm>

namespace Manifold::Table
{
    Scan::ValueType ManagedFieldType(int typeCode, int pointerSize, int& size, FieldDisplay& display)
    {
        using V = Scan::ValueType;
        display = FieldDisplay::Unsigned;
        // ∑ References and IntPtr took 8 bytes in a 32-bit game too (Terraria): the dissect showed
        //   two neighbouring pointers as one value and a table made from it read the wrong bytes.
        const int pointer = pointerSize == 4 ? 4 : 8;
        switch (typeCode)
        {
        case 0x04: size = 1; display = FieldDisplay::Signed; return V::Byte;
        case 0x02: case 0x05: size = 1; return V::Byte;
        case 0x06: size = 2; display = FieldDisplay::Signed; return V::TwoBytes;
        case 0x03: case 0x07: size = 2; return V::TwoBytes;
        case 0x08: size = 4; display = FieldDisplay::Signed; return V::FourBytes;
        case 0x09: size = 4; return V::FourBytes;
        case 0x0a: size = 8; display = FieldDisplay::Signed; return V::EightBytes;
        case 0x0b: size = 8; return V::EightBytes;
        // ∑ System.IntPtr / UIntPtr (ELEMENT_TYPE_I / U) hold native pointers,
        // UnityEngine.Object.m_CachedPtr above all, and are pointer sized in a
        // 32-bit game too.
        case 0x18: case 0x19: size = pointer; return V::Pointer;
        case 0x0c: size = 4; return V::Float;
        case 0x0d: size = 8; return V::Double;
        default: size = pointer; return V::Pointer;
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

    void FillFromManaged(StructDefinition& target, std::vector<Target::MonoField> fields, int shift, const ManagedLayouts& nested, int pointerSize)
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
                member.Type = ManagedFieldType(field.EnumCode, pointerSize, member.Size, member.Display);
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
                const int gap = index + 1 < fields.size() ? fields[index + 1].Offset - field.Offset : pointerSize;
                member.Type = Scan::ValueType::ByteArray;
                member.Size = std::clamp(gap, 1, 256);
            }
            else
            {
                member.Type = ManagedFieldType(field.TypeCode, pointerSize, member.Size, member.Display);
            }
            target.Fields.push_back(std::move(member));
        }
    }

    std::vector<Target::MonoField> ManagedArrayFields(const Target::MonoArrayLayout& array, int pointerSize, int maxElements)
    {
        // The header after the class and monitor words, then one field per
        // element, named [0], [1], ... like Cheat Engine's array view.
        std::vector<Target::MonoField> fields;
        Target::MonoField bounds;
        bounds.Name = "bounds";
        bounds.Offset = 2 * pointerSize;
        bounds.TypeCode = 0x18;
        fields.push_back(bounds);
        Target::MonoField length;
        length.Name = "max_length";
        length.Offset = 3 * pointerSize;
        length.TypeCode = pointerSize == 4 ? 0x09 : 0x0b;
        fields.push_back(length);
        const std::uint64_t count = std::min<std::uint64_t>(array.Length, static_cast<std::uint64_t>(std::max(0, maxElements)));
        const int size = std::max(1, array.ElementSize);
        for (std::uint64_t index = 0; index < count; ++index)
        {
            const std::int64_t offset = array.DataOffset + static_cast<std::int64_t>(index) * size;
            if (offset > kMaxFieldOffset)
                break;
            Target::MonoField element = array.Element;
            element.Name = "[" + std::to_string(index) + "]";
            element.Offset = static_cast<int>(offset);
            fields.push_back(std::move(element));
        }
        return fields;
    }

    std::string ManagedArrayName(const Target::MonoArrayLayout& array)
    {
        // The element count goes into the name: two arrays of one type but of
        // different lengths need structures of their own.
        std::string name = array.Name;
        if (name.size() >= 2 && name.compare(name.size() - 2, 2, "[]") == 0)
            name.resize(name.size() - 2);
        return name + "[" + std::to_string(array.Length) + "]";
    }

    void AddManagedLayouts(StructureStore& store, const ManagedLayouts& nested, int shift, const std::string& skip, int pointerSize)
    {
        for (const auto& [name, inner] : nested)
        {
            if (inner.empty() || name == skip)
                continue;
            StructDefinition* existing = store.Find(name);
            if (existing && !existing->Fields.empty())
                continue;
            StructDefinition& child = existing ? *existing : store.Add(name);
            FillFromManaged(child, inner, shift, nested, pointerSize);
        }
    }
}
