#pragma once

#include "App/Settings.h"

#include "imgui.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#define MANIFOLD_THEME_PALETTE(X) \
    X(Accent, "Accent") \
    X(AccentHovered, "Accent, hovered") \
    X(Success, "Success") \
    X(Sidebar, "Sidebar") \
    X(Page, "Window background") \
    X(Card, "Cards and notices") \
    X(CardBorder, "Card border") \
    X(RowSeparator, "Row separator") \
    X(Border, "Border") \
    X(Text, "Text") \
    X(Label, "Labels") \
    X(Muted, "Muted text") \
    X(Warning, "Warning") \
    X(Error, "Error") \
    X(Changed, "Changed value") \
    X(AddressText, "Addresses") \
    X(BytesText, "Bytes") \
    X(CodeKeyword, "Code: keywords") \
    X(CodeRegister, "Code: registers") \
    X(CodeNumber, "Code: numbers") \
    X(CodeString, "Code: strings") \
    X(CodeComment, "Code: comments") \
    X(CodeSection, "Code: sections") \
    X(CodeLabel, "Code: labels") \
    X(FlowJump, "Disassembler: jumps") \
    X(FlowCall, "Disassembler: calls") \
    X(FlowReturn, "Disassembler: returns") \
    X(Breakpoint, "Breakpoint") \
    X(CurrentLine, "Current line") \
    X(Surface, "Child areas") \
    X(Frame, "Input fields") \
    X(Button, "Buttons") \
    X(Popup, "Popups and menus") \
    X(TitleBar, "Title bars") \
    X(Shadow, "Shadows")

namespace ManifoldTheme
{
    using Manifold::ThemeFamily;
    using Manifold::ThemeMode;

    struct Palette
    {
#define MANIFOLD_THEME_FIELD(name, label) ImVec4 name;
        MANIFOLD_THEME_PALETTE(MANIFOLD_THEME_FIELD)
#undef MANIFOLD_THEME_FIELD
    };

    constexpr ImVec4 C(float r, float g, float b, float a = 1.0f)
    {
        return ImVec4(r, g, b, a);
    }

    inline constexpr Palette kStandardDark{
        .Accent = C(0.18f, 0.86f, 0.62f), .AccentHovered = C(0.26f, 0.94f, 0.70f), .Success = C(0.18f, 0.86f, 0.62f),
        .Sidebar = C(0.043f, 0.046f, 0.054f), .Page = C(0.058f, 0.061f, 0.071f), .Card = C(0.082f, 0.086f, 0.100f),
        .CardBorder = C(1.00f, 1.00f, 1.00f, 0.055f), .RowSeparator = C(1.00f, 1.00f, 1.00f, 0.045f), .Border = C(0.20f, 0.21f, 0.24f, 0.80f),
        .Text = C(0.90f, 0.91f, 0.93f), .Label = C(0.78f, 0.79f, 0.82f), .Muted = C(0.50f, 0.52f, 0.56f),
        .Warning = C(0.95f, 0.70f, 0.32f), .Error = C(0.95f, 0.42f, 0.36f), .Changed = C(0.98f, 0.38f, 0.38f),
        .AddressText = C(0.55f, 0.76f, 0.98f), .BytesText = C(0.58f, 0.60f, 0.64f),
        .CodeKeyword = C(0.80f, 0.56f, 0.98f), .CodeRegister = C(0.45f, 0.82f, 0.98f), .CodeNumber = C(0.98f, 0.78f, 0.45f),
        .CodeString = C(0.62f, 0.88f, 0.52f), .CodeComment = C(0.45f, 0.50f, 0.55f), .CodeSection = C(0.98f, 0.55f, 0.35f),
        .CodeLabel = C(0.98f, 0.92f, 0.55f), .FlowJump = C(0.98f, 0.70f, 0.40f), .FlowCall = C(0.55f, 0.85f, 0.98f),
        .FlowReturn = C(0.95f, 0.50f, 0.50f), .Breakpoint = C(0.92f, 0.26f, 0.30f), .CurrentLine = C(0.18f, 0.86f, 0.62f, 0.22f),
        .Surface = C(0.075f, 0.080f, 0.092f), .Frame = C(0.13f, 0.14f, 0.16f), .Button = C(0.15f, 0.16f, 0.18f),
        .Popup = C(0.09f, 0.095f, 0.11f, 0.98f), .TitleBar = C(0.08f, 0.085f, 0.10f), .Shadow = C(0.00f, 0.00f, 0.00f, 0.27f),
    };

