#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Xml
{
    struct Node;
}

namespace Manifold::Table
{
    // One value of a Lazarus binary component stream (what CE's form designer saves).
    struct CeValue
    {
        enum class Kind : std::uint8_t
        {
            Null,
            Integer,
            Real,
            String,
            Ident,
            Boolean,
            Binary,
            Set,
            List,
            Collection,
        };

        Kind Type = Kind::Null;
        std::int64_t Integer = 0;
        double Real = 0.0;
        bool Flag = false;
        std::string Text;
        std::vector<std::uint8_t> Bytes;
        std::vector<std::string> Names;
        std::vector<CeValue> Items;
        std::vector<std::vector<std::pair<std::string, CeValue>>> Entries;
    };

    struct CeComponent
    {
        std::string Class;
        std::string Name;
        std::vector<std::pair<std::string, CeValue>> Properties;
        std::vector<CeComponent> Children;

        const CeValue* Find(std::string_view key) const;
        std::string Text(std::string_view key, std::string_view fallback = {}) const;
        std::int64_t Integer(std::string_view key, std::int64_t fallback = 0) const;
        bool Flag(std::string_view key, bool fallback) const;
        std::vector<std::string> Strings(std::string_view key) const;
        bool HasName(std::string_view key, std::string_view name) const;
    };

    struct CeFormEntry
    {
        std::string Name;
        std::string Class;
        std::optional<CeComponent> Root;
        std::string Error;
    };

    std::optional<CeComponent> ReadCeComponents(std::span<const std::uint8_t> data, std::string& error);
    std::optional<CeComponent> DecodeCeForm(const Xml::Node& node, std::string& error);
    std::vector<CeFormEntry> ReadCeForms(const Xml::Node& node);
    std::vector<CeFormEntry> ReadCeFormFile(std::string_view text, std::string& error);
    std::vector<CeFormEntry> ReadCeTableForms(const std::vector<std::string>& extraXml);
}
