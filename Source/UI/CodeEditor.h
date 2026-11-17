#pragma once

#include "imgui.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    enum class Language
    {
        AutoAssembler,
        Lua,
        Plain
    };

    struct CodeSuggestion
    {
        std::string Name;
        std::string Kind;
        std::string Signature;
        std::string Text;
    };

    struct CodeAssist
    {
        std::function<std::vector<CodeSuggestion>(const std::string& chain, const std::string& prefix, bool method)> Complete;
        std::function<std::vector<std::string>(const std::string& chain, bool method)> Signatures;
        std::function<std::vector<std::string>(const std::string& chain, bool method, int argument)> Choices;
    };

    struct CodeAssistState
    {
        bool Open = false;
        std::vector<CodeSuggestion> Items;
        int Selected = 0;
        int First = 0;
        std::size_t WordStart = 0;
        std::string Chain;
        bool Method = false;
        bool JustAccepted = false;
        int RestoreCursor = -1;
        bool Replace = false;
        bool RemoveTyped = false;
        std::size_t ReplaceStart = 0;
        std::size_t ReplaceEnd = 0;
        std::string Replacement;
        std::string HintChain;
        bool HintMethod = false;
        std::vector<std::string> Hints;
        int HintArgument = 0;
        std::vector<std::string> HintChoices;
        std::string HintChoiceChain;
        int HintChoiceArgument = -1;
        std::size_t HintParen = static_cast<std::size_t>(-1);
        std::size_t DismissedParen = static_cast<std::size_t>(-1);
    };

    struct CodeEditorState
    {
        int ErrorLine = 0;
        int GotoLine = 0;
        bool Focus = false;
        int CursorLine = 1;
        int CursorColumn = 1;
        int LineCount = 1;
        bool ReadOnly = false;
        int CursorOffset = 0;
        bool Active = false;
        int FocusAttempts = 0;
        bool KeepCursor = false;
        int FocusCursor = 0;
        std::string Insert;
        CodeAssist* Assist = nullptr;
        CodeAssistState Assisting;
    };

    bool CodeEditor(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state);
}