    inline constexpr Palette kStandardLight{
        .Accent = C(0.05f, 0.56f, 0.41f), .AccentHovered = C(0.04f, 0.49f, 0.36f), .Success = C(0.05f, 0.56f, 0.41f),
        .Sidebar = C(0.918f, 0.925f, 0.937f), .Page = C(0.957f, 0.961f, 0.969f), .Card = C(1.000f, 1.000f, 1.000f),
        .CardBorder = C(0.08f, 0.10f, 0.14f, 0.10f), .RowSeparator = C(0.08f, 0.10f, 0.14f, 0.07f), .Border = C(0.79f, 0.80f, 0.83f, 0.95f),
        .Text = C(0.11f, 0.12f, 0.15f), .Label = C(0.26f, 0.28f, 0.32f), .Muted = C(0.45f, 0.47f, 0.52f),
        .Warning = C(0.68f, 0.42f, 0.00f), .Error = C(0.80f, 0.20f, 0.16f), .Changed = C(0.86f, 0.16f, 0.18f),
        .AddressText = C(0.10f, 0.40f, 0.78f), .BytesText = C(0.38f, 0.40f, 0.45f),
        .CodeKeyword = C(0.55f, 0.22f, 0.80f), .CodeRegister = C(0.00f, 0.50f, 0.60f), .CodeNumber = C(0.66f, 0.42f, 0.00f),
        .CodeString = C(0.20f, 0.52f, 0.18f), .CodeComment = C(0.50f, 0.54f, 0.58f), .CodeSection = C(0.80f, 0.33f, 0.10f),
        .CodeLabel = C(0.52f, 0.44f, 0.00f), .FlowJump = C(0.78f, 0.42f, 0.06f), .FlowCall = C(0.10f, 0.46f, 0.74f),
        .FlowReturn = C(0.78f, 0.25f, 0.28f), .Breakpoint = C(0.85f, 0.18f, 0.22f), .CurrentLine = C(0.05f, 0.56f, 0.41f, 0.16f),
        .Surface = C(0.986f, 0.988f, 0.991f), .Frame = C(0.906f, 0.914f, 0.929f), .Button = C(0.886f, 0.894f, 0.910f),
        .Popup = C(1.00f, 1.00f, 1.00f, 0.99f), .TitleBar = C(0.935f, 0.940f, 0.950f), .Shadow = C(0.05f, 0.07f, 0.12f, 0.10f),
    };

