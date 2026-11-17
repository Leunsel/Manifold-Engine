#include "Lua/DocText.h"

#include "Core/Text.h"

#include <algorithm>

namespace Manifold::Lua
{
    namespace
    {
        bool IsSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        bool IsWordChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        bool Opens(char c, bool angles)
        {
            return c == '(' || c == '[' || c == '{' || (angles && c == '<');
        }

        bool Closes(char c, bool angles)
        {
            return c == ')' || c == ']' || c == '}' || (angles && c == '>');
        }

        std::size_t FindTopLevel(std::string_view text, char wanted, bool angles)
        {
            int depth = 0;
            char quote = 0;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                if (quote)
                {
                    if (c == '\\')
                        ++i;
                    else if (c == quote)
                        quote = 0;
                    continue;
                }
                if (c == wanted && depth == 0)
                    return i;
                if (c == '"' || c == '\'')
                    quote = c;
                else if (Opens(c, angles))
                    ++depth;
                else if (Closes(c, angles) && depth > 0)
                    --depth;
            }
            return std::string_view::npos;
        }

        std::vector<std::string> Words(std::string_view text)
        {
            std::vector<std::string> words;
            int depth = 0;
            std::size_t start = std::string_view::npos;
            for (std::size_t i = 0; i <= text.size(); ++i)
            {
                const char c = i < text.size() ? text[i] : ' ';
                if (Opens(c, false))
                    ++depth;
                else if (Closes(c, false) && depth > 0)
                    --depth;
                if (IsSpace(c) && depth == 0)
                {
                    if (start != std::string_view::npos)
                        words.emplace_back(text.substr(start, i - start));
                    start = std::string_view::npos;
                }
                else if (start == std::string_view::npos)
                {
                    start = i;
                }
            }
            return words;
        }

        bool IsOptionalWord(std::string_view word)
        {
            while (!word.empty() && (word.back() == ':' || word.back() == ','))
                word.remove_suffix(1);
            return EqualsNoCase(word, "OPTIONAL") || EqualsNoCase(word, "OPT") || EqualsNoCase(word, "SEMIOPTIONAL") || EqualsNoCase(word, "HALFOPTIONAL");
        }

        bool IsTypeWord(std::string_view word)
        {
            static constexpr std::string_view kTypes[] = { "bool", "boolean", "int", "integer", "float", "double", "single", "number", "string", "table", "function" };
            return std::any_of(std::begin(kTypes), std::end(kTypes), [&](std::string_view type) { return EqualsNoCase(word, type); });
        }

        bool Quoted(std::string_view text)
        {
            return text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front();
        }

        std::string Join(const std::vector<std::string>& parts, std::string_view separator)
        {
            std::string result;
            for (const std::string& part : parts)
            {
                if (!result.empty())
                    result += separator;
                result += part;
            }
            return result;
        }

