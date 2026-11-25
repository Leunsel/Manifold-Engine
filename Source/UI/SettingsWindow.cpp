#include "UI/SettingsWindow.h"

#include "App/Settings.h"
#include "App/Sounds.h"
#include "Core/Profiler.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/BuildInfo.h"
#include "Generated/IconsFontAwesome5.h"
#include "Scan/ScanStore.h"
#include "Target/Process.h"
#include "Layout/Layout.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UpdateDialog.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <thread>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        struct PageInfo
        {
            const char* Icon;
            const char* Title;
            const char* Subtitle;
        };

        constexpr PageInfo kPages[] = {
            { ICON_FA_SLIDERS_H, "General", "Layout, refresh rates, sounds and the program." },
            { ICON_FA_PALETTE, "Appearance", "Theme, mode, colors and sizes." },
            { ICON_FA_SEARCH, "Scanner", "Scan options the program starts with." },
            { ICON_FA_SCROLL, "Scripts", "Lua, tables, backups and opening processes by name." },
            { ICON_FA_BUG, "Debugger", "Structure Dissect, breakpoints, Unity and the log." },
            { ICON_FA_FINGERPRINT, "Signatures", "How Copy Signature and the Signature Maker build byte patterns." },
            { ICON_FA_KEYBOARD, "Keyboard", "Click keys to change them, right-click to remove them." },
        };
        constexpr int kPageCount = static_cast<int>(std::size(kPages));
        static_assert(kPageCount == SettingsWindow::PageCount);

        struct AccentPreset
        {
            const char* Name;
            ImVec4 Color;
        };

        const AccentPreset kAccents[] = {
            { "Theme", ImVec4(0.0f, 0.0f, 0.0f, 0.0f) },
            { "Sky", ImVec4(0.36f, 0.68f, 0.98f, 1.0f) },
            { "Violet", ImVec4(0.67f, 0.54f, 0.98f, 1.0f) },
            { "Rose", ImVec4(0.96f, 0.46f, 0.64f, 1.0f) },
            { "Coral", ImVec4(0.98f, 0.52f, 0.40f, 1.0f) },
            { "Amber", ImVec4(0.98f, 0.72f, 0.30f, 1.0f) },
            { "Lime", ImVec4(0.64f, 0.86f, 0.32f, 1.0f) },
            { "Cyan", ImVec4(0.26f, 0.82f, 0.90f, 1.0f) },
        };

        const char* const kAccentColors[] = { "Accent", "AccentHovered" };
        const char* const kSurfaceColors[] = { "Page", "Sidebar", "Card", "CardBorder", "Surface", "Frame", "Button", "Popup", "TitleBar", "Border", "RowSeparator", "Shadow" };
        const char* const kTextColors[] = { "Text", "Label", "Muted", "Success", "Warning", "Error", "Changed" };

        constexpr ThemeMode kModes[] = { ThemeMode::Light, ThemeMode::Dark, ThemeMode::System };
        const char* const kCodeColors[] = { "AddressText", "BytesText", "CodeKeyword", "CodeRegister", "CodeNumber", "CodeString", "CodeComment", "CodeUserComment", "CodeSection",
            "CodeLabel", "CodeMacro", "FlowJump", "FlowCall", "FlowReturn", "Breakpoint", "CurrentLine" };

        void OpenFolder(const std::filesystem::path& folder)
        {
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        ImVec4 Lighter(ImVec4 color, float amount)
        {
            return ImVec4(std::min(1.0f, color.x + amount), std::min(1.0f, color.y + amount), std::min(1.0f, color.z + amount), color.w);
        }

        ImVec4 Deeper(ImVec4 color, float saturation, float value)
        {
            float h = 0.0f;
            float s = 0.0f;
            float v = 0.0f;
            ImGui::ColorConvertRGBtoHSV(color.x, color.y, color.z, h, s, v);
            ImVec4 result = color;
            ImGui::ColorConvertHSVtoRGB(h, std::min(1.0f, s * saturation + 0.1f), v * value, result.x, result.y, result.z);
            return result;
        }

        ImVec4 PresetAccent(int index, bool light)
        {
            if (index == 0)
                return ManifoldTheme::PaletteOf(ManifoldTheme::CurrentFamily(), light).Accent;
            const ImVec4 color = kAccents[index].Color;
            return light ? Deeper(color, 1.25f, 0.68f) : color;
        }

        ImVec4 PresetHovered(ImVec4 accent, bool light)
        {
            return light ? ImVec4(accent.x * 0.88f, accent.y * 0.88f, accent.z * 0.88f, accent.w) : Lighter(accent, 0.08f);
        }

        bool OwnColors(const ThemeSettings& theme)
        {
            return std::any_of(theme.Colors.begin(), theme.Colors.end(), [](const auto& entry) { return ManifoldTheme::BelongsToCurrent(entry.first); });
        }

        void ResetOwnColors(ThemeSettings& theme)
        {
            for (auto it = theme.Colors.begin(); it != theme.Colors.end();)
                it = ManifoldTheme::BelongsToCurrent(it->first) ? theme.Colors.erase(it) : std::next(it);
            theme.Sizes.clear();
        }

        enum ThemeNaming
        {
            NameNew = 0,
            NameDuplicate = 1,
            NameRename = 2
        };

        void CardLabel(ImDrawList* draw, const ImVec2& pos, float width, float top, std::string_view text, ImVec4 tint)
        {
            const std::string shown = Layout::Shorten(text, width);
            const float measured = Layout::FitText(ImGui::GetFont(), ImGui::GetFontSize(), shown, 1.0e6f).Width;
            draw->AddText(ImVec2(std::round(pos.x + (width - measured) * 0.5f), top), ImGui::GetColorU32(tint), shown.c_str());
        }

        bool ThemeCard(const char* id, std::string_view label, const ManifoldTheme::Palette& palette, bool selected, const std::string& tooltip, float width, float height)
        {
            const float labelHeight = ImGui::GetTextLineHeight() + Px(8.0f);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height + labelHeight));
            const bool hovered = ImGui::IsItemHovered();
            ItemTooltip(tooltip.c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const auto color = [](ImVec4 value) { return ImGui::GetColorU32(value); };
            const float rounding = Px(7.0f);
            const ImVec2 max(pos.x + width, pos.y + height);
            draw->AddRectFilled(pos, max, color(palette.Page), rounding);

            const float sidebar = std::round(width * 0.24f);
            draw->AddRectFilled(pos, ImVec2(pos.x + sidebar, max.y), color(palette.Sidebar), rounding, ImDrawFlags_RoundCornersLeft);
            const float bar = std::max(2.0f, std::round(Px(3.0f)));
            const float navLeft = pos.x + Px(6.0f);
            const float navRight = pos.x + sidebar - Px(6.0f);
            for (int i = 0; i < 4; ++i)
            {
                const float y = std::round(pos.y + Px(10.0f) + static_cast<float>(i) * Px(9.0f));
                draw->AddRectFilled(ImVec2(navLeft, y), ImVec2(i == 0 ? navRight : navRight - Px(4.0f), y + bar), color(i == 0 ? palette.Accent : ManifoldTheme::WithAlpha(palette.Muted, 0.65f)), bar);
            }

            const ImVec2 cardMin(pos.x + sidebar + Px(7.0f), pos.y + Px(8.0f));
            const ImVec2 cardMax(max.x - Px(7.0f), max.y - Px(8.0f));
            draw->AddRectFilled(cardMin, cardMax, color(palette.Card), Px(4.0f));
            draw->AddRect(cardMin, cardMax, color(palette.CardBorder), Px(4.0f));
            const float inner = cardMax.x - cardMin.x - Px(12.0f);
            const float left = cardMin.x + Px(6.0f);
            const auto line = [&](int row, float from, float to, ImVec4 tint) {
                const float y = std::round(cardMin.y + Px(7.0f) + static_cast<float>(row) * Px(8.0f));
                draw->AddRectFilled(ImVec2(std::round(left + inner * from), y), ImVec2(std::round(left + inner * to), y + bar), color(tint), bar);
            };
            line(0, 0.0f, 0.62f, palette.Text);
            line(1, 0.0f, 0.30f, palette.AddressText);
            line(1, 0.38f, 0.58f, palette.Changed);
            line(2, 0.0f, 0.30f, palette.AddressText);
            line(2, 0.38f, 0.66f, palette.Label);
            line(3, 0.0f, 0.46f, palette.Muted);
            const float pillTop = std::round(cardMax.y - Px(6.0f) - Px(8.0f));
            if (pillTop > cardMin.y + Px(36.0f))
                draw->AddRectFilled(ImVec2(cardMax.x - Px(6.0f) - inner * 0.34f, pillTop), ImVec2(cardMax.x - Px(6.0f), pillTop + Px(8.0f)), color(palette.Accent), Px(4.0f));

            const ImVec4 ring = selected ? ManifoldTheme::Accent : hovered ? ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.35f) : ManifoldTheme::Border;
            draw->AddRect(ImVec2(pos.x - 0.5f, pos.y - 0.5f), ImVec2(max.x + 0.5f, max.y + 0.5f), color(ring), rounding, 0, selected ? Px(2.0f) : Px(1.0f));
            CardLabel(draw, pos, width, std::round(max.y + Px(5.0f)), label, selected ? ManifoldTheme::Text : hovered ? ManifoldTheme::Label : ManifoldTheme::Muted);
            return clicked;
        }

        // The last card of the gallery: a dashed outline with a plus.
        bool NewThemeCard(float width, float height)
        {
            const float labelHeight = ImGui::GetTextLineHeight() + Px(8.0f);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##newtheme", ImVec2(width, height + labelHeight));
            const bool hovered = ImGui::IsItemHovered();
            ItemTooltip("Makes a custom theme from the one shown. Right-click to import a theme file.");
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 max(pos.x + width, pos.y + height);
            const ImU32 tint = ImGui::GetColorU32(hovered ? ManifoldTheme::Accent : ManifoldTheme::Muted);
            const float dash = Px(6.0f);
            const float inset = Px(1.0f);
            for (float x = pos.x + Px(8.0f); x < max.x - Px(8.0f); x += dash * 2.0f)
            {
                const float to = std::min(x + dash, max.x - Px(8.0f));
                draw->AddLine(ImVec2(x, pos.y + inset), ImVec2(to, pos.y + inset), tint, Px(1.0f));
                draw->AddLine(ImVec2(x, max.y - inset), ImVec2(to, max.y - inset), tint, Px(1.0f));
            }
            for (float y = pos.y + Px(8.0f); y < max.y - Px(8.0f); y += dash * 2.0f)
            {
                const float to = std::min(y + dash, max.y - Px(8.0f));
                draw->AddLine(ImVec2(pos.x + inset, y), ImVec2(pos.x + inset, to), tint, Px(1.0f));
                draw->AddLine(ImVec2(max.x - inset, y), ImVec2(max.x - inset, to), tint, Px(1.0f));
            }
            const float plus = std::round(std::min(width, height) * 0.14f);
            const ImVec2 center(std::round(pos.x + width * 0.5f), std::round(pos.y + height * 0.5f));
            draw->AddLine(ImVec2(center.x - plus, center.y), ImVec2(center.x + plus, center.y), tint, Px(2.0f));
            draw->AddLine(ImVec2(center.x, center.y - plus), ImVec2(center.x, center.y + plus), tint, Px(2.0f));
            CardLabel(draw, pos, width, std::round(max.y + Px(5.0f)), "New Theme", hovered ? ManifoldTheme::Label : ManifoldTheme::Muted);
            return clicked;
        }

        bool SameColor(ImVec4 a, ImVec4 b)
        {
            return std::abs(a.x - b.x) < 0.004f && std::abs(a.y - b.y) < 0.004f && std::abs(a.z - b.z) < 0.004f;
        }

        int AutoThreads()
        {
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            return static_cast<int>(std::clamp(cores / 2u, 2u, 8u));
        }

        template <typename Values>
        void Erase(Values& values, std::string_view key)
        {
            for (auto it = values.begin(); it != values.end();)
                it = EqualsNoCase(it->first, key) ? values.erase(it) : std::next(it);
        }

        template <typename Values>
        bool Overridden(const Values& values, std::string_view key)
        {
            return std::any_of(values.begin(), values.end(), [&](const auto& entry) { return EqualsNoCase(entry.first, key); });
        }

        const ManifoldTheme::PaletteColor* FindPalette(std::string_view name)
        {
            for (const ManifoldTheme::PaletteColor& color : ManifoldTheme::kPalette)
            {
                if (name == color.Name)
                    return &color;
            }
            return nullptr;
        }

        ManifoldTheme::Palette CustomPalette(const Themes::CustomTheme& theme, bool light)
        {
            ManifoldTheme::Palette palette = ManifoldTheme::PaletteOf(theme.Base, light);
            for (const auto& [name, rgba] : light ? theme.Light : theme.Dark)
            {
                if (const ManifoldTheme::PaletteColor* color = FindPalette(name))
                    palette.*color->Member = ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]);
            }
            return palette;
        }

        bool ResetButton(bool visible)
        {
            const float frame = ImGui::GetFrameHeight();
            if (!visible)
            {
                ImGui::Dummy(ImVec2(frame, frame));
                return false;
            }
            bool pressed = false;
            {
                const GhostButtons ghost(ManifoldTheme::Muted);
                pressed = ImGui::Button(ICON_FA_UNDO "##reset", ImVec2(frame, frame));
            }
            ItemTooltip("Back to the default");
            return pressed;
        }

        void RightText(const char* text, float width, ImVec4 color)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 size = ImGui::CalcTextSize(text);
            ImGui::SetCursorScreenPos(ImVec2(pos.x + width - size.x, pos.y + (ImGui::GetFrameHeight() - size.y) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(text);
            ImGui::PopStyleColor();
        }

        void BeginFillSlider(const char* id, float ratio, float width)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 max(pos.x + width, pos.y + ImGui::GetFrameHeight());
            const bool active = ImGui::GetActiveID() == ImGui::GetID(id);
            const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pos, max);
            const float rounding = ImGui::GetStyle().FrameRounding;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(pos, max, ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg)), rounding);
            const float fill = std::round(pos.x + width * std::clamp(ratio, 0.0f, 1.0f));
            if (fill > pos.x + Px(2.0f))
                draw->AddRectFilled(pos, ImVec2(fill, max.y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, active ? 0.34f : (hovered ? 0.27f : 0.20f))), rounding,
                    fill >= max.x - rounding ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
            const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
            for (ImGuiCol color : { ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive })
                ImGui::PushStyleColor(color, clear);
            ImGui::SetNextItemWidth(width);
        }

        void EndFillSlider()
        {
            ImGui::PopStyleColor(5);
        }

        float LogRatio(int value, int min, int max)
        {
            return static_cast<float>(std::log(static_cast<double>(std::max(value, min)) / min) / std::log(static_cast<double>(max) / min));
        }

        bool LogSlider(const char* id, int& value, int min, int max, const char* format, float width)
        {
            BeginFillSlider(id, LogRatio(value, min, max), width);
            ImGui::SliderInt(id, &value, min, max, format, ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            return ImGui::IsItemDeactivatedAfterEdit();
        }

        bool Milliseconds(const char* id, int& value, int min, int max, float width)
        {
            return LogSlider(id, value, min, max, "%d ms", width);
        }
    }

    class SettingsList
    {
    public:
        explicit SettingsList(std::string filter) : m_filter(std::move(filter)) {}

        bool Searching() const { return !m_filter.empty(); }
        const std::string& Filter() const { return m_filter; }
        int Shown() const { return m_shown; }
        float ControlWidth() const { return m_controlWidth; }

        void Page(int page)
        {
            EndSection();
            m_page = page;
            m_pageShown = false;
        }

        void Section(const char* title, int columns = 1)
        {
            EndSection();
            m_section = title;
            m_columns = std::max(1, columns);
        }

        bool Row(std::string_view title, std::string_view description, float controlWidth)
        {
            if (Searching() && !Matches(title, description))
                return false;
            Open();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const int column = m_rows % m_columns;
            if (column == 0 && m_rows > 0)
            {
                m_lineTop = m_lineBottom;
                draw->AddLine(ImVec2(m_left + m_padding, m_lineTop), ImVec2(m_right - m_padding, m_lineTop), ImGui::GetColorU32(ManifoldTheme::RowSeparator), Px(1.0f));
            }
            const float cell = (m_right - m_left) / static_cast<float>(m_columns);
            const float left = std::round(m_left + cell * static_cast<float>(column) + m_padding);
            const float right = std::round(m_left + cell * static_cast<float>(column + 1) - m_padding);
            const float frame = ImGui::GetFrameHeight();
            const float line = ImGui::GetTextLineHeight();
            m_below = controlWidth < 0.0f || right - left - controlWidth - Px(20.0f) < Px(120.0f);
            m_rowPadding = std::round(Px(description.empty() ? 9.0f : 12.0f));
            const float top = m_lineTop;
            const float textWidth = m_below ? right - left : right - left - controlWidth - Px(20.0f);
            const bool titleWraps = ImGui::CalcTextSize(title.data(), title.data() + title.size()).x > textWidth;

            ImGui::PushID(m_section ? m_section : "");
            ImGui::PushID(title.data(), title.data() + title.size());
            const float textTop = m_below || !description.empty() || titleWraps ? top + m_rowPadding : std::round(top + m_rowPadding + (frame - line) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(left, textTop));
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + textWidth);
            ImGui::TextUnformatted(title.data(), title.data() + title.size());
            ImGui::PopTextWrapPos();
            float textBottom = ImGui::GetItemRectMax().y;
            if (!description.empty())
            {
                ImGui::SetCursorScreenPos(ImVec2(left, textBottom + Px(3.0f)));
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + textWidth);
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                ImGui::TextUnformatted(description.data(), description.data() + description.size());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                textBottom = ImGui::GetItemRectMax().y;
            }

            if (m_below)
            {
                m_controlWidth = right - left;
                ImGui::SetCursorScreenPos(ImVec2(left, textBottom + Px(10.0f)));
            }
            else
            {
                m_controlWidth = controlWidth;
                const float content = std::max(textBottom - (top + m_rowPadding), frame);
                m_rowBottom = std::round(top + m_rowPadding * 2.0f + content);
                ImGui::SetCursorScreenPos(ImVec2(right - controlWidth, std::round(top + (m_rowBottom - top - frame) * 0.5f)));
            }
            ImGui::BeginGroup();
            return true;
        }

        void EndRow()
        {
            ImGui::EndGroup();
            float bottom = m_rowBottom;
            if (m_below)
                bottom = std::round(ImGui::GetItemRectMax().y + m_rowPadding);
            ImGui::PopID();
            ImGui::PopID();
            m_lineBottom = std::max(m_lineBottom, bottom);
            ++m_rows;
            ++m_shown;
            ImGui::SetCursorScreenPos(ImVec2(m_left, m_lineBottom));
        }

        void EndSection()
        {
            if (!m_open)
                return;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float rounding = Px(8.0f);
            draw->ChannelsSetCurrent(0);
            draw->AddRectFilled(ImVec2(m_left, m_top), ImVec2(m_right, m_lineBottom), ImGui::GetColorU32(ManifoldTheme::Card), rounding);
            draw->AddRect(ImVec2(m_left, m_top), ImVec2(m_right, m_lineBottom), ImGui::GetColorU32(ManifoldTheme::CardBorder), rounding);
            draw->ChannelsMerge();
            m_open = false;
            ImGui::SetCursorScreenPos(ImVec2(m_left, m_lineBottom));
            ImGui::Dummy(ImVec2(m_right - m_left, Px(22.0f)));
            ImGui::SetCursorScreenPos(ImVec2(m_left, m_lineBottom + Px(22.0f)));
        }

    private:
        bool Matches(std::string_view title, std::string_view description) const
        {
            return ContainsNoCase(title, m_filter) || ContainsNoCase(description, m_filter) || (m_section && ContainsNoCase(m_section, m_filter)) ||
                ContainsNoCase(kPages[m_page].Title, m_filter);
        }

        void Open()
        {
            if (m_open)
                return;
            const float x = ImGui::GetCursorScreenPos().x;
            float y = ImGui::GetCursorScreenPos().y;
            if (Searching() && !m_pageShown)
            {
                const PageInfo& page = kPages[m_page];
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
                ImGui::TextUnformatted(page.Icon);
                ImGui::PopStyleColor();
                ImGui::SetCursorScreenPos(ImVec2(x + Px(24.0f), y));
                ImGui::PushFont(Fonts::Bold(), 0.0f);
                ImGui::TextUnformatted(page.Title);
                ImGui::PopFont();
                y += ImGui::GetTextLineHeight() + Px(12.0f);
                m_pageShown = true;
            }
            if (m_section && *m_section)
            {
                ImGui::SetCursorScreenPos(ImVec2(x + Px(4.0f), y));
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                ImGui::TextUnformatted(m_section);
                ImGui::PopStyleColor();
                y += ImGui::GetTextLineHeight() + Px(7.0f);
            }
            m_left = x;
            m_right = x + ImGui::GetContentRegionAvail().x;
            m_top = y;
            m_lineTop = y;
            m_lineBottom = y;
            m_rows = 0;
            m_padding = std::round(Px(16.0f));
            ImGui::GetWindowDrawList()->ChannelsSplit(2);
            ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
            m_open = true;
        }

        std::string m_filter;
        int m_page = 0;
        bool m_pageShown = false;
        const char* m_section = nullptr;
        int m_columns = 1;
        bool m_open = false;
        int m_rows = 0;
        int m_shown = 0;
        bool m_below = false;
        float m_left = 0.0f;
        float m_right = 0.0f;
        float m_top = 0.0f;
        float m_lineTop = 0.0f;
        float m_lineBottom = 0.0f;
        float m_rowBottom = 0.0f;
        float m_rowPadding = 0.0f;
        float m_padding = 0.0f;
        float m_controlWidth = 0.0f;
    };

    const char* SettingsWindow::PageIcon(int page)
    {
        return page >= 0 && page < kPageCount ? kPages[page].Icon : "";
    }

    const char* SettingsWindow::PageTitle(int page)
    {
        return page >= 0 && page < kPageCount ? kPages[page].Title : "";
    }

    const char* SettingsWindow::PageSubtitle(int page)
    {
        return page >= 0 && page < kPageCount ? kPages[page].Subtitle : "";
    }

    SettingsWindow::SettingsWindow(Workspace& workspace) : m_workspace(workspace), m_themeNameDialog("themename", ICON_FA_PALETTE, "New Theme", ImVec2(440.0f, 0.0f))
    {
    }

    void SettingsWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        const bool visible = BeginToolWindow(ICON_FA_COG "  Settings###settingspages", &m_open, ImVec2(880.0f, 640.0f), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
            ImVec2(600.0f, 420.0f));
        ImGui::PopStyleVar();
        if (!visible)
        {
            EndToolWindow();
            return;
        }

        Settings& settings = m_workspace.Core().Options();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float navWidth = std::round(available.x < Px(640.0f) ? Px(56.0f) : Px(210.0f));
        Navigation(navWidth, available.y);
        ImGui::SameLine(0.0f, 0.0f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(26.0f, 22.0f));
        ImGui::BeginChild("##settingspage", ImVec2(0.0f, available.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        if (m_scrollTop)
        {
            ImGui::SetScrollY(0.0f);
            m_scrollTop = false;
        }

        SettingsList list(Trim(m_search));
        bool changed = false;
        bool themeEdited = false;
        const auto page = [&](int index) {
            list.Page(index);
            switch (index)
            {
            case 0: changed |= GeneralPage(list, settings); break;
            case 1: themeEdited |= AppearancePage(list, settings); break;
            case 2: changed |= ScannerPage(list, settings); break;
            case 3: changed |= ScriptsPage(list, settings); break;
            case 4: changed |= DebuggerPage(list, settings); break;
            case 5: changed |= SignatureOptionsPage(list, settings); break;
            default: changed |= ShortcutsPage(list, settings); break;
            }
        };
        if (!list.Searching())
        {
            PageHeader(settings);
            page(m_page);
        }
        else
        {
            for (int index = 0; index < kPageCount; ++index)
                page(index);
        }
        list.EndSection();
        if (list.Searching() && list.Shown() == 0)
            EmptyState(ICON_FA_SEARCH, "Nothing found", "No setting matches \"" + list.Filter() + "\".", {}, false);
        ImGui::EndChild();
        ThemeNameDialog(settings);

        if (m_themeReset)
        {
            m_themeReset = false;
            ResetOwnColors(settings.Theme);
            themeEdited = true;
            m_themeColorsEdited = true;
        }
        if (m_themeColorsEdited)
        {
            m_themeColorsEdited = false;
            FinishThemeEdit(settings);
        }
        if (m_shortcutsReset)
        {
            m_shortcutsReset = false;
            m_captureCommand = -1;
            ApplyShortcutSettings({});
            settings.Shortcuts.clear();
            changed = true;
        }
        if (themeEdited)
        {
            ManifoldTheme::Configure(settings.Theme);
            m_workspace.RequestRestyle();
            changed = true;
        }
        m_workspace.Core().Debugger().SetLogDebugStrings(settings.LogDebugStrings);
        m_workspace.Core().Debugger().SetPreferHardware(settings.UseHardwareExecuteBreakpoints);
        if (changed)
            m_workspace.Core().SaveOptionsSoon();
        const SettingsStore& store = m_workspace.Core().OptionsStore();
        if (!m_seenSaves)
            m_seenSaves = store.Saves();
        if (store.Saves() != *m_seenSaves)
        {
            m_seenSaves = store.Saves();
            m_saveError = store.LastResult().Ok() ? "" : store.LastResult().Text;
            m_savedAt = ImGui::GetTime();
        }
        EndToolWindow();
    }

    void SettingsWindow::Navigation(float width, float height)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Sidebar);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(10.0f, 14.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild("##settingsnav", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        const bool narrow = width < Px(100.0f);
        const float inner = ImGui::GetContentRegionAvail().x;
        if (!narrow)
        {
            const ImVec2 field = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemWidth(inner);
            if (!m_search.empty())
                ImGui::SetNextItemAllowOverlap();
            if (InputTextWithHint("##settingssearch", ICON_FA_SEARCH "  Search settings", m_search))
                m_scrollTop = true;
            if (!m_search.empty())
            {
                const float frame = ImGui::GetFrameHeight();
                const ImVec2 after = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(field.x + inner - frame, field.y));
                {
                    const GhostButtons ghost(ManifoldTheme::Muted);
                    if (ImGui::Button(ICON_FA_TIMES "##clearsearch", ImVec2(frame, frame)))
                    {
                        m_search.clear();
                        m_scrollTop = true;
                    }
                }
                ItemTooltip("Clear the search");
                ImGui::SetCursorScreenPos(after);
            }
            ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
        }
        const bool searching = !Trim(m_search).empty();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float itemHeight = std::round(Px(34.0f));
        const float line = ImGui::GetTextLineHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, std::round(Px(2.0f))));
        for (int index = 0; index < kPageCount; ++index)
        {
            const PageInfo& info = kPages[index];
            ImGui::PushID(index);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##page", ImVec2(inner, itemHeight)))
            {
                m_page = index;
                m_search.clear();
                m_scrollTop = true;
            }
            if (narrow)
                ItemTooltip(info.Title);
            const bool hovered = ImGui::IsItemHovered();
            const bool selected = !searching && m_page == index;
            const ImVec2 max(pos.x + inner, pos.y + itemHeight);
            const float rounding = Px(6.0f);
            if (selected)
            {
                draw->AddRectFilled(pos, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.13f)), rounding);
                draw->AddRectFilled(ImVec2(pos.x, pos.y + Px(9.0f)), ImVec2(pos.x + Px(3.0f), max.y - Px(9.0f)), ImGui::GetColorU32(ManifoldTheme::Accent), Px(2.0f));
            }
            else if (hovered)
            {
                draw->AddRectFilled(pos, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.05f)), rounding);
            }
            const float iconSlot = Px(20.0f);
            const ImVec2 iconSize = ImGui::CalcTextSize(info.Icon);
            const float textY = std::round(pos.y + (itemHeight - line) * 0.5f);
            const float iconX = narrow ? std::round(pos.x + (inner - iconSize.x) * 0.5f) : std::round(pos.x + Px(12.0f) + (iconSlot - iconSize.x) * 0.5f);
            draw->AddText(ImVec2(iconX, textY), ImGui::GetColorU32(selected ? ManifoldTheme::Accent : (hovered ? ManifoldTheme::Text : ManifoldTheme::Muted)), info.Icon);
            if (!narrow)
                draw->AddText(ImVec2(std::round(pos.x + Px(12.0f) + iconSlot + Px(10.0f)), textY), ImGui::GetColorU32(selected || hovered ? ManifoldTheme::Text : ManifoldTheme::Label), info.Title);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();

        const ImVec2 windowPos = ImGui::GetWindowPos();
        const float bottom = windowPos.y + ImGui::GetWindowHeight() - Px(14.0f);
        const float left = windowPos.x + Px(14.0f);
        const double since = ImGui::GetTime() - m_savedAt;
        if (!m_saveError.empty())
        {
            ImGui::SetCursorScreenPos(ImVec2(left, bottom - line * 2.0f - Px(4.0f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Error);
            ImGui::TextUnformatted(narrow ? ICON_FA_EXCLAMATION_TRIANGLE : ICON_FA_EXCLAMATION_TRIANGLE "  Not saved");
            ImGui::PopStyleColor();
            ItemTooltip(m_saveError.c_str());
        }
        else if (since < 2.5)
        {
            const float alpha = since < 1.8 ? 1.0f : static_cast<float>(1.0 - (since - 1.8) / 0.7);
            ImGui::SetCursorScreenPos(ImVec2(left, bottom - line * 2.0f - Px(4.0f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, alpha));
            ImGui::TextUnformatted(narrow ? ICON_FA_CHECK : ICON_FA_CHECK "  Saved");
            ImGui::PopStyleColor();
        }
        else if (!narrow)
        {
            ImGui::SetCursorScreenPos(ImVec2(left, bottom - line * 2.0f - Px(4.0f)));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            ImGui::TextUnformatted("Changes save right away");
            ImGui::PopStyleColor();
        }
        if (!narrow)
        {
            ImGui::SetCursorScreenPos(ImVec2(left, bottom - line));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.7f));
            ImGui::TextUnformatted(("Manifold Engine " + VersionText()).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + width - Px(1.0f), origin.y), ImVec2(origin.x + width - Px(1.0f), origin.y + height),
            ImGui::GetColorU32(ManifoldTheme::CardBorder), Px(1.0f));
    }

    void SettingsWindow::PageHeader(Settings& settings)
    {
        const PageInfo& info = kPages[m_page];
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float right = pos.x + ImGui::GetContentRegionAvail().x;
        const float badge = std::round(Px(42.0f));
        draw->AddRectFilled(pos, ImVec2(pos.x + badge, pos.y + badge), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.12f)), Px(10.0f));
        draw->AddRect(pos, ImVec2(pos.x + badge, pos.y + badge), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.35f)), Px(10.0f));
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.45f);
        const ImVec2 iconSize = ImGui::CalcTextSize(info.Icon);
        draw->AddText(ImVec2(std::round(pos.x + (badge - iconSize.x) * 0.5f), std::round(pos.y + (badge - iconSize.y) * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Accent), info.Icon);
        ImGui::PopFont();

        const float textX = pos.x + badge + Px(14.0f);
        const char* headerButton = m_page == 1 ? ICON_FA_UNDO "  Reset Theme" : m_page == KeyboardPage ? ICON_FA_UNDO "  Reset All Keys" : nullptr;
        const float headerButtonWidth = headerButton ? ImGui::CalcTextSize(headerButton).x + ImGui::GetStyle().FramePadding.x * 2.0f + Px(16.0f) : 0.0f;
        const float textRight = std::max(textX + Px(120.0f), right - headerButtonWidth);
        ImGui::PushFont(Fonts::Bold(), ImGui::GetFontSize() * 1.3f);
        const float titleHeight = ImGui::GetTextLineHeight();
        ImGui::SetCursorScreenPos(ImVec2(textX, pos.y + Px(1.0f)));
        ImGui::TextUnformatted(info.Title);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(textX, pos.y + Px(1.0f) + titleHeight + Px(3.0f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + (textRight - textX));
        ImGui::TextUnformatted(info.Subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        const float headerBottom = std::max(pos.y + badge, ImGui::GetItemRectMax().y);

        if (m_page == 1)
        {
            const char* label = ICON_FA_UNDO "  Reset Theme";
            const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(right - width, std::round(pos.y + (badge - ImGui::GetFrameHeight()) * 0.5f)));
            ImGui::BeginDisabled(!OwnColors(settings.Theme) && settings.Theme.Sizes.empty());
            if (ImGui::Button(label))
                m_themeReset = true;
            ImGui::EndDisabled();
            ItemTooltip(settings.Theme.Custom.empty() ? "A preset has nothing to reset." :
                ("Resets this theme's colors for the mode shown, and its sizes, to the " + std::string(Themes::FamilyLabel(settings.Theme.Family)) + " preset.").c_str());
        }
        if (m_page == KeyboardPage)
        {
            const char* label = ICON_FA_UNDO "  Reset All Keys";
            const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(right - width, std::round(pos.y + (badge - ImGui::GetFrameHeight()) * 0.5f)));
            ImGui::BeginDisabled(settings.Shortcuts.empty());
            if (ImGui::Button(label))
                m_shortcutsReset = true;
            ImGui::EndDisabled();
            ItemTooltip("Restores all default shortcuts.");
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x, std::round(headerBottom + Px(26.0f))));
    }

    bool SettingsWindow::ShortcutsPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const bool wasCapturing = m_captureCommand >= 0;
        if (m_captureCommand >= 0)
        {
            KeepEscape();
            SetShortcutCapture(true);
            const Command capturing = static_cast<Command>(m_captureCommand);
            const CommandInfo& info = Describe(capturing);
            const ImGuiKeyChord chord = CapturedChord();
            if (chord == ImGuiKey_Escape)
            {
                m_captureCommand = -1;
            }
            else if (chord)
            {
                const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
                if (!info.Digits || (key >= ImGuiKey_0 && key <= ImGuiKey_9))
                {
                    const ImGuiKeyChord stored = info.Digits ? (chord & ImGuiMod_Mask_) | ImGuiKey_0 : chord;
                    Binding binding = CurrentBinding(capturing);
                    (m_captureSlot == 0 ? binding.Chord : binding.Alternate) = stored;
                    SetBinding(capturing, binding);
                    m_captureCommand = -1;
                    changed = true;
                }
            }
        }

        const float keyWidth = std::round(Px(150.0f));
        const float frame = ImGui::GetFrameHeight();
        const float control = keyWidth * 2.0f + frame + Px(12.0f);
        for (int scope = 0; scope < static_cast<int>(CommandScope::Count); ++scope)
        {
            list.Section(ScopeName(static_cast<CommandScope>(scope)));
            for (const CommandInfo& info : AllCommands())
            {
                if (static_cast<int>(info.Scope) != scope || FromTemplateSettings(info.Id))
                    continue;
                const Binding binding = CurrentBinding(info.Id);
                std::string description;
                for (const ImGuiKeyChord chord : { binding.Chord, binding.Alternate })
                {
                    for (const Command other : ConflictsWith(info.Id, chord))
                        description += "Same keys as \"" + std::string(Describe(other).Name) + "\" here. ";
                }
                if (IsCustomized(info.Id))
                    description += "Default: " + BindingText(info.Id, DefaultBinding(info.Id)) + (info.FromCheatEngine ? " (Cheat Engine)." : ".");
                if (!list.Row(info.Name, description, control))
                    continue;
                ImGui::PushID(info.Key);
                for (int slot = 0; slot < 2; ++slot)
                {
                    const ImGuiKeyChord chord = slot == 0 ? binding.Chord : binding.Alternate;
                    const bool capturing = m_captureCommand == static_cast<int>(info.Id) && m_captureSlot == slot;
                    std::string label = capturing ? "Press keys..." : chord ? ChordText(chord, info.Digits) : slot == 0 ? "none" : "+";
                    label += "##key" + std::to_string(slot);
                    if (slot == 1)
                        ImGui::SameLine(0.0f, Px(6.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, capturing ? ManifoldTheme::Accent : chord ? ManifoldTheme::AddressText : ManifoldTheme::Muted);
                    if (capturing)
                        ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.18f));
                    PushMono();
                    if (ImGui::Button(label.c_str(), ImVec2(keyWidth, 0.0f)) && !(wasCapturing && m_captureCommand < 0))
                    {
                        m_captureCommand = static_cast<int>(info.Id);
                        m_captureSlot = slot;
                    }
                    PopMono();
                    ImGui::PopStyleColor(capturing ? 2 : 1);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && chord)
                    {
                        Binding cleared = binding;
                        (slot == 0 ? cleared.Chord : cleared.Alternate) = 0;
                        SetBinding(info.Id, cleared);
                        m_captureCommand = -1;
                        changed = true;
                    }
                    if (capturing)
                        ItemTooltip(info.Digits ? "Press modifiers with a digit, like Ctrl+Alt+1. Esc cancels." : "Press the new keys. Esc cancels.");
                    else
                        ItemTooltip(slot == 0 ? "Click to change, right-click to remove." : "Second shortcut. Right-click to remove.");
                }
                ImGui::SameLine(0.0f, Px(6.0f));
                if (ResetButton(IsCustomized(info.Id)))
                {
                    SetBinding(info.Id, DefaultBinding(info.Id));
                    m_captureCommand = -1;
                    changed = true;
                }
                ImGui::PopID();
                list.EndRow();
            }
            if (scope == static_cast<int>(CommandScope::ScriptEditor) && list.Row("Template Shortcuts", "Set in each template; the built-in ones use the Cheat Engine keys.", control))
            {
                ImGui::Dummy(ImVec2(control, frame));
                list.EndRow();
            }
        }
        if (changed)
            settings.Shortcuts = ShortcutSettings();
        return changed;
    }

    bool SettingsWindow::GeneralPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();
        const float slider = std::round(Px(230.0f));

        list.Section("Layout");
        if (list.Row("Compact Mode", "Show only the address list.", toggle))
        {
            changed |= ToggleSwitch("##compact", &settings.CompactMode);
            list.EndRow();
        }
        if (list.Row("Release Mode", "Hide addresses, types and developer tools.", toggle))
        {
            changed |= ToggleSwitch("##release", &settings.ReleaseMode);
            list.EndRow();
        }

        if (list.Row("Type Submenus", "Context menus show each group of value types as a submenu.", toggle))
        {
            changed |= ToggleSwitch("##typesubmenus", &settings.TypeSubmenus);
            list.EndRow();
        }

        list.Section("Address List");
        const float addressChoice = std::round(Px(330.0f));
        if (list.Row("Address Column", "As written (\"game.exe\"+1A2B30), resolved, or both.", addressChoice))
        {
            changed |= Segmented("##addresscolumn", &settings.AddressColumn, { "As Written", "Resolved", "Both" }, addressChoice);
            list.EndRow();
        }
        if (list.Row("Address Digits", "Digits of resolved addresses: short, at least 8 like Cheat Engine, or the pointer width.", addressChoice))
        {
            changed |= Segmented("##addressdigits", &settings.AddressDigits, { "Short", "At Least 8", "Pointer Width" }, addressChoice);
            list.EndRow();
        }

        list.Section("Refresh Rates");
        if (list.Row("Address List", "How often its values are read.", slider))
        {
            changed |= Milliseconds("##update", settings.UpdateIntervalMs, 16, 5000, slider);
            list.EndRow();
        }
        if (list.Row("Frozen Values", "How often frozen values are written back.", slider))
        {
            changed |= Milliseconds("##freeze", settings.FreezeIntervalMs, 10, 5000, slider);
            list.EndRow();
        }
        if (list.Row("Found List", "How often found values are read again.", slider))
        {
            changed |= Milliseconds("##results", settings.ResultValueRefreshMs, 50, 10000, slider);
            list.EndRow();
        }

        list.Section("Sounds");
        if (list.Row("Play Sounds", "When a scan or a long search finishes, when a script fails and when a script shows a warning.", toggle))
        {
            changed |= ToggleSwitch("##playsounds", &settings.PlaySounds);
            list.EndRow();
        }
        const float tryWidth = std::round(Px(100.0f));
        if (list.Row("Test Sounds", "Plays a sound once, even when sounds are off.", tryWidth * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f))
        {
            for (const Sounds::Cue cue : { Sounds::Cue::Success, Sounds::Cue::Warning, Sounds::Cue::Error })
            {
                if (cue != Sounds::Cue::Success)
                    ImGui::SameLine();
                if (ImGui::Button((std::string(ICON_FA_PLAY "  ") + Sounds::FileName(cue)).c_str(), ImVec2(tryWidth, 0.0f)))
                    Sounds::Preview(cue);
            }
            list.EndRow();
        }
        const float soundsFolder = std::round(Px(130.0f));
        if (list.Row("Sounds Folder", "Success, Warning and Error .wav or .mp3 here replace the built-in sounds: " + WideToUtf8(Paths::SoundsFolder().wstring()), soundsFolder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(soundsFolder, 0.0f)))
            {
                std::error_code error;
                std::filesystem::create_directories(Paths::SoundsFolder(), error);
                OpenFolder(Paths::SoundsFolder());
            }
            list.EndRow();
        }

        list.Section("Diagnostics");
        if (list.Row("Profiler", "Measures every frame, at a few microseconds per frame.", toggle))
        {
            if (ToggleSwitch("##profiler", &settings.ProfilerEnabled))
            {
                Profiler::SetEnabled(settings.ProfilerEnabled);
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Frame Time in Status Bar", "Frame time and frame rate while the profiler is on.", toggle))
        {
            ImGui::BeginDisabled(!settings.ProfilerEnabled);
            changed |= ToggleSwitch("##profilerstatus", &settings.ProfilerInStatusBar);
            ImGui::EndDisabled();
            list.EndRow();
        }
        const float profilerButton = std::round(Px(210.0f));
        if (list.Row("Profiler and Diagnostics", "Frame graph, threads and setup checks (" + BindingText(Command::OpenProfiler, CurrentBinding(Command::OpenProfiler)) + ").", profilerButton))
        {
            if (ImGui::Button(ICON_FA_TACHOMETER_ALT "  Open Profiler", ImVec2(profilerButton, 0.0f)))
                m_workspace.OpenProfiler();
            list.EndRow();
        }

        if (list.Row("Layout Checks", "Outlines content that is cut off or leaves its place, and lists it (" + BindingText(Command::OpenLayoutProblems, CurrentBinding(Command::OpenLayoutProblems)) + ").", toggle))
        {
            if (ToggleSwitch("##layoutchecks", &settings.LayoutChecks))
            {
                Layout::Audit::SetEnabled(settings.LayoutChecks);
                changed = true;
            }
            list.EndRow();
        }
        const float layoutButton = std::round(Px(210.0f));
        if (list.Row("Layout Problems", "What the layout checks found, per window; hover a row to see it on screen.", layoutButton))
        {
            Layout::ButtonStyle open;
            open.Width = Layout::Size::Fixed(210.0f);
            if (Layout::Button(ICON_FA_RULER_COMBINED "  Open Layout Problems", open))
                m_workspace.OpenLayoutProblems();
            list.EndRow();
        }

        list.Section("Program");
        if (list.Row("Splash Screen", "Logo and start-up steps for at least two seconds; applies at the next start.", toggle))
        {
            changed |= ToggleSwitch("##splash", &settings.ShowSplash);
            list.EndRow();
        }
        if (list.Row("Check for Updates at Start", "Asks GitHub for a newer release when the program starts. Help > Check for Updates asks at any time.", toggle))
        {
            changed |= ToggleSwitch("##updatecheck", &settings.CheckForUpdates);
            list.EndRow();
        }
        const float button = std::round(Px(210.0f));
        if (list.Row("Updates", settings.UpdateSkipped.empty() ? "Version " MANIFOLD_VERSION_TEXT "." : "Version " MANIFOLD_VERSION_TEXT "; version " + settings.UpdateSkipped + " is skipped.", button))
        {
            const bool checking = m_workspace.Updates().Checking();
            Layout::ButtonStyle check;
            check.Width = Layout::Size::Fixed(210.0f);
            ImGui::BeginDisabled(checking);
            if (Layout::Button(checking ? ICON_FA_SYNC "  Checking..." : ICON_FA_SYNC "  Check Now", check))
                m_workspace.Updates().CheckNow();
            ImGui::EndDisabled();
            list.EndRow();
        }
        const bool elevated = Target::IsElevated();
        if (list.Row("Administrator Rights", elevated ? "Running as administrator." : "Some games can only be opened as administrator.",
                button))
        {
            if (elevated)
                RightText(ICON_FA_SHIELD_ALT "  Administrator", button, ManifoldTheme::Accent);
            else if (ImGui::Button(ICON_FA_SHIELD_ALT "  Restart as Administrator", ImVec2(button, 0.0f)))
                m_workspace.Window().RestartElevated();
            list.EndRow();
        }
        const float folder = std::round(Px(130.0f));
        if (list.Row("Settings File", WideToUtf8(Paths::SettingsFile().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(folder, 0.0f)))
                OpenFolder(Paths::SettingsFile().parent_path());
            list.EndRow();
        }
        return changed;
    }

    bool SettingsWindow::AppearancePage(SettingsList& list, Settings& settings)
    {
        ThemeSettings& theme = settings.Theme;
        bool edited = false;
        const float frame = ImGui::GetFrameHeight();
        const float gap = std::round(Px(6.0f));
        const float colorControl = frame * 2.0f + gap;
        const int columns = ImGui::GetContentRegionAvail().x >= Px(560.0f) ? 2 : 1;
        const std::string prefix = ManifoldTheme::CurrentPrefix();
        const bool light = ManifoldTheme::IsLight();

        list.Section("Theme");
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        const bool scripted = library.Scripted();
        // ∑ the owner is a Lua chunk name: "@C:\...\probe.lua" for a file, "=name" for a named chunk
        const std::string& source = library.ScriptOwner();
        const std::string script = source.empty() ? std::string("A script")
            : source[0] == '@' ? WideToUtf8(std::filesystem::path(Utf8ToWide(source.substr(1))).filename().wstring())
            : source[0] == '=' ? source.substr(1) : source;
        if (scripted && list.Row("Changed by a Script", script + " changed the look for this session. Restore it to change the theme here.", -1.0f))
        {
            if (Layout::Button(ICON_FA_UNDO "  Restore", Layout::ButtonKind::Accent))
                library.ClearScript();
            list.EndRow();
        }
        ImGui::BeginDisabled(scripted);
        edited |= ThemeGallery(list, settings);
        const float modeWidth = std::round(Px(250.0f));
        const std::string modeText = std::string("System follows Windows, currently ") + (ManifoldTheme::SystemLight() ? "light" : "dark") + ".";
        if (list.Row("Mode", modeText, modeWidth))
        {
            int mode = 0;
            for (int i = 0; i < static_cast<int>(std::size(kModes)); ++i)
            {
                if (kModes[i] == theme.Mode)
                    mode = i;
            }
            if (Segmented("##mode", &mode, { "Light", "Dark", "System" }, modeWidth))
            {
                theme.Mode = kModes[mode];
                edited = true;
            }
            list.EndRow();
        }
        if (list.Row("Window Corners", "For the main window and floating tool windows.", modeWidth))
        {
            int corners = std::clamp(theme.Corners, 0, 2);
            if (Segmented("##corners", &corners, { "Round", "Slight", "Square" }, modeWidth))
            {
                theme.Corners = corners;
                edited = true;
            }
            list.EndRow();
        }
        const auto opacity = [&](const char* id, int& value) {
            BeginFillSlider(id, static_cast<float>(value - 20) / 80.0f, modeWidth);
            ImGui::SliderInt(id, &value, 20, 100, value >= 100 ? "Opaque" : "%d %%", ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            edited |= ImGui::IsItemDeactivatedAfterEdit();
        };
        if (list.Row("Floating Window Opacity", "Tool windows outside the main window while they have the focus.", modeWidth))
        {
            opacity("##activeopacity", settings.ActiveWindowOpacity);
            list.EndRow();
        }
        if (list.Row("Inactive Window Opacity", "The same windows while another window has the focus, to see the game behind them.", modeWidth))
        {
            opacity("##inactiveopacity", settings.InactiveWindowOpacity);
            list.EndRow();
        }

        const auto colorCell = [&](const std::string& key, const char* label, ImVec4 current) {
            if (!list.Row(label, {}, colorControl))
                return;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ResetButton(Overridden(theme.Colors, key)))
            {
                Erase(theme.Colors, key);
                edited = true;
                m_themeColorsEdited = true;
            }
            ImGui::SetCursorScreenPos(ImVec2(pos.x + frame + gap, pos.y));
            float rgba[4] = { current.x, current.y, current.z, current.w };
            const bool before = edited;
            if (ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
            {
                Erase(theme.Colors, key);
                theme.Colors[key] = { rgba[0], rgba[1], rgba[2], rgba[3] };
                edited = true;
            }
            m_themeColorsEdited |= edited && !before;
            list.EndRow();
        };
        const auto paletteCells = [&](const char* const* names, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i)
            {
                if (const ManifoldTheme::PaletteColor* color = FindPalette(names[i]))
                    colorCell(prefix + color->Name, color->Label, *color->Value);
            }
        };

        list.Section("Accent");
        const float dot = std::round(Px(22.0f));
        const float presetsWidth = static_cast<float>(std::size(kAccents)) * (dot + gap) - gap;
        if (list.Row("Accent Color", "Buttons, switches, selections and highlights.", presetsWidth))
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            for (int i = 0; i < static_cast<int>(std::size(kAccents)); ++i)
            {
                const AccentPreset& preset = kAccents[i];
                const ImVec4 shown = PresetAccent(i, light);
                const ImVec2 pos(start.x + static_cast<float>(i) * (dot + gap), start.y + std::round((frame - dot) * 0.5f));
                ImGui::SetCursorScreenPos(pos);
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("##preset", ImVec2(dot, dot)))
                {
                    Erase(theme.Colors, prefix + "Accent");
                    Erase(theme.Colors, prefix + "AccentHovered");
                    if (i > 0)
                    {
                        const ImVec4 hovered = PresetHovered(shown, light);
                        theme.Colors[prefix + "Accent"] = { shown.x, shown.y, shown.z, 1.0f };
                        theme.Colors[prefix + "AccentHovered"] = { hovered.x, hovered.y, hovered.z, 1.0f };
                    }
                    edited = true;
                    m_themeColorsEdited = true;
                }
                ItemTooltip(i == 0 ? (std::string(ManifoldTheme::FamilyLabel(theme.Family)) + " theme's own").c_str() : preset.Name);
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                const ImVec2 center(pos.x + dot * 0.5f, pos.y + dot * 0.5f);
                draw->AddCircleFilled(center, dot * 0.5f - Px(2.0f), ImGui::GetColorU32(shown), 32);
                if (SameColor(ManifoldTheme::Accent, shown))
                    draw->AddCircle(center, dot * 0.5f, ImGui::GetColorU32(ManifoldTheme::Text), 32, Px(1.5f));
                else if (hovered)
                    draw->AddCircle(center, dot * 0.5f, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.4f)), 32, Px(1.0f));
            }
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y));
            ImGui::Dummy(ImVec2(presetsWidth, frame));
            list.EndRow();
        }
        paletteCells(kAccentColors, std::size(kAccentColors));

        list.Section("Surfaces", columns);
        paletteCells(kSurfaceColors, std::size(kSurfaceColors));
        list.Section("Text", columns);
        paletteCells(kTextColors, std::size(kTextColors));
        list.Section("Code and Memory", columns);
        paletteCells(kCodeColors, std::size(kCodeColors));

        list.Section("Sizes and Spacing");
        const float slider = std::round(Px(200.0f));
        for (const ManifoldTheme::StyleSize& size : ManifoldTheme::kSizes)
        {
            if (!list.Row(size.Label, {}, slider + frame + gap))
                continue;
            float value = size.Default;
            for (const auto& [name, stored] : theme.Sizes)
            {
                if (EqualsNoCase(name, size.Name))
                    value = stored;
            }
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ResetButton(Overridden(theme.Sizes, size.Name)))
            {
                Erase(theme.Sizes, size.Name);
                edited = true;
                m_themeColorsEdited = true;
            }
            ImGui::SetCursorScreenPos(ImVec2(pos.x + frame + gap, pos.y));
            BeginFillSlider("##size", (value - size.Min) / (size.Max - size.Min), slider);
            const bool moved = ImGui::SliderFloat("##size", &value, size.Min, size.Max, size.Max <= 2.0f ? "%.2f" : "%.0f");
            EndFillSlider();
            if (moved)
            {
                Erase(theme.Sizes, size.Name);
                theme.Sizes[size.Name] = value;
                edited = true;
                m_themeColorsEdited = true;
            }
            list.EndRow();
        }

        list.Section("Advanced");
        if (list.Row("All ImGui Colors", "Colors set there override the ones above.", ToggleSwitchWidth()))
        {
            ToggleSwitch("##allcolors", &m_allImGuiColors);
            list.EndRow();
        }
        if (m_allImGuiColors || list.Searching())
        {
            list.Section("ImGui Colors", columns);
            const ImGuiStyle& style = ImGui::GetStyle();
            for (int i = 0; i < ImGuiCol_COUNT; ++i)
                colorCell(prefix + "ImGui." + ImGui::GetStyleColorName(i), ImGui::GetStyleColorName(i), style.Colors[i]);
        }
        ImGui::EndDisabled();
        return edited;
    }

    bool SettingsWindow::ThemeGallery(SettingsList& list, Settings& settings)
    {
        ThemeSettings& theme = settings.Theme;
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        const bool light = ManifoldTheme::IsLight();
        bool edited = false;
        if (ImGui::GetTime() - m_themesScannedAt > 2.0)
        {
            // ∑ files dropped into the themes folder show up while the page is open
            m_themesScannedAt = ImGui::GetTime();
            library.Refresh();
        }
        const std::vector<Themes::CustomTheme> custom = library.Custom();  // a copy: menu actions rescan the library
        if (list.Row("Theme", "Presets stay as they are: changing one makes a custom theme. Right-click a custom theme to rename, export or delete it.", -1.0f))
        {
            const float space = std::round(Px(12.0f));
            const float width = std::floor(std::min(Px(150.0f), (list.ControlWidth() - space * 3.0f) / 4.0f));
            const float height = std::round(width * 0.62f);
            const float step = height + ImGui::GetTextLineHeight() + Px(8.0f) + space;
            const int perRow = std::max(1, static_cast<int>((list.ControlWidth() + space) / (width + space)));
            const int count = static_cast<int>(std::size(kThemeFamilies) + custom.size()) + 1;
            const ImVec2 start = ImGui::GetCursorScreenPos();
            int index = 0;
            const auto place = [&]() {
                ImGui::SetCursorScreenPos(ImVec2(start.x + static_cast<float>(index % perRow) * (width + space), start.y + static_cast<float>(index / perRow) * step));
                ++index;
            };
            const std::string mode = light ? ", light" : ", dark";
            for (const ThemeFamily family : kThemeFamilies)
            {
                place();
                ImGui::PushID(static_cast<int>(family));
                const bool selected = theme.Custom.empty() && theme.Family == family;
                const char* label = Themes::FamilyLabel(family);
                if (ThemeCard("##preset", label, ManifoldTheme::PaletteOf(family, light), selected, std::string(label) + " preset" + mode, width, height) && !selected)
                {
                    SelectTheme(settings, {}, family);
                    edited = true;
                }
                ImGui::PopID();
            }
            for (const Themes::CustomTheme& entry : custom)
            {
                place();
                ImGui::PushID(entry.Id.c_str());
                const bool selected = EqualsNoCase(theme.Custom, entry.Id);
                const ManifoldTheme::Palette palette = CustomPalette(selected ? Themes::FromSettings(theme) : entry, light);
                const std::string tooltip = entry.Name + ", based on " + Themes::FamilyLabel(entry.Base) + mode;
                if (ThemeCard("##custom", entry.Name, palette, selected, tooltip, width, height) && !selected)
                {
                    SelectTheme(settings, entry.Id, entry.Base);
                    edited = true;
                }
                ThemeCardMenu(entry, settings);
                ImGui::PopID();
            }
            place();
            if (NewThemeCard(width, height))
            {
                const std::string name = theme.Custom.empty() ? std::string(Themes::FamilyLabel(theme.Family)) + " (Custom)" : theme.CustomName + " Copy";
                OpenThemeName(NameNew, {}, library.UniqueName(name));
            }
            if (ImGui::BeginPopupContextItem("##newthememenu"))
            {
                if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Import Theme..."))
                    ImportTheme(settings);
                ImGui::EndPopup();
            }
            const int rows = (count + perRow - 1) / perRow;
            ImGui::SetCursorScreenPos(start);
            ImGui::Dummy(ImVec2(static_cast<float>(std::min(perRow, count)) * (width + space) - space, static_cast<float>(rows) * step - space));
            list.EndRow();
        }
        if (!m_themeNotice.empty() && list.Row("New Custom Theme", m_themeNotice, -1.0f))
        {
            if (Layout::Button("OK"))
                m_themeNotice.clear();
            list.EndRow();
        }
        const std::vector<std::string>& problems = library.Problems();
        const std::string files = problems.empty() ? std::string("Each custom theme is an .ini file in the themes folder; copy one to share it.")
            : Format("%zu problem%s in the themes folder, the first: ", problems.size(), problems.size() == 1 ? "" : "s") + problems.front();
        if (list.Row("Theme Files", files, -1.0f))
        {
            if (Layout::Button(ICON_FA_FILE_IMPORT "  Import..."))
                ImportTheme(settings);
            ImGui::SameLine();
            if (Layout::Button(ICON_FA_FOLDER_OPEN "  Open Folder"))
                OpenFolder(library.Folder());
            list.EndRow();
        }
        return edited;
    }

    void SettingsWindow::ThemeCardMenu(const Themes::CustomTheme& entry, Settings& settings)
    {
        if (!ImGui::BeginPopupContextItem("##thememenu"))
            return;
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        if (ImGui::MenuItem(ICON_FA_PEN "  Rename..."))
            OpenThemeName(NameRename, entry.Id, entry.Name);
        if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate..."))
            OpenThemeName(NameDuplicate, entry.Id, library.UniqueName(entry.Name + " Copy"));
        if (ImGui::MenuItem(ICON_FA_FILE_EXPORT "  Export..."))
        {
            m_workspace.Report(library.SaveActive(settings.Theme));
            if (const auto path = m_workspace.SaveFileDialog({ { L"Manifold themes (*.ini)", L"*.ini" } }, Themes::StemFor(entry.Name) + ".ini", L"ini"))
                m_workspace.Report(library.Export(entry.Id, *path));
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TRASH_ALT "  Delete..."))
        {
            const std::string id = entry.Id;
            m_workspace.Confirm("Delete Theme", "Move the theme " + entry.Name + " to the recycle bin? The theme shown switches to its preset if it is this one.",
                [this, id]() {
                    Settings& options = m_workspace.Core().Options();
                    Themes::Library& themes = m_workspace.Core().ThemeLibrary();
                    if (EqualsNoCase(options.Theme.Custom, id))
                    {
                        Themes::SelectPreset(options.Theme, options.Theme.Family);
                        m_workspace.RequestRestyle();
                        m_workspace.Core().SaveOptionsSoon();
                    }
                    m_workspace.Report(themes.Remove(id));
                },
                "Delete");
        }
        ImGui::EndPopup();
    }

    void SettingsWindow::OpenThemeName(int action, const std::string& source, const std::string& name)
    {
        m_themeNaming = action;
        m_themeNameSource = source;
        m_themeName = name;
        m_themeNameDialog.SetTitle(action == NameRename ? "Rename Theme" : action == NameDuplicate ? "Duplicate Theme" : "New Theme");
        m_themeNameDialog.Open();
    }

    void SettingsWindow::ThemeNameDialog(Settings& settings)
    {
        if (!m_themeNameDialog.Begin())
            return;
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        ThemeSettings& theme = settings.Theme;
        Layout::Field("Name", { .Placement = Layout::LabelPlacement::Above });
        if (m_themeNameDialog.Appearing())
            ImGui::SetKeyboardFocusHere();
        InputText("##themename", m_themeName);
        const std::string name = Trim(m_themeName);
        const bool taken = !name.empty() && library.NameTaken(name, m_themeNaming == NameRename ? m_themeNameSource : std::string());
        if (taken)
            Layout::Colored("A theme or preset already has this name.", ManifoldTheme::Error);
        else
            Layout::Muted(m_themeNaming == NameNew ? "The new theme starts as a copy of the one shown and is selected." : "Saved as " + Themes::StemFor(name) + ".ini in the themes folder.");
        const DialogChoice choice = m_themeNameDialog.Buttons(m_themeNaming == NameRename ? "Rename" : "Create", !name.empty() && !taken);
        if (choice == DialogChoice::Ok)
        {
            Status status = library.SaveActive(theme);
            if (m_themeNaming == NameRename)
            {
                std::string id;
                status = library.Rename(m_themeNameSource, name, &id);
                if (status.Ok() && EqualsNoCase(theme.Custom, m_themeNameSource))
                {
                    theme.Custom = id;
                    theme.CustomName = name;
                    m_workspace.Core().SaveOptionsSoon();
                }
            }
            else
            {
                Themes::CustomTheme created;
                const Themes::CustomTheme* source = library.Find(m_themeNameSource);
                if (m_themeNaming == NameNew || (source && EqualsNoCase(source->Id, theme.Custom)))
                    created = theme.Custom.empty() && m_themeNaming == NameNew ? Themes::CustomTheme{ {}, {}, theme.Family } : Themes::FromSettings(theme);
                else if (source)
                    created = *source;
                created.Id.clear();
                created.Name = name;
                status = library.Save(created);
                if (status.Ok() && m_themeNaming == NameNew)
                {
                    SelectTheme(settings, created.Id, created.Base);
                    m_workspace.RequestRestyle();
                    m_workspace.Core().SaveOptionsSoon();
                }
            }
            m_workspace.Report(status);
            m_themeNameDialog.Close();
        }
        else if (choice == DialogChoice::Cancel)
            m_themeNameDialog.Close();
        m_themeNameDialog.End();
    }

    void SettingsWindow::SelectTheme(Settings& settings, const std::string& custom, ThemeFamily family)
    {
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        m_workspace.Report(library.SaveActive(settings.Theme));
        m_themeNotice.clear();
        if (custom.empty())
        {
            Themes::SelectPreset(settings.Theme, family);
            return;
        }
        settings.Theme.Custom = custom;
        settings.Theme.Family = family;
        m_workspace.Report(library.Activate(settings.Theme));
    }

    void SettingsWindow::FinishThemeEdit(Settings& settings)
    {
        ThemeSettings& theme = settings.Theme;
        if (!theme.Custom.empty())
        {
            theme.Dirty = true;
            return;
        }
        // ∑ presets stay read-only: the first change on one makes a custom theme from it
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        Themes::CustomTheme created = Themes::FromSettings(theme);
        created.Id.clear();
        created.Name = library.UniqueName(std::string(Themes::FamilyLabel(theme.Family)) + " (Custom)");
        const Status saved = library.Save(created);
        if (!saved.Ok())
        {
            m_workspace.Report(saved);
            Themes::SelectPreset(theme, theme.Family);
            return;
        }
        theme.Custom = created.Id;
        theme.CustomName = created.Name;
        theme.Dirty = false;
        m_themeNotice = "Your change is in the new theme " + created.Name + "; the " + Themes::FamilyLabel(theme.Family) + " preset stays as it was.";
    }

    void SettingsWindow::ImportTheme(Settings& settings)
    {
        (void)settings;
        Themes::Library& library = m_workspace.Core().ThemeLibrary();
        const auto path = m_workspace.OpenFileDialog({ { L"Manifold themes (*.ini)", L"*.ini" }, { L"All files", L"*.*" } });
        if (!path)
            return;
        std::string name;
        const Status imported = library.Import(*path, &name);
        m_workspace.Report(imported);
        if (imported.Ok())
            m_workspace.Core().Notify(LogLevel::Success, "Imported the theme " + name + ". Click its card to use it.");
    }

    namespace
    {
        std::string ResultMemoryLabel(int megabytes)
        {
            if (megabytes == Scan::MemoryBudget::kAuto)
                return "Auto (up to " + ByteSize(Scan::MemoryBudget::AutoBytes()) + ")";
            if (megabytes == Scan::MemoryBudget::kDiskOnly)
                return "Disk Only";
            return ByteSize(static_cast<std::uint64_t>(megabytes) * 1024 * 1024);
        }
    }

    bool SettingsWindow::ResultMemoryCombo(Settings& settings, float width)
    {
        bool changed = false;
        Layout::FieldOptions field;
        field.Placement = Layout::LabelPlacement::None;
        field.Width = width;
        Layout::Field({}, field);
        const std::string shown = ResultMemoryLabel(settings.ScanMemoryMb);
        if (ImGui::BeginCombo("##resultmemory", shown.c_str()))
        {
            for (const int megabytes : { Scan::MemoryBudget::kAuto, Scan::MemoryBudget::kDiskOnly, 1024, 2048, 4096, 8192, 16384, 32768, 65536 })
            {
                if (ImGui::Selectable(ResultMemoryLabel(megabytes).c_str(), settings.ScanMemoryMb == megabytes))
                {
                    settings.ScanMemoryMb = megabytes;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool SettingsWindow::ScannerPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();
        const float slider = std::round(Px(230.0f));

        list.Section("Scan Options");
        if (list.Row("Scan Threads", "Auto uses half the processor threads, 2 to 8.", slider))
        {
            const std::string format = settings.ScanThreads == 0 ? "Auto (" + std::to_string(AutoThreads()) + ")" : (settings.ScanThreads == 1 ? "1 thread" : "%d threads");
            BeginFillSlider("##threads", static_cast<float>(settings.ScanThreads) / 64.0f, slider);
            ImGui::SliderInt("##threads", &settings.ScanThreads, 0, 64, format.c_str(), ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            list.EndRow();
        }
        if (list.Row("Fast Scan", "Aligned addresses only: much faster, finds nearly every value.", toggle))
        {
            changed |= ToggleSwitch("##fast", &settings.FastScan);
            list.EndRow();
        }
        if (list.Row("Pause While Scanning", "Suspends the process so values can't change mid-scan.", toggle))
        {
            changed |= ToggleSwitch("##pause", &settings.PauseWhileScanning);
            list.EndRow();
        }

        list.Section("Types for All");
        for (const Scan::ValueType type : Scan::kAllTypes)
        {
            const std::uint32_t bit = Scan::AllTypeBit(type);
            if (list.Row(Scan::TypeName(type), {}, toggle))
            {
                bool on = (settings.AllTypes & bit) != 0;
                if (ToggleSwitch(("##all" + std::to_string(bit)).c_str(), &on))
                {
                    settings.AllTypes = on ? (settings.AllTypes | bit) : (settings.AllTypes & ~bit);
                    changed = true;
                }
                list.EndRow();
            }
        }

        list.Section("Memory to Scan");
        if (list.Row("Private Memory", "Heap and other allocations, where most values live.", toggle))
        {
            changed |= ToggleSwitch("##private", &settings.ScanPrivate);
            list.EndRow();
        }
        if (list.Row("Image Memory", "Code and static data of the loaded modules.", toggle))
        {
            changed |= ToggleSwitch("##image", &settings.ScanImage);
            list.EndRow();
        }
        if (list.Row("Mapped Memory", "Shared and file-backed memory; rarely needed, slower.", toggle))
        {
            changed |= ToggleSwitch("##mapped", &settings.ScanMapped);
            list.EndRow();
        }
        // ∑ Guard pages stay out without a switch: reading one clears its guard, and a thread stack
        //   that grows through it then crashes the game.
        if (list.Row("Skip No-Cache Memory", "PAGE_NOCACHE pages are device memory; reading them is slow and never finds game values.", toggle))
        {
            changed |= ToggleSwitch("##skipnocache", &settings.ScanSkipNoCache);
            list.EndRow();
        }
        if (list.Row("Skip Write-Combined Memory", "PAGE_WRITECOMBINE pages are GPU upload buffers; reading them is very slow.", toggle))
        {
            changed |= ToggleSwitch("##skipwritecombine", &settings.ScanSkipWriteCombine);
            list.EndRow();
        }

        list.Section("Storage");
        if (list.Row("Results in Memory", "Scan results stay in RAM up to this size, the rest goes to the scan files. Free RAM is always left for the game.", slider))
        {
            changed |= ResultMemoryCombo(settings, slider);
            list.EndRow();
        }
        const float folder = std::round(Px(130.0f));
        if (list.Row("Scan Files", WideToUtf8(Paths::ScanFolder().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(folder, 0.0f)))
                OpenFolder(Paths::ScanFolder());
            list.EndRow();
        }
        return changed;
    }

    bool SettingsWindow::ScriptsPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();
        const float folder = std::round(Px(130.0f));
        const float choice = std::round(Px(250.0f));

        list.Section("Lua");
        if (list.Row("Run Autorun Modules", "Run the scripts in the autorun folder at start.", toggle))
        {
            changed |= ToggleSwitch("##autorun", &settings.AutorunEnabled);
            list.EndRow();
        }
        if (list.Row("Hold New Modules", "New autorun files only run once allowed in Extensions > Modules.", toggle))
        {
            changed |= ToggleSwitch("##holdmodules", &settings.HoldNewModules);
            list.EndRow();
        }
        if (list.Row("Autorun Folder", WideToUtf8(Paths::AutorunFolder().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(folder, 0.0f)))
                OpenFolder(Paths::AutorunFolder());
            list.EndRow();
        }
        if (list.Row("Callback Time Limit", "Timers, hotkeys and events that run longer are stopped.", choice))
        {
            changed |= Milliseconds("##callbacklimit", settings.ScriptCallbackLimitMs, 250, 60000, choice);
            list.EndRow();
        }
        if (list.Row("Script Windows in App Style", "Off: createForm windows keep the script's colors and fonts, like Cheat Engine.", toggle))
        {
            changed |= ToggleSwitch("##scriptapplook", &settings.ScriptWindowsAppLook);
            list.EndRow();
        }

        list.Section("Script Editors");
        if (list.Row("Open as Tabs", "New script editors open as tabs in one window.", toggle))
        {
            changed |= ToggleSwitch("##editortabs", &settings.ScriptEditorTabs);
            list.EndRow();
        }

        list.Section("Auto Assembler");
        if (list.Row("Log Auto Assembler Steps", "Each command writes what it did to the Log window; {$log} in a script does it for that script only.", toggle))
        {
            changed |= ToggleSwitch("##aalog", &settings.AutoAssemblerLog);
            list.EndRow();
        }

        list.Section("Tables");
        if (list.Row("Table Lua Script", "When a loaded table has a Lua script.", choice))
        {
            int policy = static_cast<int>(settings.TableScripts);
            if (Segmented("##policy", &policy, { "Ask", "Always", "Never" }, choice))
            {
                settings.TableScripts = static_cast<TableScriptPolicy>(policy);
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Table File Names", "CE 7.5 keeps names readable for older Cheat Engine; CE 7.6 needs 7.6 or newer.", choice))
        {
            int naming = std::clamp(settings.TableFileNames, 0, 2);
            if (Segmented("##filenames", &naming, { "As Loaded", "CE 7.5", "CE 7.6" }, choice))
            {
                settings.TableFileNames = naming;
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Active Scripts on Quit", "When the program closes with scripts still active.", choice))
        {
            int quit = static_cast<int>(settings.ActiveScriptsOnQuit);
            if (Segmented("##quitpolicy", &quit, { "Ask", "Turn Off", "Leave" }, choice))
            {
                settings.ActiveScriptsOnQuit = static_cast<QuitPolicy>(quit);
                changed = true;
            }
            list.EndRow();
        }

        list.Section("Backups");
        if (list.Row("Session Backups", "Back up unsaved changes regularly; unapplied editor text is not included.", toggle))
        {
            changed |= ToggleSwitch("##backups", &settings.BackupsEnabled);
            list.EndRow();
        }
        if (list.Row("Backup Interval", "Only when something changed.", choice))
        {
            changed |= LogSlider("##backupinterval", settings.BackupIntervalMinutes, 1, 240, settings.BackupIntervalMinutes == 1 ? "%d minute" : "%d minutes", choice);
            list.EndRow();
        }
        if (list.Row("Backups to Keep", "Per session; the oldest are removed first.", choice))
        {
            changed |= LogSlider("##backupversions", settings.BackupVersions, 1, 500, "%d", choice);
            list.EndRow();
        }
        const float twoButtons = folder * 2.0f + ImGui::GetStyle().ItemSpacing.x;
        if (list.Row("Backups Folder", WideToUtf8(Backups::Folder(settings).wstring()), twoButtons))
        {
            if (ImGui::Button(ICON_FA_FOLDER "  Choose...", ImVec2(folder, 0.0f)))
            {
                if (const auto chosen = m_workspace.FolderDialog())
                {
                    settings.BackupFolder = WideToUtf8(chosen->wstring());
                    changed = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Folder", ImVec2(folder, 0.0f)))
                OpenFolder(Backups::Folder(settings));
            list.EndRow();
        }
        if (!settings.BackupFolder.empty() && list.Row("Default Backups Folder", WideToUtf8(Paths::BackupFolder().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_UNDO "  Use This", ImVec2(folder, 0.0f)))
            {
                settings.BackupFolder.clear();
                changed = true;
            }
            list.EndRow();
        }

        list.Section("Open Processes Automatically");
        if (list.Row("Open on Start", "Open a listed process as soon as it starts.", toggle))
        {
            changed |= ToggleSwitch("##autoattach", &settings.AutoAttachEnabled);
            list.EndRow();
        }
        if (list.Row("Processes", "File names, such as game.exe.", -1.0f))
        {
            const float width = list.ControlWidth();
            const ImGuiStyle& style = ImGui::GetStyle();
            const float frame = ImGui::GetFrameHeight();
            const float gap = std::round(Px(6.0f));
            const ImVec2 start = ImGui::GetCursorScreenPos();
            float x = start.x;
            float y = start.y;
            int remove = -1;
            for (int i = 0; i < static_cast<int>(settings.AutoAttach.size()); ++i)
            {
                const std::string label = settings.AutoAttach[static_cast<std::size_t>(i)] + "   " ICON_FA_TIMES;
                const float chip = ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2.0f;
                if (x > start.x && x + chip > start.x + width)
                {
                    x = start.x;
                    y += frame + gap;
                }
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.13f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.22f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.32f));
                if (ImGui::Button(label.c_str()))
                    remove = i;
                ImGui::PopStyleColor(3);
                ItemTooltip("Remove");
                ImGui::PopID();
                x += chip + gap;
            }
            const float input = std::round(Px(190.0f));
            const char* addLabel = ICON_FA_PLUS "  Add";
            const float add = ImGui::CalcTextSize(addLabel).x + style.FramePadding.x * 2.0f;
            if (x > start.x && x + input + gap + add > start.x + width)
            {
                x = start.x;
                y += frame + gap;
            }
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::SetNextItemWidth(input);
            bool submit = InputTextWithHint("##newprocess", "game.exe", m_autoAttach, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SetCursorScreenPos(ImVec2(x + input + gap, y));
            submit |= ImGui::Button(addLabel);
            const std::string name = Trim(m_autoAttach);
            if (submit && !name.empty())
            {
                const bool known = std::any_of(settings.AutoAttach.begin(), settings.AutoAttach.end(), [&](const std::string& entry) { return EqualsNoCase(entry, name); });
                if (!known)
                    settings.AutoAttach.push_back(name);
                m_autoAttach.clear();
                changed = true;
            }
            if (remove >= 0)
            {
                settings.AutoAttach.erase(settings.AutoAttach.begin() + remove);
                changed = true;
            }
            list.EndRow();
        }
        return changed;
    }

    bool SettingsWindow::SignatureOptionsPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();
        const float choice = std::round(Px(250.0f));
        const float field = std::round(Px(120.0f));
        const auto segmented = [&](const char* id, int& value, std::initializer_list<const char*> names)
            {
                int picked = std::clamp(value, 0, static_cast<int>(names.size()) - 1);
                if (Segmented(id, &picked, std::vector<const char*>(names), choice))
                {
                    value = picked;
                    changed = true;
                }
            };

        list.Section("Search");
        if (list.Row("Unique In", "Module counts matches in the address's module, like aobscanmodule; Process in all memory, like aobscan.", choice))
        {
            segmented("##sigscope", settings.SignatureScope, { "Module", "Process" });
            list.EndRow();
        }
        if (list.Row("Code Only", "Count matches in executable memory only. aobscanmodule itself searches all of the module.", toggle))
        {
            changed |= ToggleSwitch("##sigcode", &settings.SignatureCodeOnly);
            list.EndRow();
        }
        if (list.Row("Longest Signature", "Gives up when no pattern of this many bytes is unique.", choice))
        {
            changed |= LogSlider("##sigmax", settings.SignatureMaxBytes, 8, 1024, "%d bytes", choice);
            list.EndRow();
        }
        if (list.Row("Shortest Signature", "At least this many bytes, so it stays unique when other code changes.", choice))
        {
            BeginFillSlider("##sigmin", static_cast<float>(settings.SignatureMinBytes) / 64.0f, choice);
            ImGui::SliderInt("##sigmin", &settings.SignatureMinBytes, 0, 64, settings.SignatureMinBytes == 0 ? "As short as possible" : "%d bytes", ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            list.EndRow();
        }
        if (list.Row("Stop at the Function End", "Never go past a ret, jmp or int3; the code after it belongs to another function.", toggle))
        {
            changed |= ToggleSwitch("##sigend", &settings.SignatureStopAtFunctionEnd);
            list.EndRow();
        }

        list.Section("Wildcards");
        if (list.Row("Addresses", "RIP-relative and absolute addresses; they move with every build.", toggle))
        {
            changed |= ToggleSwitch("##sigaddresses", &settings.SignatureMaskAddresses);
            list.EndRow();
        }
        if (list.Row("Jump and Call Distances", "Long only wildcards rel32 and keeps the short jumps inside a function.", choice))
        {
            segmented("##sigbranches", settings.SignatureBranches, { "Keep", "Long Only", "All" });
            list.EndRow();
        }
        if (list.Row("Structure Offsets", "The numbers in [rcx+1A8]; Long only wildcards 32-bit offsets and keeps small ones.", choice))
        {
            segmented("##sigoffsets", settings.SignatureOffsets, { "Keep", "Long Only", "All" });
            list.EndRow();
        }
        if (list.Row("Immediates", "Constants such as the 28 in sub rsp,28; Large wildcards values from the limit below up.", choice))
        {
            segmented("##sigimmediates", settings.SignatureImmediates, { "Keep", "Large", "All" });
            list.EndRow();
        }
        if (list.Row("Registers", "The compiler may pick other registers after an update. SIB Only wildcards the base and index of memory operands like [ebx+edi].", choice))
        {
            segmented("##sigregisters", settings.SignatureRegisters, { "Keep", "SIB Only", "ModRM + SIB" });
            list.EndRow();
        }
        if (list.Row("Large Immediate From", "Hex; a value at least this large counts as large.", field))
        {
            Layout::Field("", { .Placement = Layout::LabelPlacement::None, .Width = field });
            if (InputText("##sigthreshold", settings.SignatureImmediateThreshold, ImGuiInputTextFlags_CharsHexadecimal))
                changed = true;
            list.EndRow();
        }

        list.Section("AoB Offset");
        if (list.Row("Allow an AoB Offset", "May start at an instruction before or after the address when that is shorter; the address is then the hit plus the offset.", toggle))
        {
            changed |= ToggleSwitch("##sigoffset", &settings.SignatureOffset);
            list.EndRow();
        }
        if (list.Row("Largest Offset", "How far from the address the pattern may start, in bytes.", choice))
        {
            changed |= LogSlider("##sigmaxoffset", settings.SignatureMaxOffset, 1, 512, "%d bytes", choice);
            list.EndRow();
        }
        if (list.Row("Minimum Saving", "An offset is only used when the pattern gets at least this many bytes shorter.", choice))
        {
            BeginFillSlider("##sigsaving", static_cast<float>(settings.SignatureMinSaving) / 32.0f, choice);
            ImGui::SliderInt("##sigsaving", &settings.SignatureMinSaving, 0, 32, settings.SignatureMinSaving == 1 ? "%d byte" : "%d bytes", ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            list.EndRow();
        }

        list.Section("Output");
        if (list.Row("Wildcard Symbol", "How a wildcard byte is written.", choice))
        {
            segmented("##sigwildcard", settings.SignatureWildcard, { "??", "?", "**", "*" });
            list.EndRow();
        }
        if (list.Row("Spaces Between Bytes", "Without spaces a single ? or * is written twice so the pattern still reads back.", toggle))
        {
            changed |= ToggleSwitch("##sigspaces", &settings.SignatureSpaces);
            list.EndRow();
        }
        if (list.Row("Copy Signature Copies", "What Copy Signature puts on the clipboard.", std::round(Px(330.0f))))
        {
            int format = std::clamp(settings.SignatureCopyFormat, 0, 3);
            if (Segmented("##sigformat", &format, { "AoB", "Code + Mask", "C Array", "Auto Assembler" }, std::round(Px(330.0f))))
            {
                settings.SignatureCopyFormat = format;
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Auto Assembler Label", "The name in the aobscanmodule line.", field))
        {
            Layout::Field("", { .Placement = Layout::LabelPlacement::None, .Width = field });
            if (InputText("##siglabel", settings.SignatureLabel))
                changed = true;
            list.EndRow();
        }
        return changed;
    }

    bool SettingsWindow::DebuggerPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();

        list.Section("Structure Dissect");
        if (list.Row("Open as Tabs", "New Structure Dissect windows open as tabs in one window.", toggle))
        {
            changed |= ToggleSwitch("##dissecttabs", &settings.StructureTabs);
            list.EndRow();
        }
        if (list.Row("Use C++ RTTI", "Name structures and pointers after their C++ class (MSVC, clang-cl).", toggle))
        {
            changed |= ToggleSwitch("##dissectrtti", &settings.StructureRtti);
            list.EndRow();
        }

        list.Section("Breakpoints");
        if (list.Row("Hardware Code Breakpoints", "Use one of the four debug registers and leave the code unchanged.", toggle))
        {
            changed |= ToggleSwitch("##hardware", &settings.UseHardwareExecuteBreakpoints);
            list.EndRow();
        }
        if (list.Row("In-Process Debugger (VEH)", "Also sees threads hidden from debuggers; read the note under About first.", toggle))
        {
            if (ToggleSwitch("##veh", &settings.UseInProcessDebugger))
            {
                if (m_workspace.Core().Debugger().Attached())
                    m_workspace.Core().Debugger().Detach();
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Switch to VEH for Hidden Threads", "When a watch runs into a hidden main thread, move it to the in-process debugger automatically instead of showing the hint.", toggle))
        {
            changed |= ToggleSwitch("##vehauto", &settings.UseInProcessForHiddenThreads);
            list.EndRow();
        }
        if (list.Row("Registers on Break", "Show the registers when a breakpoint hits.", toggle))
        {
            changed |= ToggleSwitch("##registers", &settings.ShowRegistersOnBreak);
            list.EndRow();
        }
        if (list.Row("Add Script Memory to Exception Regions", "Break on Exception > In a Region also stops in the code scripts put into the game.", toggle))
        {
            if (ToggleSwitch("##scriptregions", &settings.BreakInScriptMemory))
            {
                m_workspace.Report(m_workspace.Core().ApplyExceptionBreak());
                changed = true;
            }
            list.EndRow();
        }

        list.Section("Crashes");
        if (list.Row("Find the Script Behind a Crash", "Once a script writes code, a small library in the game notes the exception that ends it.", toggle))
        {
            changed |= ToggleSwitch("##crashwatch", &settings.CrashWatch);
            list.EndRow();
        }

        list.Section("Unity");
        if (list.Row("Start Scene Explorer Automatically", "Starts when a Mono runtime is loaded; its agent runs inside the game.", toggle))
        {
            changed |= ToggleSwitch("##autoexplorer", &settings.AutoExplorer);
            list.EndRow();
        }

        list.Section("Log");
        if (list.Row("Process Debug Output", "Log what the process sends with OutputDebugString.", toggle))
        {
            changed |= ToggleSwitch("##debugstrings", &settings.LogDebugStrings);
            list.EndRow();
        }
        if (list.Row("Debug Messages", "Also log the program's own debug messages.", toggle))
        {
            if (ToggleSwitch("##verbose", &settings.VerboseLog))
            {
                RuntimeLog::SetVerbose(settings.VerboseLog);
                changed = true;
            }
            list.EndRow();
        }
        return changed;
    }
}