    inline constexpr Palette kRedDark{
        .Accent = C(0.93f, 0.42f, 0.55f), .AccentHovered = C(0.97f, 0.52f, 0.63f), .Success = C(0.45f, 0.82f, 0.58f),
        .Sidebar = C(0.052f, 0.042f, 0.045f), .Page = C(0.070f, 0.057f, 0.061f), .Card = C(0.097f, 0.080f, 0.085f),
        .CardBorder = C(1.00f, 0.86f, 0.89f, 0.060f), .RowSeparator = C(1.00f, 0.86f, 0.89f, 0.048f), .Border = C(0.25f, 0.20f, 0.215f, 0.80f),
        .Text = C(0.93f, 0.90f, 0.905f), .Label = C(0.82f, 0.775f, 0.785f), .Muted = C(0.57f, 0.50f, 0.515f),
        .Warning = C(0.96f, 0.78f, 0.36f), .Error = C(1.00f, 0.47f, 0.33f), .Changed = C(1.00f, 0.56f, 0.30f),
        .AddressText = C(0.58f, 0.76f, 0.98f), .BytesText = C(0.62f, 0.58f, 0.59f),
        .CodeKeyword = C(0.82f, 0.60f, 0.98f), .CodeRegister = C(0.47f, 0.82f, 0.96f), .CodeNumber = C(0.98f, 0.80f, 0.47f),
        .CodeString = C(0.64f, 0.87f, 0.54f), .CodeComment = C(0.53f, 0.47f, 0.49f), .CodeSection = C(0.98f, 0.62f, 0.40f),
        .CodeLabel = C(0.98f, 0.91f, 0.58f), .FlowJump = C(0.98f, 0.72f, 0.42f), .FlowCall = C(0.57f, 0.84f, 0.98f),
        .FlowReturn = C(0.98f, 0.55f, 0.50f), .Breakpoint = C(0.96f, 0.30f, 0.28f), .CurrentLine = C(0.93f, 0.42f, 0.55f, 0.20f),
        .Surface = C(0.088f, 0.073f, 0.078f), .Frame = C(0.155f, 0.128f, 0.135f), .Button = C(0.178f, 0.146f, 0.154f),
        .Popup = C(0.108f, 0.089f, 0.094f, 0.98f), .TitleBar = C(0.096f, 0.078f, 0.083f), .Shadow = C(0.00f, 0.00f, 0.00f, 0.30f),
    };

    inline constexpr Palette kRedLight{
        .Accent = C(0.76f, 0.20f, 0.36f), .AccentHovered = C(0.67f, 0.15f, 0.30f), .Success = C(0.13f, 0.52f, 0.30f),
        .Sidebar = C(0.945f, 0.918f, 0.922f), .Page = C(0.973f, 0.957f, 0.959f), .Card = C(1.000f, 0.996f, 0.996f),
        .CardBorder = C(0.35f, 0.08f, 0.12f, 0.10f), .RowSeparator = C(0.35f, 0.08f, 0.12f, 0.07f), .Border = C(0.84f, 0.79f, 0.80f, 0.95f),
        .Text = C(0.15f, 0.10f, 0.11f), .Label = C(0.32f, 0.25f, 0.27f), .Muted = C(0.52f, 0.45f, 0.47f),
        .Warning = C(0.64f, 0.44f, 0.00f), .Error = C(0.80f, 0.24f, 0.08f), .Changed = C(0.82f, 0.36f, 0.02f),
        .AddressText = C(0.10f, 0.40f, 0.76f), .BytesText = C(0.42f, 0.38f, 0.40f),
        .CodeKeyword = C(0.55f, 0.24f, 0.78f), .CodeRegister = C(0.00f, 0.50f, 0.60f), .CodeNumber = C(0.64f, 0.42f, 0.00f),
        .CodeString = C(0.22f, 0.50f, 0.20f), .CodeComment = C(0.56f, 0.50f, 0.52f), .CodeSection = C(0.78f, 0.33f, 0.08f),
        .CodeLabel = C(0.52f, 0.44f, 0.00f), .FlowJump = C(0.76f, 0.42f, 0.04f), .FlowCall = C(0.10f, 0.46f, 0.72f),
        .FlowReturn = C(0.80f, 0.26f, 0.24f), .Breakpoint = C(0.84f, 0.16f, 0.18f), .CurrentLine = C(0.76f, 0.20f, 0.36f, 0.13f),
        .Surface = C(0.990f, 0.982f, 0.983f), .Frame = C(0.935f, 0.905f, 0.910f), .Button = C(0.918f, 0.886f, 0.892f),
        .Popup = C(1.00f, 0.995f, 0.995f, 0.99f), .TitleBar = C(0.952f, 0.930f, 0.934f), .Shadow = C(0.20f, 0.05f, 0.08f, 0.10f),
    };

