#include "UI/ScriptUi.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Manifold::UI
{
    namespace
    {
        constexpr int kCursorHand = -21;

        ImU32 Bgr(int color, float alpha = 1.0f)
        {
            return IM_COL32(color & 0xFF, (color >> 8) & 0xFF, (color >> 16) & 0xFF, static_cast<int>(alpha * 255.0f));
        }

        ImVec4 BgrVec(int color)
        {
            return ImGui::ColorConvertU32ToFloat4(Bgr(color));
        }

        bool Differs(ImVec2 a, ImVec2 b)
        {
            return std::abs(a.x - b.x) >= 1.0f || std::abs(a.y - b.y) >= 1.0f;
        }

        bool Inside(ImVec2 point, ImVec2 min, ImVec2 max)
        {
            return point.x >= min.x && point.y >= min.y && point.x < max.x && point.y < max.y;
        }

        int PushScrollColors(const Lua::ScriptControl& control)
        {
            if (control.Font.Color < 0 && control.Color < 0)
                return 0;
            const ImVec4 grab = control.Font.Color >= 0 ? BgrVec(control.Font.Color) : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, control.Color >= 0 ? BgrVec(control.Color) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(grab.x, grab.y, grab.z, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(grab.x, grab.y, grab.z, 0.45f));
            ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(grab.x, grab.y, grab.z, 0.6f));
            return 4;
        }

        bool Passive(const std::string& kind)
        {
            return kind == "panel" || kind == "label" || kind == "groupbox" || kind == "image" || kind == "paintbox" || kind == "radiogroup";
        }

        bool MeasureScriptText(std::string_view text, float points, bool bold, bool mono, float& width, float& height)
        {
            if (!ImGui::GetCurrentContext())
                return false;
            ImFontAtlas* atlas = ImGui::GetIO().Fonts;
            ImFont* font = mono ? Fonts::Mono() : bold ? Fonts::Bold() : (atlas && !atlas->Fonts.empty() ? atlas->Fonts[0] : nullptr);
            if (!font)
                return false;
            const float px = std::max(6.0f, points * 96.0f / 72.0f);
            const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text.data(), text.data() + text.size());
            width = size.x + (bold && mono ? 1.0f : 0.0f);
            height = std::max(size.y, px);
            return true;
        }
    }

    int DrawScriptMenuItems(const std::vector<Lua::ScriptMenuItem>& items)
    {
        int clicked = 0;
        for (const Lua::ScriptMenuItem& item : items)
        {
            if (item.Caption == "-")
            {
                ImGui::Separator();
                continue;
            }
            const std::string label = item.Caption + "##script" + std::to_string(item.Id);
            if (!item.Children.empty())
            {
                if (ImGui::BeginMenu(label.c_str(), item.Enabled))
                {
                    if (const int inner = DrawScriptMenuItems(item.Children))
                        clicked = inner;
                    ImGui::EndMenu();
                }
                continue;
            }
            if (ImGui::MenuItem(label.c_str(), item.Shortcut.empty() ? nullptr : item.Shortcut.c_str(), item.Checked, item.Enabled))
                clicked = item.Id;
        }
        return clicked;
    }

    ImGuiKeyChord ScriptChord(const std::string& shortcut)
    {
        return shortcut.empty() ? 0 : ParseChord(shortcut);
    }

    void InstallTextMeasurer()
    {
        Lua::SetTextMeasurer(&MeasureScriptText);
    }

    ScriptForms::ScriptForms(Engine& engine)
        : m_engine(engine)
    {
    }

    void ScriptForms::PushScriptFont(const Lua::ScriptFont& font)
    {
        ImFont* face = font.Mono ? Fonts::Mono() : font.Bold ? Fonts::Bold() : nullptr;
        ImGui::PushFont(face, std::max(6.0f, font.Points * 96.0f / 72.0f));
    }

    void ScriptForms::Draw()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (!lua.FormsShown())
        {
            m_forms.clear();
            m_formStates.clear();
            m_controls.clear();
            m_byId.clear();
            return;
        }
        const double now = ImGui::GetTime();
        if (lua.FormsGeneration() != m_generation || now - m_snapshotAt > 0.5)
        {
            m_byId.clear();
            m_forms = lua.ShownForms();
            m_generation = lua.FormsGeneration();
            m_snapshotAt = now;
            for (const Lua::ScriptForm& form : m_forms)
                IndexControls(form.Controls);
        }
        if (lua.MenuGeneration() != m_menuGeneration)
        {
            m_menus.clear();
            m_menuGeneration = lua.MenuGeneration();
        }
        m_scale = Scale();
        m_seen.clear();
        std::set<int> alive;
        for (const Lua::ScriptForm& form : m_forms)
        {
            alive.insert(form.Id);
            if (!form.Unsupported.empty() && m_reported.insert(form.Id).second)
                LOG_INFO("Lua", "The script window \"" + form.Caption + "\" holds a " + form.Unsupported + ", which is shown as a placeholder");
            DrawForm(form);
        }
        std::erase_if(m_formStates, [&](const auto& entry) { return !alive.count(entry.first); });
        std::erase_if(m_controls, [&](const auto& entry) { return !m_seen.count(entry.first); });
    }

    void ScriptForms::DrawMenuBar(int menu)
    {
        auto found = m_menus.find(menu);
        if (found == m_menus.end())
            found = m_menus.emplace(menu, m_engine.Lua().MenuItemsOf(menu)).first;
        if (!ImGui::BeginMenuBar())
            return;
        int clicked = 0;
        for (const Lua::ScriptMenuItem& root : found->second)
        {
            if (root.Caption == "-")
                continue;
            const std::string label = root.Caption + "##menu" + std::to_string(root.Id);
            if (root.Children.empty())
            {
                if (ImGui::MenuItem(label.c_str(), nullptr, root.Checked, root.Enabled))
                    clicked = root.Id;
                continue;
            }
            if (ImGui::BeginMenu(label.c_str(), root.Enabled))
            {
                if (const int inner = DrawScriptMenuItems(root.Children))
                    clicked = inner;
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenuBar();
        if (clicked)
            m_engine.Lua().ClickMenuItem(clicked);
    }

    void ScriptForms::DrawForm(const Lua::ScriptForm& form)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        FormState& state = m_formStates[form.Id];
        m_state = &state;
        const ImGuiStyle& style = ImGui::GetStyle();
        const float s = m_scale;
        const float menuHeight = form.Menu ? ImGui::GetFrameHeight() : 0.0f;
        const ImVec2 chrome(style.WindowPadding.x * 2.0f, ImGui::GetFrameHeight() + menuHeight + style.WindowPadding.y * 2.0f);

        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        windowClass.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&windowClass);
        if (!state.Placed)
        {
            const ImVec2 size = form.ClientWidth >= 0 && form.ClientHeight >= 0
                                    ? ImVec2(static_cast<float>(form.ClientWidth) * s + chrome.x, static_cast<float>(form.ClientHeight) * s + chrome.y)
                                    : ImVec2(static_cast<float>(form.Width) * s, static_cast<float>(form.Height) * s);
            const ImGuiViewport* main = ImGui::GetMainViewport();
            if (form.Center || (form.Left == 0 && form.Top == 0))
                ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            else
                ImGui::SetNextWindowPos(ImVec2(static_cast<float>(form.Left), static_cast<float>(form.Top)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
            state.Placed = true;
            state.Raise = form.Raise;
        }
        else if (form.Raise != state.Raise)
        {
            ImGui::SetNextWindowFocus();
            state.Raise = form.Raise;
        }
        const ImVec2 minimum(std::max(Px(160.0f), static_cast<float>(form.MinWidth) * s), std::max(Px(90.0f), static_cast<float>(form.MinHeight) * s));
        ImGui::SetNextWindowSizeConstraints(minimum, ImVec2(FLT_MAX, FLT_MAX));
        const std::string label = (form.Caption.empty() ? std::string(" ") : form.Caption) + "###scriptform" + std::to_string(form.Id);
        bool open = true;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, form.Color >= 0 ? BgrVec(form.Color) : ManifoldTheme::Page);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (form.Menu)
            flags |= ImGuiWindowFlags_MenuBar;
        const bool visible = ImGui::Begin(label.c_str(), &open, flags);
        ImGui::PopStyleColor();
        ImVec2 client = state.Client;
        if (visible)
        {
            if (form.Menu)
                DrawMenuBar(form.Menu);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            client = ImVec2(std::floor(available.x / s), std::floor(available.y / s));
            const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            m_hit = nullptr;
            m_hitPopup = 0;
            PushScriptFont(form.Font);
            DrawControls(form.Controls, origin, { origin, ImVec2(origin.x + available.x, origin.y + available.y) }, 0);
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            Interact(state, hovered);
            if (state.OpenPopup)
            {
                ImGui::OpenPopup("##scriptpopup");
                state.OpenPopup = false;
            }
            if (ImGui::BeginPopup("##scriptpopup"))
            {
                if (const int clicked = DrawScriptMenuItems(state.Popup))
                    lua.ClickMenuItem(clicked);
                ImGui::EndPopup();
            }
        }
        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        ImGui::End();
        if (Differs(position, state.Position) || Differs(size, state.Size) || Differs(client, state.Client))
        {
            state.Position = position;
            state.Size = size;
            state.Client = client;
            lua.SetFormBounds(form.Id, static_cast<int>(position.x), static_cast<int>(position.y), static_cast<int>(size.x / s), static_cast<int>(size.y / s),
                static_cast<int>(client.x), static_cast<int>(client.y));
        }
        if (!open)
            lua.CloseForm(form.Id);
        m_state = nullptr;
    }

    void ScriptForms::Interact(FormState& state, bool hovered)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        const Lua::ScriptControl* hit = hovered ? m_hit : nullptr;
        const int hitId = hit ? hit->Id : 0;
        if (hitId != state.Hovered)
        {
            if (state.Hovered && state.HoveredHasHover)
                lua.FireFormEvent(state.Hovered, "mouseleave");
            if (hit && hit->Hover)
                lua.FireFormEvent(hitId, "mouseenter");
            state.Hovered = hitId;
            state.HoveredHasHover = hit && hit->Hover;
        }
        if (!hit)
        {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                state.Pressed = 0;
            return;
        }
        if (hit->Cursor == kCursorHand)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool passive = Passive(hit->Kind) || !hit->Unsupported.empty();
        const bool free = passive && !ImGui::IsAnyItemHovered();
        if (free && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            state.Pressed = hitId;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (state.Pressed == hitId && hit->Click)
                lua.FireFormEvent(hitId, "click");
            state.Pressed = 0;
        }
        if (free && hit->DoubleClick && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            lua.FireFormEvent(hitId, "dblclick");
        if (free && m_hitPopup && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
            RequestPopup(m_hitPopup);
    }

    void ScriptForms::RequestPopup(int menu)
    {
        if (!m_state || !menu)
            return;
        Lua::LuaEngine& lua = m_engine.Lua();
        lua.MenuOpeningOf(menu);
        m_state->Popup = lua.MenuItemsOf(menu);
        m_state->OpenPopup = !m_state->Popup.empty();
    }

    void ScriptForms::DrawControls(const std::vector<Lua::ScriptControl>& controls, ImVec2 origin, Clip clip, int popup)
    {
        const float s = m_scale;
        for (const Lua::ScriptControl& control : controls)
        {
            const ImVec2 min(std::floor(origin.x + static_cast<float>(control.X) * s), std::floor(origin.y + static_cast<float>(control.Y) * s));
            const ImVec2 max(std::floor(min.x + static_cast<float>(control.W) * s), std::floor(min.y + static_cast<float>(control.H) * s));
            DrawControl(control, min, max, clip, control.Popup ? control.Popup : popup);
        }
    }

    void ScriptForms::IndexControls(const std::vector<Lua::ScriptControl>& controls)
    {
        for (const Lua::ScriptControl& control : controls)
        {
            m_byId[control.Id] = &control;
            IndexControls(control.Children);
        }
    }

    void ScriptForms::DrawCaption(const std::string& text, const Lua::ScriptFont& font, bool enabled, int alignment, int layout, bool wordWrap, ImVec2 min, ImVec2 max)
    {
        if (text.empty())
            return;
        PushScriptFont(font);
        const ImU32 color = font.Color >= 0 ? Bgr(font.Color, enabled ? 1.0f : 0.5f) : ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const float wrap = wordWrap ? std::max(1.0f, max.x - min.x) : -1.0f;
        const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
        float x = min.x;
        float y = min.y;
        if (alignment == 2)
            x = std::floor(min.x + (max.x - min.x - size.x) * 0.5f);
        else if (alignment == 1)
            x = max.x - size.x;
        if (layout == 1)
            y = std::floor(min.y + (max.y - min.y - size.y) * 0.5f);
        else if (layout == 2)
            y = max.y - size.y;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(min, max, true);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, y), color, text.c_str(), nullptr, wordWrap ? wrap : 0.0f);
        if (font.Bold && font.Mono)
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + 1.0f, y), color, text.c_str(), nullptr, wordWrap ? wrap : 0.0f);
        draw->PopClipRect();
        ImGui::PopFont();
    }

    void ScriptForms::DrawControl(const Lua::ScriptControl& control, ImVec2 min, ImVec2 max, Clip clip, int popup)
    {
        m_seen.insert(control.Id);
        const Clip inner{ ImVec2(std::max(min.x, clip.Min.x), std::max(min.y, clip.Min.y)), ImVec2(std::min(max.x, clip.Max.x), std::min(max.y, clip.Max.y)) };
        if (inner.Max.x <= inner.Min.x || inner.Max.y <= inner.Min.y)
            return;
        if (Inside(ImGui::GetIO().MousePos, inner.Min, inner.Max))
        {
            m_hit = &control;
            m_hitPopup = popup;
        }
        Lua::LuaEngine& lua = m_engine.Lua();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 size(max.x - min.x, max.y - min.y);
        const std::string& kind = control.Kind;
        ImGui::PushID(control.Id);
        ImGui::PushClipRect(inner.Min, inner.Max, true);
        ImGui::BeginDisabled(!control.Enabled);
        if (!control.Unsupported.empty())
        {
            draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::Border), Px(3.0f));
            draw->AddText(ImVec2(min.x + Px(6.0f), min.y + Px(4.0f)), ImGui::GetColorU32(ImGuiCol_TextDisabled), control.Unsupported.c_str());
        }
        else if (kind == "panel" || kind == "groupbox" || kind == "radiogroup")
        {
            if (control.Color >= 0)
                draw->AddRectFilled(min, max, Bgr(control.Color));
            if (kind == "panel" && control.Bevel != 0)
            {
                const float width = std::max(1.0f, static_cast<float>(control.BevelWidth) * m_scale);
                const ImU32 color = control.BevelColor >= 0 ? Bgr(control.BevelColor) : ImGui::GetColorU32(ManifoldTheme::Border);
                draw->AddRect(ImVec2(min.x + width * 0.5f, min.y + width * 0.5f), ImVec2(max.x - width * 0.5f, max.y - width * 0.5f), color, 0.0f, 0, width);
            }
            if (kind == "panel")
                DrawCaption(control.Text, control.Font, control.Enabled, 2, 1, false, min, max);
            else
            {
                PushScriptFont(control.Font);
                const float text = ImGui::GetTextLineHeight();
                draw->AddRect(ImVec2(min.x, min.y + text * 0.5f), max, ImGui::GetColorU32(ManifoldTheme::Border), Px(3.0f));
                if (!control.Text.empty())
                {
                    const ImVec2 at(min.x + Px(9.0f), min.y);
                    const ImVec2 extent = ImGui::CalcTextSize(control.Text.c_str());
                    draw->AddRectFilled(ImVec2(at.x - Px(3.0f), at.y), ImVec2(at.x + extent.x + Px(3.0f), at.y + text), ImGui::GetColorU32(ManifoldTheme::Page));
                    draw->AddText(at, control.Font.Color >= 0 ? Bgr(control.Font.Color) : ImGui::GetColorU32(ImGuiCol_Text), control.Text.c_str());
                }
                ImGui::PopFont();
            }
            DrawControls(control.Children, min, inner, popup);
        }
        else if (kind == "label")
        {
            if (control.Color >= 0)
                draw->AddRectFilled(min, max, Bgr(control.Color));
            DrawCaption(control.Text, control.Font, control.Enabled, control.Alignment, control.Layout, control.WordWrap, min, max);
        }
        else if (kind == "button" || kind == "cecustombutton" || kind == "togglebox")
        {
            PushScriptFont(control.Font);
            ImGui::SetCursorScreenPos(min);
            const bool toggled = kind == "togglebox" && control.Checked;
            if (toggled)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(control.Text.empty() ? "##button" : control.Text.c_str(), size))
            {
                if (kind == "togglebox")
                {
                    lua.SetFormValue(control.Id, "checked", control.Checked ? "0" : "1");
                    lua.FireFormEvent(control.Id, "change");
                }
                else
                {
                    lua.FireFormEvent(control.Id, "click");
                }
            }
            if (toggled)
                ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        else if (kind == "checkbox")
        {
            PushScriptFont(control.Font);
            ImGui::SetCursorScreenPos(min);
            bool checked = control.Checked;
            if (ImGui::Checkbox(control.Text.empty() ? "##check" : control.Text.c_str(), &checked))
            {
                lua.SetFormValue(control.Id, "checked", checked ? "1" : "0");
                lua.FireFormEvent(control.Id, "change");
            }
            ImGui::PopFont();
        }
        else if (kind == "edit" || kind == "memo" || kind == "synedit")
        {
            DrawEdit(control, min, max);
        }
        else if (kind == "listbox")
        {
            DrawList(control, min, size);
        }
        else if (kind == "combobox")
        {
            PushScriptFont(control.Font);
            ImGui::SetCursorScreenPos(min);
            const bool valid = control.ItemIndex >= 0 && control.ItemIndex < static_cast<int>(control.Items.size());
            ImGui::SetNextItemWidth(size.x);
            if (ImGui::BeginCombo("##combo", valid ? control.Items[static_cast<std::size_t>(control.ItemIndex)].c_str() : ""))
            {
                for (int i = 0; i < static_cast<int>(control.Items.size()); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(control.Items[static_cast<std::size_t>(i)].c_str(), i == control.ItemIndex))
                    {
                        lua.SetFormValue(control.Id, "itemindex", std::to_string(i));
                        lua.FireFormEvent(control.Id, "select");
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::PopFont();
        }
        else if (kind == "treeview")
        {
            DrawTree(control, min, size, popup);
        }
        else if (kind == "splitter")
        {
            DrawSplitter(control, min, size);
        }
        ImGui::EndDisabled();
        ImGui::PopClipRect();
        ImGui::PopID();
    }

    void ScriptForms::DrawEdit(const Lua::ScriptControl& control, ImVec2 min, ImVec2 max)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        ControlState& state = m_controls[control.Id];
        if (!state.Editing)
            state.Text = control.Text;
        PushScriptFont(control.Font);
        const bool memo = control.Kind != "edit";
        const ImVec2 size(max.x - min.x, max.y - min.y);
        const float padY = memo ? Px(3.0f) : std::max(0.0f, std::floor((size.y - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(4.0f), padY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, control.Frame ? 1.0f : 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, control.Frame ? ImGui::GetStyle().FrameRounding : 0.0f);
        int colors = PushScrollColors(control);
        if (control.Color >= 0)
        {
            const ImVec4 background = BgrVec(control.Color);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, background);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, background);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, background);
            colors += 3;
        }
        if (control.Font.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, BgrVec(control.Font.Color));
            ++colors;
        }
        ImGui::SetCursorScreenPos(min);
        ImGuiInputTextFlags flags = control.ReadOnly ? ImGuiInputTextFlags_ReadOnly : 0;
        if (control.Password)
            flags |= ImGuiInputTextFlags_Password;
        bool changed = false;
        if (memo)
        {
            changed = InputMultiline("##memo", state.Text, size, flags);
        }
        else
        {
            ImGui::SetNextItemWidth(size.x);
            changed = control.Hint.empty() ? InputText("##edit", state.Text, flags) : InputTextWithHint("##edit", control.Hint.c_str(), state.Text, flags);
        }
        state.Editing = ImGui::IsItemActive();
        ImGui::PopStyleColor(colors);
        ImGui::PopStyleVar(3);
        ImGui::PopFont();
        if (changed)
        {
            lua.SetFormValue(control.Id, "text", state.Text);
            lua.FireFormEvent(control.Id, "change");
        }
    }

    void ScriptForms::DrawList(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        ControlState& state = m_controls[control.Id];
        const int count = static_cast<int>(control.Items.size());
        int index = control.ItemIndex;
        const bool scroll = index != state.ItemIndex || state.Scroll;
        state.Scroll = false;
        const auto pick = [&](int next)
            {
                index = next;
                lua.SetFormValue(control.Id, "itemindex", std::to_string(next));
                lua.FireFormEvent(control.Id, "select");
            };
        PushScriptFont(control.Font);
        int colors = PushScrollColors(control);
        if (control.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, BgrVec(control.Color));
            ++colors;
        }
        if (control.Font.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, BgrVec(control.Font.Color));
            ++colors;
        }
        ImGui::SetCursorScreenPos(min);
        if (ImGui::BeginListBox("##list", size))
        {
            ImGuiListClipper clipper;
            clipper.Begin(count);
            if (scroll && index >= 0 && index < count)
                clipper.IncludeItemByIndex(index);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    ImGui::PushID(i);
                    const bool selected = i == index;
                    if (ImGui::Selectable(control.Items[static_cast<std::size_t>(i)].c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        pick(i);
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            lua.FireFormEvent(control.Id, "dblclick");
                    }
                    if (selected && scroll)
                        ImGui::SetScrollHereY();
                    ImGui::PopID();
                }
            }
            const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput;
            if (focused && count > 0)
            {
                int next = index;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    next = index < 0 ? 0 : std::min(index + 1, count - 1);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    next = index < 0 ? 0 : std::max(index - 1, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
                    next = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_End, false))
                    next = count - 1;
                if (next != index)
                {
                    pick(next);
                    state.Scroll = true;
                }
            }
            ImGui::EndListBox();
        }
        ImGui::PopStyleColor(colors);
        ImGui::PopFont();
        state.ItemIndex = index;
    }

    void ScriptForms::SelectNode(const Lua::ScriptControl& tree, int node)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (node != tree.Selected)
        {
            lua.SetFormValue(tree.Id, "selected", std::to_string(node));
            lua.FireFormEvent(tree.Id, "nodechange");
        }
    }

    void ScriptForms::DrawNodes(const Lua::ScriptControl& tree, const std::vector<Lua::ScriptNode>& nodes, std::vector<const Lua::ScriptNode*>& visible, bool scroll, int popup)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        for (const Lua::ScriptNode& node : nodes)
        {
            const bool leaf = node.Children.empty() && !node.HasChildren;
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
            if (leaf)
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (node.Id == tree.Selected)
                flags |= ImGuiTreeNodeFlags_Selected;
            if (!leaf)
                ImGui::SetNextItemOpen(node.Expanded, ImGuiCond_Always);
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::intptr_t>(node.Id)), flags, "%s", node.Text.c_str());
            visible.push_back(&node);
            if (scroll && node.Id == tree.Selected)
                ImGui::SetScrollHereY();
            if (ImGui::IsItemToggledOpen())
            {
                lua.SetFormValue(node.Id, "expanded", open ? "1" : "0");
            }
            else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                SelectNode(tree, node.Id);
                lua.FireFormEvent(tree.Id, "click");
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                lua.FireFormEvent(tree.Id, "dblclick");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                if (tree.RightClickSelect)
                    SelectNode(tree, node.Id);
                RequestPopup(popup);
            }
            if (open && !leaf)
            {
                DrawNodes(tree, node.Children, visible, scroll, popup);
                ImGui::TreePop();
            }
        }
    }

    void ScriptForms::DrawTree(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size, int popup)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        ControlState& state = m_controls[control.Id];
        const bool scroll = state.Selected != control.Selected || state.Scroll;
        state.Selected = control.Selected;
        state.Scroll = false;
        int colors = PushScrollColors(control);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, control.Color >= 0 ? BgrVec(control.Color) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ++colors;
        if (control.Font.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, BgrVec(control.Font.Color));
            ++colors;
        }
        ImGui::SetCursorScreenPos(min);
        if (ImGui::BeginChild("##tree", size, control.Frame ? ImGuiChildFlags_Borders : ImGuiChildFlags_None))
        {
            PushScriptFont(control.Font);
            std::vector<const Lua::ScriptNode*> visible;
            DrawNodes(control, control.Nodes, visible, scroll, popup);
            if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                RequestPopup(popup);
            if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !visible.empty())
            {
                int at = -1;
                for (int i = 0; i < static_cast<int>(visible.size()); ++i)
                {
                    if (visible[static_cast<std::size_t>(i)]->Id == control.Selected)
                        at = i;
                }
                int next = at;
                const int last = static_cast<int>(visible.size()) - 1;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    next = at < 0 ? 0 : std::min(at + 1, last);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    next = at < 0 ? 0 : std::max(at - 1, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
                    next = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_End, false))
                    next = last;
                if (next != at && next >= 0)
                {
                    SelectNode(control, visible[static_cast<std::size_t>(next)]->Id);
                    lua.FireFormEvent(control.Id, "click");
                    state.Scroll = true;
                }
                if (at >= 0)
                {
                    const Lua::ScriptNode& current = *visible[static_cast<std::size_t>(at)];
                    const bool parent = !current.Children.empty() || current.HasChildren;
                    if (parent && ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !current.Expanded)
                        lua.SetFormValue(current.Id, "expanded", "1");
                    if (parent && ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && current.Expanded)
                        lua.SetFormValue(current.Id, "expanded", "0");
                    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                        lua.FireFormEvent(control.Id, "dblclick");
                }
            }
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(colors);
    }

    void ScriptForms::DrawSplitter(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        ControlState& state = m_controls[control.Id];
        const bool horizontal = control.Align == 3 || control.Align == 4;
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##split", ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
            ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.35f)));
        }
        const auto target = m_byId.find(control.Resizes);
        if (target == m_byId.end() || !target->second)
            return;
        if (ImGui::IsItemActivated())
            state.DragStart = static_cast<float>(horizontal ? target->second->W : target->second->H);
        if (ImGui::IsItemActive())
        {
            const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            const float delta = (horizontal ? drag.x : drag.y) / m_scale;
            const float sign = control.Align == 3 || control.Align == 1 ? 1.0f : -1.0f;
            const int wanted = std::max(control.MinSize, static_cast<int>(std::round(state.DragStart + sign * delta)));
            const int current = horizontal ? target->second->W : target->second->H;
            if (wanted != current)
                lua.SetFormValue(control.Resizes, horizontal ? "width" : "height", std::to_string(wanted));
        }
        if (ImGui::IsItemDeactivated())
            lua.FireFormEvent(control.Id, "moved");
    }
}
