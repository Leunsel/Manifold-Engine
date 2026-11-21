#include "Text/TextBuffer.h"

#include <algorithm>

namespace Manifold::Text
{
    namespace
    {
        constexpr std::size_t kUndoLimit = 1000;
        constexpr std::size_t kGroupEdits = 512;

        bool IsContinuation(unsigned char c)
        {
            return (c & 0xC0) == 0x80;
        }

        bool IsBlank(char c)
        {
            return c == ' ' || c == '\t';
        }

        int Class(unsigned char c)
        {
            if (c == '\n')
                return 3;
            if (IsBlank(static_cast<char>(c)))
                return 0;
            if (IsWordByte(c))
                return 1;
            return 2;
        }

        std::size_t Leading(std::string_view text)
        {
            std::size_t count = 0;
            while (count < text.size() && IsBlank(text[count]))
                ++count;
            return count;
        }

        bool StartsWord(std::string_view text, std::string_view word)
        {
            return text.compare(0, word.size(), word) == 0 && (text.size() == word.size() || !IsWordByte(static_cast<unsigned char>(text[word.size()])));
        }

        bool StartsCloser(std::string_view text)
        {
            if (!text.empty() && (text.front() == '}' || text.front() == ')' || text.front() == ']'))
                return true;
            return StartsWord(text, "end") || StartsWord(text, "else") || StartsWord(text, "elseif") || StartsWord(text, "until");
        }

        bool Blank(std::string_view text)
        {
            return Leading(text) == text.size();
        }
    }

    bool IsWordByte(unsigned char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
    }

    int Columns(std::string_view text, int tabWidth, int start)
    {
        int column = start;
        for (const char raw : text)
        {
            const auto c = static_cast<unsigned char>(raw);
            if (c == '\t')
                column += tabWidth - column % tabWidth;
            else if (!IsContinuation(c))
                ++column;
        }
        return column;
    }

    std::vector<std::size_t> FindAll(std::string_view text, std::string_view needle, SearchOptions options)
    {
        std::vector<std::size_t> found;
        if (needle.empty() || needle.size() > text.size())
            return found;
        const auto fold = [&](char c)
        {
            return options.MatchCase || c < 'A' || c > 'Z' ? c : static_cast<char>(c - 'A' + 'a');
        };
        const char first = fold(needle.front());
        const std::size_t last = text.size() - needle.size();
        for (std::size_t at = 0; at <= last;)
        {
            if (fold(text[at]) != first)
            {
                ++at;
                continue;
            }
            bool same = true;
            for (std::size_t i = 1; i < needle.size() && same; ++i)
                same = fold(text[at + i]) == fold(needle[i]);
            if (same && options.WholeWord)
            {
                const bool before = at > 0 && IsWordByte(static_cast<unsigned char>(text[at - 1]));
                const bool after = at + needle.size() < text.size() && IsWordByte(static_cast<unsigned char>(text[at + needle.size()]));
                same = !before && !after;
            }
            if (same)
            {
                found.push_back(at);
                at += needle.size();
            }
            else
            {
                ++at;
            }
        }
        return found;
    }

    int NextMatch(const std::vector<std::size_t>& matches, std::size_t from, bool forward)
    {
        if (matches.empty())
            return -1;
        if (forward)
        {
            const auto it = std::lower_bound(matches.begin(), matches.end(), from);
            return it == matches.end() ? 0 : static_cast<int>(it - matches.begin());
        }
        const auto it = std::lower_bound(matches.begin(), matches.end(), from);
        return it == matches.begin() ? static_cast<int>(matches.size()) - 1 : static_cast<int>(it - matches.begin()) - 1;
    }

    TextBuffer::TextBuffer(std::string text) : m_text(std::move(text)) {}

    void TextBuffer::SetText(std::string text)
    {
        m_text = std::move(text);
        m_caret = std::min(m_caret, m_text.size());
        while (m_caret > 0 && m_caret < m_text.size() && IsContinuation(static_cast<unsigned char>(m_text[m_caret])))
            --m_caret;
        m_anchor = m_caret;
        m_preferredColumn = -1;
        ++m_revision;
        m_changedFrom = 0;
        ClearHistory();
    }

    std::size_t TextBuffer::TakeChanges()
    {
        const std::size_t from = m_changedFrom;
        m_changedFrom = std::string::npos;
        return from;
    }

