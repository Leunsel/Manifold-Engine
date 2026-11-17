#include "Lua/DocAnnotations.h"

#include "Core/Text.h"
#include "Lua/DocText.h"

#include <algorithm>
#include <optional>

namespace Manifold::Lua
{
    namespace
    {
        constexpr std::string_view kSigma = "\xE2\x88\x91";

        bool IsWordChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        bool IsSpace(char c)
        {
            return c == ' ' || c == '\t';
        }

        std::string_view TrimLeft(std::string_view text)
        {
            while (!text.empty() && IsSpace(text.front()))
                text.remove_prefix(1);
            return text;
        }

        std::string_view Token(std::string_view& text)
        {
            text = TrimLeft(text);
            std::size_t end = 0;
            while (end < text.size() && !IsSpace(text[end]))
                ++end;
            const std::string_view token = text.substr(0, end);
            text = TrimLeft(text.substr(end));
            return token;
        }

        std::string Description(std::string_view text)
        {
            text = TrimLeft(text);
            if (!text.empty() && text.front() == '#')
                text = TrimLeft(text.substr(1));
            if (!text.empty() && text.front() == '@')
                text = TrimLeft(text.substr(1));
            return Trim(text);
        }

        void Append(std::string& target, std::string_view text)
        {
            text = TrimView(text);
            if (text.empty())
                return;
            if (!target.empty() && target.back() != '\n')
                target += ' ';
            target += text;
        }

        struct Definition
        {
            std::string Name;
            std::string Params;
            bool Local = false;
        };

        std::string ParamsAfter(std::string_view line, std::size_t open)
        {
            int depth = 0;
            for (std::size_t i = open; i < line.size(); ++i)
            {
                if (line[i] == '(')
                    ++depth;
                else if (line[i] == ')' && --depth == 0)
                    return Trim(line.substr(open + 1, i - open - 1));
            }
            return Trim(line.substr(open + 1));
        }

        std::optional<Definition> MatchDefinition(std::string_view line)
        {
            Definition definition;
            std::string_view rest = line;
            if (rest.compare(0, 6, "local ") == 0)
            {
                definition.Local = true;
                rest = TrimLeft(rest.substr(6));
            }
            if (rest.compare(0, 8, "function") == 0 && rest.size() > 8 && (IsSpace(rest[8]) || rest[8] == '('))
            {
                rest = TrimLeft(rest.substr(8));
                std::size_t end = 0;
                while (end < rest.size() && (IsWordChar(rest[end]) || rest[end] == '.' || rest[end] == ':'))
                    ++end;
                if (end == 0)
                    return std::nullopt;
                definition.Name = std::string(rest.substr(0, end));
                const std::string_view after = TrimLeft(rest.substr(end));
                if (after.empty() || after.front() != '(')
                    return std::nullopt;
                definition.Params = ParamsAfter(after, 0);
                return definition;
            }
            std::size_t end = 0;
            while (end < rest.size() && (IsWordChar(rest[end]) || rest[end] == '.'))
                ++end;
            if (end == 0 || (rest[0] >= '0' && rest[0] <= '9'))
                return std::nullopt;
            definition.Name = std::string(rest.substr(0, end));
            std::string_view after = TrimLeft(rest.substr(end));
            if (after.empty() || after.front() != '=' || (after.size() > 1 && after[1] == '='))
                return std::nullopt;
            after = TrimLeft(after.substr(1));
            if (after.compare(0, 8, "function") != 0 || after.size() <= 8 || IsWordChar(after[8]))
                return std::nullopt;
            after = TrimLeft(after.substr(8));
            if (after.empty() || after.front() != '(')
                return std::nullopt;
            definition.Params = ParamsAfter(after, 0);
            return definition;
        }

        struct CommentLine
        {
            int Line = 0;
            std::string Text;
        };

        enum class Tag
        {
            None,
            Param,
            Return,
            Field,
            Alias,
            Other
        };

        class BlockParser
        {
        public:
            BlockParser(AnnotatedFile& file, std::string_view source) : m_file(file), m_source(source) {}

            void Parse(const std::vector<CommentLine>& lines)
            {
                for (const CommentLine& line : lines)
                    Line(line.Text, line.Line);
            }

            void Finish()
            {
                if (m_class)
                {
                    m_class->Text = Trim(m_description);
                    m_class->Summary = FirstSentence(m_class->Text);
                    m_file.Classes.push_back(std::move(*m_class));
                    m_class.reset();
                }
            }

            bool Documented() const
            {
                return !Trim(m_description).empty() || m_tags > 0;
            }

