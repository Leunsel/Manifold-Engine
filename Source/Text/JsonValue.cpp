#include "Text/JsonValue.h"

#include <cstdlib>

namespace Manifold::Text
{
    namespace
    {
        constexpr int kMaxDepth = 256;

        class Reader
        {
        public:
            explicit Reader(std::string_view text) : m_text(text) {}

            bool Document(JsonValue& out)
            {
                if (m_text.substr(0, 3) == "\xEF\xBB\xBF")
                    m_at = 3;
                Skip();
                if (!Value(out, 0))
                    return false;
                Skip();
                return m_at == m_text.size() || Fail("more text follows the value");
            }

            const std::string& Error() const { return m_error; }

        private:
            bool Fail(const char* message)
            {
                if (m_error.empty())
                    m_error = std::string(message) + " at offset " + std::to_string(m_at);
                return false;
            }

            void Skip()
            {
                while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\r' || m_text[m_at] == '\n'))
                    ++m_at;
            }

            bool Word(std::string_view word)
            {
                if (m_text.substr(m_at, word.size()) != word)
                    return Fail("unknown word");
                m_at += word.size();
                return true;
            }

            bool Value(JsonValue& out, int depth)
            {
                if (depth > kMaxDepth)
                    return Fail("the values nest too deep");
                if (m_at >= m_text.size())
                    return Fail("a value is missing");
                const char c = m_text[m_at];
                if (c == '{')
                    return Object(out, depth);
                if (c == '[')
                    return Array(out, depth);
                if (c == '"')
                {
                    out.Type = JsonValue::Kind::String;
                    return String(out.String);
                }
                if (c == 't' || c == 'f')
                {
                    out.Type = JsonValue::Kind::Bool;
                    out.Bool = c == 't';
                    return Word(c == 't' ? "true" : "false");
                }
                if (c == 'n')
                {
                    out.Type = JsonValue::Kind::Null;
                    return Word("null");
                }
                return Number(out);
            }

            bool Object(JsonValue& out, int depth)
            {
                out.Type = JsonValue::Kind::Object;
                ++m_at;
                Skip();
                if (m_at < m_text.size() && m_text[m_at] == '}')
                {
                    ++m_at;
                    return true;
                }
                for (;;)
                {
                    Skip();
                    if (m_at >= m_text.size() || m_text[m_at] != '"')
                        return Fail("a member name is missing");
                    std::string key;
                    if (!String(key))
                        return false;
                    Skip();
                    if (m_at >= m_text.size() || m_text[m_at] != ':')
                        return Fail("':' is missing");
                    ++m_at;
                    Skip();
                    JsonValue value;
                    if (!Value(value, depth + 1))
                        return false;
                    out.Members[std::move(key)] = std::move(value);
                    Skip();
                    if (m_at < m_text.size() && m_text[m_at] == ',')
                    {
                        ++m_at;
                        continue;
                    }
                    if (m_at < m_text.size() && m_text[m_at] == '}')
                    {
                        ++m_at;
                        return true;
                    }
                    return Fail("',' or '}' is missing");
                }
            }

            bool Array(JsonValue& out, int depth)
            {
                out.Type = JsonValue::Kind::Array;
                ++m_at;
                Skip();
                if (m_at < m_text.size() && m_text[m_at] == ']')
                {
                    ++m_at;
                    return true;
                }
                for (;;)
                {
                    Skip();
                    JsonValue& item = out.Items.emplace_back();
                    if (!Value(item, depth + 1))
                        return false;
                    Skip();
                    if (m_at < m_text.size() && m_text[m_at] == ',')
                    {
                        ++m_at;
                        continue;
                    }
                    if (m_at < m_text.size() && m_text[m_at] == ']')
                    {
                        ++m_at;
                        return true;
                    }
                    return Fail("',' or ']' is missing");
                }
            }

