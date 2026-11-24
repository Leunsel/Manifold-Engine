#include "Table/TableCleanup.h"

#include "Core/Text.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <set>

namespace Manifold::Table
{
    namespace
    {
        bool WordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
        }

        char Fold(char c)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool ContainsFolded(std::string_view text, std::string_view piece)
        {
            if (piece.empty() || piece.size() > text.size())
                return false;
            for (std::size_t at = 0; at + piece.size() <= text.size(); ++at)
            {
                std::size_t i = 0;
                while (i < piece.size() && Fold(text[at + i]) == Fold(piece[i]))
                    ++i;
                if (i == piece.size())
                    return true;
            }
            return false;
        }

        bool NameSeparator(char c)
        {
            return c == '.' || c == '-' || c == '_' || c == ' ';
        }

        // ∑ A file a script loads by a name it puts together is never named whole. The leading
        //   pieces of the name ("Manifold.Dark-") or its trailing ones (".Min.json", not just the
        //   extension) in a script say it may be one of those.
        std::string BuiltNameHint(const struct Corpus& corpus, std::string_view name);

        // Text table files (Lua, CEA, JSON...) take part in the search; a binary one does not.
        bool LooksLikeText(const std::vector<std::uint8_t>& data)
        {
            const std::size_t probe = std::min<std::size_t>(data.size(), 4096);
            std::size_t odd = 0;
            for (std::size_t i = 0; i < probe; ++i)
            {
                const std::uint8_t byte = data[i];
                if (byte == 0)
                    return false;
                if (byte < 0x09 || (byte > 0x0D && byte < 0x20))
                    ++odd;
            }
            return odd * 50 <= probe;
        }

        std::string Extension(std::string_view name)
        {
            const std::size_t dot = name.rfind('.');
            return dot == std::string_view::npos ? std::string() : Lower(name.substr(dot + 1));
        }

        std::string Stem(std::string_view name)
        {
            const std::size_t dot = name.rfind('.');
            return std::string(dot == std::string_view::npos ? name : name.substr(0, dot));
        }

        void ForEachRecord(const std::vector<std::unique_ptr<MemoryRecord>>& records, const std::function<void(const MemoryRecord&)>& visit)
        {
            for (const auto& record : records)
            {
                visit(*record);
                ForEachRecord(record->Children, visit);
            }
        }

