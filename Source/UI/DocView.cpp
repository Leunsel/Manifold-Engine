#include "UI/DocView.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <filesystem>

namespace Manifold::UI
{
    namespace
    {
        class Flow
        {
        public:
            Flow(float width, float spacing)
                : m_origin(ImGui::GetCursorScreenPos()), m_width(std::max(width, Px(40.0f))), m_x(m_origin.x), m_y(m_origin.y), m_line(ImGui::GetTextLineHeight()),
                  m_spacing(spacing)
            {
            }

            ImVec2 Place(float width, float gap)
            {
                if (m_x > m_origin.x && m_x + gap + width > m_origin.x + m_width)
                {
                    m_x = m_origin.x;
                    m_y += m_line + m_spacing;
                }
                else if (m_x > m_origin.x)
                {
                    m_x += gap;
                }
                const ImVec2 at(m_x, m_y);
                m_x += width;
                m_used = std::max(m_used, m_x - m_origin.x);
                return at;
            }

            void Break()
            {
                m_x = m_origin.x;
                m_y += m_line + m_spacing;
            }

            void Finish()
            {
                ImGui::SetCursorScreenPos(m_origin);
                ImGui::Dummy(ImVec2(std::max(m_used, 1.0f), m_y + m_line - m_origin.y));
            }

            float Line() const { return m_line; }

        private:
            ImVec2 m_origin;
            float m_width;
            float m_x;
            float m_y;
            float m_line;
            float m_spacing;
            float m_used = 0.0f;
        };

        void Token(Flow& flow, std::string_view text, ImU32 color, float gap = 0.0f, bool underline = false)
        {
            if (text.empty())
                return;
            const float width = ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
            const ImVec2 at = flow.Place(width, gap);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddText(at, color, text.data(), text.data() + text.size());
            if (underline)
                draw->AddLine(ImVec2(at.x, at.y + flow.Line()), ImVec2(at.x + width, at.y + flow.Line()), color, Px(1.0f));
        }

        void SignatureLine(const Lua::DocRecord& record, const Lua::DocSignature* signature, int active, float width)
        {
            PushMono();
            Flow flow(width, Px(2.0f));
            const ImU32 muted = ImGui::GetColorU32(ManifoldTheme::Muted);
            const ImU32 text = ImGui::GetColorU32(ManifoldTheme::Text);
            const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
            if (!signature)
            {
                Token(flow, record.Path, ImGui::GetColorU32(DocKindColor(record.Kind)));
                if (!record.Type.empty())
                    Token(flow, ": " + record.Type, muted);
                if (!record.Value.empty())
                    Token(flow, " = " + record.Value, ImGui::GetColorU32(ManifoldTheme::CodeNumber));
                flow.Finish();
                PopMono();
                return;
            }
            const std::string head = (record.Owner.empty() || record.OwnerIsClass ? record.Name : record.Path) + "(";
            Token(flow, head, ImGui::GetColorU32(ManifoldTheme::CodeRegister));
            const int count = static_cast<int>(signature->Params.size());
            int highlighted = active;
            if (count > 0 && highlighted >= count && signature->Params.back().Variadic)
                highlighted = count - 1;
            for (int i = 0; i < count; ++i)
            {
                const bool current = i == highlighted;
                Token(flow, FormatParam(signature->Params[static_cast<std::size_t>(i)]), current ? accent : text, i == 0 ? 0.0f : ImGui::CalcTextSize(" ").x, current);
                if (i + 1 < count)
                    Token(flow, ",", muted);
            }
            Token(flow, ")", muted);
            if (!signature->Returns.empty())
                Token(flow, "\xE2\x86\x92 " + signature->Returns, muted, ImGui::CalcTextSize(" ").x);
            flow.Finish();
            PopMono();
        }

        void Gap(float pixels)
        {
            ImGui::Dummy(ImVec2(0.0f, Px(pixels)));
        }

        std::string KeysText(const Lua::DocParam& param)
        {
            std::string text;
            for (const auto& [key, type] : param.Keys)
                text += (text.empty() ? "`" : ", `") + key + "` " + type;
            return text;
        }
    }

    const char* DocKindIcon(Lua::DocItemKind kind)
    {
        switch (kind)
        {
        case Lua::DocItemKind::Function:
        case Lua::DocItemKind::Method: return ICON_FA_CUBE;
        case Lua::DocItemKind::Table: return ICON_FA_FOLDER;
        case Lua::DocItemKind::Class: return ICON_FA_CUBES;
        case Lua::DocItemKind::Property:
        case Lua::DocItemKind::Field: return ICON_FA_TAG;
        case Lua::DocItemKind::Event: return ICON_FA_BOLT;
        case Lua::DocItemKind::Constant: return ICON_FA_HASHTAG;
        case Lua::DocItemKind::Keyword: return ICON_FA_KEY;
        }
        return ICON_FA_CIRCLE;
    }

