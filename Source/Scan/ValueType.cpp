#include "Scan/ValueType.h"

#include "Core/Text.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Manifold::Scan
{
    namespace
    {
        template <class T>
        T Load(std::span<const std::uint8_t> bytes)
        {
            T value{};
            std::memcpy(&value, bytes.data(), std::min(sizeof(T), bytes.size()));
            return value;
        }

        template <class T>
        std::vector<std::uint8_t> Store(T value)
        {
            std::vector<std::uint8_t> bytes(sizeof(T));
            std::memcpy(bytes.data(), &value, sizeof(T));
            return bytes;
        }

        std::string FloatText(double value, bool isFloat)
        {
            if (std::isnan(value))
                return "NaN";
            if (std::isinf(value))
                return value > 0 ? "Inf" : "-Inf";
            std::string text = Format(isFloat ? "%.6g" : "%.10g", value);
            if (text.find('e') != std::string::npos || text.find('E') != std::string::npos)
                text = Format(isFloat ? "%.6f" : "%.10f", value);
            if (text.find('.') != std::string::npos)
            {
                while (!text.empty() && text.back() == '0')
                    text.pop_back();
                if (!text.empty() && text.back() == '.')
                    text.pop_back();
            }
            return text;
        }

        std::optional<std::uint64_t> ParseInteger(std::string_view text, bool hex, int bytes)
        {
            text = TrimView(text);
            if (text.empty())
                return std::nullopt;
            std::optional<std::uint64_t> value;
            if (hex || StartsWithNoCase(text, "0x") || text.front() == '$')
            {
                value = ParseHex(text);
            }
            else
            {
                const auto decimal = ParseDecimal(text);
                if (decimal)
                    value = static_cast<std::uint64_t>(*decimal);
            }
            if (!value)
                return std::nullopt;
            if (bytes < 8)
            {
                const std::uint64_t mask = (1ull << (bytes * 8)) - 1;
                const std::int64_t asSigned = static_cast<std::int64_t>(*value);
                const std::int64_t lowest = -(1ll << (bytes * 8 - 1));
                if (*value > mask && !(asSigned < 0 && asSigned >= lowest))
                    return std::nullopt;
                value = *value & mask;
            }
            return value;
        }
    }

    bool Pattern::Matches(const std::uint8_t* data) const
    {
        for (std::size_t i = 0; i < Bytes.size(); ++i)
        {
            if ((data[i] & Mask[i]) != Bytes[i])
                return false;
        }
        return true;
    }

    const char* TypeName(ValueType type)
    {
        switch (type)
        {
        case ValueType::Byte: return "Byte";
        case ValueType::TwoBytes: return "2 Bytes";
        case ValueType::FourBytes: return "4 Bytes";
        case ValueType::EightBytes: return "8 Bytes";
        case ValueType::Float: return "Float";
        case ValueType::Double: return "Double";
        case ValueType::String: return "String";
        case ValueType::ByteArray: return "Array of byte";
        case ValueType::Binary: return "Binary";
        case ValueType::Pointer: return "Pointer";
        }
        return "?";
    }

    const char* TableTypeName(ValueType type)
    {
        return TypeName(type);
    }

    std::optional<ValueType> TypeFromTableName(std::string_view name)
    {
        for (ValueType type : kRecordTypes)
        {
            if (EqualsNoCase(name, TypeName(type)))
                return type;
        }
        if (EqualsNoCase(name, "vtByte"))
            return ValueType::Byte;
        if (EqualsNoCase(name, "vtWord"))
            return ValueType::TwoBytes;
        if (EqualsNoCase(name, "vtDword"))
            return ValueType::FourBytes;
        if (EqualsNoCase(name, "vtQword"))
            return ValueType::EightBytes;
        if (EqualsNoCase(name, "vtSingle"))
            return ValueType::Float;
        if (EqualsNoCase(name, "vtDouble"))
            return ValueType::Double;
        if (EqualsNoCase(name, "vtString") || EqualsNoCase(name, "Unicode String"))
            return ValueType::String;
        if (EqualsNoCase(name, "vtByteArray") || EqualsNoCase(name, "Array of bytes"))
            return ValueType::ByteArray;
        return std::nullopt;
    }

    bool IsNumeric(ValueType type)
    {
        return type == ValueType::Byte || type == ValueType::TwoBytes || type == ValueType::FourBytes || type == ValueType::EightBytes ||
               type == ValueType::Float || type == ValueType::Double || type == ValueType::Pointer || type == ValueType::Binary;
    }

    bool IsFloating(ValueType type)
    {
        return type == ValueType::Float || type == ValueType::Double;
    }

    int ValueSize(ValueType type, const ValueFormat& format)
    {
        switch (type)
        {
        case ValueType::Byte: return 1;
        case ValueType::TwoBytes: return 2;
        case ValueType::FourBytes: return 4;
        case ValueType::EightBytes: return 8;
        case ValueType::Float: return 4;
        case ValueType::Double: return 8;
        case ValueType::Pointer: return format.PointerSize;
        case ValueType::String: return std::max(1, format.Length) * (format.Unicode ? 2 : 1);
        case ValueType::ByteArray: return std::max(1, format.Length);
        case ValueType::Binary: return std::max(1, (format.BitStart + format.BitLength + 7) / 8);
        }
        return 1;
    }

    int DefaultAlignment(ValueType type)
    {
        switch (type)
        {
        case ValueType::TwoBytes: return 2;
        case ValueType::FourBytes:
        case ValueType::EightBytes:
        case ValueType::Float:
        case ValueType::Double:
        case ValueType::Pointer: return 4;
        default: return 1;
        }
    }

    std::string FormatValue(ValueType type, std::span<const std::uint8_t> bytes, const ValueFormat& format)
    {
        if (bytes.empty())
            return "??";
        switch (type)
        {
        case ValueType::Byte:
            return format.Hex ? Hex(bytes[0], 2) : format.Signed ? std::to_string(static_cast<std::int8_t>(bytes[0])) : std::to_string(bytes[0]);
        case ValueType::TwoBytes:
        {
            const auto value = Load<std::uint16_t>(bytes);
            return format.Hex ? Hex(value, 4) : format.Signed ? std::to_string(static_cast<std::int16_t>(value)) : std::to_string(value);
        }
        case ValueType::FourBytes:
        {
            const auto value = Load<std::uint32_t>(bytes);
            return format.Hex ? Hex(value, 8) : format.Signed ? std::to_string(static_cast<std::int32_t>(value)) : std::to_string(value);
        }
        case ValueType::EightBytes:
        {
            const auto value = Load<std::uint64_t>(bytes);
            return format.Hex ? Hex(value, 16) : format.Signed ? std::to_string(static_cast<std::int64_t>(value)) : std::to_string(value);
        }
        case ValueType::Pointer:
        {
            const std::uint64_t value = format.PointerSize == 4 ? Load<std::uint32_t>(bytes) : Load<std::uint64_t>(bytes);
            return Hex(value, format.PointerSize * 2);
        }
        case ValueType::Float:
        {
            const auto value = Load<float>(bytes);
            return format.Hex ? Hex(Load<std::uint32_t>(bytes), 8) : FloatText(value, true);
        }
        case ValueType::Double:
        {
            const auto value = Load<double>(bytes);
            return format.Hex ? Hex(Load<std::uint64_t>(bytes), 16) : FloatText(value, false);
        }
        case ValueType::String:
        {
            std::string text;
            if (format.Unicode)
            {
                std::wstring wide;
                for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
                {
                    const wchar_t c = static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8));
                    if (c == 0)
                        break;
                    wide += c;
                }
                text = WideToUtf8(wide);
            }
            else
            {
                for (std::uint8_t c : bytes)
                {
                    if (c == 0)
                        break;
                    text += static_cast<char>(c >= 0x20 || c == '\t' ? c : '?');
                }
            }
            return text;
        }
        case ValueType::ByteArray:
            return HexBytes(bytes);
        case ValueType::Binary:
        {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes.data(), std::min<std::size_t>(8, bytes.size()));
            value >>= format.BitStart;
            std::string text;
            for (int bit = format.BitLength - 1; bit >= 0; --bit)
                text += ((value >> bit) & 1) ? '1' : '0';
            return text;
        }
        }
        return "?";
    }

    std::optional<std::vector<std::uint8_t>> ParseValue(ValueType type, std::string_view text, const ValueFormat& format)
    {
        text = TrimView(text);
        switch (type)
        {
        case ValueType::Byte:
        case ValueType::TwoBytes:
        case ValueType::FourBytes:
        case ValueType::EightBytes:
        case ValueType::Pointer:
        {
            const int size = ValueSize(type, format);
            const auto value = ParseInteger(text, format.Hex || type == ValueType::Pointer, size);
            if (!value)
            {
                const bool fractional = text.find_first_of(".,") != std::string_view::npos;
                if (const auto floating = fractional ? ParseDouble(text) : std::nullopt; floating && !format.Hex)
                {
                    const double rounded = std::round(*floating);
                    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                    const auto whole = static_cast<std::int64_t>(rounded);
                    std::memcpy(bytes.data(), &whole, bytes.size());
                    return bytes;
                }
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::memcpy(bytes.data(), &*value, bytes.size());
            return bytes;
        }
        case ValueType::Float:
        {
            if (format.Hex)
            {
                const auto raw = ParseHex(text);
                if (!raw || *raw > 0xFFFFFFFFull)
                    return std::nullopt;
                return Store(static_cast<std::uint32_t>(*raw));
            }
            const auto value = ParseDouble(text);
            if (!value)
                return std::nullopt;
            return Store(static_cast<float>(*value));
        }
        case ValueType::Double:
        {
            if (format.Hex)
            {
                const auto raw = ParseHex(text);
                if (!raw)
                    return std::nullopt;
                return Store(*raw);
            }
            const auto value = ParseDouble(text);
            if (!value)
                return std::nullopt;
            return Store(*value);
        }
        case ValueType::String:
        {
            if (text.empty())
                return std::nullopt;
            if (format.Unicode)
            {
                const std::wstring wide = Utf8ToWide(text);
                std::vector<std::uint8_t> bytes(wide.size() * 2);
                std::memcpy(bytes.data(), wide.data(), bytes.size());
                return bytes;
            }
            return std::vector<std::uint8_t>(text.begin(), text.end());
        }
        case ValueType::ByteArray:
            return ParseHexBytes(text);
        case ValueType::Binary:
        {
            std::uint64_t value = 0;
            if (text.empty() || text.size() > 64)
                return std::nullopt;
            for (char c : text)
            {
                if (c != '0' && c != '1')
                    return std::nullopt;
                value = (value << 1) | static_cast<std::uint64_t>(c - '0');
            }
            std::vector<std::uint8_t> bytes(8);
            std::memcpy(bytes.data(), &value, 8);
            return bytes;
        }
        }
        return std::nullopt;
    }

    std::optional<Pattern> ParsePattern(std::string_view text)
    {
        Pattern pattern;
        std::string token;
        const auto addPair = [&](char high, char low) -> bool
            {
                std::uint8_t value = 0;
                std::uint8_t mask = 0;
                for (const char c : { high, low })
                {
                    value = static_cast<std::uint8_t>(value << 4);
                    mask = static_cast<std::uint8_t>(mask << 4);
                    if (c == '?' || c == '*')
                        continue;
                    const int digit = HexValue(c);
                    if (digit < 0)
                        return false;
                    value = static_cast<std::uint8_t>(value | digit);
                    mask = static_cast<std::uint8_t>(mask | 0xF);
                }
                pattern.Bytes.push_back(value);
                pattern.Mask.push_back(mask);
                return true;
            };
        const auto flush = [&]() -> bool
            {
                if (token.empty())
                    return true;
                bool ok = true;
                if (token.size() == 1)
                    ok = (token[0] == '?' || token[0] == '*') ? addPair('?', '?') : addPair('0', token[0]);
                else if (token.size() % 2 != 0)
                    ok = false;
                else
                {
                    for (std::size_t i = 0; ok && i < token.size(); i += 2)
                        ok = addPair(token[i], token[i + 1]);
                }
                token.clear();
                return ok;
            };
        for (char c : text)
        {
            if (c == ' ' || c == ',' || c == '\t' || c == '-' || c == '\r' || c == '\n')
            {
                if (!flush())
                    return std::nullopt;
            }
            else
            {
                token += c;
            }
        }
        if (!flush() || pattern.Bytes.empty())
            return std::nullopt;
        return pattern;
    }

    std::string PatternText(const Pattern& pattern)
    {
        std::string text;
        for (std::size_t i = 0; i < pattern.Bytes.size(); ++i)
        {
            if (i)
                text += ' ';
            if (pattern.Mask[i] == 0)
                text += "??";
            else
                text += Hex(pattern.Bytes[i], 2);
        }
        return text;
    }

    std::optional<double> AsDouble(ValueType type, std::span<const std::uint8_t> bytes, bool isSigned)
    {
        switch (type)
        {
        case ValueType::Byte: return isSigned ? static_cast<double>(static_cast<std::int8_t>(bytes[0])) : static_cast<double>(bytes[0]);
        case ValueType::TwoBytes: return isSigned ? static_cast<double>(Load<std::int16_t>(bytes)) : static_cast<double>(Load<std::uint16_t>(bytes));
        case ValueType::FourBytes: return isSigned ? static_cast<double>(Load<std::int32_t>(bytes)) : static_cast<double>(Load<std::uint32_t>(bytes));
        case ValueType::EightBytes: return isSigned ? static_cast<double>(Load<std::int64_t>(bytes)) : static_cast<double>(Load<std::uint64_t>(bytes));
        case ValueType::Float: return static_cast<double>(Load<float>(bytes));
        case ValueType::Double: return Load<double>(bytes);
        default: return std::nullopt;
        }
    }

    std::vector<std::uint8_t> FromDouble(ValueType type, double value, int pointerSize)
    {
        switch (type)
        {
        case ValueType::Byte: return Store(static_cast<std::uint8_t>(static_cast<std::int64_t>(std::llround(value))));
        case ValueType::TwoBytes: return Store(static_cast<std::uint16_t>(static_cast<std::int64_t>(std::llround(value))));
        case ValueType::FourBytes: return Store(static_cast<std::uint32_t>(static_cast<std::int64_t>(std::llround(value))));
        case ValueType::EightBytes: return Store(static_cast<std::uint64_t>(std::llround(value)));
        case ValueType::Float: return Store(static_cast<float>(value));
        case ValueType::Double: return Store(value);
        case ValueType::Pointer:
            return pointerSize == 4 ? Store(static_cast<std::uint32_t>(std::llround(value))) : Store(static_cast<std::uint64_t>(std::llround(value)));
        default: return {};
        }
    }
}
