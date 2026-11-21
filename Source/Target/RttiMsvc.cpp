#include "Target/RttiMsvc.h"

#include <algorithm>
#include <map>
#include <optional>

namespace Manifold::Target
{
    namespace
    {
        constexpr int kMaxDepth = 200;
        constexpr std::size_t kMaxParts = 64;
        // Back-references copy whole remembered names, so nested templates can grow the text
        // exponentially from a few hundred input bytes; no real type name comes near this.
        constexpr std::size_t kMaxText = 4096;

        struct Parser
        {
            struct Entry
            {
                std::string Key;
                std::string Text;
            };

            std::string_view Source;
            std::size_t At = 0;
            bool Failed = false;
            int Depth = 0;
            std::vector<Entry> Names;
            std::vector<std::string> Params;

            bool Fail()
            {
                Failed = true;
                return false;
            }

            bool End() const { return At >= Source.size(); }

            std::string_view Peek(std::size_t count = 1) const { return At < Source.size() ? Source.substr(At, count) : std::string_view(); }

            char Take()
            {
                if (Failed || At >= Source.size())
                {
                    Failed = true;
                    return '\0';
                }
                return Source[At++];
            }

            bool Skip(std::string_view text)
            {
                if (Failed || Source.substr(std::min(At, Source.size()), text.size()) != text)
                    return false;
                At += text.size();
                return true;
            }

            // Fails the parse when a piece grew past kMaxText; returns whether it may be used.
            bool Fits(const std::string& text)
            {
                if (text.size() > kMaxText)
                    Failed = true;
                return !Failed;
            }

            void Memorize(std::string key, std::string text)
            {
                if (!Fits(text) || Names.size() >= 10)
                    return;
                for (const Entry& entry : Names)
                {
                    if (entry.Key == key)
                        return;
                }
                Names.push_back({ std::move(key), std::move(text) });
            }
        };

        struct Nested
        {
            explicit Nested(Parser& parser) : P(parser)
            {
                if (++P.Depth > kMaxDepth)
                    P.Failed = true;
            }
            ~Nested() { --P.Depth; }
            Parser& P;
        };

        bool IsDigit(char c) { return c >= '0' && c <= '9'; }

        std::string QualifiedName(Parser& p);
        std::string Type(Parser& p);
        std::string FunctionType(Parser& p, std::string_view sigil = {});
        std::string TemplateArguments(Parser& p);

        const char* Primitive(char c)
        {
            switch (c)
            {
            case 'C': return "signed char";
            case 'D': return "char";
            case 'E': return "unsigned char";
            case 'F': return "short";
            case 'G': return "unsigned short";
            case 'H': return "int";
            case 'I': return "unsigned int";
            case 'J': return "long";
            case 'K': return "unsigned long";
            case 'M': return "float";
            case 'N': return "double";
            case 'O': return "long double";
            case 'X': return "void";
            default: return nullptr;
            }
        }

        const char* Extended(char c)
        {
            switch (c)
            {
            case 'N': return "bool";
            case 'J': return "__int64";
            case 'K': return "unsigned __int64";
            case 'W': return "wchar_t";
            case 'S': return "char16_t";
            case 'U': return "char32_t";
            case 'Q': return "char8_t";
            case 'D': return "__int8";
            case 'E': return "unsigned __int8";
            case 'F': return "__int16";
            case 'G': return "unsigned __int16";
            case 'H': return "__int32";
            case 'I': return "unsigned __int32";
            case 'L': return "__int128";
            case 'M': return "unsigned __int128";
            default: return nullptr;
            }
        }

        std::optional<std::string_view> Qualifier(char c)
        {
            switch (c)
            {
            case 'A': return std::string_view();
            case 'B': return std::string_view("const");
            case 'C': return std::string_view("volatile");
            case 'D': return std::string_view("const volatile");
            default: return std::nullopt;
            }
        }

