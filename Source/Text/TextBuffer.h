#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Text
{
    enum class Motion
    {
        Left,
        Right,
        WordLeft,
        WordRight,
        Home,
        End,
        Up,
        Down,
        PageUp,
        PageDown,
        DocumentStart,
        DocumentEnd
    };

    struct SearchOptions
    {
        bool MatchCase = false;
        bool WholeWord = false;
    };

    // Start offsets of every match of `needle`, in order and without overlaps.
    // Case folding is ASCII only; a whole word has no word byte on either side.
    std::vector<std::size_t> FindAll(std::string_view text, std::string_view needle, SearchOptions options);
    // Index of the first match at or after `from` (or the last one before it,
    // going back), wrapping around the end; -1 without matches.
    int NextMatch(const std::vector<std::size_t>& matches, std::size_t from, bool forward);

    class TextBuffer
    {
    public:
        TextBuffer() = default;
        explicit TextBuffer(std::string text);

        void SetText(std::string text);
        const std::string& Text() const { return m_text; }
        std::size_t Size() const { return m_text.size(); }
        std::uint64_t Revision() const { return m_revision; }
        std::size_t TakeChanges();

        int LineCount() const;
        int LineOf(std::size_t offset) const;
        std::size_t LineStart(int line) const;
        std::size_t LineEnd(int line) const;
        std::string_view Line(int line) const;
        int ColumnOf(std::size_t offset) const;
        std::size_t OffsetAt(int line, int column) const;
        int LongestLine() const;
        std::size_t Next(std::size_t offset) const;
        std::size_t Previous(std::size_t offset) const;

        std::size_t Caret() const { return m_caret; }
        std::size_t Anchor() const { return m_anchor; }
        bool HasSelection() const { return m_caret != m_anchor; }
        std::pair<std::size_t, std::size_t> Selection() const;
        std::string SelectedText() const;
        void SetCaret(std::size_t offset, bool extend);
        void Select(std::size_t anchor, std::size_t caret);
        void SelectAll();
        void SelectWordAt(std::size_t offset);
        void SelectLineAt(std::size_t offset);
        void Move(Motion motion, bool extend, int pageLines = 20);
        void GotoLine(int line);

        void Type(std::string_view text);
        void Paste(std::string_view text);
        void Newline();
        void Backspace(bool word);
        void Delete(bool word);
        void Replace(std::size_t start, std::size_t end, std::string_view text);
        // Replaces every match in one undo step and returns how many there were.
        std::size_t ReplaceAll(std::string_view needle, std::string_view replacement, SearchOptions options);
        void Indent();
        void Outdent();
        void ToggleComment(std::string_view prefix);
        void DuplicateLines();
        void MoveLines(int direction);
        void DeleteLines();

        bool Undo();
        bool Redo();
        bool CanUndo() const { return !m_undo.empty(); }
        bool CanRedo() const { return !m_redo.empty(); }
        void BreakUndoGroup() { m_coalesce = false; }
        void ClearHistory();

        int TabWidth = 4;
        bool LuaIndent = true;

    private:
        enum class EditKind
        {
            Word,
            Space,
            Backspace,
            Delete,
            Other
        };

        struct Edit
        {
            std::size_t Position = 0;
            std::string Removed;
            std::string Inserted;
        };

        struct Group
        {
            EditKind Kind = EditKind::Other;
            std::vector<Edit> Edits;
            std::size_t CaretBefore = 0;
            std::size_t AnchorBefore = 0;
            std::size_t CaretAfter = 0;
            std::size_t AnchorAfter = 0;
        };

        Group& Begin(EditKind kind);
        void End(Group& group);
        void Apply(Group& group, std::size_t position, std::size_t count, std::string_view text);
        void Raw(std::size_t position, std::size_t count, std::string_view text);
        void ReplaceSelection(Group& group, std::string_view text);
        void Lines() const;
        std::pair<int, int> SelectedLines() const;
        int IndentOf(int line) const;
        void SetIndent(Group& group, int line, int columns);
        void DedentCloser(Group& group, int line);
        bool OpensBlock(std::string_view text) const;

        std::string m_text;
        std::size_t m_caret = 0;
        std::size_t m_anchor = 0;
        int m_preferredColumn = -1;
        std::uint64_t m_revision = 1;
        std::size_t m_changedFrom = 0;
        mutable std::vector<std::size_t> m_lineStarts;
        mutable std::uint64_t m_linesRevision = 0;
        mutable int m_longest = 0;
        mutable std::uint64_t m_longestRevision = 0;
        std::vector<Group> m_undo;
        std::vector<Group> m_redo;
        bool m_coalesce = false;
    };

    bool IsWordByte(unsigned char c);
    int Columns(std::string_view text, int tabWidth, int start = 0);
}
