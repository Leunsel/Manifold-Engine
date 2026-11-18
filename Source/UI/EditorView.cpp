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
            return language == Language::AutoAssembler ? "//" : "--";
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

        bool edited = false;
        bool moved = false;
        bool forceComplete = false;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const bool focused = ImGui::IsWindowFocused();
        CodeAssistState& assist = state.Assisting;
        const bool assistEnabled = language == Language::Lua && state.Assist && !state.ReadOnly;
        const bool editable = !state.ReadOnly;
        ImVec2 textOrigin(window->Pos.x + metrics.Gutter + metrics.PadX - window->Scroll.x, window->Pos.y + metrics.PadY - window->Scroll.y);

        if (!state.Insert.empty() && editable)
        {
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
                    assist.DismissedParen = assist.HintParen;
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
                edited = true;
            }
            if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Select all", "Ctrl+A"))
                buffer.SelectAll();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COMMENT "  Comment lines", ShortcutText(Command::EditorToggleComment), false, editable && language != Language::Json))
            {
                buffer.ToggleComment(CommentPrefix(language));
                edited = true;
            }
            if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate lines", ShortcutText(Command::EditorDuplicateLines), false, editable))
            {
                buffer.DuplicateLines();
                edited = true;
            }
            if (!state.Console && ImGui::MenuItem(ICON_FA_LIST_OL "  Go to line…", ShortcutText(Command::EditorGotoLine)))
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
        if (language == Language::Lua && state.Assist && overText && !view.Dragging && !ImGui::IsAnyMouseDown())
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
                    assist.HoverDoc = EditorShared::DescribeAt(assist, *state.Assist, buffer.Text(), chain.Chain, chain.Method);
                    assist.HoverResolved = true;
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
                spans.clear();
                Text::LineState lineState = view.States[static_cast<std::size_t>(line)];
                const std::string_view content = buffer.Line(line);
                if (language != Language::Plain)
                    Text::HighlightLine(language, content, lineStart, lineState, spans);
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
            EditorShared::DrawHover(*hoverDoc);
        if (assistEnabled)
        {
            if (!focused)
            {
                EditorShared::ClearAssist(assist);
            }
            else
            {
                EditorShared::UpdateAssist(assist, *state.Assist, buffer.Text(), buffer.Caret(), edited, forceComplete);
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