            bool Hex4(unsigned& code)
            {
                if (m_at + 4 > m_text.size())
                    return Fail("a \\u escape is cut off");
                code = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = m_text[m_at++];
                    code <<= 4;
                    if (c >= '0' && c <= '9')
                        code |= static_cast<unsigned>(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        code |= static_cast<unsigned>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        code |= static_cast<unsigned>(c - 'A' + 10);
                    else
                        return Fail("a \\u escape is not hex");
                }
                return true;
            }

            static void Utf8(std::string& out, unsigned code)
            {
                if (code < 0x80)
                    out += static_cast<char>(code);
                else if (code < 0x800)
                {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                else if (code < 0x10000)
                {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xF0 | (code >> 18));
                    out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
            }

            bool String(std::string& out)
            {
                ++m_at;
                for (;;)
                {
                    if (m_at >= m_text.size())
                        return Fail("a string is not closed");
                    const char c = m_text[m_at++];
                    if (c == '"')
                        return true;
                    if (static_cast<unsigned char>(c) < 0x20)
                        return Fail("a control character is inside a string");
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (m_at >= m_text.size())
                        return Fail("an escape is cut off");
                    const char e = m_text[m_at++];
                    switch (e)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        unsigned code = 0;
                        if (!Hex4(code))
                            return false;
                        if (code >= 0xD800 && code < 0xDC00 && m_text.substr(m_at, 2) == "\\u")
                        {
                            m_at += 2;
                            unsigned low = 0;
                            if (!Hex4(low))
                                return false;
                            if (low >= 0xDC00 && low < 0xE000)
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            else
                            {
                                Utf8(out, 0xFFFD);
                                code = low;
                            }
                        }
                        if (code >= 0xD800 && code < 0xE000)
                            code = 0xFFFD;
                        Utf8(out, code);
                        break;
                    }
                    default: return Fail("unknown escape");
                    }
                }
            }

            bool Number(JsonValue& out)
            {
                const std::size_t start = m_at;
                if (m_at < m_text.size() && m_text[m_at] == '-')
                    ++m_at;
                const std::size_t digits = m_at;
                while (m_at < m_text.size() && ((m_text[m_at] >= '0' && m_text[m_at] <= '9') || m_text[m_at] == '.' || m_text[m_at] == 'e' || m_text[m_at] == 'E' || m_text[m_at] == '+' || m_text[m_at] == '-'))
                    ++m_at;
                if (m_at == digits || m_text[digits] < '0' || m_text[digits] > '9')
                    return Fail("unexpected character");
                const std::string number(m_text.substr(start, m_at - start));
                char* end = nullptr;
                out.Type = JsonValue::Kind::Number;
                out.Number = std::strtod(number.c_str(), &end);
                return end == number.c_str() + number.size() || Fail("a number is not valid");
            }

            std::string_view m_text;
            std::size_t m_at = 0;
            std::string m_error;
        };
    }

    const JsonValue* JsonValue::Find(std::string_view key) const
    {
        if (Type != Kind::Object)
            return nullptr;
        const auto found = Members.find(key);
        return found == Members.end() ? nullptr : &found->second;
    }

    std::string JsonValue::Text(std::string_view key) const
    {
        const JsonValue* value = Find(key);
        return value && value->Type == Kind::String ? value->String : std::string();
    }

    double JsonValue::NumberOf(std::string_view key, double fallback) const
    {
        const JsonValue* value = Find(key);
        return value && value->Type == Kind::Number ? value->Number : fallback;
    }

    bool JsonValue::BoolOf(std::string_view key, bool fallback) const
    {
        const JsonValue* value = Find(key);
        return value && value->Type == Kind::Bool ? value->Bool : fallback;
    }

    std::optional<JsonValue> ParseJson(std::string_view text, std::string* error)
    {
        Reader reader(text);
        JsonValue value;
        if (reader.Document(value))
            return value;
        if (error)
            *error = reader.Error();
        return std::nullopt;
    }
}
