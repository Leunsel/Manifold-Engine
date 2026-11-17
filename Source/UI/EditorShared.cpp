#include "UI/EditorShared.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/DocAnnotations.h"
#include "UI/DocView.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace Manifold::UI::EditorShared
{
    namespace
    {
        bool IsIdentifier(char c)
        {
            return Text::IsIdentifierChar(c);
        }

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        void AddBufferWords(const std::string& text, const Text::WordContext& word, std::vector<CodeSuggestion>& items)
        {
            std::set<std::string> known;
            for (const CodeSuggestion& item : items)
                known.insert(Lower(item.Name));
            const auto functions = Text::BufferFunctions(text);
            const std::string prefix = Lower(word.Prefix);
            const std::string member = word.Member ? word.Chain : std::string();
            std::size_t i = 0;
            while (i < text.size() && items.size() < 400)
            {
                if (!IsIdentifier(text[i]) || IsDigit(text[i]))
                {
                    ++i;
                    continue;
                }
                const std::size_t start = i;
                while (i < text.size() && IsIdentifier(text[i]))
                    ++i;
                if (start == word.Start)
                    continue;
                const bool afterSeparator = start > 0 && (text[start - 1] == '.' || text[start - 1] == ':');
                if (word.Member)
                {
                    if (!afterSeparator || start < member.size() + 1 || text.compare(start - 1 - member.size(), member.size(), member) != 0)
                        continue;
                    if (start - 1 > member.size() && (IsIdentifier(text[start - 2 - member.size()]) || text[start - 2 - member.size()] == '.'))
                        continue;
                }
                else if (afterSeparator)
                {
                    continue;
                }
                const std::string name = text.substr(start, i - start);
                const std::string lower = Lower(name);
                if (name.size() < 3 || known.count(lower) || Text::IsLuaKeyword(name) || lower.compare(0, prefix.size(), prefix) != 0 || lower == prefix)
                    continue;
                known.insert(lower);
                CodeSuggestion suggestion{ name, "word", "", "" };
                for (const Text::BufferFunction& function : functions)
                {
                    const std::size_t separator = function.Name.find_last_of(".:");
                    const std::string last = separator == std::string::npos ? function.Name : function.Name.substr(separator + 1);
                    if (last == name)
                    {
                        suggestion.Kind = "function";
                        suggestion.Signature = "(" + function.Params + ")";
                        break;
                    }
                }
                items.push_back(std::move(suggestion));
            }
        }

        void Refresh(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, const Text::WordContext& word)
        {
            std::vector<CodeSuggestion> items = provider.Complete ? provider.Complete(word.Chain, word.Prefix, word.Method) : std::vector<CodeSuggestion>{};
            AddBufferWords(text, word, items);
            std::stable_sort(items.begin(), items.end(), [&](const CodeSuggestion& a, const CodeSuggestion& b)
                {
                    const int left = Text::FuzzyScore(a.Name, word.Prefix);
                    const int right = Text::FuzzyScore(b.Name, word.Prefix);
                    if (left != right)
                        return left > right;
                    return Lower(a.Name) < Lower(b.Name);
                });
            items.erase(std::remove_if(items.begin(), items.end(), [&](const CodeSuggestion& item) { return Text::FuzzyScore(item.Name, word.Prefix) < 0; }), items.end());
            if (items.empty() || (items.size() == 1 && items.front().Name == word.Prefix))
            {
                assist.Open = false;
                return;
            }
            const std::string previous = assist.Open && assist.Selected < static_cast<int>(assist.Items.size()) ? assist.Items[static_cast<std::size_t>(assist.Selected)].Name : std::string();
            assist.Items = std::move(items);
            assist.Selected = 0;
            for (std::size_t i = 0; i < assist.Items.size(); ++i)
            {
                if (!previous.empty() && assist.Items[i].Name == previous)
                    assist.Selected = static_cast<int>(i);
            }
            assist.Open = true;
            assist.WordStart = word.Start;
            assist.Chain = word.Chain;
            assist.Method = word.Method;
        }

        const char* KindIcon(const std::string& kind)
        {
            if (kind == "function" || kind == "method")
                return ICON_FA_CUBE;
            if (kind == "table")
                return ICON_FA_FOLDER;
            if (kind == "object")
                return ICON_FA_CUBES;
            if (kind == "property")
                return ICON_FA_TAG;
            if (kind == "event")
                return ICON_FA_BOLT;
            if (kind == "keyword")
                return ICON_FA_KEY;
            if (kind == "word")
                return ICON_FA_FONT;
            return ICON_FA_CIRCLE;
        }

        ImVec4 KindColor(const std::string& kind)
        {
            if (kind == "function" || kind == "method")
                return ManifoldTheme::CodeRegister;
            if (kind == "table")
                return ManifoldTheme::CodeLabel;
            if (kind == "object" || kind == "property")
                return ManifoldTheme::AddressText;
            if (kind == "event")
                return ManifoldTheme::Warning;
            if (kind == "keyword")
                return ManifoldTheme::CodeKeyword;
            if (kind == "value")
                return ManifoldTheme::CodeNumber;
            return ManifoldTheme::Muted;
        }

        constexpr ImGuiWindowFlags kPopupFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;
    }

    ImU32 TokenColor(Text::Token kind)
    {
        switch (kind)
        {
        case Text::Token::Comment:
            return ImGui::GetColorU32(ManifoldTheme::CodeComment);
        case Text::Token::Keyword:
            return ImGui::GetColorU32(ManifoldTheme::CodeKeyword);
        case Text::Token::Number:
            return ImGui::GetColorU32(ManifoldTheme::CodeNumber);
        case Text::Token::String:
            return ImGui::GetColorU32(ManifoldTheme::CodeString);
        case Text::Token::Call:
        case Text::Token::Register:
            return ImGui::GetColorU32(ManifoldTheme::CodeRegister);
        case Text::Token::Section:
            return ImGui::GetColorU32(ManifoldTheme::CodeSection);
        case Text::Token::Label:
            return ImGui::GetColorU32(ManifoldTheme::CodeLabel);
        case Text::Token::Mnemonic:
            return ImGui::GetColorU32(ManifoldTheme::Label);
        case Text::Token::Text:
            break;
        }
        return ImGui::GetColorU32(ManifoldTheme::Text);
    }

    void ClearAssist(CodeAssistState& assist)
    {
        assist.Open = false;
        assist.Hints.clear();
        assist.HintChain.clear();
        assist.HintChoices.clear();
        assist.HintChoiceChain.clear();
        assist.HintChoiceArgument = -1;
    }

    std::shared_ptr<const Lua::DocRecord> BufferDoc(CodeAssistState& assist, const std::string& text, std::string_view chain)
    {
        const std::size_t hash = std::hash<std::string>{}(text);
        if (!assist.BufferDocs || assist.BufferDocsHash != hash)
        {
            assist.BufferDocs = std::make_shared<const Lua::AnnotatedFile>(Lua::ParseAnnotations(text));
            assist.BufferDocsHash = hash;
        }
        const std::string wanted(chain);
        const std::size_t dot = wanted.rfind('.');
        const std::string last = dot == std::string::npos ? wanted : wanted.substr(dot + 1);
        const Lua::AnnotatedFunction* best = nullptr;
        for (const Lua::AnnotatedFunction& function : assist.BufferDocs->Functions)
        {
            if (function.Doc.Path == wanted)
            {
                best = &function;
                break;
            }
            if (!best && function.Doc.Name == last && (dot != std::string::npos || function.Doc.Owner.empty()))
                best = &function;
        }
        if (!best)
            return nullptr;
        auto record = std::make_shared<Lua::DocRecord>(best->Doc);
        if (!best->Documented)
            record->Origin = Lua::DocOrigin::Reflection;
        return record;
    }

    std::shared_ptr<const Lua::DocRecord> DescribeAt(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, std::string_view chain, bool method)
    {
        std::shared_ptr<const Lua::DocRecord> found = provider.Describe ? provider.Describe(std::string(chain), method) : nullptr;
        if (!found || found->Origin == Lua::DocOrigin::Reflection)
        {
            std::shared_ptr<const Lua::DocRecord> local = BufferDoc(assist, text, chain);
            if (local && (!found || local->Origin != Lua::DocOrigin::Reflection))
                found = local;
        }
        if (found && method && found->Callable())
        {
            bool self = false;
            for (const Lua::DocSignature& signature : found->Signatures)
                self = self || (!signature.Params.empty() && signature.Params.front().Name == "self");
            if (self)
            {
                auto copy = std::make_shared<Lua::DocRecord>(*found);
                for (Lua::DocSignature& signature : copy->Signatures)
                {
                    if (!signature.Params.empty() && signature.Params.front().Name == "self")
                        signature.Params.erase(signature.Params.begin());
                }
                found = copy;
            }
        }
        return found;
    }

    void UpdateAssist(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, std::size_t cursor, bool changed, bool force)
    {
        cursor = std::min(cursor, text.size());
        const Text::WordContext word = Text::WordAt(text, cursor);
        if (assist.JustAccepted)
        {
            assist.JustAccepted = false;
        }
        else if (force || changed)
        {
            const char typed = cursor > 0 ? text[cursor - 1] : '\0';
            const bool want = word.Valid && (force || (word.Member && (typed == '.' || typed == ':' || IsIdentifier(typed))) ||
                (!word.Member && IsIdentifier(typed) && (word.Prefix.size() >= 2 || assist.Open)));
            if (want)
                Refresh(assist, provider, text, word);
            else
                assist.Open = false;
        }
        else if (assist.Open && (!word.Valid || word.Start != assist.WordStart || word.Chain != assist.Chain))
        {
            assist.Open = false;
        }

        const Text::CallContext call = Text::CallAt(text, cursor);
        if (!call.Valid || call.Paren == assist.DismissedParen)
        {
            assist.Hints.clear();
            assist.HintChain.clear();
            assist.HintChoices.clear();
            assist.HintChoiceChain.clear();
            assist.HintChoiceArgument = -1;
            assist.HintDoc.reset();
            if (!call.Valid)
                assist.DismissedParen = static_cast<std::size_t>(-1);
            return;
        }
        if (call.Chain != assist.HintChain || call.Method != assist.HintMethod || call.Paren != assist.HintParen)
        {
            assist.HintChain = call.Chain;
            assist.HintMethod = call.Method;
            assist.HintParen = call.Paren;
            assist.HintSignatureChosen = false;
            assist.HintChoiceChain.clear();
            assist.HintDoc = DescribeAt(assist, provider, text, call.Chain, call.Method);
            assist.Hints.clear();
            if (assist.HintDoc && assist.HintDoc->Callable())
            {
                for (const Lua::DocSignature& signature : assist.HintDoc->Signatures)
                    assist.Hints.push_back(assist.HintDoc->Name + "(" + Lua::FormatParams(signature) + ")");
            }
            else
            {
                assist.HintDoc.reset();
                if (provider.Signatures)
                    assist.Hints = provider.Signatures(call.Chain, call.Method);
            }
        }
        assist.HintArgument = call.Argument;
        if (assist.HintDoc && !assist.HintSignatureChosen)
            assist.HintSignature = BestSignature(*assist.HintDoc, call.Argument);
        if (assist.HintDoc)
        {
            assist.HintChoices.clear();
            const auto& signatures = assist.HintDoc->Signatures;
            if (!signatures.empty())
            {
                const auto& params = signatures[static_cast<std::size_t>(std::clamp(assist.HintSignature, 0, static_cast<int>(signatures.size()) - 1))].Params;
                if (call.Argument < static_cast<int>(params.size()))
                    assist.HintChoices = params[static_cast<std::size_t>(call.Argument)].Choices;
                else if (!params.empty() && params.back().Variadic)
                    assist.HintChoices = params.back().Choices;
            }
        }
        else if (call.Chain != assist.HintChoiceChain || call.Argument != assist.HintChoiceArgument)
        {
            assist.HintChoiceChain = call.Chain;
            assist.HintChoiceArgument = call.Argument;
            assist.HintChoices = provider.Choices ? provider.Choices(call.Chain, call.Method, call.Argument) : std::vector<std::string>{};
        }
    }

    namespace
    {
        void PushPopupStyle(ImVec2 padding)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(padding.x, padding.y));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(5.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::Border);
        }

        void PopPopupStyle()
        {
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }

        void PlaceNear(const std::string& name, ImVec2 caret, float lineHeight, ImGuiID viewport, bool preferAbove)
        {
            const ImGuiWindow* previous = ImGui::FindWindowByName(name.c_str());
            const float height = previous ? previous->Size.y : Px(160.0f);
            const ImGuiViewport* host = ImGui::FindViewportByID(viewport);
            const float top = host ? host->WorkPos.y : -FLT_MAX;
            const float bottom = host ? host->WorkPos.y + host->WorkSize.y : FLT_MAX;
            const ImVec2 above(caret.x - Px(6.0f), caret.y - Px(3.0f));
            const ImVec2 below(caret.x - Px(6.0f), caret.y + lineHeight + Px(3.0f));
            const bool roomAbove = above.y - height >= top;
            const bool roomBelow = below.y + height <= bottom;
            const bool useAbove = preferAbove ? (roomAbove || !roomBelow) : (!roomBelow && roomAbove);
            if (useAbove)
                ImGui::SetNextWindowPos(above, ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            else
                ImGui::SetNextWindowPos(below, ImGuiCond_Always, ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowViewport(viewport);
        }

        void TextHints(const CodeAssistState& assist)
        {
            PushMono();
            const std::size_t shown = std::min<std::size_t>(assist.Hints.size(), 4);
            for (std::size_t h = 0; h < shown; ++h)
            {
                const std::string& hint = assist.Hints[h];
                const std::size_t open = hint.find('(');
                const std::size_t close = hint.rfind(')');
                const std::string head = open == std::string::npos ? hint : hint.substr(0, open);
                const std::vector<std::string> params = open == std::string::npos || close == std::string::npos || close < open ? std::vector<std::string>{} :
                    Text::SplitParameters(std::string_view(hint).substr(open + 1, close - open - 1));
                int current = assist.HintArgument;
                if (!params.empty() && current >= static_cast<int>(params.size()) && params.back().find("...") != std::string::npos)
                    current = static_cast<int>(params.size()) - 1;
                ImGui::TextColored(ManifoldTheme::CodeRegister, "%s", head.c_str());
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ManifoldTheme::Muted, "(");
                for (std::size_t p = 0; p < params.size(); ++p)
                {
                    ImGui::SameLine(0.0f, 0.0f);
                    if (p > 0)
                    {
                        ImGui::TextColored(ManifoldTheme::Muted, ", ");
                        ImGui::SameLine(0.0f, 0.0f);
                    }
                    const bool active = static_cast<int>(p) == current;
                    ImGui::TextColored(active ? ManifoldTheme::Accent : ManifoldTheme::Text, "%s", params[p].c_str());
                }
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ManifoldTheme::Muted, ")");
            }
            if (assist.Hints.size() > shown)
                ImGui::TextColored(ManifoldTheme::Muted, "+%zu more", assist.Hints.size() - shown);
            PopMono();
        }
    }

    PopupAction DrawSuggestions(const char* id, CodeAssistState& assist, ImVec2 caret, float lineHeight, ImGuiID viewport)
    {
        PopupAction action;
        constexpr int kVisible = 10;
        const int count = static_cast<int>(assist.Items.size());
        if (count == 0)
            return action;
        assist.Selected = std::clamp(assist.Selected, 0, count - 1);
        if (assist.Selected < assist.First)
            assist.First = assist.Selected;
        if (assist.Selected >= assist.First + kVisible)
            assist.First = assist.Selected - kVisible + 1;
        assist.First = std::clamp(assist.First, 0, std::max(0, count - kVisible));

        const std::string name = std::string("##suggestions") + id;
        PlaceNear(name, caret, lineHeight, viewport, false);
        PushPopupStyle(ImVec2(6.0f, 6.0f));
        if (ImGui::Begin(name.c_str(), nullptr, kPopupFlags))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            const bool hoveredWindow = ImGui::IsWindowHovered();
            if (hoveredWindow && ImGui::GetIO().MouseWheel != 0.0f)
                assist.First = std::clamp(assist.First - static_cast<int>(ImGui::GetIO().MouseWheel * 3.0f), 0, std::max(0, count - kVisible));
            const float width = std::round(Px(300.0f));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImGui::BeginGroup();
            PushMono();
            ImFont* font = ImGui::GetFont();
            const float fontSize = ImGui::GetFontSize();
            const float rowHeight = std::round(ImGui::GetTextLineHeight() + Px(5.0f));
            const float iconWidth = std::round(Px(22.0f));
            for (int i = assist.First; i < std::min(count, assist.First + kVisible); ++i)
            {
                const CodeSuggestion& item = assist.Items[static_cast<std::size_t>(i)];
                const ImVec2 row = ImGui::GetCursorScreenPos();
                const ImVec2 rowMax(row.x + width, row.y + rowHeight);
                const bool hovered = hoveredWindow && ImGui::IsMouseHoveringRect(row, rowMax);
                if (i == assist.Selected)
                    draw->AddRectFilled(row, rowMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.20f)), Px(3.0f));
                else if (hovered)
                    draw->AddRectFilled(row, rowMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f)), Px(3.0f));
                if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    assist.Selected = i;
                    action.Accept = i;
                }
                const float textY = row.y + std::round((rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
                const ImVec4 kindColor = item.Doc ? DocKindColor(item.Doc->Kind) : KindColor(item.Kind);
                const char* icon = item.Doc ? DocKindIcon(item.Doc->Kind) : KindIcon(item.Kind);
                draw->AddText(font, fontSize, ImVec2(row.x + Px(5.0f), textY), ImGui::GetColorU32(kindColor), icon);
                const ImVec4 clip(row.x, row.y, row.x + width - Px(4.0f), row.y + rowHeight);
                const float nameX = row.x + iconWidth;
                draw->AddText(font, fontSize, ImVec2(nameX, textY), ImGui::GetColorU32(ManifoldTheme::Text), item.Name.c_str(), nullptr, 0.0f, &clip);
                if (!item.Signature.empty())
                {
                    const float signatureX = nameX + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, item.Name.c_str()).x;
                    draw->AddText(font, fontSize, ImVec2(signatureX, textY), ImGui::GetColorU32(ManifoldTheme::Muted), item.Signature.c_str(), nullptr, 0.0f, &clip);
                }
                ImGui::Dummy(ImVec2(width, rowHeight));
            }
            PopMono();
            if (count > kVisible)
                ImGui::TextColored(ManifoldTheme::Muted, "%d of %d", assist.Selected + 1, count);
            ImGui::EndGroup();

            const CodeSuggestion& current = assist.Items[static_cast<std::size_t>(assist.Selected)];
            if (current.Doc || !current.Text.empty())
            {
                ImGui::SameLine(0.0f, Px(8.0f));
                const ImVec2 top = ImGui::GetCursorScreenPos();
                ImGui::BeginGroup();
                ImGui::Dummy(ImVec2(Px(4.0f), 0.0f));
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::BeginGroup();
                const float detail = std::round(Px(340.0f));
                if (current.Doc)
                {
                    DocViewOptions options;
                    options.Width = detail;
                    DocView(*current.Doc, options);
                }
                else
                {
                    RichText(current.Text, detail, ManifoldTheme::Label);
                }
                ImGui::EndGroup();
                ImGui::EndGroup();
                draw->AddLine(ImVec2(top.x - Px(4.0f), top.y), ImVec2(top.x - Px(4.0f), ImGui::GetItemRectMax().y), ImGui::GetColorU32(ManifoldTheme::Border), Px(1.0f));
            }
            if (hoveredWindow && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                action.Clicked = true;
        }
        ImGui::End();
        PopPopupStyle();
        return action;
    }

    PopupAction DrawSignatureHelp(const char* id, CodeAssistState& assist, ImVec2 caret, float lineHeight, ImGuiID viewport)
    {
        PopupAction action;
        if (!assist.HintDoc && assist.Hints.empty())
            return action;
        const std::string name = std::string("##hints") + id;
        PlaceNear(name, caret, lineHeight, viewport, true);
        PushPopupStyle(ImVec2(8.0f, 6.0f));
        if (ImGui::Begin(name.c_str(), nullptr, kPopupFlags))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            if (assist.HintDoc)
            {
                DocViewOptions options;
                options.Width = std::round(Px(420.0f));
                options.Signature = assist.HintSignature;
                options.Parameter = assist.HintArgument;
                options.SignatureHelp = true;
                options.Detail = false;
                const DocViewResult result = DocView(*assist.HintDoc, options);
                const int signatures = static_cast<int>(assist.HintDoc->Signatures.size());
                if (result.SignatureStep != 0 && signatures > 0)
                {
                    assist.HintSignature = (assist.HintSignature + result.SignatureStep + signatures) % signatures;
                    assist.HintSignatureChosen = true;
                }
                action.Choice = result.Choice;
            }
            else
            {
                TextHints(assist);
                if (!assist.HintChoices.empty())
                    action.Choice = ChoiceChips(assist.HintChoices, std::round(Px(380.0f)), true);
            }
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                action.Clicked = true;
        }
        ImGui::End();
        PopPopupStyle();
        return action;
    }

    void DrawHover(const Lua::DocRecord& record)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(8.0f, 6.0f));
        if (ImGui::BeginTooltip())
        {
            DocViewOptions options;
            options.Width = std::round(Px(440.0f));
            DocView(record, options);
            ImGui::EndTooltip();
        }
        ImGui::PopStyleVar();
    }
}
