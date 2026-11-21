#include "UI/EditorView.h"

#include "Generated/IconsFontAwesome5.h"
#include "Text/Highlight.h"
#include "Text/LuaContext.h"
#include "Lua/DocRecord.h"
#include "UI/EditorShared.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Manifold::UI
{
    namespace
    {
        struct Metrics
        {
            ImFont* Font = nullptr;
            float FontSize = 0.0f;
            float Row = 0.0f;
            float Char = 0.0f;
            float Gutter = 0.0f;
            float PadX = 0.0f;
            float PadY = 0.0f;
        };

        std::string Utf8(unsigned int c)
        {
            std::string out;
            if (c < 0x80)
            {
                out += static_cast<char>(c);
            }
            else if (c < 0x800)
            {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else if (c < 0x10000)
            {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            return out;
        }

        const char* CommentPrefix(Language language)
        {
            return language == Language::AutoAssembler || language == Language::Template ? "//" : "--";
        }

        void EnsureStates(EditorViewState& view, Language language, int upTo)
        {
            const Text::TextBuffer& buffer = view.Buffer;
            const std::size_t changed = view.Buffer.TakeChanges();
            if (view.StatesLanguage != language || view.States.empty())
            {
                view.States.assign(1, Text::InitialState(language));
                view.StatesValid = 1;
                view.StatesLanguage = language;
            }
            else if (changed != std::string::npos)
            {
                view.StatesValid = std::min(view.StatesValid, buffer.LineOf(changed) + 1);
            }
            const int lines = buffer.LineCount();
            view.States.resize(static_cast<std::size_t>(lines));
            view.StatesValid = std::clamp(view.StatesValid, 1, lines);
            upTo = std::min(upTo, lines - 1);
            std::vector<Text::TokenSpan> scratch;
            while (view.StatesValid <= upTo)
            {
                const int line = view.StatesValid - 1;
                Text::LineState state = view.States[static_cast<std::size_t>(line)];
                scratch.clear();
                if (language != Language::Plain)
                    Text::HighlightLine(language, buffer.Line(line), buffer.LineStart(line), state, scratch);
                view.States[static_cast<std::size_t>(view.StatesValid)] = state;
                ++view.StatesValid;
            }
        }

        void DrawLine(ImDrawList* draw, const Metrics& metrics, float x, float y, std::string_view line, std::size_t lineStart, const std::vector<Text::TokenSpan>& spans,
            int tabWidth, ImU32 plain)
        {
            int column = 0;
            std::size_t i = 0;
            std::size_t span = 0;
            while (i < line.size())
            {
                const std::size_t at = lineStart + i;
                while (span < spans.size() && spans[span].End <= at)
                    ++span;
                ImU32 color = plain;
                std::size_t end = line.size();
                if (span < spans.size())
                {
                    if (spans[span].Start <= at)
                    {
                        color = EditorShared::TokenColor(spans[span].Kind);
                        end = std::min(end, spans[span].End - lineStart);
                    }
                    else
                    {
                        end = std::min(end, spans[span].Start - lineStart);
                    }
                }
                const std::size_t tab = line.find('\t', i);
                if (tab != std::string_view::npos && tab < end)
                    end = tab;
                if (end > i)
                {
                    draw->AddText(metrics.Font, metrics.FontSize, ImVec2(x + static_cast<float>(column) * metrics.Char, y), color, line.data() + i, line.data() + end);
                    column = Text::Columns(line.substr(i, end - i), tabWidth, column);
                    i = end;
                }
                if (i < line.size() && line[i] == '\t')
                {
                    column += tabWidth - column % tabWidth;
                    ++i;
                }
            }
        }

        std::size_t OffsetAtPoint(const Text::TextBuffer& buffer, const Metrics& metrics, ImVec2 textOrigin, ImVec2 point)
        {
            const int line = std::clamp(static_cast<int>(std::floor((point.y - textOrigin.y) / metrics.Row)), 0, buffer.LineCount() - 1);
            const int column = std::max(0, static_cast<int>(std::floor((point.x - textOrigin.x) / metrics.Char + 0.5f)));
            return buffer.OffsetAt(line, column);
        }

        std::size_t WordStartAt(const Text::TextBuffer& buffer, std::size_t offset)
        {
            Text::TextBuffer probe = buffer;
            probe.SelectWordAt(offset);
            return probe.Selection().first;
        }

        std::size_t WordEndAt(const Text::TextBuffer& buffer, std::size_t offset)
        {
            Text::TextBuffer probe = buffer;
            probe.SelectWordAt(offset);
            return probe.Selection().second;
        }

        bool Key(ImGuiKeyChord chord, bool repeat = true)
        {
            return ImGui::Shortcut(chord, repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None);
        }

        void CopyText(const std::string& text)
        {
            ImGui::SetClipboardText(text.c_str());
        }

        std::string LineCopy(const Text::TextBuffer& buffer)
        {
            const int line = buffer.LineOf(buffer.Caret());
            return std::string(buffer.Line(line)) + "\n";
        }

        Text::SearchOptions FindOptions(const EditorViewState& view)
        {
            return { view.MatchCase, view.WholeWord };
        }

        void RefreshMatches(EditorViewState& view)
        {
            std::string key = std::string(view.MatchCase ? "C" : "c") + (view.WholeWord ? "W" : "w") + view.FindText;
            if (view.MatchesRevision == view.Buffer.Revision() && view.MatchesKey == key)
                return;
            view.Matches = Text::FindAll(view.Buffer.Text(), view.FindText, FindOptions(view));
            view.MatchesRevision = view.Buffer.Revision();
            view.MatchesKey = std::move(key);
        }

        int CurrentMatch(EditorViewState& view)
        {
            RefreshMatches(view);
            const auto [start, end] = view.Buffer.Selection();
            if (end - start != view.FindText.size() || view.FindText.empty())
                return -1;
            const auto it = std::lower_bound(view.Matches.begin(), view.Matches.end(), start);
            return it != view.Matches.end() && *it == start ? static_cast<int>(it - view.Matches.begin()) : -1;
        }

        void SelectMatch(EditorViewState& view, int index)
        {
            const std::size_t at = view.Matches[static_cast<std::size_t>(index)];
            view.Buffer.Select(at, at + view.FindText.size());
            view.ScrollToCaret = true;
        }

        bool StepMatch(EditorViewState& view, bool forward)
        {
            RefreshMatches(view);
            view.FindNote.clear();
            const auto [start, end] = view.Buffer.Selection();
            const std::size_t from = forward ? (view.Buffer.HasSelection() ? start + 1 : view.Buffer.Caret()) : start;
            const int index = Text::NextMatch(view.Matches, from, forward);
            if (index < 0)
                return false;
            SelectMatch(view, index);
            return true;
        }

        // A single-line selection is what the user wants to find; a longer one
        // or a selection over lines keeps the last search text.
        void SeedFind(EditorViewState& view)
        {
            const std::string selected = view.Buffer.SelectedText();
            if (!selected.empty() && selected.size() <= 200 && selected.find('\n') == std::string::npos)
                view.FindText = selected;
        }

        void OpenFind(EditorViewState& view, bool replace)
        {
            SeedFind(view);
            view.FindOpen = true;
            view.ReplaceOpen = replace;
            view.FindFocus = replace && !view.FindText.empty() ? 0 : 2;
            view.ReplaceFocus = replace && !view.FindText.empty() ? 2 : 0;
            view.FindOrigin = view.Buffer.Selection().first;
            view.FindNote.clear();
        }

        void FindStep(EditorViewState& view, bool forward)
        {
            if (view.FindText.empty())
                SeedFind(view);
            if (view.FindText.empty())
            {
                OpenFind(view, false);
                return;
            }
            if (!StepMatch(view, forward))
                view.FindNote = "No results";
        }

        bool ReplaceCurrent(EditorViewState& view)
        {
            const int current = CurrentMatch(view);
            if (current < 0)
            {
                StepMatch(view, true);
                return false;
            }
            const std::size_t at = view.Matches[static_cast<std::size_t>(current)];
            view.Buffer.Replace(at, at + view.FindText.size(), view.ReplaceText);
            RefreshMatches(view);
            const int next = Text::NextMatch(view.Matches, view.Buffer.Caret(), true);
            if (next >= 0)
                SelectMatch(view, next);
            view.ScrollToCaret = true;
            return true;
        }

        void ActivateWhenAsked(int& frames, const char* id)
        {
            if (frames <= 0)
                return;
            const ImGuiID item = ImGui::GetID(id);
            if (ImGui::GetActiveID() != item)
                ImGui::ActivateItemByID(item);
            --frames;
        }

        void BarToggle(const char* label, const char* tooltip, bool& value, float side, bool& changed)
        {
            // The pop must follow the push of this frame, not the value the
            // click just flipped, or ImGui reports a style stack mismatch.
            const bool lit = value;
            if (lit)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.30f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.42f));
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AccentHovered);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (ImGui::Button(label, ImVec2(side, side)))
            {
                value = !value;
                changed = true;
            }
            ImGui::PopStyleVar();
            if (lit)
                ImGui::PopStyleColor(3);
            ItemTooltip(tooltip);
        }

        // The find bar sits above the text in its own child window, so its
        // fields keep the focus apart from the editor and Escape closes only it.
        void DrawFindBar(CodeEditorState& state, float width, bool editable, bool& edited)
        {
            EditorViewState& view = state.View;
            const ImGuiStyle& style = ImGui::GetStyle();
            const bool replace = view.ReplaceOpen && editable;
            const float pad = std::round(Px(4.0f));
            const int rows = replace ? 2 : 1;
            const float side = ImGui::GetFrameHeight();
            const float height = static_cast<float>(rows) * side + static_cast<float>(rows - 1) * style.ItemSpacing.y + pad * 2.0f;
            if (view.FindFocus > 0 || view.ReplaceFocus > 0)
                ImGui::SetNextWindowFocus();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(4.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::WithAlpha(ManifoldTheme::Sidebar, 0.75f));
            const bool open = ImGui::BeginChild("##findbar", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            if (!open)
            {
                ImGui::EndChild();
                return;
            }
            const bool focused = ImGui::IsWindowFocused();
            if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                // Checked before the fields are drawn: an active field would
                // otherwise take the Escape and revert its text.
                view.FindOpen = false;
                state.Focus = true;
                KeepEscape();
                ImGui::EndChild();
                return;
            }
            bool step = false;
            bool forward = true;
            if (focused)
            {
                if (Pressed(Command::EditorFind))
                    view.FindFocus = 2;
                if (editable && Pressed(Command::EditorReplace))
                {
                    view.ReplaceOpen = true;
                    view.ReplaceFocus = 2;
                }
                if (PressedWhileTyping(Command::EditorFindNext))
                    step = true;
                if (PressedWhileTyping(Command::EditorFindPrevious))
                {
                    step = true;
                    forward = false;
                }
            }

            RefreshMatches(view);
            const int current = CurrentMatch(view);
            std::string count;
            if (!view.FindNote.empty())
                count = view.FindNote;
            else if (view.FindText.empty())
                count.clear();
            else if (view.Matches.empty())
                count = "No results";
            else if (current >= 0)
                count = std::to_string(current + 1) + " of " + std::to_string(view.Matches.size());
            else
                count = std::to_string(view.Matches.size()) + (view.Matches.size() == 1 ? " result" : " results");
            const float countWidth = std::max(ImGui::CalcTextSize(count.c_str()).x, ImGui::CalcTextSize("0000 of 0000").x);
            const int buttons = editable ? 6 : 5;
            const float right = static_cast<float>(buttons) * side + countWidth + style.ItemSpacing.x * static_cast<float>(buttons + 1);
            const float inputWidth = std::max(Px(80.0f), ImGui::GetContentRegionAvail().x - right);

            ActivateWhenAsked(view.FindFocus, "##findtext");
            const std::string before = view.FindText;
            ImGui::SetNextItemWidth(inputWidth);
            ImGuiIO& io = ImGui::GetIO();
            const bool keepActive = io.ConfigInputTextEnterKeepActive;
            io.ConfigInputTextEnterKeepActive = true;
            const bool enter = InputTextWithHint("##findtext", "Find", view.FindText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            io.ConfigInputTextEnterKeepActive = keepActive;
            bool options = false;
            ImGui::SameLine();
            BarToggle("Aa##case", "Match Case", view.MatchCase, side, options);
            ImGui::SameLine();
            BarToggle("W##word", "Whole Word", view.WholeWord, side, options);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            const float countX = ImGui::GetCursorPosX();
            ImGui::TextColored(count == "No results" ? ManifoldTheme::Error : ManifoldTheme::Muted, "%s", count.c_str());
            ImGui::SameLine(countX + countWidth + style.ItemSpacing.x);
            if (IconButton(ICON_FA_ARROW_UP "##previous", WithKeys("Previous Match", Command::EditorFindPrevious).c_str()))
            {
                step = true;
                forward = false;
            }
            ImGui::SameLine();
            if (IconButton(ICON_FA_ARROW_DOWN "##next", WithKeys("Next Match", Command::EditorFindNext).c_str()))
                step = true;
            if (editable)
            {
                ImGui::SameLine();
                bool shown = view.ReplaceOpen;
                bool toggled = false;
                BarToggle(ICON_FA_EXCHANGE_ALT "##showreplace", WithKeys("Replace", Command::EditorReplace).c_str(), shown, side, toggled);
                if (toggled)
                {
                    view.ReplaceOpen = shown;
                    if (shown)
                        view.ReplaceFocus = 2;
                }
            }
            ImGui::SameLine();
            if (IconButton(ICON_FA_TIMES "##closefind", "Close (Esc)"))
            {
                view.FindOpen = false;
                state.Focus = true;
            }

            if (view.FindText != before || options)
            {
                view.FindNote.clear();
                RefreshMatches(view);
                const int index = Text::NextMatch(view.Matches, view.FindOrigin, true);
                if (index >= 0)
                    SelectMatch(view, index);
            }
            if (enter)
            {
                step = true;
                forward = !io.KeyShift;
            }
            if (step)
                FindStep(view, forward);

            if (replace)
            {
                ActivateWhenAsked(view.ReplaceFocus, "##replacetext");
                ImGui::SetNextItemWidth(inputWidth);
                io.ConfigInputTextEnterKeepActive = true;
                const bool replaceEnter = InputTextWithHint("##replacetext", "Replace", view.ReplaceText, ImGuiInputTextFlags_EnterReturnsTrue);
                io.ConfigInputTextEnterKeepActive = keepActive;
                ImGui::SameLine();
                if ((ImGui::Button("Replace") || replaceEnter) && !view.FindText.empty())
                {
                    view.FindNote.clear();
                    edited |= ReplaceCurrent(view);
                }
                ItemTooltip("Replaces the selected match and selects the next one.");
                ImGui::SameLine();
                if (ImGui::Button("Replace All") && !view.FindText.empty())
                {
                    const std::size_t replaced = view.Buffer.ReplaceAll(view.FindText, view.ReplaceText, FindOptions(view));
                    view.FindNote = replaced == 0 ? "No results" : "Replaced " + std::to_string(replaced);
                    view.ScrollToCaret = true;
                    edited |= replaced > 0;
                }
                ItemTooltip("Replaces every match in one step; Undo takes it back.");
            }
            else if (view.ReplaceFocus > 0)
            {
                view.ReplaceFocus = 0;
            }
            ImGui::EndChild();
        }
    }

    bool EditorView(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state)
    {
        EditorViewState& view = state.View;
        Text::TextBuffer& buffer = view.Buffer;
        buffer.LuaIndent = language == Language::Lua;
        if (!view.Synced || buffer.Size() != text.size() || buffer.Text() != text)
        {
            buffer.SetText(text);
            if (state.Console)
                buffer.SetCaret(text.size(), false);
            view.Synced = true;
            view.ScrollToCaret = state.Console;
        }

        if (state.OpenFind)
        {
            state.OpenFind = false;
            if (!state.Console)
                OpenFind(view, false);
        }

        ImGuiIO& io = ImGui::GetIO();
        ImGui::PushID(id);
        ImGui::PushFont(Fonts::Mono(), 0.0f);
        Metrics metrics;
        metrics.Font = ImGui::GetFont();
        metrics.FontSize = ImGui::GetFontSize();
        metrics.Row = std::round(ImGui::GetTextLineHeight() + Px(3.0f));
        metrics.Char = metrics.Font->CalcTextSizeA(metrics.FontSize, FLT_MAX, 0.0f, " ").x;
        metrics.PadX = std::round(Px(6.0f));
        metrics.PadY = std::round(Px(4.0f));
        const int lines = buffer.LineCount();
        state.LineCount = lines;
        const int digits = std::max(3, static_cast<int>(std::to_string(lines).size()));
        metrics.Gutter = state.Console ? std::round(metrics.Char * 2.0f + Px(8.0f)) : std::round(metrics.Char * static_cast<float>(digits) + Px(16.0f));

        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (size.x <= 0.0f)
            size.x = available.x + size.x;
        const float contentWidth = metrics.Gutter + metrics.PadX * 2.0f + static_cast<float>(buffer.LongestLine() + 2) * metrics.Char;
        const float contentHeight = metrics.PadY * 2.0f + static_cast<float>(lines) * metrics.Row;
        if (state.Console)
        {
            const int rows = std::clamp(lines, 1, std::max(1, state.ConsoleMaxLines));
            size.y = static_cast<float>(rows) * metrics.Row + metrics.PadY * 2.0f;
            if (contentWidth > size.x)
                size.y += ImGui::GetStyle().ScrollbarSize;
        }
        else if (size.y <= 0.0f)
        {
            size.y = available.y + size.y;
        }
        size.x = std::max(size.x, metrics.Gutter + Px(60.0f));
        size.y = std::max(size.y, state.Console ? metrics.Row + metrics.PadY * 2.0f : metrics.Row * 3.0f);

        bool barEdited = false;
        if (view.FindOpen && !state.Console)
        {
            ImGui::PopFont();
            const float top = ImGui::GetCursorPosY();
            DrawFindBar(state, size.x, !state.ReadOnly, barEdited);
            size.y = std::max(size.y - (ImGui::GetCursorPosY() - top), metrics.Row * 3.0f);
            ImGui::PushFont(Fonts::Mono(), 0.0f);
        }

        if (state.Focus)
        {
            ImGui::SetNextWindowFocus();
            view.ScrollToCaret = true;
            state.Focus = false;
        }
        ImGui::SetNextWindowContentSize(ImVec2(contentWidth, contentHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(4.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::WithAlpha(ManifoldTheme::Sidebar, 0.75f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove;
        if (state.Console && lines <= state.ConsoleMaxLines)
            flags |= ImGuiWindowFlags_NoScrollWithMouse;
        const bool visible = ImGui::BeginChild("##view", size, ImGuiChildFlags_None, flags);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        bool edited = barEdited;
        bool pasted = false;
        bool moved = false;
        bool forceComplete = false;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const bool focused = ImGui::IsWindowFocused();
        CodeAssistState& assist = state.Assisting;
        const bool assistEnabled = (language == Language::Lua || language == Language::AutoAssembler || language == Language::Template) && state.Assist && !state.ReadOnly;
        const bool editable = !state.ReadOnly;
        ImVec2 textOrigin(window->Pos.x + metrics.Gutter + metrics.PadX - window->Scroll.x, window->Pos.y + metrics.PadY - window->Scroll.y);

        if (!state.Insert.empty() && editable)
        {
            pasted = state.Insert.size() > 1;
            buffer.Paste(state.Insert);
            state.Insert.clear();
            edited = true;
            view.ScrollToCaret = true;
        }
        if (state.GotoLine > 0)
        {
            buffer.GotoLine(state.GotoLine - 1);
            state.GotoLine = 0;
            view.ScrollToCaret = true;
            moved = true;
        }

        const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(window->InnerRect.Min, window->InnerRect.Max, false);
        const bool overText = hovered && io.MousePos.x >= window->Pos.x + metrics.Gutter;
        if (overText)
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            ImGui::FocusWindow(window);
            const std::size_t at = OffsetAtPoint(buffer, metrics, textOrigin, io.MousePos);
            const int clicks = io.MouseClickedCount[ImGuiMouseButton_Left];
            if (!overText)
            {
                buffer.SelectLineAt(at);
                view.ClickMode = 3;
            }
            else if (clicks >= 3)
            {
                buffer.SelectLineAt(at);
                view.ClickMode = 3;
            }
            else if (clicks == 2)
            {
                buffer.SelectWordAt(at);
                view.ClickMode = 2;
            }
            else
            {
                buffer.SetCaret(at, io.KeyShift);
                view.ClickMode = 1;
            }
            view.DragStart = buffer.Selection().first;
            view.DragEnd = buffer.Selection().second;
            view.Dragging = true;
            view.LastInput = ImGui::GetTime();
            moved = true;
            assist.Open = false;
        }
        if (view.Dragging)
        {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                view.Dragging = false;
            }
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) || view.ClickMode > 1)
            {
                const std::size_t at = OffsetAtPoint(buffer, metrics, textOrigin, io.MousePos);
                if (view.ClickMode == 1)
                {
                    buffer.SetCaret(at, true);
                }
                else if (view.ClickMode == 2)
                {
                    if (at < view.DragStart)
                        buffer.Select(view.DragEnd, WordStartAt(buffer, at));
                    else
                        buffer.Select(view.DragStart, std::max(view.DragEnd, WordEndAt(buffer, at)));
                }
                else
                {
                    const int line = buffer.LineOf(at);
                    const std::size_t lineEnd = line + 1 < buffer.LineCount() ? buffer.LineStart(line + 1) : buffer.Size();
                    if (at < view.DragStart)
                        buffer.Select(view.DragEnd, buffer.LineStart(line));
                    else
                        buffer.Select(view.DragStart, std::max(view.DragEnd, lineEnd));
                }
                const ImRect inner = window->InnerRect;
                const float step = metrics.Row;
                if (io.MousePos.y < inner.Min.y)
                    ImGui::SetScrollY(std::max(0.0f, window->Scroll.y - step * std::min(4.0f, 1.0f + (inner.Min.y - io.MousePos.y) / step)));
                else if (io.MousePos.y > inner.Max.y)
                    ImGui::SetScrollY(window->Scroll.y + step * std::min(4.0f, 1.0f + (io.MousePos.y - inner.Max.y) / step));
                if (io.MousePos.x < inner.Min.x + metrics.Gutter)
                    ImGui::SetScrollX(std::max(0.0f, window->Scroll.x - metrics.Char * 2.0f));
                else if (io.MousePos.x > inner.Max.x)
                    ImGui::SetScrollX(window->Scroll.x + metrics.Char * 2.0f);
                moved = true;
            }
        }

        if (focused && !view.GotoOpen)
        {
            ImGuiContext& g = *GImGui;
            g.WantTextInputNextFrame = 1;
            const int page = std::max(1, static_cast<int>(window->InnerRect.GetHeight() / metrics.Row) - 1);
            bool assistOpen = assistEnabled && assist.Open && !assist.Items.empty();
            if (assistOpen && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            {
                const std::size_t caret = buffer.Caret();
                const std::size_t start = std::min(assist.WordStart, caret);
                const std::string typed = buffer.Text().substr(start, caret - start);
                if (assist.Items[static_cast<std::size_t>(std::clamp(assist.Selected, 0, static_cast<int>(assist.Items.size()) - 1))].Name == typed)
                {
                    assist.Open = false;
                    assistOpen = false;
                }
            }
            const auto motion = [&](Text::Motion where, bool extend, int pageLines = 20)
            {
                buffer.Move(where, extend, pageLines);
                moved = true;
                view.ScrollToCaret = true;
            };

            if (assistOpen)
            {
                const int count = static_cast<int>(assist.Items.size());
                if (Key(ImGuiKey_DownArrow))
                    assist.Selected = (assist.Selected + 1) % count;
                if (Key(ImGuiKey_UpArrow))
                    assist.Selected = (assist.Selected + count - 1) % count;
                if (Key(ImGuiKey_PageDown))
                    assist.Selected = std::min(count - 1, assist.Selected + 10);
                if (Key(ImGuiKey_PageUp))
                    assist.Selected = std::max(0, assist.Selected - 10);
                if (Key(ImGuiKey_Tab, false) || Key(ImGuiKey_Enter, false) || Key(ImGuiKey_KeypadEnter, false))
                {
                    const std::size_t caret = buffer.Caret();
                    const std::size_t start = std::min(assist.WordStart, caret);
                    buffer.Replace(start, caret, assist.Items[static_cast<std::size_t>(std::clamp(assist.Selected, 0, count - 1))].Name);
                    assist.Open = false;
                    assist.JustAccepted = true;
                    edited = true;
                    view.ScrollToCaret = true;
                }
            }
            if (Key(ImGuiKey_Escape, false))
            {
                if (!assist.Open && !assist.Hints.empty())
                {
                    assist.DismissedParen = assist.HintParen;
                }
                else if (!assist.Open && view.FindOpen)
                {
                    view.FindOpen = false;
                    KeepEscape();
                }
                else if (!assist.Open && buffer.HasSelection())
                    buffer.SetCaret(buffer.Caret(), false);
                assist.Open = false;
            }

            if (Key(ImGuiKey_LeftArrow))
                motion(Text::Motion::Left, false);
            if (Key(ImGuiMod_Shift | ImGuiKey_LeftArrow))
                motion(Text::Motion::Left, true);
            if (Key(ImGuiMod_Ctrl | ImGuiKey_LeftArrow))
                motion(Text::Motion::WordLeft, false);
            if (Key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_LeftArrow))
                motion(Text::Motion::WordLeft, true);
            if (Key(ImGuiKey_RightArrow))
                motion(Text::Motion::Right, false);
            if (Key(ImGuiMod_Shift | ImGuiKey_RightArrow))
                motion(Text::Motion::Right, true);
            if (Key(ImGuiMod_Ctrl | ImGuiKey_RightArrow))
                motion(Text::Motion::WordRight, false);
            if (Key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_RightArrow))
                motion(Text::Motion::WordRight, true);
            if (!assistOpen && Key(ImGuiKey_UpArrow))
            {
                if (state.Console && buffer.LineOf(buffer.Caret()) == 0 && !buffer.HasSelection())
                    state.HistoryStep = -1;
                else
                    motion(Text::Motion::Up, false);
            }
            if (!assistOpen && Key(ImGuiKey_DownArrow))
            {
                if (state.Console && buffer.LineOf(buffer.Caret()) == buffer.LineCount() - 1 && !buffer.HasSelection())
                    state.HistoryStep = 1;
                else
                    motion(Text::Motion::Down, false);
            }
            if (Key(ImGuiMod_Shift | ImGuiKey_UpArrow))
                motion(Text::Motion::Up, true);
            if (Key(ImGuiMod_Shift | ImGuiKey_DownArrow))
                motion(Text::Motion::Down, true);
            if (Key(ImGuiMod_Ctrl | ImGuiKey_UpArrow))
                ImGui::SetScrollY(std::max(0.0f, window->Scroll.y - metrics.Row));
            if (Key(ImGuiMod_Ctrl | ImGuiKey_DownArrow))
                ImGui::SetScrollY(window->Scroll.y + metrics.Row);
            if (Key(ImGuiKey_Home))
                motion(Text::Motion::Home, false);
            if (Key(ImGuiMod_Shift | ImGuiKey_Home))
                motion(Text::Motion::Home, true);
            if (Key(ImGuiKey_End))
                motion(Text::Motion::End, false);
            if (Key(ImGuiMod_Shift | ImGuiKey_End))
                motion(Text::Motion::End, true);
            if (Key(ImGuiMod_Ctrl | ImGuiKey_Home))
                motion(Text::Motion::DocumentStart, false);
            if (Key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Home))
                motion(Text::Motion::DocumentStart, true);
            if (Key(ImGuiMod_Ctrl | ImGuiKey_End))
                motion(Text::Motion::DocumentEnd, false);
            if (Key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_End))
                motion(Text::Motion::DocumentEnd, true);
            if (!assistOpen && Key(ImGuiKey_PageUp))
                motion(Text::Motion::PageUp, false, page);
            if (!assistOpen && Key(ImGuiKey_PageDown))
                motion(Text::Motion::PageDown, false, page);
            if (Key(ImGuiMod_Shift | ImGuiKey_PageUp))
                motion(Text::Motion::PageUp, true, page);
            if (Key(ImGuiMod_Shift | ImGuiKey_PageDown))
                motion(Text::Motion::PageDown, true, page);
            if (Key(ImGuiMod_Ctrl | ImGuiKey_A, false))
            {
                buffer.SelectAll();
                moved = true;
            }
            if (Key(ImGuiMod_Ctrl | ImGuiKey_C, false) || Key(ImGuiMod_Ctrl | ImGuiKey_Insert, false))
                CopyText(buffer.HasSelection() ? buffer.SelectedText() : LineCopy(buffer));

            if (editable)
            {
                const auto edit = [&]()
                {
                    edited = true;
                    view.ScrollToCaret = true;
                };
                if (Key(ImGuiKey_Backspace) || Key(ImGuiMod_Shift | ImGuiKey_Backspace))
                {
                    buffer.Backspace(false);
                    edit();
                }
                if (Key(ImGuiMod_Ctrl | ImGuiKey_Backspace))
                {
                    buffer.Backspace(true);
                    edit();
                }
                if (Key(ImGuiKey_Delete))
                {
                    buffer.Delete(false);
                    edit();
                }
                if (Key(ImGuiMod_Ctrl | ImGuiKey_Delete))
                {
                    buffer.Delete(true);
                    edit();
                }
                if (Key(ImGuiMod_Ctrl | ImGuiKey_X, false) || Key(ImGuiMod_Shift | ImGuiKey_Delete, false))
                {
                    if (buffer.HasSelection())
                    {
                        CopyText(buffer.SelectedText());
                        buffer.Backspace(false);
                    }
                    else
                    {
                        CopyText(LineCopy(buffer));
                        buffer.DeleteLines();
                    }
                    edit();
                }
                if (Key(ImGuiMod_Ctrl | ImGuiKey_V) || Key(ImGuiMod_Shift | ImGuiKey_Insert))
                {
                    if (const char* clipboard = ImGui::GetClipboardText(); clipboard && clipboard[0])
                    {
                        buffer.Paste(clipboard);
                        pasted = true;
                        edit();
                    }
                }
                if (Key(ImGuiMod_Ctrl | ImGuiKey_Z))
                {
                    if (buffer.Undo())
                        edit();
                }
                if (Key(ImGuiMod_Ctrl | ImGuiKey_Y) || Key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
                {
                    if (buffer.Redo())
                        edit();
                }
                if (!assistOpen && (Key(ImGuiKey_Enter) || Key(ImGuiKey_KeypadEnter)))
                {
                    if (state.Console)
                    {
                        state.Submitted = true;
                    }
                    else
                    {
                        buffer.Newline();
                        edit();
                    }
                }
                if (Key(ImGuiMod_Shift | ImGuiKey_Enter) || Key(ImGuiMod_Shift | ImGuiKey_KeypadEnter))
                {
                    buffer.Newline();
                    edit();
                }
                if (!assistOpen && Key(ImGuiKey_Tab))
                {
                    buffer.Indent();
                    edit();
                }
                if (Key(ImGuiMod_Shift | ImGuiKey_Tab))
                {
                    buffer.Outdent();
                    edit();
                }
                if (Pressed(Command::EditorToggleComment) && language != Language::Json)
                {
                    buffer.ToggleComment(CommentPrefix(language));
                    edit();
                }
                if (Pressed(Command::EditorDuplicateLines))
                {
                    buffer.DuplicateLines();
                    edit();
                }
                if (Pressed(Command::EditorMoveLinesUp))
                {
                    buffer.MoveLines(-1);
                    edit();
                }
                if (Pressed(Command::EditorMoveLinesDown))
                {
                    buffer.MoveLines(1);
                    edit();
                }
                if (Pressed(Command::EditorDeleteLines))
                {
                    buffer.DeleteLines();
                    edit();
                }
                if (Pressed(Command::EditorComplete))
                    forceComplete = true;
                const bool ignoreCharacters = io.KeyCtrl && !io.KeyAlt;
                if (!ignoreCharacters)
                {
                    for (const ImWchar c : io.InputQueueCharacters)
                    {
                        if (c < 0x20 || c == 0x7F)
                            continue;
                        buffer.Type(Utf8(c));
                        edit();
                    }
                }
                io.InputQueueCharacters.resize(0);
            }
            if (!state.Console)
            {
                if (Pressed(Command::EditorFind))
                    OpenFind(view, false);
                if (editable && Pressed(Command::EditorReplace))
                    OpenFind(view, true);
                if (PressedWhileTyping(Command::EditorFindNext))
                {
                    FindStep(view, true);
                    moved = true;
                }
                if (PressedWhileTyping(Command::EditorFindPrevious))
                {
                    FindStep(view, false);
                    moved = true;
                }
            }
            if (!state.Console && Pressed(Command::EditorGotoLine))
            {
                view.GotoOpen = true;
                view.GotoText = std::to_string(buffer.LineOf(buffer.Caret()) + 1);
                ImGui::OpenPopup("##goto");
            }
            if (edited || moved)
                view.LastInput = ImGui::GetTime();
        }

        if (ImGui::BeginPopupContextWindow("##context"))
        {
            const bool selection = buffer.HasSelection();
            if (ImGui::MenuItem(ICON_FA_UNDO "  Undo", "Ctrl+Z", false, editable && buffer.CanUndo()) && buffer.Undo())
                edited = true;
            if (ImGui::MenuItem(ICON_FA_REDO "  Redo", "Ctrl+Y", false, editable && buffer.CanRedo()) && buffer.Redo())
                edited = true;
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CUT "  Cut", "Ctrl+X", false, editable && selection))
            {
                CopyText(buffer.SelectedText());
                buffer.Backspace(false);
                edited = true;
            }
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, selection))
                CopyText(buffer.SelectedText());
            const char* clipboard = ImGui::GetClipboardText();
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, editable && clipboard && clipboard[0]))
            {
                buffer.Paste(clipboard);
                pasted = true;
                edited = true;
            }
            if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Select All", "Ctrl+A"))
                buffer.SelectAll();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COMMENT "  Comment Lines", ShortcutText(Command::EditorToggleComment), false, editable && language != Language::Json))
            {
                buffer.ToggleComment(CommentPrefix(language));
                edited = true;
            }
            if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate Lines", ShortcutText(Command::EditorDuplicateLines), false, editable))
            {
                buffer.DuplicateLines();
                edited = true;
            }
            if (!state.Console)
            {
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_SEARCH "  Find…", ShortcutText(Command::EditorFind)))
                    OpenFind(view, false);
                if (ImGui::MenuItem(ICON_FA_EXCHANGE_ALT "  Replace…", ShortcutText(Command::EditorReplace), false, editable))
                    OpenFind(view, true);
                if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Find Next", ShortcutText(Command::EditorFindNext), false, !view.FindText.empty() || buffer.HasSelection()))
                    FindStep(view, true);
                if (ImGui::MenuItem(ICON_FA_ARROW_UP "  Find Previous", ShortcutText(Command::EditorFindPrevious), false, !view.FindText.empty() || buffer.HasSelection()))
                    FindStep(view, false);
            }
            if (!state.Console && ImGui::MenuItem(ICON_FA_LIST_OL "  Go to Line…", ShortcutText(Command::EditorGotoLine)))
            {
                view.GotoOpen = true;
                view.GotoText = std::to_string(buffer.LineOf(buffer.Caret()) + 1);
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                ImGui::OpenPopup("##goto");
            }
            else
            {
                ImGui::EndPopup();
            }
        }
        if (view.GotoOpen)
        {
            ImGui::PopFont();
            if (ImGui::BeginPopup("##goto"))
            {
                ImGui::TextUnformatted(("Go to line (1 - " + std::to_string(lines) + ")").c_str());
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(Px(160.0f));
                if (InputText("##line", view.GotoText, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    const int target = std::atoi(view.GotoText.c_str());
                    if (target > 0)
                    {
                        buffer.GotoLine(target - 1);
                        view.ScrollToCaret = true;
                    }
                    ImGui::CloseCurrentPopup();
                    state.Focus = true;
                }
                ImGui::EndPopup();
            }
            else
            {
                view.GotoOpen = false;
                state.Focus = true;
            }
            ImGui::PushFont(Fonts::Mono(), 0.0f);
        }

        const std::size_t caret = buffer.Caret();
        const int caretLine = buffer.LineOf(caret);
        const int caretColumn = buffer.ColumnOf(caret);
        if (view.ScrollToCaret)
        {
            const ImRect inner = window->InnerRect;
            const float viewWidth = inner.GetWidth();
            const float viewHeight = inner.GetHeight();
            const float caretX = metrics.Gutter + metrics.PadX + static_cast<float>(caretColumn) * metrics.Char;
            const float caretY = metrics.PadY + static_cast<float>(caretLine) * metrics.Row;
            const float margin = metrics.Char * 4.0f;
            float scrollX = window->Scroll.x;
            float scrollY = window->Scroll.y;
            if (caretX < scrollX + metrics.Gutter + metrics.PadX)
                scrollX = std::max(0.0f, caretX - metrics.Gutter - metrics.PadX - margin);
            else if (caretX + metrics.Char > scrollX + viewWidth - metrics.Char)
                scrollX = caretX + metrics.Char + margin - viewWidth;
            if (caretY < scrollY + metrics.PadY)
                scrollY = std::max(0.0f, caretY - metrics.PadY);
            else if (caretY + metrics.Row > scrollY + viewHeight - metrics.PadY)
                scrollY = caretY + metrics.Row + metrics.PadY - viewHeight;
            if (scrollX != window->Scroll.x)
                ImGui::SetScrollX(scrollX);
            if (scrollY != window->Scroll.y)
                ImGui::SetScrollY(scrollY);
            view.ScrollToCaret = false;
        }

        ImVec2 caretScreen(textOrigin.x + static_cast<float>(caretColumn) * metrics.Char, textOrigin.y + static_cast<float>(caretLine) * metrics.Row);
        std::shared_ptr<const Lua::DocRecord> hoverDoc;
        if (assist.HoverShown && assist.HoverDoc && ImGui::IsMouseHoveringRect(assist.HoverMin, assist.HoverMax, false))
        {
            hoverDoc = assist.HoverDoc;
        }
        else if ((language == Language::Lua || language == Language::AutoAssembler || language == Language::Template) && state.Assist && overText && !view.Dragging && !ImGui::IsAnyMouseDown())
        {
            const int line = std::clamp(static_cast<int>(std::floor((io.MousePos.y - textOrigin.y) / metrics.Row)), 0, buffer.LineCount() - 1);
            const int column = static_cast<int>(std::floor((io.MousePos.x - textOrigin.x) / metrics.Char));
            const bool onText = column >= 0 && column < Text::Columns(buffer.Line(line), buffer.TabWidth) && io.MousePos.y >= textOrigin.y;
            const Text::ChainContext chain = onText ? Text::ChainAt(buffer.Text(), buffer.OffsetAt(line, column)) : Text::ChainContext{};
            if (!chain.Valid)
            {
                assist.HoverOffset = static_cast<std::size_t>(-1);
            }
            else if (assist.HoverOffset != chain.End)
            {
                assist.HoverOffset = chain.End;
                assist.HoverSince = ImGui::GetTime();
                assist.HoverResolved = false;
                assist.HoverDoc.reset();
            }
            else
            {
                if (!assist.HoverResolved && ImGui::GetTime() - assist.HoverSince > 0.4)
                {
                    const CodeAssist& provider = EditorShared::ProviderAt(*state.Assist, buffer.Text(), chain.Start);
                    assist.HoverDoc = EditorShared::DescribeAt(assist, provider, buffer.Text(), chain.Chain, chain.Method);
                    assist.HoverResolved = true;
                    assist.HoverShown = false;
                    assist.HoverAnchor = ImVec2(io.MousePos.x, textOrigin.y + static_cast<float>(line) * metrics.Row);
                    assist.HoverRow = metrics.Row;
                }
                hoverDoc = assist.HoverDoc;
            }
        }
        else if (!overText)
        {
            assist.HoverOffset = static_cast<std::size_t>(-1);
        }
        if (visible)
        {
            ImDrawList* draw = window->DrawList;
            const ImRect inner = window->InnerRect;
            const int count = buffer.LineCount();
            const int first = std::clamp(static_cast<int>((window->Scroll.y - metrics.PadY) / metrics.Row), 0, count - 1);
            const int last = std::clamp(first + static_cast<int>(inner.GetHeight() / metrics.Row) + 1, 0, count - 1);
            EnsureStates(view, language, last);
            const ImVec2 textMin(inner.Min.x + metrics.Gutter, inner.Min.y);
            draw->PushClipRect(textMin, inner.Max, true);
            const auto [selStart, selEnd] = buffer.Selection();
            const ImU32 plain = ImGui::GetColorU32(ManifoldTheme::Text);
            const ImU32 selectionColor = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, focused ? 0.32f : 0.18f));
            const ImU32 embeddedColor = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.075f));
            const bool showMatches = view.FindOpen && !view.FindText.empty() && !state.Console;
            if (showMatches)
                RefreshMatches(view);
            const ImU32 matchColor = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.22f));
            const ImU32 matchBorder = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.70f));
            std::vector<Text::TokenSpan> spans;
            for (int line = first; line <= last; ++line)
            {
                const float y = textOrigin.y + static_cast<float>(line) * metrics.Row;
                const float textY = y + std::round((metrics.Row - ImGui::GetTextLineHeight()) * 0.5f);
                const std::size_t lineStart = buffer.LineStart(line);
                const std::size_t lineEnd = buffer.LineEnd(line);
                if (state.ErrorLine == line + 1)
                    draw->AddRectFilled(ImVec2(textMin.x, y), ImVec2(inner.Max.x, y + metrics.Row), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.20f)));
                else if (line == caretLine && focused && selStart == selEnd && !state.Console)
                    draw->AddRectFilled(ImVec2(textMin.x, y), ImVec2(inner.Max.x, y + metrics.Row), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.04f)));
                spans.clear();
                Text::LineState lineState = view.States[static_cast<std::size_t>(line)];
                const std::string_view content = buffer.Line(line);
                if (language != Language::Plain)
                    Text::HighlightLine(language, content, lineStart, lineState, spans);
                // Template tags get a faint tint, so the Lua stands apart from
                // the Auto Assembler text around it; one rectangle per run.
                for (std::size_t index = 0; index < spans.size(); ++index)
                {
                    if (!spans[index].Embedded)
                        continue;
                    std::size_t last = index;
                    while (last + 1 < spans.size() && spans[last + 1].Embedded && spans[last + 1].Start == spans[last].End)
                        ++last;
                    const float x1 = textOrigin.x + static_cast<float>(buffer.ColumnOf(spans[index].Start)) * metrics.Char;
                    const float x2 = textOrigin.x + static_cast<float>(buffer.ColumnOf(spans[last].End)) * metrics.Char;
                    draw->AddRectFilled(ImVec2(x1, y + Px(1.0f)), ImVec2(x2, y + metrics.Row - Px(1.0f)), embeddedColor, Px(2.0f));
                    index = last;
                }
                if (showMatches)
                {
                    for (auto at = std::lower_bound(view.Matches.begin(), view.Matches.end(), lineStart); at != view.Matches.end() && *at < lineEnd; ++at)
                    {
                        const float x1 = textOrigin.x + static_cast<float>(buffer.ColumnOf(*at)) * metrics.Char;
                        const float x2 = textOrigin.x + static_cast<float>(buffer.ColumnOf(std::min(*at + view.FindText.size(), lineEnd))) * metrics.Char;
                        draw->AddRectFilled(ImVec2(x1, y + Px(1.0f)), ImVec2(x2, y + metrics.Row - Px(1.0f)), matchColor, Px(2.0f));
                        if (*at == selStart && selEnd - selStart == view.FindText.size())
                            draw->AddRect(ImVec2(x1, y + Px(1.0f)), ImVec2(x2, y + metrics.Row - Px(1.0f)), matchBorder, Px(2.0f));
                    }
                }
                if (selStart != selEnd && selStart <= lineEnd && selEnd >= lineStart && !(selEnd == lineStart && line > buffer.LineOf(selStart)))
                {
                    const std::size_t from = std::max(selStart, lineStart);
                    const std::size_t to = std::min(selEnd, lineEnd);
                    const float x1 = textOrigin.x + static_cast<float>(buffer.ColumnOf(from)) * metrics.Char;
                    float x2 = textOrigin.x + static_cast<float>(buffer.ColumnOf(to)) * metrics.Char;
                    if (selEnd > lineEnd)
                        x2 += metrics.Char * 0.6f;
                    draw->AddRectFilled(ImVec2(x1, y), ImVec2(x2, y + metrics.Row), selectionColor);
                }
                DrawLine(draw, metrics, textOrigin.x, textY, content, lineStart, spans, buffer.TabWidth, plain);
            }
            if (buffer.Size() == 0 && !state.Placeholder.empty())
                draw->AddText(metrics.Font, metrics.FontSize, ImVec2(textOrigin.x, textOrigin.y + std::round((metrics.Row - ImGui::GetTextLineHeight()) * 0.5f)),
                    ImGui::GetColorU32(ManifoldTheme::Muted), state.Placeholder.c_str());
            if (focused && !view.GotoOpen)
            {
                const bool blink = io.ConfigInputTextCursorBlink;
                const float since = static_cast<float>(ImGui::GetTime() - view.LastInput);
                if (!blink || since < 0.0f || std::fmod(since, 1.2f) <= 0.8f)
                    draw->AddRectFilled(ImVec2(caretScreen.x, caretScreen.y + Px(1.0f)), ImVec2(caretScreen.x + std::max(1.0f, Px(1.5f)), caretScreen.y + metrics.Row - Px(1.0f)), plain);
            }
            draw->PopClipRect();

            const ImVec2 gutterMin(window->Pos.x, window->Pos.y);
            const ImVec2 gutterMax(window->Pos.x + metrics.Gutter, window->Pos.y + window->Size.y);
            draw->PushClipRect(gutterMin, gutterMax, true);
            draw->AddRectFilled(gutterMin, gutterMax, ImGui::GetColorU32(ManifoldTheme::Sidebar), Px(4.0f), ImDrawFlags_RoundCornersLeft);
            if (state.Console)
            {
                const char* prompt = "\xE2\x80\xBA";
                const ImU32 color = ImGui::GetColorU32(focused ? ManifoldTheme::Accent : ManifoldTheme::Muted);
                const float y = textOrigin.y + std::round((metrics.Row - ImGui::GetTextLineHeight()) * 0.5f);
                draw->AddText(metrics.Font, metrics.FontSize, ImVec2(gutterMin.x + Px(7.0f), y), color, prompt);
            }
            else
            {
                for (int line = first; line <= last; ++line)
                {
                    const float y = textOrigin.y + static_cast<float>(line) * metrics.Row + std::round((metrics.Row - ImGui::GetTextLineHeight()) * 0.5f);
                    const std::string number = std::to_string(line + 1);
                    const float width = metrics.Font->CalcTextSizeA(metrics.FontSize, FLT_MAX, 0.0f, number.c_str()).x;
                    const ImVec4 color = state.ErrorLine == line + 1 ? ManifoldTheme::Error : line == caretLine ? ManifoldTheme::Label : ManifoldTheme::Muted;
                    draw->AddText(metrics.Font, metrics.FontSize, ImVec2(gutterMax.x - width - Px(8.0f), y), ImGui::GetColorU32(color), number.c_str());
                }
            }
            draw->PopClipRect();

            if (focused && !state.ReadOnly)
            {
                ImGuiContext& g = *GImGui;
                g.PlatformImeData.WantVisible = true;
                g.PlatformImeData.WantTextInput = true;
                g.PlatformImeData.InputPos = ImVec2(caretScreen.x - 1.0f, caretScreen.y);
                g.PlatformImeData.InputLineHeight = metrics.Row;
                g.PlatformImeData.ViewportId = window->Viewport->ID;
            }
        }
        const ImGuiID viewport = window->Viewport->ID;
        ImGui::EndChild();
        ImGui::PopFont();

        if (edited)
            text = buffer.Text();
        state.Active = focused;
        state.CursorLine = caretLine + 1;
        state.CursorColumn = caretColumn + 1;
        state.CursorOffset = static_cast<int>(caret);
        state.LineCount = buffer.LineCount();

        if (hoverDoc)
            EditorShared::DrawHover(id, assist, *hoverDoc, viewport);
        else
            assist.HoverShown = false;
        if (assistEnabled)
        {
            if (!focused)
            {
                EditorShared::ClearAssist(assist);
            }
            else
            {
                if (pasted)
                    assist.Open = false;
                EditorShared::UpdateAssist(assist, *state.Assist, buffer.Text(), buffer.Caret(), edited && !pasted, forceComplete);
                EditorShared::PopupAction action;
                if (assist.Open && !assist.Items.empty())
                    action = EditorShared::DrawSuggestions(id, assist, caretScreen, metrics.Row, viewport);
                else if (assist.HintDoc || !assist.Hints.empty())
                    action = EditorShared::DrawSignatureHelp(id, assist, caretScreen, metrics.Row, viewport);
                if (action.Accept >= 0 && action.Accept < static_cast<int>(assist.Items.size()))
                {
                    const std::size_t now = buffer.Caret();
                    buffer.Replace(std::min(assist.WordStart, now), now, assist.Items[static_cast<std::size_t>(action.Accept)].Name);
                    assist.Open = false;
                    assist.JustAccepted = true;
                    edited = true;
                    view.ScrollToCaret = true;
                }
                if (!action.Choice.empty())
                {
                    const bool code = Text::InCode(buffer.Text(), buffer.Caret());
                    buffer.Paste(code ? "\"" + action.Choice + "\"" : action.Choice);
                    edited = true;
                    view.ScrollToCaret = true;
                }
                if (action.Clicked || action.Accept >= 0 || !action.Choice.empty())
                    state.Focus = true;
                if (edited)
                    text = buffer.Text();
            }
        }
        ImGui::PopID();
        return edited;
    }
}