    inline constexpr Palette kGreenDark{
        .Accent = C(0.52f, 0.82f, 0.40f), .AccentHovered = C(0.60f, 0.90f, 0.48f), .Success = C(0.52f, 0.82f, 0.40f),
        .Sidebar = C(0.042f, 0.052f, 0.046f), .Page = C(0.056f, 0.068f, 0.060f), .Card = C(0.078f, 0.094f, 0.083f),
        .CardBorder = C(0.86f, 1.00f, 0.90f, 0.055f), .RowSeparator = C(0.86f, 1.00f, 0.90f, 0.045f), .Border = C(0.19f, 0.235f, 0.205f, 0.80f),
        .Text = C(0.90f, 0.93f, 0.91f), .Label = C(0.77f, 0.81f, 0.785f), .Muted = C(0.50f, 0.56f, 0.52f),
        .Warning = C(0.96f, 0.72f, 0.32f), .Error = C(0.97f, 0.43f, 0.38f), .Changed = C(0.99f, 0.40f, 0.40f),
        .AddressText = C(0.56f, 0.76f, 0.98f), .BytesText = C(0.58f, 0.63f, 0.60f),
        .CodeKeyword = C(0.80f, 0.58f, 0.98f), .CodeRegister = C(0.42f, 0.82f, 0.94f), .CodeNumber = C(0.98f, 0.78f, 0.45f),
        .CodeString = C(0.86f, 0.84f, 0.50f), .CodeComment = C(0.45f, 0.53f, 0.48f), .CodeSection = C(0.98f, 0.57f, 0.36f),
        .CodeLabel = C(0.96f, 0.90f, 0.62f), .FlowJump = C(0.98f, 0.70f, 0.40f), .FlowCall = C(0.55f, 0.84f, 0.98f),
        .FlowReturn = C(0.95f, 0.52f, 0.50f), .Breakpoint = C(0.92f, 0.28f, 0.30f), .CurrentLine = C(0.52f, 0.82f, 0.40f, 0.20f),
        .Surface = C(0.071f, 0.086f, 0.076f), .Frame = C(0.125f, 0.150f, 0.133f), .Button = C(0.145f, 0.172f, 0.153f),
        .Popup = C(0.087f, 0.104f, 0.092f, 0.98f), .TitleBar = C(0.077f, 0.093f, 0.082f), .Shadow = C(0.00f, 0.00f, 0.00f, 0.30f),
    };

    inline constexpr Palette kGreenLight{
        .Accent = C(0.20f, 0.52f, 0.20f), .AccentHovered = C(0.16f, 0.45f, 0.16f), .Success = C(0.20f, 0.52f, 0.20f),
        .Sidebar = C(0.918f, 0.937f, 0.922f), .Page = C(0.955f, 0.966f, 0.957f), .Card = C(0.997f, 1.000f, 0.997f),
        .CardBorder = C(0.05f, 0.25f, 0.12f, 0.10f), .RowSeparator = C(0.05f, 0.25f, 0.12f, 0.07f), .Border = C(0.78f, 0.82f, 0.79f, 0.95f),
        .Text = C(0.10f, 0.14f, 0.115f), .Label = C(0.24f, 0.30f, 0.26f), .Muted = C(0.44f, 0.50f, 0.46f),
        .Warning = C(0.68f, 0.44f, 0.00f), .Error = C(0.80f, 0.20f, 0.16f), .Changed = C(0.85f, 0.16f, 0.18f),
        .AddressText = C(0.10f, 0.40f, 0.76f), .BytesText = C(0.38f, 0.43f, 0.40f),
        .CodeKeyword = C(0.55f, 0.22f, 0.78f), .CodeRegister = C(0.00f, 0.48f, 0.64f), .CodeNumber = C(0.66f, 0.42f, 0.00f),
        .CodeString = C(0.45f, 0.45f, 0.00f), .CodeComment = C(0.48f, 0.55f, 0.50f), .CodeSection = C(0.80f, 0.33f, 0.10f),
        .CodeLabel = C(0.56f, 0.40f, 0.00f), .FlowJump = C(0.78f, 0.42f, 0.06f), .FlowCall = C(0.10f, 0.46f, 0.72f),
        .FlowReturn = C(0.78f, 0.25f, 0.28f), .Breakpoint = C(0.85f, 0.18f, 0.22f), .CurrentLine = C(0.20f, 0.52f, 0.20f, 0.14f),
        .Surface = C(0.984f, 0.990f, 0.985f), .Frame = C(0.900f, 0.925f, 0.906f), .Button = C(0.880f, 0.906f, 0.886f),
        .Popup = C(0.997f, 1.00f, 0.997f, 0.99f), .TitleBar = C(0.930f, 0.946f, 0.933f), .Shadow = C(0.04f, 0.12f, 0.06f, 0.10f),
    };