        bool ForEachRecordMutable(std::vector<std::unique_ptr<MemoryRecord>>& records, const std::function<bool(std::vector<std::unique_ptr<MemoryRecord>>&, std::size_t)>& remove)
        {
            bool changed = false;
            for (std::size_t i = 0; i < records.size();)
            {
                if (remove(records, i))
                {
                    records.erase(records.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    continue;
                }
                changed = ForEachRecordMutable(records[i]->Children, remove) || changed;
                ++i;
            }
            return changed;
        }

        // Everything a name can be used from, with the text of one table file left out (its own
        // content does not make it used).
        struct Corpus
        {
            std::string Common;
            std::vector<std::pair<std::string, std::string>> Files;  // name, text

            bool Names(std::string_view name, std::string_view skipFile = {}) const
            {
                if (NamedIn(Common, name))
                    return true;
                return std::any_of(Files.begin(), Files.end(), [&](const auto& file) { return file.first != skipFile && NamedIn(file.second, name); });
            }

            // A piece of text anywhere, not as a word: a name a script puts together ("Manifold.Dark-" .. theme .. ".Min.json").
            bool Mentions(std::string_view piece, std::string_view skipFile = {}) const
            {
                if (ContainsFolded(Common, piece))
                    return true;
                return std::any_of(Files.begin(), Files.end(), [&](const auto& file) { return file.first != skipFile && ContainsFolded(file.second, piece); });
            }
        };

        std::string BuiltNameHint(const Corpus& corpus, std::string_view name)
        {
            const std::size_t extension = name.rfind('.');
            for (std::size_t at = name.size() - 1; at >= 6 && at != std::string_view::npos; --at)
            {
                if (!NameSeparator(name[at - 1]) || at - 1 >= (extension == std::string_view::npos ? name.size() : extension))
                    continue;
                const std::string_view head = name.substr(0, at);
                if (head.size() >= 6 && corpus.Mentions(head, name))
                    return std::string(head);
            }
            for (std::size_t at = 1; extension != std::string_view::npos && at < extension; ++at)
            {
                if (!NameSeparator(name[at]))
                    continue;
                const std::string_view tail = name.substr(at);
                if (tail.size() >= 5 && corpus.Mentions(tail, name))
                    return std::string(tail);
            }
            return {};
        }

        Corpus Gather(const LoadedTable& table)
        {
            Corpus corpus;
            std::string& text = corpus.Common;
            ForEachRecord(table.Records, [&](const MemoryRecord& record)
                {
                    text += record.Script;
                    text += '\n';
                    text += record.LuaOnActivate;
                    text += '\n';
                    text += record.LuaOnDeactivate;
                    text += '\n';
                    text += record.Address;
                    text += '\n';
                    for (const std::string& offset : record.Offsets)
                    {
                        text += offset;
                        text += '\n';
                    }
                    text += record.DropDownLink;
                    text += '\n';
                    text += record.CustomTypeName;
                    text += '\n';
                });
            text += table.LuaScript;
            text += '\n';
            for (const TableSymbol& symbol : table.Symbols)
            {
                text += symbol.Address;
                text += '\n';
            }
            for (const auto& structure : table.Structures)
            {
                for (const StructField& field : structure->Fields)
                {
                    text += field.ChildStructure;
                    text += '\n';
                    text += field.CustomType;
                    text += '\n';
                }
            }
            for (const std::string& xml : table.ExtraXml)
            {
                text += xml;
                text += '\n';
            }
            for (const TableFile& file : table.Files)
            {
                if (file.Data && LooksLikeText(*file.Data))
                    corpus.Files.emplace_back(file.Name, std::string(file.Data->begin(), file.Data->end()));
            }
            return corpus;
        }

        // The structure's part of the saved file, near enough: its tag and one line per field.
        std::size_t StructureBytes(const StructDefinition& structure)
        {
            std::size_t bytes = 80 + structure.Name.size();
            for (const StructField& field : structure.Fields)
                bytes += 110 + field.Name.size() + field.ChildStructure.size();
            return bytes;
        }

        bool IsAutoAssemblerFile(std::string_view name)
        {
            const std::string extension = Extension(name);
            return extension == "cea" || extension == "aa";
        }

        void BuildList(LoadedTable& table, AddressList& list, StructureStore& structures)
        {
            list.TableLuaScript = table.LuaScript;
            list.TableComment = table.Comments;
            list.DisasmNotes = std::move(table.DisasmNotes);
            list.Files = std::move(table.Files);
            list.ExtraXml = std::move(table.ExtraXml);
            list.LoadedVersion = table.Version;
            list.LoadedFileNaming = table.Naming;
            const std::function<void(const MemoryRecord&)> noteIds = [&](const MemoryRecord& record)
                {
                    list.NoteId(record.Id);
                    for (const auto& child : record.Children)
                        noteIds(*child);
                };
            for (const auto& record : table.Records)
                noteIds(*record);
            for (auto& record : table.Records)
                list.Add(std::move(record));
            const std::function<void(MemoryRecord&)> fix = [&](MemoryRecord& record)
                {
                    for (auto& child : record.Children)
                    {
                        child->Parent = &record;
                        fix(*child);
                    }
                };
            for (auto& record : list.Roots())
                fix(*record);
            for (auto& structure : table.Structures)
                structures.Items.push_back(std::move(structure));
        }
    }

    const char* CleanupKindName(CleanupKind kind)
    {
        switch (kind)
        {
        case CleanupKind::Structure: return "Structure";
        case CleanupKind::Symbol: return "Symbol";
        case CleanupKind::File: return "Table File";
        case CleanupKind::EmptyGroup: return "Empty Group";
        case CleanupKind::DisasmComments: return "Disassembler Comments";
        case CleanupKind::TableComment: return "Table Comment";
        case CleanupKind::ScriptComments: return "Script Comments";
        case CleanupKind::FileComments: return "File Comments";
        }
        return "";
    }

