#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"
#include "Table/Structures.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Table
{
    enum class StructLanguage
    {
        Cpp,
        CSharp
    };

    struct StructExportOptions
    {
        StructLanguage Language = StructLanguage::Cpp;
        bool Is64Bit = true;          // the size of a pointer the code is meant for
        bool Children = true;         // also every structure the chosen ones point to or embed
        bool OffsetComments = true;   // // +0x10 behind each field
        bool Checks = true;           // C/C++: static_assert on the size and every offset
    };

    // The structures as source code, with everything a compiler needs to lay them
    // out byte for byte as the dissect shows them: C/C++ with #pragma pack(1) and
    // explicit padding, C# with an explicit layout. Names that are no identifiers
    // are made into ones, the original stays in a comment.
    std::string ExportStructures(const StructureStore& store, const std::vector<std::string>& roots, const StructExportOptions& options);
    // The structures an export writes: the roots and, with children, everything
    // they point to or embed, an embedded one before the one that holds it.
    std::vector<const StructDefinition*> ExportOrder(const StructureStore& store, const std::vector<std::string>& roots, bool children);

    struct StructImport
    {
        std::vector<StructDefinition> Structures;  // in the order they were defined
        std::vector<std::string> Warnings;         // with line numbers
    };

    struct StructImportOptions
    {
        bool Is64Bit = true;  // pointers, size_t, uintptr_t, HANDLE, IntPtr
    };

    // C/C++ struct and class definitions (typedef struct, unions, arrays, pointers,
    // bit fields, enums, #pragma pack, MSVC alignment) or C# structs and classes
    // ([StructLayout(LayoutKind.Explicit)] with [FieldOffset], or sequential).
    // Methods, static members and access specifiers are passed over.
    Result<StructImport> ImportStructures(std::string_view text, const StructImportOptions& options);

    // What an import does with a structure whose name the table has already.
    enum class StructConflict
    {
        Replace,  // the imported one takes the place of the existing one
        Rename,   // both stay; the imported one is called Name_2 (its references follow)
        Skip      // the existing one stays; references to the name now mean it
    };

    // The name each imported structure gets (empty: skipped), in the order given.
    std::vector<std::string> PlanMerge(const StructureStore& store, const std::vector<StructDefinition>& structures, StructConflict conflict);

    struct StructMerge
    {
        int Added = 0;
        int Replaced = 0;
        int Skipped = 0;
        std::vector<std::string> Names;  // as they are now in the store, skipped ones left out
    };
    StructMerge MergeStructures(StructureStore& store, std::vector<StructDefinition> structures, StructConflict conflict);

    // Adds a hex "padding" field at the end so definition.Size() is at least size.
    void PadStructure(StructDefinition& definition, int size);

    // An identifier made from any name: invalid characters become _, a leading digit gets one.
    std::string Identifier(std::string_view name);

    // ReClass.NET project (.rcnet): a ZIP archive with one Data.xml. ReClass.NET has
    // no offsets, a class is its nodes one after another, so gaps are written as
    // Hex nodes; class ids are written in the Base64 form ReClass.NET 1.2 reads.
    std::vector<std::uint8_t> ExportReClass(const StructureStore& store, const std::vector<std::string>& roots, bool is64Bit, bool children);
    // A .rcnet file, or its Data.xml on its own. The platform written in the file
    // wins over options.Is64Bit. Hex nodes with no name of their own (ReClass.NET
    // names new nodes N0000001A) are padding and give no field.
    Result<StructImport> ImportReClass(std::span<const std::uint8_t> file, const StructImportOptions& options);
    bool LooksLikeReClass(std::span<const std::uint8_t> file);
}
