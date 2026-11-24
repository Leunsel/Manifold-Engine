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

        void Present(CodeAssistState& assist, std::vector<CodeSuggestion> items, const std::string& prefix, std::size_t start, const std::string& chain, bool method)
        {
            std::stable_sort(items.begin(), items.end(), [&](const CodeSuggestion& a, const CodeSuggestion& b)
                {
                    const int left = Text::FuzzyScore(a.Name, prefix);
                    const int right = Text::FuzzyScore(b.Name, prefix);
                    if (left != right)
                        return left > right;
                    return Lower(a.Name) < Lower(b.Name);
                });
            items.erase(std::remove_if(items.begin(), items.end(), [&](const CodeSuggestion& item) { return Text::FuzzyScore(item.Name, prefix) < 0; }), items.end());
            if (items.empty() || (items.size() == 1 && items.front().Name == prefix))
            {
                assist.Open = false;
                return;
            }
            const std::string previous = assist.Open && assist.Selected < static_cast<int>(assist.Items.size()) ? assist.Items[static_cast<std::size_t>(assist.Selected)].Name : std::string();
            assist.Items = std::move(items);
            assist.Selected = 0;
            assist.ShownSelected = -1;
            for (std::size_t i = 0; i < assist.Items.size(); ++i)
            {
                if (!previous.empty() && assist.Items[i].Name == previous)
                    assist.Selected = static_cast<int>(i);
            }
            assist.Open = true;
            assist.WordStart = start;
            assist.Chain = chain;
            assist.Method = method;
        }

        std::pair<std::string, std::string> SplitLast(std::string_view chain)
        {
            const std::size_t at = chain.find_last_of(".:");
            if (at == std::string_view::npos)
                return { std::string(), std::string(chain) };
            return { std::string(chain.substr(0, at)), std::string(chain.substr(at + 1)) };
        }

        std::string InferClass(const CodeAssist& provider, const std::string& text, std::size_t before, std::string_view chain, int depth)
        {
            if (depth > 4 || chain.empty() || !provider.ResolveType || !provider.ClassMember)
                return {};
            const std::size_t split = chain.find_first_of(".:");
            const std::string head(chain.substr(0, split));
            const std::string call = Text::AssignedCall(text, before, head);
            if (call.empty())
                return {};
            const auto [callObject, callName] = SplitLast(call);
            std::shared_ptr<const Lua::DocRecord> doc;
            if (!callObject.empty())
            {
                if (const std::string owner = InferClass(provider, text, before, callObject, depth + 1); !owner.empty())
                    doc = provider.ClassMember(owner, callName);
            }
            if (!doc && provider.Describe)
                doc = provider.Describe(call, false);
            if (!doc || !doc->Callable() || doc->Signatures.empty())
                return {};
            std::string docClass = provider.ResolveType(doc->Signatures.front().Returns, doc->Owner.empty() ? doc->Path : doc->Owner);
            for (std::size_t at = split; !docClass.empty() && at != std::string_view::npos;)
            {
                const std::size_t next = chain.find_first_of(".:", at + 1);
                const std::string field(chain.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1));
                const std::shared_ptr<const Lua::DocRecord> member = provider.ClassMember(docClass, field);
                docClass = member && !member->Type.empty() ? provider.ResolveType(member->Type, docClass) : std::string();
                at = next;
            }
            return docClass;
        }

        void Refresh(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, const Text::WordContext& word)
        {
            std::vector<CodeSuggestion> items = provider.Complete ? provider.Complete(word.Chain, word.Prefix, word.Method) : std::vector<CodeSuggestion>{};
            if (word.Member && provider.CompleteClass)
            {
                if (const std::string docClass = InferClass(provider, text, word.Start, word.Chain, 0); !docClass.empty())
                {
                    std::vector<CodeSuggestion> members = provider.CompleteClass(docClass, word.Prefix, word.Method);
                    for (CodeSuggestion& item : items)
                    {
                        if (std::none_of(members.begin(), members.end(), [&](const CodeSuggestion& member) { return member.Name == item.Name; }))
                            members.push_back(std::move(item));
                    }
                    items = std::move(members);
                }
            }
            AddBufferWords(text, word, items);
            Present(assist, std::move(items), word.Prefix, word.Start, word.Chain, word.Method);
        }

        bool AsmWordChar(char c)
        {
            return IsIdentifier(c) || c == '.' || c == '$' || c == '@';
        }

        void UpdateAsmCompletion(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, std::size_t cursor, bool changed, bool force)
        {
            if (assist.JustAccepted)
            {
                assist.JustAccepted = false;
                return;
            }
            const char typed = cursor > 0 ? text[cursor - 1] : '\0';
            if (force || changed)
            {
                std::size_t start = cursor;
                std::vector<CodeSuggestion> items = provider.CompleteAt(text, cursor, start);
                const std::string prefix = text.substr(start, cursor - start);
                const bool want = force || typed == '[' || (AsmWordChar(typed) && !prefix.empty() && (prefix.size() >= 2 || assist.Open || typed == '$' || typed == '.'));
                if (want)
                    Present(assist, std::move(items), prefix, start, std::string(), false);
                else
                    assist.Open = false;
                return;
            }
            if (!assist.Open)
                return;
            bool inside = cursor >= assist.WordStart;
            for (std::size_t i = assist.WordStart; inside && i < cursor; ++i)
                inside = AsmWordChar(text[i]);
            if (!inside)
                assist.Open = false;
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
        case Text::Token::Delimiter:
            return ImGui::GetColorU32(ManifoldTheme::Accent);
        case Text::Token::Variable:
            return ImGui::GetColorU32(ManifoldTheme::AddressText);
        case Text::Token::Macro:
            return ImGui::GetColorU32(ManifoldTheme::CodeMacro);
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
        if (!found && provider.ClassMember)
        {
            const auto [object, name] = SplitLast(chain);
            if (!object.empty())
            {
                if (const std::string docClass = InferClass(provider, text, text.size(), object, 0); !docClass.empty())
                    found = provider.ClassMember(docClass, name);
            }
        }
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

    const CodeAssist& ProviderAt(const CodeAssist& provider, const std::string& text, std::size_t cursor)
    {
        if (provider.Embedded && provider.UseEmbedded && provider.UseEmbedded(text, std::min(cursor, text.size())))
            return *provider.Embedded;
        return provider;
    }

    void UpdateAssist(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, std::size_t cursor, bool changed, bool force)
    {
        cursor = std::min(cursor, text.size());
        if (const CodeAssist& inner = ProviderAt(provider, text, cursor); &inner != &provider)
        {
            UpdateAssist(assist, inner, text, cursor, changed, force);
            return;
        }
        const Text::WordContext word = provider.CompleteAt ? Text::WordContext{} : Text::WordAt(text, cursor);
        if (provider.CompleteAt)
            UpdateAsmCompletion(assist, provider, text, cursor, changed, force);
        else if (assist.JustAccepted)
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

        struct Room
        {
            float Left = -FLT_MAX;
            float Top = -FLT_MAX;
            float Right = FLT_MAX;
            float Bottom = FLT_MAX;

            float Width() const { return Right - Left; }
        };

        Room RoomOf(ImGuiID viewport)
        {
            Room room;
            if (const ImGuiViewport* host = ImGui::FindViewportByID(viewport))
            {
                const float margin = Px(4.0f);
                room.Left = host->WorkPos.x + margin;
                room.Top = host->WorkPos.y + margin;
                room.Right = host->WorkPos.x + host->WorkSize.x - margin;
                room.Bottom = host->WorkPos.y + host->WorkSize.y - margin;
            }
            return room;
        }

        float PlaceNear(const std::string& name, ImVec2 anchor, float lineHeight, ImGuiID viewport, bool preferAbove)
        {
            const ImGuiWindow* previous = ImGui::FindWindowByName(name.c_str());
            const bool known = previous && previous->WasActive;
            const float height = known ? previous->Size.y : Px(160.0f);
            const float width = known ? previous->Size.x : Px(300.0f);
            const Room room = RoomOf(viewport);
            const float x = std::max(room.Left, std::min(anchor.x - Px(6.0f), room.Right - width));
            const float aboveY = anchor.y - Px(3.0f);
            const float belowY = anchor.y + lineHeight + Px(3.0f);
            const float spaceAbove = aboveY - room.Top;
            const float spaceBelow = room.Bottom - belowY;
            const bool roomAbove = spaceAbove >= height;
            const bool roomBelow = spaceBelow >= height;
            bool useAbove = preferAbove ? (roomAbove || !roomBelow) : (!roomBelow && roomAbove);
            if (!roomAbove && !roomBelow)
                useAbove = spaceAbove > spaceBelow;
            if (useAbove)
                ImGui::SetNextWindowPos(ImVec2(x, aboveY), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            else
                ImGui::SetNextWindowPos(ImVec2(x, belowY), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowViewport(viewport);
            return std::max(useAbove ? spaceAbove : spaceBelow, Px(80.0f));
        }

        bool BeginScrollingDoc(const char* id, float width, float maxHeight, bool toTop)
        {
            const float bar = ImGui::GetStyle().ScrollbarSize;
            ImGui::SetNextWindowSizeConstraints(ImVec2(width + bar, 0.0f), ImVec2(width + bar, std::max(maxHeight, Px(60.0f))));
            if (toTop)
                ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
            return ImGui::BeginChild(id, ImVec2(width + bar, 0.0f), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
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
        const int count = static_cast<int>(assist.Items.size());
        if (count == 0)
            return action;

        const std::string name = std::string("##suggestions") + id;
        const float available = PlaceNear(name, caret, lineHeight, viewport, false);
        const Room room = RoomOf(viewport);
        const float padding = Px(6.0f);
        const float gap = Px(12.0f);
        const float bar = ImGui::GetStyle().ScrollbarSize;
        const float roomWidth = room.Width() - padding * 2.0f;
        float width = std::round(Px(300.0f));
        const bool beside = roomWidth >= width + gap + bar + Px(220.0f);
        float detail = beside ? std::min(Px(340.0f), roomWidth - width - gap - bar) : std::max(Px(160.0f), std::min(Px(340.0f), roomWidth - bar));
        detail = std::round(detail);
        if (!beside)
            width = std::round(std::max(Px(160.0f), std::min(width, roomWidth)));

        const float rowGuess = std::round(ImGui::GetTextLineHeight() + Px(5.0f));
        const float listRoom = available - padding * 2.0f - ImGui::GetTextLineHeight() - (beside ? 0.0f : Px(72.0f));
        const int visible = std::clamp(static_cast<int>(listRoom / rowGuess), 3, 10);
        assist.Selected = std::clamp(assist.Selected, 0, count - 1);
        const bool moved = assist.Selected != assist.ShownSelected;
        if (moved)
        {
            if (assist.Selected < assist.First)
                assist.First = assist.Selected;
            if (assist.Selected >= assist.First + visible)
                assist.First = assist.Selected - visible + 1;
        }
        assist.First = std::clamp(assist.First, 0, std::max(0, count - visible));
        assist.ShownSelected = assist.Selected;
        PushPopupStyle(ImVec2(6.0f, 6.0f));
        if (ImGui::Begin(name.c_str(), nullptr, kPopupFlags))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            const bool hoveredWindow = ImGui::IsWindowHovered();
            if (hoveredWindow && ImGui::GetIO().MouseWheel != 0.0f)
                assist.First = std::clamp(assist.First - static_cast<int>(ImGui::GetIO().MouseWheel * 3.0f), 0, std::max(0, count - visible));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImGui::BeginGroup();
            PushMono();
            ImFont* font = ImGui::GetFont();
            const float fontSize = ImGui::GetFontSize();
            const float rowHeight = std::round(ImGui::GetTextLineHeight() + Px(5.0f));
            const float iconWidth = std::round(Px(22.0f));
            for (int i = assist.First; i < std::min(count, assist.First + visible); ++i)
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
            if (count > visible)
                ImGui::TextColored(ManifoldTheme::Muted, "%d of %d", assist.Selected + 1, count);
            ImGui::EndGroup();
            const float listHeight = ImGui::GetItemRectSize().y;

            const CodeSuggestion& current = assist.Items[static_cast<std::size_t>(assist.Selected)];
            if (current.Doc || !current.Text.empty())
            {
                if (beside)
                    ImGui::SameLine(0.0f, gap);
                else
                    ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
                const ImVec2 top = ImGui::GetCursorScreenPos();
                const float spacing = ImGui::GetStyle().ItemSpacing.y * 2.0f + Px(6.0f);
                const float maxHeight = beside ? available - padding * 2.0f - Px(2.0f) : available - padding * 2.0f - listHeight - spacing;
                if (BeginScrollingDoc("##detail", detail, maxHeight, moved))
                {
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
                }
                ImGui::EndChild();
                const ImU32 line = ImGui::GetColorU32(ManifoldTheme::Border);
                if (beside)
                    draw->AddLine(ImVec2(top.x - gap * 0.5f, top.y), ImVec2(top.x - gap * 0.5f, ImGui::GetItemRectMax().y), line, Px(1.0f));
                else
                    draw->AddLine(ImVec2(top.x, top.y - Px(3.0f)), ImVec2(top.x + width, top.y - Px(3.0f)), line, Px(1.0f));
            }
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
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
        const float available = PlaceNear(name, caret, lineHeight, viewport, true);
        const float width = std::round(std::max(Px(160.0f), std::min(Px(420.0f), RoomOf(viewport).Width() - Px(16.0f) - ImGui::GetStyle().ScrollbarSize)));
        PushPopupStyle(ImVec2(8.0f, 6.0f));
        if (ImGui::Begin(name.c_str(), nullptr, kPopupFlags))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            const bool open = BeginScrollingDoc("##hint", width, available - Px(12.0f), false);
            if (open && assist.HintDoc)
            {
                DocViewOptions options;
                options.Width = width;
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
            else if (open)
            {
                TextHints(assist);
                if (!assist.HintChoices.empty())
                    action.Choice = ChoiceChips(assist.HintChoices, width, true);
            }
            ImGui::EndChild();
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                action.Clicked = true;
        }
        ImGui::End();
        PopPopupStyle();
        return action;
    }

    void DrawHover(const char* id, CodeAssistState& assist, const Lua::DocRecord& record, ImGuiID viewport)
    {
        const std::string name = std::string("##hover") + id;
        const bool appearing = !assist.HoverShown;
        const float available = PlaceNear(name, assist.HoverAnchor, assist.HoverRow, viewport, false);
        const float width = std::round(std::max(Px(160.0f), std::min(Px(440.0f), RoomOf(viewport).Width() - Px(16.0f) - ImGui::GetStyle().ScrollbarSize)));
        PushPopupStyle(ImVec2(8.0f, 6.0f));
        if (ImGui::Begin(name.c_str(), nullptr, kPopupFlags))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            if (BeginScrollingDoc("##doc", width, available - Px(12.0f), appearing))
            {
                DocViewOptions options;
                options.Width = width;
                DocView(record, options);
            }
            ImGui::EndChild();
            const ImGuiWindow* window = ImGui::GetCurrentWindow();
            const float reach = Px(6.0f);
            assist.HoverMin = ImVec2(window->Pos.x - reach, window->Pos.y - reach);
            assist.HoverMax = ImVec2(window->Pos.x + window->Size.x + reach, window->Pos.y + window->Size.y + reach);
            assist.HoverShown = true;
        }
        ImGui::End();
        PopPopupStyle();
    }
}
