#pragma once

#include "Scan/Scanner.h"
#include "Scan/ValueType.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Scan
{
    // One choice in a value type list: a built-in type, a scan over several types, or a custom type by name.
    struct TypeChoice
    {
        ScanKind Kind = ScanKind::Single;
        ValueType Type = ValueType::FourBytes;
        std::string Custom;

        bool Same(const TypeChoice& other) const;
    };

    struct CustomTypeInfo
    {
        std::string Name;
        int Bytes = 0;
        bool Float = false;
        bool String = false;
    };

    struct TypeEntry
    {
        TypeChoice Choice;
        std::string Label;
        std::string Detail;
        const char* Tooltip = nullptr;
    };

    struct TypeSection
    {
        std::string Title;
        std::vector<TypeEntry> Entries;
    };

    struct TypeCatalogOptions
    {
        std::span<const ValueType> Types;
        bool Several = false;
        int PointerSize = 8;
        std::vector<CustomTypeInfo> Customs;
    };

    const char* TypeCategory(ValueType type);
    // Types a game engine module registers ("UE FName", "Godot String") go under that engine; the rest by what they hold.
    std::string CustomTypeCategory(const CustomTypeInfo& type);
    std::vector<TypeSection> TypeCatalog(const TypeCatalogOptions& options);
    std::string TypeChoiceLabel(const TypeChoice& choice);
    bool TypeEntryMatches(const TypeEntry& entry, std::string_view filter);
}