    bool NamedIn(std::string_view text, std::string_view name)
    {
        if (name.empty() || name.size() > text.size())
            return false;
        const char first = Fold(name.front());
        for (std::size_t at = 0; at + name.size() <= text.size(); ++at)
        {
            if (Fold(text[at]) != first)
                continue;
            std::size_t i = 1;
            while (i < name.size() && Fold(text[at + i]) == Fold(name[i]))
                ++i;
            if (i != name.size())
                continue;
            const bool before = at == 0 || !WordChar(text[at - 1]) || !WordChar(name.front());
            const bool after = at + name.size() == text.size() || !WordChar(text[at + name.size()]) || !WordChar(name.back());
            if (before && after)
                return true;
        }
        return false;
    }

    std::string StripAutoAssemblerComments(std::string_view script)
    {
        constexpr char kRemoved = '\x01';  // where a comment was, so a line that held only one goes too
        std::string out;
        out.reserve(script.size());
        bool lua = false;
        char quote = 0;
        std::size_t at = 0;
        while (at < script.size())
        {
            const char c = script[at];
            if (c == '\n')
                quote = 0;
            if (lua)
            {
                // a Lua part ends at {$asm}; inside it, // is integer division and -- the comment
                if (c == '{' && Lower(script.substr(at, 6)) == "{$asm}")
                {
                    lua = false;
                    out.append(script.substr(at, 6));
                    at += 6;
                    continue;
                }
                out += c;
                ++at;
                continue;
            }
            if (quote)
            {
                if (c == quote)
                    quote = 0;
                out += c;
                ++at;
                continue;
            }
            if (c == '\'' || c == '"')
            {
                quote = c;
                out += c;
                ++at;
                continue;
            }
            if (c == '/' && at + 1 < script.size() && script[at + 1] == '/')
            {
                while (at < script.size() && script[at] != '\n')
                    ++at;
                out += kRemoved;
                continue;
            }
            if (c == '{')
            {
                const std::size_t close = script.find('}', at + 1);
                if (at + 1 < script.size() && script[at + 1] == '$')
                {
                    const std::size_t end = close == std::string_view::npos ? script.size() : close + 1;
                    const std::string directive = Lower(script.substr(at, end - at));
                    out.append(script.substr(at, end - at));
                    if (directive.starts_with("{$lua"))
                        lua = true;
                    at = end;
                    continue;
                }
                if (close == std::string_view::npos)
                {
                    out.append(script.substr(at));
                    break;
                }
                // a block comment keeps its line breaks out: the lines it spanned close up
                at = close + 1;
                out += kRemoved;
                continue;
            }
            out += c;
            ++at;
        }
        // trailing blanks off every line, no empty lines at the start, never two in a row
        const std::string newline = script.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        std::string tidy;
        tidy.reserve(out.size());
        bool lastEmpty = true;
        std::size_t start = 0;
        while (start <= out.size())
        {
            const std::size_t end = std::min(out.find('\n', start), out.size());
            std::string line(std::string_view(out).substr(start, end - start));
            const bool hadComment = line.find(kRemoved) != std::string::npos;
            std::erase(line, kRemoved);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                line.pop_back();
            // a line that held only a comment goes away; an empty line of the script stays (one in a row)
            const bool blank = line.find_first_not_of(" \t") == std::string::npos;
            if (hadComment && blank)
            {
                if (end == out.size())
                    break;
                start = end + 1;
                continue;
            }
            if (blank)
                line.clear();
            if (!line.empty() || !lastEmpty)
            {
                tidy.append(line);
                tidy += newline;
            }
            lastEmpty = line.empty();
            if (end == out.size())
                break;
            start = end + 1;
        }
        while (tidy.ends_with(newline + newline))
            tidy.resize(tidy.size() - newline.size());
        if (!script.ends_with('\n') && tidy.ends_with(newline))
            tidy.resize(tidy.size() - newline.size());
        return tidy;
    }

