#include "Table/Structures.h"

#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

namespace Manifold::Table
{
    namespace
    {
        bool LooksLikeFloat(float value)
        {
            if (!std::isfinite(value) || value == 0.0f)
                return false;
            const float magnitude = std::fabs(value);
            if (magnitude < 1e-4f || magnitude > 1e7f)
                return false;
            const float scaled = magnitude * 1000.0f;
            return std::fabs(scaled - std::round(scaled)) < 0.01f || magnitude < 100000.0f;
        }
    }

    void GuessFields(const Target::Process& process, StructDefinition& definition, std::uint64_t address, int offset, int size)
    {
        if (size <= 0 || offset < 0)
            return;
        const int end = offset + size;
        std::erase_if(definition.Fields, [&](const StructField& field) { return field.Offset >= offset && field.Offset < end; });
        const int pointerSize = process.PointerSize();
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        process.Read(address + static_cast<std::uint64_t>(offset), bytes.data(), bytes.size());

        std::map<std::uint64_t, Target::RegionInfo> regions;
        const auto mapped = [&](std::uint64_t value)
        {
            auto found = regions.upper_bound(value);
            if (found != regions.begin())
            {
                --found;
                if (value < found->second.End())
                    return found->second.Committed();
            }
            const auto region = process.RegionAt(value);
            if (!region || region->Size == 0)
                return false;
            regions[region->Base] = *region;
            return region->Committed();
        };
        const auto pointerAt = [&](int at)
        {
            if (at < 0 || at + pointerSize > size || (address + static_cast<std::uint64_t>(offset + at)) % static_cast<std::uint64_t>(pointerSize) != 0)
                return false;
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data() + at, static_cast<std::size_t>(pointerSize));
            return value >= 0x10000 && value < process.HighestAddress() && mapped(value);
        };

        std::vector<StructField> added;
        int at = 0;
        while (at + 4 <= size)
        {
            StructField field;
            field.Offset = offset + at;
            if (at + pointerSize <= size)
            {
                if (pointerAt(at))
                {
                    field.Type = Scan::ValueType::Pointer;
                    field.Size = pointerSize;
                    field.Display = FieldDisplay::Hex;
                    field.Name = "Pointer";
                    added.push_back(field);
                    at += pointerSize;
                    continue;
                }
            }
            int printable = 0;
            while (at + printable < size && printable < 64)
            {
                const std::uint8_t c = bytes[static_cast<std::size_t>(at + printable)];
                if (c < 0x20 || c >= 0x7F)
                    break;
                ++printable;
            }
            int length = std::min(size - at, (printable + 4) & ~3);
            for (int slot = at + 1; slot < at + length; ++slot)
            {
                if (pointerAt(slot))
                {
                    length = slot - at;
                    break;
                }
            }
            if (printable >= 4 && length >= 4)
            {
                field.Type = Scan::ValueType::String;
                field.Size = length;
                field.Name = "String";
                added.push_back(field);
                at += field.Size;
                continue;
            }
            float asFloat = 0.0f;
            std::memcpy(&asFloat, bytes.data() + at, 4);
            if (LooksLikeFloat(asFloat))
            {
                field.Type = Scan::ValueType::Float;
                field.Name = "Float";
            }
            else
            {
                field.Type = Scan::ValueType::FourBytes;
                field.Name = "Value";
            }
            field.Size = 4;
            added.push_back(field);
            at += 4;
        }
        for (StructField& field : added)
        {
            field.Name += " " + Hex(static_cast<std::uint64_t>(field.Offset), 4);
            definition.Fields.push_back(std::move(field));
        }
        definition.Sort();
    }

    int StructDefinition::Size() const
    {
        std::int64_t size = 0;
        for (const StructField& field : Fields)
            size = std::max(size, static_cast<std::int64_t>(field.Offset) + field.Size);
        return static_cast<int>(std::min<std::int64_t>(size, std::numeric_limits<int>::max()));
    }

    StructField* StructDefinition::FieldAt(int offset)
    {
        for (StructField& field : Fields)
        {
            if (field.Offset == offset)
                return &field;
        }
        return nullptr;
    }

    void StructDefinition::Sort()
    {
        const auto bit = [](const StructField& field) { return field.Type == Scan::ValueType::Binary ? field.BitStart : 0; };
        std::stable_sort(Fields.begin(), Fields.end(), [&](const StructField& a, const StructField& b) { return a.Offset != b.Offset ? a.Offset < b.Offset : bit(a) < bit(b); });
    }

    int BitFieldSize(int bitStart, int bitLength)
    {
        const int bits = std::clamp(bitStart, 0, 63) + std::clamp(bitLength, 1, 64);
        return bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8;
    }

    StructDefinition* StructureStore::Find(const std::string& name)
    {
        for (const auto& item : Items)
        {
            if (EqualsNoCase(item->Name, name))
                return item.get();
        }
        return nullptr;
    }

    StructDefinition& StructureStore::Add(std::string name)
    {
        auto definition = std::make_unique<StructDefinition>();
        definition->Name = std::move(name);
        Items.push_back(std::move(definition));
        ++Generation;
        return *Items.back();
    }

    void StructureStore::Remove(const StructDefinition& definition)
    {
        Items.erase(std::remove_if(Items.begin(), Items.end(), [&](const auto& item) { return item.get() == &definition; }), Items.end());
        ++Generation;
    }

    void StructureStore::Clear()
    {
        Items.clear();
        ++Generation;
    }

    int ContainingField(const std::vector<FieldSpan>& spans, std::uint64_t address)
    {
        int best = -1;
        for (std::size_t i = 0; i < spans.size(); ++i)
        {
            const FieldSpan& span = spans[i];
            if (!span.Address || address < span.Address || address - span.Address >= std::max<std::uint64_t>(1, span.Size))
                continue;
            if (best < 0)
            {
                best = static_cast<int>(i);
                continue;
            }
            const FieldSpan& chosen = spans[static_cast<std::size_t>(best)];
            if (span.Size < chosen.Size || (span.Size == chosen.Size && span.Depth > chosen.Depth))
                best = static_cast<int>(i);
        }
        return best;
    }

    std::optional<std::uint64_t> FilterAddress(std::string_view text)
    {
        std::string_view digits = TrimView(text);
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
            digits.remove_prefix(2);
        if (digits.empty() || digits.size() > 16)
            return std::nullopt;
        std::uint64_t value = 0;
        for (const char c : digits)
        {
            int digit = 0;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            else
                return std::nullopt;
            value = value << 4 | static_cast<std::uint64_t>(digit);
        }
        if (value < 0x10000)
            return std::nullopt;
        return value;
    }
}
