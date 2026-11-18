#include "Text/Json.h"

#include <algorithm>

namespace Manifold::Text
{
    namespace
    {
        constexpr int kMaxDepth = 512;
        constexpr std::string_view kBom = "\xEF\xBB\xBF";
        constexpr std::size_t npos = std::string_view::npos;

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool IsHex(char c)
        {
            return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        class Parser
        {
        public:
            Parser(std::string_view text, int indent, std::string* out)
                : m_text(text), m_indent(indent), m_out(out), m_newline(text.find("\r\n") != npos ? "\r\n" : "\n")
            {
            }

            bool Run()
            {
                if (m_text.substr(0, kBom.size()) == kBom)
                {
                    m_at = kBom.size();
                    Emit(kBom);
                }
                if (!Skip())
                    return false;
                if (m_at >= m_text.size())
                    return Fail("the text is empty");
                if (!Value(0) || !Skip())
                    return false;
                if (m_at < m_text.size())
                    return Fail("more text follows the value");
                if (!m_text.empty() && m_text.back() == '\n')
                    Emit(m_newline);
                return true;
            }

            std::size_t ErrorAt() const { return m_errorAt; }
            const std::string& Message() const { return m_message; }

        private:
            bool Fail(std::string message)
            {
                if (m_message.empty())
                {
                    m_message = std::move(message);
                    m_errorAt = m_at;
                }
                return false;
            }

            bool Skip()
            {
                while (m_at < m_text.size())
                {
                    const char c = m_text[m_at];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    {
                        ++m_at;
                        continue;
                    }
                    if (c == '/' && m_at + 1 < m_text.size() && (m_text[m_at + 1] == '/' || m_text[m_at + 1] == '*'))
                        return Fail("JSON has no comments");
                    break;
                }
                return true;
            }

            bool At(char c) const
            {
                return m_at < m_text.size() && m_text[m_at] == c;
            }

            bool Digit() const
            {
                return m_at < m_text.size() && IsDigit(m_text[m_at]);
            }

            void Emit(std::string_view text)
            {
                if (m_out)
                    m_out->append(text);
            }

            void Break(int depth)
            {
                if (!m_out || m_indent <= 0)
                    return;
                m_out->append(m_newline);
                m_out->append(static_cast<std::size_t>(depth) * static_cast<std::size_t>(m_indent), ' ');
            }

            bool Value(int depth)
            {
                if (depth > kMaxDepth)
                    return Fail("nested deeper than 512 levels");
                if (m_at >= m_text.size())
                    return Fail("expected a value");
                const char c = m_text[m_at];
                if (c == '{')
                    return Container(depth, '}', true);
                if (c == '[')
                    return Container(depth, ']', false);
                if (c == '"')
                    return String();
                if (c == '-' || IsDigit(c))
                    return Number();
                for (const std::string_view word : { std::string_view("true"), std::string_view("false"), std::string_view("null") })
                {
                    if (m_text.substr(m_at, word.size()) == word)
                    {
                        Emit(word);
                        m_at += word.size();
                        return true;
                    }
                }
                return Fail("expected a value");
            }

            bool Container(int depth, char close, bool object)
            {
                Emit(m_text.substr(m_at, 1));
                ++m_at;
                if (!Skip())
                    return false;
                if (At(close))
                {
                    Emit(m_text.substr(m_at, 1));
                    ++m_at;
                    return true;
                }
                const std::string expected = std::string("expected \",\" or \"") + close + "\"";
                for (;;)
                {
                    Break(depth + 1);
                    if (object)
                    {
                        if (!At('"'))
                            return Fail("expected a key in quotes");
                        if (!String() || !Skip())
                            return false;
                        if (!At(':'))
                            return Fail("expected \":\" after the key");
                        ++m_at;
                        Emit(m_indent > 0 ? ": " : ":");
                        if (!Skip())
                            return false;
                    }
                    if (!Value(depth + 1) || !Skip())
                        return false;
                    if (At(','))
                    {
                        Emit(",");
                        ++m_at;
                        if (!Skip())
                            return false;
                        if (At(close))
                            return Fail(std::string("a comma before \"") + close + "\"");
                        continue;
                    }
                    if (At(close))
                    {
                        Break(depth);
                        Emit(m_text.substr(m_at, 1));
                        ++m_at;
                        return true;
                    }
                    return Fail(expected);
                }
            }

            bool String()
            {
                const std::size_t start = m_at;
                ++m_at;
                while (m_at < m_text.size())
                {
                    const unsigned char c = static_cast<unsigned char>(m_text[m_at]);
                    if (c == '"')
                    {
                        ++m_at;
                        Emit(m_text.substr(start, m_at - start));
                        return true;
                    }
                    if (c == '\n' || c == '\r')
                        return Fail("the string is not closed on its line");
                    if (c < 0x20)
                        return Fail("a control character inside a string");
                    if (c == '\\')
                    {
                        if (m_at + 1 >= m_text.size())
                            break;
                        const char escape = m_text[m_at + 1];
                        if (escape == 'u')
                        {
                            for (std::size_t k = 2; k < 6; ++k)
                            {
                                if (m_at + k >= m_text.size() || !IsHex(m_text[m_at + k]))
                                    return Fail("\\u needs four hex digits");
                            }
                            m_at += 6;
                            continue;
                        }
                        if (std::string_view("\"\\/bfnrt").find(escape) == npos)
                            return Fail(std::string("\\") + escape + " is no JSON escape");
                        m_at += 2;
                        continue;
                    }
                    ++m_at;
                }
                m_at = start;
                return Fail("the string is not closed");
            }

            bool Number()
            {
                const std::size_t start = m_at;
                if (At('-'))
                    ++m_at;
                if (!Digit())
                    return Fail("expected a digit");
                if (At('0'))
                {
                    ++m_at;
                    if (Digit())
                        return Fail("a number with a leading zero");
                }
                while (Digit())
                    ++m_at;
                if (At('.'))
                {
                    ++m_at;
                    if (!Digit())
                        return Fail("a digit must follow the decimal point");
                    while (Digit())
                        ++m_at;
                }
                if (At('e') || At('E'))
                {
                    ++m_at;
                    if (At('+') || At('-'))
                        ++m_at;
                    if (!Digit())
                        return Fail("a digit must follow the exponent");
                    while (Digit())
                        ++m_at;
                }
                Emit(m_text.substr(start, m_at - start));
                return true;
            }

            std::string_view m_text;
            int m_indent = 0;
            std::string* m_out = nullptr;
            std::string_view m_newline;
            std::size_t m_at = 0;
            std::size_t m_errorAt = 0;
            std::string m_message;
        };

        JsonProblem Locate(std::string_view text, std::size_t offset, std::string message)
        {
            JsonProblem problem;
            problem.Offset = std::min(offset, text.size());
            problem.Message = std::move(message);
            std::size_t lineStart = 0;
            for (std::size_t i = 0; i < problem.Offset; ++i)
            {
                if (text[i] == '\n')
                {
                    ++problem.Line;
                    lineStart = i + 1;
                }
            }
            for (std::size_t i = lineStart; i < problem.Offset; ++i)
            {
                if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
                    ++problem.Column;
            }
            return problem;
        }
    }

    JsonFormatted FormatJson(std::string_view text, int indent)
    {
        JsonFormatted result;
        result.Text.reserve(indent > 0 ? text.size() + text.size() / 2 : text.size());
        Parser parser(text, std::clamp(indent, 0, 16), &result.Text);
        if (!parser.Run())
        {
            result.Text.clear();
            result.Problem = Locate(text, parser.ErrorAt(), parser.Message());
        }
        return result;
    }

    std::optional<JsonProblem> CheckJson(std::string_view text)
    {
        Parser parser(text, 0, nullptr);
        if (parser.Run())
            return std::nullopt;
        return Locate(text, parser.ErrorAt(), parser.Message());
    }

    bool LooksLikeJson(std::string_view text)
    {
        if (text.substr(0, kBom.size()) == kBom)
            text.remove_prefix(kBom.size());
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        return first != npos && (text[first] == '{' || text[first] == '[');
    }
}
