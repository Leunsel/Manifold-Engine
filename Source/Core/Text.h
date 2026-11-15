#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold
{
    std::wstring Utf8ToWide(std::string_view text);
    std::string WideToUtf8(std::wstring_view text);

    std::string Trim(std::string_view text);
    std::string_view TrimView(std::string_view text);
    std::string Lower(std::string_view text);
    std::string Upper(std::string_view text);
    bool EqualsNoCase(std::string_view a, std::string_view b);
    bool StartsWithNoCase(std::string_view text, std::string_view prefix);
    bool ContainsNoCase(std::string_view text, std::string_view part);
    std::vector<std::string> Split(std::string_view text, char separator, bool keepEmpty = false);
    std::vector<std::string> SplitLines(std::string_view text);
    std::string Join(const std::vector<std::string>& parts, std::string_view separator);
    std::string ReplaceAll(std::string text, std::string_view from, std::string_view to);

    std::string Hex(std::uint64_t value, int digits = 0);
    std::string HexAddress(std::uint64_t value, bool is64Bit);
    std::string HexBytes(std::span<const std::uint8_t> bytes, std::string_view separator = " ");
    std::string Grouped(std::uint64_t value);
    std::string ByteSize(std::uint64_t bytes);
    std::string Format(const char* format, ...);

    std::optional<std::uint64_t> ParseHex(std::string_view text);
    std::optional<std::int64_t> ParseDecimal(std::string_view text);
    std::optional<double> ParseDouble(std::string_view text);
    std::optional<std::vector<std::uint8_t>> ParseHexBytes(std::string_view text);

    bool IsHexDigit(char c);
    int HexValue(char c);

    std::string EscapeXml(std::string_view text);
}
