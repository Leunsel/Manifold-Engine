#include "Layout/Typography.h"

#include "Layout/Region.h"
#include "Layout/Rules.h"
#include "UI/Theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <vector>

namespace Manifold::Layout
{
    namespace
    {
        const char* EllipsisText(ImFont* font)
        {
            return font && font->IsGlyphInFont(0x2026) ? "\xE2\x80\xA6" : "...";
        }

        float LineWidth(ImFont* font, float size, const char* begin, const char* end)
        {
            return font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end).x;
        }

        std::vector<std::string_view> Lines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t start = 0;
            for (;;)
            {
                const std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                {
                    lines.push_back(text.substr(start));
                    break;
                }
                lines.push_back(text.substr(start, end - start));
                start = end + 1;
            }
            return lines;
        }
    }

    TextFit FitText(ImFont* font, float size, std::string_view text, float width)
    {
        TextFit fit;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const float full = LineWidth(font, size, begin, end);
        if (full <= width + 0.01f)
        {
            fit.Bytes = text.size();
            fit.Width = full;
            return fit;
        }
        fit.Truncated = true;
        const char* ellipsis = EllipsisText(font);
        const float dots = LineWidth(font, size, ellipsis, nullptr);
        std::vector<std::size_t> cuts;   // byte offsets that start a character, and the end
        for (std::size_t at = 0; at < text.size(); ++at)
        {
            if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80)
                cuts.push_back(at);
        }
        cuts.push_back(text.size());
        std::size_t low = 0;
        std::size_t high = cuts.size() - 1;
        while (low < high)
        {
            const std::size_t middle = (low + high + 1) / 2;
            if (LineWidth(font, size, begin, begin + cuts[middle]) + dots <= width)
                low = middle;
            else
                high = middle - 1;
        }
        std::size_t bytes = cuts[low];
        while (bytes > 0 && text[bytes - 1] == ' ')
            --bytes;
        fit.Bytes = bytes;
        fit.Width = LineWidth(font, size, begin, begin + bytes) + dots;
        return fit;
    }

    void TextTooltip(std::string_view text)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Pt(ImVec2(10.0f, 8.0f)));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Pt(ImVec2(8.0f, 4.0f)));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * Current().TooltipWidthEm);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleVar(2);
    }

    bool Text(std::string_view text, const TextStyle& style)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        const bool font = style.Font || style.FontSize > 0.0f;
        if (font)
            ImGui::PushFont(style.Font, style.FontSize);
        const bool color = style.Color.w > 0.0f;
        if (color)
            ImGui::PushStyleColor(ImGuiCol_Text, style.Color);
        if (style.FrameLine || FrameLine())
            ImGui::AlignTextToFramePadding();

        Overflow mode = style.Mode;
        if (mode == Overflow::Ellipsis && window->DC.TextWrapPos >= 0.0f)
            mode = Overflow::Wrap;   // the caller asked for wrapping around it
        const bool bounded = Bounded() || style.MaxWidth > 0.0f;
        if (!bounded && mode != Overflow::Wrap)
            mode = Overflow::Visible;
        float limit = Avail();
        if (style.MaxWidth > 0.0f)
            limit = Bounded() ? std::min(limit, style.MaxWidth) : style.MaxWidth;

        const char* begin = text.data();
        const char* end = begin + text.size();
        const float natural = ImGui::CalcTextSize(begin, end).x;
        const float minimum = std::min(natural, MinTextWidth());
        bool truncated = false;

        if (mode == Overflow::Wrap)
        {
            Report(natural, minimum);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(limit, 1.0f));
            ImGui::TextUnformatted(begin, end);
            ImGui::PopTextWrapPos();
        }
        else if (mode == Overflow::Visible || natural <= limit + 0.01f)
        {
            Report(natural, mode == Overflow::Visible ? natural : minimum);
            const float free = limit - natural;
            if (style.Justify != Align::Start && free > 0.0f && mode != Overflow::Visible)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor(style.Justify == Align::End ? free : free * 0.5f));
            ImGui::TextUnformatted(begin, end);
        }
        else
        {
            Report(natural, minimum);
            truncated = true;
            ImFont* current = ImGui::GetFont();
            const float size = ImGui::GetFontSize();
            const std::vector<std::string_view> lines = Lines(text);
            const float line = ImGui::GetTextLineHeight();
            const ImVec2 pos = window->DC.CursorPos;
            const ImVec2 extent(std::max(0.0f, limit), line * static_cast<float>(lines.size()));
            const ImRect rect(pos, ImVec2(pos.x + extent.x, pos.y + extent.y));
            ImGui::ItemSize(extent, 0.0f);
            if (ImGui::ItemAdd(rect, 0))
            {
                const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
                const ImVec4 clip(rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y);
                float y = pos.y;
                for (const std::string_view part : lines)
                {
                    if (mode == Overflow::Clip)
                        window->DrawList->AddText(current, size, ImVec2(pos.x, y), ink, part.data(), part.data() + part.size(), 0.0f, &clip);
                    else
                    {
                        const TextFit fit = FitText(current, size, part, limit);
                        window->DrawList->AddText(current, size, ImVec2(pos.x, y), ink, part.data(), part.data() + fit.Bytes, 0.0f, &clip);
                        if (fit.Truncated)
                        {
                            const float shown = current->CalcTextSizeA(size, FLT_MAX, 0.0f, part.data(), part.data() + fit.Bytes).x;
                            window->DrawList->AddText(current, size, ImVec2(pos.x + shown, y), ink, EllipsisText(current), nullptr, 0.0f, &clip);
                        }
                    }
                    y += line;
                }
            }
            WantWidth(pos.x, natural);
            if (style.Tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                TextTooltip(text);
        }
        if (color)
            ImGui::PopStyleColor();
        if (font)
            ImGui::PopFont();
        return truncated;
    }

    bool Muted(std::string_view text, Overflow overflow)
    {
        TextStyle style;
        style.Color = ManifoldTheme::Muted;
        style.Mode = overflow;
        return Text(text, style);
    }

    bool Label(std::string_view text, Overflow overflow)
    {
        TextStyle style;
        style.Color = ManifoldTheme::Label;
        style.Mode = overflow;
        return Text(text, style);
    }

    bool Heading(std::string_view text, Overflow overflow)
    {
        TextStyle style;
        style.Font = Current().Bold;
        style.Mode = overflow;
        return Text(text, style);
    }

    bool Colored(std::string_view text, ImVec4 color, Overflow overflow)
    {
        TextStyle style;
        style.Color = color;
        style.Mode = overflow;
        return Text(text, style);
    }

    bool Wrapped(std::string_view text, ImVec4 color)
    {
        TextStyle style;
        style.Color = color;
        style.Mode = Overflow::Wrap;
        return Text(text, style);
    }
}
