#include "UI/Widgets.h"

#include "Core/Errors.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Loading.h"
#include "UI/UiScale.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <string_view>
#include <utility>

#include <windows.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr float kCardRounding = 6.0f;

        int ResizeCallback(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                auto* text = static_cast<std::string*>(data->UserData);
                text->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = text->data();
            }
            return 0;
        }

        struct ChainedCallback
        {
            std::string* Text;
            ImGuiInputTextCallback Inner;
            void* InnerUser;
        };

        int ChainCallback(ImGuiInputTextCallbackData* data)
        {
            auto* chain = static_cast<ChainedCallback*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
            {
                chain->Text->resize(static_cast<std::size_t>(data->BufTextLen));
                data->Buf = chain->Text->data();
                return 0;
            }
            if (chain->Inner)
            {
                data->UserData = chain->InnerUser;
                const int result = chain->Inner(data);
                data->UserData = chain;
                return result;
            }
            return 0;
        }

        int g_toolWindowCount = 0;

        std::vector<std::string> WrapWords(std::string_view text, float width)
        {
            std::vector<std::string> lines;
            std::string line;
            std::size_t at = 0;
            while (at < text.size())
            {
                while (at < text.size() && text[at] == ' ')
                    ++at;
                const std::size_t end = std::min(text.find(' ', at), text.size());
                if (end == at)
                    break;
                const std::string word(text.substr(at, end - at));
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && ImGui::CalcTextSize(candidate.c_str()).x > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                {
                    line = candidate;
                }
                at = end;
            }
            if (!line.empty())
                lines.push_back(line);
            return lines;
        }

        std::vector<std::string> BalancedWrap(std::string_view text, float width)
        {
            std::vector<std::string> lines = WrapWords(text, width);
            if (lines.size() < 2)
                return lines;
            float low = width * 0.4f;
            float high = width;
            for (int step = 0; step < 10; ++step)
            {
                const float middle = (low + high) * 0.5f;
                if (WrapWords(text, middle).size() == lines.size())
                    high = middle;
                else
                    low = middle;
            }
            return WrapWords(text, high);
        }

        void CenteredText(ImDrawList* draw, float centerX, float y, ImU32 color, std::string_view text, float maxWidth)
        {
            const char* begin = text.data();
            const char* end = text.data() + text.size();
            const float width = ImGui::CalcTextSize(begin, end).x;
            const float left = std::round(centerX - std::min(width, maxWidth) * 0.5f);
            const ImVec4 clip(left, y, left + maxWidth, y + ImGui::GetTextLineHeight());
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(left, y), color, begin, end, 0.0f, &clip);
        }

        ImVec4 Mix(ImVec4 from, ImVec4 to, float amount)
        {
            return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount, from.z + (to.z - from.z) * amount, from.w);
        }
    }

    unsigned int SkeletonNoise(unsigned int value)
    {
        value *= 2654435761u;
        value ^= value >> 15;
        value *= 2246822519u;
        value ^= value >> 13;
        return value;
    }

    int EmptyState(const char* icon, std::string_view title, std::string_view text, const std::vector<EmptyAction>& actions, bool card, std::string_view footer)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float line = ImGui::GetTextLineHeight();
        const float button = ImGui::GetFrameHeight();
        const float width = std::round(std::clamp(available.x - Px(24.0f), Px(220.0f), Px(card ? 440.0f : 400.0f)));
        const float radius = std::round(Px(card ? 30.0f : 24.0f));
        const float rowGap = std::round(Px(10.0f));

        std::vector<std::vector<int>> rows;
        for (int i = 0; i < static_cast<int>(actions.size()); ++i)
        {
            if (rows.empty() || actions[static_cast<std::size_t>(i)].NewRow)
                rows.emplace_back();
            rows.back().push_back(i);
        }

        struct Layout
        {
            bool Icon = true;
            float Padding = 0.0f;
            float Height = 0.0f;
            std::vector<std::string> Lines;
        };
        const auto measure = [&](bool withIcon, bool withText, float padding) {
            Layout layout;
            layout.Icon = withIcon;
            layout.Padding = padding;
            if (withText && !text.empty())
                layout.Lines = BalancedWrap(text, width - padding * 2.0f - Px(card ? 0.0f : 20.0f));
            float height = padding * 2.0f + line;
            if (withIcon)
                height += radius * 2.0f + Px(14.0f);
            if (!layout.Lines.empty())
                height += Px(6.0f) + line * static_cast<float>(layout.Lines.size());
            if (!rows.empty())
                height += Px(18.0f) + button * static_cast<float>(rows.size()) + rowGap * static_cast<float>(rows.size() - 1);
            if (!footer.empty())
                height += Px(12.0f) + line;
            layout.Height = height;
            return layout;
        };
        const float padding = std::round(Px(card ? 26.0f : 8.0f));
        const float tight = std::round(Px(card ? 14.0f : 4.0f));
        const float room = available.y - Px(8.0f);
        Layout layout = measure(true, true, padding);
        if (layout.Height > room)
            layout = measure(false, true, tight);
        if (layout.Height > room)
            layout = measure(false, false, tight);

        const float inner = width - layout.Padding * 2.0f;
        const ImVec2 min(std::round(origin.x + (available.x - width) * 0.5f), std::round(origin.y + std::max(0.0f, (available.y - layout.Height) * 0.45f)));
        const ImVec2 max(min.x + width, min.y + layout.Height);
        const float centerX = std::round(min.x + width * 0.5f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (card)
        {
            const float rounding = Px(8.0f);
            for (int layer = 3; layer >= 1; --layer)
            {
                const float spread = Px(3.0f) * static_cast<float>(layer);
                draw->AddRectFilled(ImVec2(min.x - spread, min.y - spread + Px(4.0f)), ImVec2(max.x + spread, max.y + spread + Px(4.0f)), IM_COL32(0, 0, 0, 16), rounding + spread);
            }
            draw->AddRectFilled(min, max, ImGui::GetColorU32(Mix(ManifoldTheme::Card, ManifoldTheme::Text, 0.025f)), rounding);
            draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::Border), rounding);
        }
        else
        {
            for (int layer = 4; layer >= 0; --layer)
            {
                const float spread = Px(10.0f) * static_cast<float>(layer);
                draw->AddRectFilled(ImVec2(min.x - spread, min.y - spread), ImVec2(max.x + spread, max.y + spread), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Card, 0.30f)),
                    Px(16.0f) + spread);
            }
        }

        float y = min.y + layout.Padding;
        if (layout.Icon)
        {
            const ImVec2 center(centerX, y + radius);
            const ImVec4 tint = card ? ManifoldTheme::Accent : ManifoldTheme::Muted;
            draw->AddCircleFilled(center, radius, ImGui::GetColorU32(ManifoldTheme::WithAlpha(tint, card ? 0.12f : 0.08f)), 48);
            draw->AddCircle(center, radius, ImGui::GetColorU32(ManifoldTheme::WithAlpha(tint, card ? 0.40f : 0.22f)), 48, Px(1.0f));
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * (card ? 1.9f : 1.6f));
            const ImVec2 iconSize = ImGui::CalcTextSize(icon);
            draw->AddText(ImVec2(std::round(center.x - iconSize.x * 0.5f), std::round(center.y - iconSize.y * 0.5f)), ImGui::GetColorU32(card ? ManifoldTheme::Accent : ManifoldTheme::Label), icon);
            ImGui::PopFont();
            y += radius * 2.0f + Px(14.0f);
        }

        ImGui::PushFont(Fonts::Bold(), 0.0f);
        CenteredText(draw, centerX, y, ImGui::GetColorU32(card ? ManifoldTheme::Text : ManifoldTheme::Label), title, inner);
        ImGui::PopFont();
        y += line;
        if (!layout.Lines.empty())
        {
            y += Px(6.0f);
            for (const std::string& part : layout.Lines)
            {
                CenteredText(draw, centerX, y, ImGui::GetColorU32(ManifoldTheme::Muted), part, inner);
                y += line;
            }
        }

        int clicked = -1;
        if (!rows.empty())
        {
            y += Px(18.0f);
            const float gap = std::round(Px(10.0f));
            for (const std::vector<int>& row : rows)
            {
                std::vector<float> widths;
                float total = 0.0f;
                for (int index : row)
                {
                    const EmptyAction& action = actions[static_cast<std::size_t>(index)];
                    const float labelWidth = std::min(inner, std::round(ImGui::CalcTextSize(action.Label.c_str(), nullptr, true).x + style.FramePadding.x * 2.0f + Px(18.0f)));
                    widths.push_back(labelWidth);
                    total += labelWidth + (action.Shortcut ? gap + ImGui::CalcTextSize(action.Shortcut).x : 0.0f);
                }
                total += Px(12.0f) * static_cast<float>(row.size() - 1);
                float x = std::round(centerX - total * 0.5f);
                for (std::size_t i = 0; i < row.size(); ++i)
                {
                    const int index = row[i];
                    const EmptyAction& action = actions[static_cast<std::size_t>(index)];
                    ImGui::SetCursorScreenPos(ImVec2(x, y));
                    ImGui::PushID(index);
                    const bool pressed = action.Accent ? AccentButton(action.Label.c_str(), ImVec2(widths[i], 0.0f)) : ImGui::Button(action.Label.c_str(), ImVec2(widths[i], 0.0f));
                    ImGui::PopID();
                    if (pressed)
                        clicked = index;
                    if (action.Tooltip)
                        ItemTooltip(action.Tooltip);
                    x += widths[i];
                    if (action.Shortcut)
                    {
                        draw->AddText(ImVec2(x + gap, y + style.FramePadding.y), ImGui::GetColorU32(ManifoldTheme::Muted), action.Shortcut);
                        x += gap + ImGui::CalcTextSize(action.Shortcut).x;
                    }
                    x += Px(12.0f);
                }
                y += button + rowGap;
            }
            y -= rowGap;
        }
        if (!footer.empty())
        {
            y += Px(12.0f);
            CenteredText(draw, centerX, y, ImGui::GetColorU32(ManifoldTheme::Muted), footer, inner);
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(available.x, std::max(available.y, max.y - origin.y)));
        return clicked;
    }

    void TableSkeleton(ImVec2 min, ImVec2 max, const std::vector<SkeletonColumn>& columns, unsigned int seed, ImVec4 background)
    {
        if (columns.empty() || max.x <= min.x || max.y <= min.y)
            return;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        const float line = ImGui::GetTextLineHeight();
        const float bar = std::round(line * 0.55f);
        const float rowHeight = std::round(line + ImGui::GetStyle().CellPadding.y * 2.0f);
        const float header = ImGui::GetFrameHeight();
        const float rounding = Px(2.0f);
        const float inset = std::round(Px(8.0f));

        float shares = 0.0f;
        for (const SkeletonColumn& column : columns)
            shares += column.Share;
        std::vector<float> starts;
        float x = min.x;
        for (const SkeletonColumn& column : columns)
        {
            starts.push_back(std::round(x));
            x += (max.x - min.x) * column.Share / shares;
        }
        starts.push_back(max.x);

        draw->AddRectFilled(min, ImVec2(max.x, min.y + header), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.03f)), Px(4.0f), ImDrawFlags_RoundCornersTop);
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            const float columnWidth = starts[i + 1] - starts[i];
            const float top = std::round(min.y + (header - bar) * 0.5f);
            const float labelWidth = std::min(Px(36.0f + static_cast<float>(SkeletonNoise(seed + static_cast<unsigned int>(i)) % 4) * 6.0f), columnWidth - inset * 2.0f);
            if (labelWidth > 0.0f)
                draw->AddRectFilled(ImVec2(starts[i] + inset, top), ImVec2(starts[i] + inset + labelWidth, top + bar), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.18f)), rounding);
            if (i > 0)
                draw->AddLine(ImVec2(starts[i], min.y), ImVec2(starts[i], max.y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.04f)), Px(1.0f));
        }

        float y = min.y + header;
        for (unsigned int row = 0; y < max.y; ++row, y += rowHeight)
        {
            if (row % 2 == 1)
                draw->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y + rowHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.012f)));
            const float top = std::round(y + (rowHeight - bar) * 0.5f);
            for (std::size_t i = 0; i < columns.size(); ++i)
            {
                const SkeletonColumn& column = columns[i];
                const ImU32 color = ImGui::GetColorU32(ManifoldTheme::WithAlpha(column.Color, column.Alpha));
                const float left = starts[i] + inset;
                if (column.Square)
                {
                    const float side = std::round(line * 0.8f);
                    const float squareTop = std::round(y + (rowHeight - side) * 0.5f);
                    draw->AddRect(ImVec2(left, squareTop), ImVec2(left + side, squareTop + side), color, Px(3.0f), 0, Px(1.0f));
                    continue;
                }
                const unsigned int noise = SkeletonNoise(seed * 977u + row * 31u + static_cast<unsigned int>(i));
                const float room = starts[i + 1] - starts[i] - inset * 2.0f;
                const float barWidth = std::round(room * (0.35f + 0.45f * static_cast<float>(noise % 100) / 100.0f));
                if (barWidth > Px(6.0f))
                    draw->AddRectFilled(ImVec2(left, top), ImVec2(left + barWidth, top + bar), color, rounding);
            }
        }

        const float fade = std::round(min.y + (max.y - min.y) * 0.3f);
        const ImU32 clear = ImGui::GetColorU32(ManifoldTheme::WithAlpha(background, 0.0f));
        const ImU32 solid = ImGui::GetColorU32(ManifoldTheme::WithAlpha(background, 0.94f));
        draw->AddRectFilledMultiColor(ImVec2(min.x, fade), max, clear, clear, solid, solid);
        draw->PopClipRect();
    }

    void BeginCard(const char* id, const char* title, ImVec2 size, ImGuiWindowFlags flags)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Card);
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(kCardRounding));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(10.0f, 8.0f));
        ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, flags);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (title && title[0])
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
        }
    }

    void EndCard()
    {
        ImGui::EndChild();
    }

    void CardTitle(const char* icon, const char* title, std::string_view note)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
        ImGui::TextUnformatted(icon);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Px(6.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (!note.empty())
        {
            ImGui::SameLine(0.0f, Px(10.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            ImGui::TextUnformatted(note.data(), note.data() + note.size());
            ImGui::PopStyleColor();
        }
    }

    bool IconButton(const char* icon, const char* tooltip, bool danger, float side)
    {
        if (side <= 0.0f)
            side = ImGui::GetFrameHeight();
        if (danger)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.32f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.45f));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        const bool clicked = ImGui::Button(icon, ImVec2(side, side));
        ImGui::PopStyleVar();
        if (danger)
            ImGui::PopStyleColor(3);
        ItemTooltip(tooltip);
        return clicked;
    }

    bool AccentButton(const char* label, ImVec2 size)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.34f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.48f));
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AccentHovered);
        const bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(4);
        return clicked;
    }

    bool DangerButton(const char* label, ImVec2 size)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.45f));
        const bool clicked = ImGui::Button(label, size);
        ImGui::PopStyleColor(3);
        return clicked;
    }

    bool ToolbarButton(const char* icon, const char* text, const char* tooltip, bool enabled)
    {
        ImGui::BeginDisabled(!enabled);
        const std::string label = std::string(icon) + "  " + text;
        const bool clicked = ImGui::Button(label.c_str());
        ImGui::EndDisabled();
        ItemTooltip(tooltip);
        return clicked && enabled;
    }

    bool DangerMenuItem(const char* label, const char* shortcut, bool enabled)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Error);
        const bool clicked = ImGui::MenuItem(label, shortcut, false, enabled);
        ImGui::PopStyleColor();
        return clicked;
    }

    void ShowTooltip(const std::function<void()>& content)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(10.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Px(8.0f, 4.0f));
        ImGui::BeginTooltip();
        content();
        ImGui::EndTooltip();
        ImGui::PopStyleVar(2);
    }

    void ItemTooltip(const char* text)
    {
        if (!text || !text[0] || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            return;
        ShowTooltip([text]()
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                ImGui::TextUnformatted(text);
                ImGui::PopTextWrapPos();
            });
    }

    bool ErrorTooltip(std::string_view message)
    {
        const ErrorInfo* info = ErrorOf(message);
        if (!info || !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            return false;
        static ErrorCode copiedCode{};
        static double copiedAt = -10.0;
        const std::string name = ErrorName(info->Code);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (clicked)
        {
            ImGui::SetClipboardText(name.c_str());
            copiedCode = info->Code;
            copiedAt = ImGui::GetTime();
        }
        const bool copied = copiedCode == info->Code && ImGui::GetTime() - copiedAt < 2.0;
        const std::string heading = name + "  " + std::string(ErrorArea(info->Code));
        const std::string footer = copied ? "Copied " + name + "." : "Click to copy " + name + ".";
        ShowTooltip([&]()
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
                ImGui::TextUnformatted(heading.c_str());
                ImGui::PopStyleColor();
                ImGui::TextUnformatted(info->Cause.data(), info->Cause.data() + info->Cause.size());
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
                ImGui::TextUnformatted(info->Help.data(), info->Help.data() + info->Help.size());
                ImGui::PopStyleColor();
                ImGui::TextDisabled("%s", footer.c_str());
                ImGui::PopTextWrapPos();
            });
        return clicked;
    }

    void TextWithError(std::string_view text, ImVec4 color)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
        ErrorTooltip(text);
    }

    void Muted(std::string_view text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    }

    void SectionLabel(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }

    namespace
    {
        struct PendingSelection
        {
            ImGuiID Id = 0;
            int Start = 0;
            int End = 0;
            int Frame = 0;
        };

        PendingSelection& Pending()
        {
            static PendingSelection pending;
            return pending;
        }

        void Refocus(ImGuiID id, int start, int end)
        {
            Pending() = { id, start, end, ImGui::GetFrameCount() };
            ImGui::ActivateItemByID(id);
        }

        void ApplyPendingSelection(ImGuiID id)
        {
            PendingSelection& pending = Pending();
            if (pending.Id != id)
                return;
            if (ImGui::GetFrameCount() - pending.Frame > 3)
            {
                pending = {};
                return;
            }
            if (ImGui::GetActiveID() != id)
                return;
            if (ImGuiInputTextState* state = ImGui::GetInputTextState(id))
            {
                state->ReloadSelectionStart = pending.Start;
                state->ReloadSelectionEnd = pending.End;
                state->WantReloadUserBuf = true;
            }
            pending = {};
        }

        std::string FilterPasted(std::string_view text, ImGuiInputTextFlags flags, bool multiline)
        {
            std::string out;
            out.reserve(text.size());
            for (char c : text)
            {
                if (c == '\r' || (!multiline && (c == '\n' || (c == '\t' && !(flags & ImGuiInputTextFlags_AllowTabInput)))))
                    continue;
                if ((flags & ImGuiInputTextFlags_CharsNoBlank) && (c == ' ' || c == '\t'))
                    continue;
                if ((flags & ImGuiInputTextFlags_CharsHexadecimal) && !std::isxdigit(static_cast<unsigned char>(c)))
                    continue;
                if ((flags & (ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_CharsScientific)) && !std::isdigit(static_cast<unsigned char>(c)) &&
                    std::string_view((flags & ImGuiInputTextFlags_CharsScientific) ? ".+-*/eE" : ".+-*/").find(c) == std::string_view::npos)
                    continue;
                if (flags & ImGuiInputTextFlags_CharsUppercase)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                out += c;
            }
            return out;
        }

        bool& SkipNextEditMenu()
        {
            static bool skip = false;
            return skip;
        }

        bool EditMenu(std::string& value, ImGuiInputTextFlags flags, bool multiline)
        {
            const bool skip = std::exchange(SkipNextEditMenu(), false);
            const ImGuiID id = ImGui::GetItemID();
            if (!id)
                return false;
            ApplyPendingSelection(id);
            if (skip)
                return false;
            if (!ImGui::BeginPopupContextItem(nullptr))
                return false;
            static ImGuiID trustedField = 0;
            if (ImGui::IsWindowAppearing())
            {
                const ImGuiContext& context = *ImGui::GetCurrentContext();
                trustedField = (context.ActiveId == id || context.ActiveIdPreviousFrame == id) ? id : 0;
            }
            const bool readOnly = (flags & ImGuiInputTextFlags_ReadOnly) != 0;
            const bool hidden = (flags & ImGuiInputTextFlags_Password) != 0;
            const int length = static_cast<int>(value.size());
            int start = length;
            int end = length;
            const ImGuiInputTextState* state = trustedField == id ? ImGui::GetInputTextState(id) : nullptr;
            if (state)
            {
                start = std::clamp(std::min(state->GetSelectionStart(), state->GetSelectionEnd()), 0, length);
                end = std::clamp(std::max(state->GetSelectionStart(), state->GetSelectionEnd()), 0, length);
                if (start == end)
                    start = end = std::clamp(state->GetCursorPos(), 0, length);
            }
            const bool selection = start != end;
            const std::size_t from = static_cast<std::size_t>(start);
            const std::size_t count = static_cast<std::size_t>(end - start);
            const char* clipboard = ImGui::GetClipboardText();
            bool changed = false;
            if (ImGui::MenuItem(ICON_FA_CUT "  Cut", "Ctrl+X", false, selection && !readOnly && !hidden))
            {
                ImGui::SetClipboardText(value.substr(from, count).c_str());
                value.erase(from, count);
                changed = true;
                Refocus(id, start, start);
            }
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, !hidden && !value.empty()))
            {
                ImGui::SetClipboardText((selection ? value.substr(from, count) : value).c_str());
                if (state)
                    Refocus(id, start, end);
            }
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, !readOnly && clipboard && *clipboard))
            {
                const std::string text = FilterPasted(clipboard, flags, multiline);
                value.replace(from, count, text);
                changed = true;
                const int caret = start + static_cast<int>(text.size());
                Refocus(id, caret, caret);
            }
            if (ImGui::MenuItem(ICON_FA_BACKSPACE "  Delete", "Del", false, selection && !readOnly))
            {
                value.erase(from, count);
                changed = true;
                Refocus(id, start, start);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Select all", "Ctrl+A", false, !value.empty()))
                Refocus(id, 0, static_cast<int>(value.size()));
            ImGui::EndPopup();
            return changed && !(flags & ImGuiInputTextFlags_EnterReturnsTrue);
        }
    }

    void SkipEditMenu()
    {
        SkipNextEditMenu() = true;
    }

    bool InputText(const char* label, std::string& value, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user)
    {
        ChainedCallback chain{ &value, callback, user };
        bool changed = ImGui::InputText(label, value.data(), value.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, &ChainCallback, &chain);
        changed |= EditMenu(value, flags, false);
        return changed;
    }

    bool InputTextWithHint(const char* label, const char* hint, std::string& value, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user)
    {
        bool changed = false;
        if (!callback)
        {
            changed = ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, &ResizeCallback, &value);
        }
        else
        {
            ChainedCallback chain{ &value, callback, user };
            changed = ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize, &ChainCallback, &chain);
        }
        changed |= EditMenu(value, flags, false);
        return changed;
    }

    bool InputMultiline(const char* label, std::string& value, ImVec2 size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user)
    {
        ChainedCallback chain{ &value, callback, user };
        bool changed = ImGui::InputTextMultiline(label, value.data(), value.capacity() + 1, size, flags | ImGuiInputTextFlags_CallbackResize, &ChainCallback, &chain);
        changed |= EditMenu(value, flags, true);
        return changed;
    }

    bool Splitter(const char* id, bool vertical, float thickness, float* first, float* second, float minFirst, float minSecond, float length)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImGuiID splitterId = window->GetID(id);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float across = length > 0.0f ? std::min(length, vertical ? available.y : available.x) : (vertical ? available.y : available.x);
        ImRect bb;
        bb.Min = ImVec2(window->DC.CursorPos.x + (vertical ? *first : 0.0f), window->DC.CursorPos.y + (vertical ? 0.0f : *first));
        bb.Max = ImVec2(bb.Min.x + (vertical ? thickness : across), bb.Min.y + (vertical ? across : thickness));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0, 0, 0, 0));
        const bool moved = ImGui::SplitterBehavior(bb, splitterId, vertical ? ImGuiAxis_X : ImGuiAxis_Y, first, second, minFirst, minSecond, 4.0f, 0.04f);
        ImGui::PopStyleColor();
        return moved;
    }

    namespace
    {
        bool AsmWordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '@' || c == '?' || c == '!';
        }

        bool AsmRegister(std::string_view word)
        {
            std::string lower(word);
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static const char* const kNames[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip",
                "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "ip", "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh", "sil", "dil", "bpl", "spl", "cs", "ds", "es", "fs", "gs", "ss", "st" };
            for (const char* name : kNames)
            {
                if (lower == name)
                    return true;
            }
            const auto digitsFrom = [&](std::size_t start, std::size_t end) {
                if (start >= end)
                    return false;
                for (std::size_t i = start; i < end; ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(lower[i])))
                        return false;
                }
                return true;
            };
            if (lower.size() >= 2 && lower[0] == 'r')
            {
                const std::size_t end = (lower.back() == 'd' || lower.back() == 'w' || lower.back() == 'b') ? lower.size() - 1 : lower.size();
                if (digitsFrom(1, end))
                    return true;
            }
            for (const char* prefix : { "xmm", "ymm", "zmm", "mm", "cr", "dr", "k", "st" })
            {
                const std::size_t length = std::char_traits<char>::length(prefix);
                if (lower.rfind(prefix, 0) == 0 && digitsFrom(length, lower.size()))
                    return true;
            }
            return false;
        }

        bool AsmSizeWord(std::string_view word)
        {
            std::string lower(word);
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const char* name : { "byte", "word", "dword", "qword", "tword", "tbyte", "fword", "oword", "xmmword", "ymmword", "zmmword", "ptr", "short", "near", "far" })
            {
                if (lower == name)
                    return true;
            }
            return false;
        }

        bool AsmNumber(std::string_view word)
        {
            std::string_view digits = word;
            if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
                digits.remove_prefix(2);
            else if (digits.size() > 1 && (digits.back() == 'h' || digits.back() == 'H'))
                digits.remove_suffix(1);
            return !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
        }
    }

    void AsmOperands(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, const ImVec4& clip, std::string_view operands)
    {
        float x = pos.x;
        std::size_t at = 0;
        while (at < operands.size())
        {
            std::size_t end = at + 1;
            ImU32 color = 0;
            if (AsmWordChar(operands[at]))
            {
                while (end < operands.size() && AsmWordChar(operands[end]))
                    ++end;
                const std::string_view word = operands.substr(at, end - at);
                if (AsmRegister(word))
                    color = ImGui::GetColorU32(ManifoldTheme::CodeRegister);
                else if (AsmSizeWord(word))
                    color = ImGui::GetColorU32(ManifoldTheme::Muted);
                else if (AsmNumber(word))
                    color = ImGui::GetColorU32(ManifoldTheme::CodeNumber);
                else
                    color = ImGui::GetColorU32(ManifoldTheme::CodeLabel);
            }
            else
            {
                color = ImGui::GetColorU32(operands[at] == ',' || operands[at] == ' ' ? ManifoldTheme::Text : ManifoldTheme::Muted);
            }
            const char* begin = operands.data() + at;
            const char* stop = operands.data() + end;
            draw->AddText(font, size, ImVec2(x, pos.y), color, begin, stop, 0.0f, &clip);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, stop).x;
            at = end;
        }
    }

    void Spinner(float size, ImVec4 color)
    {
        NoteAnimation();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
        const float radius = size * 0.5f - Px(2.0f);
        const float thickness = std::max(1.0f, Px(2.0f));
        draw->AddCircle(center, radius, ImGui::GetColorU32(ManifoldTheme::WithAlpha(color, 0.18f)), 24, thickness);
        const float start = static_cast<float>(std::fmod(ImGui::GetTime() * 6.0, 6.283185307179586));
        draw->PathArcTo(center, radius, start, start + 4.0f, 20);
        draw->PathStroke(ImGui::GetColorU32(color), 0, thickness);
    }

    float ToggleSwitchWidth()
    {
        return std::round(ImGui::GetFrameHeight() * 0.74f * 1.85f);
    }

    bool ToggleSwitch(const char* id, bool* value)
    {
        const float frame = ImGui::GetFrameHeight();
        const float height = std::round(frame * 0.74f);
        const float width = ToggleSwitchWidth();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        bool changed = false;
        if (ImGui::InvisibleButton(id, ImVec2(width, frame)))
        {
            *value = !*value;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID key = ImGui::GetItemID();
        const float target = *value ? 1.0f : 0.0f;
        float t = storage->GetFloat(key, target);
        const float step = ImGui::GetIO().DeltaTime * 9.0f;
        t = t < target ? std::min(target, t + step) : std::max(target, t - step);
        storage->SetFloat(key, t);
        const float eased = t * t * (3.0f - 2.0f * t);

        const ImVec2 min(pos.x, std::round(pos.y + (frame - height) * 0.5f));
        const ImVec2 max(min.x + width, min.y + height);
        const ImVec4 off = hovered ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
        const ImVec4 on = hovered ? ManifoldTheme::AccentHovered : ManifoldTheme::Accent;
        const ImVec4 track(off.x + (on.x - off.x) * eased, off.y + (on.y - off.y) * eased, off.z + (on.z - off.z) * eased, 1.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, max, ImGui::GetColorU32(track), height * 0.5f);
        if (eased < 0.5f)
            draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.08f * (1.0f - eased * 2.0f))), height * 0.5f);
        const float radius = height * 0.5f - Px(2.5f);
        const float left = min.x + height * 0.5f;
        const float right = max.x - height * 0.5f;
        const ImVec2 knob(left + (right - left) * eased, min.y + height * 0.5f);
        const ImVec4 knobOff = ManifoldTheme::Label;
        const ImVec4 knobOn = ManifoldTheme::Page;
        const ImVec4 knobColor(knobOff.x + (knobOn.x - knobOff.x) * eased, knobOff.y + (knobOn.y - knobOff.y) * eased, knobOff.z + (knobOn.z - knobOff.z) * eased, 1.0f);
        draw->AddCircleFilled(knob, radius, ImGui::GetColorU32(knobColor), 24);
        return changed;
    }

    bool Segmented(const char* id, int* value, const std::vector<const char*>& options, float width)
    {
        if (options.empty())
            return false;
        const float frame = ImGui::GetFrameHeight();
        const ImGuiStyle& style = ImGui::GetStyle();
        if (width <= 0.0f)
        {
            for (const char* option : options)
                width += ImGui::CalcTextSize(option, nullptr, true).x + style.FramePadding.x * 2.0f + Px(8.0f);
        }
        const float cell = std::floor(width / static_cast<float>(options.size()));
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 end(pos.x + cell * static_cast<float>(options.size()), pos.y + frame);
        const float rounding = style.FrameRounding + Px(1.0f);
        const float inset = Px(2.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, end, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);

        bool changed = false;
        ImGui::PushID(id);
        ImGui::BeginGroup();
        for (int i = 0; i < static_cast<int>(options.size()); ++i)
        {
            if (i > 0)
                ImGui::SameLine(0.0f, 0.0f);
            const ImVec2 cellMin(pos.x + cell * static_cast<float>(i), pos.y);
            const ImVec2 cellMax(cellMin.x + cell, cellMin.y + frame);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##option", ImVec2(cell, frame)) && *value != i)
            {
                *value = i;
                changed = true;
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool selected = *value == i;
            if (selected)
            {
                draw->AddRectFilled(ImVec2(cellMin.x + inset, cellMin.y + inset), ImVec2(cellMax.x - inset, cellMax.y - inset),
                    ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.20f)), rounding - inset);
                draw->AddRect(ImVec2(cellMin.x + inset, cellMin.y + inset), ImVec2(cellMax.x - inset, cellMax.y - inset),
                    ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.45f)), rounding - inset);
            }
            else if (hovered)
            {
                draw->AddRectFilled(ImVec2(cellMin.x + inset, cellMin.y + inset), ImVec2(cellMax.x - inset, cellMax.y - inset),
                    ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.05f)), rounding - inset);
            }
            const char* text = options[static_cast<std::size_t>(i)];
            const ImVec2 size = ImGui::CalcTextSize(text, nullptr, true);
            const ImVec4 clip(cellMin.x + inset, cellMin.y, cellMax.x - inset, cellMax.y);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(std::round(std::max(cellMin.x + Px(6.0f), cellMin.x + (cell - size.x) * 0.5f)), std::round(cellMin.y + (frame - size.y) * 0.5f)),
                ImGui::GetColorU32(selected ? ManifoldTheme::Accent : (hovered ? ManifoldTheme::Text : ManifoldTheme::Label)), text, ImGui::FindRenderedTextEnd(text), 0.0f, &clip);
        }
        ImGui::EndGroup();
        ImGui::PopID();
        return changed;
    }

    bool TriStateCheckbox(const char* label, int& state)
    {
        bool mixed = state == 2;
        bool checked = state == 1;
        if (mixed)
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        const bool clicked = ImGui::Checkbox(label, &checked);
        if (mixed)
            ImGui::PopItemFlag();
        if (clicked)
            state = (state + 1) % 3;
        return clicked;
    }

    bool ColorDot(const char* id, ImVec4 color, float size)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.35f, ImGui::GetColorU32(color));
        return clicked;
    }

    namespace
    {
        std::set<ImGuiID>& PendingRaises()
        {
            static std::set<ImGuiID> pending;
            return pending;
        }
    }

    bool FocusRequested()
    {
        const ImGuiContext* context = ImGui::GetCurrentContext();
        return context && (context->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasFocus) != 0;
    }

    void RequestWindowRaise(const char* title)
    {
        ImGui::SetWindowFocus(title);
        PendingRaises().insert(ImHashStr(title));
    }

    void RaisePlatformWindow(bool requested)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window)
            return;
        std::set<ImGuiID>& pending = PendingRaises();
        if (requested)
            pending.insert(window->RootWindow ? window->RootWindow->ID : window->ID);
        const ImGuiID id = window->RootWindow ? window->RootWindow->ID : window->ID;
        if (!pending.count(id))
            return;
        ImGuiViewport* viewport = window->Viewport;
        const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        if (!viewport || !platform.Platform_SetWindowFocus)
        {
            pending.erase(id);
            return;
        }
        if (!viewport->PlatformWindowCreated || !viewport->PlatformUserData)
            return;
        if (HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw))
        {
            if (::IsIconic(hwnd))
                ::ShowWindow(hwnd, SW_RESTORE);
            ::SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        platform.Platform_SetWindowFocus(viewport);
        pending.erase(id);
    }

    float CheckboxWidth(const char* label)
    {
        return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
    }

    float SmallButtonWidth(const char* label)
    {
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    bool FlowNext(float width, float indent)
    {
        const float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width;
        const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (next <= limit + 0.5f)
        {
            ImGui::SameLine();
            return true;
        }
        if (indent >= 0.0f)
            ImGui::SetCursorPosX(indent);
        return false;
    }

    void TextFitted(std::string_view text, float maxWidth, const ImVec4& color)
    {
        const char* begin = text.data();
        const char* end = begin + text.size();
        const ImVec2 size = ImGui::CalcTextSize(begin, end);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (size.x <= maxWidth || maxWidth <= 0.0f)
        {
            ImGui::TextUnformatted(begin, end);
            ImGui::PopStyleColor();
            return;
        }
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = std::max(0.0f, maxWidth);
        const ImRect rect(pos, ImVec2(pos.x + width, pos.y + size.y));
        ImGui::ItemSize(rect.GetSize());
        if (ImGui::ItemAdd(rect, 0))
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), rect.Min, rect.Max, rect.Max.x, begin, end, &size);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), begin);
    }

    bool KeepOutOfCentralDock(const char* title)
    {
        ImGuiID dockId = 0;
        if (const ImGuiWindow* window = ImGui::FindWindowByName(title))
            dockId = window->DockId;
        else if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(title)))
            dockId = settings->DockId;
        if (dockId == 0)
            return false;
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (!node || !node->IsCentralNode())
            return false;
        ImGui::SetNextWindowDockID(0);
        std::string_view name(title);
        name = name.substr(0, name.find("###"));
        while (!name.empty() && static_cast<unsigned char>(name.front()) >= 0x80)
            name.remove_prefix(1);
        LOG_INFO("Interface", "\"" + Trim(name) + "\" was docked into the middle of the main window, under its panes; it opens as its own window again");
        return true;
    }

    bool BeginToolWindow(const char* title, bool* open, ImVec2 size, ImGuiWindowFlags flags, ImVec2 minimum)
    {
        const bool raise = FocusRequested();
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        windowClass.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&windowClass);
        const ImGuiViewport* main = ImGui::GetMainViewport();
        const bool undocked = KeepOutOfCentralDock(title);
        if (!ImGui::FindWindowByName(title) || undocked)
        {
            const float step = Px(28.0f) * static_cast<float>(g_toolWindowCount++ % 8);
            ImGui::SetNextWindowPos(ImVec2(main->Pos.x + Px(80.0f) + step, main->Pos.y + Px(70.0f) + step), undocked ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        }
        ImGui::SetNextWindowSize(ImVec2(Px(size.x), Px(size.y)), undocked ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(Px(minimum.x, minimum.y), ImVec2(FLT_MAX, FLT_MAX));
        // ∑ Docking drops size constraints ("Docking currently override
        // constraints" in imgui.cpp) and its splitters only keep the style's
        // tiny WindowMinSize, so a docked tool window could be dragged down to
        // a sliver and squeeze its layout into unreadable slivers (a docked
        // Mono window at 390 px). Below its minimum the content now keeps the
        // minimum size and the window scrolls instead. Measured from last
        // frame's size, so it follows one frame late, which nobody sees.
        if (const ImGuiWindow* window = ImGui::FindWindowByName(title); window && !window->Collapsed && window->InnerRect.GetWidth() > 0.0f)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float bars = ImGui::GetFrameHeight() * ((flags & ImGuiWindowFlags_MenuBar) ? 2.0f : 1.0f);
            const ImVec2 need(Px(minimum.x) - style.WindowPadding.x * 2.0f, Px(minimum.y) - style.WindowPadding.y * 2.0f - bars);
            const ImVec2 natural(window->InnerRect.GetWidth() - window->WindowPadding.x * 2.0f, window->InnerRect.GetHeight() - window->WindowPadding.y * 2.0f);
            const ImVec2 content(natural.x + 1.0f < need.x ? need.x : 0.0f, natural.y + 1.0f < need.y ? need.y : 0.0f);
            if (content.x > 0.0f || content.y > 0.0f)
            {
                ImGui::SetNextWindowContentSize(content);   // 0 keeps that axis automatic
                if (content.x > 0.0f)
                    flags |= ImGuiWindowFlags_HorizontalScrollbar;
                flags &= ~(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            }
        }
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Page);
        const bool visible = ImGui::Begin(title, open, flags);
        ImGui::PopStyleColor();
        RaisePlatformWindow(raise);
        return visible;
    }

    void EndToolWindow()
    {
        ImGui::End();
    }

    ImGuiID CallerViewport()
    {
        const ImGuiContext* context = ImGui::GetCurrentContext();
        const ImGuiWindow* window = context ? context->CurrentWindow : nullptr;
        if (!window || window->IsFallbackWindow || !window->Viewport)
            return ImGui::GetMainViewport()->ID;
        return window->Viewport->ID;
    }

    bool BeginModal(const char* id, const char* icon, const char* title, ImVec2 size, bool* open, ImGuiID viewportId, ImGuiWindowFlags flags)
    {
        const ImGuiViewport* viewport = viewportId ? ImGui::FindViewportByID(viewportId) : nullptr;
        if (!viewport)
            viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (size.x > 0.0f || size.y > 0.0f)
            ImGui::SetNextWindowSize(Px(size.x, size.y), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(14.0f, 12.0f));
        const std::string label = std::string(icon) + "  " + title + "###" + id;
        const bool visible = ImGui::BeginPopupModal(label.c_str(), open, ImGuiWindowFlags_NoSavedSettings | flags);
        ImGui::PopStyleVar();
        return visible;
    }

    void EndModal()
    {
        ImGui::EndPopup();
    }

    Dialog::Dialog(std::string id, std::string icon, std::string title, ImVec2 size) : m_id(std::move(id)), m_icon(std::move(icon)), m_title(std::move(title)), m_size(size)
    {
    }

    void Dialog::Open(ImGuiID viewport)
    {
        m_pending = true;
        m_viewport = viewport ? viewport : CallerViewport();
    }

    bool Dialog::Begin()
    {
        if (m_pending)
        {
            ImGui::OpenPopup(("###" + m_id).c_str());
            m_pending = false;
        }
        if (m_size.y <= 0.0f)
            ImGui::SetNextWindowSizeConstraints(ImVec2(m_size.x > 0.0f ? Px(m_size.x) : 0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
        const bool autoSize = m_size.y <= 0.0f;
        const bool open = BeginModal(m_id.c_str(), m_icon.c_str(), m_title.c_str(), autoSize ? ImVec2(0.0f, 0.0f) : m_size, nullptr, m_viewport, autoSize ? ImGuiWindowFlags_AlwaysAutoResize : 0);
        m_appearing = open && ImGui::IsWindowAppearing();
        return open;
    }

    void Dialog::End()
    {
        EndModal();
    }

    void Dialog::Close()
    {
        ImGui::CloseCurrentPopup();
    }

    DialogChoice Dialog::Buttons(const char* ok, bool okEnabled, bool enterConfirms, const char* cancel)
    {
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool enter = enterConfirms && focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        const bool escape = focused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        ImGui::Spacing();
        DialogChoice choice = DialogChoice::None;
        ImGui::BeginDisabled(!okEnabled);
        if (AccentButton(ok, Px(100.0f, 0.0f)) || (enter && okEnabled))
            choice = DialogChoice::Ok;
        ImGui::EndDisabled();
        if (cancel)
        {
            ImGui::SameLine();
            if (ImGui::Button(cancel, Px(100.0f, 0.0f)) || escape)
                choice = DialogChoice::Cancel;
        }
        else if (escape)
        {
            choice = DialogChoice::Cancel;
        }
        if (choice == DialogChoice::Cancel)
            ImGui::CloseCurrentPopup();
        return choice;
    }

    bool BeginDataTable(const char* id, std::initializer_list<DataColumn> columns, ImVec2 size, ImGuiTableFlags extra)
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | extra;
        if (!ImGui::BeginTable(id, static_cast<int>(columns.size()), flags, size))
            return false;
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const DataColumn& column : columns)
        {
            if (column.Fixed > 0.0f)
                ImGui::TableSetupColumn(column.Name, ImGuiTableColumnFlags_WidthFixed, Px(column.Fixed));
            else
                ImGui::TableSetupColumn(column.Name, ImGuiTableColumnFlags_WidthStretch, column.Weight);
        }
        ImGui::TableHeadersRow();
        return true;
    }

    bool TaskStatus(std::string_view text, bool cancellable)
    {
        Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
        ImGui::SameLine(0.0f, Px(6.0f));
        ImGui::AlignTextToFramePadding();
        Muted(text);
        if (!cancellable)
            return false;
        ImGui::SameLine(0.0f, Px(8.0f));
        return ImGui::Button("Cancel");
    }

    ImVec4 LevelColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug: return ManifoldTheme::Muted;
        case LogLevel::Success: return ManifoldTheme::Success;
        case LogLevel::Warning: return ManifoldTheme::Warning;
        case LogLevel::Error: return ManifoldTheme::Error;
        default: return ManifoldTheme::Label;
        }
    }

    const char* LevelIcon(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Debug: return ICON_FA_BUG;
        case LogLevel::Success: return ICON_FA_CHECK_CIRCLE;
        case LogLevel::Warning: return ICON_FA_EXCLAMATION_TRIANGLE;
        case LogLevel::Error: return ICON_FA_TIMES_CIRCLE;
        default: return ICON_FA_INFO_CIRCLE;
        }
    }

    ImVec4 RecordColor(std::uint32_t bgr)
    {
        return ImVec4(static_cast<float>(bgr & 0xFF) / 255.0f, static_cast<float>((bgr >> 8) & 0xFF) / 255.0f, static_cast<float>((bgr >> 16) & 0xFF) / 255.0f, 1.0f);
    }

    void PushMono()
    {
        ImGui::PushFont(Fonts::Mono(), 0.0f);
    }

    void PopMono()
    {
        ImGui::PopFont();
    }
}