    inline constexpr Palette kBlueDark{
        .Accent = C(0.38f, 0.61f, 0.97f), .AccentHovered = C(0.48f, 0.69f, 0.99f), .Success = C(0.36f, 0.84f, 0.62f),
        .Sidebar = C(0.040f, 0.050f, 0.075f), .Page = C(0.053f, 0.066f, 0.095f), .Card = C(0.075f, 0.092f, 0.128f),
        .CardBorder = C(0.80f, 0.88f, 1.00f, 0.060f), .RowSeparator = C(0.80f, 0.88f, 1.00f, 0.048f), .Border = C(0.18f, 0.22f, 0.30f, 0.80f),
        .Text = C(0.89f, 0.92f, 0.97f), .Label = C(0.76f, 0.80f, 0.88f), .Muted = C(0.49f, 0.54f, 0.63f),
        .Warning = C(0.96f, 0.72f, 0.34f), .Error = C(0.96f, 0.44f, 0.40f), .Changed = C(0.99f, 0.42f, 0.42f),
        .AddressText = C(0.52f, 0.82f, 0.96f), .BytesText = C(0.58f, 0.62f, 0.70f),
        .CodeKeyword = C(0.78f, 0.58f, 0.98f), .CodeRegister = C(0.40f, 0.86f, 0.80f), .CodeNumber = C(0.98f, 0.78f, 0.47f),
        .CodeString = C(0.62f, 0.88f, 0.54f), .CodeComment = C(0.45f, 0.51f, 0.60f), .CodeSection = C(0.98f, 0.57f, 0.38f),
        .CodeLabel = C(0.97f, 0.91f, 0.56f), .FlowJump = C(0.98f, 0.70f, 0.42f), .FlowCall = C(0.52f, 0.84f, 0.98f),
        .FlowReturn = C(0.95f, 0.52f, 0.52f), .Breakpoint = C(0.92f, 0.28f, 0.32f), .CurrentLine = C(0.38f, 0.61f, 0.97f, 0.22f),
        .Surface = C(0.068f, 0.084f, 0.118f), .Frame = C(0.115f, 0.140f, 0.190f), .Button = C(0.135f, 0.162f, 0.215f),
        .Popup = C(0.083f, 0.101f, 0.140f, 0.98f), .TitleBar = C(0.073f, 0.090f, 0.126f), .Shadow = C(0.00f, 0.01f, 0.04f, 0.32f),
    };

