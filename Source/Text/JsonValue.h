#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Text
{
    // A parsed JSON value. Small on purpose: the update check reads one GitHub
    // answer with it, so numbers stay doubles and objects keep the last of
    // duplicate keys.
    struct JsonValue
    {
        enum class Kind
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object,
        };

        Kind Type = Kind::Null;
        bool Bool = false;
        double Number = 0.0;
        std::string String;
        std::vector<JsonValue> Items;
        std::map<std::string, JsonValue, std::less<>> Members;

        bool IsObject() const { return Type == Kind::Object; }
        bool IsArray() const { return Type == Kind::Array; }
        // The member, or nullptr when this is no object or has no such member.
        const JsonValue* Find(std::string_view key) const;
        // The member's text, or empty when it is missing or no string.
        std::string Text(std::string_view key) const;
        double NumberOf(std::string_view key, double fallback = 0.0) const;
        bool BoolOf(std::string_view key, bool fallback = false) const;
    };

    std::optional<JsonValue> ParseJson(std::string_view text, std::string* error = nullptr);
}