            void Apply(const Definition& definition, int line, AnnotatedFunction& function)
            {
                DocRecord& doc = function.Doc;
                std::string path = definition.Name;
                const bool method = path.find(':') != std::string::npos;
                std::replace(path.begin(), path.end(), ':', '.');
                doc.Path = path;
                SplitPath(path, doc.Owner, doc.Name);
                doc.Kind = method ? DocItemKind::Method : DocItemKind::Function;
                doc.Origin = DocOrigin::Annotation;
                doc.Source = std::string(m_source);
                doc.Line = line;
                doc.Text = Trim(m_description);
                doc.Summary = FirstSentence(doc.Text);
                doc.Deprecated = m_deprecated;
                doc.IsDeprecated = m_isDeprecated;
                doc.Hidden = m_hidden;
                doc.Async = m_async;
                doc.See = m_see;

                DocSignature main;
                std::vector<bool> used(m_params.size(), false);
                for (const std::string& part : SplitTopLevel(definition.Params, ",", false))
                {
                    if (part.empty())
                        continue;
                    DocParam param;
                    param.Name = part;
                    for (std::size_t i = 0; i < m_params.size(); ++i)
                    {
                        if (!used[i] && (m_params[i].Name == part || (part == "..." && m_params[i].Variadic)))
                        {
                            param = m_params[i];
                            param.Name = part;
                            used[i] = true;
                            break;
                        }
                    }
                    if (part == "...")
                        param.Variadic = true;
                    main.Params.push_back(std::move(param));
                }
                for (std::size_t i = 0; i < m_params.size(); ++i)
                {
                    if (!used[i] && (definition.Params.empty() || m_params[i].Variadic))
                        main.Params.push_back(m_params[i]);
                }
                main.Returns = m_returns;
                main.ReturnsText = m_returnsText;
                doc.Signatures.push_back(std::move(main));
                for (DocSignature& overload : m_overloads)
                    doc.Signatures.push_back(std::move(overload));
                function.Documented = Documented();
            }

        private:
            void Line(std::string_view text, int line)
            {
                text = TrimLeft(text);
                std::size_t dashes = 0;
                while (dashes < text.size() && text[dashes] == '-')
                    ++dashes;
                std::string_view body = text.substr(dashes);
                if (!body.empty() && body.front() == ' ')
                    body.remove_prefix(1);
                const bool indented = !body.empty() && IsSpace(body.front());
                body = TrimLeft(body);
                while (!body.empty() && (body.back() == ' ' || body.back() == '\t'))
                    body.remove_suffix(1);
                if (body.compare(0, kSigma.size(), kSigma) == 0)
                    body = TrimLeft(body.substr(kSigma.size()));
                if (body.empty() || std::all_of(body.begin(), body.end(), [](char c) { return c == '-' || c == '=' || c == '*' || c == ' '; }))
                {
                    if (body.empty() && m_tag == Tag::None && !m_description.empty() && m_description.back() != '\n')
                        m_description += "\n\n";
                    return;
                }
                if (body.front() == '@')
                {
                    TagLine(body.substr(1), line);
                    return;
                }
                if (body.front() == '|' && m_tag == Tag::Alias && !m_file.Aliases.empty())
                {
                    std::string_view value = TrimLeft(body.substr(1));
                    const std::size_t end = TypeEnd(value, 0);
                    DocParam probe;
                    probe.Type = std::string(value.substr(0, end));
                    ApplyTypeDetails(probe);
                    for (std::string& choice : probe.Choices)
                        m_file.Aliases.back().Choices.push_back(std::move(choice));
                    return;
                }
                if (indented && m_last)
                {
                    Append(*m_last, body);
                    return;
                }
                m_tag = Tag::None;
                m_last = nullptr;
                Append(m_description, body);
            }