    inline constexpr Palette kBlueLight{
        .Accent = C(0.15f, 0.40f, 0.82f), .AccentHovered = C(0.12f, 0.34f, 0.72f), .Success = C(0.08f, 0.52f, 0.36f),
        .Sidebar = C(0.906f, 0.925f, 0.957f), .Page = C(0.947f, 0.958f, 0.978f), .Card = C(0.995f, 0.997f, 1.000f),
        .CardBorder = C(0.06f, 0.14f, 0.32f, 0.11f), .RowSeparator = C(0.06f, 0.14f, 0.32f, 0.075f), .Border = C(0.76f, 0.80f, 0.87f, 0.95f),
        .Text = C(0.10f, 0.13f, 0.20f), .Label = C(0.24f, 0.28f, 0.37f), .Muted = C(0.43f, 0.48f, 0.57f),
        .Warning = C(0.68f, 0.44f, 0.00f), .Error = C(0.80f, 0.20f, 0.16f), .Changed = C(0.85f, 0.16f, 0.18f),
        .AddressText = C(0.00f, 0.46f, 0.62f), .BytesText = C(0.38f, 0.42f, 0.50f),
        .CodeKeyword = C(0.52f, 0.24f, 0.80f), .CodeRegister = C(0.00f, 0.50f, 0.48f), .CodeNumber = C(0.66f, 0.42f, 0.00f),
        .CodeString = C(0.20f, 0.50f, 0.18f), .CodeComment = C(0.47f, 0.52f, 0.60f), .CodeSection = C(0.80f, 0.33f, 0.10f),
        .CodeLabel = C(0.52f, 0.44f, 0.00f), .FlowJump = C(0.78f, 0.42f, 0.06f), .FlowCall = C(0.05f, 0.44f, 0.70f),
        .FlowReturn = C(0.78f, 0.25f, 0.28f), .Breakpoint = C(0.85f, 0.18f, 0.22f), .CurrentLine = C(0.15f, 0.40f, 0.82f, 0.14f),
        .Surface = C(0.979f, 0.985f, 0.996f), .Frame = C(0.890f, 0.910f, 0.950f), .Button = C(0.868f, 0.890f, 0.935f),
        .Popup = C(0.995f, 0.997f, 1.00f, 0.99f), .TitleBar = C(0.918f, 0.935f, 0.965f), .Shadow = C(0.04f, 0.08f, 0.20f, 0.11f),
    };

    constexpr bool Complete(const Palette& palette)
    {
        return true
#define MANIFOLD_THEME_CHECK(name, label) && palette.name.w > 0.0f
            MANIFOLD_THEME_PALETTE(MANIFOLD_THEME_CHECK)
#undef MANIFOLD_THEME_CHECK
            ;
    }

    static_assert(Complete(kStandardDark) && Complete(kStandardLight) && Complete(kRedDark) && Complete(kRedLight) && Complete(kGreenDark) &&
        Complete(kGreenLight) && Complete(kBlueDark) && Complete(kBlueLight));

    inline const Palette& PaletteOf(ThemeFamily family, bool light)
    {
        switch (family)
        {
        case ThemeFamily::Red: return light ? kRedLight : kRedDark;
        case ThemeFamily::Green: return light ? kGreenLight : kGreenDark;
        case ThemeFamily::Blue: return light ? kBlueLight : kBlueDark;
        default: return light ? kStandardLight : kStandardDark;
        }
    }

    inline const char* FamilyLabel(ThemeFamily family)
    {
        switch (family)
        {
        case ThemeFamily::Red: return "Red";
        case ThemeFamily::Green: return "Green";
        case ThemeFamily::Blue: return "Blue";
        default: return "Standard";
        }
    }

#define MANIFOLD_THEME_VALUE(name, label) inline ImVec4 name = kStandardDark.name;
    MANIFOLD_THEME_PALETTE(MANIFOLD_THEME_VALUE)
#undef MANIFOLD_THEME_VALUE

    struct PaletteColor
    {
        const char* Name;
        const char* Label;
        ImVec4* Value;
        ImVec4 Palette::* Member;
    };

#define MANIFOLD_THEME_ENTRY(name, label) PaletteColor{ #name, label, &name, &Palette::name },
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
    inline ThemeFamily g_Family = ThemeFamily::Standard;
    inline bool g_Light = false;
    inline bool g_SystemLight = false;

    inline bool IsLight()
    {
        return g_Light;
    }

    inline ThemeFamily CurrentFamily()
    {
        return g_Family;
    }

    inline void SetSystemLight(bool light)
    {
        g_SystemLight = light;
    }

    inline bool SystemLight()
    {
        return g_SystemLight;
    }

    inline bool ResolveLight(const Manifold::ThemeSettings& theme)
    {
        return theme.Mode == ThemeMode::System ? g_SystemLight : theme.Mode == ThemeMode::Light;
    }

