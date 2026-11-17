#include "UI/CodeEditor.h"

#include "UI/EditorShared.h"
#include "UI/EditorView.h"

#include "Core/Text.h"
#include "Text/Highlight.h"
#include "Text/LuaContext.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string_view>
#include <vector>

namespace Manifold::UI
{
    namespace
    {
        bool g_legacy = false;

        int TabCallback(ImGuiInputTextCallbackData* data)
        {
            auto* state = static_cast<CodeEditorState*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
            {
                if (state->KeepCursor)
                {
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = std::clamp(state->FocusCursor, 0, data->BufTextLen);
                    state->KeepCursor = false;
                }
                CodeAssistState& assist = state->Assisting;
                const bool replace = assist.Replace;
                if (replace)
                {
                    const int start = static_cast<int>(assist.ReplaceStart);
                    const int end = static_cast<int>(assist.ReplaceEnd);
                    if (assist.RemoveTyped && end < data->BufTextLen && (data->Buf[end] == '\t' || data->Buf[end] == '\n'))
                        data->DeleteChars(end, 1);
                    if (start <= end && end <= data->BufTextLen)
                    {
                        data->DeleteChars(start, end - start);
                        data->InsertChars(start, assist.Replacement.c_str());
                        data->CursorPos = data->SelectionStart = data->SelectionEnd = start + static_cast<int>(assist.Replacement.size());
                    }
                    assist.Replace = false;
                    assist.RemoveTyped = false;
                }
                for (int i = 0; i < data->BufTextLen; ++i)
                {
                    if (data->Buf[i] == '\t')
                    {
                        data->DeleteChars(i, 1);
                        data->InsertChars(i, "    ");
                        if (data->CursorPos > i)
                            data->CursorPos += 3;
                        data->SelectionStart = data->SelectionEnd = data->CursorPos;
                    }
                }
                if (!replace && assist.RestoreCursor >= 0)
                {
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = std::min(assist.RestoreCursor, data->BufTextLen);
                    assist.RestoreCursor = -1;
                }
                if (!state->Insert.empty())
                {
                    const int at = data->CursorPos;
                    data->InsertChars(at, state->Insert.c_str());
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = at + static_cast<int>(state->Insert.size());
                    state->Insert.clear();
                }
                int line = 1;
                int column = 1;
                for (int i = 0; i < data->CursorPos && i < data->BufTextLen; ++i)
                {
                    if (data->Buf[i] == '\n')
                    {
                        ++line;
                        column = 1;
                    }
                    else
                    {
                        ++column;
                    }
                }
                state->CursorLine = line;
                state->CursorColumn = column;
                if (state->GotoLine > 0)
                {
                    int target = 0;
                    int current = 1;
                    for (int i = 0; i < data->BufTextLen && current < state->GotoLine; ++i)
                    {
                        if (data->Buf[i] == '\n')
                        {
                            ++current;
                            target = i + 1;
                        }
                    }
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = target;
                    state->GotoLine = 0;
                }
                state->CursorOffset = data->CursorPos;
            }
            return 0;
        }

    }

