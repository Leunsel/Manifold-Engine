#include "Table/Structures.h"

#include "Core/Text.h"
#include "Table/AddressList.h"
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

    int FieldByteWidth(const StructField& field, int pointerSize)
    {
        return field.Type == Scan::ValueType::Pointer ? std::max(1, pointerSize) : std::max(1, field.Size);
    }

    namespace
    {
        // [begin, end) in bits from bit 0 of the structure.
        std::pair<std::int64_t, std::int64_t> BitRange(const StructField& field, int pointerSize)
        {
            const std::int64_t base = static_cast<std::int64_t>(field.Offset) * 8;
            if (field.Type == Scan::ValueType::Binary && field.CustomType.empty())
            {
                const int start = std::clamp(field.BitStart, 0, 63);
                return { base + start, base + start + std::clamp(field.BitLength, 1, 64 - start) };
            }
            return { base, base + static_cast<std::int64_t>(FieldByteWidth(field, pointerSize)) * 8 };
        }
    }

    bool FieldsOverlap(const StructField& a, const StructField& b, int pointerSize)
    {
        const auto [aBegin, aEnd] = BitRange(a, pointerSize);
        const auto [bBegin, bEnd] = BitRange(b, pointerSize);
        return aBegin < bEnd && bBegin < aEnd;
    }

    std::optional<int> ParseFieldOffset(std::string_view text)
    {
        text = TrimView(text);
        bool negative = false;
        if (!text.empty() && (text.front() == '+' || text.front() == '-'))
        {
            negative = text.front() == '-';
            text.remove_prefix(1);
        }
        // ParseHex takes a sign of its own; a second one ("+-8") is no offset.
        if (text.empty() || text.front() == '+' || text.front() == '-')
            return std::nullopt;
        const std::optional<std::uint64_t> value = ParseHex(text);
        if (!value || *value > static_cast<std::uint64_t>(kMaxFieldOffset))
            return std::nullopt;
        const int offset = static_cast<int>(*value);
        return negative ? -offset : offset;
    }

    std::string FieldOffsetText(int offset)
    {
        return offset < 0 ? "-" + Hex(static_cast<std::uint64_t>(-static_cast<std::int64_t>(offset)), 4) : "+" + Hex(static_cast<std::uint64_t>(offset), 4);
    }

    std::vector<std::size_t> StructDefinition::Overlapping(const StructField& probe, int pointerSize, std::optional<std::size_t> skip) const
    {
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < Fields.size(); ++i)
        {
            if (skip && *skip == i)
                continue;
            if (FieldsOverlap(Fields[i], probe, pointerSize))
                found.push_back(i);
        }
        return found;
    }

    namespace
    {
        // Sorts and finds `moved` again by its place in memory: the stable sort keeps the order of equal offsets, so the
        // marker is the one field that is unique.
        FieldMove SortAndFind(StructDefinition& definition, std::size_t index, int replaced)
        {
            std::vector<std::uint8_t> marks(definition.Fields.size(), 0);
            marks[index] = 1;
            std::vector<std::pair<StructField, std::uint8_t>> paired;
            paired.reserve(definition.Fields.size());
            for (std::size_t i = 0; i < definition.Fields.size(); ++i)
                paired.push_back({ std::move(definition.Fields[i]), marks[i] });
            const auto bit = [](const StructField& field) { return field.Type == Scan::ValueType::Binary ? field.BitStart : 0; };
            std::stable_sort(paired.begin(), paired.end(), [&](const auto& a, const auto& b)
                {
                    return a.first.Offset != b.first.Offset ? a.first.Offset < b.first.Offset : bit(a.first) < bit(b.first);
                });
            FieldMove move;
            move.Replaced = replaced;
            definition.Fields.clear();
            for (std::size_t i = 0; i < paired.size(); ++i)
            {
                if (paired[i].second)
                    move.Index = i;
                definition.Fields.push_back(std::move(paired[i].first));
            }
            for (std::size_t i = move.Index; i > 0 && definition.Fields[i - 1].Offset == definition.Fields[move.Index].Offset; --i)
                ++move.Twin;
            return move;
        }
    }

    FieldMove MoveField(StructDefinition& definition, std::size_t index, int offset, FieldOverlap overlap, int pointerSize)
    {
        if (index >= definition.Fields.size())
            return {};
        definition.Fields[index].Offset = std::clamp(offset, -kMaxFieldOffset, kMaxFieldOffset);
        int replaced = 0;
        if (overlap == FieldOverlap::Replace)
        {
            const std::vector<std::size_t> doomed = definition.Overlapping(definition.Fields[index], pointerSize, index);
            for (auto it = doomed.rbegin(); it != doomed.rend(); ++it)
            {
                definition.Fields.erase(definition.Fields.begin() + static_cast<std::ptrdiff_t>(*it));
                if (*it < index)
                    --index;
                ++replaced;
            }
        }
        return SortAndFind(definition, index, replaced);
    }

    std::optional<FieldPaste> PasteFields(StructDefinition& definition, std::vector<StructField> fields, std::optional<int> at, int pointerSize)
    {
        if (fields.empty())
            return FieldPaste{};
        std::stable_sort(fields.begin(), fields.end(), [](const StructField& a, const StructField& b) { return a.Offset < b.Offset; });
        const std::int64_t first = fields.front().Offset;
        for (StructField& field : fields)
        {
            const std::int64_t offset = at ? *at + (field.Offset - first) : field.Offset;
            if (offset < -kMaxFieldOffset || offset > kMaxFieldOffset)
                return std::nullopt;
            field.Offset = static_cast<int>(offset);
        }
        FieldPaste result;
        result.First = fields.front().Offset;
        result.Added = static_cast<int>(fields.size());
        const std::size_t before = definition.Fields.size();
        std::erase_if(definition.Fields, [&](const StructField& existing)
            {
                return std::any_of(fields.begin(), fields.end(), [&](const StructField& pasted) { return FieldsOverlap(existing, pasted, pointerSize); });
            });
        result.Replaced = static_cast<int>(before - definition.Fields.size());
        for (StructField& field : fields)
            definition.Fields.push_back(std::move(field));
        definition.Sort();
        return result;
    }

    void StructureHistory::Record(const std::string& structure, std::vector<StructField> before, std::vector<StructField> after, std::string label, std::string merge,
        std::uint64_t group)
    {
        if (before == after)
            return;
        m_redo.clear();
        if (!merge.empty() && !m_undo.empty() && m_undo.back().Merge == merge && m_undo.back().Structure == structure)
        {
            m_undo.back().After = std::move(after);
            if (m_undo.back().After == m_undo.back().Before)
                m_undo.pop_back();
            return;
        }
        for (auto step = m_undo.rbegin(); group && step != m_undo.rend() && step->Group == group; ++step)
        {
            if (step->Structure == structure)
            {
                step->After = std::move(after);
                return;
            }
        }
        m_undo.push_back({ structure, std::move(before), std::move(after), std::move(label), std::move(merge), group });
        if (m_undo.size() > kLimit && !(group && m_undo.front().Group == group))
        {
            // Drops a whole group from the bottom, so no group is left half undoable.
            const std::uint64_t oldest = m_undo.front().Group;
            std::size_t count = 1;
            while (oldest && count < m_undo.size() && m_undo[count].Group == oldest)
                ++count;
            m_undo.erase(m_undo.begin(), m_undo.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }

    std::optional<StructureHistory::Result> StructureHistory::Apply(StructureStore& store, std::vector<Step>& from, std::vector<Step>& to, bool undo)
    {
        if (from.empty())
            return std::nullopt;
        const std::uint64_t group = from.back().Group;
        Result result{ from.back().Label, from.back().Structure, false };
        do
        {
            Step step = std::move(from.back());
            from.pop_back();
            StructDefinition* definition = store.Find(step.Structure);
            if (!definition)
            {
                result.Missing = true;
                continue;
            }
            definition->Fields = undo ? step.Before : step.After;
            // A merged run ends here: the next edit starts a step of its own.
            step.Merge.clear();
            to.push_back(std::move(step));
        } while (group && !from.empty() && from.back().Group == group);
        ++store.Generation;
        return result;
    }

    std::optional<StructureHistory::Result> StructureHistory::Undo(StructureStore& store)
    {
        return Apply(store, m_undo, m_redo, true);
    }

    std::optional<StructureHistory::Result> StructureHistory::Redo(StructureStore& store)
    {
        return Apply(store, m_redo, m_undo, false);
    }

    void StructureHistory::Renamed(const std::string& from, const std::string& to)
    {
        for (std::vector<Step>* steps : { &m_undo, &m_redo })
        {
            for (Step& step : *steps)
            {
                if (EqualsNoCase(step.Structure, from))
                    step.Structure = to;
                for (std::vector<StructField>* fields : { &step.Before, &step.After })
                {
                    for (StructField& field : *fields)
                    {
                        if (field.ChildStructure == from)
                            field.ChildStructure = to;
                    }
                }
            }
        }
    }

    void StructureHistory::Clear()
    {
        m_undo.clear();
        m_redo.clear();
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

    const StructDefinition* StructureStore::Find(const std::string& name) const
    {
        return const_cast<StructureStore*>(this)->Find(name);
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
        const bool prefixed = digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X');
        if (prefixed)
            digits.remove_prefix(2);
        if (digits.empty() || digits.size() > 16)
            return std::nullopt;
        // A word of letters only ("added", "facade", "decade") is a name the
        // filter looks for, not an address, unless it is written with 0x.
        if (!prefixed && std::none_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
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

    std::string RelativeFieldAddress(std::int64_t offset)
    {
        return offset < 0 ? "-" + Hex(static_cast<std::uint64_t>(-offset)) : "+" + Hex(static_cast<std::uint64_t>(offset));
    }

    // ∑ An embedded structure (a .NET Vector2, an Unreal FVector, a C struct member) becomes a group
    //   that counts from its own offset, with one record per field of the child structure. It used
    //   to become one AOB record of the whole struct: Terraria's position showed as 8 raw bytes
    //   instead of X and Y.
    MemoryRecord& AddFieldRecords(AddressList& table, StructureStore& store, const StructField& field, std::string address, MemoryRecord* parent, int depth, int maxDepth)
    {
        if (field.Type != Scan::ValueType::Pointer && !field.ChildStructure.empty() && depth < maxDepth)
        {
            const StructDefinition* child = store.Find(field.ChildStructure);
            if (child && !child->Fields.empty())
            {
                MemoryRecord& group = table.AddGroup(field.Name, parent);
                group.AddressGroupHeader = true;
                group.Address = std::move(address);
                for (const StructField& inner : child->Fields)
                    AddFieldRecords(table, store, inner, RelativeFieldAddress(inner.Offset), &group, depth + 1, maxDepth);
                return group;
            }
        }
        MemoryRecord& record = table.AddValue(field.Name, std::move(address), field.Type, parent);
        record.Length = field.Size;
        record.Unicode = field.Unicode;
        record.ShowHex = field.Display == FieldDisplay::Hex;
        record.CustomTypeName = field.CustomType;
        if (field.Type == Scan::ValueType::Binary)
        {
            record.BitStart = field.BitStart;
            record.BitLength = field.BitLength;
        }
        return record;
    }
}