    inline std::string VariantPrefix(ThemeFamily family, bool light)
    {
        if (family == ThemeFamily::Standard && !light)
            return {};
        return std::string(Manifold::ThemeFamilyKey(family)) + (light ? "-light." : "-dark.");
    }

    inline std::string CurrentPrefix()
    {
        return VariantPrefix(g_Family, g_Light);
    }

    inline std::size_t PrefixLength(std::string_view lower)
    {
        for (ThemeFamily family : Manifold::kThemeFamilies)
        {
            for (const char* mode : { "-dark.", "-light." })
            {
                const std::string prefix = std::string(Manifold::ThemeFamilyKey(family)) + mode;
                if (lower.substr(0, prefix.size()) == prefix)
                    return prefix.size();
            }
        }
        return 0;
    }

    inline bool BelongsToCurrent(std::string_view key)
    {
        const std::string lower = LowerName(key);
        const std::size_t length = PrefixLength(lower);
        const std::string own = LowerName(CurrentPrefix());
        if (own.empty())
            return length == 0 || lower.substr(0, length) == "standard-dark.";
        return length > 0 && lower.substr(0, length) == own;
    }

    inline ImVec4 WithAlpha(ImVec4 color, float alpha)
    {
        color.w = alpha;
        return color;
    }

    inline ImVec4 Mix(ImVec4 from, ImVec4 to, float amount)
    {
        return ImVec4(from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount, from.z + (to.z - from.z) * amount, from.w + (to.w - from.w) * amount);
    }

