#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A reader for the ECMA-335 metadata of a .NET assembly (the tables behind
// every type, field and method name). The .NET helper uses it on the metadata
// the runtime has mapped in the game, so names come straight from the tables
// instead of through COM interfaces whose layout would have to be guessed.
// Read-only and bounds-checked: corrupt or truncated metadata makes Parse or a
// lookup fail, never read outside the buffer.
namespace Manifold::Target
{
    class ClrMetadata
    {
    public:
        enum Table : int
        {
            Module = 0x00, TypeRef = 0x01, TypeDef = 0x02, FieldPtr = 0x03, Field = 0x04, MethodPtr = 0x05,
            MethodDef = 0x06, ParamPtr = 0x07, Param = 0x08, InterfaceImpl = 0x09, MemberRef = 0x0A,
            Constant = 0x0B, CustomAttribute = 0x0C, FieldMarshal = 0x0D, DeclSecurity = 0x0E,
            ClassLayout = 0x0F, FieldLayout = 0x10, StandAloneSig = 0x11, EventMap = 0x12, EventPtr = 0x13,
            Event = 0x14, PropertyMap = 0x15, PropertyPtr = 0x16, Property = 0x17, MethodSemantics = 0x18,
            MethodImpl = 0x19, ModuleRef = 0x1A, TypeSpec = 0x1B, ImplMap = 0x1C, FieldRva = 0x1D,
            EncLog = 0x1E, EncMap = 0x1F, Assembly = 0x20, AssemblyProcessor = 0x21, AssemblyOs = 0x22,
            AssemblyRef = 0x23, AssemblyRefProcessor = 0x24, AssemblyRefOs = 0x25, File = 0x26,
            ExportedType = 0x27, ManifestResource = 0x28, NestedClass = 0x29, GenericParam = 0x2A,
            MethodSpec = 0x2B, GenericParamConstraint = 0x2C,
            TableCount = 0x2D
        };

        struct TypeDefRow
        {
            std::uint32_t Flags = 0;
            std::string Name;
            std::string Namespace;
            std::uint32_t Extends = 0;  // a TypeDef, TypeRef or TypeSpec token, 0 for none
            std::uint32_t FieldFirst = 0;  // [FieldFirst, FieldEnd) Field rows
            std::uint32_t FieldEnd = 0;
            std::uint32_t MethodFirst = 0; // [MethodFirst, MethodEnd) MethodDef rows
            std::uint32_t MethodEnd = 0;
        };

        struct FieldRow
        {
            std::uint16_t Flags = 0;
            std::string Name;
            std::uint32_t Signature = 0;  // blob index
        };

        struct MethodRow
        {
            std::uint32_t Rva = 0;
            std::uint16_t ImplFlags = 0;
            std::uint16_t Flags = 0;
            std::string Name;
            std::uint32_t Signature = 0;
        };

        // Parses a metadata block (it starts with "BSJB"). Copies the bytes.
        bool Parse(std::span<const std::uint8_t> metadata);
        // Finds the metadata block inside a PE file as it lies on disk.
        static std::optional<std::vector<std::uint8_t>> FromPeFile(std::span<const std::uint8_t> file);

        bool Valid() const { return m_valid; }
        std::uint32_t Rows(Table table) const { return table >= 0 && table < TableCount ? m_rows[static_cast<std::size_t>(table)] : 0; }

        std::optional<TypeDefRow> TypeDefAt(std::uint32_t row) const;
        std::optional<FieldRow> FieldAt(std::uint32_t row) const;
        std::optional<MethodRow> MethodAt(std::uint32_t row) const;

        // "Namespace.Name", nested types as "Outer+Inner" the way the runtime
        // and reflection name them.
        std::string TypeDefName(std::uint32_t row) const;
        // The type a token names (TypeDef, TypeRef or TypeSpec), as above.
        std::string TypeName(std::uint32_t token) const;
        // The enclosing type of a nested TypeDef, 0 when it is not nested.
        std::uint32_t EnclosingOf(std::uint32_t row) const;
        // Whether a TypeDef has generic parameters of its own.
        bool HasGenericParams(std::uint32_t row) const;
        // The TypeDef that declares a Field or MethodDef row, 0 when none.
        std::uint32_t FieldOwner(std::uint32_t row) const;
        std::uint32_t MethodOwner(std::uint32_t row) const;

        // The declared type of a field, in C# spelling for the built-in types
        // ("int", "string", "Item[]", "List<Item>"), empty when unreadable.
        std::string FieldTypeName(std::uint32_t row) const;
        // The parameter count of a method from its signature, -1 when unreadable.
        int ParamCount(std::uint32_t row) const;

    private:
        struct Cursor
        {
            std::span<const std::uint8_t> Bytes;
            std::size_t At = 0;
            bool Ok = true;
            int* Budget = nullptr;  // steps left for the whole name, shared by nested cursors
        };

        std::uint32_t Cell(Table table, std::uint32_t row, int column) const;
        std::string String(std::uint32_t index) const;
        std::span<const std::uint8_t> Blob(std::uint32_t index) const;
        std::uint32_t ListEnd(Table table, std::uint32_t row, int column, Table target) const;
        // ∑ One depth and one step budget for the whole resolution: a TypeSpec
        // naming itself or TypeRefs nested in each other (A in B in A) would
        // otherwise recurse until the stack ends, and a generic instance whose
        // arguments name itself again would branch exponentially. Past either
        // limit the name fails (ok = false, empty).
        static constexpr int kMaxDepth = 64;
        static constexpr int kMaxSteps = 4096;
        std::string TypeNameAt(std::uint32_t token, int depth, bool& ok, int& budget) const;
        std::string SignatureType(Cursor& cursor, int depth) const;

        bool m_valid = false;
        std::vector<std::uint8_t> m_data;
        std::size_t m_strings = 0, m_stringsSize = 0;
        std::size_t m_blob = 0, m_blobSize = 0;
        int m_stringIndex = 2, m_guidIndex = 2, m_blobIndex = 2;
        std::array<std::uint32_t, TableCount> m_rows{};
        std::array<std::size_t, TableCount> m_tableAt{};
        std::array<std::uint32_t, TableCount> m_rowSize{};
        // Per table: the byte offset and size of each column.
        std::array<std::array<std::uint8_t, 10>, TableCount> m_columnAt{};
        std::array<std::array<std::uint8_t, 10>, TableCount> m_columnSize{};
        // NestedClass rows sorted by the nested type, built once by Parse.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> m_nesting;
    };
}
