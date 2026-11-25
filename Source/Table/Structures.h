#pragma once

#include "Scan/ValueType.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Table
{
    class AddressList;
    struct MemoryRecord;

    enum class FieldDisplay
    {
        Unsigned,
        Signed,
        Hex,
        Binary
    };

    struct StructField
    {
        int Offset = 0;
        std::string Name;
        Scan::ValueType Type = Scan::ValueType::FourBytes;
        int Size = 4;
        FieldDisplay Display = FieldDisplay::Unsigned;
        bool Unicode = false;
        std::string ChildStructure;
        std::string CustomType;
        int BitStart = 0;
        int BitLength = 1;
    };

    // ∑ Limits for fields read from a table: the editor allows the same length, and offsets this far apart keep Offset + Size inside int.
    inline constexpr int kMaxFieldBytes = 4096;
    inline constexpr int kMaxFieldOffset = 0x10000000;

    int BitFieldSize(int bitStart, int bitLength);

    struct StructDefinition
    {
        std::string Name;
        std::vector<StructField> Fields;

        int Size() const;
        StructField* FieldAt(int offset);
        void Sort();
    };

    struct StructureStore
    {
        std::vector<std::unique_ptr<StructDefinition>> Items;
        std::uint64_t Generation = 1;

        StructDefinition* Find(const std::string& name);
        StructDefinition& Add(std::string name);
        void Remove(const StructDefinition& definition);
        void Clear();
    };

    void GuessFields(const Target::Process& process, StructDefinition& definition, std::uint64_t address, int offset, int size);

    // A field as shown in a dissect view: where it lies and how deep it hangs.
    struct FieldSpan
    {
        std::uint64_t Address = 0;
        std::uint64_t Size = 1;
        int Depth = 0;
    };

    // The span that holds `address`: the smallest one, and of equal ones the
    // deepest (a field inside an embedded structure beats the structure); -1
    // when none does.
    int ContainingField(const std::vector<FieldSpan>& spans, std::uint64_t address);
    // A field filter that is an address rather than an offset: hex digits
    // (0x allowed, no + in front) of at least 0x10000.
    std::optional<std::uint64_t> FilterAddress(std::string_view text);

    // "+1C" or "-8": a record address that counts from the group above it.
    std::string RelativeFieldAddress(std::int64_t offset);
    // Adds a structure field to the address list at `address`. An embedded structure (not a
    // pointer, with a child structure in the store) becomes an address group with one record per
    // child field, counting from the group, down to maxDepth levels; anything else one record.
    MemoryRecord& AddFieldRecords(AddressList& table, StructureStore& store, const StructField& field, std::string address, MemoryRecord* parent, int depth, int maxDepth);
}
