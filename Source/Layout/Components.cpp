#include "Layout/Components.h"

#include "Layout/Region.h"
#include "Layout/Rules.h"
#include "UI/Theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <vector>

namespace Manifold::Layout
{
    namespace
    {
        void Hint(const char* tooltip)
        {
            if (tooltip && tooltip[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                TextTooltip(tooltip);
        }

        // ∑ a card is a child window, so what its content reports would stop
        // at it: the card opens a region inside and keeps that region's
        // minimum (plus its padding) for the next frame, in its parent window
        struct OpenCard
        {
            ImGuiID Key = 0;
            float Padding = 0.0f;
            std::unique_ptr<Region> Content;
        };

        std::vector<OpenCard>& OpenCards()
        {
            static std::vector<OpenCard> cards;
            return cards;
        }

        ImGuiID CardMinimumKey(ImGuiID id)
        {
            const int salt = 0x43617264;
            return ImHashData(&salt, sizeof(salt), id);
        }

        float WidthOf(const Size& spec, float natural, float avail)
        {
            float width = natural;
            if (spec.Kind == Size::Mode::Fill)
                width = avail;
            else if (spec.Kind == Size::Mode::Fixed)
                width = Pt(spec.Value);
            if (spec.MinPoints > 0.0f)
                width = std::max(width, Pt(spec.MinPoints));
            if (spec.MaxPoints > 0.0f)
                width = std::min(width, Pt(spec.MaxPoints));
            return width;
        }
    }

    void PushButtonColors(ButtonKind kind)
    {
        switch (kind)
        {
        case ButtonKind::Accent:
            ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.34f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.48f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::AccentHovered);
            break;
        case ButtonKind::Danger:
            ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.32f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.45f));
            break;
        case ButtonKind::Ghost:
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.08f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            break;
        case ButtonKind::Normal:
            break;
        }
    }

    void PopButtonColors(ButtonKind kind)
    {
        switch (kind)
        {
        case ButtonKind::Accent:
        case ButtonKind::Ghost:
            ImGui::PopStyleColor(4);
            break;
        case ButtonKind::Danger:
            ImGui::PopStyleColor(3);
            break;
        case ButtonKind::Normal:
            break;
        }
    }

    bool Button(std::string_view label, ButtonKind kind)
    {
        ButtonStyle style;
        style.Kind = kind;
        return Button(label, style);
    }

    bool Button(std::string_view label, const ButtonStyle& style)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        const ImGuiStyle& look = ImGui::GetStyle();
        const char* begin = label.data();
        const char* end = begin + label.size();
        const ImGuiID id = window->GetID(begin, end);
        const char* shownEnd = ImGui::FindRenderedTextEnd(begin, end);
        const std::string_view shown(begin, static_cast<std::size_t>(shownEnd - begin));
        const ImVec2 text = ImGui::CalcTextSize(begin, shownEnd, false);
        const ImVec2 pad = look.FramePadding;
        const float frame = ImGui::GetFrameHeight();
        const float natural = std::max(text.x + pad.x * 2.0f, frame);
        const float minimum = std::max(frame, std::min(natural, pad.x * 2.0f + MinTextWidth()));
        const float avail = Avail();
        float width = WidthOf(style.Width, natural, avail);
        if (Bounded())
            width = std::min(width, avail);
        width = std::max(width, minimum);
        const ImVec2 size(std::round(width), style.Height > 0.0f ? style.Height : text.y + pad.y * 2.0f);
        Report(style.Width.Kind == Size::Mode::Fixed ? std::max(minimum, width) : natural, minimum);
        const ImVec2 pos = window->DC.CursorPos;
        const ImRect box(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(size, pad.y);
        if (!ImGui::ItemAdd(box, id))
            return false;
        bool hovered = false;
        bool held = false;
        const bool pressed = ImGui::ButtonBehavior(box, id, &hovered, &held);
        PushButtonColors(style.Kind);
        const ImU32 fill = ImGui::GetColorU32(held && hovered ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
        ImGui::RenderNavCursor(box, id);
        ImGui::RenderFrame(box.Min, box.Max, fill, true, look.FrameRounding);
        const ImRect inner(ImVec2(box.Min.x + pad.x, box.Min.y + pad.y), ImVec2(box.Max.x - pad.x, box.Max.y - pad.y));
        bool truncated = false;
        if (text.x <= inner.GetWidth() + 0.5f)
            ImGui::RenderTextClipped(inner.Min, inner.Max, begin, shownEnd, &text, look.ButtonTextAlign, &box);
        else
        {
            truncated = true;
            ImFont* font = ImGui::GetFont();
            const float fontSize = ImGui::GetFontSize();
            const TextFit fit = FitText(font, fontSize, shown, inner.GetWidth());
            const ImVec4 clip(box.Min.x, box.Min.y, box.Max.x, box.Max.y);
            const float y = inner.Min.y + std::max(0.0f, (inner.GetHeight() - text.y) * look.ButtonTextAlign.y);
            const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
            window->DrawList->AddText(font, fontSize, ImVec2(inner.Min.x, y), ink, begin, begin + fit.Bytes, 0.0f, &clip);
            const float drawn = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, begin + fit.Bytes).x;
            window->DrawList->AddText(font, fontSize, ImVec2(inner.Min.x + drawn, y), ink, font->IsGlyphInFont(0x2026) ? "\xE2\x80\xA6" : "...", nullptr, 0.0f, &clip);
            WantWidth(pos.x, natural);
        }
        PopButtonColors(style.Kind);
        if (style.Tooltip)
            Hint(style.Tooltip);
        else if (truncated && style.AutoTooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
            TextTooltip(shown);
        return pressed;
    }

    bool IconButton(const char* id, const char* icon, const char* tooltip, ButtonKind kind, float side)
    {
        if (side <= 0.0f)
            side = ImGui::GetFrameHeight();
        Report(side, side);
        ImGui::PushID(id);
        PushButtonColors(kind);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        const bool clicked = ImGui::Button(icon, ImVec2(side, side));
        ImGui::PopStyleVar();
        PopButtonColors(kind);
        ImGui::PopID();
        Hint(tooltip);
        return clicked;
    }

    float LabelColumn(const std::vector<std::string_view>& labels)
    {
        float widest = 0.0f;
        for (const std::string_view label : labels)
            widest = std::max(widest, ImGui::CalcTextSize(label.data(), label.data() + label.size()).x);
        return std::min(widest, std::floor(Avail() * Current().LabelColumnShare));
    }

    float LabelColumn(std::initializer_list<std::string_view> labels)
    {
        return LabelColumn(std::vector<std::string_view>(labels));
    }

    FieldLayout Field(std::string_view label, const FieldOptions& options)
    {
        const ImGuiStyle& look = ImGui::GetStyle();
        const Rules& rules = Current();
        FieldLayout layout;
        const float avail = Avail();
        // ∑ a width of the caller's own may be below the rules' minimum; then it is the minimum
        float least = options.MinWidth > 0.0f ? options.MinWidth : Pt(rules.MinFieldWidth);
        if (options.Width > 0.0f && options.MinWidth <= 0.0f)
            least = std::min(least, options.Width);
        const float own = label.empty() ? 0.0f : ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
        const float spacing = look.ItemInnerSpacing.x * 2.0f;
        const float column = std::min(std::max(options.LabelColumn, own), std::max(Pt(24.0f), std::floor(avail * rules.LabelColumnShare)));
        LabelPlacement placement = label.empty() ? LabelPlacement::None : options.Placement;
        if (placement == LabelPlacement::Auto)
            placement = avail - column - spacing >= Pt(rules.NarrowField) ? LabelPlacement::Left : LabelPlacement::Above;
        layout.Placement = placement;
        const float labelLeast = std::min(own, MinTextWidth());
        const float reserve = std::max(0.0f, options.Reserve);   // what follows the field on its line is the field's to report
        float width = 0.0f;
        switch (placement)
        {
        case LabelPlacement::Left:
        {
            Report(column + spacing + least * 2.0f + reserve, (options.Placement == LabelPlacement::Auto ? std::max(labelLeast, least + reserve) : labelLeast + spacing + least + reserve));
            const float start = ImGui::GetCursorPosX();
            TextStyle style;
            style.MaxWidth = column;
            style.FrameLine = true;
            {
                QuietReports quiet;
                Text(label, style);
            }
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetCursorPosX(start + column + spacing);
            width = options.Width > 0.0f ? options.Width : avail - column - spacing - options.Reserve;
            break;
        }
        case LabelPlacement::Above:
            Report(std::max(own, least * 2.0f + reserve), std::max(labelLeast, least + reserve));
            {
                QuietReports quiet;
                Text(label);
            }
            width = options.Width > 0.0f ? options.Width : avail - options.Reserve;
            break;
        case LabelPlacement::Right:
            Report(least * 2.0f + look.ItemInnerSpacing.x + own + reserve, least + look.ItemInnerSpacing.x + labelLeast + reserve);
            layout.LabelInWidget = true;
            width = options.Width > 0.0f ? options.Width : avail - own - look.ItemInnerSpacing.x - options.Reserve;
            break;
        case LabelPlacement::None:
        case LabelPlacement::Auto:
            Report(least * 2.0f + reserve, least + reserve);
            width = options.Width > 0.0f ? options.Width : avail - options.Reserve;
            break;
        }
        if (options.MaxWidth > 0.0f)
            width = std::min(width, options.MaxWidth);
        width = std::max(std::floor(width), least);
        layout.Width = width;
        ImGui::SetNextItemWidth(width);
        return layout;
    }

    bool BeginCard(const char* id, const CardOptions& options)
    {
        const Rules& rules = Current();
        const ImVec2 least = Pt(rules.MinCard);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, options.Background.w > 0.0f ? options.Background : ManifoldTheme::Card);
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Pt(rules.CardRounding));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Pt(rules.CardPadding));
        const ImVec2 room = ImGui::GetContentRegionAvail();
        float width = options.Width > 0.0f ? options.Width : room.x + options.Width;
        width = std::max(width, least.x);
        float height = options.Height;
        if (height < 0.0f)
            height = std::max(room.y + height, least.y);
        else if (height > 0.0f)
            height = std::max(height, least.y);
        ImGuiChildFlags flags = ImGuiChildFlags_AlwaysUseWindowPadding;
        if (options.Border)
            flags |= ImGuiChildFlags_Borders;
        if (height == 0.0f)
            flags |= ImGuiChildFlags_AutoResizeY;
        const ImGuiID key = CardMinimumKey(ImGui::GetID(id));
        const float content = ImGui::GetStateStorage()->GetFloat(key, 0.0f);
        Report(options.Width > 0.0f ? width : std::max(least.x, room.x), std::max(least.x, content));
        const float padding = ImGui::GetStyle().WindowPadding.x * 2.0f + (options.Border ? ImGui::GetStyle().ChildBorderSize * 2.0f : 0.0f);
        const bool visible = ImGui::BeginChild(id, ImVec2(width, height), flags, options.Flags);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        // a card that scrolls sideways shows content wider than itself, so no region bounds it
        const bool scrolls = (options.Flags & ImGuiWindowFlags_HorizontalScrollbar) != 0;
        OpenCards().push_back({ key, padding, scrolls ? nullptr : std::make_unique<Region>() });
        if (visible && (!options.Title.empty() || !options.Badge.empty()))
            TitleLine(options.Icon, options.Title, options.Badge);
        return visible;
    }

    void EndCard()
    {
        std::vector<OpenCard>& cards = OpenCards();
        IM_ASSERT(!cards.empty() && "EndCard without BeginCard");
        if (cards.empty())
        {
            ImGui::EndChild();
            return;
        }
        OpenCard card = std::move(cards.back());
        cards.pop_back();
        float minimum = 0.0f;
        if (card.Content)
        {
            card.Content->End();
            minimum = card.Content->Reports().Minimum;
        }
        ImGui::EndChild();
        ImGui::GetStateStorage()->SetFloat(card.Key, minimum > 0.0f ? minimum + card.Padding : 0.0f);
    }

    Card::Card(const char* id, const CardOptions& options) : m_visible(BeginCard(id, options))
    {
    }

    Card::~Card()
    {
        End();
    }

    void Card::End()
    {
        if (m_ended)
            return;
        m_ended = true;
        EndCard();
    }

    Panel::Panel(const char* id, float width, float height, ImGuiWindowFlags flags)
        : m_card(id, CardOptions{ nullptr, {}, {}, width, height, flags, false, ManifoldTheme::Surface })
    {
    }

    Panel::~Panel() = default;

    bool RowSelectable(const char* id, bool selected, ImGuiSelectableFlags flags)
    {
        const bool clicked = ImGui::Selectable(id, selected, flags | ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
        ImGui::SameLine(0.0f, 0.0f);
        return clicked;
    }

    namespace
    {
        template <class Open>
        bool FittedTreeNode(std::string_view label, ImGuiTreeNodeFlags flags, Open&& open)
        {
            ImGuiContext& g = *GImGui;
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            const bool framed = (flags & ImGuiTreeNodeFlags_Framed) != 0;
            // ∑ where ImGui's TreeNodeBehavior starts the label: after the arrow and the padding around it
            const float offset = g.FontSize + g.Style.FramePadding.x * (framed ? 3.0f : 2.0f);
            const float start = window->DC.CursorPos.x;
            bool truncated = false;
            const std::string shown = Bounded() ? Shorten(label, Avail() - offset - (framed ? g.Style.FramePadding.x : 0.0f), &truncated) : std::string(label);
            const bool opened = open(shown.c_str());
            // the natural width still counts for windows and columns that fit their content
            const float natural = offset + ImGui::CalcTextSize(label.data(), label.data() + label.size()).x;
            window->DC.IdealMaxPos.x = std::max(window->DC.IdealMaxPos.x, start + natural);
            if (truncated && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                TextTooltip(label);
            return opened;
        }
    }

    bool TreeNode(const char* id, std::string_view label, ImGuiTreeNodeFlags flags)
    {
        return FittedTreeNode(label, flags, [&](const char* shown) { return ImGui::TreeNodeEx(id, flags, "%s", shown); });
    }

    bool TreeNode(const void* id, std::string_view label, ImGuiTreeNodeFlags flags)
    {
        return FittedTreeNode(label, flags, [&](const char* shown) { return ImGui::TreeNodeEx(id, flags, "%s", shown); });
    }

    void TitleLine(const char* icon, std::string_view title, std::string_view badge)
    {
        const float avail = Avail();
        const float iconGap = Pt(6.0f);
        const float badgeGap = Pt(10.0f);
        const float iconWidth = icon && icon[0] ? ImGui::CalcTextSize(icon).x + iconGap : 0.0f;
        const float badgeWidth = badge.empty() ? 0.0f : ImGui::CalcTextSize(badge.data(), badge.data() + badge.size()).x + badgeGap;
        const float titleWidth = ImGui::CalcTextSize(title.data(), title.data() + title.size()).x;
        // ∑ the badge stays while the title keeps a readable start beside it
        const bool showBadge = badgeWidth > 0.0f && iconWidth + std::min(titleWidth, MinTextWidth() * 2.0f) + badgeWidth <= avail;
        if (iconWidth > 0.0f)
        {
            Colored(icon, ManifoldTheme::Accent, Overflow::Visible);
            ImGui::SameLine(0.0f, iconGap);
        }
        if (!title.empty())
        {
            TextStyle style;
            style.Color = ManifoldTheme::Label;
            style.MaxWidth = std::max(1.0f, avail - iconWidth - (showBadge ? badgeWidth : 0.0f));
            Text(title, style);
        }
        if (showBadge)
        {
            if (!title.empty() || iconWidth > 0.0f)
                ImGui::SameLine(0.0f, badgeGap);
            Muted(badge);
        }
    }

    float KeyCapWidth(std::string_view key)
    {
        return ImGui::CalcTextSize(key.data(), key.data() + key.size()).x + Pt(Current().KeyCapPadding) * 2.0f;
    }

    void KeyCap(std::string_view key, const char* tooltip)
    {
        const ImVec2 text = ImGui::CalcTextSize(key.data(), key.data() + key.size());
        const ImVec2 pad(Pt(Current().KeyCapPadding), Pt(1.0f));
        const float width = text.x + pad.x * 2.0f;
        const float frame = ImGui::GetFrameHeight();
        Report(width, width);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, frame));
        const float top = std::round(at.y + (frame - text.y) * 0.5f - pad.y);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        draw->AddRect(ImVec2(at.x, top), ImVec2(at.x + width, top + text.y + pad.y * 2.0f), ink, Pt(3.0f));
        draw->AddText(ImVec2(at.x + pad.x, top + pad.y), ink, key.data(), key.data() + key.size());
        Hint(tooltip);
    }

    void Badge(std::string_view text, ImVec4 color)
    {
        const ImVec2 size = ImGui::CalcTextSize(text.data(), text.data() + text.size());
        const ImVec2 pad(Pt(6.0f), Pt(1.0f));
        const ImVec2 box(size.x + pad.x * 2.0f, size.y + pad.y * 2.0f);
        Report(box.x, box.x);
        if (FrameLine())
            ImGui::AlignTextToFramePadding();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy(box);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + box.x, at.y + box.y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(color, 0.16f)), box.y * 0.5f);
        draw->AddText(ImVec2(at.x + pad.x, at.y + pad.y), ImGui::GetColorU32(color), text.data(), text.data() + text.size());
    }
}
