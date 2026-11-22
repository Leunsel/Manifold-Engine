#include "Table/RecordCheck.h"

#include "Core/Text.h"
#include "Table/RecordPath.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <unordered_map>

namespace Manifold::Table
{
    namespace
    {
        // ∑ Lower case and matched without regard to case, as CE's class methods are. Only the method name is matched, so
        //   getAddressList().getMemoryRecordByDescription, AddressList.getMemoryRecordByDescription and al:getMemoryRecordByDescription all count.
        constexpr const char* kLookups[] = { "getmemoryrecordbydescription", "getmemoryrecordbyid", "manifold.record.find" };

        bool IdentifierChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        std::size_t SkipSpaces(std::string_view text, std::size_t at)
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
                ++at;
            return at;
        }

        // Spaces, tabs and line breaks, for table literals that span lines.
        std::size_t SkipBlank(std::string_view text, std::size_t at)
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
                ++at;
            return at;
        }

        // A Lua string literal at `at` ('...', "..." or [[...]]); returns its text and moves `at` past it.
        std::optional<std::string> StringLiteral(std::string_view text, std::size_t& at)
        {
            if (at >= text.size())
                return std::nullopt;
            const char quote = text[at];
            if (quote == '"' || quote == '\'')
            {
                std::string value;
                for (std::size_t i = at + 1; i < text.size(); ++i)
                {
                    const char c = text[i];
                    if (c == '\n')
                        return std::nullopt;
                    if (c == quote)
                    {
                        at = i + 1;
                        return value;
                    }
                    if (c == '\\' && i + 1 < text.size())
                    {
                        const char next = text[++i];
                        value += next == 'n' ? '\n' : next == 't' ? '\t' : next;
                        continue;
                    }
                    value += c;
                }
                return std::nullopt;
            }
            if (quote == '[' && at + 1 < text.size() && text[at + 1] == '[')
            {
                const std::size_t close = text.find("]]", at + 2);
                if (close == std::string_view::npos)
                    return std::nullopt;
                std::string value(text.substr(at + 2, close - at - 2));
                at = close + 2;
                return value;
            }
            return std::nullopt;
        }

        std::optional<std::int64_t> NumberLiteral(std::string_view text, std::size_t& at)
        {
            std::size_t end = at;
            while (end < text.size() && (IdentifierChar(text[end])))
                ++end;
            if (end == at)
                return std::nullopt;
            const std::string_view word = text.substr(at, end - at);
            std::optional<std::int64_t> value;
            if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X'))
            {
                if (const auto hex = ParseHex(word.substr(2)); hex && *hex <= static_cast<std::uint64_t>(kMaxRecordId))
                    value = static_cast<std::int64_t>(*hex);
            }
            else
                value = ParseDecimal(word);
            if (value)
                at = end;
            return value;
        }

        // ∑ Rough on purpose: a "--" earlier on the line means a comment (a string holding "--" is rare in record lookups), and a
        //   literal followed by ".." is the start of a built name, so it is no literal lookup at all.
        bool InLineComment(std::string_view text, std::size_t at)
        {
            const std::size_t lineStart = text.rfind('\n', at == 0 ? 0 : at - 1);
            const std::size_t from = lineStart == std::string_view::npos ? 0 : lineStart + 1;
            return text.substr(from, at - from).find("--") != std::string_view::npos;
        }

        std::string Quoted(std::string_view text)
        {
            return "\"" + std::string(text) + "\"";
        }

        std::string RecordName(const MemoryRecord& record)
        {
            return record.Description.empty() ? "record " + std::to_string(record.Id) : Quoted(record.Description);
        }

        struct SectionMarks
        {
            bool Enable = false;
            bool Disable = false;
        };

        SectionMarks SectionsOf(std::string_view script)
        {
            SectionMarks marks;
            for (const std::string& line : SplitLines(script))
            {
                const std::string trimmed = Trim(line);
                if (EqualsNoCase(trimmed, "[enable]"))
                    marks.Enable = true;
                else if (EqualsNoCase(trimmed, "[disable]"))
                    marks.Disable = true;
            }
            return marks;
        }

        bool RelativeAddress(const MemoryRecord& record)
        {
            const std::string_view written = TrimView(record.Address);
            return !written.empty() && (written.front() == '+' || written.front() == '-');
        }

        bool HasOwnAddress(const MemoryRecord& record)
        {
            return record.Kind == RecordKind::Value || (record.Kind == RecordKind::Group && record.AddressGroupHeader);
        }

        bool ValueAction(HotkeyAction action)
        {
            return action == HotkeyAction::SetValue || action == HotkeyAction::Increase || action == HotkeyAction::Decrease;
        }

        std::string KeysId(std::vector<int> keys)
        {
            std::sort(keys.begin(), keys.end());
            std::string id;
            for (int key : keys)
                id += std::to_string(key) + ",";
            return id;
        }
    }

    const char* IssueLevelName(IssueLevel level)
    {
        switch (level)
        {
        case IssueLevel::Error: return "Error";
        case IssueLevel::Warning: return "Warning";
        default: return "Info";
        }
    }

    std::string DropDownLinkTarget(const MemoryRecord& record)
    {
        std::string link = Trim(record.DropDownLink);
        if (link.size() >= 2 && link.front() == '"' && link.back() == '"')
            link = link.substr(1, link.size() - 2);
        return link;
    }

    std::vector<MemoryRecord*> RecordsLinkingTo(AddressList& list, const MemoryRecord& record)
    {
        std::vector<MemoryRecord*> linking;
        list.ForEach([&](MemoryRecord& other)
            {
                if (&other != &record && !other.DropDownLink.empty() && list.FindByDescription(DropDownLinkTarget(other)) == &record)
                    linking.push_back(&other);
            });
        return linking;
    }

    int CountWithDescription(AddressList& list, std::string_view description)
    {
        int count = 0;
        list.ForEach([&](MemoryRecord& record)
            {
                if (record.Description == description)
                    ++count;
            });
        return count;
    }

    std::vector<RecordReference> FindRecordReferencesIn(std::string_view code)
    {
        std::vector<RecordReference> found;
        const std::string lower = Lower(code);
        for (const char* lookup : kLookups)
        {
            const std::string_view name = lookup;
            for (std::size_t at = lower.find(name); at != std::string::npos; at = lower.find(name, at + name.size()))
            {
                if (at > 0 && IdentifierChar(lower[at - 1]))
                    continue;
                // ∑ "manifold.record.find" also matches inside a longer dotted name only when the character before is not a dot.
                if (lookup[0] == 'm' && at > 0 && lower[at - 1] == '.')
                    continue;
                std::size_t next = at + name.size();
                if (next < lower.size() && IdentifierChar(lower[next]))
                    continue;
                if (InLineComment(code, at))
                    continue;
                next = SkipSpaces(code, next);
                const bool parenthesis = next < code.size() && code[next] == '(';
                if (parenthesis)
                    next = SkipSpaces(code, next + 1);
                RecordReference reference;
                reference.Function = std::string(code.substr(at, name.size()));
                reference.Line = static_cast<int>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1;
                std::size_t end = next;
                if (lookup[0] == 'm' && parenthesis && end < code.size() && code[end] == '{')
                {
                    // ∑ A path is a table of string literals; anything else in it (a variable, a call) makes it no literal lookup.
                    std::vector<std::string> steps;
                    std::size_t at2 = SkipBlank(code, end + 1);
                    bool literal = true;
                    while (at2 < code.size() && code[at2] != '}')
                    {
                        auto step = StringLiteral(code, at2);
                        if (!step)
                        {
                            literal = false;
                            break;
                        }
                        steps.push_back(std::move(*step));
                        at2 = SkipBlank(code, at2);
                        if (at2 < code.size() && (code[at2] == ',' || code[at2] == ';'))
                            at2 = SkipBlank(code, at2 + 1);
                    }
                    if (!literal || steps.empty() || at2 >= code.size())
                        continue;
                    reference.Path = std::move(steps);
                    reference.Target = PathText(reference.Path);
                    end = at2 + 1;
                }
                else if (auto text = StringLiteral(code, end))
                {
                    reference.Target = std::move(*text);
                    reference.ById = false;
                }
                else if (const auto number = parenthesis ? NumberLiteral(code, end) : std::nullopt)
                {
                    reference.Target = std::to_string(*number);
                    reference.ById = true;
                }
                else
                    continue;
                end = SkipSpaces(code, end);
                if (end + 1 < code.size() && code[end] == '.' && code[end + 1] == '.')
                    continue;
                if (parenthesis && (end >= code.size() || (code[end] != ')' && code[end] != ',')))
                    continue;
                // ∑ The kind follows the literal, not the name: manifold.record.find takes both, and getMemoryRecordByID("Health") is reported as written.
                found.push_back(std::move(reference));
            }
        }
        std::sort(found.begin(), found.end(), [](const RecordReference& a, const RecordReference& b) { return a.Line < b.Line; });
        return found;
    }

    std::vector<RecordReference> FindRecordReferences(AddressList& list)
    {
        std::vector<RecordReference> all;
        const auto add = [&](std::string_view code, int from, const std::string& where)
            {
                for (RecordReference& reference : FindRecordReferencesIn(code))
                {
                    reference.FromRecord = from;
                    reference.Where = where;
                    if (!reference.Path.empty())
                    {
                        const std::vector<MemoryRecord*> matches = FindByPath(list, reference.Path);
                        reference.TargetId = matches.size() == 1 ? matches.front()->Id : -1;
                        reference.Matches = static_cast<int>(matches.size());
                    }
                    else if (reference.ById)
                    {
                        const auto id = ParseDecimal(reference.Target);
                        const MemoryRecord* target = id && *id >= 0 && *id <= kMaxRecordId ? list.FindById(static_cast<int>(*id)) : nullptr;
                        reference.TargetId = target ? target->Id : -1;
                    }
                    else if (const MemoryRecord* target = list.FindByDescription(reference.Target))
                        reference.TargetId = target->Id;
                    all.push_back(std::move(reference));
                }
            };
        add(list.TableLuaScript, kTableLevel, "Lua script of the table");
        for (const TableFile& file : list.Files)
        {
            if (file.Data && file.Name.size() > 4 && EqualsNoCase(std::string_view(file.Name).substr(file.Name.size() - 4), ".lua"))
                add(std::string_view(reinterpret_cast<const char*>(file.Data->data()), file.Data->size()), kTableLevel, "table file " + file.Name);
        }
        for (MemoryRecord* record : list.Flatten())
        {
            if (record->Kind == RecordKind::Script)
                add(record->Script, record->Id, "script");
            add(record->LuaOnActivate, record->Id, "On Activate");
            add(record->LuaOnDeactivate, record->Id, "On Deactivate");
        }
        return all;
    }

    std::vector<RecordIssue> CheckRecords(AddressList& list, const CheckHooks& hooks)
    {
        std::unordered_map<int, std::vector<RecordIssue>> byRecord;
        const auto issue = [&](int id, IssueLevel level, const char* check, std::string text)
            {
                byRecord[id].push_back({ id, level, check, std::move(text) });
            };

        const std::vector<MemoryRecord*> records = list.Flatten();
        std::map<std::string, std::vector<const MemoryRecord*>> hotkeyUsers;
        std::unordered_map<int, const MemoryRecord*> renumbered;

        for (MemoryRecord* recordPointer : records)
        {
            MemoryRecord& record = *recordPointer;
            const int id = record.Id;

            if (record.FileId >= 0 && record.FileId != record.Id)
            {
                renumbered[record.FileId] = &record;
                const MemoryRecord* holder = list.FindById(record.FileId);
                issue(id, IssueLevel::Warning, "id", holder ? Format("The table file gives it ID %d, which %s has, so it got ID %d. Lua that looks for ID %d finds the other record.",
                    record.FileId, RecordName(*holder).c_str(), record.Id, record.FileId)
                    : Format("The table file gives it ID %d, but it got ID %d when the table was loaded. Lua that looks for ID %d finds nothing.", record.FileId, record.Id, record.FileId));
            }
            if (record.Description.empty())
                issue(id, IssueLevel::Info, "description", "No description, so dropdown links and Lua can't find it by name.");

            if (record.Kind == RecordKind::Value)
            {
                if (TrimView(record.Address).empty())
                    issue(id, IssueLevel::Error, "address", "No address.");
                else if (RelativeAddress(record) && !record.Parent)
                    issue(id, IssueLevel::Error, "address", "The address " + Trim(record.Address) + " is relative to the parent, but the record has no parent.");
                else if (RelativeAddress(record) && !HasOwnAddress(*record.Parent))
                    issue(id, IssueLevel::Error, "address", "The address " + Trim(record.Address) + " is relative to the parent " + RecordName(*record.Parent) +
                        ", which has no address (only values and groups shown as address headers have one).");
                for (std::size_t i = 0; i < record.Offsets.size(); ++i)
                {
                    if (TrimView(record.Offsets[i]).empty())
                        issue(id, IssueLevel::Info, "address", Format("Offset %zu of the pointer is empty and counts as 0.", i + 1));
                }
                if (!record.CustomTypeName.empty() && hooks.CustomTypeExists && !hooks.CustomTypeExists(record.CustomTypeName))
                    issue(id, IssueLevel::Error, "type", "The custom type " + Quoted(record.CustomTypeName) + " is not registered, so the value shows ??.");
            }
            else if (record.Kind == RecordKind::Group && record.AddressGroupHeader && TrimView(record.Address).empty())
                issue(id, IssueLevel::Warning, "address", "The group is shown as an address header but has no address.");

            if (record.Kind == RecordKind::Script)
            {
                const SectionMarks sections = SectionsOf(record.Script);
                if (TrimView(record.Script).empty())
                    issue(id, IssueLevel::Error, "script", "The script is empty, so it can't be switched on.");
                else if (!sections.Enable && !sections.Disable)
                    issue(id, IssueLevel::Error, "script", "The script has no [ENABLE] and [DISABLE] sections, so it can't be switched on.");
                else if (!sections.Disable)
                    issue(id, IssueLevel::Warning, "script", "No [DISABLE] section: switching the script off undoes nothing.");
                else if (!sections.Enable)
                    issue(id, IssueLevel::Warning, "script", "No [ENABLE] section: switching the script on does nothing.");
            }

            if (!record.DropDownLink.empty())
            {
                const std::string link = DropDownLinkTarget(record);
                MemoryRecord* target = list.FindByDescription(link);
                if (!target)
                    issue(id, IssueLevel::Error, "dropdown-link", "The dropdown list links to " + Quoted(link) + ", but no record has that description. The record's own list is used.");
                else if (target == &record)
                    issue(id, IssueLevel::Warning, "dropdown-link", "The dropdown list links to the record itself.");
                else
                {
                    const int named = CountWithDescription(list, link);
                    if (named > 1)
                        issue(id, IssueLevel::Warning, "dropdown-link", Format("The dropdown list links to %s, which %d records are called; the first one (ID %d) is used.",
                            Quoted(link).c_str(), named, target->Id));
                    if (!target->DropDownLink.empty())
                        issue(id, IssueLevel::Warning, "dropdown-link", "The dropdown list links to " + Quoted(link) + ", whose list is a link again; links are not followed further, so its own (" +
                            (target->DropDown.empty() ? std::string("empty") : std::to_string(target->DropDown.size()) + " entries") + ") is used.");
                    else if (target->DropDown.empty())
                        issue(id, IssueLevel::Warning, "dropdown-link", "The dropdown list links to " + Quoted(link) + ", which has no dropdown entries.");
                }
            }

            if (record.Children.empty())
            {
                std::vector<std::string> options;
                if (record.Options.HideChildren)
                    options.push_back("hide children");
                if (record.Options.AlwaysHideChildren)
                    options.push_back("always hide children");
                if (record.Options.ActivateChildren)
                    options.push_back("activate children");
                if (record.Options.DeactivateChildren)
                    options.push_back("deactivate children");
                if (record.Options.RecursiveSetValue)
                    options.push_back("set value of children");
                if (!options.empty())
                    issue(id, IssueLevel::Info, "options", "Group options without children: " + Join(options, ", ") + ".");
            }

            std::map<std::string, int> ownKeys;
            for (const Hotkey& hotkey : record.Hotkeys)
            {
                const std::string keys = KeysText(hotkey.Keys);
                if (!UsableHotkeyKeys(hotkey.Keys))
                    issue(id, IssueLevel::Warning, "hotkey", "The hotkey " + (keys.empty() ? std::string("without keys") : keys) + " can't be used (no key, a key code out of range, or only mouse buttons).");
                else
                {
                    const std::string keyId = KeysId(hotkey.Keys);
                    if (++ownKeys[keyId] == 2)
                        issue(id, IssueLevel::Warning, "hotkey", "Two hotkeys of the record use " + keys + "; both run on every press.");
                    if (ownKeys[keyId] == 1)
                        hotkeyUsers[keyId].push_back(&record);
                }
                if (ValueAction(hotkey.Action) && TrimView(hotkey.Value).empty())
                    issue(id, IssueLevel::Warning, "hotkey", "The hotkey " + keys + " (" + HotkeyActionName(hotkey.Action) + ") has no value, so it does nothing.");
                else if (ValueAction(hotkey.Action) && record.Kind != RecordKind::Value && !record.Options.RecursiveSetValue)
                    issue(id, IssueLevel::Info, "hotkey", "The hotkey " + keys + " sets a value, but the record holds none and doesn't set its children's.");
            }
        }

        // ∑ Records with the same full path are real duplicates (a group pasted twice): no lookup can tell them apart. Only the
        //   topmost of a duplicated tree is reported; the records below it share its problem.
        std::map<std::vector<std::string>, std::vector<const MemoryRecord*>> byPath;
        for (const MemoryRecord* record : records)
            byPath[RecordPath(*record)].push_back(record);
        for (const auto& [path, same] : byPath)
        {
            if (same.size() < 2)
                continue;
            const MemoryRecord* parent = same.front()->Parent;
            if (parent && byPath[RecordPath(*parent)].size() > 1)
                continue;
            std::vector<std::string> ids;
            for (const MemoryRecord* record : same)
                ids.push_back(std::to_string(record->Id));
            for (const MemoryRecord* record : same)
                issue(record->Id, IssueLevel::Warning, "path", Format("%zu records have the path %s (IDs %s), so no lookup by name or path can tell them apart.", same.size(),
                    Quoted(PathText(path)).c_str(), Join(ids, ", ").c_str()));
        }

        for (const auto& [keyId, users] : hotkeyUsers)
        {
            if (users.size() < 2)
                continue;
            std::vector<std::string> names;
            for (const MemoryRecord* user : users)
                names.push_back(RecordName(*user));
            for (const MemoryRecord* user : users)
            {
                const Hotkey* hotkey = nullptr;
                for (const Hotkey& candidate : user->Hotkeys)
                {
                    if (KeysId(candidate.Keys) == keyId)
                        hotkey = &candidate;
                }
                issue(user->Id, IssueLevel::Info, "hotkey", Format("%s is also a hotkey of %zu other records (%s); one press triggers all of them.",
                    hotkey ? KeysText(hotkey->Keys).c_str() : "The key", users.size() - 1, Join(names, ", ").c_str()));
            }
        }

        for (const RecordReference& reference : FindRecordReferences(list))
        {
            const std::string at = reference.Where + ", line " + std::to_string(reference.Line);
            if (!reference.Path.empty())
            {
                if (reference.Matches == 0)
                    issue(reference.FromRecord, IssueLevel::Warning, "lua-reference", "Lua in the " + at + " looks for the path " + Quoted(reference.Target) + ", which is not in the table.");
                else if (reference.Matches > 1)
                    issue(reference.FromRecord, IssueLevel::Warning, "lua-reference", Format("Lua in the %s looks for the path %s, which %d records have; the lookup fails with ME-6011.",
                        at.c_str(), Quoted(reference.Target).c_str(), reference.Matches));
                continue;
            }
            if (reference.TargetId < 0)
            {
                std::string text = "Lua in the " + at + " looks for " + (reference.ById ? "ID " + reference.Target : "record " + Quoted(reference.Target)) + " (" + reference.Function +
                    "), which is not in the table.";
                if (reference.ById)
                {
                    if (const auto id = ParseDecimal(reference.Target); id)
                    {
                        if (const auto moved = renumbered.find(static_cast<int>(*id)); moved != renumbered.end())
                            text += Format(" The file gave that ID to %s, which is ID %d now.", RecordName(*moved->second).c_str(), moved->second->Id);
                    }
                }
                issue(reference.FromRecord, IssueLevel::Warning, "lua-reference", text);
                continue;
            }
            if (reference.ById)
            {
                const auto id = ParseDecimal(reference.Target);
                if (const auto moved = id ? renumbered.find(static_cast<int>(*id)) : renumbered.end(); moved != renumbered.end())
                    issue(reference.FromRecord, IssueLevel::Warning, "lua-reference", Format("Lua in the %s looks for ID %s. The file gave that ID to %s too, which is ID %d now, so Lua finds the other one.",
                        at.c_str(), reference.Target.c_str(), RecordName(*moved->second).c_str(), moved->second->Id));
                continue;
            }
            if (const int named = CountWithDescription(list, reference.Target); named > 1)
                issue(reference.FromRecord, IssueLevel::Warning, "lua-reference", Format("Lua in the %s looks for %s, which %d records are called; it gets the first one (ID %d).",
                    at.c_str(), Quoted(reference.Target).c_str(), named, reference.TargetId));
        }

        std::vector<RecordIssue> ordered;
        if (const auto table = byRecord.find(kTableLevel); table != byRecord.end())
            ordered = std::move(table->second);
        for (const MemoryRecord* record : records)
        {
            if (const auto found = byRecord.find(record->Id); found != byRecord.end())
                ordered.insert(ordered.end(), std::make_move_iterator(found->second.begin()), std::make_move_iterator(found->second.end()));
        }
        return ordered;
    }
}