    namespace
    {
        bool LegacyEditor(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state)
        {
            if (text.find('\t') != std::string::npos)
                text = ReplaceAll(text, "\t", "    ");

            ImGui::PushID(id);
            ImGui::PushFont(Fonts::Mono(), 0.0f);
            const float lineHeight = ImGui::GetTextLineHeight();
            const int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
            state.LineCount = lines;
            const int digits = std::max(3, static_cast<int>(std::to_string(lines).size()));
            const float gutter = ImGui::CalcTextSize("0").x * static_cast<float>(digits) + Px(14.0f);
            const ImVec2 available = ImGui::GetContentRegionAvail();
            if (size.x <= 0.0f)
                size.x = available.x + size.x;
            if (size.y <= 0.0f)
                size.y = available.y + size.y;
            size.x = std::max(size.x, gutter + Px(60.0f));
            size.y = std::max(size.y, lineHeight * 3.0f);

            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* parentList = ImGui::GetWindowDrawList();
            parentList->AddRectFilled(origin, ImVec2(origin.x + gutter, origin.y + size.y), ImGui::GetColorU32(ManifoldTheme::Sidebar), Px(4.0f), ImDrawFlags_RoundCornersLeft);

            CodeAssistState& assist = state.Assisting;
            const bool assistEnabled = language == Language::Lua && state.Assist && !state.ReadOnly;
            if (state.Active)
            {
                const ImGuiID owner = ImGui::GetID("##assist");
                const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive;
                if (ImGui::Shortcut(ImGuiKey_Escape, route, owner))
                {
                    if (!assist.Open && !assist.Hints.empty())
                        assist.DismissedParen = assist.HintParen;
                    assist.Open = false;
                }
                if (assistEnabled && assist.Open && !assist.Items.empty())
                {
                    const ImGuiIO& io = ImGui::GetIO();
                    const bool plain = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
                    const bool tab = plain && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
                    const bool enter = plain && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
                    const bool keypad = plain && ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
                    const int count = static_cast<int>(assist.Items.size());
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    {
                        assist.Selected = (assist.Selected + 1) % count;
                        assist.RestoreCursor = state.CursorOffset;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    {
                        assist.Selected = (assist.Selected + count - 1) % count;
                        assist.RestoreCursor = state.CursorOffset;
                    }
                    if (tab || enter || keypad)
                    {
                        assist.Replace = true;
                        assist.RemoveTyped = true;
                        assist.ReplaceStart = assist.WordStart;
                        assist.ReplaceEnd = static_cast<std::size_t>(std::max(0, state.CursorOffset));
                        assist.Replacement = assist.Items[static_cast<std::size_t>(std::clamp(assist.Selected, 0, count - 1))].Name;
                        assist.Open = false;
                        assist.JustAccepted = true;
                    }
                }
            }

            ImGui::SetCursorScreenPos(ImVec2(origin.x + gutter, origin.y));
            if (state.Focus)
            {
                ImGui::SetKeyboardFocusHere();
                if (!state.KeepCursor)
                {
                    state.KeepCursor = true;
                    state.FocusCursor = state.CursorOffset;
                }
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ManifoldTheme::WithAlpha(ManifoldTheme::Sidebar, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ManifoldTheme::Text);
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways;
            if (state.ReadOnly)
                flags |= ImGuiInputTextFlags_ReadOnly;
            const bool changed = InputMultiline("##code", text, ImVec2(size.x - gutter, size.y), flags, &TabCallback, &state);
            ImGui::PopStyleColor(3);
            state.Active = ImGui::IsItemActive();
            if (state.Focus && (state.Active || ++state.FocusAttempts > 30))
            {
                if (!state.Active)
                    state.KeepCursor = false;
                state.Focus = false;
                state.FocusAttempts = 0;
            }

            const std::size_t cursor = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, state.CursorOffset)), text.size());
            if (assistEnabled)
            {
                if (!state.Active)
                    EditorShared::ClearAssist(assist);
                else
                    EditorShared::UpdateAssist(assist, *state.Assist, text, cursor, changed, ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false));
            }
            bool caretKnown = false;
            ImVec2 caret(0.0f, 0.0f);

            ImGuiWindow* parent = ImGui::GetCurrentWindow();
            ImGuiWindow* child = parent->DC.ChildWindows.empty() ? nullptr : parent->DC.ChildWindows.back();
            if (child)
            {
                const ImVec2 padding = ImGui::GetStyle().FramePadding;
                const ImVec2 textOrigin(child->Pos.x + padding.x - child->Scroll.x, child->Pos.y + padding.y - child->Scroll.y);
                const int firstVisible = std::max(0, static_cast<int>((child->Scroll.y - padding.y) / lineHeight));
                const int lastVisible = firstVisible + static_cast<int>(child->Size.y / lineHeight) + 2;

                ImDrawList* drawList = child->DrawList;
                drawList->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
                ImFont* font = Fonts::Mono();
                const float fontSize = ImGui::GetFontSize();
                {
                    std::size_t lineBegin = cursor;
                    while (lineBegin > 0 && text[lineBegin - 1] != '\n')
                        --lineBegin;
                    caret.x = textOrigin.x + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data() + lineBegin, text.data() + cursor).x;
                    caret.y = textOrigin.y + static_cast<float>(state.CursorLine - 1) * lineHeight;
                    caretKnown = true;
                }

