#include "Table/CeForms.h"

#include "Core/Deflate.h"
#include "Core/Text.h"
#include "Core/Xml.h"
#include "Table/CheatTable.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Manifold::Table
{
    namespace
    {
        constexpr int kMaxDepth = 64;
        constexpr std::size_t kMaxComponents = 20000;
        constexpr std::size_t kMaxFormBytes = 64u * 1024u * 1024u;
        // ∑ a list item or set name takes one byte of the stream but over a
        // hundred in memory: 64 MB of vaNil items would be ~10 GB of values
        constexpr std::size_t kMaxValues = 1000000;

        // ∑ Free Pascal's TValueType order; Lazarus' LRS writer uses the
        // same numbers as the classic binary DFM stream.
        enum Value : std::uint8_t
        {
            vaNull,
            vaList,
            vaInt8,
            vaInt16,
            vaInt32,
            vaExtended,
            vaString,
            vaIdent,
            vaFalse,
            vaTrue,
            vaBinary,
            vaSet,
            vaLString,
            vaNil,
            vaCollection,
            vaSingle,
            vaCurrency,
            vaDate,
            vaWString,
            vaInt64,
            vaUTF8String,
            vaUString,
            vaQWord,
        };

        struct Reader
        {
            std::span<const std::uint8_t> Data;
            std::size_t At = 0;
            std::size_t Components = 0;
            std::size_t Values = 0; // values and set names read so far, against kMaxValues
            std::string Error;

            bool Fail(std::string text)
            {
                if (Error.empty())
                    Error = std::move(text) + " at byte " + std::to_string(At);
                return false;
            }

            bool Need(std::size_t count)
            {
                return At + count <= Data.size() || Fail("the form data ends early");
            }

            bool Byte(std::uint8_t& out)
            {
                if (!Need(1))
                    return false;
                out = Data[At++];
                return true;
            }

            bool Peek(std::uint8_t& out)
            {
                if (!Need(1))
                    return false;
                out = Data[At];
                return true;
            }

            template <typename T>
            bool Raw(T& out)
            {
                if (!Need(sizeof(T)))
                    return false;
                std::memcpy(&out, Data.data() + At, sizeof(T));
                At += sizeof(T);
                return true;
            }

            bool Bytes(std::size_t count, std::string& out)
            {
                if (!Need(count))
                    return false;
                out.assign(reinterpret_cast<const char*>(Data.data() + At), count);
                At += count;
                return true;
            }

            bool Short(std::string& out)
            {
                std::uint8_t length = 0;
                return Byte(length) && Bytes(length, out);
            }

            bool Wide(std::uint32_t count, std::string& out)
            {
                if (count > kMaxFormBytes / 2 || !Need(static_cast<std::size_t>(count) * 2))
                    return Fail("a text is too long");
                std::wstring wide(count, L'\0');
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    std::uint16_t unit = 0;
                    Raw(unit);
                    wide[i] = static_cast<wchar_t>(unit);
                }
                out = WideToUtf8(wide);
                return true;
            }

            static double Extended(const std::uint8_t* bytes)
            {
                std::uint64_t mantissa = 0;
                std::memcpy(&mantissa, bytes, 8);
                const std::uint16_t top = static_cast<std::uint16_t>(bytes[8] | (bytes[9] << 8));
                const int exponent = top & 0x7FFF;
                const bool negative = (top & 0x8000) != 0;
                if (exponent == 0 && mantissa == 0)
                    return negative ? -0.0 : 0.0;
                if (exponent == 0x7FFF)
                    return negative ? -HUGE_VAL : HUGE_VAL;
                const double value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
                return negative ? -value : value;
            }

            bool Counted()
            {
                return ++Values <= kMaxValues || Fail("the form has more than " + std::to_string(kMaxValues) + " values");
            }

            // a real number that is no number (an Extended's infinity, a raw NaN) is dropped: it reads as no value
            static void Real(CeValue& out, double value)
            {
                out.Type = std::isfinite(value) ? CeValue::Kind::Real : CeValue::Kind::Null;
                out.Real = std::isfinite(value) ? value : 0.0;
            }

            bool Value(CeValue& out, int depth)
            {
                if (depth > kMaxDepth)
                    return Fail("the form nests too deep");
                if (!Counted())
                    return false;
                std::uint8_t type = 0;
                if (!Byte(type))
                    return false;
                switch (type)
                {
                case vaNull:
                case vaNil:
                    out.Type = CeValue::Kind::Null;
                    return true;
                case vaList:
                    out.Type = CeValue::Kind::List;
                    while (true)
                    {
                        std::uint8_t next = 0;
                        if (!Peek(next))
                            return false;
                        if (next == vaNull)
                        {
                            ++At;
                            return true;
                        }
                        CeValue item;
                        if (!Value(item, depth + 1))
                            return false;
                        out.Items.push_back(std::move(item));
                    }
                case vaInt8:
                {
                    std::int8_t value = 0;
                    out.Type = CeValue::Kind::Integer;
                    if (!Raw(value))
                        return false;
                    out.Integer = value;
                    return true;
                }
                case vaInt16:
                {
                    std::int16_t value = 0;
                    out.Type = CeValue::Kind::Integer;
                    if (!Raw(value))
                        return false;
                    out.Integer = value;
                    return true;
                }
                case vaInt32:
                {
                    std::int32_t value = 0;
                    out.Type = CeValue::Kind::Integer;
                    if (!Raw(value))
                        return false;
                    out.Integer = value;
                    return true;
                }
                case vaInt64:
                case vaQWord:
                {
                    std::int64_t value = 0;
                    out.Type = CeValue::Kind::Integer;
                    if (!Raw(value))
                        return false;
                    out.Integer = value;
                    return true;
                }
                case vaExtended:
                {
                    if (!Need(10))
                        return false;
                    Real(out, Extended(Data.data() + At));
                    At += 10;
                    return true;
                }
                case vaSingle:
                {
                    float value = 0.0f;
                    if (!Raw(value))
                        return false;
                    Real(out, value);
                    return true;
                }
                case vaDate:
                {
                    double value = 0.0;
                    if (!Raw(value))
                        return false;
                    Real(out, value);
                    return true;
                }
                case vaCurrency:
                {
                    std::int64_t value = 0;
                    out.Type = CeValue::Kind::Real;
                    if (!Raw(value))
                        return false;
                    out.Real = static_cast<double>(value) / 10000.0;
                    return true;
                }
                case vaString:
                    out.Type = CeValue::Kind::String;
                    return Short(out.Text);
                case vaIdent:
                    out.Type = CeValue::Kind::Ident;
                    return Short(out.Text);
                case vaFalse:
                case vaTrue:
                    out.Type = CeValue::Kind::Boolean;
                    out.Flag = type == vaTrue;
                    return true;
                case vaLString:
                case vaUTF8String:
                {
                    std::uint32_t length = 0;
                    out.Type = CeValue::Kind::String;
                    if (!Raw(length))
                        return false;
                    if (length > kMaxFormBytes)
                        return Fail("a text is too long");
                    return Bytes(length, out.Text);
                }
                case vaWString:
                case vaUString:
                {
                    std::uint32_t length = 0;
                    out.Type = CeValue::Kind::String;
                    return Raw(length) && Wide(length, out.Text);
                }
                case vaBinary:
                {
                    std::uint32_t length = 0;
                    out.Type = CeValue::Kind::Binary;
                    if (!Raw(length) || length > kMaxFormBytes || !Need(length))
                        return Fail("a binary property is damaged");
                    out.Bytes.assign(Data.begin() + static_cast<std::ptrdiff_t>(At), Data.begin() + static_cast<std::ptrdiff_t>(At + length));
                    At += length;
                    return true;
                }
                case vaSet:
                    out.Type = CeValue::Kind::Set;
                    while (true)
                    {
                        std::string name;
                        if (!Short(name))
                            return false;
                        if (name.empty())
                            return true;
                        if (!Counted())
                            return false;
                        out.Names.push_back(std::move(name));
                    }
                case vaCollection:
                    out.Type = CeValue::Kind::Collection;
                    while (true)
                    {
                        std::uint8_t next = 0;
                        if (!Peek(next))
                            return false;
                        if (next == vaNull)
                        {
                            ++At;
                            return true;
                        }
                        if (next == vaInt8 || next == vaInt16 || next == vaInt32)
                        {
                            CeValue order;
                            if (!Value(order, depth + 1))
                                return false;
                        }
                        std::uint8_t list = 0;
                        if (!Byte(list) || list != vaList)
                            return Fail("a collection item does not start with a list");
                        std::vector<std::pair<std::string, CeValue>> entry;
                        if (!Counted() || !Properties(entry, depth + 1))
                            return false;
                        out.Entries.push_back(std::move(entry));
                    }
                default:
                    return Fail("unknown value type " + std::to_string(type));
                }
            }

            bool Properties(std::vector<std::pair<std::string, CeValue>>& out, int depth)
            {
                while (true)
                {
                    std::uint8_t next = 0;
                    if (!Peek(next))
                        return false;
                    if (next == 0)
                    {
                        ++At;
                        return true;
                    }
                    std::string name;
                    CeValue value;
                    if (!Short(name) || !Value(value, depth))
                        return false;
                    out.push_back({ std::move(name), std::move(value) });
                }
            }

            bool Component(CeComponent& out, int depth)
            {
                if (depth > kMaxDepth)
                    return Fail("the form nests too deep");
                if (++Components > kMaxComponents)
                    return Fail("the form has too many components");
                std::uint8_t first = 0;
                if (!Peek(first))
                    return false;
                if ((first & 0xF0) == 0xF0)
                {
                    ++At;
                    if (first & 0x02)
                    {
                        CeValue position;
                        if (!Value(position, depth))
                            return false;
                    }
                }
                if (!Short(out.Class) || !Short(out.Name) || !Properties(out.Properties, depth))
                    return false;
                while (true)
                {
                    std::uint8_t next = 0;
                    if (!Peek(next))
                        return false;
                    if (next == 0)
                    {
                        ++At;
                        return true;
                    }
                    CeComponent child;
                    if (!Component(child, depth + 1))
                        return false;
                    out.Children.push_back(std::move(child));
                }
            }
        };

        std::optional<std::vector<std::uint8_t>> FromHex(std::string_view text)
        {
            std::vector<std::uint8_t> bytes;
            int high = -1;
            for (const char c : text)
            {
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                else if (static_cast<unsigned char>(c) <= ' ')
                    continue;
                else
                    return std::nullopt;
                if (high < 0)
                    high = digit;
                else
                {
                    bytes.push_back(static_cast<std::uint8_t>(high * 16 + digit));
                    high = -1;
                }
            }
            return bytes;
        }

        std::optional<std::vector<std::uint8_t>> Unpack(std::span<const std::uint8_t> packed)
        {
            auto unpacked = Deflate::Decompress(packed, kMaxFormBytes + 4);
            if (!unpacked || unpacked->size() < 4)
                return std::nullopt;
            const std::uint32_t size = static_cast<std::uint32_t>((*unpacked)[0]) | (static_cast<std::uint32_t>((*unpacked)[1]) << 8) |
                                       (static_cast<std::uint32_t>((*unpacked)[2]) << 16) | (static_cast<std::uint32_t>((*unpacked)[3]) << 24);
            if (size > unpacked->size() - 4)
                return std::nullopt;
            unpacked->erase(unpacked->begin(), unpacked->begin() + 4);
            unpacked->resize(size);
            return unpacked;
        }

        const Xml::Node* FindForms(const Xml::Node& node, int depth)
        {
            if (node.Name == "Forms" || node.Name == "FormData")
                return &node;
            if (depth > 3)
                return nullptr;
            for (const auto& child : node.Children)
            {
                if (const Xml::Node* found = FindForms(*child, depth + 1))
                    return found;
            }
            return nullptr;
        }
    }

    const CeValue* CeComponent::Find(std::string_view key) const
    {
        for (const auto& [name, value] : Properties)
        {
            if (EqualsNoCase(name, key))
                return &value;
        }
        return nullptr;
    }

    std::string CeComponent::Text(std::string_view key, std::string_view fallback) const
    {
        const CeValue* value = Find(key);
        if (!value || (value->Type != CeValue::Kind::String && value->Type != CeValue::Kind::Ident))
            return std::string(fallback);
        return value->Text;
    }

    std::int64_t CeComponent::Integer(std::string_view key, std::int64_t fallback) const
    {
        const CeValue* value = Find(key);
        if (!value)
            return fallback;
        if (value->Type == CeValue::Kind::Integer)
            return value->Integer;
        // llround of a value past the int64 range gives no defined result
        if (value->Type == CeValue::Kind::Real)
            return std::isfinite(value->Real) && std::fabs(value->Real) < 9.0e18 ? static_cast<std::int64_t>(std::llround(value->Real)) : fallback;
        return fallback;
    }

    bool CeComponent::Flag(std::string_view key, bool fallback) const
    {
        const CeValue* value = Find(key);
        return value && value->Type == CeValue::Kind::Boolean ? value->Flag : fallback;
    }

    std::vector<std::string> CeComponent::Strings(std::string_view key) const
    {
        std::vector<std::string> out;
        if (const CeValue* value = Find(key); value && value->Type == CeValue::Kind::List)
        {
            for (const CeValue& item : value->Items)
            {
                if (item.Type == CeValue::Kind::String || item.Type == CeValue::Kind::Ident)
                    out.push_back(item.Text);
            }
        }
        return out;
    }

    bool CeComponent::HasName(std::string_view key, std::string_view name) const
    {
        const CeValue* value = Find(key);
        if (!value || value->Type != CeValue::Kind::Set)
            return false;
        return std::any_of(value->Names.begin(), value->Names.end(), [&](const std::string& entry) { return EqualsNoCase(entry, name); });
    }

    std::optional<CeComponent> ReadCeComponents(std::span<const std::uint8_t> data, std::string& error)
    {
        Reader reader{ data };
        if (data.size() < 4 || std::memcmp(data.data(), "TPF0", 4) != 0)
        {
            error = "the form data is no Lazarus component stream (no TPF0 signature)";
            return std::nullopt;
        }
        reader.At = 4;
        CeComponent root;
        if (!reader.Component(root, 0))
        {
            error = reader.Error;
            return std::nullopt;
        }
        return root;
    }

    std::optional<CeComponent> DecodeCeForm(const Xml::Node& node, std::string& error)
    {
        std::optional<std::vector<std::uint8_t>> data;
        if (node.Attribute("Encoding").value_or("") == "Ascii85")
            data = DecodeTableFile(node.Text, kMaxFormBytes);
        else if (const auto packed = FromHex(node.Text))
            data = Unpack(*packed);
        if (!data)
        {
            error = "the form data could not be unpacked";
            return std::nullopt;
        }
        return ReadCeComponents(*data, error);
    }

    std::vector<CeFormEntry> ReadCeForms(const Xml::Node& node)
    {
        std::vector<CeFormEntry> forms;
        for (const auto& child : node.Children)
        {
            CeFormEntry entry;
            entry.Name = child->Name;
            entry.Class = child->Attribute("Class").value_or("TCEForm");
            entry.Root = DecodeCeForm(*child, entry.Error);
            forms.push_back(std::move(entry));
        }
        return forms;
    }

    std::vector<CeFormEntry> ReadCeFormFile(std::string_view text, std::string& error)
    {
        Xml::ParseResult parsed = Xml::Parse(text);
        if (!parsed.Root)
        {
            error = "not XML: " + parsed.Error;
            return {};
        }
        const Xml::Node* forms = FindForms(*parsed.Root, 0);
        if (!forms)
        {
            error = "the file holds no Cheat Engine forms (no <FormData> or <Forms>)";
            return {};
        }
        std::vector<CeFormEntry> found = ReadCeForms(*forms);
        if (found.empty())
            error = "the file's form list is empty";
        return found;
    }

    std::vector<CeFormEntry> ReadCeTableForms(const std::vector<std::string>& extraXml)
    {
        std::vector<CeFormEntry> found;
        for (const std::string& text : extraXml)
        {
            Xml::ParseResult parsed = Xml::Parse(text);
            if (!parsed.Root || parsed.Root->Name != "Forms")
                continue;
            for (CeFormEntry& entry : ReadCeForms(*parsed.Root))
                found.push_back(std::move(entry));
        }
        return found;
    }
}