        std::string Collapse(std::string_view text)
        {
            std::string result;
            bool space = false;
            for (const char c : text)
            {
                if (IsSpace(c))
                {
                    space = !result.empty();
                    continue;
                }
                if (space)
                    result += ' ';
                space = false;
                result += c;
            }
            return result;
        }
    }

    const char* DocKindName(DocItemKind kind)
    {
        switch (kind)
        {
        case DocItemKind::Function: return "function";
        case DocItemKind::Method: return "method";
        case DocItemKind::Property: return "property";
        case DocItemKind::Event: return "event";
        case DocItemKind::Table: return "table";
        case DocItemKind::Class: return "class";
        case DocItemKind::Constant: return "constant";
        case DocItemKind::Keyword: return "keyword";
        case DocItemKind::Field: return "field";
        }
        return "value";
    }

    const char* DocOriginName(DocOrigin origin)
    {
        switch (origin)
        {
        case DocOrigin::Reflection: return "reflection";
        case DocOrigin::CheatEngine: return "cheatengine";
        case DocOrigin::Standard: return "standard";
        case DocOrigin::Annotation: return "annotation";
        case DocOrigin::Script: return "script";
        case DocOrigin::Manifold: return "manifold";
        }
        return "unknown";
    }

    std::string FormatParam(const DocParam& param)
    {
        std::string text = param.Name.empty() ? std::string(param.Variadic ? "..." : "?") : param.Name;
        if (param.Variadic && text != "...")
            text += "...";
        if (param.Optional && !param.Variadic)
            text += '?';
        return text;
    }

    std::string FormatParams(const DocSignature& signature)
    {
        std::string text;
        for (const DocParam& param : signature.Params)
        {
            if (!text.empty())
                text += ", ";
            text += FormatParam(param);
        }
        return text;
    }

    std::string FormatCall(const DocRecord& record, std::size_t signature, bool fullPath)
    {
        std::string text = fullPath ? record.Path : record.Name;
        if (record.Indexed)
            text += "[]";
        if (!record.Callable())
            return text;
        if (signature < record.Signatures.size())
            return text + "(" + FormatParams(record.Signatures[signature]) + ")";
        return text + "()";
    }

    std::string FirstSentence(std::string_view text, std::size_t limit)
    {
        std::size_t end = text.size();
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] == '\n')
            {
                end = i;
                break;
            }
            if (text[i] != '.' || (i + 1 < text.size() && !IsSpace(text[i + 1])))
                continue;
            if (i >= 3 && (text.substr(i - 3, 3) == "e.g" || text.substr(i - 3, 3) == "i.e"))
                continue;
            end = i + 1;
            break;
        }
        std::string sentence = Collapse(text.substr(0, end));
        if (sentence.size() > limit)
        {
            std::size_t cut = sentence.rfind(' ', limit);
            if (cut == std::string::npos || cut < limit / 2)
                cut = limit;
            sentence = sentence.substr(0, cut);
            while (!sentence.empty() && (sentence.back() == ',' || sentence.back() == ';' || sentence.back() == ' '))
                sentence.pop_back();
            sentence += "\xE2\x80\xA6";
        }
        return sentence;
    }

    void SplitPath(std::string_view path, std::string& owner, std::string& name)
    {
        const std::size_t separator = path.find_last_of(".:");
        if (separator == std::string_view::npos)
        {
            owner.clear();
            name = std::string(path);
            return;
        }
        owner = std::string(path.substr(0, separator));
        std::replace(owner.begin(), owner.end(), ':', '.');
        name = std::string(path.substr(separator + 1));
    }

    std::vector<std::string> SplitTopLevel(std::string_view text, std::string_view separators, bool angles)
    {
        std::vector<std::string> parts;
        int depth = 0;
        char quote = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (quote)
            {
                if (c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\'')
                quote = c;
            else if (Opens(c, angles))
                ++depth;
            else if (Closes(c, angles) && depth > 0)
                --depth;
            else if (depth == 0 && separators.find(c) != std::string_view::npos)
            {
                parts.push_back(Trim(text.substr(start, i - start)));
                start = i + 1;
            }
        }
        if (!TrimView(text.substr(std::min(start, text.size()))).empty())
            parts.push_back(Trim(text.substr(start)));
        return parts;
    }

    std::size_t TypeEnd(std::string_view text, std::size_t from)
    {
        int depth = 0;
        char quote = 0;
        std::size_t i = from;
        while (i < text.size())
        {
            const char c = text[i];
            if (quote)
            {
                if (c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                ++i;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`')
            {
                quote = c;
            }
            else if (Opens(c, true))
            {
                ++depth;
            }
            else if (Closes(c, true))
            {
                if (depth == 0)
                    return i;
                --depth;
            }
            else if (depth == 0 && (c == ',' || c == '#'))
            {
                return i;
            }
            else if (depth == 0 && IsSpace(c))
            {
                std::size_t next = i;
                while (next < text.size() && IsSpace(text[next]))
                    ++next;
                std::size_t previous = i;
                while (previous > from && IsSpace(text[previous - 1]))
                    --previous;
                const char before = previous > from ? text[previous - 1] : '\0';
                if ((next < text.size() && text[next] == '|') || before == '|' || before == ':')
                {
                    i = next;
                    continue;
                }
                return i;
            }
            ++i;
        }
        return text.size();
    }

    std::string NormalizeType(std::string_view type)
    {
        type = TrimView(type);
        if (EqualsNoCase(type, "bool") || EqualsNoCase(type, "boolean"))
            return "boolean";
        if (EqualsNoCase(type, "int") || EqualsNoCase(type, "integer"))
            return "integer";
        if (EqualsNoCase(type, "float") || EqualsNoCase(type, "double") || EqualsNoCase(type, "single") || EqualsNoCase(type, "number"))
            return "number";
        if (EqualsNoCase(type, "string") || EqualsNoCase(type, "table") || EqualsNoCase(type, "function"))
            return Lower(type);
        return std::string(type);
    }

    void ApplyTypeDetails(DocParam& param)
    {
        param.Name = Trim(param.Name);
        if (!param.Name.empty() && param.Name.back() == '?')
        {
            param.Optional = true;
            param.Name.pop_back();
        }
        if (param.Name == "...")
            param.Variadic = true;
        std::string type = Trim(param.Type);
        if (!type.empty() && type.back() == '?')
        {
            param.Optional = true;
            type.pop_back();
        }
        if (type.empty())
        {
            param.Type.clear();
            return;
        }
        if (type.size() >= 2 && type.front() == '{' && type.back() == '}')
        {
            for (const std::string& field : SplitTopLevel(std::string_view(type).substr(1, type.size() - 2), ",", true))
            {
                const std::size_t colon = FindTopLevel(field, ':', true);
                if (colon == std::string::npos || colon == 0)
                    continue;
                std::string key = Trim(std::string_view(field).substr(0, colon));
                if (!key.empty() && key.back() == '?')
                    key.pop_back();
                param.Keys.emplace_back(std::move(key), Trim(std::string_view(field).substr(colon + 1)));
            }
            if (!param.Keys.empty())
            {
                param.Type = "table";
                return;
            }
        }
        std::vector<std::string> kept;
        for (std::string& part : SplitTopLevel(type, "|", true))
        {
            if (Quoted(part))
            {
                const std::string value = part.substr(1, part.size() - 2);
                if (std::find(param.Choices.begin(), param.Choices.end(), value) == param.Choices.end())
                    param.Choices.push_back(value);
            }
            else if (part == "nil")
            {
                param.Optional = true;
            }
            else if (!part.empty())
            {
                kept.push_back(std::move(part));
            }
        }
        if (!param.Choices.empty() && std::find(kept.begin(), kept.end(), "string") == kept.end())
            kept.emplace_back("string");
        param.Type = Join(kept, "|");
    }

    DocSignature ParseLooseParams(std::string_view params)
    {
        DocSignature signature;
        for (const std::string& part : SplitTopLevel(params, ",;", false))
        {
            DocParam param;
            std::vector<std::string> kept;
            for (std::string& word : Words(part))
            {
                if (IsOptionalWord(word))
                    param.Optional = true;
                else
                    kept.push_back(std::move(word));
            }
            std::string text = Join(kept, " ");
            if (text.empty())
                continue;
            if (text.size() > 2 && text.front() == '[' && text.back() == ']')
            {
                param.Optional = true;
                text = Trim(std::string_view(text).substr(1, text.size() - 2));
            }
            if (text.size() >= 3 && text.compare(text.size() - 3, 3, "...") == 0)
            {
                param.Variadic = true;
                while (!text.empty() && text.back() == '.')
                    text.pop_back();
                text = Trim(text);
                if (text.empty())
                    text = "...";
            }
            if (const std::size_t open = text.find("(default "); open != std::string::npos && text.back() == ')')
            {
                param.Default = Trim(std::string_view(text).substr(open + 9, text.size() - open - 10));
                text = Trim(std::string_view(text).substr(0, open));
            }
            if (const std::size_t equals = FindTopLevel(text, '=', false); equals != std::string::npos && equals > 0)
            {
                param.Default = Trim(std::string_view(text).substr(equals + 1));
                text = Trim(std::string_view(text).substr(0, equals));
            }
            const std::vector<std::string> words = Words(text);
            if (Quoted(text))
            {
                param.Name = text.substr(1, text.size() - 2);
                param.Type = "string";
            }
            else if (text.compare(0, 8, "function") == 0 && (text.size() == 8 || !IsWordChar(text[8])))
            {
                param.Type = "function";
                const std::string rest = Trim(std::string_view(text).substr(8));
                std::size_t identifier = 0;
                while (identifier < rest.size() && IsWordChar(rest[identifier]))
                    ++identifier;
                param.Name = identifier > 0 ? rest.substr(0, identifier) : text;
            }
            else if (const std::size_t colon = FindTopLevel(text, ':', false); colon != std::string::npos && colon > 0 && text.front() != '{')
            {
                param.Name = Trim(std::string_view(text).substr(0, colon));
                param.Type = NormalizeType(std::string_view(text).substr(colon + 1));
            }
            else if (words.size() == 2 && IsTypeWord(words[0]) && !IsTypeWord(words[1]))
            {
                param.Type = NormalizeType(words[0]);
                param.Name = words[1];
            }
            else if (words.size() == 2 && IsTypeWord(words[1]))
            {
                param.Name = words[0];
                param.Type = NormalizeType(words[1]);
            }
            else
            {
                param.Name = text;
            }
            if (param.Name.size() > 2 && param.Name.compare(param.Name.size() - 2, 2, "()") == 0 && param.Name.find('(') == param.Name.size() - 2)
            {
                param.Name.resize(param.Name.size() - 2);
                if (param.Type.empty())
                    param.Type = "function";
            }
            if (param.Name == "...")
                param.Variadic = true;
            signature.Params.push_back(std::move(param));
        }
        return signature;
    }

    CeProperty ParseCeProperty(std::string_view text)
    {
        CeProperty property;
        const std::string_view trimmed = TrimView(text);
        if (!trimmed.empty() && trimmed.front() == '\'')
        {
            std::vector<std::string> values;
            bool literals = true;
            for (const std::string& part : SplitTopLevel(trimmed, ",", false))
            {
                if (!Quoted(part))
                {
                    literals = false;
                    break;
                }
                values.push_back(part.substr(1, part.size() - 2));
            }
            if (literals && values.size() > 1)
            {
                property.Type = "string";
                property.Choices = std::move(values);
                return property;
            }
        }
        const std::size_t dash = trimmed.find(" - ");
        if (dash != std::string_view::npos && dash > 0 && dash <= 40 && trimmed.substr(0, dash).find(". ") == std::string_view::npos)
        {
            const std::string head = Trim(trimmed.substr(0, dash));
            property.Type = Words(head).size() == 1 ? NormalizeType(head) : head;
            property.Text = Trim(trimmed.substr(dash + 3));
            return property;
        }
        const bool function = trimmed.compare(0, 8, "function") == 0 && trimmed.find(')') != std::string_view::npos && trimmed.find(". ") == std::string_view::npos &&
                              trimmed.find(' ', trimmed.find(')')) == std::string_view::npos;
        const bool word = !trimmed.empty() && trimmed.size() <= 40 && trimmed.find(' ') == std::string_view::npos;
        if (function || word)
        {
            property.Type = function ? std::string(trimmed) : NormalizeType(trimmed);
            return property;
        }
        property.Text = std::string(trimmed);
        return property;
    }

    bool ParseFunType(std::string_view text, DocSignature& signature)
    {
        text = TrimView(text);
        if (text.compare(0, 4, "fun(") != 0)
            return false;
        int depth = 0;
        std::size_t close = std::string_view::npos;
        for (std::size_t i = 3; i < text.size(); ++i)
        {
            if (text[i] == '(')
                ++depth;
            else if (text[i] == ')' && --depth == 0)
            {
                close = i;
                break;
            }
        }
        if (close == std::string_view::npos)
            return false;
        signature = DocSignature{};
        for (const std::string& part : SplitTopLevel(text.substr(4, close - 4), ",", true))
        {
            DocParam param;
            const std::size_t colon = FindTopLevel(part, ':', true);
            param.Name = colon == std::string::npos ? part : Trim(std::string_view(part).substr(0, colon));
            param.Type = colon == std::string::npos ? std::string() : Trim(std::string_view(part).substr(colon + 1));
            ApplyTypeDetails(param);
            signature.Params.push_back(std::move(param));
        }
        const std::string_view rest = TrimView(text.substr(close + 1));
        if (!rest.empty() && rest.front() == ':')
            signature.Returns = Trim(rest.substr(1));
        return true;
    }
}
