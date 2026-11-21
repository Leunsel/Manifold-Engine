#include "Table/CommentList.h"

#include "Core/Text.h"

#include <algorithm>
#include <tuple>

namespace Manifold::Table
{
    NoteKey ParseNoteKey(std::string_view key)
    {
        NoteKey parsed;
        const std::string_view text = TrimView(key);
        if (!text.empty() && text.front() == '"')
        {
            const std::size_t close = text.find('"', 1);
            if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != '+')
                return parsed;
            const auto offset = ParseHex(text.substr(close + 2));
            if (!offset || close == 1)
                return parsed;
            parsed.Module = std::string(text.substr(1, close - 1));
            parsed.Offset = *offset;
            parsed.Parsed = true;
            return parsed;
        }
        if (const auto address = ParseHex(text))
        {
            parsed.Offset = *address;
            parsed.Parsed = true;
        }
        return parsed;
    }

    std::vector<std::string> SortedNoteKeys(const AddressList& list, std::string_view filter)
    {
        struct Entry
        {
            NoteKey Key;
            std::string Lower;
            const std::string* Text;
        };
        const std::string wanted(TrimView(filter));
        std::vector<Entry> entries;
        entries.reserve(list.DisasmNotes.size());
        for (const auto& [key, note] : list.DisasmNotes)
        {
            if (note.Comment.empty() && note.Header.empty())
                continue;
            if (!wanted.empty() && !ContainsNoCase(key, wanted) && !ContainsNoCase(note.Comment, wanted) && !ContainsNoCase(note.Header, wanted))
                continue;
            NoteKey parsed = ParseNoteKey(key);
            std::string lower = Lower(parsed.Module);
            entries.push_back({ std::move(parsed), std::move(lower), &key });
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
            {
                const bool aLast = !a.Key.Parsed;
                const bool bLast = !b.Key.Parsed;
                return std::tie(aLast, a.Lower, a.Key.Offset, *a.Text) < std::tie(bLast, b.Lower, b.Key.Offset, *b.Text);
            });
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        for (const Entry& entry : entries)
            keys.push_back(*entry.Text);
        return keys;
    }

    std::vector<RemovedNote> RemoveNotes(AddressList& list, const std::vector<std::string>& keys)
    {
        std::vector<RemovedNote> removed;
        for (const std::string& key : keys)
        {
            const auto found = list.DisasmNotes.find(key);
            if (found == list.DisasmNotes.end())
                continue;
            removed.push_back({ found->first, std::move(found->second) });
            list.DisasmNotes.erase(found);
        }
        if (!removed.empty())
            list.Touch();
        return removed;
    }

    std::size_t RestoreNotes(AddressList& list, const std::vector<RemovedNote>& removed)
    {
        std::size_t restored = 0;
        for (const RemovedNote& entry : removed)
        {
            if (list.DisasmNotes.emplace(entry.Key, entry.Note).second)
                ++restored;
        }
        if (restored > 0)
            list.Touch();
        return restored;
    }
}
