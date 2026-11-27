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
        std::string ChildStructStart;
        std::string BackgroundColor;
        std::string ExpandChangesAddress;

        bool operator==(const StructField&) const = default;
    };

    // ∑ Limits for fields read from a table: the editor allows the same length, and offsets this far apart keep Offset + Size inside int.
    inline constexpr int kMaxFieldBytes = 4096;
    inline constexpr int kMaxFieldOffset = 0x10000000;

    int BitFieldSize(int bitStart, int bitLength);
    // How many bytes a field covers in a target with pointers this wide: a pointer is as wide as the target's pointers.
    int FieldByteWidth(const StructField& field, int pointerSize);
    // Two fields overlap when they share a bit: bit fields at the same offset with separate bits do not.
    bool FieldsOverlap(const StructField& a, const StructField& b, int pointerSize);
    // An offset typed into the dissect: hex, + or - in front and 0x allowed, within kMaxFieldOffset.
    std::optional<int> ParseFieldOffset(std::string_view text);
    // "+0010" or "-0008": an offset as the dissect shows it in messages.
    std::string FieldOffsetText(int offset);

    struct StructDefinition
    {
        std::string Name;
        std::vector<StructField> Fields;

        int Size() const;
        StructField* FieldAt(int offset);
        void Sort();
        // The fields that share a bit with `probe`, leaving out the field at `skip`.
        std::vector<std::size_t> Overlapping(const StructField& probe, int pointerSize, std::optional<std::size_t> skip = std::nullopt) const;
    };

    enum class FieldOverlap
    {
        Replace,  // the fields in the way go
        Keep  // both stay, like a union
    };

    struct FieldMove
    {
        std::size_t Index = 0;  // where the moved field is after the sort
        int Twin = 0;  // how many fields before it share its offset
        int Replaced = 0;
    };

    // Moves the field at `index` to `offset` and sorts the definition again.
    FieldMove MoveField(StructDefinition& definition, std::size_t index, int offset, FieldOverlap overlap, int pointerSize);

    struct FieldPaste
    {
        int Added = 0;
        int Replaced = 0;
        int First = 0;  // offset of the first pasted field
    };

    // Adds copies of `fields` to the definition, their distances to each other kept: the first at `at`, or every one at its own
    // offset without it. Fields already there that a pasted one overlaps are replaced. Nothing changes and nullopt comes back
    // when a field would land beyond kMaxFieldOffset.
    std::optional<FieldPaste> PasteFields(StructDefinition& definition, std::vector<StructField> fields, std::optional<int> at, int pointerSize);

    struct StructureStore
    {
        std::vector<std::unique_ptr<StructDefinition>> Items;
        std::uint64_t Generation = 1;

        StructDefinition* Find(const std::string& name);
        const StructDefinition* Find(const std::string& name) const;
        StructDefinition& Add(std::string name);
        void Remove(const StructDefinition& definition);
        void Clear();
    };

    // Undo and redo for edits to the fields of structures, by structure name: each step holds the fields before and after.
    class StructureHistory
    {
    public:
        static constexpr std::size_t kLimit = 200;

        struct Result
        {
            std::string Label;
            std::string Structure;
            bool Missing = false;  // the structure is gone; the step was dropped
        };

        // `merge`: a later step with the same key on the same structure folds into this one (typing a length digit by digit).
        // `group`: steps with the same non-zero group are undone and redone together.
        void Record(const std::string& structure, std::vector<StructField> before, std::vector<StructField> after, std::string label, std::string merge = {},
            std::uint64_t group = 0);
        std::optional<Result> Undo(StructureStore& store);
        std::optional<Result> Redo(StructureStore& store);
        bool CanUndo() const { return !m_undo.empty(); }
        bool CanRedo() const { return !m_redo.empty(); }
        std::string UndoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().Label; }
        std::string RedoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().Label; }
        std::size_t UndoCount() const { return m_undo.size(); }
        void Renamed(const std::string& from, const std::string& to);
        void Clear();

    private:
        struct Step
        {
            std::string Structure;
            std::vector<StructField> Before;
            std::vector<StructField> After;
            std::string Label;
            std::string Merge;
            std::uint64_t Group = 0;
        };

        std::optional<Result> Apply(StructureStore& store, std::vector<Step>& from, std::vector<Step>& to, bool undo);

        std::vector<Step> m_undo;
        std::vector<Step> m_redo;
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
