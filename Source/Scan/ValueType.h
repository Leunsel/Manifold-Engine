#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Scan
{
    enum class ValueType
    {
        Byte,
        TwoBytes,
        FourBytes,
        EightBytes,
        Float,
        Double,
        String,
        ByteArray,
        Binary,
        Pointer
    };

    inline constexpr ValueType kScanTypes[] = { ValueType::Byte, ValueType::TwoBytes, ValueType::FourBytes, ValueType::EightBytes, ValueType::Float,
        ValueType::Double, ValueType::String, ValueType::ByteArray };

    inline constexpr ValueType kRecordTypes[] = { ValueType::Byte, ValueType::TwoBytes, ValueType::FourBytes, ValueType::EightBytes, ValueType::Float,
        ValueType::Double, ValueType::String, ValueType::ByteArray, ValueType::Binary, ValueType::Pointer };

    struct ValueFormat
    {
        bool Hex = false;
        bool Signed = true;
        bool Unicode = false;
        int Length = 0;
        int BitStart = 0;
        int BitLength = 1;
        int PointerSize = 8;
    };

    struct Pattern
    {
        std::vector<std::uint8_t> Bytes;
        std::vector<std::uint8_t> Mask;

        bool Empty() const { return Bytes.empty(); }
        bool Matches(const std::uint8_t* data) const;
    };

    const char* TypeName(ValueType type);
    const char* TableTypeName(ValueType type);
    std::optional<ValueType> TypeFromTableName(std::string_view name);
    bool IsNumeric(ValueType type);
    bool IsFloating(ValueType type);
    int ValueSize(ValueType type, const ValueFormat& format);
    int DefaultAlignment(ValueType type);

    std::string FormatValue(ValueType type, std::span<const std::uint8_t> bytes, const ValueFormat& format);
    std::optional<std::vector<std::uint8_t>> ParseValue(ValueType type, std::string_view text, const ValueFormat& format);
    std::optional<Pattern> ParsePattern(std::string_view text);
    std::string PatternText(const Pattern& pattern);

    std::optional<double> AsDouble(ValueType type, std::span<const std::uint8_t> bytes, bool isSigned);
    std::vector<std::uint8_t> FromDouble(ValueType type, double value, int pointerSize);
}