    ImVec4 DocKindColor(Lua::DocItemKind kind)
    {
        switch (kind)
        {
        case Lua::DocItemKind::Function:
        case Lua::DocItemKind::Method: return ManifoldTheme::CodeRegister;
        case Lua::DocItemKind::Table: return ManifoldTheme::CodeLabel;
        case Lua::DocItemKind::Class:
        case Lua::DocItemKind::Property:
        case Lua::DocItemKind::Field: return ManifoldTheme::AddressText;
        case Lua::DocItemKind::Event: return ManifoldTheme::Warning;
        case Lua::DocItemKind::Constant: return ManifoldTheme::CodeNumber;
        case Lua::DocItemKind::Keyword: return ManifoldTheme::CodeKeyword;
        }
        return ManifoldTheme::Text;
    }

    const char* DocOriginLabel(Lua::DocOrigin origin)
    {
        switch (origin)
        {
        case Lua::DocOrigin::Manifold: return "Manifold API";
        case Lua::DocOrigin::CheatEngine: return "Cheat Engine";
        case Lua::DocOrigin::Standard: return "Lua standard library";
        case Lua::DocOrigin::Annotation: return "Annotations";
        case Lua::DocOrigin::Script: return "Registered by a script";
        case Lua::DocOrigin::Reflection: return "Parameter names only";
        }
        return "";
    }

    int BestSignature(const Lua::DocRecord& record, int argument)
    {
        for (std::size_t i = 0; i < record.Signatures.size(); ++i)
        {
            const auto& params = record.Signatures[i].Params;
            if (argument < static_cast<int>(params.size()) || (!params.empty() && params.back().Variadic))
                return static_cast<int>(i);
        }
        return 0;
    }

    void RichText(std::string_view text, float width, const ImVec4& color)
    {
        if (text.empty())
            return;
        Flow flow(width, Px(1.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float space = ImGui::CalcTextSize(" ").x;
        const ImU32 plain = ImGui::GetColorU32(color);
        const ImU32 code = ImGui::GetColorU32(ManifoldTheme::CodeRegister);
        bool inCode = false;
        bool first = true;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            const std::string_view line = text.substr(start, end - start);
            if (!first)
                flow.Break();
            first = false;
            std::size_t at = 0;
            while (at < line.size())
            {
                while (at < line.size() && line[at] == ' ')
                    ++at;
                if (at >= line.size())
                    break;
                std::size_t stop = at;
                while (stop < line.size() && line[stop] != ' ')
                    ++stop;
                const std::string_view word = line.substr(at, stop - at);
                float wordWidth = 0.0f;
                for (const char c : word)
                {
                    if (c != '`')
                        wordWidth += ImGui::CalcTextSize(&c, &c + 1).x;
                }
                const ImVec2 position = flow.Place(wordWidth, space);
                float x = position.x;
                std::size_t piece = 0;
                while (piece < word.size())
                {
                    if (word[piece] == '`')
                    {
                        inCode = !inCode;
                        ++piece;
                        continue;
                    }
                    std::size_t next = word.find('`', piece);
                    if (next == std::string_view::npos)
                        next = word.size();
                    draw->AddText(ImVec2(x, position.y), inCode ? code : plain, word.data() + piece, word.data() + next);
                    x += ImGui::CalcTextSize(word.data() + piece, word.data() + next).x;
                    piece = next;
                }
                at = stop;
            }
            if (end >= text.size())
                break;
            start = end + 1;
        }
        flow.Finish();
    }

    std::string ChoiceChips(const std::vector<std::string>& values, float width, bool interactive)
    {
        std::string clicked;
        if (values.empty())
            return clicked;
        PushMono();
        Flow flow(width, Px(3.0f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float pad = std::round(Px(5.0f));
        const float gap = std::round(Px(4.0f));
        const bool windowHovered = interactive && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        for (const std::string& value : values)
        {
            const std::string label = value.empty() ? "\"\"" : value;
            const float textWidth = ImGui::CalcTextSize(label.c_str()).x;
            const ImVec2 at = flow.Place(textWidth + pad * 2.0f, gap);
            const ImVec2 min(at.x, at.y);
            const ImVec2 max(at.x + textWidth + pad * 2.0f, at.y + flow.Line());
            const bool hovered = windowHovered && ImGui::IsMouseHoveringRect(min, max);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::CodeString, hovered ? 0.30f : 0.12f)), Px(3.0f));
            draw->AddText(ImVec2(min.x + pad, min.y), ImGui::GetColorU32(ManifoldTheme::CodeString), label.c_str());
            if (hovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    clicked = value;
            }
        }
        flow.Finish();
        PopMono();
        return clicked;
    }