    CleanupPlan PlanCleanup(const LoadedTable& table, std::size_t originalBytes)
    {
        CleanupPlan plan;
        plan.OriginalBytes = originalBytes;
        const Corpus corpus = Gather(table);

        for (const auto& structure : table.Structures)
        {
            if (corpus.Names(structure->Name))
                continue;
            CleanupItem item;
            item.Kind = CleanupKind::Structure;
            item.Name = structure->Name;
            item.Reason = Format("%zu fields; no script, Lua, record or other structure names it", structure->Fields.size());
            item.Bytes = StructureBytes(*structure);
            item.Key = "structure:" + structure->Name;
            plan.Items.push_back(std::move(item));
        }

        for (const TableSymbol& symbol : table.Symbols)
        {
            if (corpus.Names(symbol.Name))
                continue;
            CleanupItem item;
            item.Kind = CleanupKind::Symbol;
            item.Name = symbol.Name;
            item.Reason = "= " + symbol.Address + "; no script, Lua or record uses it";
            item.Bytes = 70 + symbol.Name.size() + symbol.Address.size();
            item.Key = "symbol:" + symbol.Name;
            plan.Items.push_back(std::move(item));
        }

        for (const TableFile& file : table.Files)
        {
            const std::string stem = Stem(file.Name);
            if (corpus.Names(file.Name, file.Name) || (stem.size() >= 3 && corpus.Names(stem, file.Name)))
                continue;
            CleanupItem item;
            item.Kind = CleanupKind::File;
            item.Name = file.Name;
            const std::size_t size = file.Data ? file.Data->size() : 0;
            item.Reason = ByteSize(size) + "; no script, Lua or other file names it";
            if (const std::string hint = BuiltNameHint(corpus, file.Name); !hint.empty())
            {
                item.Recommended = false;
                item.Reason = ByteSize(size) + "; not named whole, but a script mentions \"" + hint + "\": it may load it by a name it puts together";
            }
            item.Bytes = file.Data ? EncodeTableFile(*file.Data).size() + 60 + file.Name.size() : 0;
            item.Key = "file:" + file.Name;
            plan.Items.push_back(std::move(item));
        }

        ForEachRecord(table.Records, [&](const MemoryRecord& record)
            {
                if (record.Kind != RecordKind::Group || !record.Children.empty() || !record.LuaOnActivate.empty() || !record.LuaOnDeactivate.empty() ||
                    !record.Hotkeys.empty() || !record.Script.empty())
                    return;
                // ∑ Not offered at all: an unnamed one is a spacer between sections, and one with a
                //   drop-down list holds the list other records link to (DropDownListLink).
                if (Trim(record.Description).empty() || !record.DropDown.empty())
                    return;
                // Lua may look the group up to add records to it at run time
                if (!record.Description.empty() && corpus.Names(record.Description))
                    return;
                CleanupItem item;
                item.Kind = CleanupKind::EmptyGroup;
                item.Name = record.Description.empty() ? Format("(ID %d)", record.Id) : record.Description;
                item.Reason = Format("ID %d, nothing in it, no Lua names it; it still shows in the list as a heading, so kept unless chosen", record.Id);
                item.Bytes = 160 + record.Description.size();
                item.Recommended = false;
                item.RecordId = record.Id;
                item.Key = Format("group:%d", record.Id);
                plan.Items.push_back(std::move(item));
            });

        if (!table.DisasmNotes.empty())
        {
            std::size_t bytes = 0;
            for (const auto& [address, note] : table.DisasmNotes)
                bytes += 80 + address.size() + note.Comment.size() + note.Header.size();
            CleanupItem item;
            item.Kind = CleanupKind::DisasmComments;
            item.Name = Format("%zu disassembler comments", table.DisasmNotes.size());
            item.Reason = "notes from working on the table; the memory viewer shows them, nothing runs them";
            item.Bytes = bytes;
            item.Key = "disasm";
            plan.Items.push_back(std::move(item));
        }

        if (!Trim(table.Comments).empty())
        {
            CleanupItem item;
            item.Kind = CleanupKind::TableComment;
            item.Name = "The table's comment";
            item.Reason = "Table Extras > Comments; often the credits and how to use the table, so kept unless chosen";
            item.Bytes = table.Comments.size() + 30;
            item.Recommended = false;
            item.Key = "comment";
            plan.Items.push_back(std::move(item));
        }

        std::size_t scripts = 0;
        std::size_t saved = 0;
        ForEachRecord(table.Records, [&](const MemoryRecord& record)
            {
                if (record.Kind != RecordKind::Script || record.Script.empty())
                    return;
                const std::size_t stripped = StripAutoAssemblerComments(record.Script).size();
                if (stripped < record.Script.size())
                {
                    ++scripts;
                    saved += record.Script.size() - stripped;
                }
            });
        if (scripts)
        {
            CleanupItem item;
            item.Kind = CleanupKind::ScriptComments;
            item.Name = Format("Comments in %zu Auto Assembler scripts", scripts);
            item.Reason = "// and { } comments; {$lua} parts and {$...} lines stay. Headers often hold the credits, so kept unless chosen";
            item.Bytes = saved;
            item.Recommended = false;
            item.Key = "scriptcomments";
            plan.Items.push_back(std::move(item));
        }

        std::size_t files = 0;
        saved = 0;
        for (const TableFile& file : table.Files)
        {
            if (!file.Data || !IsAutoAssemblerFile(file.Name) || !LooksLikeText(*file.Data))
                continue;
            const std::string text(file.Data->begin(), file.Data->end());
            const std::size_t stripped = StripAutoAssemblerComments(text).size();
            if (stripped < text.size())
            {
                ++files;
                saved += text.size() - stripped;
            }
        }
        if (files)
        {
            CleanupItem item;
            item.Kind = CleanupKind::FileComments;
            item.Name = Format("Comments in %zu .CEA files", files);
            item.Reason = "the same in the Auto Assembler files of the table; kept unless chosen";
            item.Bytes = saved / 2;  // deflated in the file
            item.Recommended = false;
            item.Key = "filecomments";
            plan.Items.push_back(std::move(item));
        }
        return plan;
    }

