#include "Table/UnrealStructures.h"

#include "Core/Text.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace Manifold::Table
{
    namespace
    {
        constexpr int kInlineDepth = 6;
        constexpr int kStaticArrayLimit = 64;
        constexpr int kGapLimit = 0x1000;
        constexpr int kArrayPreview = 64;
        constexpr int kArrayPreviewElements = 1024;

        using Scan::ValueType;

        struct Spec
        {
            int Offset = 0;
            std::string Name;
            ValueType Type = ValueType::FourBytes;
            int Size = 4;
            FieldDisplay Display = FieldDisplay::Unsigned;
            std::string Custom;
            std::string Child;
            int Bit = -1;
            int Order = 0;
            int Width = 8;
        };

        using NestedStructs = std::vector<std::pair<std::string, std::vector<StructField>>>;

        int SizeOf(ValueType type, int bytes, int pointer)
        {
            switch (type)
            {
            case ValueType::Byte: return 1;
            case ValueType::TwoBytes: return 2;
            case ValueType::FourBytes:
            case ValueType::Float: return 4;
            case ValueType::EightBytes:
            case ValueType::Double: return 8;
            case ValueType::Pointer: return pointer;
            default: return std::max(1, bytes);
            }
        }

        ValueType BySize(int size)
        {
            switch (size)
            {
            case 1: return ValueType::Byte;
            case 2: return ValueType::TwoBytes;
            case 4: return ValueType::FourBytes;
            case 8: return ValueType::EightBytes;
            default: return ValueType::ByteArray;
            }
        }

        std::string Annotated(const std::string& name, const std::string& text)
        {
            return text.empty() ? name : name + "  (" + text + ")";
        }

        std::optional<int> MaskBit(std::uint8_t mask)
        {
            if (mask == 0 || (mask & (mask - 1)) != 0)
                return std::nullopt;
            int bit = 0;
            while (mask > 1)
            {
                mask = static_cast<std::uint8_t>(mask >> 1);
                ++bit;
            }
            return bit;
        }

        class Filler
        {
        public:
            Filler(const Target::UnrealRuntime& runtime, const Target::Process& process, std::uint64_t base, bool customTypes, NestedStructs& nested,
                int limit = kUnrealFieldLimit)
                : m_runtime(runtime), m_process(process), m_base(base), m_custom(customTypes), m_pointer(runtime.PointerSize()), m_limit(std::max(1, limit)), m_nested(nested)
            {
            }

            int Count() const { return m_count; }
            bool Truncated() const { return m_truncated; }
            std::vector<UnrealArrayRef>& Arrays() { return m_arrays; }

            void Add(int offset, std::string name, ValueType type, int width, FieldDisplay display = FieldDisplay::Unsigned, std::string custom = {}, int bit = -1,
                std::string child = {})
            {
                if (m_count >= m_limit)
                {
                    m_truncated = true;
                    return;
                }
                ++m_count;
                Spec spec;
                spec.Offset = offset;
                spec.Name = std::move(name);
                spec.Type = type;
                spec.Size = SizeOf(type, width, m_pointer);
                spec.Display = display;
                spec.Custom = m_custom ? std::move(custom) : std::string();
                spec.Child = std::move(child);
                spec.Bit = bit;
                spec.Order = m_count;
                spec.Width = std::max(1, width);
                m_specs.push_back(std::move(spec));
            }

            // ∑ A struct member is one embedded field (like a Mono value type), not its members spread out with
            //   dotted names; the struct's own definition goes into m_nested once per name.
            void Embed(const Target::UnrealField& f, int offset, const std::string& name, int depth)
            {
                const std::string child = m_runtime.CppName(f.Inner);
                Add(offset, name, ValueType::ByteArray, std::clamp(f.Size, 1, kMaxFieldBytes), FieldDisplay::Unsigned, {}, -1, child);
                if (m_base)
                    CollectArrays(f.Inner, offset, depth + 1);
                if (depth >= kInlineDepth)
                    return;
                const auto known = std::find_if(m_nested.begin(), m_nested.end(), [&](const auto& entry) { return entry.first == child; });
                if (known != m_nested.end())
                    return;
                const std::size_t slot = m_nested.size();
                m_nested.push_back({ child, {} });
                Filler inner(m_runtime, m_process, 0, m_custom, m_nested);
                inner.Struct(f.Inner, 0, "", depth + 1);
                inner.Gaps(m_runtime.StructSize(f.Inner));
                m_nested[slot].second = inner.Flush();
            }

            // TArrays inside embedded structs, at their place in this object, so expanding their data still works.
            void CollectArrays(std::uint64_t type, int base, int depth)
            {
                for (const Target::UnrealField& f : m_runtime.Fields(type))
                {
                    const int dim = std::min(f.Dim, kStaticArrayLimit);
                    for (int i = 0; i < dim; ++i)
                    {
                        const int at = base + f.Offset + i * std::max(1, f.Size);
                        if (f.Type == "ArrayProperty" && f.InnerProperty)
                        {
                            const std::uint64_t header = m_base + static_cast<std::uint64_t>(at);
                            const auto data = m_process.ReadPointer(header);
                            if (data && m_runtime.IsPointer(*data))
                                m_arrays.push_back({ *data, f, header });
                        }
                        else if (f.Type == "StructProperty" && f.Inner && depth < kInlineDepth)
                        {
                            CollectArrays(f.Inner, at, depth + 1);
                        }
                    }
                }
            }

            void Value(const Target::UnrealField& f, int offset, const std::string& name, int depth)
            {
                const std::string& kind = f.Type;
                struct Scalar
                {
                    ValueType Type;
                    bool Signed;
                };
                static const std::unordered_map<std::string, Scalar> scalars = { { "FloatProperty", { ValueType::Float, false } }, { "DoubleProperty", { ValueType::Double, false } },
                    { "LargeWorldCoordinatesRealProperty", { ValueType::Double, false } }, { "Int8Property", { ValueType::Byte, true } }, { "ByteProperty", { ValueType::Byte, false } }, { "Int16Property", { ValueType::TwoBytes, true } },
                    { "UInt16Property", { ValueType::TwoBytes, false } }, { "IntProperty", { ValueType::FourBytes, true } }, { "UInt32Property", { ValueType::FourBytes, false } },
                    { "Int64Property", { ValueType::EightBytes, true } }, { "UInt64Property", { ValueType::EightBytes, false } } };
                if (const auto it = scalars.find(kind); it != scalars.end())
                {
                    const std::string text = kind == "ByteProperty" && f.Inner ? m_runtime.CppName(f.Inner) : std::string();
                    Add(offset, Annotated(name, text), it->second.Type, std::max(1, f.Size), it->second.Signed ? FieldDisplay::Signed : FieldDisplay::Unsigned);
                    return;
                }
                if (kind == "BoolProperty")
                {
                    if (f.MaskedBit())
                    {
                        const auto bit = MaskBit(f.Mask);
                        if (bit)
                            Add(offset + f.ByteOffset, name, ValueType::Binary, 1, FieldDisplay::Unsigned, {}, *bit);
                        else
                            Add(offset + f.ByteOffset, Annotated(name, Format("bit mask %02X", f.Mask)), ValueType::Byte, 1, FieldDisplay::Hex);
                        return;
                    }
                    Add(offset, name, BySize(f.Size) == ValueType::ByteArray ? ValueType::Byte : BySize(f.Size), std::max(1, f.Size));
                    return;
                }
                if (kind == "EnumProperty")
                {
                    const ValueType type = BySize(f.Size) == ValueType::ByteArray ? ValueType::Byte : BySize(f.Size);
                    Add(offset, Annotated(name, f.Inner ? m_runtime.CppName(f.Inner) : std::string()), type, std::max(1, f.Size));
                    return;
                }
                if (kind == "NameProperty")
                {
                    Add(offset, name, m_custom ? ValueType::ByteArray : ValueType::EightBytes, 8, FieldDisplay::Hex, Target::kUnrealFNameType);
                    return;
                }
                if (kind == "StrProperty")
                {
                    if (m_custom)
                    {
                        Add(offset, name, ValueType::ByteArray, m_pointer + 8, FieldDisplay::Unsigned, Target::kUnrealFStringType);
                    }
                    else
                    {
                        Add(offset, name, ValueType::Pointer, m_pointer);
                        Add(offset + m_pointer, name + ".Num", ValueType::FourBytes, 4, FieldDisplay::Signed);
                    }
                    return;
                }
                if (Target::IsObjectProperty(kind) || kind == "InterfaceProperty")
                {
                    Add(offset, Annotated(name, m_runtime.TypeText(f)), ValueType::Pointer, m_pointer);
                    return;
                }
                if (kind == "WeakObjectProperty" || kind == "LazyObjectProperty" || kind == "SoftObjectProperty" || kind == "SoftClassProperty")
                {
                    Add(offset, Annotated(name, m_runtime.TypeText(f)), m_custom ? ValueType::ByteArray : ValueType::FourBytes, m_custom ? 8 : 4, FieldDisplay::Unsigned,
                        Target::kUnrealObjectIndexType);
                    return;
                }
                if (kind == "DelegateProperty")
                {
                    Add(offset, Annotated(name + ".Object", "FScriptDelegate"), m_custom ? ValueType::ByteArray : ValueType::FourBytes, m_custom ? 8 : 4, FieldDisplay::Unsigned,
                        Target::kUnrealObjectIndexType);
                    Add(offset + 8, name + ".FunctionName", m_custom ? ValueType::ByteArray : ValueType::EightBytes, 8, FieldDisplay::Hex, Target::kUnrealFNameType);
                    return;
                }
                if (kind == "ArrayProperty" || kind == "MulticastInlineDelegateProperty" || kind == "MulticastDelegateProperty")
                {
                    if (kind == "ArrayProperty" && m_base && f.InnerProperty)
                    {
                        const std::uint64_t header = m_base + static_cast<std::uint64_t>(offset);
                        const auto data = m_process.ReadPointer(header);
                        if (data && m_runtime.IsPointer(*data))
                            m_arrays.push_back({ *data, f, header });
                    }
                    Add(offset, Annotated(name, m_runtime.TypeText(f)), ValueType::Pointer, m_pointer);
                    Add(offset + m_pointer, name + ".Num", ValueType::FourBytes, 4, FieldDisplay::Signed);
                    Add(offset + m_pointer + 4, name + ".Max", ValueType::FourBytes, 4, FieldDisplay::Signed);
                    return;
                }
                if (kind == "SetProperty" || kind == "MapProperty")
                {
                    Add(offset, Annotated(name + ".Elements", m_runtime.TypeText(f)), ValueType::Pointer, m_pointer);
                    Add(offset + m_pointer, name + ".Num", ValueType::FourBytes, 4, FieldDisplay::Signed);
                    return;
                }
                if (kind == "TextProperty")
                {
                    Add(offset, Annotated(name, "FText"), ValueType::Pointer, m_pointer);
                    return;
                }
                if (kind == "StructProperty" && f.Inner)
                {
                    Embed(f, offset, name, depth);
                    return;
                }
                const int size = std::max(1, f.Size);
                const ValueType type = BySize(size);
                if (type != ValueType::ByteArray)
                    Add(offset, Annotated(name, m_runtime.TypeText(f)), type, size, FieldDisplay::Hex);
                else
                    Add(offset, Annotated(name, m_runtime.TypeText(f)), ValueType::ByteArray, std::min(size, 256));
            }

            void Field(const Target::UnrealField& f, int base, const std::string& prefix, int depth)
            {
                const int dim = std::min(f.Dim, kStaticArrayLimit);
                for (int i = 0; i < dim; ++i)
                {
                    const std::string name = prefix + f.Name + (f.Dim > 1 ? "[" + std::to_string(i) + "]" : std::string());
                    Value(f, base + f.Offset + i * std::max(1, f.Size), name, depth);
                }
            }

            void Struct(std::uint64_t type, int base, const std::string& prefix, int depth)
            {
                for (const Target::UnrealField& f : m_runtime.Fields(type))
                {
                    if (m_count >= m_limit)
                    {
                        m_truncated = true;
                        return;
                    }
                    Field(f, base, prefix, depth);
                }
            }

            void Gaps(std::optional<int> size)
            {
                std::vector<std::pair<int, int>> ranges;
                for (const Spec& spec : m_specs)
                    ranges.push_back({ spec.Offset, spec.Offset + spec.Width });
                std::sort(ranges.begin(), ranges.end());
                std::vector<std::pair<int, int>> gaps;
                int cursor = 0;
                for (const auto& [from, to] : ranges)
                {
                    if (from > cursor)
                        gaps.push_back({ cursor, from });
                    cursor = std::max(cursor, to);
                }
                if (size && *size > cursor)
                    gaps.push_back({ cursor, *size });
                std::vector<std::uint8_t> live;
                for (const auto& [from, to] : gaps)
                {
                    const int stop = std::min(to, from + kGapLimit);
                    const int word = m_pointer;
                    live.assign(static_cast<std::size_t>(std::max(0, std::min(to, stop + word) - from)), 0);
                    const bool read = m_base && !live.empty() && m_process.ReadExact(m_base + static_cast<std::uint64_t>(from), live.data(), live.size());
                    if (m_count >= m_limit && from < stop)
                        m_truncated = true;
                    for (int at = from; at < stop && m_count < m_limit;)
                    {
                        if (at % word == 0 && at + word <= to)
                        {
                            std::optional<std::uint64_t> value;
                            if (read && static_cast<std::size_t>(at - from + word) <= live.size())
                            {
                                std::uint64_t raw = 0;
                                std::memcpy(&raw, live.data() + (at - from), static_cast<std::size_t>(word));
                                value = raw;
                            }
                            else if (m_base)
                            {
                                value = m_process.ReadPointer(m_base + static_cast<std::uint64_t>(at));
                            }
                            const bool pointer = value && m_runtime.IsPointer(*value);
                            Add(at, "(not reflected)", pointer ? ValueType::Pointer : BySize(word), word, pointer ? FieldDisplay::Unsigned : FieldDisplay::Hex);
                            at += word;
                        }
                        else
                        {
                            const int step = at % 4 == 0 && at + 4 <= to ? 4 : 1;
                            Add(at, "(not reflected)", step == 4 ? ValueType::FourBytes : ValueType::Byte, step, FieldDisplay::Hex);
                            at += step;
                        }
                    }
                }
            }

            void Header()
            {
                const Target::UnrealLayout& l = m_runtime.Layout();
                const Target::UnrealHeaderNames names = m_runtime.HeaderNames();
                // ∑ UE3's Object.uc declares the header itself (VfTableObject, ObjectFlags, Outer, Name, Class ...) as
                //   script properties; where the reflection has a field, it is not added a second time.
                std::vector<std::pair<int, int>> reflected;
                if (l.Ue3)
                {
                    for (const Target::UnrealField& f : m_runtime.Fields(m_runtime.Type("Object"), true))
                        reflected.push_back({ f.Start(), std::max(1, f.Size * std::max(1, f.Dim)) });
                }
                const auto add = [&](int offset, const char* name, ValueType type, int width, FieldDisplay display = FieldDisplay::Unsigned, std::string custom = {})
                    {
                        for (const auto& [start, size] : reflected)
                        {
                            if (offset < start + size && start < offset + width)
                                return;
                        }
                        Add(offset, name, type, width, display, std::move(custom));
                    };
                add(0, names.VTable, ValueType::Pointer, m_pointer);
                if (!l.Ue3 && l.Index == m_pointer + 4)
                    add(m_pointer, "ObjectFlags", ValueType::FourBytes, 4, FieldDisplay::Hex);
                add(l.Index, names.Index, ValueType::FourBytes, 4, FieldDisplay::Signed);
                add(l.Class, names.Class, ValueType::Pointer, m_pointer);
                add(l.Name, names.Name, m_custom ? ValueType::ByteArray : ValueType::EightBytes, 8, FieldDisplay::Hex, Target::kUnrealFNameType);
                add(l.Outer, names.Outer, ValueType::Pointer, m_pointer);
            }

            std::vector<StructField> Flush()
            {
                std::stable_sort(m_specs.begin(), m_specs.end(), [](const Spec& a, const Spec& b)
                    {
                        if (a.Offset != b.Offset)
                            return a.Offset < b.Offset;
                        return a.Order < b.Order;
                    });
                std::vector<StructField> fields;
                fields.reserve(m_specs.size());
                for (const Spec& spec : m_specs)
                {
                    StructField field;
                    field.Offset = spec.Offset;
                    field.Name = spec.Name;
                    field.Type = spec.Type;
                    field.Size = spec.Size;
                    field.Display = spec.Display;
                    field.ChildStructure = spec.Child;
                    if (!spec.Custom.empty())
                    {
                        field.CustomType = spec.Custom;
                        field.Type = ValueType::ByteArray;
                        field.Size = spec.Width;
                    }
                    else if (spec.Bit >= 0)
                    {
                        field.Type = ValueType::Binary;
                        field.BitStart = spec.Bit;
                        field.BitLength = 1;
                        field.Size = BitFieldSize(spec.Bit, 1);
                    }
                    fields.push_back(std::move(field));
                }
                m_specs.clear();
                return fields;
            }

        private:
            const Target::UnrealRuntime& m_runtime;
            const Target::Process& m_process;
            std::uint64_t m_base = 0;
            bool m_custom = true;
            int m_pointer = 8;
            int m_limit = kUnrealFieldLimit;
            int m_count = 0;
            bool m_truncated = false;
            std::vector<Spec> m_specs;
            std::vector<UnrealArrayRef> m_arrays;
            NestedStructs& m_nested;
        };

        UnrealFill Finish(Filler& filler, NestedStructs& nested)
        {
            UnrealFill fill;
            fill.Fields = filler.Flush();
            fill.Arrays = std::move(filler.Arrays());
            fill.Structs = std::move(nested);
            fill.Truncated = filler.Truncated();
            fill.Handled = !fill.Fields.empty();
            return fill;
        }

        UnrealFill FillStruct(const Target::UnrealRuntime& runtime, const Target::Process& process, std::uint64_t type, std::uint64_t base, bool isObject, bool customTypes,
            int limit)
        {
            NestedStructs nested;
            Filler filler(runtime, process, base, customTypes, nested, limit);
            if (isObject)
                filler.Header();
            filler.Struct(type, 0, "", 0);
            filler.Gaps(runtime.StructSize(type));
            return Finish(filler, nested);
        }

        UnrealFill FillArray(const Target::UnrealRuntime& runtime, const Target::Process& process, const Target::UnrealField& array, std::uint64_t header, bool customTypes)
        {
            const auto info = runtime.Describe(array.InnerProperty, array.InnerType, {});
            if (!info)
                return {};
            const int size = std::max(1, info->Size);
            int perEntry = 1;
            if (info->Type == "StructProperty" && info->Inner)
                perEntry = std::max<int>(1, static_cast<int>(runtime.Fields(info->Inner).size()));
            const int count = std::max(1, process.ReadValue<std::int32_t>(header + static_cast<std::uint64_t>(runtime.PointerSize())).value_or(1));
            const int entries = std::min(count, std::max(4, std::min(kArrayPreview, kArrayPreviewElements / perEntry)));
            const std::uint64_t data = process.ReadPointer(header).value_or(0);
            NestedStructs nested;
            Filler filler(runtime, process, data, customTypes, nested);
            for (int i = 0; i < entries; ++i)
                filler.Value(*info, i * size, "[" + std::to_string(i) + "]", 0);
            return Finish(filler, nested);
        }
    }

    void AddUnrealStructures(StructureStore& store, const UnrealFill& fill, const std::string& skip)
    {
        for (const auto& [name, fields] : fill.Structs)
        {
            if (fields.empty() || name == skip)
                continue;
            StructDefinition* existing = store.Find(name);
            if (existing && !existing->Fields.empty())
                continue;
            StructDefinition& child = existing ? *existing : store.Add(name);
            child.Fields = fields;
        }
    }

    UnrealFill FillUnrealObject(const Target::UnrealRuntime& runtime, std::uint64_t object, bool customTypes, int limit)
    {
        const std::uint64_t type = runtime.ClassOf(object);
        const auto process = runtime.ProcessHandle();
        if (!type || !process)
            return {};
        return FillStruct(runtime, *process, type, object, true, customTypes, limit);
    }

    UnrealFill FillFromUnreal(const Target::UnrealRuntime& runtime, std::uint64_t address, const UnrealArrayLookup& arrays, bool customTypes, int limit)
    {
        const auto process = runtime.ProcessHandle();
        if (!process || !address)
            return {};
        if (runtime.IsObject(address))
            return FillUnrealObject(runtime, address, customTypes, limit);
        if (arrays)
        {
            if (const auto array = arrays(address))
                return FillArray(runtime, *process, array->first, array->second, customTypes);
        }
        const auto hit = runtime.Locate(address);
        if (hit && hit->Struct)
            return FillStruct(runtime, *process, hit->Struct, address, false, customTypes, limit);
        return {};
    }

    std::optional<std::pair<std::string, std::uint64_t>> UnrealStructureName(const Target::UnrealRuntime& runtime, std::uint64_t address, bool containing,
        const UnrealArrayLookup& arrays)
    {
        if (!address)
            return std::nullopt;
        if (arrays)
        {
            if (const auto array = arrays(address))
            {
                const auto process = runtime.ProcessHandle();
                const int count = process ? std::max(0, process->ReadValue<std::int32_t>(array->second + static_cast<std::uint64_t>(runtime.PointerSize())).value_or(0)) : 0;
                return std::make_pair(runtime.TypeText(array->first) + "[" + std::to_string(count) + "]", address);
            }
        }
        if (!containing && runtime.IsObject(address))
            return std::make_pair(runtime.NameOf(runtime.ClassOf(address)).value_or("UObject"), address);
        const auto owner = runtime.OwnerOf(address);
        if (!owner)
            return std::nullopt;
        return std::make_pair(runtime.NameOf(owner->second).value_or("UObject"), owner->first);
    }
}
