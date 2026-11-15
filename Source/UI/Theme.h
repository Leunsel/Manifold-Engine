#pragma once

#include "imgui.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#define MANIFOLD_THEME_PALETTE(X) \
    X(Accent, "Accent", 0.18f, 0.86f, 0.62f, 1.00f) \
    X(AccentHovered, "Accent, hovered", 0.26f, 0.94f, 0.70f, 1.00f) \
    X(Sidebar, "Sidebar", 0.043f, 0.046f, 0.054f, 1.00f) \
    X(Page, "Window background", 0.058f, 0.061f, 0.071f, 1.00f) \
    X(Card, "Cards and notices", 0.082f, 0.086f, 0.100f, 1.00f) \
    X(CardBorder, "Card border", 1.00f, 1.00f, 1.00f, 0.055f) \
    X(RowSeparator, "Row separator", 1.00f, 1.00f, 1.00f, 0.045f) \
    X(Border, "Border", 0.20f, 0.21f, 0.24f, 0.80f) \
    X(Text, "Text", 0.90f, 0.91f, 0.93f, 1.00f) \
    X(Label, "Labels", 0.78f, 0.79f, 0.82f, 1.00f) \
    X(Muted, "Muted text", 0.50f, 0.52f, 0.56f, 1.00f) \
    X(Warning, "Warning", 0.95f, 0.70f, 0.32f, 1.00f) \
    X(Error, "Error", 0.95f, 0.42f, 0.36f, 1.00f) \
    X(Changed, "Changed value", 0.98f, 0.38f, 0.38f, 1.00f) \
    X(AddressText, "Addresses", 0.55f, 0.76f, 0.98f, 1.00f) \
    X(BytesText, "Bytes", 0.58f, 0.60f, 0.64f, 1.00f) \
    X(CodeKeyword, "Code: keywords", 0.80f, 0.56f, 0.98f, 1.00f) \
    X(CodeRegister, "Code: registers", 0.45f, 0.82f, 0.98f, 1.00f) \
    X(CodeNumber, "Code: numbers", 0.98f, 0.78f, 0.45f, 1.00f) \
    X(CodeString, "Code: strings", 0.62f, 0.88f, 0.52f, 1.00f) \
    X(CodeComment, "Code: comments", 0.45f, 0.50f, 0.55f, 1.00f) \
    X(CodeSection, "Code: sections", 0.98f, 0.55f, 0.35f, 1.00f) \
    X(CodeLabel, "Code: labels", 0.98f, 0.92f, 0.55f, 1.00f) \
    X(FlowJump, "Disassembler: jumps", 0.98f, 0.70f, 0.40f, 1.00f) \
    X(FlowCall, "Disassembler: calls", 0.55f, 0.85f, 0.98f, 1.00f) \
    X(FlowReturn, "Disassembler: returns", 0.95f, 0.50f, 0.50f, 1.00f) \
    X(Breakpoint, "Breakpoint", 0.92f, 0.26f, 0.30f, 1.00f) \
    X(CurrentLine, "Current line", 0.18f, 0.86f, 0.62f, 0.22f) \
    X(Surface, "Child areas", 0.075f, 0.080f, 0.092f, 1.00f) \
    X(Frame, "Input fields", 0.13f, 0.14f, 0.16f, 1.00f) \
    X(Button, "Buttons", 0.15f, 0.16f, 0.18f, 1.00f) \
    X(Popup, "Popups and menus", 0.09f, 0.095f, 0.11f, 0.98f) \
    X(TitleBar, "Title bars", 0.08f, 0.085f, 0.10f, 1.00f)

namespace ManifoldTheme
{
#define MANIFOLD_THEME_DEFAULT(name, label, r, g, b, a) inline constexpr ImVec4 Default##name = ImVec4(r, g, b, a);
#define MANIFOLD_THEME_VALUE(name, label, r, g, b, a) inline ImVec4 name = Default##name;
    MANIFOLD_THEME_PALETTE(MANIFOLD_THEME_DEFAULT)
    MANIFOLD_THEME_PALETTE(MANIFOLD_THEME_VALUE)
#undef MANIFOLD_THEME_DEFAULT
#undef MANIFOLD_THEME_VALUE

