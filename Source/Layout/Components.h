#pragma once

#include "Layout/Containers.h"
#include "Layout/Typography.h"

#include "imgui.h"

#include <initializer_list>
#include <string_view>
#include <vector>

namespace Manifold::Layout
{
    enum class ButtonKind
    {
        Normal,
        Accent,
        Danger,
        Ghost
    };

    struct ButtonStyle
    {
        ButtonKind Kind = ButtonKind::Normal;
        Size Width = Size::Fit();     // Fit: its text; Fill: the place's width; Fixed: that many points
        float Height = 0.0f;          // pixels; 0: a frame's height
        const char* Tooltip = nullptr;
        bool AutoTooltip = true;      // the whole text as a tooltip when it was shortened (off when the caller shows its own)
    };

    // ∑ A button whose text never leaves it: narrower than its text, it ends
    // in "..." and shows the text as a tooltip. Never narrower than its
    // padding and a few characters, never wider than its place (a Fixed or Fit
    // width shrinks to the place before the text is shortened). The label may
    // carry an ImGui ID after "##".
    bool Button(std::string_view label, const ButtonStyle& style = {});
    bool Button(std::string_view label, ButtonKind kind);

    // ∑ The ID comes from id, not from the glyph, so two buttons with the
    // same icon in one window never share an ID (ImGui would draw its
    // conflict overlay over them).
    bool IconButton(const char* id, const char* icon, const char* tooltip, ButtonKind kind = ButtonKind::Normal, float side = 0.0f);

    void PushButtonColors(ButtonKind kind);
    void PopButtonColors(ButtonKind kind);

    enum class LabelPlacement
    {
        Auto,    // left, or above when the line is too narrow for a readable field beside it
        Left,
        Above,
        Right,
        None
    };

    struct FieldOptions
    {
        LabelPlacement Placement = LabelPlacement::Auto;
        float LabelColumn = 0.0f;     // pixels; the shared label column of the container (0: the label's own width)
        float Width = 0.0f;           // pixels; 0: the rest of the line
        float MinWidth = 0.0f;        // pixels; 0: the rules' field minimum
        float MaxWidth = 0.0f;        // pixels; 0: none
        float Reserve = 0.0f;         // pixels kept free after the field on its line (key caps and such)
    };

    struct FieldLayout
    {
        float Width = 0.0f;           // what the field gets; SetNextItemWidth was called with it
        LabelPlacement Placement = LabelPlacement::None;
        bool LabelInWidget = false;   // Right: the widget draws the label itself, keep it in the ImGui label
    };

    // Draws the label where it goes and sets the next item's width. Call the ImGui widget right after with a label
    // that only carries its ID ("##name"), unless LabelInWidget.
    FieldLayout Field(std::string_view label, const FieldOptions& options = {});
    // the label column of a container: its widest label, but at most the rules' share of the line
    float LabelColumn(const std::vector<std::string_view>& labels);
    float LabelColumn(std::initializer_list<std::string_view> labels);

    struct CardOptions
    {
        const char* Icon = nullptr;
        std::string_view Title;
        std::string_view Badge;
        float Width = 0.0f;           // pixels; 0 or less: the place's width less that much (ImGui's way)
        float Height = 0.0f;          // pixels; 0: its content's height; below 0: what is left of the place less that much
        ImGuiWindowFlags Flags = 0;
        bool Border = true;
        ImVec4 Background = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);   // alpha 0: the theme's card colour
    };

    // ∑ A card: a child window with the rules' padding and rounding that
    // clips its content, a title line (icon, title shortened with "...", a
    // badge that is left out first when the line is short), never smaller than
    // the rules' card minimum. Draw the content when Visible().
    class Card
    {
    public:
        Card(const char* id, const CardOptions& options = {});
        ~Card();
        Card(const Card&) = delete;
        Card& operator=(const Card&) = delete;
        bool Visible() const { return m_visible; }
        void End();

    private:
        bool m_visible = false;
        bool m_ended = false;
    };

    // the same as calls that pair, for places that cannot keep a Card object; EndCard always (like EndChild)
    bool BeginCard(const char* id, const CardOptions& options = {});
    void EndCard();

    // a card without border or title on the child-area colour
    class Panel
    {
    public:
        Panel(const char* id, float width = 0.0f, float height = 0.0f, ImGuiWindowFlags flags = 0);
        ~Panel();
        Panel(const Panel&) = delete;
        Panel& operator=(const Panel&) = delete;
        bool Visible() const { return m_card.Visible(); }

    private:
        Card m_card;
    };

    // ∑ A table row's selectable over all its columns, with the cursor left
    // at the start of the cell, so the cell's text (a Layout text, which ends
    // in "..." when the column is narrow) is drawn over it.
    bool RowSelectable(const char* id, bool selected, ImGuiSelectableFlags flags = 0);

    // the title line a card draws, for places that draw their own frame
    void TitleLine(const char* icon, std::string_view title, std::string_view badge = {});

    // a key in a frame ("F1"), on the line of the widget before it
    void KeyCap(std::string_view key, const char* tooltip = nullptr);
    float KeyCapWidth(std::string_view key);
    // a small coloured tag
    void Badge(std::string_view text, ImVec4 color);
}