        bool IsConvention(char c)
        {
            return std::string_view("ABCDEFGHIJMNQSUW").find(c) != std::string_view::npos && c != '\0';
        }

        std::string Identifier(Parser& p)
        {
            if (p.Failed)
                return {};
            const std::size_t stop = p.Source.find('@', p.At);
            if (stop == std::string_view::npos || stop == p.At)
            {
                p.Fail();
                return {};
            }
            std::string text(p.Source.substr(p.At, stop - p.At));
            p.At = stop + 1;
            return text;
        }

        long long EncodedNumber(Parser& p)
        {
            const bool negative = p.Skip("?");
            char c = p.Take();
            long long value = 0;
            if (IsDigit(c))
            {
                value = c - '0' + 1;
            }
            else
            {
                while (!p.Failed && c != '@')
                {
                    const int digit = c - 'A';
                    if (digit < 0 || digit > 15)
                    {
                        p.Fail();
                        return 0;
                    }
                    value = value * 16 + digit;
                    c = p.Take();
                }
            }
            return negative ? -value : value;
        }

        bool IsLocalScope(const Parser& p)
        {
            if (p.Peek() != "?")
                return false;
            const std::string_view rest = p.Source.substr(std::min(p.At + 1, p.Source.size()), 24);
            if (rest.size() >= 2 && IsDigit(rest[0]) && rest[1] == '?')
                return true;
            if (rest.size() >= 2 && rest[0] == '@' && rest[1] == '?')
                return true;
            if (rest.empty() || rest[0] < 'B' || rest[0] > 'P')
                return false;
            std::size_t at = 1;
            while (at < rest.size() && rest[at] >= 'A' && rest[at] <= 'P')
                ++at;
            return at + 1 < rest.size() && rest[at] == '@' && rest[at + 1] == '?';
        }

        std::string FunctionSymbol(Parser& p)
        {
            const Nested nested(p);
            if (p.Failed || !p.Skip("?"))
            {
                p.Fail();
                return {};
            }
            std::string name = QualifiedName(p);
            const char code = p.Take();
            if (p.Failed)
                return {};
            if (code == 'Y' || code == 'Z')
            {
                FunctionType(p);
            }
            else if (std::string_view("ABEFIJMNQRUV").find(code) != std::string_view::npos)
            {
                p.Skip("E");
                if (!Qualifier(p.Take()))
                {
                    p.Fail();
                    return {};
                }
                FunctionType(p);
            }
            else if (std::string_view("CDKLST").find(code) != std::string_view::npos)
            {
                FunctionType(p);
            }
            else
            {
                p.Fail();
                return {};
            }
            return name + "()";
        }

        std::string Fragment(Parser& p)
        {
            const Nested nested(p);
            if (p.Failed)
                return {};
            const std::string_view next = p.Peek();
            const char c = next.empty() ? '\0' : next[0];
            if (IsDigit(c))
            {
                ++p.At;
                const std::size_t index = static_cast<std::size_t>(c - '0');
                if (index >= p.Names.size())
                {
                    p.Fail();
                    return {};
                }
                return p.Names[index].Text;
            }
            if (p.Skip("?$"))
            {
                std::vector<Parser::Entry> outerNames = std::move(p.Names);
                std::vector<std::string> outerParams = std::move(p.Params);
                p.Names.clear();
                p.Params.clear();
                const std::string name = Identifier(p);
                p.Memorize(name, name);
                const std::string arguments = TemplateArguments(p);
                p.Names = std::move(outerNames);
                p.Params = std::move(outerParams);
                if (p.Failed)
                    return {};
                std::string text = name + "<" + arguments + ">";
                if (!p.Fits(text))
                    return {};
                p.Memorize(text, text);
                return text;
            }
            if (p.Peek(2) == "?A")
            {
                ++p.At;
                const std::string key = Identifier(p);
                p.Memorize(key, "`anonymous namespace'");
                return "`anonymous namespace'";
            }
            if (IsLocalScope(p))
            {
                ++p.At;
                EncodedNumber(p);
                if (!p.Skip("?"))
                {
                    p.Fail();
                    return {};
                }
                return FunctionSymbol(p);
            }
            if (c == '?')
            {
                p.Fail();
                return {};
            }
            std::string name = Identifier(p);
            p.Memorize(name, name);
            return name;
        }