    struct PaletteColor
    {
        const char* Name;
        const char* Label;
        ImVec4* Value;
        ImVec4 Default;
    };

#define MANIFOLD_THEME_ENTRY(name, label, r, g, b, a) PaletteColor{ #name, label, &name, Default##name },
    inline const PaletteColor kPalette[] = { MANIFOLD_THEME_PALETTE(MANIFOLD_THEME_ENTRY) };
#undef MANIFOLD_THEME_ENTRY

    struct StyleSize
    {
        const char* Name;
        const char* Label;
        float ImGuiStyle::* Scalar;
        ImVec2 ImGuiStyle::* Pair;
        int Component;
        float Default;
        float Min;
        float Max;
    };

    inline const StyleSize kSizes[] = {
        { "FrameRounding", "Frame rounding", &ImGuiStyle::FrameRounding, nullptr, 0, 3.0f, 0.0f, 12.0f },
        { "ChildRounding", "Child rounding", &ImGuiStyle::ChildRounding, nullptr, 0, 4.0f, 0.0f, 12.0f },
        { "PopupRounding", "Popup rounding", &ImGuiStyle::PopupRounding, nullptr, 0, 4.0f, 0.0f, 12.0f },
        { "GrabRounding", "Slider rounding", &ImGuiStyle::GrabRounding, nullptr, 0, 3.0f, 0.0f, 12.0f },
        { "TabRounding", "Tab rounding", &ImGuiStyle::TabRounding, nullptr, 0, 3.0f, 0.0f, 12.0f },
        { "ScrollbarRounding", "Scrollbar rounding", &ImGuiStyle::ScrollbarRounding, nullptr, 0, 6.0f, 0.0f, 12.0f },
        { "ScrollbarSize", "Scrollbar width", &ImGuiStyle::ScrollbarSize, nullptr, 0, 11.0f, 6.0f, 24.0f },
        { "GrabMinSize", "Slider grab size", &ImGuiStyle::GrabMinSize, nullptr, 0, 9.0f, 4.0f, 24.0f },
        { "FramePadding.x", "Frame padding, horizontal", nullptr, &ImGuiStyle::FramePadding, 0, 8.0f, 0.0f, 20.0f },
        { "FramePadding.y", "Frame padding, vertical", nullptr, &ImGuiStyle::FramePadding, 1, 5.0f, 0.0f, 20.0f },
        { "ItemSpacing.x", "Item spacing, horizontal", nullptr, &ImGuiStyle::ItemSpacing, 0, 8.0f, 0.0f, 20.0f },
        { "ItemSpacing.y", "Item spacing, vertical", nullptr, &ImGuiStyle::ItemSpacing, 1, 7.0f, 0.0f, 20.0f },
        { "ItemInnerSpacing.x", "Inner spacing, horizontal", nullptr, &ImGuiStyle::ItemInnerSpacing, 0, 6.0f, 0.0f, 20.0f },
        { "ItemInnerSpacing.y", "Inner spacing, vertical", nullptr, &ImGuiStyle::ItemInnerSpacing, 1, 4.0f, 0.0f, 20.0f },
        { "CellPadding.x", "Table cell padding, horizontal", nullptr, &ImGuiStyle::CellPadding, 0, 6.0f, 0.0f, 20.0f },
        { "CellPadding.y", "Table cell padding, vertical", nullptr, &ImGuiStyle::CellPadding, 1, 4.0f, 0.0f, 20.0f },
        { "WindowPadding.x", "Window padding, horizontal", nullptr, &ImGuiStyle::WindowPadding, 0, 8.0f, 0.0f, 24.0f },
        { "WindowPadding.y", "Window padding, vertical", nullptr, &ImGuiStyle::WindowPadding, 1, 8.0f, 0.0f, 24.0f },
        { "IndentSpacing", "Indent", &ImGuiStyle::IndentSpacing, nullptr, 0, 21.0f, 0.0f, 40.0f },
        { "WindowBorderSize", "Window border", &ImGuiStyle::WindowBorderSize, nullptr, 0, 1.0f, 0.0f, 2.0f },
        { "ChildBorderSize", "Child border", &ImGuiStyle::ChildBorderSize, nullptr, 0, 1.0f, 0.0f, 2.0f },
        { "PopupBorderSize", "Popup border", &ImGuiStyle::PopupBorderSize, nullptr, 0, 1.0f, 0.0f, 2.0f },
        { "FrameBorderSize", "Frame border", &ImGuiStyle::FrameBorderSize, nullptr, 0, 0.0f, 0.0f, 2.0f },
        { "DisabledAlpha", "Disabled items opacity", &ImGuiStyle::DisabledAlpha, nullptr, 0, 0.6f, 0.1f, 1.0f },
    };