    inline ImVec4 Opaque(ImVec4 color)
    {
        color.w = 1.0f;
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

    inline void Configure(const Manifold::ThemeSettings& theme)
    {
        g_Family = theme.Family;
        g_Light = ResolveLight(theme);
        const Palette& base = PaletteOf(g_Family, g_Light);
        for (const PaletteColor& color : kPalette)
            *color.Value = base.*color.Member;
        g_ColorOverrides.clear();
        g_SizeOverrides.clear();
        for (const auto& [name, rgba] : theme.Colors)
        {
            if (!BelongsToCurrent(name))
                continue;
            const ImVec4 value(rgba[0], rgba[1], rgba[2], rgba[3]);
            const std::string full = LowerName(name);
            const std::string lower = full.substr(PrefixLength(full));
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
        for (const auto& [name, value] : theme.Sizes)
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
        const ImVec4 clear(0.00f, 0.00f, 0.00f, 0.00f);
        const auto tone = [](float amount) { return Opaque(Mix(Page, Text, amount)); };

        colors[ImGuiCol_Text]                   = Text;
        colors[ImGuiCol_TextDisabled]           = Muted;
        colors[ImGuiCol_WindowBg]               = Opaque(Mix(Page, Sidebar, 0.20f));
        colors[ImGuiCol_ChildBg]                = Surface;
        colors[ImGuiCol_PopupBg]                = Popup;
        colors[ImGuiCol_Border]                 = Border;
        colors[ImGuiCol_BorderShadow]           = clear;
        colors[ImGuiCol_FrameBg]                = Frame;
        colors[ImGuiCol_FrameBgHovered]         = Mix(Frame, WithAlpha(Text, Frame.w), 0.05f);
        colors[ImGuiCol_FrameBgActive]          = Mix(Frame, WithAlpha(Text, Frame.w), 0.09f);
        colors[ImGuiCol_TitleBg]                = Opaque(Mix(TitleBar, Page, 0.90f));
        colors[ImGuiCol_TitleBgActive]          = TitleBar;
        colors[ImGuiCol_TitleBgCollapsed]       = WithAlpha(Mix(TitleBar, Sidebar, 0.80f), 0.60f);
        colors[ImGuiCol_MenuBarBg]              = TitleBar;
        colors[ImGuiCol_ScrollbarBg]            = clear;
        colors[ImGuiCol_ScrollbarGrab]          = WithAlpha(tone(0.21f), 0.80f);
        colors[ImGuiCol_ScrollbarGrabHovered]   = WithAlpha(tone(0.31f), 0.90f);
        colors[ImGuiCol_ScrollbarGrabActive]    = WithAlpha(Accent, 0.70f);
        colors[ImGuiCol_CheckMark]              = Accent;
        colors[ImGuiCol_CheckboxSelectedBg]     = Frame;
        colors[ImGuiCol_SliderGrab]             = WithAlpha(Accent, 0.85f);
        colors[ImGuiCol_SliderGrabActive]       = Accent;
        colors[ImGuiCol_Button]                 = Button;
        colors[ImGuiCol_ButtonHovered]          = Mix(Button, WithAlpha(Text, Button.w), 0.07f);
        colors[ImGuiCol_ButtonActive]           = WithAlpha(Accent, 0.35f);
        colors[ImGuiCol_Header]                 = WithAlpha(Accent, 0.16f);
        colors[ImGuiCol_HeaderHovered]          = WithAlpha(Accent, 0.24f);
        colors[ImGuiCol_HeaderActive]           = WithAlpha(Accent, 0.32f);
        colors[ImGuiCol_Separator]              = Border;
        colors[ImGuiCol_SeparatorHovered]       = WithAlpha(Accent, 0.55f);
        colors[ImGuiCol_SeparatorActive]        = Accent;
        colors[ImGuiCol_ResizeGrip]             = clear;
        colors[ImGuiCol_ResizeGripHovered]      = WithAlpha(Accent, 0.45f);
        colors[ImGuiCol_ResizeGripActive]       = WithAlpha(Accent, 0.80f);
        colors[ImGuiCol_InputTextCursor]        = Text;
        colors[ImGuiCol_Tab]                    = tone(0.05f);
        colors[ImGuiCol_TabHovered]             = WithAlpha(Accent, 0.24f);
        colors[ImGuiCol_TabSelected]            = WithAlpha(Accent, 0.18f);
        colors[ImGuiCol_TabSelectedOverline]    = Accent;
        colors[ImGuiCol_TabDimmed]              = tone(0.038f);
        colors[ImGuiCol_TabDimmedSelected]      = tone(0.074f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(Accent, 0.0f);
        colors[ImGuiCol_DockingPreview]         = WithAlpha(Accent, 0.40f);
        colors[ImGuiCol_DockingEmptyBg]         = Sidebar;
        colors[ImGuiCol_PlotLines]              = tone(0.70f);
        colors[ImGuiCol_PlotLinesHovered]       = Accent;
        colors[ImGuiCol_PlotHistogram]          = WithAlpha(Accent, 0.80f);
        colors[ImGuiCol_PlotHistogramHovered]   = Accent;
        colors[ImGuiCol_TableHeaderBg]          = tone(0.05f);
        colors[ImGuiCol_TableBorderStrong]      = tone(0.145f);
        colors[ImGuiCol_TableBorderLight]       = tone(0.098f);
        colors[ImGuiCol_TableRowBg]             = clear;
        colors[ImGuiCol_TableRowBgAlt]          = WithAlpha(Text, 0.025f);
        colors[ImGuiCol_TextLink]               = Accent;
        colors[ImGuiCol_TextSelectedBg]         = WithAlpha(Accent, 0.30f);
        colors[ImGuiCol_TreeLines]              = Border;
        colors[ImGuiCol_DragDropTarget]         = Accent;
        colors[ImGuiCol_DragDropTargetBg]       = clear;
        colors[ImGuiCol_UnsavedMarker]          = Text;
        colors[ImGuiCol_NavCursor]              = WithAlpha(Accent, 0.80f);
        colors[ImGuiCol_NavWindowingHighlight]  = WithAlpha(Text, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg]      = g_Light ? ImVec4(0.10f, 0.12f, 0.16f, 0.20f) : ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg]       = g_Light ? ImVec4(0.10f, 0.12f, 0.16f, 0.28f) : ImVec4(0.00f, 0.00f, 0.00f, 0.55f);

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
