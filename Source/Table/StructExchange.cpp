#include "Table/StructExchange.h"

#include "Core/Text.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace Manifold::Table
{
    namespace
    {
        // ---------------------------------------------------------------- export

        const StructDefinition* FindIn(const StructureStore& store, std::string_view name)
        {
            for (const auto& item : store.Items)
            {
                if (EqualsNoCase(item->Name, name))
                    return item.get();
            }
            return nullptr;
        }

        bool EmbeddedField(const StructField& field)
        {
            return field.Type != Scan::ValueType::Pointer && !field.ChildStructure.empty();
        }

        std::string HexText(std::int64_t value)
        {
            return Format("0x%llX", static_cast<unsigned long long>(value));
        }


        struct Names
        {
            std::map<const StructDefinition*, std::string> Types;

            std::string Of(const StructureStore& store, std::string_view name) const
            {
                const StructDefinition* definition = FindIn(store, name);
                const auto it = definition ? Types.find(definition) : Types.end();
                return it == Types.end() ? std::string() : it->second;
            }
        };

        std::string IntType(const StructField& field, bool csharp)
        {
            const bool signedType = field.Display == FieldDisplay::Signed;
            switch (field.Size)
            {
            case 1: return csharp ? (signedType ? "sbyte" : "byte") : (signedType ? "int8_t" : "uint8_t");
            case 2: return csharp ? (signedType ? "short" : "ushort") : (signedType ? "int16_t" : "uint16_t");
            case 4: return csharp ? (signedType ? "int" : "uint") : (signedType ? "int32_t" : "uint32_t");
            case 8: return csharp ? (signedType ? "long" : "ulong") : (signedType ? "int64_t" : "uint64_t");
            default: return {};
            }
        }

        struct Declaration
        {
            std::string Type;    // C/C++: the type; C#: everything before the name
            std::string Suffix;  // [n]
            std::string Note;
        };

        Declaration Declare(const StructureStore& store, const Names& names, const StructField& field, const StructExportOptions& options)
        {
            const bool csharp = options.Language == StructLanguage::CSharp;
            const int pointer = options.Is64Bit ? 8 : 4;
            Declaration out;
            const auto bytes = [&](int count)
                {
                    out.Type = csharp ? "fixed byte" : "uint8_t";
                    out.Suffix = "[" + HexText(count) + "]";
                };
            if (!field.CustomType.empty())
            {
                bytes(field.Size);
                out.Note = "custom type " + field.CustomType;
                return out;
            }
            switch (field.Type)
            {
            case Scan::ValueType::Byte:
            case Scan::ValueType::TwoBytes:
            case Scan::ValueType::FourBytes:
            case Scan::ValueType::EightBytes:
                out.Type = IntType(field, csharp);
                if (out.Type.empty())
                    bytes(field.Size);
                return out;
            case Scan::ValueType::Float:
                if (field.Size == 4)
                    out.Type = "float";
                else
                    bytes(field.Size);
                return out;
            case Scan::ValueType::Double:
                if (field.Size == 8)
                    out.Type = "double";
                else
                    bytes(field.Size);
                return out;
            case Scan::ValueType::String:
                if (field.Unicode && field.Size % 2 == 0)
                {
                    out.Type = csharp ? "fixed char" : "char16_t";
                    out.Suffix = "[" + HexText(field.Size / 2) + "]";
                }
                else
                {
                    out.Type = csharp ? "fixed byte" : "char";
                    out.Suffix = "[" + HexText(field.Size) + "]";
                }
                return out;
            case Scan::ValueType::Binary:
            {
                StructField container = field;
                container.Display = FieldDisplay::Unsigned;
                out.Type = IntType(container, csharp);
                if (out.Type.empty())
                    bytes(field.Size);
                out.Note = Format("bits %d..%d", field.BitStart, field.BitStart + field.BitLength - 1);
                return out;
            }
            case Scan::ValueType::Pointer:
            {
                const std::string child = field.ChildStructure.empty() ? std::string() : names.Of(store, field.ChildStructure);
                if (field.Size != pointer)
                {
                    out.Type = field.Size == 4 ? (csharp ? "uint" : "uint32_t") : field.Size == 8 ? (csharp ? "ulong" : "uint64_t") : std::string();
                    if (out.Type.empty())
                        bytes(field.Size);
                    out.Note = "pointer" + (field.ChildStructure.empty() ? std::string() : " to " + field.ChildStructure);
                    return out;
                }
                if (!child.empty())
                    out.Type = child + "*";
                else
                {
                    out.Type = csharp ? "IntPtr" : "void*";
                    if (!field.ChildStructure.empty())
                        out.Note = "points to " + field.ChildStructure + " (not exported)";
                }
                return out;
            }
            case Scan::ValueType::ByteArray:
            default:
                if (EmbeddedField(field))
                {
                    const StructDefinition* child = FindIn(store, field.ChildStructure);
                    const std::string type = names.Of(store, field.ChildStructure);
                    if (child && !type.empty() && child->Size() == field.Size)
                    {
                        out.Type = type;
                        return out;
                    }
                    bytes(field.Size);
                    out.Note = field.ChildStructure + (type.empty() ? " (not exported)" : " (its size differs)");
                    return out;
                }
                bytes(field.Size);
                return out;
            }
        }

        std::string Unique(std::set<std::string>& used, std::string name)
        {
            std::string candidate = name;
            for (int n = 2; !used.insert(Lower(candidate)).second; ++n)
                candidate = name + "_" + std::to_string(n);
            return candidate;
        }

        // C/C++: the bit fields of one unit as real bit fields, the unused bits as
        // unnamed ones; 0 when they overlap and cannot be written that way.
        int WriteBitFields(std::string& out, std::set<std::string>& used, const std::vector<StructField>& fields, std::size_t first, std::size_t last,
            std::int64_t cursor, const StructExportOptions& options)
        {
            std::vector<const StructField*> group;
            int unit = 1;
            for (std::size_t i = first; i <= last; ++i)
            {
                group.push_back(&fields[i]);
                unit = std::max(unit, BitFieldSize(fields[i].BitStart, fields[i].BitLength));
            }
            std::stable_sort(group.begin(), group.end(), [](const StructField* a, const StructField* b) { return a->BitStart < b->BitStart; });
            int bit = 0;
            for (const StructField* field : group)
            {
                if (field->BitStart < bit || field->BitLength < 1 || field->BitStart + field->BitLength > unit * 8)
                    return 0;
                bit = field->BitStart + field->BitLength;
            }
            const char* type = unit == 1 ? "uint8_t" : unit == 2 ? "uint16_t" : unit == 4 ? "uint32_t" : "uint64_t";
            const int offset = group.front()->Offset;
            if (offset > cursor)
                out += Format("    uint8_t %s[%s];\n", Unique(used, Format("pad_%04X", static_cast<unsigned>(cursor))).c_str(), HexText(offset - cursor).c_str());
            bit = 0;
            for (const StructField* field : group)
            {
                if (field->BitStart > bit)
                    out += Format("    %s : %d;\n", type, field->BitStart - bit);
                std::string name = Identifier(field->Name.empty() ? Format("bits_%04X_%d", offset, field->BitStart) : field->Name);
                const bool renamed = !field->Name.empty() && name != field->Name;
                name = Unique(used, name);
                std::string comment = options.OffsetComments ? Format("+%s bit %d", HexText(offset).c_str(), field->BitStart) : std::string();
                if (renamed)
                    comment += (comment.empty() ? "" : " ") + field->Name;
                out += Format("    %s %s : %d;%s\n", type, name.c_str(), field->BitLength, comment.empty() ? "" : (" // " + comment).c_str());
                bit = field->BitStart + field->BitLength;
            }
            return unit;
        }

        void WriteStructure(std::string& out, const StructureStore& store, const Names& names, const StructDefinition& definition, const StructExportOptions& options)
        {
            const bool csharp = options.Language == StructLanguage::CSharp;
            const std::string type = names.Types.at(&definition);
            std::vector<StructField> fields = definition.Fields;
            std::stable_sort(fields.begin(), fields.end(), [](const StructField& a, const StructField& b) { return a.Offset < b.Offset; });
            const int size = definition.Size();
            if (type != definition.Name)
                out += "// " + definition.Name + "\n";
            if (csharp)
                out += "[StructLayout(LayoutKind.Explicit, Size = " + HexText(size) + ")]\npublic unsafe struct " + type + "\n{\n";
            else
                out += "struct " + type + "\n{\n";
            std::set<std::string> used;
            std::vector<std::pair<std::string, int>> offsets;
            std::int64_t cursor = 0;
            int lastBinaryOffset = -1;
            for (std::size_t index = 0; index < fields.size(); ++index)
            {
                const StructField& field = fields[index];
                if (!csharp && field.Type == Scan::ValueType::Binary && field.Offset >= cursor && field.Offset != lastBinaryOffset)
                {
                    std::size_t last = index;
                    while (last + 1 < fields.size() && fields[last + 1].Offset == field.Offset && fields[last + 1].Type == Scan::ValueType::Binary)
                        ++last;
                    if (const int unit = WriteBitFields(out, used, fields, index, last, cursor, options); unit > 0)
                    {
                        cursor = static_cast<std::int64_t>(field.Offset) + unit;
                        index = last;
                        continue;
                    }
                }
                const Declaration declaration = Declare(store, names, field, options);
                std::string name = Identifier(field.Name.empty() ? Format("field_%04X", field.Offset) : field.Name);
                std::string note = declaration.Note;
                if (!field.Name.empty() && name != field.Name)
                    note = field.Name + (note.empty() ? std::string() : ", " + note);
                if (field.Type == Scan::ValueType::Binary && field.Offset == lastBinaryOffset)
                {
                    out += Format("    // +%s: %s, %s\n", HexText(field.Offset).c_str(), field.Name.c_str(), declaration.Note.c_str());
                    continue;
                }
                lastBinaryOffset = field.Type == Scan::ValueType::Binary ? field.Offset : -1;
                if (!csharp && field.Offset < cursor)
                {
                    out += Format("    // +%s overlaps the field before: %s %s%s\n", HexText(field.Offset).c_str(), declaration.Type.c_str(), name.c_str(), declaration.Suffix.c_str());
                    continue;
                }
                if (!csharp && field.Offset > cursor)
                    out += Format("    uint8_t %s[%s];\n", Unique(used, Format("pad_%04X", static_cast<unsigned>(cursor))).c_str(), HexText(field.Offset - cursor).c_str());
                name = Unique(used, name);
                std::string line = csharp ? Format("    [FieldOffset(%s)] public %s %s%s;", HexText(field.Offset).c_str(), declaration.Type.c_str(), name.c_str(), declaration.Suffix.c_str())
                                          : Format("    %s %s%s;", declaration.Type.c_str(), name.c_str(), declaration.Suffix.c_str());
                std::string comment;
                if (options.OffsetComments)
                    comment = "+" + HexText(field.Offset);
                if (!note.empty())
                    comment += (comment.empty() ? "" : " ") + note;
                if (!comment.empty())
                    line += " // " + comment;
                out += line + "\n";
                offsets.emplace_back(name, field.Offset);
                cursor = std::max<std::int64_t>(cursor, static_cast<std::int64_t>(field.Offset) + field.Size);
            }
            if (!csharp && cursor < size)
                out += Format("    uint8_t %s[%s];\n", Unique(used, Format("pad_%04X", static_cast<unsigned>(cursor))).c_str(), HexText(size - cursor).c_str());
            if (!csharp && fields.empty())
                out += "    uint8_t pad_0000[1];\n";
            out += "};\n";
            if (!csharp && options.Checks)
            {
                out += "static_assert(sizeof(" + type + ") == " + HexText(std::max(size, 1)) + ");\n";
                for (const auto& [name, offset] : offsets)
                    out += "static_assert(offsetof(" + type + ", " + name + ") == " + HexText(offset) + ");\n";
            }
            out += "\n";
        }
    }

    void PadStructure(StructDefinition& definition, int size)
    {
        const int fieldsEnd = definition.Size();
        if (fieldsEnd >= size)
            return;
        // ∑ A field is at most kMaxFieldBytes long; the padding sits at the end, so
        //   Size() comes out right even when the gap is longer than that.
        StructField padding;
        padding.Size = std::min(size - fieldsEnd, kMaxFieldBytes);
        padding.Offset = size - padding.Size;
        padding.Name = "padding";
        padding.Type = padding.Size == 4 ? Scan::ValueType::FourBytes : Scan::ValueType::ByteArray;
        padding.Display = FieldDisplay::Hex;
        definition.Fields.push_back(padding);
    }

    std::vector<std::string> PlanMerge(const StructureStore& store, const std::vector<StructDefinition>& structures, StructConflict conflict)
    {
        std::set<std::string> taken;
        for (const auto& item : store.Items)
            taken.insert(Lower(item->Name));
        // a new name must not be one that comes later in the same import
        std::set<std::string> incoming;
        for (const StructDefinition& definition : structures)
            incoming.insert(Lower(definition.Name));
        std::set<std::string> imported;
        std::vector<std::string> names;
        for (const StructDefinition& definition : structures)
        {
            const bool exists = taken.count(Lower(definition.Name)) > 0;
            if (!imported.insert(Lower(definition.Name)).second)
            {
                names.emplace_back();  // the same name twice in the text: the first one counts
                continue;
            }
            if (!exists || conflict == StructConflict::Replace)
                names.push_back(definition.Name);
            else if (conflict == StructConflict::Skip)
                names.emplace_back();
            else
            {
                std::string name;
                for (int n = 2;; ++n)
                {
                    name = definition.Name + "_" + std::to_string(n);
                    if (!taken.count(Lower(name)) && !incoming.count(Lower(name)))
                        break;
                }
                names.push_back(name);
            }
            if (!names.back().empty())
                taken.insert(Lower(names.back()));
        }
        return names;
    }

    StructMerge MergeStructures(StructureStore& store, std::vector<StructDefinition> structures, StructConflict conflict)
    {
        const std::vector<std::string> names = PlanMerge(store, structures, conflict);
        std::map<std::string, std::string> renamed;
        for (std::size_t i = 0; i < structures.size(); ++i)
        {
            if (!names[i].empty() && names[i] != structures[i].Name)
                renamed[Lower(structures[i].Name)] = names[i];
        }
        StructMerge merge;
        for (std::size_t i = 0; i < structures.size(); ++i)
        {
            if (names[i].empty())
            {
                ++merge.Skipped;
                continue;
            }
            StructDefinition& definition = structures[i];
            definition.Name = names[i];
            for (StructField& field : definition.Fields)
            {
                if (const auto it = renamed.find(Lower(field.ChildStructure)); it != renamed.end())
                    field.ChildStructure = it->second;
            }
            if (StructDefinition* existing = store.Find(definition.Name))
            {
                *existing = std::move(definition);
                ++store.Generation;
                ++merge.Replaced;
            }
            else
            {
                store.Add(definition.Name) = std::move(definition);
                ++merge.Added;
            }
            merge.Names.push_back(names[i]);
        }
        return merge;
    }

    std::string Identifier(std::string_view name)
    {
        std::string out;
        for (const char c : name)
            out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
        while (out.find("__") != std::string::npos)
            out.erase(out.find("__"), 1);
        if (out.empty() || out == "_")
            return "unnamed";
        if (std::isdigit(static_cast<unsigned char>(out.front())))
            out.insert(out.begin(), '_');
        static const std::set<std::string> kReserved = { "int", "char", "float", "double", "long", "short", "class", "struct", "union", "public", "private", "new",
            "delete", "default", "this", "operator", "namespace", "template", "typename", "bool", "void", "signed", "unsigned", "const", "static", "auto", "base",
            "string", "object", "fixed", "byte", "event", "params", "ref", "out", "in", "is", "as", "lock", "checked", "decimal", "goto", "case", "switch", "return" };
        if (kReserved.count(Lower(out)))
            out += "_";
        return out;
    }

    std::vector<const StructDefinition*> ExportOrder(const StructureStore& store, const std::vector<std::string>& roots, bool children)
    {
        std::vector<const StructDefinition*> found;
        std::set<const StructDefinition*> seen;
        std::vector<const StructDefinition*> queue;
        for (const std::string& root : roots)
        {
            if (const StructDefinition* definition = FindIn(store, root); definition && seen.insert(definition).second)
                queue.push_back(definition);
        }
        for (std::size_t i = 0; i < queue.size(); ++i)
        {
            found.push_back(queue[i]);
            if (!children)
                continue;
            for (const StructField& field : queue[i]->Fields)
            {
                if (field.ChildStructure.empty())
                    continue;
                if (const StructDefinition* child = FindIn(store, field.ChildStructure); child && seen.insert(child).second)
                    queue.push_back(child);
            }
        }
        std::vector<const StructDefinition*> ordered;
        std::set<const StructDefinition*> placed;
        std::set<const StructDefinition*> visiting;
        const std::set<const StructDefinition*> chosen(found.begin(), found.end());
        const std::function<void(const StructDefinition*)> place = [&](const StructDefinition* definition)
            {
                if (placed.count(definition) || visiting.count(definition))
                    return;
                visiting.insert(definition);
                for (const StructField& field : definition->Fields)
                {
                    if (!EmbeddedField(field))
                        continue;
                    if (const StructDefinition* child = FindIn(store, field.ChildStructure); child && chosen.count(child))
                        place(child);
                }
                visiting.erase(definition);
                placed.insert(definition);
                ordered.push_back(definition);
            };
        for (const StructDefinition* definition : found)
            place(definition);
        return ordered;
    }

    std::string ExportStructures(const StructureStore& store, const std::vector<std::string>& roots, const StructExportOptions& options)
    {
        const bool csharp = options.Language == StructLanguage::CSharp;
        const std::vector<const StructDefinition*> structures = ExportOrder(store, roots, options.Children);
        Names names;
        std::set<std::string> used;
        for (const StructDefinition* definition : structures)
            names.Types[definition] = Unique(used, Identifier(definition->Name));
        std::string out = Format("// Exported by Manifold Engine: %zu %s, %s pointers.\n", structures.size(), structures.size() == 1 ? "structure" : "structures",
            options.Is64Bit ? "64-bit" : "32-bit");
        if (csharp)
        {
            out += "// Compile with /unsafe: the layouts are explicit, byte for byte as in the dissect.\nusing System;\nusing System.Runtime.InteropServices;\n\n";
        }
        else
        {
            out += "// Packed to 1 byte with the gaps as padding, so the layout is byte for byte as in the dissect.\n#pragma once\n#include <cstddef>\n#include <cstdint>\n\n";
            for (const StructDefinition* definition : structures)
                out += "struct " + names.Types[definition] + ";\n";
            out += "\n#pragma pack(push, 1)\n\n";
        }
        for (const StructDefinition* definition : structures)
            WriteStructure(out, store, names, *definition, options);
        if (!csharp)
            out += "#pragma pack(pop)\n";
        return out;
    }

    // ---------------------------------------------------------------- import

    namespace
    {
        enum class TokenKind
        {
            Word,
            Number,
            Punct,
            Pack,
            End
        };

        struct Token
        {
            TokenKind Kind = TokenKind::End;
            std::string Text;
            std::int64_t Value = 0;
            int Line = 0;
        };

        std::vector<Token> Tokenize(std::string_view text)
        {
            std::vector<Token> tokens;
            int line = 1;
            std::size_t at = 0;
            bool lineStart = true;
            while (at < text.size())
            {
                const char c = text[at];
                if (c == '\n')
                {
                    ++line;
                    ++at;
                    lineStart = true;
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(c)))
                {
                    ++at;
                    continue;
                }
                if (c == '/' && at + 1 < text.size() && text[at + 1] == '/')
                {
                    while (at < text.size() && text[at] != '\n')
                        ++at;
                    continue;
                }
                if (c == '/' && at + 1 < text.size() && text[at + 1] == '*')
                {
                    at += 2;
                    while (at + 1 < text.size() && !(text[at] == '*' && text[at + 1] == '/'))
                    {
                        if (text[at] == '\n')
                            ++line;
                        ++at;
                    }
                    at += 2;
                    continue;
                }
                if (c == '#' && lineStart)
                {
                    std::size_t end = at;
                    while (end < text.size() && text[end] != '\n')
                        ++end;
                    const std::string directive = Lower(Trim(text.substr(at + 1, end - at - 1)));
                    if (directive.starts_with("pragma") && directive.find("pack") != std::string::npos)
                    {
                        Token token{ TokenKind::Pack, directive, 0, line };
                        if (directive.find("pop") != std::string::npos)
                            token.Value = -1;
                        else
                        {
                            std::size_t digit = directive.find_last_of("0123456789");
                            if (digit != std::string::npos)
                            {
                                std::size_t first = digit;
                                while (first > 0 && std::isdigit(static_cast<unsigned char>(directive[first - 1])))
                                    --first;
                                token.Value = std::atoll(directive.substr(first, digit - first + 1).c_str());
                                if (directive.find("push") != std::string::npos)
                                    token.Value += 1000;  // push and set
                            }
                            else if (directive.find("push") != std::string::npos)
                                token.Value = 1000;  // push only
                            else
                                token.Value = 0;     // back to the default
                        }
                        tokens.push_back(token);
                    }
                    at = end;
                    continue;
                }
                lineStart = false;
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                {
                    std::size_t end = at;
                    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' ||
                                                  (text[end] == ':' && end + 2 < text.size() && text[end + 1] == ':' &&
                                                   (std::isalpha(static_cast<unsigned char>(text[end + 2])) || text[end + 2] == '_'))))
                        end += text[end] == ':' ? 2 : 1;
                    std::string word(text.substr(at, end - at));
                    if (word.starts_with("std::"))
                        word = word.substr(5);
                    tokens.push_back({ TokenKind::Word, word, 0, line });
                    at = end;
                    continue;
                }
                if (std::isdigit(static_cast<unsigned char>(c)))
                {
                    std::size_t end = at;
                    while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '\''))
                        ++end;
                    std::string number;
                    for (const char d : text.substr(at, end - at))
                    {
                        if (d != '\'')
                            number += d;
                    }
                    while (!number.empty() && (number.back() == 'u' || number.back() == 'U' || number.back() == 'l' || number.back() == 'L'))
                        number.pop_back();
                    Token token{ TokenKind::Number, number, 0, line };
                    token.Value = static_cast<std::int64_t>(std::strtoull(number.c_str(), nullptr, number.size() > 2 && (number[1] == 'x' || number[1] == 'X') ? 16 : 10));
                    tokens.push_back(token);
                    at = end;
                    continue;
                }
                if (c == ':' && at + 1 < text.size() && text[at + 1] == ':')
                {
                    tokens.push_back({ TokenKind::Punct, "::", 0, line });
                    at += 2;
                    continue;
                }
                tokens.push_back({ TokenKind::Punct, std::string(1, c), 0, line });
                ++at;
            }
            tokens.push_back({ TokenKind::End, "", 0, line });
            return tokens;
        }

        enum class Basic
        {
            Integer,
            Float,
            Char8,
            Char16,
            Bool,
            Pointer,
            Struct,
            MemberFunction,  // void (Class::*)(): the code address, then int adjustments
            MemberData,      // int Class::*: an int offset, then int adjustments
            Unknown
        };

        // ∑ MSVC picks the representation of a pointer to member from the class:
        //   Single, Multiple and Virtual inheritance, Unknown for a class it has not
        //   seen. Each step adds an int (measured against cl x86 and x64).
        enum class InheritanceModel
        {
            Single,
            Multiple,
            Virtual,
            Unknown
        };

        struct TypeInfo
        {
            Basic Kind = Basic::Unknown;
            int Size = 0;
            int Align = 1;
            bool Signed = false;
            std::string Struct;  // Struct: the embedded one; Pointer: what it points to (may be defined later)
            InheritanceModel Model = InheritanceModel::Single;  // MemberFunction, MemberData
        };

        // A declarator in parentheses: (*name)(...), (__stdcall *name[4])(...), (Class::*name)(...).
        struct ParenDeclaration
        {
            std::string Name;      // empty in a type alias: using F = void (*)();
            int Count = 0;
            std::string MemberOf;  // the class of a pointer to member
            bool Function = false; // a parameter list follows
            bool Method = false;   // R (*name(int))(): a function returning a function pointer
        };

        struct Parsed
        {
            StructDefinition Definition;
            int Size = 0;
            int Align = 1;
            bool Virtual = false;
        };

        class Parser
        {
        public:
            Parser(std::vector<Token> tokens, const StructImportOptions& options, bool csharp) : m_tokens(std::move(tokens)), m_options(options), m_csharp(csharp)
            {
                m_pointer = options.Is64Bit ? 8 : 4;
            }

            StructImport Run()
            {
                while (!AtEnd())
                    TopLevel();
                // pointers to structures defined after the pointer
                for (StructDefinition& definition : m_import.Structures)
                {
                    for (StructField& field : definition.Fields)
                    {
                        if (field.Type == Scan::ValueType::Pointer && !field.ChildStructure.empty() && !m_structs.count(field.ChildStructure))
                            field.ChildStructure.clear();
                    }
                }
                return std::move(m_import);
            }

        private:
            const Token& Peek(int ahead = 0) const
            {
                const std::size_t at = std::min(m_at + static_cast<std::size_t>(ahead), m_tokens.size() - 1);
                return m_tokens[at];
            }
            const Token& Next() { const Token& token = Peek(); if (m_at + 1 < m_tokens.size()) ++m_at; return token; }
            bool AtEnd() const { return Peek().Kind == TokenKind::End; }
            bool Is(std::string_view text, int ahead = 0) const { return Peek(ahead).Kind != TokenKind::End && Peek(ahead).Text == text; }
            bool Accept(std::string_view text)
            {
                if (!Is(text))
                    return false;
                Next();
                return true;
            }

            void Warn(int line, std::string text)
            {
                if (m_import.Warnings.size() < 50)
                    m_import.Warnings.push_back(Format("line %d: ", line) + text);
            }

            // Past a balanced {...} or (...) starting at the current token.
            void SkipBalanced()
            {
                const std::string open = Peek().Text;
                const std::string close = open == "{" ? "}" : open == "(" ? ")" : open == "[" ? "]" : open == "<" ? ">" : "";
                if (close.empty())
                {
                    Next();
                    return;
                }
                int depth = 0;
                while (!AtEnd())
                {
                    const Token& token = Next();
                    if (token.Text == open)
                        ++depth;
                    else if (token.Text == close && --depth == 0)
                        return;
                }
            }

            void SkipStatement()
            {
                while (!AtEnd() && !Is(";") && !Is("}"))
                {
                    if (Is("{"))
                    {
                        SkipBalanced();
                        if (!Is(";"))
                            return;  // a function body: no ; after it
                        break;
                    }
                    if (Is("(") || Is("["))
                    {
                        SkipBalanced();
                        continue;
                    }
                    Next();
                }
                Accept(";");
            }

            int Pack() const { return m_pack.empty() ? 8 : m_pack.back(); }

            void PackToken(const Token& token)
            {
                if (token.Value == -1)
                {
                    if (!m_pack.empty())
                        m_pack.pop_back();
                }
                else if (token.Value >= 1000)
                {
                    m_pack.push_back(token.Value > 1000 ? static_cast<int>(token.Value - 1000) : Pack());
                }
                else if (token.Value > 0)
                {
                    if (m_pack.empty())
                        m_pack.push_back(static_cast<int>(token.Value));
                    else
                        m_pack.back() = static_cast<int>(token.Value);
                }
                else if (!m_pack.empty())
                {
                    m_pack.back() = 8;
                }
            }

            // C#: [StructLayout(LayoutKind.Explicit, Size = 0x20, Pack = 1)] and [FieldOffset(8)]
            void Attribute()
            {
                const std::size_t start = m_at;
                SkipBalanced();
                std::string text;
                std::vector<std::int64_t> numbers;
                for (std::size_t i = start; i < m_at; ++i)
                {
                    text += m_tokens[i].Text + " ";
                    if (m_tokens[i].Kind == TokenKind::Number)
                        numbers.push_back(m_tokens[i].Value);
                }
                if (text.find("FieldOffset") != std::string::npos && !numbers.empty())
                    m_fieldOffset = static_cast<int>(numbers.front());
                if (text.find("StructLayout") != std::string::npos)
                {
                    m_explicit = text.find("Explicit") != std::string::npos;
                    m_layoutSize.reset();
                    m_layoutPack.reset();
                    for (std::size_t i = start; i + 2 < m_at; ++i)
                    {
                        if (m_tokens[i].Text == "Size" && m_tokens[i + 1].Text == "=" && m_tokens[i + 2].Kind == TokenKind::Number)
                            m_layoutSize = static_cast<int>(m_tokens[i + 2].Value);
                        if (m_tokens[i].Text == "Pack" && m_tokens[i + 1].Text == "=" && m_tokens[i + 2].Kind == TokenKind::Number)
                            m_layoutPack = static_cast<int>(m_tokens[i + 2].Value);
                    }
                }
            }

            bool Modifier(std::string_view word) const
            {
                static const std::set<std::string, std::less<>> kModifiers = { "public", "private", "protected", "internal", "unsafe", "readonly", "volatile",
                    "const", "mutable", "new", "sealed", "partial", "final", "alignas", "__declspec", "extern", "inline", "explicit", "constexpr" };
                return kModifiers.count(word) > 0;
            }

            void TopLevel()
            {
                const Token& token = Peek();
                if (token.Kind == TokenKind::Pack)
                {
                    PackToken(Next());
                    return;
                }
                if (Is("["))
                {
                    Attribute();
                    return;
                }
                if (Is("namespace"))
                {
                    Next();
                    while (!AtEnd() && !Is("{") && !Is(";"))
                        Next();
                    Accept("{");  // its contents are read like the top level; the } is passed over below
                    return;
                }
                if (Is("}") || Is(";"))
                {
                    Next();
                    return;
                }
                if (Is("using") && Peek(1).Kind == TokenKind::Word && Is("=", 2))
                {
                    Next();
                    const std::string name = Next().Text;
                    Next();
                    Alias(name);
                    return;
                }
                if (Is("typedef"))
                {
                    Next();
                    Typedef();
                    return;
                }
                if (token.Kind == TokenKind::Word && Modifier(token.Text))
                {
                    Next();
                    return;
                }
                if (Is("struct") || Is("class") || Is("union"))
                {
                    const bool isUnion = Is("union");
                    Next();
                    std::string name;
                    if (Peek().Kind == TokenKind::Word && !Is("final"))
                        name = Next().Text;
                    Accept("final");
                    Accept("sealed");
                    if (Is(";"))
                    {
                        Next();  // a forward declaration
                        return;
                    }
                    std::string base;
                    int bases = 0;
                    bool virtualInheritance = false;
                    if (Accept(":"))
                    {
                        bases = 1;
                        int depth = 0;  // template arguments: Base<A, B>
                        while (!AtEnd() && !Is("{"))
                        {
                            const Token& part = Next();
                            if (part.Text == "<")
                                ++depth;
                            else if (part.Text == ">" && depth > 0)
                                --depth;
                            else if (part.Text == "," && depth == 0)
                                ++bases;
                            else if (part.Text == "virtual")
                                virtualInheritance = true;
                            if (part.Kind == TokenKind::Word && !Modifier(part.Text) && part.Text != "virtual" && base.empty())
                                base = part.Text;
                        }
                    }
                    if (!Is("{"))
                    {
                        SkipStatement();
                        return;
                    }
                    // ∑ Known from the base clause on, so a pointer to a member of the class
                    //   inside its own body already has its final size.
                    InheritanceModel model = InheritanceModel::Single;
                    if (virtualInheritance)
                        model = InheritanceModel::Virtual;
                    else if (bases > 1)
                        model = InheritanceModel::Multiple;
                    else if (bases == 1 && m_models.count(base))
                        model = m_models[base];
                    if (!name.empty())
                        m_models[name] = model;
                    Parsed parsed = Body(name, isUnion, base);
                    // a polymorphic class on a base without virtual functions: its base is
                    // not at offset 0, so it takes the multiple inheritance form
                    if (model == InheritanceModel::Single && bases == 1 && parsed.Virtual && m_structs.count(base) && !m_virtual.count(base))
                        model = InheritanceModel::Multiple;
                    Define(parsed);
                    m_models[parsed.Definition.Name] = model;
                    // struct Name { } instance;  -> nothing to keep at the top level
                    if (!m_csharp)
                        SkipStatement();
                    return;
                }
                if (Is("enum"))
                {
                    Enum();
                    return;
                }
                SkipStatement();
            }

            void Alias(const std::string& name)
            {
                const int line = Peek().Line;
                std::optional<TypeInfo> type = TypeSpec();
                int stars = 0;
                while (Accept("*"))
                    ++stars;
                if (Is("("))
                {
                    if (!PointerAlias(name, type, line))
                    {
                        Warn(line, "the alias " + name + " is not read");
                        SkipStatement();
                    }
                    return;
                }
                if (type)
                {
                    if (stars)
                        type = PointerTo(*type);
                    m_aliases[name] = *type;
                }
                else
                {
                    Warn(line, "the alias " + name + " names an unknown type");
                }
                SkipStatement();
            }

            void Typedef()
            {
                if (Is("struct") || Is("union") || Is("class"))
                {
                    const bool isUnion = Is("union");
                    Next();
                    std::string tag;
                    if (Peek().Kind == TokenKind::Word)
                        tag = Next().Text;
                    if (Is("{"))
                    {
                        Parsed parsed = Body(tag, isUnion, {});
                        std::string name = tag;
                        std::vector<std::string> pointerNames;
                        while (!AtEnd() && !Is(";"))
                        {
                            bool pointer = false;
                            while (Accept("*"))
                                pointer = true;
                            if (Peek().Kind == TokenKind::Word)
                            {
                                const std::string declared = Next().Text;
                                if (pointer)
                                    pointerNames.push_back(declared);
                                else if (name.empty() || name == tag)
                                    name = declared;
                            }
                            else
                                Next();
                        }
                        Accept(";");
                        if (name.empty())
                            name = Format("unnamed_%d", static_cast<int>(m_import.Structures.size()) + 1);
                        parsed.Definition.Name = name;
                        Define(parsed);
                        if (!tag.empty() && tag != name)
                            m_aliases[tag] = StructType(name);
                        for (const std::string& pointerName : pointerNames)
                            m_aliases[pointerName] = PointerTo(StructType(name));
                        return;
                    }
                }
                // typedef <type> Name;
                const int line = Peek().Line;
                std::optional<TypeInfo> type = TypeSpec();
                int stars = 0;
                while (Accept("*"))
                    ++stars;
                if (Is("("))
                {
                    if (!PointerAlias({}, type, line))
                        SkipStatement();  // a function type or an array of pointers: no alias
                    return;
                }
                if (type && Peek().Kind == TokenKind::Word)
                {
                    const std::string name = Next().Text;
                    m_aliases[name] = stars ? PointerTo(*type) : *type;
                }
                SkipStatement();
            }

            // typedef void (*Name)();  using Name = void (*)();  typedef void (Class::*Name)();
            // The declarator names the alias of a typedef (name empty).
            bool PointerAlias(const std::string& name, const std::optional<TypeInfo>& type, int line)
            {
                const std::optional<ParenDeclaration> declaration = ParenDeclarator(line);
                if (!declaration || declaration->Method || declaration->Count)
                    return false;
                const std::string alias = name.empty() ? declaration->Name : name;
                if (alias.empty())
                    return false;
                m_aliases[alias] = DeclaredType(*declaration, type);
                SkipStatement();
                return true;
            }

            void Enum()
            {
                Next();
                Accept("class");
                Accept("struct");
                std::string name;
                if (Peek().Kind == TokenKind::Word)
                    name = Next().Text;
                TypeInfo type{ Basic::Integer, 4, 4, true, {} };
                if (Accept(":"))
                {
                    if (std::optional<TypeInfo> underlying = TypeSpec())
                        type = *underlying;
                }
                if (Is("{"))
                    SkipBalanced();
                if (!name.empty())
                    m_aliases[name] = type;
                if (!m_csharp)
                    SkipStatement();
            }

            TypeInfo StructType(const std::string& name) const
            {
                const auto it = m_structs.find(name);
                TypeInfo type{ Basic::Struct, it == m_structs.end() ? 0 : it->second.first, it == m_structs.end() ? 1 : it->second.second, false, name };
                return type;
            }

            TypeInfo PointerTo(const TypeInfo& target) const
            {
                TypeInfo pointer{ Basic::Pointer, m_pointer, m_pointer, false, {} };
                if (target.Kind == Basic::Struct)
                    pointer.Struct = target.Struct;
                return pointer;
            }

            // ∑ MSVC layout, measured with cl x86 and x64:
            //   void (C::*)()  Single 4/8, Multiple 8/16, Virtual 12/16, Unknown 16/24
            //   int C::*       Single and Multiple 4, Virtual 8, Unknown 12
            //   A form with more than one part is 8 aligned on x86 as well. GCC and
            //   MinGW use two pointers for every function form; that is not read.
            TypeInfo MemberPointer(const std::string& owner, bool function) const
            {
                InheritanceModel model = InheritanceModel::Unknown;
                if (const auto it = m_models.find(owner); it != m_models.end())
                    model = it->second;
                else if (m_structs.count(owner))
                    model = InheritanceModel::Single;
                const int steps = static_cast<int>(model);
                TypeInfo type;
                type.Kind = function ? Basic::MemberFunction : Basic::MemberData;
                type.Model = model;
                type.Struct = owner;  // for the warning only
                type.Signed = !function;
                const int parts = function ? steps : std::max(steps - 1, 0);
                type.Size = function ? AlignUp(m_pointer + 4 * parts, m_pointer) : 4 + 4 * parts;
                type.Align = parts > 0 && m_pointer == 4 ? 8 : function ? m_pointer : 4;
                return type;
            }

            // At "(" after the type. On anything else than a pointer declarator the
            // position is left as it was.
            std::optional<ParenDeclaration> ParenDeclarator(int line)
            {
                static const std::set<std::string, std::less<>> kQualifiers = { "const", "volatile", "__restrict", "restrict", "__ptr32", "__ptr64", "__unaligned", "__sptr", "__uptr" };
                const std::size_t start = m_at;
                const auto fail = [&]() -> std::optional<ParenDeclaration>
                    {
                        m_at = start;
                        return std::nullopt;
                    };
                ParenDeclaration result;
                Next();
                // calling conventions and macros before the pointer: __stdcall, WINAPI, __attribute__((x))
                while (Peek().Kind == TokenKind::Word && !Is("::", 1))
                {
                    Next();
                    if (Is("("))
                        SkipBalanced();
                }
                Accept("::");
                while (Peek().Kind == TokenKind::Word && Is("::", 1))
                {
                    result.MemberOf = Next().Text;  // the last name before ::* is the class
                    Next();
                }
                if (!Is("*") && !Is("&"))
                    return fail();
                if (!result.MemberOf.empty() && !Is("*"))
                    return fail();
                while (Accept("*") || Accept("&"))
                {
                    while (Peek().Kind == TokenKind::Word && kQualifiers.count(Peek().Text))
                        Next();
                }
                if (Peek().Kind == TokenKind::Word)
                    result.Name = Next().Text;
                if (Is("("))
                {
                    result.Method = true;
                    return result;
                }
                while (Accept("["))
                {
                    int dimension = 0;
                    if (Peek().Kind == TokenKind::Number)
                        dimension = static_cast<int>(std::min<std::uint64_t>(Next().Value, kMaxFieldOffset));
                    else
                        Warn(line, "the length of " + result.Name + " is not a number");
                    while (!AtEnd() && !Is("]") && !Is(")"))
                        Next();
                    if (!Accept("]"))
                        return fail();
                    result.Count = static_cast<int>(std::min<std::int64_t>(result.Count ? static_cast<std::int64_t>(result.Count) * dimension : dimension, kMaxFieldOffset));
                }
                if (!Accept(")"))
                    return fail();
                if (Is("("))
                {
                    result.Function = true;
                    SkipBalanced();
                    // const, noexcept(...), throw(...), & and the like after the parameters
                    while (Is("&") || (Peek().Kind == TokenKind::Word && (kQualifiers.count(Peek().Text) || Is("noexcept") || Is("throw"))))
                    {
                        Next();
                        if (Is("("))
                            SkipBalanced();
                    }
                }
                else
                {
                    while (Is("["))
                        SkipBalanced();  // int (*p)[4]: one pointer to an array
                }
                return result;
            }

            TypeInfo DeclaredType(const ParenDeclaration& declaration, const std::optional<TypeInfo>& target) const
            {
                if (!declaration.MemberOf.empty())
                    return MemberPointer(declaration.MemberOf, declaration.Function);
                if (declaration.Function || !target)
                    return TypeInfo{ Basic::Pointer, m_pointer, m_pointer, false, {} };
                return PointerTo(*target);
            }

            std::optional<TypeInfo> Builtin(const std::vector<std::string>& words) const
            {
                auto has = [&](std::string_view word) { return std::find(words.begin(), words.end(), word) != words.end(); };
                const int longs = static_cast<int>(std::count(words.begin(), words.end(), "long"));
                const bool isUnsigned = has("unsigned");
                if (m_csharp && words.size() == 1)
                {
                    static const std::map<std::string, std::pair<int, int>, std::less<>> kSharp = {  // size, 0 unsigned / 1 signed / 2 float / 3 bool / 4 char / 5 pointer
                        { "byte", { 1, 0 } }, { "sbyte", { 1, 1 } }, { "short", { 2, 1 } }, { "ushort", { 2, 0 } }, { "int", { 4, 1 } }, { "uint", { 4, 0 } },
                        { "long", { 8, 1 } }, { "ulong", { 8, 0 } }, { "float", { 4, 2 } }, { "double", { 8, 2 } }, { "bool", { 1, 3 } }, { "char", { 2, 4 } },
                        { "IntPtr", { 0, 5 } }, { "UIntPtr", { 0, 5 } }, { "nint", { 0, 5 } }, { "nuint", { 0, 5 } } };
                    if (const auto it = kSharp.find(words.front()); it != kSharp.end())
                    {
                        const auto [size, kind] = it->second;
                        if (kind == 5)
                            return TypeInfo{ Basic::Pointer, m_pointer, m_pointer, false, {} };
                        const Basic basic = kind == 2 ? Basic::Float : kind == 3 ? Basic::Bool : kind == 4 ? Basic::Char16 : Basic::Integer;
                        return TypeInfo{ basic, size, size, kind == 1, {} };
                    }
                }
                if (has("float"))
                    return TypeInfo{ Basic::Float, 4, 4, true, {} };
                if (has("double"))
                    return longs ? TypeInfo{ Basic::Float, 8, 8, true, {} } : TypeInfo{ Basic::Float, 8, 8, true, {} };
                if (has("bool") || has("_Bool"))
                    return TypeInfo{ Basic::Bool, 1, 1, false, {} };
                if (has("wchar_t") || has("char16_t"))
                    return TypeInfo{ Basic::Char16, 2, 2, false, {} };
                if (has("char32_t"))
                    return TypeInfo{ Basic::Integer, 4, 4, false, {} };
                if (has("char"))
                    return TypeInfo{ Basic::Char8, 1, 1, !isUnsigned, {} };
                if (has("short"))
                    return TypeInfo{ Basic::Integer, 2, 2, !isUnsigned, {} };
                if (longs >= 2 || has("__int64"))
                    return TypeInfo{ Basic::Integer, 8, 8, !isUnsigned, {} };
                if (has("__int8"))
                    return TypeInfo{ Basic::Integer, 1, 1, !isUnsigned, {} };
                if (has("__int16"))
                    return TypeInfo{ Basic::Integer, 2, 2, !isUnsigned, {} };
                if (longs == 1 || has("int") || has("__int32") || has("signed") || isUnsigned)
                    return TypeInfo{ Basic::Integer, 4, 4, !isUnsigned, {} };
                if (has("void"))
                    return TypeInfo{ Basic::Integer, 1, 1, false, {} };  // only as void*
                return std::nullopt;
            }

            std::optional<TypeInfo> Named(const std::string& name) const
            {
                if (const auto alias = m_aliases.find(name); alias != m_aliases.end())
                    return alias->second;
                if (m_structs.count(name))
                    return StructType(name);
                static const std::map<std::string, std::pair<int, int>, std::less<>> kNamed = {  // size (0 pointer), 0 unsigned / 1 signed / 2 float / 3 bool / 4 char8 / 5 char16
                    { "int8_t", { 1, 1 } }, { "uint8_t", { 1, 0 } }, { "int16_t", { 2, 1 } }, { "uint16_t", { 2, 0 } }, { "int32_t", { 4, 1 } }, { "uint32_t", { 4, 0 } },
                    { "int64_t", { 8, 1 } }, { "uint64_t", { 8, 0 } }, { "BYTE", { 1, 0 } }, { "UCHAR", { 1, 0 } }, { "CHAR", { 1, 4 } }, { "BOOLEAN", { 1, 3 } },
                    { "INT8", { 1, 1 } }, { "UINT8", { 1, 0 } }, { "WORD", { 2, 0 } }, { "SHORT", { 2, 1 } }, { "USHORT", { 2, 0 } }, { "WCHAR", { 2, 5 } },
                    { "INT16", { 2, 1 } }, { "UINT16", { 2, 0 } }, { "DWORD", { 4, 0 } }, { "BOOL", { 4, 1 } }, { "INT", { 4, 1 } }, { "UINT", { 4, 0 } },
                    { "LONG", { 4, 1 } }, { "ULONG", { 4, 0 } }, { "INT32", { 4, 1 } }, { "UINT32", { 4, 0 } }, { "FLOAT", { 4, 2 } }, { "QWORD", { 8, 0 } },
                    { "DWORD64", { 8, 0 } }, { "ULONGLONG", { 8, 0 } }, { "LONGLONG", { 8, 1 } }, { "INT64", { 8, 1 } }, { "UINT64", { 8, 0 } }, { "DOUBLE", { 8, 2 } },
                    { "uintptr_t", { 0, 0 } }, { "intptr_t", { 0, 1 } }, { "size_t", { 0, 0 } }, { "ptrdiff_t", { 0, 1 } }, { "SIZE_T", { 0, 0 } }, { "ULONG_PTR", { 0, 0 } },
                    { "LONG_PTR", { 0, 1 } }, { "DWORD_PTR", { 0, 0 } }, { "UINT_PTR", { 0, 0 } }, { "INT_PTR", { 0, 1 } }, { "HANDLE", { 0, 0 } }, { "HMODULE", { 0, 0 } },
                    { "HWND", { 0, 0 } }, { "PVOID", { 0, 0 } }, { "LPVOID", { 0, 0 } }, { "LPCVOID", { 0, 0 } }, { "LPSTR", { 0, 0 } }, { "LPCSTR", { 0, 0 } },
                    { "LPWSTR", { 0, 0 } }, { "LPCWSTR", { 0, 0 } }, { "PBYTE", { 0, 0 } }, { "IntPtr", { 0, 0 } }, { "UIntPtr", { 0, 0 } } };
                if (const auto it = kNamed.find(name); it != kNamed.end())
                {
                    const auto [size, kind] = it->second;
                    if (size == 0)
                        return TypeInfo{ kind == 1 ? Basic::Integer : Basic::Pointer, m_pointer, m_pointer, kind == 1, {} };
                    const Basic basic = kind == 2 ? Basic::Float : kind == 3 ? Basic::Bool : kind == 4 ? Basic::Char8 : kind == 5 ? Basic::Char16 : Basic::Integer;
                    return TypeInfo{ basic, size, size, kind == 1, {} };
                }
                return std::nullopt;
            }

            // The type part of a declaration; the declarator (*, name, [n]) follows.
            std::optional<TypeInfo> TypeSpec()
            {
                static const std::set<std::string, std::less<>> kWords = { "unsigned", "signed", "short", "long", "int", "char", "float", "double", "bool", "_Bool",
                    "wchar_t", "char16_t", "char32_t", "void", "__int8", "__int16", "__int32", "__int64" };
                std::vector<std::string> words;
                while (Peek().Kind == TokenKind::Word && (Modifier(Peek().Text) || Is("struct") || Is("class") || Is("enum") || Is("union") || (m_csharp && Is("fixed"))))
                    Next();
                if (m_csharp && Peek().Kind == TokenKind::Word)
                {
                    const std::string word = Next().Text;
                    std::optional<TypeInfo> type = Builtin({ word });
                    if (!type)
                        type = Named(word);
                    if (!type)
                        m_unknown = word;
                    return type;
                }
                while (Peek().Kind == TokenKind::Word && kWords.count(Peek().Text))
                    words.push_back(Next().Text);
                while (Peek().Kind == TokenKind::Word && Modifier(Peek().Text))
                    Next();
                if (!words.empty())
                    return Builtin(words);
                if (Peek().Kind != TokenKind::Word)
                    return std::nullopt;
                const std::string name = Next().Text;
                if (Is("<"))
                    SkipBalanced();  // a template: its size is not known
                while (Peek().Kind == TokenKind::Word && Modifier(Peek().Text))
                    Next();
                std::optional<TypeInfo> type = Named(name);
                if (!type)
                    m_unknown = name;
                return type;
            }

            static int AlignUp(int value, int align)
            {
                return align <= 1 ? value : (value + align - 1) / align * align;
            }

            // ∑ MSVC rules: a field is aligned to the smaller of its own alignment and the
            //   pack; bit fields share a unit while the next one has the same size and
            //   fits; the structure is padded to its largest alignment.
            struct Layout
            {
                int Offset = 0;
                int Align = 1;
                int Pack = 8;
                bool Union = false;
                int UnionSize = 0;
                int UnitOffset = -1;
                int UnitSize = 0;
                int UnitBits = 0;
            };

            void AddField(Parsed& parsed, Layout& layout, const TypeInfo& type, const std::string& name, int count, int bits, int line)
            {
                const int align = std::max(1, std::min(type.Align, layout.Pack));
                int at = 0;
                if (bits > 0 && type.Kind != Basic::Struct)
                {
                    const int unit = type.Size;
                    if (layout.UnitOffset >= 0 && layout.UnitSize == unit && layout.UnitBits + bits <= unit * 8 && !layout.Union)
                    {
                        at = layout.UnitOffset;
                    }
                    else
                    {
                        at = layout.Union ? 0 : AlignUp(layout.Offset, align);
                        if (at >= kMaxFieldOffset)
                        {
                            Warn(line, Format("%s lies beyond the largest offset a dissect field can have (%X); it is left out", name.c_str(), kMaxFieldOffset));
                            return;
                        }
                        layout.UnitOffset = at;
                        layout.UnitSize = unit;
                        layout.UnitBits = 0;
                        if (!layout.Union)
                            layout.Offset = at + unit;
                    }
                    m_fieldOffset.reset();
                    if (!name.empty())  // an unnamed bit field only takes its bits
                    {
                        StructField field;
                        field.Offset = at;
                        field.Name = name;
                        field.Type = Scan::ValueType::Binary;
                        field.BitStart = layout.UnitBits;
                        field.BitLength = bits;
                        field.Size = BitFieldSize(field.BitStart, field.BitLength);
                        field.Display = FieldDisplay::Unsigned;
                        parsed.Definition.Fields.push_back(field);
                    }
                    layout.UnitBits += bits;
                    layout.Align = std::max(layout.Align, align);
                    layout.UnionSize = std::max(layout.UnionSize, unit);
                    return;
                }
                layout.UnitOffset = -1;
                if (m_fieldOffset)
                    at = *m_fieldOffset;
                else
                    at = layout.Union ? 0 : AlignUp(layout.Offset, align);
                m_fieldOffset.reset();
                if (at >= kMaxFieldOffset)
                {
                    Warn(line, Format("%s lies beyond the largest offset a dissect field can have (%X); it is left out", name.c_str(), kMaxFieldOffset));
                    return;
                }
                const int total = static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(type.Size) * std::max(count, 1), kMaxFieldOffset - at));
                Emit(parsed, type, name, at, count, line);
                if (!layout.Union)
                    layout.Offset = std::max(layout.Offset, at + total);
                layout.UnionSize = std::max(layout.UnionSize, at + total);
                layout.Align = std::max(layout.Align, align);
            }

            void Emit(Parsed& parsed, const TypeInfo& type, const std::string& name, int at, int count, int line)
            {
                const auto plain = [&](const TypeInfo& element, int offset, const std::string& fieldName)
                    {
                        StructField field;
                        field.Offset = offset;
                        field.Name = fieldName;
                        field.Size = element.Size;
                        switch (element.Kind)
                        {
                        case Basic::Float:
                            field.Type = element.Size == 8 ? Scan::ValueType::Double : Scan::ValueType::Float;
                            break;
                        case Basic::Pointer:
                            field.Type = Scan::ValueType::Pointer;
                            field.ChildStructure = element.Struct;
                            field.Display = FieldDisplay::Hex;
                            break;
                        case Basic::Struct:
                            field.Type = Scan::ValueType::ByteArray;
                            field.ChildStructure = element.Struct;
                            break;
                        default:
                            field.Type = element.Size == 1 ? Scan::ValueType::Byte : element.Size == 2 ? Scan::ValueType::TwoBytes
                                       : element.Size == 4 ? Scan::ValueType::FourBytes : element.Size == 8 ? Scan::ValueType::EightBytes : Scan::ValueType::ByteArray;
                            field.Display = element.Signed ? FieldDisplay::Signed : FieldDisplay::Unsigned;
                            break;
                        }
                        parsed.Definition.Fields.push_back(field);
                    };
                if (type.Kind == Basic::MemberFunction || type.Kind == Basic::MemberData)
                {
                    // ∑ After the code address (or the data offset) MSVC keeps ints in this
                    //   order: this adjustment (function form only), vbptr offset, vbtable index.
                    std::vector<std::string> parts;
                    if (type.Kind == Basic::MemberFunction && type.Model != InheritanceModel::Single)
                        parts.push_back("thisAdjust");
                    if (type.Model == InheritanceModel::Unknown)
                        parts.push_back("vbptrOffset");
                    if (type.Model == InheritanceModel::Virtual || type.Model == InheritanceModel::Unknown)
                        parts.push_back("vbIndex");
                    const int head = type.Kind == Basic::MemberFunction ? m_pointer : 4;
                    const int elements = std::min(std::max(count, 1), kExpand);
                    for (int i = 0; i < elements; ++i)
                    {
                        const int base = at + i * type.Size;
                        const std::string element = count > 0 ? Format("%s[%d]", name.c_str(), i) : name;
                        if (base >= kMaxFieldOffset)
                        {
                            Warn(line, Format("%s lies beyond the largest offset a dissect field can have (%X); it is left out", element.c_str(), kMaxFieldOffset));
                            break;
                        }
                        if (type.Kind == Basic::MemberFunction)
                            plain(TypeInfo{ Basic::Pointer, m_pointer, m_pointer, false, {} }, base, element);
                        else
                            plain(TypeInfo{ Basic::Integer, 4, 4, true, {} }, base, element);
                        for (std::size_t part = 0; part < parts.size(); ++part)
                        {
                            if (base + head + 4 * static_cast<int>(part) >= kMaxFieldOffset)
                            {
                                Warn(line, Format("%s_%s lies beyond the largest offset a dissect field can have (%X); it is left out", element.c_str(), parts[part].c_str(), kMaxFieldOffset));
                                break;
                            }
                            plain(TypeInfo{ Basic::Integer, 4, 4, true, {} }, base + head + 4 * static_cast<int>(part), element + "_" + parts[part]);
                        }
                    }
                    if (type.Model == InheritanceModel::Unknown)
                        Warn(line, Format("%s: %s is not defined before it, so the pointer to member takes MSVC's general form (%d bytes); "
                                          "with the class defined it can be smaller", name.c_str(), type.Struct.c_str(), type.Size));
                    return;
                }
                if (count <= 0)
                {
                    plain(type, at, name);
                    return;
                }
                if (type.Kind == Basic::Char8 || type.Kind == Basic::Char16)
                {
                    StructField field;
                    field.Offset = at;
                    field.Name = name;
                    field.Type = Scan::ValueType::String;
                    field.Unicode = type.Kind == Basic::Char16;
                    field.Size = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(type.Size) * count, 1, kMaxFieldBytes));
                    parsed.Definition.Fields.push_back(field);
                    if (static_cast<std::int64_t>(type.Size) * count > kMaxFieldBytes)
                        Warn(line, Format("%s is longer than a dissect field can be (%d bytes)", name.c_str(), kMaxFieldBytes));
                    return;
                }
                if (type.Kind == Basic::Integer && type.Size == 1)
                {
                    // ∑ Byte arrays stay one field; pad_0010[8] and the like are the gaps
                    //   an export writes (ours included) and give no field at all.
                    std::string_view bare = name;
                    while (!bare.empty() && bare.front() == '_')
                        bare.remove_prefix(1);
                    if (Lower(bare).starts_with("pad"))
                        return;
                }
                if (count <= kExpand && !(type.Kind == Basic::Integer && type.Size == 1))
                {
                    for (int i = 0; i < count; ++i)
                    {
                        if (at + static_cast<std::int64_t>(i) * type.Size >= kMaxFieldOffset)
                        {
                            Warn(line, Format("%s[%d] lies beyond the largest offset a dissect field can have (%X); it is left out", name.c_str(), i, kMaxFieldOffset));
                            return;
                        }
                        plain(type, at + i * type.Size, Format("%s[%d]", name.c_str(), i));
                    }
                    return;
                }
                StructField field;
                field.Offset = at;
                field.Name = name;
                field.Type = Scan::ValueType::ByteArray;
                field.Size = static_cast<int>(std::clamp<std::int64_t>(static_cast<std::int64_t>(type.Size) * count, 1, kMaxFieldBytes));
                parsed.Definition.Fields.push_back(field);
                if (static_cast<std::int64_t>(type.Size) * count > kMaxFieldBytes)
                    Warn(line, Format("%s (%d elements) is longer than a dissect field can be; it is cut to %d bytes", name.c_str(), count, kMaxFieldBytes));
            }

            // One member line: type, then declarators separated by commas.
            void Member(Parsed& parsed, Layout& layout)
            {
                const int line = Peek().Line;
                if (Is("virtual"))
                {
                    parsed.Virtual = true;
                    SkipStatement();
                    return;
                }
                if (Is("static") || Is("friend") || Is("using") || Is("typedef") || Is("template") || (Is("const") && Is("static", 1)))
                {
                    SkipStatement();
                    return;
                }
                if (Is("~") || (Peek().Kind == TokenKind::Word && Peek().Text == parsed.Definition.Name && Is("(", 1)))
                {
                    SkipStatement();  // a constructor or destructor
                    return;
                }
                m_unknown.clear();
                std::optional<TypeInfo> type = TypeSpec();
                // ∑ Only a pointer to an unknown type: its size does not matter, and it
                //   may be defined further down (or only forward declared).
                if (!type && !m_unknown.empty() && (Is("*") || Is("&")))
                    type = TypeInfo{ Basic::Struct, 0, 1, false, m_unknown };
                // a function pointer: the size does not depend on what it returns
                if (!type && !m_unknown.empty() && Is("("))
                {
                    const std::size_t start = m_at;
                    if (ParenDeclarator(line))
                        type = TypeInfo{ Basic::Struct, 0, 1, false, m_unknown };
                    m_at = start;
                }
                if (!type)
                {
                    if (!m_unknown.empty())
                        Warn(line, "unknown type " + m_unknown + "; the member is left out, so the offsets after it may be wrong");
                    SkipStatement();
                    return;
                }
                for (;;)
                {
                    int stars = 0;
                    while (Accept("*") || Accept("&"))
                        ++stars;
                    // int Class::*name: a pointer to a data member
                    std::string memberOf;
                    if (Accept("::") && !(Peek().Kind == TokenKind::Word && Is("::", 1)))
                    {
                        SkipStatement();
                        return;
                    }
                    while (Peek().Kind == TokenKind::Word && Is("::", 1))
                    {
                        memberOf = Next().Text;
                        Next();
                    }
                    if (!memberOf.empty() && !Accept("*"))
                    {
                        Warn(line, "a declaration with " + memberOf + ":: is not read; the offsets after it may be wrong");
                        SkipStatement();
                        return;
                    }
                    std::optional<TypeInfo> declared;  // set by a declarator that names its own type
                    if (!memberOf.empty())
                        declared = MemberPointer(memberOf, false);
                    std::string name;
                    int count = 0;
                    if (memberOf.empty() && Is("("))
                    {
                        // void (*callback)(), void (__stdcall *table[4])(int), void (Class::*method)()
                        const std::optional<ParenDeclaration> declaration = ParenDeclarator(line);
                        if (!declaration || declaration->Name.empty())
                        {
                            if (!declaration)
                                Warn(line, "a declaration in parentheses is not read; the offsets after it may be wrong");
                            SkipStatement();
                            return;
                        }
                        if (declaration->Method)
                        {
                            SkipStatement();  // a method that returns a function pointer
                            return;
                        }
                        name = declaration->Name;
                        count = declaration->Count;
                        declared = DeclaredType(*declaration, type);
                    }
                    else
                    {
                        if (Is(":") && Peek(1).Kind == TokenKind::Number && !stars && !declared)
                        {
                            // an unnamed bit field: its bits stay unused, : 0 ends the unit
                            Next();
                            const int bits = static_cast<int>(Next().Value);
                            if (bits == 0)
                                layout.UnitOffset = -1;
                            else
                                AddField(parsed, layout, *type, {}, 0, bits, line);
                            if (!Accept(","))
                                break;
                            continue;
                        }
                        if (Peek().Kind != TokenKind::Word)
                        {
                            SkipStatement();
                            return;
                        }
                        name = Next().Text;
                        if (Is("("))
                        {
                            SkipStatement();  // a method
                            return;
                        }
                        if (m_csharp && (Is("{") || (Is("=") && Is(">", 1))))
                        {
                            SkipStatement();  // a property
                            return;
                        }
                        while (Accept("["))
                        {
                            int dimension = 0;
                            if (Peek().Kind == TokenKind::Number)
                                dimension = static_cast<int>(std::min<std::uint64_t>(Next().Value, kMaxFieldOffset));
                            else
                                Warn(line, "the length of " + name + " is not a number");
                            while (!AtEnd() && !Is("]"))
                                Next();
                            Accept("]");
                            count = static_cast<int>(std::min<std::int64_t>(count ? static_cast<std::int64_t>(count) * dimension : dimension, kMaxFieldOffset));
                        }
                    }
                    int bits = 0;
                    if (Accept(":") && Peek().Kind == TokenKind::Number)
                        bits = static_cast<int>(Next().Value);
                    if (Accept("="))
                    {
                        while (!AtEnd() && !Is(",") && !Is(";") && !Is("}"))
                        {
                            if (Is("{") || Is("("))
                                SkipBalanced();
                            else
                                Next();
                        }
                    }
                    TypeInfo actual = declared ? *declared : stars ? PointerTo(*type) : *type;
                    if (declared)
                        bits = 0;
                    if (actual.Kind == Basic::Struct && actual.Size == 0)
                        Warn(line, "the structure " + actual.Struct + " is used before it is defined; " + name + " takes no room");
                    else
                        AddField(parsed, layout, actual, name, count, bits, line);
                    if (!Accept(","))
                        break;
                }
                Accept(";");
            }

            Parsed Body(const std::string& name, bool isUnion, const std::string& base)
            {
                Parsed parsed;
                parsed.Definition.Name = name;
                Layout layout;
                layout.Pack = m_layoutPack ? *m_layoutPack : Pack();
                layout.Union = isUnion;
                const bool explicitLayout = m_explicit;
                const std::optional<int> declaredSize = m_layoutSize;
                bool virtualBase = false;
                m_explicit = false;
                m_layoutSize.reset();
                m_layoutPack.reset();
                if (!base.empty())
                {
                    if (const auto found = m_structs.find(base); found != m_structs.end())
                    {
                        parsed.Definition.Fields = m_ownFields[base];
                        layout.Offset = found->second.first;
                        layout.Align = found->second.second;
                        virtualBase = m_virtual.count(base) > 0;
                    }
                    else
                    {
                        Warn(Peek().Line, "the base class " + base + " is not known; its fields are missing");
                    }
                }
                Accept("{");
                while (!AtEnd() && !Is("}"))
                {
                    if (Peek().Kind == TokenKind::Pack)
                    {
                        PackToken(Next());
                        continue;
                    }
                    if (Is("["))
                    {
                        Attribute();
                        continue;
                    }
                    if (Peek().Kind == TokenKind::Word && (Is(":", 1)) && (Is("public") || Is("private") || Is("protected")))
                    {
                        Next();
                        Next();
                        continue;
                    }
                    if ((Is("struct") || Is("union") || Is("class")) && (Is("{", 1) || (Peek(1).Kind == TokenKind::Word && Is("{", 2))))
                    {
                        // a nested type: anonymous ones put their members here
                        const bool nestedUnion = Is("union");
                        Next();
                        std::string nestedName;
                        if (Peek().Kind == TokenKind::Word)
                            nestedName = Next().Text;
                        Parsed nested = Body(nestedName.empty() ? std::string() : nestedName, nestedUnion, {});
                        const bool declares = Peek().Kind == TokenKind::Word || Is("*");
                        if (nestedName.empty() && !declares)
                        {
                            const int align = std::max(1, std::min(nested.Align, layout.Pack));
                            const int at = layout.Union ? 0 : AlignUp(layout.Offset, align);
                            for (StructField field : nested.Definition.Fields)
                            {
                                field.Offset += at;
                                if (field.Offset >= kMaxFieldOffset)
                                {
                                    Warn(Peek().Line, Format("%s lies beyond the largest offset a dissect field can have (%X); it is left out", field.Name.c_str(), kMaxFieldOffset));
                                    continue;
                                }
                                parsed.Definition.Fields.push_back(field);
                            }
                            if (!layout.Union)
                                layout.Offset = std::min(at + nested.Size, kMaxFieldOffset);
                            layout.UnionSize = std::max(layout.UnionSize, at + nested.Size);
                            layout.Align = std::max(layout.Align, align);
                            Accept(";");
                            continue;
                        }
                        if (nestedName.empty())
                            nestedName = (name.empty() ? std::string("unnamed") : name) + "_" + std::to_string(++m_anonymous);
                        nested.Definition.Name = nestedName;
                        Define(nested);
                        if (declares)
                        {
                            // struct Inner { } member;  -> read the declarators with the new type
                            m_aliases["\x01" + nestedName] = StructType(nestedName);
                            const TypeInfo type = StructType(nestedName);
                            for (;;)
                            {
                                int stars = 0;
                                while (Accept("*"))
                                    ++stars;
                                if (Peek().Kind != TokenKind::Word)
                                    break;
                                const std::string member = Next().Text;
                                int count = 0;
                                while (Accept("["))
                                {
                                    if (Peek().Kind == TokenKind::Number)
                                    {
                                        const std::int64_t dimension = std::min<std::uint64_t>(Next().Value, kMaxFieldOffset);
                                        count = static_cast<int>(std::min<std::int64_t>(count ? count * dimension : dimension, kMaxFieldOffset));
                                    }
                                    while (!AtEnd() && !Is("]"))
                                        Next();
                                    Accept("]");
                                }
                                AddField(parsed, layout, stars ? PointerTo(type) : type, member, count, 0, Peek().Line);
                                if (!Accept(","))
                                    break;
                            }
                        }
                        Accept(";");
                        continue;
                    }
                    if (Is("enum"))
                    {
                        Enum();
                        continue;
                    }
                    if (Is(";"))
                    {
                        Next();
                        continue;
                    }
                    Member(parsed, layout);
                }
                Accept("}");
                // ∑ A class with virtual functions and no base that has them starts with the
                //   pointer to its virtual function table.
                if (parsed.Virtual && !virtualBase && !m_csharp)
                {
                    for (StructField& field : parsed.Definition.Fields)
                        field.Offset += m_pointer;
                    StructField table;
                    table.Name = "vftable";
                    table.Type = Scan::ValueType::Pointer;
                    table.Size = m_pointer;
                    table.Display = FieldDisplay::Hex;
                    parsed.Definition.Fields.insert(parsed.Definition.Fields.begin(), table);
                    layout.Offset += m_pointer;
                    layout.UnionSize += m_pointer;
                    layout.Align = std::max(layout.Align, std::min(m_pointer, layout.Pack));
                }
                parsed.Virtual = parsed.Virtual || virtualBase;
                parsed.Align = layout.Align;
                const int end = layout.Union ? layout.UnionSize : layout.Offset;
                parsed.Size = explicitLayout ? std::max(end, declaredSize.value_or(0)) : std::max(AlignUp(std::max(end, 0), layout.Align), declaredSize.value_or(0));
                if (parsed.Size == 0)
                    parsed.Size = 1;
                return parsed;
            }

            void Define(Parsed& parsed)
            {
                if (parsed.Definition.Name.empty())
                    parsed.Definition.Name = Format("unnamed_%d", ++m_anonymous);
                m_ownFields[parsed.Definition.Name] = parsed.Definition.Fields;
                PadStructure(parsed.Definition, parsed.Size);
                parsed.Definition.Sort();
                m_structs[parsed.Definition.Name] = { parsed.Size, parsed.Align };
                if (parsed.Virtual)
                    m_virtual.insert(parsed.Definition.Name);
                for (StructDefinition& existing : m_import.Structures)
                {
                    if (existing.Name == parsed.Definition.Name)
                    {
                        existing = parsed.Definition;
                        return;
                    }
                }
                m_import.Structures.push_back(parsed.Definition);
            }

            static constexpr int kExpand = 64;

            std::vector<Token> m_tokens;
            std::size_t m_at = 0;
            StructImportOptions m_options;
            bool m_csharp = false;
            int m_pointer = 8;
            std::vector<int> m_pack;
            std::map<std::string, TypeInfo> m_aliases;
            std::map<std::string, std::pair<int, int>> m_structs;  // size, alignment
            std::set<std::string> m_virtual;
            std::map<std::string, InheritanceModel> m_models;  // classes with a base clause; the others are Single
            std::map<std::string, std::vector<StructField>> m_ownFields;  // without the padding at the end, for derived classes
            std::optional<int> m_fieldOffset;
            bool m_explicit = false;
            std::optional<int> m_layoutSize;
            std::optional<int> m_layoutPack;
            std::string m_unknown;
            int m_anonymous = 0;
            StructImport m_import;
        };

        bool LooksLikeCSharp(std::string_view text)
        {
            return text.find("[StructLayout") != std::string_view::npos || text.find("[FieldOffset") != std::string_view::npos ||
                   text.find("using System") != std::string_view::npos || text.find("public unsafe struct") != std::string_view::npos ||
                   text.find("public struct") != std::string_view::npos || text.find("public class") != std::string_view::npos;
        }
    }

    Result<StructImport> ImportStructures(std::string_view text, const StructImportOptions& options)
    {
        Parser parser(Tokenize(text), options, LooksLikeCSharp(text));
        StructImport imported = parser.Run();
        if (imported.Structures.empty())
            return Status::Fail(ErrorCode::StructNotFound, "the text");
        return imported;
    }
}
