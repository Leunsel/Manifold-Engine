#pragma once

#include "Text/Highlight.h"
#include "Text/TextBuffer.h"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Manifold::Lua
{
    struct DocRecord;
    struct AnnotatedFile;
}

namespace Manifold::UI
{
    using Language = Text::Language;

    struct CodeSuggestion
    {
        std::string Name;
        std::string Kind;
        std::string Signature;
        std::string Text;
        std::shared_ptr<const Lua::DocRecord> Doc;
    };

    struct CodeAssist
    {
        std::function<std::vector<CodeSuggestion>(const std::string& chain, const std::string& prefix, bool method)> Complete;
        std::function<std::vector<std::string>(const std::string& chain, bool method)> Signatures;
        std::function<std::vector<std::string>(const std::string& chain, bool method, int argument)> Choices;
        std::function<std::shared_ptr<const Lua::DocRecord>(const std::string& chain, bool method)> Describe;
        std::function<std::string(const std::string& type, const std::string& scope)> ResolveType;
        std::function<std::vector<CodeSuggestion>(const std::string& className, const std::string& prefix, bool method)> CompleteClass;
        std::function<std::shared_ptr<const Lua::DocRecord>(const std::string& className, const std::string& member)> ClassMember;
    };

    struct CodeAssistState
    {
        bool Open = false;
        std::vector<CodeSuggestion> Items;
        int Selected = 0;
        int First = 0;
        int ShownSelected = -1;
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
        std::shared_ptr<const Lua::DocRecord> HintDoc;
        int HintSignature = 0;
        bool HintSignatureChosen = false;
        std::size_t HoverOffset = static_cast<std::size_t>(-1);
        double HoverSince = 0.0;
        bool HoverResolved = false;
        std::shared_ptr<const Lua::DocRecord> HoverDoc;
        ImVec2 HoverAnchor;
        float HoverRow = 0.0f;
        bool HoverShown = false;
        ImVec2 HoverMin;
        ImVec2 HoverMax;
        std::shared_ptr<const Lua::AnnotatedFile> BufferDocs;
        std::size_t BufferDocsHash = 0;
        bool PopupClicked = false;
    };

    struct EditorViewState
    {
        Text::TextBuffer Buffer;
        bool Synced = false;
        bool Dragging = false;
        int ClickMode = 0;
        std::size_t DragStart = 0;
        std::size_t DragEnd = 0;
        bool ScrollToCaret = false;
        double LastInput = 0.0;
        bool GotoOpen = false;
        std::string GotoText;
        std::vector<Text::LineState> States;
        int StatesValid = 0;
        Text::Language StatesLanguage = Text::Language::Plain;
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
        EditorViewState View;
        bool Console = false;
        int ConsoleMaxLines = 8;
        std::string Placeholder;
        bool Submitted = false;
        int HistoryStep = 0;
    };

    bool CodeEditor(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state);
    void SetLegacyCodeEditor(bool legacy);
    bool LegacyCodeEditor();
}