                Text::LineState block = Text::InitialState(language);
                std::vector<Text::TokenSpan> spans;
                const ImU32 plainColor = ImGui::GetColorU32(ManifoldTheme::Text);
                std::size_t lineStart = 0;
                for (int line = 0; lineStart <= text.size(); ++line)
                {
                    std::size_t lineEnd = text.find('\n', lineStart);
                    if (lineEnd == std::string::npos)
                        lineEnd = text.size();
                    const std::string_view content(text.data() + lineStart, lineEnd - lineStart);
                    const bool visible = line >= firstVisible && line <= lastVisible;
                    spans.clear();
                    Text::HighlightLine(language, content, lineStart, block, spans);

                    if (visible)
                    {
                        const float y = textOrigin.y + static_cast<float>(line) * lineHeight;
                        if (state.ErrorLine == line + 1)
                            drawList->AddRectFilled(ImVec2(child->Pos.x, y), ImVec2(child->Pos.x + child->Size.x, y + lineHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.20f)));
                        float x = textOrigin.x;
                        std::size_t drawn = lineStart;
                        for (const Text::TokenSpan& span : spans)
                        {
                            if (span.Start > drawn)
                            {
                                const char* begin = text.data() + drawn;
                                const char* end = text.data() + span.Start;
                                drawList->AddText(font, fontSize, ImVec2(x, y), plainColor, begin, end);
                                x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, end).x;
                            }
                            const char* begin = text.data() + span.Start;
                            const char* end = text.data() + span.End;
                            drawList->AddText(font, fontSize, ImVec2(x, y), EditorShared::TokenColor(span.Kind), begin, end);
                            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, end).x;
                            drawn = span.End;
                        }
                        if (drawn < lineEnd)
                            drawList->AddText(font, fontSize, ImVec2(x, y), plainColor, text.data() + drawn, text.data() + lineEnd);
                    }
                    if (line > lastVisible)
                        break;
                    lineStart = lineEnd + 1;
                }
                drawList->PopClipRect();

                parentList->PushClipRect(origin, ImVec2(origin.x + gutter, origin.y + size.y), true);
                for (int line = firstVisible; line <= std::min(lastVisible, lines - 1); ++line)
                {
                    const float y = textOrigin.y + static_cast<float>(line) * lineHeight;
                    const std::string number = std::to_string(line + 1);
                    const float width = ImGui::CalcTextSize(number.c_str()).x;
                    const bool current = line + 1 == state.CursorLine;
                    const ImVec4 color = state.ErrorLine == line + 1 ? ManifoldTheme::Error : current ? ManifoldTheme::Label : ManifoldTheme::Muted;
                    parentList->AddText(font, fontSize, ImVec2(origin.x + gutter - width - Px(7.0f), y), ImGui::GetColorU32(color), number.c_str());
                }
                parentList->PopClipRect();
            }
            ImGui::PopFont();
            if (assistEnabled && state.Active && caretKnown)
            {
                const ImGuiID viewport = ImGui::GetWindowViewport()->ID;
                if (assist.Open && !assist.Items.empty())
                    EditorShared::DrawSuggestions(id, assist, caret, lineHeight, viewport);
                else if (!assist.Hints.empty())
                    EditorShared::DrawSignatureHelp(id, assist, caret, lineHeight, viewport);
            }
            ImGui::PopID();
            return changed;
        }
    }

    void SetLegacyCodeEditor(bool legacy)
    {
        g_legacy = legacy;
    }

    bool LegacyCodeEditor()
    {
        return g_legacy;
    }

    bool CodeEditor(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state)
    {
        if (g_legacy)
            return LegacyEditor(id, text, language, size, state);
        return EditorView(id, text, language, size, state);
    }
}
