#include "Core/Xml.h"

#include "Core/Text.h"

#include <cstdlib>

namespace Manifold::Xml
{
    namespace
    {
        void AppendCodePoint(std::string& out, unsigned long code)
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

        std::string Decode(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] != '&')
                {
                    out += text[i];
                    continue;
                }
                const std::size_t end = text.find(';', i);
                if (end == std::string_view::npos || end - i > 10)
                {
                    out += text[i];
                    continue;
                }
                const std::string_view entity = text.substr(i + 1, end - i - 1);
                if (entity == "amp") out += '&';
                else if (entity == "lt") out += '<';
                else if (entity == "gt") out += '>';
                else if (entity == "quot") out += '"';
                else if (entity == "apos") out += '\'';
                else if (!entity.empty() && entity[0] == '#')
                {
                    // ∑ Only a character XML allows becomes text: no NUL, no surrogate half, nothing past U+10FFFF, and only
                    //   digits (strtoul took "&#;" as 0 and skipped signs and spaces). Anything else stays as written.
                    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    const std::string_view digits = entity.substr(hex ? 2 : 1);
                    unsigned long code = 0;
                    bool valid = !digits.empty() && digits.size() <= 8;
                    for (const char c : digits)
                    {
                        const int digit = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                        if (digit < 0)
                        {
                            valid = false;
                            break;
                        }
                        code = code * (hex ? 16 : 10) + static_cast<unsigned long>(digit);
                    }
                    valid = valid && code != 0 && code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF);
                    if (valid)
                        AppendCodePoint(out, code);
                    else
                        out.append(text.substr(i, end - i + 1));
                }
                else
                {
                    out.append(text.substr(i, end - i + 1));
                }
                i = end;
            }
            return out;
        }

        class Reader
        {
        public:
            explicit Reader(std::string_view text) : m_text(text) {}

            ParseResult Run()
            {
                ParseResult result;
                if (m_text.size() >= 3 && static_cast<unsigned char>(m_text[0]) == 0xEF && static_cast<unsigned char>(m_text[1]) == 0xBB &&
                    static_cast<unsigned char>(m_text[2]) == 0xBF)
                    m_at = 3;
                SkipMisc();
                if (m_at >= m_text.size() || m_text[m_at] != '<')
                {
                    Fail(result, "no root element");
                    return result;
                }
                result.Root = Element();
                if (!m_error.empty())
                {
                    result.Root.reset();
                    Fail(result, m_error);
                }
                return result;
            }

        private:
            void Fail(ParseResult& result, const std::string& error)
            {
                result.Error = error;
                result.Line = LineAt(m_at);
            }

            int LineAt(std::size_t at) const
            {
                int line = 1;
                for (std::size_t i = 0; i < at && i < m_text.size(); ++i)
                {
                    if (m_text[i] == '\n')
                        ++line;
                }
                return line;
            }

            bool StartsWith(std::string_view prefix) const
            {
                return m_text.substr(m_at, prefix.size()) == prefix;
            }

            void SkipSpace()
            {
                while (m_at < m_text.size() && static_cast<unsigned char>(m_text[m_at]) <= ' ')
                    ++m_at;
            }

            void SkipMisc()
            {
                for (;;)
                {
                    SkipSpace();
                    if (StartsWith("<?"))
                    {
                        const std::size_t end = m_text.find("?>", m_at);
                        m_at = end == std::string_view::npos ? m_text.size() : end + 2;
                    }
                    else if (StartsWith("<!--"))
                    {
                        const std::size_t end = m_text.find("-->", m_at);
                        m_at = end == std::string_view::npos ? m_text.size() : end + 3;
                    }
                    else if (StartsWith("<!DOCTYPE"))
                    {
                        const std::size_t end = m_text.find('>', m_at);
                        m_at = end == std::string_view::npos ? m_text.size() : end + 1;
                    }
                    else
                    {
                        return;
                    }
                }
            }

            std::string Name()
            {
                const std::size_t start = m_at;
                while (m_at < m_text.size())
                {
                    const char c = m_text[m_at];
                    if (static_cast<unsigned char>(c) <= ' ' || c == '>' || c == '/' || c == '=')
                        break;
                    ++m_at;
                }
                return std::string(m_text.substr(start, m_at - start));
            }

            std::unique_ptr<Node> Element(int depth = 1)
            {
                auto node = std::make_unique<Node>();
                if (depth > kMaxDepth)
                {
                    m_error = "elements are nested deeper than " + std::to_string(kMaxDepth) + " levels";
                    return node;
                }
                ++m_at;
                node->Name = Name();
                if (node->Name.empty())
                {
                    m_error = "element without name";
                    return node;
                }
                for (;;)
                {
                    SkipSpace();
                    if (m_at >= m_text.size())
                    {
                        m_error = "unexpected end inside <" + node->Name + ">";
                        return node;
                    }
                    if (StartsWith("/>"))
                    {
                        m_at += 2;
                        return node;
                    }
                    if (m_text[m_at] == '>')
                    {
                        ++m_at;
                        break;
                    }
                    std::string attribute = Name();
                    SkipSpace();
                    if (attribute.empty() || m_at >= m_text.size() || m_text[m_at] != '=')
                    {
                        m_error = "bad attribute in <" + node->Name + ">";
                        return node;
                    }
                    ++m_at;
                    SkipSpace();
                    if (m_at >= m_text.size() || (m_text[m_at] != '"' && m_text[m_at] != '\''))
                    {
                        m_error = "attribute value without quotes in <" + node->Name + ">";
                        return node;
                    }
                    const char quote = m_text[m_at++];
                    const std::size_t end = m_text.find(quote, m_at);
                    if (end == std::string_view::npos)
                    {
                        m_error = "unterminated attribute in <" + node->Name + ">";
                        return node;
                    }
                    node->Attributes.emplace_back(std::move(attribute), Decode(m_text.substr(m_at, end - m_at)));
                    m_at = end + 1;
                }

                for (;;)
                {
                    if (m_at >= m_text.size())
                    {
                        m_error = "<" + node->Name + "> is not closed";
                        return node;
                    }
                    if (StartsWith("</"))
                    {
                        m_at += 2;
                        const std::string closing = Name();
                        SkipSpace();
                        if (closing != node->Name || m_at >= m_text.size() || m_text[m_at] != '>')
                        {
                            m_error = "</" + closing + "> does not close <" + node->Name + ">";
                            return node;
                        }
                        ++m_at;
                        return node;
                    }
                    if (StartsWith("<!--"))
                    {
                        const std::size_t end = m_text.find("-->", m_at);
                        m_at = end == std::string_view::npos ? m_text.size() : end + 3;
                        continue;
                    }
                    if (StartsWith("<![CDATA["))
                    {
                        const std::size_t end = m_text.find("]]>", m_at);
                        if (end == std::string_view::npos)
                        {
                            m_error = "unterminated CDATA";
                            return node;
                        }
                        node->Text.append(m_text.substr(m_at + 9, end - m_at - 9));
                        m_at = end + 3;
                        continue;
                    }
                    if (m_text[m_at] == '<')
                    {
                        node->Children.push_back(Element(depth + 1));
                        if (!m_error.empty())
                            return node;
                        continue;
                    }
                    const std::size_t end = m_text.find('<', m_at);
                    const std::size_t stop = end == std::string_view::npos ? m_text.size() : end;
                    node->Text += Decode(m_text.substr(m_at, stop - m_at));
                    m_at = stop;
                }
            }

            std::string_view m_text;
            std::size_t m_at = 0;
            std::string m_error;
        };

        bool NeedsCdata(const std::string& text)
        {
            return text.find('\n') != std::string::npos || text.find('<') != std::string::npos || text.find('&') != std::string::npos;
        }

        void WriteNode(const Node& node, std::string& out, int depth)
        {
            const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
            out += indent + "<" + node.Name;
            for (const auto& [name, value] : node.Attributes)
                out += " " + name + "=\"" + EscapeXml(value) + "\"";
            if (node.Children.empty() && node.Text.empty())
            {
                out += "/>\n";
                return;
            }
            out += ">";
            if (!node.Children.empty())
            {
                out += "\n";
                for (const auto& child : node.Children)
                    WriteNode(*child, out, depth + 1);
                out += indent;
            }
            else if (NeedsCdata(node.Text) && node.Text.find("]]>") == std::string::npos)
            {
                out += "<![CDATA[" + node.Text + "]]>";
            }
            else
            {
                out += EscapeXml(node.Text);
            }
            out += "</" + node.Name + ">\n";
        }
    }

    const Node* Node::Child(std::string_view name) const
    {
        for (const auto& child : Children)
        {
            if (child->Name == name)
                return child.get();
        }
        return nullptr;
    }

    Node* Node::Child(std::string_view name)
    {
        for (const auto& child : Children)
        {
            if (child->Name == name)
                return child.get();
        }
        return nullptr;
    }

    std::vector<const Node*> Node::ChildrenNamed(std::string_view name) const
    {
        std::vector<const Node*> list;
        for (const auto& child : Children)
        {
            if (child->Name == name)
                list.push_back(child.get());
        }
        return list;
    }

    std::string Node::ChildText(std::string_view name, std::string_view fallback) const
    {
        const Node* child = Child(name);
        return child ? child->Text : std::string(fallback);
    }

    std::optional<std::string> Node::Attribute(std::string_view name) const
    {
        for (const auto& [key, value] : Attributes)
        {
            if (key == name)
                return value;
        }
        return std::nullopt;
    }

    Node& Node::Add(std::string name, std::string text)
    {
        auto child = std::make_unique<Node>();
        child->Name = std::move(name);
        child->Text = std::move(text);
        Children.push_back(std::move(child));
        return *Children.back();
    }

    Node& Node::SetAttribute(std::string name, std::string value)
    {
        for (auto& [key, existing] : Attributes)
        {
            if (key == name)
            {
                existing = std::move(value);
                return *this;
            }
        }
        Attributes.emplace_back(std::move(name), std::move(value));
        return *this;
    }

    ParseResult Parse(std::string_view text)
    {
        Reader reader(text);
        return reader.Run();
    }

    std::string Write(const Node& root, bool header)
    {
        std::string out = header ? "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" : "";
        WriteNode(root, out, 0);
        return out;
    }
}
