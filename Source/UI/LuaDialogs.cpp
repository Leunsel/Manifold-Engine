#include "UI/LuaDialogs.h"

#include "App/Sounds.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/InputBox.h"
#include "UI/DialogHost.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kDefaultTitle = "Manifold Engine";

        void Gap(float height)
        {
            ImGui::Dummy(ImVec2(0.0f, std::round(Px(height))));
        }

        void Paragraph(const std::string& text)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
        }

        int ButtonRow(const std::vector<std::string>& labels, int accent)
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            std::vector<float> widths;
            float total = style.ItemSpacing.x * static_cast<float>(labels.size() > 0 ? labels.size() - 1 : 0);
            for (const std::string& label : labels)
            {
                widths.push_back(std::max(std::round(Px(92.0f)), ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 4.0f));
                total += widths.back();
            }
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - total));
            int clicked = -1;
            for (std::size_t i = 0; i < labels.size(); ++i)
            {
                if (i > 0)
                    ImGui::SameLine();
                const ImVec2 size(widths[i], 0.0f);
                const bool pressed = static_cast<int>(i) == accent ? AccentButton(labels[i].c_str(), size) : ImGui::Button(labels[i].c_str(), size);
                if (pressed)
                    clicked = static_cast<int>(i);
            }
            return clicked;
        }

        bool EnterPressed()
        {
            return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        }

        bool EscapePressed()
        {
            return ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        }

        const char* KindIcon(Lua::MessageKind kind)
        {
            switch (kind)
            {
            case Lua::MessageKind::Warning: return ICON_FA_EXCLAMATION_TRIANGLE;
            case Lua::MessageKind::Error: return ICON_FA_TIMES_CIRCLE;
            case Lua::MessageKind::Information: return ICON_FA_INFO_CIRCLE;
            case Lua::MessageKind::Confirmation: return ICON_FA_QUESTION_CIRCLE;
            default: return nullptr;
            }
        }

        ImVec4 KindColor(Lua::MessageKind kind)
        {
            switch (kind)
            {
            case Lua::MessageKind::Warning: return ManifoldTheme::Warning;
            case Lua::MessageKind::Error: return ManifoldTheme::Error;
            default: return ManifoldTheme::Accent;
            }
        }

        int EscapeResult(const std::vector<int>& buttons)
        {
            const auto has = [&](int button) { return std::find(buttons.begin(), buttons.end(), button) != buttons.end(); };
            if (has(Lua::ButtonCancel))
                return Lua::ResultCancel;
            if (has(Lua::ButtonNo))
                return Lua::ResultNo;
            if (has(Lua::ButtonClose))
                return Lua::ResultClose;
            if (buttons.size() == 1)
                return Lua::ButtonResult(buttons.front());
            return Lua::ResultCancel;
        }

        std::optional<std::string> AskText(DialogHost& host, const std::string& caption, const std::string& prompt, const std::string& value)
        {
            std::string text = value;
            bool accepted = false;
            bool focus = true;
            host.Run(caption.empty() ? kDefaultTitle : caption, ICON_FA_MICROCHIP, 440.0f, [&]()
                {
                    if (!prompt.empty())
                    {
                        Paragraph(prompt);
                        Gap(2.0f);
                    }
                    ImGui::SetNextItemWidth(-1.0f);
                    if (focus)
                    {
                        ImGui::SetKeyboardFocusHere();
                        focus = false;
                    }
                    const bool enter = InputText("##value", text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    Gap(10.0f);
                    const int clicked = ButtonRow({ "OK", "Cancel" }, 0);
                    if (enter || clicked == 0)
                    {
                        accepted = true;
                        return true;
                    }
                    return clicked == 1 || EscapePressed();
                });
            if (!accepted)
                return std::nullopt;
            return text;
        }

        int AskMessage(DialogHost& host, const std::string& title, const std::string& text, Lua::MessageKind kind, const std::vector<int>& buttons)
        {
            std::vector<std::string> labels;
            for (int button : buttons)
                labels.emplace_back(Lua::ButtonCaption(button));
            int result = EscapeResult(buttons);
            const char* icon = KindIcon(kind);
            host.Run(title.empty() ? kDefaultTitle : title, ICON_FA_MICROCHIP, 460.0f, [&]()
                {
                    if (icon)
                    {
                        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.8f);
                        ImGui::PushStyleColor(ImGuiCol_Text, KindColor(kind));
                        ImGui::TextUnformatted(icon);
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        ImGui::SameLine(0.0f, std::round(Px(14.0f)));
                    }
                    ImGui::BeginGroup();
                    Paragraph(text);
                    ImGui::EndGroup();
                    Gap(12.0f);
                    const int clicked = ButtonRow(labels, 0);
                    if (clicked >= 0)
                    {
                        result = Lua::ButtonResult(buttons[static_cast<std::size_t>(clicked)]);
                        return true;
                    }
                    if (EnterPressed())
                    {
                        result = Lua::ButtonResult(buttons.front());
                        return true;
                    }
                    return EscapePressed();
                });
            return result;
        }

        std::pair<int, std::string> AskSelection(DialogHost& host, const std::string& title, const std::string& caption, const std::vector<std::string>& items, bool allowCustom)
        {
            std::pair<int, std::string> result{ -1, "" };
            std::string filter;
            int selected = items.empty() ? -1 : 0;
            bool focus = true;
            bool scroll = true;
            std::vector<int> shown;
            host.Run(title.empty() ? kDefaultTitle : title, ICON_FA_MICROCHIP, 480.0f, [&]()
                {
                    if (!caption.empty())
                    {
                        Paragraph(caption);
                        Gap(2.0f);
                    }
                    ImGui::SetNextItemWidth(-1.0f);
                    if (focus)
                    {
                        ImGui::SetKeyboardFocusHere();
                        focus = false;
                    }
                    bool accept = InputTextWithHint("##filter", allowCustom ? ICON_FA_SEARCH " Filter, or type your own value" : ICON_FA_SEARCH " Filter", filter,
                        ImGuiInputTextFlags_EnterReturnsTrue);

                    shown.clear();
                    for (std::size_t i = 0; i < items.size(); ++i)
                    {
                        if (filter.empty() || ContainsNoCase(items[i], filter))
                            shown.push_back(static_cast<int>(i));
                    }
                    auto position = std::find(shown.begin(), shown.end(), selected);
                    if (position == shown.end())
                    {
                        selected = shown.empty() ? -1 : shown.front();
                        position = shown.begin();
                        scroll = true;
                    }
                    if (!shown.empty() && ImGui::IsKeyPressed(ImGuiKey_DownArrow) && position + 1 != shown.end())
                    {
                        selected = *(position + 1);
                        scroll = true;
                    }
                    if (!shown.empty() && ImGui::IsKeyPressed(ImGuiKey_UpArrow) && position != shown.begin())
                    {
                        selected = *(position - 1);
                        scroll = true;
                    }

                    Gap(4.0f);
                    ImGui::BeginChild("##items", ImVec2(-1.0f, std::round(Px(260.0f))), ImGuiChildFlags_Borders);
                    if (shown.empty())
                        Muted(allowCustom && !filter.empty() ? "Nothing matches. OK takes the typed text." : "Nothing matches.");
                    for (const int index : shown)
                    {
                        ImGui::PushID(index);
                        if (ImGui::Selectable("##item", index == selected, ImGuiSelectableFlags_AllowDoubleClick))
                        {
                            selected = index;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                accept = true;
                        }
                        const ImVec2 min = ImGui::GetItemRectMin();
                        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + std::round(Px(6.0f)), min.y), ImGui::GetColorU32(ImGuiCol_Text), items[static_cast<std::size_t>(index)].c_str());
                        if (index == selected && scroll)
                            ImGui::SetScrollHereY(0.5f);
                        ImGui::PopID();
                    }
                    scroll = false;
                    ImGui::EndChild();
                    Gap(10.0f);
                    const int clicked = ButtonRow({ "OK", "Cancel" }, 0);
                    if (clicked == 0)
                        accept = true;
                    if (accept)
                    {
                        if (selected >= 0)
                        {
                            result = { selected, items[static_cast<std::size_t>(selected)] };
                            return true;
                        }
                        if (allowCustom && !filter.empty())
                        {
                            result = { -1, filter };
                            return true;
                        }
                    }
                    return clicked == 1 || EscapePressed();
                });
            return result;
        }
    }

    void InstallLuaDialogs(DialogHost& host)
    {
        Lua::DialogProvider provider;
        provider.Available = [&host]() { return host.Available(); };
        provider.AskText = [&host](const std::string& caption, const std::string& prompt, const std::string& value) { return AskText(host, caption, prompt, value); };
        provider.Message = [&host](const std::string& title, const std::string& text, Lua::MessageKind kind, const std::vector<int>& buttons)
            {
                Sounds::Play(kind == Lua::MessageKind::Error ? Sounds::Cue::Error : kind == Lua::MessageKind::Warning ? Sounds::Cue::Warning : Sounds::Cue::Success);
                return AskMessage(host, title, text, kind, buttons);
            };
        provider.Select = [&host](const std::string& title, const std::string& caption, const std::vector<std::string>& items, bool allowCustom)
            {
                return AskSelection(host, title, caption, items, allowCustom);
            };
        Lua::SetDialogProvider(std::move(provider));
    }

    void RemoveLuaDialogs()
    {
        Lua::ClearDialogProvider();
    }
}
