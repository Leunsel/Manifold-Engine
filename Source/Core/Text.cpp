#include "Core/Text.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <windows.h>

namespace Manifold
{
    std::wstring Utf8ToWide(std::string_view text)
    {
        if (text.empty())
            return {};
        const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    }

    std::string WideToUtf8(std::wstring_view text)
    {
        if (text.empty())
            return {};
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};
        std::string narrow(static_cast<std::size_t>(length), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
        return narrow;
    }

    std::string_view TrimView(std::string_view text)
    {
        const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; };
        while (!text.empty() && space(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && space(text.back()))
            text.remove_suffix(1);
        return text;
    }

    std::string Trim(std::string_view text)
    {
        return std::string(TrimView(text));
    }

    std::string Lower(std::string_view text)
    {
        std::string result(text);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
        return result;
    }

    std::string Upper(std::string_view text)
    {
        std::string result(text);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c); });
        return result;
    }

    bool EqualsNoCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            const unsigned char x = static_cast<unsigned char>(a[i]);
            const unsigned char y = static_cast<unsigned char>(b[i]);
            if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y))
                return false;
        }
        return true;
    }

    bool StartsWithNoCase(std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
    }

    bool ContainsNoCase(std::string_view text, std::string_view part)
    {
        if (part.empty())
            return true;
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        return std::search(text.begin(), text.end(), part.begin(), part.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
    }

    std::vector<std::string> Split(std::string_view text, char separator, bool keepEmpty)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (;;)
        {
            const std::size_t end = text.find(separator, start);
            const std::string_view part = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (keepEmpty || !TrimView(part).empty())
                parts.emplace_back(part);
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
        return parts;
    }

    std::vector<std::string> SplitLines(std::string_view text)
    {
        std::vector<std::string> lines;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            lines.emplace_back(line);
            start = end + 1;
        }
        return lines;
    }

    std::string Join(const std::vector<std::string>& parts, std::string_view separator)
    {
        std::string result;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                result += separator;
            result += parts[i];
        }
        return result;
    }

    std::string ReplaceAll(std::string text, std::string_view from, std::string_view to)
    {
        if (from.empty())
            return text;
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
        return text;
    }

    std::string Hex(std::uint64_t value, int digits)
    {
        char text[32];
        if (digits > 0)
            std::snprintf(text, sizeof(text), "%0*llX", digits, static_cast<unsigned long long>(value));
        else
            std::snprintf(text, sizeof(text), "%llX", static_cast<unsigned long long>(value));
        return text;
    }

    std::string HexAddress(std::uint64_t value, bool is64Bit)
    {
        return Hex(value, is64Bit ? (value > 0xFFFFFFFFull ? 12 : 8) : 8);
    }

    std::string HexBytes(std::span<const std::uint8_t> bytes, std::string_view separator)
    {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        std::string text;
        text.reserve(bytes.size() * (2 + separator.size()));
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            if (i)
                text += separator;
            text += kDigits[bytes[i] >> 4];
            text += kDigits[bytes[i] & 15];
        }
        return text;
    }

    std::string Grouped(std::uint64_t value)
    {
        std::string digits = std::to_string(value);
        for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
            digits.insert(static_cast<std::size_t>(at), 1, ',');
        return digits;
    }

    std::string ByteSize(std::uint64_t bytes)
    {
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        double value = static_cast<double>(bytes);
        int unit = 0;
        while (value >= 1024.0 && unit < 4)
        {
            value /= 1024.0;
            ++unit;
        }
        return unit == 0 ? Format("%llu B", static_cast<unsigned long long>(bytes)) : Format("%.1f %s", value, units[unit]);
    }

    std::string Format(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        va_list copy;
        va_copy(copy, args);
        const int length = std::vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        std::string text;
        if (length > 0)
        {
            text.resize(static_cast<std::size_t>(length) + 1);
            std::vsnprintf(text.data(), text.size(), format, args);
            text.resize(static_cast<std::size_t>(length));
        }
        va_end(args);
        return text;
    }

    bool IsHexDigit(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    int HexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    std::optional<std::uint64_t> ParseHex(std::string_view text)
    {
        text = TrimView(text);
        bool negative = false;
        if (!text.empty() && (text.front() == '-' || text.front() == '+'))
        {
            negative = text.front() == '-';
            text.remove_prefix(1);
        }
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
            text.remove_prefix(2);
        else if (!text.empty() && text.front() == '$')
            text.remove_prefix(1);
        if (text.empty() || text.size() > 16)
            return std::nullopt;
        std::uint64_t value = 0;
        for (char c : text)
        {
            const int digit = HexValue(c);
            if (digit < 0)
                return std::nullopt;
            value = (value << 4) | static_cast<std::uint64_t>(digit);
        }
        return negative ? ~value + 1 : value;
    }

    std::optional<std::int64_t> ParseDecimal(std::string_view text)
    {
        text = TrimView(text);
        if (!text.empty() && text.front() == '+')
            text.remove_prefix(1);
        if (text.empty())
            return std::nullopt;
        std::int64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error == std::errc::result_out_of_range && text.front() != '-')
        {
            std::uint64_t unsignedValue = 0;
            const auto [end2, error2] = std::from_chars(text.data(), text.data() + text.size(), unsignedValue);
            if (error2 != std::errc() || end2 != text.data() + text.size())
                return std::nullopt;
            return static_cast<std::int64_t>(unsignedValue);
        }
        if (error != std::errc() || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    std::optional<double> ParseDouble(std::string_view text)
    {
        std::string copy = Trim(text);
        std::replace(copy.begin(), copy.end(), ',', '.');
        if (copy.empty())
            return std::nullopt;
        double value = 0.0;
        const auto [end, error] = std::from_chars(copy.data(), copy.data() + copy.size(), value);
        if (error != std::errc() || end != copy.data() + copy.size())
            return std::nullopt;
        return value;
    }

    std::optional<std::vector<std::uint8_t>> ParseHexBytes(std::string_view text)
    {
        std::vector<std::uint8_t> bytes;
        std::string digits;
        for (char c : text)
        {
            if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n')
                continue;
            if (!IsHexDigit(c))
                return std::nullopt;
            digits += c;
        }
        if (digits.size() % 2 != 0)
            return std::nullopt;
        for (std::size_t i = 0; i < digits.size(); i += 2)
            bytes.push_back(static_cast<std::uint8_t>(HexValue(digits[i]) * 16 + HexValue(digits[i + 1])));
        return bytes;
    }

    std::string EscapeXml(std::string_view text)
    {
        std::string result;
        result.reserve(text.size());
        for (char c : text)
        {
            switch (c)
            {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result += c; break;
            }
        }
        return result;
    }
}