    inline float& SizeOf(ImGuiStyle& style, const StyleSize& size)
    {
        return size.Scalar ? style.*size.Scalar : (size.Component == 0 ? (style.*size.Pair).x : (style.*size.Pair).y);
    }

    inline std::string LowerName(std::string_view name)
    {
        std::string lower(name);
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lower;
    }

    inline constexpr const char* kImGuiPrefix = "imgui.";
    inline std::map<std::string, ImVec4> g_ColorOverrides;
    inline std::map<std::string, float> g_SizeOverrides;

    inline ImVec4 WithAlpha(ImVec4 color, float alpha)
    {
        color.w = alpha;
        return color;
    }

    inline int ImGuiColorIndex(std::string_view name)
    {
        const std::string wanted = LowerName(name);
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            if (LowerName(ImGui::GetStyleColorName(i)) == wanted)
                return i;
        }
        return -1;
    }

    inline void Configure(const std::map<std::string, std::array<float, 4>>& colors, const std::map<std::string, float>& sizes)
    {
        for (const PaletteColor& color : kPalette)
            *color.Value = color.Default;
        g_ColorOverrides.clear();
        g_SizeOverrides.clear();
        for (const auto& [name, rgba] : colors)
        {
            const ImVec4 value(rgba[0], rgba[1], rgba[2], rgba[3]);
            const std::string lower = LowerName(name);
            if (lower.rfind(kImGuiPrefix, 0) == 0)
            {
                const std::string imgui = lower.substr(std::string_view(kImGuiPrefix).size());
                if (ImGuiColorIndex(imgui) >= 0)
                    g_ColorOverrides[imgui] = value;
                continue;
            }
            for (const PaletteColor& color : kPalette)
            {
                if (LowerName(color.Name) == lower)
                {
                    *color.Value = value;
                    break;
                }
            }
        }
        for (const auto& [name, value] : sizes)
        {
            const std::string lower = LowerName(name);
            for (const StyleSize& size : kSizes)
            {
                if (LowerName(size.Name) == lower)
                {
                    g_SizeOverrides[lower] = value < size.Min ? size.Min : value > size.Max ? size.Max : value;
                    break;
                }
            }
        }
    }

    inline void Apply(ImGuiStyle& style)
    {
        ImVec4* colors = style.Colors;

        colors[ImGuiCol_Text]                   = Text;
        colors[ImGuiCol_TextDisabled]           = Muted;
        colors[ImGuiCol_WindowBg]               = ImVec4(Page.x * 0.95f, Page.y * 0.95f, Page.z * 0.96f, 1.00f);
        colors[ImGuiCol_ChildBg]                = Surface;
        colors[ImGuiCol_PopupBg]                = Popup;
        colors[ImGuiCol_Border]                 = Border;
        colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]                = Frame;
        colors[ImGuiCol_FrameBgHovered]         = ImVec4(Frame.x + 0.04f, Frame.y + 0.04f, Frame.z + 0.05f, Frame.w);
        colors[ImGuiCol_FrameBgActive]          = ImVec4(Frame.x + 0.07f, Frame.y + 0.08f, Frame.z + 0.09f, Frame.w);
        colors[ImGuiCol_TitleBg]                = ImVec4(TitleBar.x * 0.75f, TitleBar.y * 0.76f, TitleBar.z * 0.75f, 1.00f);
        colors[ImGuiCol_TitleBgActive]          = TitleBar;
        colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(TitleBar.x * 0.62f, TitleBar.y * 0.59f, TitleBar.z * 0.60f, 0.60f);
        colors[ImGuiCol_MenuBarBg]              = TitleBar;
        colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.24f, 0.25f, 0.28f, 0.80f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.32f, 0.33f, 0.37f, 0.90f);
        colors[ImGuiCol_ScrollbarGrabActive]    = WithAlpha(Accent, 0.70f);
        colors[ImGuiCol_CheckMark]              = Accent;
        colors[ImGuiCol_SliderGrab]             = WithAlpha(Accent, 0.85f);
        colors[ImGuiCol_SliderGrabActive]       = Accent;
        colors[ImGuiCol_Button]                 = Button;
        colors[ImGuiCol_ButtonHovered]          = ImVec4(Button.x + 0.05f, Button.y + 0.06f, Button.z + 0.07f, Button.w);
        colors[ImGuiCol_ButtonActive]           = WithAlpha(Accent, 0.35f);
        colors[ImGuiCol_Header]                 = WithAlpha(Accent, 0.16f);
        colors[ImGuiCol_HeaderHovered]          = WithAlpha(Accent, 0.24f);
        colors[ImGuiCol_HeaderActive]           = WithAlpha(Accent, 0.32f);
        colors[ImGuiCol_Separator]              = Border;
        colors[ImGuiCol_SeparatorHovered]       = WithAlpha(Accent, 0.55f);
        colors[ImGuiCol_SeparatorActive]        = Accent;
        colors[ImGuiCol_ResizeGrip]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_ResizeGripHovered]      = WithAlpha(Accent, 0.45f);
        colors[ImGuiCol_ResizeGripActive]       = WithAlpha(Accent, 0.80f);
        colors[ImGuiCol_Tab]                    = ImVec4(0.10f, 0.105f, 0.12f, 1.00f);
        colors[ImGuiCol_TabHovered]             = WithAlpha(Accent, 0.24f);
        colors[ImGuiCol_TabSelected]            = WithAlpha(Accent, 0.18f);
        colors[ImGuiCol_TabDimmed]              = ImVec4(0.09f, 0.095f, 0.11f, 1.00f);
        colors[ImGuiCol_TabDimmedSelected]      = ImVec4(0.12f, 0.13f, 0.15f, 1.00f);
        colors[ImGuiCol_PlotLines]              = ImVec4(0.65f, 0.67f, 0.70f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered]       = Accent;
        colors[ImGuiCol_PlotHistogram]          = WithAlpha(Accent, 0.80f);
        colors[ImGuiCol_PlotHistogramHovered]   = Accent;
        colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.10f, 0.105f, 0.12f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.18f, 0.19f, 0.22f, 1.00f);
        colors[ImGuiCol_TableBorderLight]       = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
        colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);
        colors[ImGuiCol_TextSelectedBg]         = WithAlpha(Accent, 0.30f);
        colors[ImGuiCol_DragDropTarget]         = Accent;
        colors[ImGuiCol_NavCursor]              = WithAlpha(Accent, 0.80f);
        colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);

        style.WindowRounding    = 6.0f;
        style.WindowBorderSize  = 1.0f;
        style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
        for (const StyleSize& size : kSizes)
            SizeOf(style, size) = size.Default;

        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const auto found = g_ColorOverrides.find(LowerName(ImGui::GetStyleColorName(i)));
            if (found != g_ColorOverrides.end())
                colors[i] = found->second;
        }
        for (const StyleSize& size : kSizes)
        {
            const auto found = g_SizeOverrides.find(LowerName(size.Name));
            if (found != g_SizeOverrides.end())
                SizeOf(style, size) = found->second;
        }
    }
}
