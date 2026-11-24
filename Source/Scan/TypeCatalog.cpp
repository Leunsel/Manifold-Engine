#include "Scan/TypeCatalog.h"

#include "Core/Text.h"

#include <algorithm>

namespace Manifold::Scan
{
    namespace
    {
        constexpr const char* kWholeNumbers = "Whole Numbers";
        constexpr const char* kDecimals = "Decimals";
        constexpr const char* kTextAndBytes = "Text and Bytes";
        constexpr const char* kBitsAndPointers = "Bits and Pointers";
        constexpr const char* kSeveral = "Several at Once";
        constexpr const char* kCustomNumbers = "Custom Numbers";
        constexpr const char* kCustomDecimals = "Custom Decimals";
        constexpr const char* kCustomText = "Custom Text";

        struct EnginePrefix
        {
            const char* Prefix;
            const char* Category;
        };

        constexpr EnginePrefix kEngines[] = {
            { "UE ", "Unreal Engine" },
            { "Unreal ", "Unreal Engine" },
            { "Godot ", "Godot" },
            { "Unity ", "Unity" },
            { "Mono ", "Unity" },
            { "IL2CPP ", "Unity" },
        };

        std::string Bytes(int count)
        {
            return count == 1 ? "1 byte" : std::to_string(count) + " bytes";
        }

        std::string Detail(ValueType type, int pointerSize)
        {
            switch (type)
            {
            case ValueType::String:
            case ValueType::ByteArray: return {};
            case ValueType::Binary: return "bits";
            default: return Bytes(ValueSize(type, ValueFormat{ .PointerSize = pointerSize }));
            }
        }

        TypeSection& SectionFor(std::vector<TypeSection>& sections, const std::string& title)
        {
            for (TypeSection& section : sections)
            {
                if (section.Title == title)
                    return section;
            }
            return sections.emplace_back(TypeSection{ title, {} });
        }
    }

    bool TypeChoice::Same(const TypeChoice& other) const
    {
        if (Kind != other.Kind)
            return false;
        if (Kind == ScanKind::Single)
            return Type == other.Type;
        if (Kind == ScanKind::Custom)
            return EqualsNoCase(Custom, other.Custom);
        return true;
    }

    const char* TypeCategory(ValueType type)
    {
        switch (type)
        {
        case ValueType::Byte:
        case ValueType::TwoBytes:
        case ValueType::FourBytes:
        case ValueType::EightBytes: return kWholeNumbers;
        case ValueType::Float:
        case ValueType::Double: return kDecimals;
        case ValueType::String:
        case ValueType::ByteArray: return kTextAndBytes;
        case ValueType::Binary:
        case ValueType::Pointer: return kBitsAndPointers;
        }
        return kWholeNumbers;
    }

    std::string CustomTypeCategory(const CustomTypeInfo& type)
    {
        for (const EnginePrefix& engine : kEngines)
        {
            if (StartsWithNoCase(type.Name, engine.Prefix) && type.Name.size() > std::string_view(engine.Prefix).size())
                return engine.Category;
        }
        if (type.String)
            return kCustomText;
        return type.Float ? kCustomDecimals : kCustomNumbers;
    }

    std::vector<TypeSection> TypeCatalog(const TypeCatalogOptions& options)
    {
        std::vector<TypeSection> sections;
        for (const char* title : { kWholeNumbers, kDecimals, kTextAndBytes, kBitsAndPointers })
        {
            TypeSection section{ title, {} };
            for (ValueType type : options.Types)
            {
                if (std::string_view(TypeCategory(type)) == title)
                    section.Entries.push_back({ TypeChoice{ ScanKind::Single, type, {} }, TypeName(type), Detail(type, options.PointerSize) });
            }
            if (!section.Entries.empty())
                sections.push_back(std::move(section));
        }
        if (options.Several)
        {
            TypeSection several{ kSeveral, {} };
            several.Entries.push_back({ TypeChoice{ ScanKind::All, ValueType::FourBytes, {} }, "All", {},
                "Byte, 2, 4 and 8 bytes, float and double in one scan. Settings, Scanner, Types for All picks which of them." });
            several.Entries.push_back({ TypeChoice{ ScanKind::Grouped, ValueType::FourBytes, {} }, "Grouped", {}, "Several values that lie next to each other, like 4:100 f:1.5 d:*" });
            sections.push_back(std::move(several));
        }

        std::vector<CustomTypeInfo> customs = options.Customs;
        std::sort(customs.begin(), customs.end(), [](const CustomTypeInfo& a, const CustomTypeInfo& b) { return Lower(a.Name) < Lower(b.Name); });
        std::vector<TypeSection> custom;
        for (const char* title : { kCustomNumbers, kCustomDecimals, kCustomText })
            custom.push_back(TypeSection{ title, {} });
        std::vector<TypeSection> engines;
        for (const CustomTypeInfo& type : customs)
        {
            const std::string category = CustomTypeCategory(type);
            const bool plain = category == kCustomNumbers || category == kCustomDecimals || category == kCustomText;
            TypeSection& section = SectionFor(plain ? custom : engines, category);
            section.Entries.push_back({ TypeChoice{ ScanKind::Custom, ValueType::ByteArray, type.Name }, type.Name, type.Bytes > 0 ? Bytes(type.Bytes) : std::string() });
        }
        std::sort(engines.begin(), engines.end(), [](const TypeSection& a, const TypeSection& b) { return a.Title < b.Title; });
        for (std::vector<TypeSection>* group : { &custom, &engines })
        {
            for (TypeSection& section : *group)
            {
                if (!section.Entries.empty())
                    sections.push_back(std::move(section));
            }
        }
        return sections;
    }

    std::string TypeChoiceLabel(const TypeChoice& choice)
    {
        switch (choice.Kind)
        {
        case ScanKind::All: return "All";
        case ScanKind::Grouped: return "Grouped";
        case ScanKind::Custom: return choice.Custom;
        default: return TypeName(choice.Type);
        }
    }

    bool TypeEntryMatches(const TypeEntry& entry, std::string_view filter)
    {
        const std::string_view needle = TrimView(filter);
        return needle.empty() || ContainsNoCase(entry.Label, needle);
    }
}