    DocViewResult DocView(const Lua::DocRecord& record, const DocViewOptions& options)
    {
        DocViewResult result;
        const float width = std::round(options.Width);
        const int count = static_cast<int>(record.Signatures.size());
        const int chosen = count > 0 ? std::clamp(options.Signature, 0, count - 1) : 0;
        const Lua::DocSignature* signature = record.Callable() && count > 0 ? &record.Signatures[static_cast<std::size_t>(chosen)] : nullptr;

        if (options.SignatureHelp && count > 1)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, PxWhole(3.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            if (ImGui::SmallButton(ICON_FA_CHEVRON_LEFT "##previous"))
                result.SignatureStep = -1;
            ImGui::SameLine(0.0f, Px(2.0f));
            ImGui::TextColored(ManifoldTheme::Muted, "%d/%d", chosen + 1, count);
            ImGui::SameLine(0.0f, Px(2.0f));
            if (ImGui::SmallButton(ICON_FA_CHEVRON_RIGHT "##next"))
                result.SignatureStep = 1;
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        SignatureLine(record, signature, options.SignatureHelp ? options.Parameter : -1, width);

        if (record.IsDeprecated)
        {
            Gap(2.0f);
            RichText("Deprecated" + (record.Deprecated.empty() ? std::string(".") : ": " + record.Deprecated), width, ManifoldTheme::Warning);
        }
        if (record.Support != Lua::DocSupport::Full && !record.SupportNote.empty())
        {
            Gap(2.0f);
            RichText(record.SupportNote, width, record.Support == Lua::DocSupport::Missing ? ManifoldTheme::Warning : ManifoldTheme::Muted);
        }

        if (options.SignatureHelp && signature && options.Parameter >= 0 && !signature->Params.empty())
        {
            int index = options.Parameter;
            if (index >= static_cast<int>(signature->Params.size()) && signature->Params.back().Variadic)
                index = static_cast<int>(signature->Params.size()) - 1;
            if (index < static_cast<int>(signature->Params.size()))
            {
                const Lua::DocParam& param = signature->Params[static_cast<std::size_t>(index)];
                if (!param.Type.empty() || !param.Text.empty() || !param.Choices.empty() || !param.Keys.empty())
                {
                    Gap(3.0f);
                    PushMono();
                    ImGui::TextColored(ManifoldTheme::Accent, "%s", param.Name.c_str());
                    if (!param.Type.empty())
                    {
                        ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x);
                        ImGui::TextColored(ManifoldTheme::Muted, "%s", param.Type.c_str());
                    }
                    PopMono();
                    if (!param.Text.empty())
                        RichText(param.Text, width, ManifoldTheme::Label);
                    if (!param.Keys.empty())
                        RichText("Options: " + KeysText(param), width, ManifoldTheme::Label);
                    if (!param.Default.empty())
                        RichText("Default: `" + param.Default + "`", width, ManifoldTheme::Label);
                    if (!param.Choices.empty())
                    {
                        Gap(2.0f);
                        result.Choice = ChoiceChips(param.Choices, width, true);
                    }
                }
            }
        }

        const std::string& body = options.Detail ? record.Text : record.Summary;
        if (!body.empty())
        {
            Gap(3.0f);
            RichText(body, width, options.SignatureHelp ? ManifoldTheme::Muted : ManifoldTheme::Text);
        }
        if (!record.Callable() && !record.Choices.empty())
        {
            Gap(2.0f);
            ChoiceChips(record.Choices, width, false);
        }

        if (options.Detail && signature)
        {
            bool any = false;
            for (const Lua::DocParam& param : signature->Params)
                any = any || !param.Text.empty() || !param.Type.empty() || !param.Choices.empty();
            if (any)
            {
                Gap(4.0f);
                for (const Lua::DocParam& param : signature->Params)
                {
                    PushMono();
                    ImGui::TextColored(ManifoldTheme::Text, "%s", FormatParam(param).c_str());
                    if (!param.Type.empty())
                    {
                        ImGui::SameLine(0.0f, ImGui::CalcTextSize(" ").x);
                        ImGui::TextColored(ManifoldTheme::Muted, "%s", param.Type.c_str());
                    }
                    PopMono();
                    ImGui::Indent(Px(12.0f));
                    if (!param.Text.empty())
                        RichText(param.Text, width - Px(12.0f), ManifoldTheme::Label);
                    if (!param.Keys.empty())
                        RichText("Options: " + KeysText(param), width - Px(12.0f), ManifoldTheme::Label);
                    if (!param.Choices.empty())
                        ChoiceChips(param.Choices, width - Px(12.0f), false);
                    ImGui::Unindent(Px(12.0f));
                }
            }
            if (!signature->Returns.empty() || !signature->ReturnsText.empty())
            {
                Gap(3.0f);
                RichText("Returns `" + signature->Returns + "`" + (signature->ReturnsText.empty() ? std::string() : ": " + signature->ReturnsText), width, ManifoldTheme::Label);
            }
        }

        if (options.Detail)
        {
            Gap(4.0f);
            std::string footer = DocOriginLabel(record.Origin);
            if (!record.Source.empty())
            {
                std::string file = WideToUtf8(std::filesystem::path(Utf8ToWide(record.Source)).filename().wstring());
                if (file.empty())
                    file = record.Source;
                footer += "  \xC2\xB7  " + file + (record.Line > 0 ? ":" + std::to_string(record.Line) : std::string());
            }
            if (!record.See.empty())
            {
                std::string see;
                for (const std::string& item : record.See)
                    see += (see.empty() ? "" : ", ") + item;
                footer += "  \xC2\xB7  see " + see;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextUnformatted(footer.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        return result;
    }
}