        std::string QualifiedName(Parser& p)
        {
            const Nested nested(p);
            std::vector<std::string> parts;
            while (!p.Failed && !p.Skip("@"))
            {
                if (p.End())
                {
                    p.Fail();
                    break;
                }
                parts.push_back(Fragment(p));
                if (parts.size() > kMaxParts)
                    p.Fail();
            }
            if (p.Failed || parts.empty())
            {
                p.Fail();
                return {};
            }
            std::string text;
            for (auto part = parts.rbegin(); part != parts.rend(); ++part)
            {
                if (!text.empty())
                    text += "::";
                text += *part;
            }
            if (!p.Fits(text))
                return {};
            return text;
        }

        std::string TemplateArguments(Parser& p)
        {
            const Nested nested(p);
            std::vector<std::string> list;
            while (!p.Failed && !p.Skip("@"))
            {
                if (p.End())
                {
                    p.Fail();
                    break;
                }
                if (p.Skip("$$V") || p.Skip("$$Z"))
                {
                }
                else if (p.Skip("$0"))
                {
                    list.push_back(std::to_string(EncodedNumber(p)));
                }
                else if (p.Peek() == "$" && p.Peek(2) != "$$")
                {
                    p.Fail();
                }
                else
                {
                    list.push_back(Type(p));
                }
                if (list.size() > kMaxParts)
                    p.Fail();
            }
            if (p.Failed)
                return {};
            std::string text;
            for (const std::string& item : list)
            {
                if (!text.empty())
                    text += ", ";
                text += item;
            }
            if (!p.Fits(text))
                return {};
            return text;
        }

        std::string Qualified(std::string_view cv, const std::string& text)
        {
            if (cv.empty())
                return text;
            return std::string(cv) + " " + text;
        }

        std::string Indirection(Parser& p, char kind, std::string_view sigil)
        {
            const Nested nested(p);
            if (p.Failed)
                return {};
            if (p.Skip("6"))
                return FunctionType(p, sigil);
            if (p.Peek() == "8")
            {
                p.Fail();
                return {};
            }
            while (p.Skip("E") || p.Skip("I") || p.Skip("F"))
            {
            }
            const auto cv = Qualifier(p.Take());
            if (!cv)
            {
                p.Fail();
                return {};
            }
            std::string text = Qualified(*cv, Type(p)) + std::string(sigil);
            if (kind == 'Q')
                text += " const";
            else if (kind == 'R')
                text += " volatile";
            else if (kind == 'S')
                text += " const volatile";
            return text;
        }

        std::string Type(Parser& p)
        {
            const Nested nested(p);
            const char c = p.Take();
            if (p.Failed)
                return {};
            if (const char* primitive = Primitive(c))
                return primitive;
            if (c == '_')
            {
                const char* extended = Extended(p.Take());
                if (!extended)
                {
                    p.Fail();
                    return {};
                }
                return extended;
            }
            if (c == 'V' || c == 'U' || c == 'T')
                return QualifiedName(p);
            if (c == 'W')
            {
                p.Take();
                return QualifiedName(p);
            }
            if (c == 'P' || c == 'Q' || c == 'R' || c == 'S')
                return Indirection(p, c, "*");
            if (c == 'A' || c == 'B')
                return Indirection(p, c, "&");
            if (c == '$')
            {
                if (p.Skip("$Q") || p.Skip("$R"))
                    return Indirection(p, 'A', "&&");
                if (p.Skip("$C"))
                {
                    const auto cv = Qualifier(p.Take());
                    if (!cv)
                    {
                        p.Fail();
                        return {};
                    }
                    return Qualified(*cv, Type(p));
                }
                if (p.Skip("$T"))
                    return "std::nullptr_t";
                if (p.Skip("$A6"))
                    return FunctionType(p);
                p.Fail();
                return {};
            }
            if (IsDigit(c))
            {
                const std::size_t index = static_cast<std::size_t>(c - '0');
                if (index >= p.Params.size())
                {
                    p.Fail();
                    return {};
                }
                return p.Params[index];
            }
            p.Fail();
            return {};
        }