            void TagLine(std::string_view text, int line)
            {
                ++m_tags;
                std::string_view rest = text;
                const std::string_view name = Token(rest);
                m_last = nullptr;
                m_tag = Tag::Other;
                if (name == "param")
                {
                    DocParam param;
                    param.Name = std::string(Token(rest));
                    const std::size_t end = TypeEnd(rest, 0);
                    param.Type = std::string(rest.substr(0, end));
                    param.Text = Description(rest.substr(end));
                    ApplyTypeDetails(param);
                    m_params.push_back(std::move(param));
                    m_last = &m_params.back().Text;
                    m_tag = Tag::Param;
                }
                else if (name == "vararg")
                {
                    DocParam param;
                    param.Name = "...";
                    param.Variadic = true;
                    const std::size_t end = TypeEnd(rest, 0);
                    param.Type = std::string(rest.substr(0, end));
                    param.Text = Description(rest.substr(end));
                    ApplyTypeDetails(param);
                    m_params.push_back(std::move(param));
                    m_last = &m_params.back().Text;
                    m_tag = Tag::Param;
                }
                else if (name == "return" || name == "returns")
                {
                    std::string types;
                    std::string description;
                    const std::size_t hash = rest.find('#');
                    if (hash != std::string_view::npos)
                    {
                        types = Trim(rest.substr(0, hash));
                        description = Trim(rest.substr(hash + 1));
                    }
                    else
                    {
                        const std::size_t end = TypeEnd(rest, 0);
                        types = Trim(rest.substr(0, end));
                        std::string_view after = TrimLeft(rest.substr(end));
                        while (!after.empty() && after.front() == ',')
                        {
                            after = TrimLeft(after.substr(1));
                            const std::size_t next = TypeEnd(after, 0);
                            types += ", " + Trim(after.substr(0, next));
                            after = TrimLeft(after.substr(next));
                        }
                        description = Description(after);
                    }
                    if (!m_returns.empty())
                        m_returns += ", ";
                    m_returns += types;
                    if (!description.empty())
                    {
                        if (!m_returnsText.empty())
                            m_returnsText += "; ";
                        m_returnsText += description;
                    }
                    m_last = &m_returnsText;
                    m_tag = Tag::Return;
                }
                else if (name == "overload")
                {
                    DocSignature signature;
                    if (ParseFunType(rest, signature))
                        m_overloads.push_back(std::move(signature));
                }
                else if (name == "deprecated")
                {
                    m_isDeprecated = true;
                    m_deprecated = Trim(rest);
                }
                else if (name == "private" || name == "package" || name == "protected")
                {
                    m_hidden = true;
                }
                else if (name == "async")
                {
                    m_async = true;
                }
                else if (name == "see")
                {
                    m_see.push_back(Trim(rest));
                }
                else if (name == "class")
                {
                    if (!rest.empty() && rest.front() == '(')
                    {
                        const std::size_t close = rest.find(')');
                        rest = close == std::string_view::npos ? std::string_view() : TrimLeft(rest.substr(close + 1));
                    }
                    Finish();
                    DocRecord record;
                    const std::size_t colon = rest.find(':');
                    record.Path = Trim(rest.substr(0, colon));
                    record.Name = record.Path;
                    if (colon != std::string_view::npos)
                    {
                        const auto parents = SplitTopLevel(rest.substr(colon + 1), ",", true);
                        if (!parents.empty())
                            record.Parent = parents.front();
                    }
                    record.Kind = DocItemKind::Class;
                    record.Origin = DocOrigin::Annotation;
                    record.Source = std::string(m_source);
                    record.Line = line;
                    if (!record.Path.empty())
                        m_class = std::move(record);
                }
                else if (name == "field")
                {
                    std::string_view probe = rest;
                    const std::string_view first = Token(probe);
                    if (first == "public" || first == "private" || first == "protected" || first == "package")
                        rest = probe;
                    DocParam param;
                    param.Name = std::string(Token(rest));
                    const std::size_t end = TypeEnd(rest, 0);
                    param.Type = std::string(rest.substr(0, end));
                    param.Text = Description(rest.substr(end));
                    ApplyTypeDetails(param);
                    if (m_class && !param.Name.empty())
                    {
                        DocRecord field;
                        field.Owner = m_class->Path;
                        field.OwnerIsClass = true;
                        field.Name = param.Name;
                        field.Path = field.Owner + "." + field.Name;
                        field.Kind = DocItemKind::Field;
                        field.Origin = DocOrigin::Annotation;
                        field.Type = param.Type;
                        field.Text = param.Text;
                        field.Summary = FirstSentence(field.Text);
                        field.Source = std::string(m_source);
                        field.Line = line;
                        m_file.Fields.push_back(std::move(field));
                        m_last = &m_file.Fields.back().Text;
                    }
                    m_tag = Tag::Field;
                }
                else if (name == "alias")
                {
                    AnnotatedAlias alias;
                    alias.Name = std::string(Token(rest));
                    DocParam probe;
                    probe.Type = std::string(rest.substr(0, TypeEnd(rest, 0)));
                    ApplyTypeDetails(probe);
                    alias.Type = probe.Type;
                    alias.Choices = std::move(probe.Choices);
                    if (!alias.Name.empty())
                    {
                        m_file.Aliases.push_back(std::move(alias));
                        m_tag = Tag::Alias;
                    }
                }
            }

