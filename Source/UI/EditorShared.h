#pragma once

#include "Text/Highlight.h"
#include "Text/LuaContext.h"
#include "UI/CodeEditor.h"

#include "imgui.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace Manifold::UI::EditorShared
{
    struct PopupAction
    {
        int Accept = -1;
        std::string Choice;
        bool Clicked = false;
    };

    ImU32 TokenColor(Text::Token kind);
    void ClearAssist(CodeAssistState& assist);
    void UpdateAssist(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, std::size_t cursor, bool changed, bool force);
    std::shared_ptr<const Lua::DocRecord> BufferDoc(CodeAssistState& assist, const std::string& text, std::string_view chain);
    std::shared_ptr<const Lua::DocRecord> DescribeAt(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, std::string_view chain, bool method);
    PopupAction DrawSuggestions(const char* id, CodeAssistState& assist, ImVec2 caret, float lineHeight, ImGuiID viewport);
    PopupAction DrawSignatureHelp(const char* id, CodeAssistState& assist, ImVec2 caret, float lineHeight, ImGuiID viewport);
    void DrawHover(const char* id, CodeAssistState& assist, const Lua::DocRecord& record, ImGuiID viewport);
}