    Result<std::string> CleanTableText(std::string_view original, const std::vector<CleanupItem>& chosen)
    {
        LoadedTable table;
        if (Status parsed = ParseTable(original, table); !parsed)
            return parsed;
        std::set<std::string> structures;
        std::set<std::string> symbols;
        std::set<std::string> files;
        std::set<int> groups;
        bool disasm = false;
        bool comment = false;
        bool scriptComments = false;
        bool fileComments = false;
        for (const CleanupItem& item : chosen)
        {
            switch (item.Kind)
            {
            case CleanupKind::Structure: structures.insert(item.Name); break;
            case CleanupKind::Symbol: symbols.insert(item.Name); break;
            case CleanupKind::File: files.insert(item.Name); break;
            case CleanupKind::EmptyGroup: groups.insert(item.RecordId); break;
            case CleanupKind::DisasmComments: disasm = true; break;
            case CleanupKind::TableComment: comment = true; break;
            case CleanupKind::ScriptComments: scriptComments = true; break;
            case CleanupKind::FileComments: fileComments = true; break;
            }
        }
        std::erase_if(table.Structures, [&](const auto& structure) { return structures.count(structure->Name) > 0; });
        std::erase_if(table.Symbols, [&](const TableSymbol& symbol) { return symbols.count(symbol.Name) > 0; });
        std::erase_if(table.Files, [&](const TableFile& file) { return files.count(file.Name) > 0; });
        if (!groups.empty())
        {
            ForEachRecordMutable(table.Records, [&](std::vector<std::unique_ptr<MemoryRecord>>& siblings, std::size_t index)
                {
                    const MemoryRecord& record = *siblings[index];
                    return record.Kind == RecordKind::Group && record.Children.empty() && groups.count(record.Id) > 0;
                });
        }
        if (disasm)
            table.DisasmNotes.clear();
        if (comment)
            table.Comments.clear();
        if (scriptComments)
        {
            const std::function<void(std::vector<std::unique_ptr<MemoryRecord>>&)> strip = [&](std::vector<std::unique_ptr<MemoryRecord>>& records)
                {
                    for (auto& record : records)
                    {
                        if (record->Kind == RecordKind::Script)
                            record->Script = StripAutoAssemblerComments(record->Script);
                        strip(record->Children);
                    }
                };
            strip(table.Records);
        }
        if (fileComments)
        {
            for (TableFile& file : table.Files)
            {
                if (!file.Data || !IsAutoAssemblerFile(file.Name) || !LooksLikeText(*file.Data))
                    continue;
                const std::string text = StripAutoAssemblerComments(std::string(file.Data->begin(), file.Data->end()));
                file.Data = std::make_shared<std::vector<std::uint8_t>>(text.begin(), text.end());
            }
        }
        AddressList list;
        StructureStore store;
        const std::vector<TableSymbol> keptSymbols = table.Symbols;
        const FileNaming naming = table.Naming;
        BuildList(table, list, store);
        return WriteTable(list, store, keptSymbols, naming);
    }
}