        std::string FunctionType(Parser& p, std::string_view sigil)
        {
            const Nested nested(p);
            if (!IsConvention(p.Take()))
            {
                p.Fail();
                return {};
            }
            if (p.Skip("?"))
                p.Take();
            const std::string result = p.Skip("@") ? std::string() : Type(p);
            std::vector<std::string> parameters;
            if (!p.Skip("X"))
            {
                while (!p.Failed)
                {
                    if (p.Skip("@"))
                        break;
                    if (p.Skip("Z"))
                    {
                        parameters.emplace_back("...");
                        break;
                    }
                    if (p.End())
                    {
                        p.Fail();
                        break;
                    }
                    const std::size_t from = p.At;
                    std::string text = Type(p);
                    if (p.At - from > 1 && p.Params.size() < 10 && p.Fits(text))
                        p.Params.push_back(text);
                    parameters.push_back(std::move(text));
                }
            }
            p.Skip("_E");
            if (p.Failed || !p.Skip("Z"))
            {
                p.Fail();
                return {};
            }
            std::string list;
            for (const std::string& item : parameters)
            {
                if (!list.empty())
                    list += ", ";
                list += item;
            }
            if (!sigil.empty())
                return result + " (" + std::string(sigil) + ")(" + list + ")";
            return result + " (" + list + ")";
        }
    }

    bool IsMsvcTypeName(std::string_view decorated)
    {
        if (decorated.substr(0, 3) != ".?A")
            return false;
        return std::none_of(decorated.begin(), decorated.end(), [](char c) { return static_cast<unsigned char>(c) <= 0x20 || c == 0x7F; });
    }

    std::string MsvcDemangle(std::string_view decorated)
    {
        if (decorated.substr(0, 3) != ".?A")
            return std::string(decorated);
        Parser p;
        p.Source = decorated;
        p.At = 3;
        const char kind = p.Take();
        if (kind == 'W')
            p.Take();
        else if (kind != 'V' && kind != 'U' && kind != 'T')
            p.Fail();
        std::string name = QualifiedName(p);
        if (p.Failed || !p.End() || name.empty())
            return std::string(decorated);
        return name;
    }

    std::vector<std::string> MsvcMangle(std::string_view name)
    {
        if (name.substr(0, 3) == ".?A")
            return { std::string(name) };
        const bool plain = std::all_of(name.begin(), name.end(), [](char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':';
            });
        if (!plain || name.find(":::") != std::string_view::npos)
            return {};
        std::vector<std::string> parts;
        std::size_t at = 0;
        while (at < name.size())
        {
            if (name[at] == ':')
            {
                ++at;
                continue;
            }
            std::size_t end = at;
            while (end < name.size() && name[end] != ':')
                ++end;
            parts.emplace(parts.begin(), name.substr(at, end - at));
            at = end;
        }
        if (parts.empty())
            return {};
        std::map<std::string, int> seen;
        std::string body;
        int count = 0;
        for (const std::string& part : parts)
        {
            if (const auto found = seen.find(part); found != seen.end())
            {
                body += std::to_string(found->second);
                continue;
            }
            if (count < 10)
                seen[part] = count;
            ++count;
            body += part + "@";
        }
        body += "@";
        return { ".?AV" + body, ".?AU" + body };
    }
}
