#include "UI/Windows.h"

#include "App/Settings.h"
#include "App/Sounds.h"
#include "Core/Profiler.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/BuildInfo.h"
#include "Generated/IconsFontAwesome5.h"
#include "Target/Process.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
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
            { ICON_FA_SLIDERS_H, "General", "Layout, refresh rates and the program itself." },
            { ICON_FA_PALETTE, "Appearance", "Theme, light or dark, accent, colors and sizes. Changes show at once." },
            { ICON_FA_SEARCH, "Scanner", "The scan options start with these values each time the program opens." },
            { ICON_FA_SCROLL, "Scripts", "Lua at start, scripts inside tables and opening processes by name." },
            { ICON_FA_BUG, "Debugger", "Breakpoints, the in-process debugger and the log." },
            { ICON_FA_KEYBOARD, "Keyboard", "The shortcuts of every window. Click keys to record new ones, right-click to remove them." },
        };
        constexpr int kPageCount = static_cast<int>(std::size(kPages));

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
        const char* const kCodeColors[] = { "AddressText", "BytesText", "CodeKeyword", "CodeRegister", "CodeNumber", "CodeString", "CodeComment", "CodeSection",
            "CodeLabel", "FlowJump", "FlowCall", "FlowReturn", "Breakpoint", "CurrentLine" };

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

        bool ThemeCard(ThemeFamily family, bool selected, bool light, float width, float height)
        {
            const ManifoldTheme::Palette& palette = ManifoldTheme::PaletteOf(family, light);
            const char* label = ManifoldTheme::FamilyLabel(family);
            const float labelHeight = ImGui::GetTextLineHeight() + Px(8.0f);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height + labelHeight));
            const bool hovered = ImGui::IsItemHovered();
            ItemTooltip((std::string(label) + (light ? ", light" : ", dark")).c_str());
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
            const ImVec2 text = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(std::round(pos.x + (width - text.x) * 0.5f), std::round(max.y + Px(5.0f))),
                color(selected ? ManifoldTheme::Text : hovered ? ManifoldTheme::Label : ManifoldTheme::Muted), label);
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

        bool ResetButton(bool visible)
        {
            const float frame = ImGui::GetFrameHeight();
            if (!visible)
            {
                ImGui::Dummy(ImVec2(frame, frame));
                return false;
            }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            const bool pressed = ImGui::Button(ICON_FA_UNDO "##reset", ImVec2(frame, frame));
            ImGui::PopStyleColor(2);
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

        bool Milliseconds(const char* id, int& value, int min, int max, float width)
        {
            BeginFillSlider(id, LogRatio(value, min, max), width);
            ImGui::SliderInt(id, &value, min, max, "%d ms", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            return ImGui::IsItemDeactivatedAfterEdit();
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

    SettingsWindow::SettingsWindow(Workspace& workspace) : m_workspace(workspace)
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

        if (m_themeReset)
        {
            m_themeReset = false;
            ResetOwnColors(settings.Theme);
            themeEdited = true;
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
            m_themeSaveAt = ImGui::GetTime() + 0.6;
        }
        if (m_themeSaveAt > 0.0 && ImGui::GetTime() >= m_themeSaveAt && !ImGui::IsAnyItemActive())
        {
            m_themeSaveAt = 0.0;
            changed = true;
        }
        m_workspace.Core().Debugger().SetLogDebugStrings(settings.LogDebugStrings);
        m_workspace.Core().Debugger().SetPreferHardware(settings.UseHardwareExecuteBreakpoints);
        if (changed)
        {
            const Status status = settings.Save(Paths::SettingsFile());
            m_saveError = status.Ok() ? "" : status.Text;
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
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                if (ImGui::Button(ICON_FA_TIMES "##clearsearch", ImVec2(frame, frame)))
                {
                    m_search.clear();
                    m_scrollTop = true;
                }
                ImGui::PopStyleColor(2);
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
        const char* headerButton = m_page == 1 ? ICON_FA_UNDO "  Reset theme" : m_page == KeyboardPage ? ICON_FA_UNDO "  Reset all keys" : nullptr;
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
            const char* label = ICON_FA_UNDO "  Reset theme";
            const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(right - width, std::round(pos.y + (badge - ImGui::GetFrameHeight()) * 0.5f)));
            ImGui::BeginDisabled(!OwnColors(settings.Theme) && settings.Theme.Sizes.empty());
            if (ImGui::Button(label))
                m_themeReset = true;
            ImGui::EndDisabled();
            ItemTooltip("Back to this theme's own colors and the default sizes; theme and mode stay");
        }
        if (m_page == KeyboardPage)
        {
            const char* label = ICON_FA_UNDO "  Reset all keys";
            const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(right - width, std::round(pos.y + (badge - ImGui::GetFrameHeight()) * 0.5f)));
            ImGui::BeginDisabled(settings.Shortcuts.empty());
            if (ImGui::Button(label))
                m_shortcutsReset = true;
            ImGui::EndDisabled();
            ItemTooltip("Back to the default shortcuts, the Cheat Engine keys and Manifold's own");
        }
        ImGui::SetCursorScreenPos(ImVec2(pos.x, std::round(headerBottom + Px(26.0f))));
    }

    bool SettingsWindow::ShortcutsPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const bool wasCapturing = m_captureCommand >= 0;
        if (m_captureCommand >= 0)
        {
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
                        ItemTooltip(info.Digits ? "Press the modifier keys with a digit, like Ctrl+Alt+1. Esc cancels." : "Press the new keys. Esc cancels.");
                    else
                        ItemTooltip(slot == 0 ? "Click and press the new keys. Right-click removes them." : "A second shortcut for the same command. Right-click removes it.");
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
            if (scope == static_cast<int>(CommandScope::ScriptEditor) && list.Row("Template shortcuts", "Each template has its own Shortcut setting; the built-in ones use the Cheat Engine keys Ctrl+I, Ctrl+Shift+A, Ctrl+Shift+F, Ctrl+L and Ctrl+Alt+T.", control))
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
        if (list.Row("Compact mode", "Show only the address list. Handy while playing with a finished table.", toggle))
        {
            changed |= ToggleSwitch("##compact", &settings.CompactMode);
            list.EndRow();
        }
        if (list.Row("Release mode", "Hide addresses, types and developer tools. View > Release mode switches it back.", toggle))
        {
            changed |= ToggleSwitch("##release", &settings.ReleaseMode);
            list.EndRow();
        }

        list.Section("Refresh rates");
        if (list.Row("Address list", "How often the values in the address list are read.", slider))
        {
            changed |= Milliseconds("##update", settings.UpdateIntervalMs, 16, 5000, slider);
            list.EndRow();
        }
        if (list.Row("Frozen values", "How often frozen values are written back.", slider))
        {
            changed |= Milliseconds("##freeze", settings.FreezeIntervalMs, 10, 5000, slider);
            list.EndRow();
        }
        if (list.Row("Found list", "How often the values in the found list are read again.", slider))
        {
            changed |= Milliseconds("##results", settings.ResultValueRefreshMs, 50, 10000, slider);
            list.EndRow();
        }

        list.Section("Sounds");
        if (list.Row("Play sounds", "A sound plays when a scan finishes, and when a search, a dissect or a patch scan that took longer than two seconds is done. "
                "The error sound plays when a scan fails, when a script you switch on or off (with the mouse or a hotkey) fails, and when Execute, the Lua engine window or the table script ends in an error; "
                "errors in timers and other callbacks stay silent, and it plays at most once a second. "
                "Message boxes from scripts play the error sound for errors and warnings, the other sound for the rest.", toggle))
        {
            changed |= ToggleSwitch("##playsounds", &settings.PlaySounds);
            list.EndRow();
        }
        const float tryWidth = std::round(Px(100.0f));
        if (list.Row("Try them", "Plays the sound once, also when sounds are off. Files you put in the sounds folder are read again.", tryWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x))
        {
            if (ImGui::Button(ICON_FA_PLAY "  Success", ImVec2(tryWidth, 0.0f)))
                Sounds::Preview(Sounds::Cue::Success);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_PLAY "  Error", ImVec2(tryWidth, 0.0f)))
                Sounds::Preview(Sounds::Cue::Error);
            list.EndRow();
        }
        const float soundsFolder = std::round(Px(130.0f));
        if (list.Row("Sounds folder", "Success.wav or .mp3 and Error.wav or .mp3 here replace the built-in sounds. " + WideToUtf8(Paths::SoundsFolder().wstring()), soundsFolder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open folder", ImVec2(soundsFolder, 0.0f)))
            {
                std::error_code error;
                std::filesystem::create_directories(Paths::SoundsFolder(), error);
                OpenFolder(Paths::SoundsFolder());
            }
            list.EndRow();
        }

        list.Section("Diagnostics");
        if (list.Row("Profiler", "Measures every frame: the engine, Lua timers and tasks, each window, drawing and waiting, and adds Help > Profiler and diagnostics. "
                "It costs a few microseconds per frame. The checks of setup and environment work without it.", toggle))
        {
            if (ToggleSwitch("##profiler", &settings.ProfilerEnabled))
            {
                Profiler::SetEnabled(settings.ProfilerEnabled);
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Frame time in the status bar", "Shows the work per frame and the frame rate at the bottom of the main window while the profiler is on. Click it to open the profiler.", toggle))
        {
            ImGui::BeginDisabled(!settings.ProfilerEnabled);
            changed |= ToggleSwitch("##profilerstatus", &settings.ProfilerInStatusBar);
            ImGui::EndDisabled();
            list.EndRow();
        }
        const float profilerButton = std::round(Px(210.0f));
        if (list.Row("Profiler and diagnostics", "Frame graph, the breakdown of a frame, threads, checks of the setup and app info. " + BindingText(Command::OpenProfiler, CurrentBinding(Command::OpenProfiler)) + " opens it from anywhere.",
                profilerButton))
        {
            if (ImGui::Button(ICON_FA_TACHOMETER_ALT "  Open the profiler", ImVec2(profilerButton, 0.0f)))
                m_workspace.OpenProfiler();
            list.EndRow();
        }

        list.Section("Program");
        if (list.Row("Splash screen", "Shows the logo and the start-up steps right away and for at least two seconds, then the main window appears. Off, the main window appears as soon as it is drawn. Takes effect at the next start.", toggle))
        {
            changed |= ToggleSwitch("##splash", &settings.ShowSplash);
            list.EndRow();
        }
        const bool elevated = Target::IsElevated();
        const float button = std::round(Px(210.0f));
        if (list.Row("Administrator rights", elevated ? "The program runs as administrator and can open protected processes." : "Some games can only be opened with administrator rights.",
                button))
        {
            if (elevated)
                RightText(ICON_FA_SHIELD_ALT "  Administrator", button, ManifoldTheme::Accent);
            else if (ImGui::Button(ICON_FA_SHIELD_ALT "  Restart as administrator", ImVec2(button, 0.0f)))
                m_workspace.Window().RestartElevated();
            list.EndRow();
        }
        const float folder = std::round(Px(130.0f));
        if (list.Row("Settings file", WideToUtf8(Paths::SettingsFile().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open folder", ImVec2(folder, 0.0f)))
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
        if (list.Row("Theme", "Standard, or the same look tinted red, green or blue. Color changes further down belong to the theme and mode shown.", -1.0f))
        {
            const float space = std::round(Px(12.0f));
            const float width = std::floor(std::min(Px(150.0f), (list.ControlWidth() - space * 3.0f) / 4.0f));
            const float height = std::round(width * 0.62f);
            const ImVec2 start = ImGui::GetCursorScreenPos();
            for (int i = 0; i < static_cast<int>(std::size(kThemeFamilies)); ++i)
            {
                const ThemeFamily family = kThemeFamilies[i];
                ImGui::SetCursorScreenPos(ImVec2(start.x + static_cast<float>(i) * (width + space), start.y));
                if (ThemeCard(family, theme.Family == family, light, width, height) && theme.Family != family)
                {
                    theme.Family = family;
                    edited = true;
                }
            }
            list.EndRow();
        }
        const float modeWidth = std::round(Px(250.0f));
        const std::string modeText = std::string("System follows the app mode set in Windows and switches along with it. Windows is set to ") +
            (ManifoldTheme::SystemLight() ? "light" : "dark") + " right now.";
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

        const auto colorCell = [&](const std::string& key, const char* label, ImVec4 current) {
            if (!list.Row(label, {}, colorControl))
                return;
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ResetButton(Overridden(theme.Colors, key)))
            {
                Erase(theme.Colors, key);
                edited = true;
            }
            ImGui::SetCursorScreenPos(ImVec2(pos.x + frame + gap, pos.y));
            float rgba[4] = { current.x, current.y, current.z, current.w };
            if (ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf))
            {
                Erase(theme.Colors, key);
                theme.Colors[key] = { rgba[0], rgba[1], rgba[2], rgba[3] };
                edited = true;
            }
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
        if (list.Row("Accent color", "Buttons, switches, selections and highlights.", presetsWidth))
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
        list.Section("Code and memory", columns);
        paletteCells(kCodeColors, std::size(kCodeColors));

        list.Section("Sizes and spacing");
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
            }
            list.EndRow();
        }

        list.Section("Advanced");
        if (list.Row("Every ImGui color", "Set here, a color wins over the one the colors above give it.", ToggleSwitchWidth()))
        {
            ToggleSwitch("##allcolors", &m_allImGuiColors);
            list.EndRow();
        }
        if (m_allImGuiColors || list.Searching())
        {
            list.Section("ImGui colors", columns);
            const ImGuiStyle& style = ImGui::GetStyle();
            for (int i = 0; i < ImGuiCol_COUNT; ++i)
                colorCell(prefix + "ImGui." + ImGui::GetStyleColorName(i), ImGui::GetStyleColorName(i), style.Colors[i]);
        }
        return edited;
    }

    bool SettingsWindow::ScannerPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();
        const float slider = std::round(Px(230.0f));

        list.Section("Scan options");
        if (list.Row("Scan threads", "How many threads search at once. Auto uses half of the processor's threads, from 2 to 8.", slider))
        {
            const std::string format = settings.ScanThreads == 0 ? "Auto (" + std::to_string(AutoThreads()) + ")" : (settings.ScanThreads == 1 ? "1 thread" : "%d threads");
            BeginFillSlider("##threads", static_cast<float>(settings.ScanThreads) / 64.0f, slider);
            ImGui::SliderInt("##threads", &settings.ScanThreads, 0, 64, format.c_str(), ImGuiSliderFlags_AlwaysClamp);
            EndFillSlider();
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            list.EndRow();
        }
        if (list.Row("Fast scan", "Look only at aligned addresses. Much faster and finds nearly every value.", toggle))
        {
            changed |= ToggleSwitch("##fast", &settings.FastScan);
            list.EndRow();
        }
        if (list.Row("Pause while scanning", "Stop the process during a scan, so values cannot change halfway through.", toggle))
        {
            changed |= ToggleSwitch("##pause", &settings.PauseWhileScanning);
            list.EndRow();
        }

        list.Section("Types for All");
        for (const Scan::ValueType type : Scan::kAllTypes)
        {
            const std::uint32_t bit = Scan::AllTypeBit(type);
            const std::string hint = std::string("The value type All looks for ") + Scan::TypeName(type) + " values as well.";
            if (list.Row(Scan::TypeName(type), hint, toggle))
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

        list.Section("Memory to scan");
        if (list.Row("Private memory", "Memory the process allocated itself, such as the heap. Most values live here.", toggle))
        {
            changed |= ToggleSwitch("##private", &settings.ScanPrivate);
            list.EndRow();
        }
        if (list.Row("Image memory", "The process's own files loaded into memory: code and static data.", toggle))
        {
            changed |= ToggleSwitch("##image", &settings.ScanImage);
            list.EndRow();
        }
        if (list.Row("Mapped memory", "Shared and file-backed memory. Rarely holds values and makes scans slower.", toggle))
        {
            changed |= ToggleSwitch("##mapped", &settings.ScanMapped);
            list.EndRow();
        }

        list.Section("Storage");
        const float folder = std::round(Px(130.0f));
        if (list.Row("Temporary scan files", WideToUtf8(Paths::ScanFolder().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open folder", ImVec2(folder, 0.0f)))
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
        if (list.Row("Run autorun scripts", "Run the Lua scripts in the autorun folder when the program starts.", toggle))
        {
            changed |= ToggleSwitch("##autorun", &settings.AutorunEnabled);
            list.EndRow();
        }
        if (list.Row("Autorun folder", WideToUtf8(Paths::AutorunFolder().wstring()), folder))
        {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open folder", ImVec2(folder, 0.0f)))
                OpenFolder(Paths::AutorunFolder());
            list.EndRow();
        }
        if (list.Row("Time limit for callbacks",
                "Timers, hotkeys, menu and form events, structure and symbol lookups and custom types run while the window waits. One that takes longer than this is stopped with an error instead of freezing the program. "
                "Scripts started with createThread, the table script and the Lua engine window have no limit.", choice))
        {
            changed |= Milliseconds("##callbacklimit", settings.ScriptCallbackLimitMs, 250, 60000, choice);
            list.EndRow();
        }
        if (list.Row("Script windows in the app's look",
                "Windows that scripts build with createForm look like Manifold's own: cards, its colors, its font and font size, whatever colors, borders and fonts the script sets. "
                "Off: the colors, borders and fonts of the script, as Cheat Engine shows them. Pictures that scripts paint keep their colors either way.", toggle))
        {
            changed |= ToggleSwitch("##scriptapplook", &settings.ScriptWindowsAppLook);
            list.EndRow();
        }

        list.Section("Tables");
        if (list.Row("Lua script in a table", "What happens when a loaded table carries a Lua script.", choice))
        {
            int policy = static_cast<int>(settings.TableScripts);
            if (Segmented("##policy", &policy, { "Ask", "Always", "Never" }, choice))
            {
                settings.TableScripts = static_cast<TableScriptPolicy>(policy);
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Active scripts on quit", "What happens when the program closes while scripts are still active in the game.", choice))
        {
            int quit = static_cast<int>(settings.ActiveScriptsOnQuit);
            if (Segmented("##quitpolicy", &quit, { "Ask", "Turn off", "Leave" }, choice))
            {
                settings.ActiveScriptsOnQuit = static_cast<QuitPolicy>(quit);
                changed = true;
            }
            list.EndRow();
        }

        list.Section("Open processes automatically");
        if (list.Row("Open on start", "Open one of the processes below as soon as it starts.", toggle))
        {
            changed |= ToggleSwitch("##autoattach", &settings.AutoAttachEnabled);
            list.EndRow();
        }
        if (list.Row("Processes", "File names of the programs, for example game.exe.", -1.0f))
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

    bool SettingsWindow::DebuggerPage(SettingsList& list, Settings& settings)
    {
        bool changed = false;
        const float toggle = ToggleSwitchWidth();

        list.Section("Breakpoints");
        if (list.Row("Hardware breakpoints for code", "Use a free debug register instead of changing the code, so integrity checks do not see it. There are only four.", toggle))
        {
            changed |= ToggleSwitch("##hardware", &settings.UseHardwareExecuteBreakpoints);
            list.EndRow();
        }
        if (list.Row("In-process debugger (VEH)",
                "Runs the debugger inside the target instead of through Windows. It watches threads that hide themselves from a debugger, where the normal debugger "
                "would see nothing. Use it per the note under About; the normal debugger is the default. Takes effect the next time you attach.",
                toggle))
        {
            if (ToggleSwitch("##veh", &settings.UseInProcessDebugger))
            {
                if (m_workspace.Core().Debugger().Attached())
                    m_workspace.Core().Debugger().Detach();
                changed = true;
            }
            list.EndRow();
        }
        if (list.Row("Registers on break", "Show the registers when the process stops at a breakpoint.", toggle))
        {
            changed |= ToggleSwitch("##registers", &settings.ShowRegistersOnBreak);
            list.EndRow();
        }

        list.Section("Log");
        if (list.Row("Debug output of the process", "Write what the process sends with OutputDebugString to the log.", toggle))
        {
            changed |= ToggleSwitch("##debugstrings", &settings.LogDebugStrings);
            list.EndRow();
        }
        if (list.Row("Debug messages", "Write the program's own debug messages to the log as well.", toggle))
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