    void TextBuffer::ClearHistory()
    {
        m_undo.clear();
        m_redo.clear();
        m_coalesce = false;
    }

    void TextBuffer::Lines() const
    {
        if (m_linesRevision == m_revision)
            return;
        m_lineStarts.clear();
        m_lineStarts.push_back(0);
        for (std::size_t i = 0; i < m_text.size(); ++i)
        {
            if (m_text[i] == '\n')
                m_lineStarts.push_back(i + 1);
        }
        m_linesRevision = m_revision;
    }

    int TextBuffer::LineCount() const
    {
        Lines();
        return static_cast<int>(m_lineStarts.size());
    }

    int TextBuffer::LineOf(std::size_t offset) const
    {
        Lines();
        const auto found = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), std::min(offset, m_text.size()));
        return static_cast<int>(found - m_lineStarts.begin()) - 1;
    }

    std::size_t TextBuffer::LineStart(int line) const
    {
        Lines();
        line = std::clamp(line, 0, static_cast<int>(m_lineStarts.size()) - 1);
        return m_lineStarts[static_cast<std::size_t>(line)];
    }

    std::size_t TextBuffer::LineEnd(int line) const
    {
        Lines();
        line = std::clamp(line, 0, static_cast<int>(m_lineStarts.size()) - 1);
        if (static_cast<std::size_t>(line) + 1 < m_lineStarts.size())
            return m_lineStarts[static_cast<std::size_t>(line) + 1] - 1;
        return m_text.size();
    }

    std::string_view TextBuffer::Line(int line) const
    {
        const std::size_t start = LineStart(line);
        return std::string_view(m_text).substr(start, LineEnd(line) - start);
    }

    int TextBuffer::ColumnOf(std::size_t offset) const
    {
        offset = std::min(offset, m_text.size());
        const std::size_t start = LineStart(LineOf(offset));
        return Columns(std::string_view(m_text).substr(start, offset - start), TabWidth);
    }

    std::size_t TextBuffer::OffsetAt(int line, int column) const
    {
        const std::size_t start = LineStart(line);
        const std::size_t end = LineEnd(line);
        int current = 0;
        std::size_t at = start;
        while (at < end)
        {
            const std::size_t next = Next(at);
            const int width = m_text[at] == '\t' ? TabWidth - current % TabWidth : 1;
            if (column <= current)
                return at;
            if (column < current + width)
                return column - current <= width / 2 ? at : next;
            current += width;
            at = next;
        }
        return end;
    }

    int TextBuffer::LongestLine() const
    {
        if (m_longestRevision == m_revision)
            return m_longest;
        int longest = 0;
        const int count = LineCount();
        for (int line = 0; line < count; ++line)
            longest = std::max(longest, Columns(Line(line), TabWidth));
        m_longest = longest;
        m_longestRevision = m_revision;
        return longest;
    }

    std::size_t TextBuffer::Next(std::size_t offset) const
    {
        if (offset >= m_text.size())
            return m_text.size();
        ++offset;
        while (offset < m_text.size() && IsContinuation(static_cast<unsigned char>(m_text[offset])))
            ++offset;
        return offset;
    }

    std::size_t TextBuffer::Previous(std::size_t offset) const
    {
        if (offset == 0)
            return 0;
        offset = std::min(offset, m_text.size()) - 1;
        while (offset > 0 && IsContinuation(static_cast<unsigned char>(m_text[offset])))
            --offset;
        return offset;
    }

    std::pair<std::size_t, std::size_t> TextBuffer::Selection() const
    {
        return { std::min(m_caret, m_anchor), std::max(m_caret, m_anchor) };
    }

    std::string TextBuffer::SelectedText() const
    {
        const auto [start, end] = Selection();
        return m_text.substr(start, end - start);
    }

    void TextBuffer::SetCaret(std::size_t offset, bool extend)
    {
        offset = std::min(offset, m_text.size());
        while (offset > 0 && offset < m_text.size() && IsContinuation(static_cast<unsigned char>(m_text[offset])))
            --offset;
        m_caret = offset;
        if (!extend)
            m_anchor = offset;
        m_preferredColumn = -1;
        m_coalesce = false;
    }

    void TextBuffer::Select(std::size_t anchor, std::size_t caret)
    {
        SetCaret(anchor, false);
        SetCaret(caret, true);
    }

    void TextBuffer::SelectAll()
    {
        Select(0, m_text.size());
    }

    void TextBuffer::SelectWordAt(std::size_t offset)
    {
        offset = std::min(offset, m_text.size());
        if (offset == m_text.size() || m_text[offset] == '\n')
        {
            if (offset == 0 || m_text[offset - 1] == '\n')
            {
                Select(offset, offset);
                return;
            }
            offset = Previous(offset);
        }
        const int kind = Class(static_cast<unsigned char>(m_text[offset]));
        std::size_t start = offset;
        while (start > 0 && Class(static_cast<unsigned char>(m_text[start - 1])) == kind)
            --start;
        std::size_t end = offset;
        while (end < m_text.size() && Class(static_cast<unsigned char>(m_text[end])) == kind)
            ++end;
        Select(start, end);
    }

    void TextBuffer::SelectLineAt(std::size_t offset)
    {
        const int line = LineOf(offset);
        const std::size_t end = line + 1 < LineCount() ? LineStart(line + 1) : m_text.size();
        Select(LineStart(line), end);
    }

    void TextBuffer::Move(Motion motion, bool extend, int pageLines)
    {
        std::size_t target = m_caret;
        int keepColumn = -1;
        const auto vertical = [&](int delta)
        {
            const int column = m_preferredColumn >= 0 ? m_preferredColumn : ColumnOf(m_caret);
            const int line = LineOf(m_caret) + delta;
            keepColumn = column;
            if (line < 0)
                return std::size_t(0);
            if (line >= LineCount())
                return m_text.size();
            return OffsetAt(line, column);
        };
        switch (motion)
        {
        case Motion::Left:
            target = HasSelection() && !extend ? Selection().first : Previous(m_caret);
            break;
        case Motion::Right:
            target = HasSelection() && !extend ? Selection().second : Next(m_caret);
            break;
        case Motion::WordLeft:
        {
            std::size_t at = m_caret;
            while (at > 0 && IsBlank(m_text[at - 1]))
                --at;
            if (at > 0 && m_text[at - 1] == '\n')
            {
                target = at == m_caret ? at - 1 : at;
                break;
            }
            if (at > 0)
            {
                const int kind = Class(static_cast<unsigned char>(m_text[at - 1]));
                while (at > 0 && Class(static_cast<unsigned char>(m_text[at - 1])) == kind)
                    --at;
            }
            target = at;
            break;
        }
        case Motion::WordRight:
        {
            std::size_t at = m_caret;
            if (at < m_text.size() && m_text[at] == '\n')
            {
                target = at + 1;
                break;
            }
            while (at < m_text.size() && IsBlank(m_text[at]))
                ++at;
            if (at < m_text.size() && m_text[at] != '\n')
            {
                const int kind = Class(static_cast<unsigned char>(m_text[at]));
                while (at < m_text.size() && Class(static_cast<unsigned char>(m_text[at])) == kind)
                    ++at;
            }
            target = at;
            break;
        }
        case Motion::Home:
        {
            const int line = LineOf(m_caret);
            const std::size_t start = LineStart(line);
            const std::size_t first = start + Leading(Line(line));
            target = m_caret == first ? start : first;
            break;
        }
        case Motion::End:
            target = LineEnd(LineOf(m_caret));
            break;
        case Motion::Up:
            target = vertical(-1);
            break;
        case Motion::Down:
            target = vertical(1);
            break;
        case Motion::PageUp:
            target = vertical(-std::max(1, pageLines));
            break;
        case Motion::PageDown:
            target = vertical(std::max(1, pageLines));
            break;
        case Motion::DocumentStart:
            target = 0;
            break;
        case Motion::DocumentEnd:
            target = m_text.size();
            break;
        }
        m_caret = target;
        if (!extend)
            m_anchor = target;
        m_preferredColumn = keepColumn;
        m_coalesce = false;
    }

    void TextBuffer::GotoLine(int line)
    {
        SetCaret(LineStart(line), false);
    }

    TextBuffer::Group& TextBuffer::Begin(EditKind kind)
    {
        m_redo.clear();
        const bool join = m_coalesce && kind != EditKind::Other && !m_undo.empty() && m_undo.back().Kind == kind && m_undo.back().CaretAfter == m_caret &&
                          !HasSelection() && m_undo.back().Edits.size() < kGroupEdits;
        if (!join)
        {
            Group group;
            group.Kind = kind;
            group.CaretBefore = m_caret;
            group.AnchorBefore = m_anchor;
            m_undo.push_back(std::move(group));
            if (m_undo.size() > kUndoLimit)
                m_undo.erase(m_undo.begin());
        }
        return m_undo.back();
    }

    void TextBuffer::End(Group& group)
    {
        group.CaretAfter = m_caret;
        group.AnchorAfter = m_anchor;
        m_coalesce = group.Kind != EditKind::Other;
        m_preferredColumn = -1;
        if (group.Edits.empty() && &group == &m_undo.back())
            m_undo.pop_back();
    }

    void TextBuffer::Raw(std::size_t position, std::size_t count, std::string_view text)
    {
        m_text.replace(position, count, text);
        ++m_revision;
        m_changedFrom = std::min(m_changedFrom, position);
    }

    void TextBuffer::Apply(Group& group, std::size_t position, std::size_t count, std::string_view text)
    {
        position = std::min(position, m_text.size());
        count = std::min(count, m_text.size() - position);
        if (count == 0 && text.empty())
            return;
        std::string removed = m_text.substr(position, count);
        Raw(position, count, text);
        if (!group.Edits.empty())
        {
            Edit& last = group.Edits.back();
            if (count == 0 && position == last.Position + last.Inserted.size())
            {
                last.Inserted += text;
                return;
            }
            if (text.empty() && last.Inserted.empty() && position + count == last.Position)
            {
                last.Removed = removed + last.Removed;
                last.Position = position;
                return;
            }
            if (text.empty() && last.Inserted.empty() && position == last.Position)
            {
                last.Removed += removed;
                return;
            }
        }
        group.Edits.push_back({ position, std::move(removed), std::string(text) });
    }

    void TextBuffer::ReplaceSelection(Group& group, std::string_view text)
    {
        const auto [start, end] = Selection();
        Apply(group, start, end - start, text);
        m_caret = m_anchor = start + text.size();
    }

    std::pair<int, int> TextBuffer::SelectedLines() const
    {
        const auto [start, end] = Selection();
        const int first = LineOf(start);
        int last = LineOf(end);
        if (end > start && last > first && end == LineStart(last))
            --last;
        return { first, last };
    }

    int TextBuffer::IndentOf(int line) const
    {
        const std::string_view text = Line(line);
        return Columns(text.substr(0, Leading(text)), TabWidth);
    }

    void TextBuffer::SetIndent(Group& group, int line, int columns)
    {
        const std::size_t start = LineStart(line);
        const std::size_t old = Leading(Line(line));
        const std::size_t now = static_cast<std::size_t>(std::max(0, columns));
        Apply(group, start, old, std::string(now, ' '));
        const auto shift = [&](std::size_t& position)
        {
            if (position >= start + old)
                position = position - old + now;
            else if (position > start)
                position = std::min(position, start + now);
        };
        shift(m_caret);
        shift(m_anchor);
    }

    bool TextBuffer::OpensBlock(std::string_view text) const
    {
        int balance = 0;
        std::size_t i = 0;
        while (i < text.size())
        {
            const char c = text[i];
            if (c == '-' && i + 1 < text.size() && text[i + 1] == '-')
                break;
            if (c == '"' || c == '\'')
            {
                ++i;
                while (i < text.size() && text[i] != c)
                    i += text[i] == '\\' ? 2 : 1;
                ++i;
                continue;
            }
            if (c == '[' && i + 1 < text.size() && (text[i + 1] == '[' || text[i + 1] == '='))
            {
                const std::size_t close = text.find("]", i + 2);
                if (close == std::string_view::npos)
                    return true;
                i = close + 1;
                continue;
            }
            if (IsWordByte(static_cast<unsigned char>(c)) && !(c >= '0' && c <= '9'))
            {
                std::size_t end = i;
                while (end < text.size() && IsWordByte(static_cast<unsigned char>(text[end])))
                    ++end;
                const std::string_view word = text.substr(i, end - i);
                if (word == "function" || word == "then" || word == "do" || word == "repeat" || word == "else")
                    ++balance;
                else if (word == "end" || word == "until")
                    --balance;
                i = end;
                continue;
            }
            if (c == '{' || c == '(' || c == '[')
                ++balance;
            else if (c == '}' || c == ')' || c == ']')
                --balance;
            ++i;
        }
        return balance > 0;
    }

    void TextBuffer::DedentCloser(Group& group, int line)
    {
        const std::string_view text = Line(line);
        if (!StartsCloser(text.substr(Leading(text))))
            return;
        int previous = line - 1;
        while (previous >= 0 && Blank(Line(previous)))
            --previous;
        if (previous < 0)
            return;
        const int base = IndentOf(previous);
        const int target = OpensBlock(Line(previous)) ? base : std::max(0, base - TabWidth);
        if (target < IndentOf(line))
            SetIndent(group, line, target);
    }

    void TextBuffer::Type(std::string_view text)
    {
        if (text.empty())
            return;
        if (text == "\n" || text == "\r" || text == "\r\n")
        {
            Newline();
            return;
        }
        if (text == "\t")
        {
            Indent();
            return;
        }
        const auto first = static_cast<unsigned char>(text.front());
        const EditKind kind = text == " " ? EditKind::Space : IsWordByte(first) && text.find('\n') == std::string_view::npos ? EditKind::Word : EditKind::Other;
        Group& group = Begin(kind);
        ReplaceSelection(group, text);
        if (LuaIndent && text == " ")
            DedentCloser(group, LineOf(m_caret));
        End(group);
    }

    void TextBuffer::Paste(std::string_view text)
    {
        std::string clean;
        clean.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\r')
            {
                if (i + 1 >= text.size() || text[i + 1] != '\n')
                    clean += '\n';
                continue;
            }
            clean += text[i];
        }
        Group& group = Begin(EditKind::Other);
        ReplaceSelection(group, clean);
        End(group);
    }

    void TextBuffer::Newline()
    {
        Group& group = Begin(EditKind::Other);
        if (LuaIndent && !HasSelection())
            DedentCloser(group, LineOf(m_caret));
        const auto [start, end] = Selection();
        const int line = LineOf(start);
        const std::size_t lineStart = LineStart(line);
        const std::string_view lineText = Line(line);
        const std::string indent(lineText.substr(0, std::min(Leading(lineText), start - lineStart)));
        const std::string_view before = std::string_view(m_text).substr(lineStart, start - lineStart);
        const std::size_t afterEnd = LineEnd(LineOf(end));
        const std::string_view after = std::string_view(m_text).substr(end, afterEnd - end);
        const std::size_t afterBlank = Leading(after);
        const bool opens = LuaIndent && OpensBlock(before);
        std::string insert = "\n" + indent;
        if (opens)
            insert += std::string(static_cast<std::size_t>(TabWidth), ' ');
        std::size_t caret = start + insert.size();
        if (opens && StartsCloser(after.substr(afterBlank)))
            insert += "\n" + indent;
        Apply(group, start, end - start + afterBlank, insert);
        m_caret = m_anchor = caret;
        End(group);
    }

    void TextBuffer::Backspace(bool word)
    {
        Group& group = Begin(word ? EditKind::Other : EditKind::Backspace);
        if (HasSelection())
        {
            ReplaceSelection(group, "");
        }
        else if (m_caret > 0)
        {
            std::size_t from = Previous(m_caret);
            if (word)
            {
                const std::size_t caret = m_caret;
                Move(Motion::WordLeft, false);
                from = m_caret;
                m_caret = m_anchor = caret;
            }
            else
            {
                const std::size_t lineStart = LineStart(LineOf(m_caret));
                const std::string_view head = std::string_view(m_text).substr(lineStart, m_caret - lineStart);
                if (!head.empty() && head.find_first_not_of(' ') == std::string_view::npos)
                {
                    const std::size_t remove = head.size() % static_cast<std::size_t>(TabWidth) == 0 ? static_cast<std::size_t>(TabWidth) : head.size() % static_cast<std::size_t>(TabWidth);
                    from = m_caret - std::min(remove, head.size());
                }
            }
            Apply(group, from, m_caret - from, "");
            m_caret = m_anchor = from;
        }
        End(group);
    }

    void TextBuffer::Delete(bool word)
    {
        Group& group = Begin(word ? EditKind::Other : EditKind::Delete);
        if (HasSelection())
        {
            ReplaceSelection(group, "");
        }
        else if (m_caret < m_text.size())
        {
            std::size_t to = Next(m_caret);
            if (word)
            {
                const std::size_t caret = m_caret;
                Move(Motion::WordRight, false);
                to = m_caret;
                m_caret = m_anchor = caret;
            }
            Apply(group, m_caret, to - m_caret, "");
        }
        End(group);
    }

    void TextBuffer::Replace(std::size_t start, std::size_t end, std::string_view text)
    {
        start = std::min(start, m_text.size());
        end = std::clamp(end, start, m_text.size());
        Group& group = Begin(EditKind::Other);
        Apply(group, start, end - start, text);
        m_caret = m_anchor = start + text.size();
        End(group);
    }

    std::size_t TextBuffer::ReplaceAll(std::string_view needle, std::string_view replacement, SearchOptions options)
    {
        const std::vector<std::size_t> matches = FindAll(m_text, needle, options);
        if (matches.empty())
            return 0;
        // Back to front, so the earlier offsets stay valid; Undo reverts the
        // edits in reverse order, which is front to back again.
        Group& group = Begin(EditKind::Other);
        std::size_t caret = m_caret;
        for (auto at = matches.rbegin(); at != matches.rend(); ++at)
        {
            Apply(group, *at, needle.size(), replacement);
            if (*at < caret)
                caret = caret >= *at + needle.size() ? caret - needle.size() + replacement.size() : *at;
        }
        m_caret = m_anchor = std::min(caret, m_text.size());
        End(group);
        return matches.size();
    }

    void TextBuffer::Indent()
    {
        const auto [first, last] = SelectedLines();
        if (!HasSelection() || first == last)
        {
            Group& group = Begin(EditKind::Space);
            const int column = ColumnOf(Selection().first);
            ReplaceSelection(group, std::string(static_cast<std::size_t>(TabWidth - column % TabWidth), ' '));
            End(group);
            return;
        }
        Group& group = Begin(EditKind::Other);
        const std::string spaces(static_cast<std::size_t>(TabWidth), ' ');
        for (int line = last; line >= first; --line)
        {
            if (!Blank(Line(line)))
                Apply(group, LineStart(line), 0, spaces);
        }
        m_anchor = LineStart(first);
        m_caret = LineEnd(last);
        End(group);
    }

    void TextBuffer::Outdent()
    {
        const auto [first, last] = SelectedLines();
        const bool selection = HasSelection();
        Group& group = Begin(EditKind::Other);
        std::size_t caretShift = 0;
        for (int line = last; line >= first; --line)
        {
            const std::string_view text = Line(line);
            std::size_t remove = 0;
            if (!text.empty() && text.front() == '\t')
                remove = 1;
            else
                while (remove < text.size() && remove < static_cast<std::size_t>(TabWidth) && text[remove] == ' ')
                    ++remove;
            if (remove == 0)
                continue;
            const std::size_t start = LineStart(line);
            if (!selection && line == first)
                caretShift = std::min(remove, m_caret - start);
            Apply(group, start, remove, "");
        }
        if (selection)
        {
            m_anchor = LineStart(first);
            m_caret = LineEnd(last);
        }
        else
        {
            m_caret -= caretShift;
            m_anchor = m_caret;
        }
        End(group);
    }

    void TextBuffer::ToggleComment(std::string_view prefix)
    {
        if (prefix.empty())
            return;
        const auto [first, last] = SelectedLines();
        bool all = true;
        bool any = false;
        std::size_t indent = std::string::npos;
        for (int line = first; line <= last; ++line)
        {
            const std::string_view text = Line(line);
            if (Blank(text))
                continue;
            any = true;
            const std::size_t lead = Leading(text);
            indent = std::min(indent, lead);
            if (text.substr(lead).compare(0, prefix.size(), prefix) != 0)
                all = false;
        }
        if (!any)
            return;
        const bool selection = HasSelection();
        const int caretLine = LineOf(m_caret);
        const std::size_t caretColumn = m_caret - LineStart(caretLine);
        long long caretDelta = 0;
        Group& group = Begin(EditKind::Other);
        for (int line = last; line >= first; --line)
        {
            const std::string_view text = Line(line);
            if (Blank(text))
                continue;
            const std::size_t start = LineStart(line);
            if (all)
            {
                const std::size_t lead = Leading(text);
                std::size_t count = prefix.size();
                if (lead + count < text.size() && text[lead + count] == ' ')
                    ++count;
                if (line == caretLine && caretColumn > lead)
                    caretDelta = -static_cast<long long>(std::min(count, caretColumn - lead));
                Apply(group, start + lead, count, "");
            }
            else
            {
                const std::string insert = std::string(prefix) + " ";
                if (line == caretLine && caretColumn >= indent)
                    caretDelta = static_cast<long long>(insert.size());
                Apply(group, start + indent, 0, insert);
            }
        }
        if (selection)
        {
            m_anchor = LineStart(first);
            m_caret = LineEnd(last);
        }
        else
        {
            m_caret = static_cast<std::size_t>(static_cast<long long>(m_caret) + caretDelta);
            m_anchor = m_caret;
        }
        End(group);
    }

    void TextBuffer::DuplicateLines()
    {
        const auto [first, last] = SelectedLines();
        const std::size_t start = LineStart(first);
        const std::size_t end = LineEnd(last);
        const std::string block = m_text.substr(start, end - start);
        Group& group = Begin(EditKind::Other);
        Apply(group, end, 0, "\n" + block);
        m_caret += block.size() + 1;
        m_anchor += block.size() + 1;
        End(group);
    }

    void TextBuffer::MoveLines(int direction)
    {
        const auto [first, last] = SelectedLines();
        if ((direction < 0 && first == 0) || (direction > 0 && last + 1 >= LineCount()) || direction == 0)
            return;
        const std::size_t start = LineStart(first);
        const std::size_t end = LineEnd(last);
        const std::string block = m_text.substr(start, end - start);
        Group& group = Begin(EditKind::Other);
        if (direction < 0)
        {
            const std::size_t otherStart = LineStart(first - 1);
            const std::string other = m_text.substr(otherStart, start - 1 - otherStart);
            Apply(group, otherStart, end - otherStart, block + "\n" + other);
            m_caret -= other.size() + 1;
            m_anchor -= other.size() + 1;
        }
        else
        {
            const std::size_t otherEnd = LineEnd(last + 1);
            const std::string other = m_text.substr(end + 1, otherEnd - end - 1);
            Apply(group, start, otherEnd - start, other + "\n" + block);
            m_caret += other.size() + 1;
            m_anchor += other.size() + 1;
        }
        End(group);
    }

    void TextBuffer::DeleteLines()
    {
        const auto [first, last] = SelectedLines();
        std::size_t start = LineStart(first);
        const std::size_t end = last + 1 < LineCount() ? LineStart(last + 1) : m_text.size();
        const bool tail = end == m_text.size() && first > 0;
        if (tail)
            start = LineEnd(first - 1);
        Group& group = Begin(EditKind::Other);
        Apply(group, start, end - start, "");
        m_caret = m_anchor = tail ? LineStart(LineOf(start)) : start;
        End(group);
    }

    bool TextBuffer::Undo()
    {
        if (m_undo.empty())
            return false;
        Group group = std::move(m_undo.back());
        m_undo.pop_back();
        for (auto edit = group.Edits.rbegin(); edit != group.Edits.rend(); ++edit)
            Raw(edit->Position, edit->Inserted.size(), edit->Removed);
        m_caret = std::min(group.CaretBefore, m_text.size());
        m_anchor = std::min(group.AnchorBefore, m_text.size());
        m_redo.push_back(std::move(group));
        m_coalesce = false;
        m_preferredColumn = -1;
        return true;
    }

    bool TextBuffer::Redo()
    {
        if (m_redo.empty())
            return false;
        Group group = std::move(m_redo.back());
        m_redo.pop_back();
        for (const Edit& edit : group.Edits)
            Raw(edit.Position, edit.Removed.size(), edit.Inserted);
        m_caret = std::min(group.CaretAfter, m_text.size());
        m_anchor = std::min(group.AnchorAfter, m_text.size());
        m_undo.push_back(std::move(group));
        m_coalesce = false;
        m_preferredColumn = -1;
        return true;
    }
}