            AnnotatedFile& m_file;
            std::string_view m_source;
            std::string m_description;
            std::vector<DocParam> m_params;
            std::string m_returns;
            std::string m_returnsText;
            std::vector<DocSignature> m_overloads;
            std::vector<std::string> m_see;
            std::string m_deprecated;
            bool m_isDeprecated = false;
            bool m_hidden = false;
            bool m_async = false;
            int m_tags = 0;
            std::optional<DocRecord> m_class;
            std::string* m_last = nullptr;
            Tag m_tag = Tag::None;
        };

        void ResolveAliases(AnnotatedFile& file)
        {
            if (file.Aliases.empty())
                return;
            const auto choicesOf = [&](std::string_view type) -> const AnnotatedAlias*
            {
                for (const AnnotatedAlias& alias : file.Aliases)
                {
                    if (alias.Name == type)
                        return &alias;
                }
                return nullptr;
            };
            const auto resolve = [&](DocParam& param)
            {
                for (const std::string& part : SplitTopLevel(param.Type, "|", true))
                {
                    if (const AnnotatedAlias* alias = choicesOf(part))
                    {
                        for (const std::string& choice : alias->Choices)
                        {
                            if (std::find(param.Choices.begin(), param.Choices.end(), choice) == param.Choices.end())
                                param.Choices.push_back(choice);
                        }
                    }
                }
            };
            for (AnnotatedFunction& function : file.Functions)
            {
                for (DocSignature& signature : function.Doc.Signatures)
                {
                    for (DocParam& param : signature.Params)
                        resolve(param);
                }
            }
        }
    }

    const AnnotatedFunction* AnnotatedFile::At(int line) const
    {
        const auto found = std::lower_bound(Functions.begin(), Functions.end(), line, [](const AnnotatedFunction& function, int value) { return function.Line < value; });
        return found != Functions.end() && found->Line == line ? &*found : nullptr;
    }

    const AnnotatedFunction* AnnotatedFile::Named(std::string_view path) const
    {
        for (const AnnotatedFunction& function : Functions)
        {
            if (function.Doc.Path == path)
                return &function;
        }
        return nullptr;
    }

    AnnotatedFile ParseAnnotations(std::string_view text, std::string_view source)
    {
        AnnotatedFile file;
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
            text.remove_prefix(3);
        std::vector<CommentLine> block;
        int lastComment = -1;
        int longLevel = -1;
        const auto flush = [&]()
        {
            if (block.empty())
                return;
            BlockParser parser(file, source);
            parser.Parse(block);
            parser.Finish();
            block.clear();
        };
        int number = 0;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(start, end - start);
            start = end + 1;
            ++number;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            const std::string_view trimmed = TrimView(line);
            if (longLevel >= 0)
            {
                const std::string close = "]" + std::string(static_cast<std::size_t>(longLevel), '=') + "]";
                if (trimmed.find(close) != std::string_view::npos)
                    longLevel = -1;
                continue;
            }
            if (trimmed.compare(0, 3, "--[") == 0)
            {
                std::size_t i = 3;
                while (i < trimmed.size() && trimmed[i] == '=')
                    ++i;
                if (i < trimmed.size() && trimmed[i] == '[')
                {
                    flush();
                    const int level = static_cast<int>(i - 3);
                    const std::string close = "]" + std::string(static_cast<std::size_t>(level), '=') + "]";
                    if (trimmed.find(close, i + 1) == std::string_view::npos)
                        longLevel = level;
                    continue;
                }
            }
            if (trimmed.compare(0, 2, "--") == 0)
            {
                if (lastComment != number - 1)
                    flush();
                block.push_back({ number, std::string(trimmed) });
                lastComment = number;
                continue;
            }
            if (trimmed.empty())
            {
                flush();
                continue;
            }
            if (const auto definition = MatchDefinition(trimmed))
            {
                AnnotatedFunction function;
                function.Line = number;
                function.Local = definition->Local;
                BlockParser parser(file, source);
                if (lastComment == number - 1 && !block.empty())
                {
                    function.CommentLine = block.front().Line;
                    parser.Parse(block);
                }
                parser.Apply(*definition, number, function);
                parser.Finish();
                block.clear();
                file.Functions.push_back(std::move(function));
                continue;
            }
            flush();
        }
        flush();
        std::stable_sort(file.Functions.begin(), file.Functions.end(), [](const AnnotatedFunction& a, const AnnotatedFunction& b) { return a.Line < b.Line; });
        ResolveAliases(file);
        return file;
    }
}
