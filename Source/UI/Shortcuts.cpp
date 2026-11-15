#include "UI/Shortcuts.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Windows.h"

#include "imgui_internal.h"

namespace Manifold::UI
{
    namespace
    {
        int g_captureFrame = -10;

        bool TypingBlocks(ImGuiKeyChord chord)
        {
            return ImGui::GetIO().WantTextInput && !(chord & (ImGuiMod_Ctrl | ImGuiMod_Alt));
        }

        bool Route(ImGuiKeyChord chord, CommandScope scope)
        {
            if (!chord || g_captureFrame >= ImGui::GetFrameCount() - 1)
                return false;
            const bool global = scope == CommandScope::Global;
            if (!ImGui::Shortcut(chord, global ? ImGuiInputFlags_RouteGlobal : ImGuiInputFlags_RouteFocused))
                return false;
            if (TypingBlocks(chord))
                return false;
            return !global || !ImGui::GetTopMostPopupModal();
        }

        bool Matches(const CommandInfo& info, const std::string& filter)
        {
            if (filter.empty())
                return true;
            const Binding binding = CurrentBinding(info.Id);
            return ContainsNoCase(info.Name, filter) || ContainsNoCase(ChordText(binding.Chord, info.Digits), filter) ||
                   (binding.Alternate && ContainsNoCase(ChordText(binding.Alternate, info.Digits), filter)) || ContainsNoCase(ScopeName(info.Scope), filter);
        }

        bool IsModifier(ImGuiKey key)
        {
            return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt ||
                   key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper;
        }
    }

    bool Pressed(Command command)
    {
        const CommandInfo& info = Describe(command);
        const Binding binding = CurrentBinding(command);
        const bool first = Route(binding.Chord, info.Scope);
        const bool second = Route(binding.Alternate, info.Scope);
        return first || second;
    }

    void SetShortcutCapture(bool capturing)
    {
        g_captureFrame = capturing ? ImGui::GetFrameCount() : -10;
    }

    ImGuiKeyChord CapturedChord()
    {
        const ImGuiIO& io = ImGui::GetIO();
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_GamepadStart; ++key)
        {
            const ImGuiKey named = static_cast<ImGuiKey>(key);
            if (IsModifier(named))
                continue;
            if (!ImGui::IsKeyPressed(named, false))
                continue;
            ImGuiKeyChord chord = key;
            if (io.KeyCtrl)
                chord |= ImGuiMod_Ctrl;
            if (io.KeyShift)
                chord |= ImGuiMod_Shift;
            if (io.KeyAlt)
                chord |= ImGuiMod_Alt;
            return chord;
        }
        return 0;
    }

    int PressedDigit(Command command)
    {
        const CommandInfo& info = Describe(command);
        const ImGuiKeyChord mods = CurrentBinding(command).Chord & ImGuiMod_Mask_;
        if (!CurrentBinding(command).Chord)
            return -1;
        int digit = -1;
        for (int i = 0; i < 10; ++i)
        {
            if (Route(mods | (ImGuiKey_0 + i), info.Scope) && digit < 0)
                digit = i;
        }
        return digit;
    }

    bool CommandItem(const char* label, Command command, bool selected, bool enabled)
    {
        return ImGui::MenuItem(label, ShortcutText(command), selected, enabled);
    }

    bool DangerCommandItem(const char* label, Command command, bool enabled)
    {
        return DangerMenuItem(label, ShortcutText(command), enabled);
    }

    std::string WithKeys(const char* text, Command command)
    {
        const char* keys = ShortcutText(command);
        return keys ? std::string(text) + " (" + keys + ")" : std::string(text);
    }

    ShortcutsWindow::ShortcutsWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void ShortcutsWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_KEYBOARD "  Keyboard shortcuts###shortcuts", &m_open, ImVec2(640.0f, 680.0f), 0, ImVec2(440.0f, 300.0f)))
        {
            EndToolWindow();
            return;
        }
        if (m_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSearch = false;
        }
        const char* change = ICON_FA_PEN "  Change keys...";
        const float changeWidth = ImGui::CalcTextSize(change).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Cheat Engine keys only").x - ImGui::GetFrameHeight() - changeWidth - Px(36.0f));
        InputTextWithHint("##filter", ICON_FA_SEARCH " Command, keys or window", m_filter);
        ImGui::SameLine();
        ImGui::Checkbox("Cheat Engine keys only", &m_cheatEngineOnly);
        ItemTooltip("Only the shortcuts that Cheat Engine 7.5 uses for the same feature");
        ImGui::SameLine();
        if (ImGui::Button(change))
            m_workspace.OpenSettings(SettingsWindow::KeyboardPage);
        ItemTooltip("Settings > Keyboard");
        ImGui::Spacing();

        ImGui::BeginChild("##list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        const std::string filter(Trim(m_filter));
        bool any = false;
        for (int scope = 0; scope < static_cast<int>(CommandScope::Count); ++scope)
        {
            std::vector<const CommandInfo*> shown;
            for (const CommandInfo& info : AllCommands())
            {
                if (static_cast<int>(info.Scope) == scope && Matches(info, filter) && (!m_cheatEngineOnly || info.FromCheatEngine))
                    shown.push_back(&info);
            }
            if (shown.empty())
                continue;
            any = true;
            ImGui::PushID(scope);
            ImGui::Spacing();
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(ScopeName(static_cast<CommandScope>(scope)));
            ImGui::PopFont();
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            SectionLabel(ScopeHint(static_cast<CommandScope>(scope)));
            ImGui::PopTextWrapPos();
            if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, Px(170.0f));
                ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Origin", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
                for (const CommandInfo* info : shown)
                {
                    const Binding binding = CurrentBinding(info->Id);
                    const bool customized = IsCustomized(info->Id);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    PushMono();
                    ImGui::PushStyleColor(ImGuiCol_Text, binding.Chord ? ManifoldTheme::AddressText : ManifoldTheme::Muted);
                    ImGui::TextUnformatted(binding.Chord ? ChordText(binding.Chord, info->Digits).c_str() : "none");
                    if (binding.Alternate)
                    {
                        ImGui::PopStyleColor();
                        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                        ImGui::TextUnformatted(("or " + ChordText(binding.Alternate, info->Digits)).c_str());
                    }
                    ImGui::PopStyleColor();
                    PopMono();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(info->Name);
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, customized ? ManifoldTheme::Warning : info->FromCheatEngine ? ManifoldTheme::Accent : ManifoldTheme::Muted);
                    ImGui::TextUnformatted(customized ? "Yours" : info->FromCheatEngine ? "CE" : "Manifold");
                    ImGui::PopStyleColor();
                    if (customized)
                        ItemTooltip(("Default: " + BindingText(info->Id, DefaultBinding(info->Id))).c_str());
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
        if (!any)
            EmptyState(ICON_FA_KEYBOARD, "No shortcut found", "Nothing matches \"" + filter + "\".", {}, false);
        ImGui::EndChild();
        EndToolWindow();
    }
}
