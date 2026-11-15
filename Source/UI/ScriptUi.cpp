#include "UI/ScriptUi.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace Manifold::UI
{
    namespace
    {
        constexpr int kAlignTop = 1;
        constexpr int kAlignBottom = 2;
        constexpr int kAlignLeft = 3;
        constexpr int kAlignRight = 4;
        constexpr int kAlignClient = 5;

        std::set<int>* g_seenControls = nullptr;

        bool Differs(ImVec2 a, ImVec2 b)
        {
            return std::abs(a.x - b.x) >= 1.0f || std::abs(a.y - b.y) >= 1.0f;
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

    ScriptForms::ScriptForms(Engine& engine)
        : m_engine(engine)
    {
    }

    void ScriptForms::Draw()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (!lua.FormsShown())
        {
            m_forms.clear();
            m_controls.clear();
            return;
        }
        const std::vector<Lua::ScriptForm> forms = lua.ShownForms();
        std::set<int> alive;
        std::set<int> seen;
        g_seenControls = &seen;
        for (const Lua::ScriptForm& form : forms)
        {
            alive.insert(form.Id);
            if (!form.Unsupported.empty())
            {
                if (m_reported.insert(form.Id).second)
                    LOG_INFO("Lua", "The script window \"" + form.Caption + "\" uses " + form.Unsupported + ", which is not drawn yet");
                continue;
            }
            DrawForm(form);
        }
        g_seenControls = nullptr;
        std::erase_if(m_forms, [&](const auto& entry) { return !alive.count(entry.first); });
        std::erase_if(m_controls, [&](const auto& entry) { return !seen.count(entry.first); });
    }

    void ScriptForms::DrawForm(const Lua::ScriptForm& form)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        FormState& state = m_forms[form.Id];
        const ImGuiStyle& style = ImGui::GetStyle();
        const float title = ImGui::GetFrameHeight();
        const ImVec2 chrome(style.WindowPadding.x * 2.0f, title + style.WindowPadding.y * 2.0f);

        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        windowClass.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&windowClass);
        if (!state.Placed)
        {
            const ImGuiViewport* main = ImGui::GetMainViewport();
            if (form.Center || (form.Left == 0 && form.Top == 0))
                ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            else
                ImGui::SetNextWindowPos(ImVec2(static_cast<float>(form.Left), static_cast<float>(form.Top)), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(form.Width) + chrome.x, static_cast<float>(form.Height) + chrome.y), ImGuiCond_Always);
            ImGui::SetNextWindowFocus();
            state.Placed = true;
            state.Raise = form.Raise;
        }
        else if (form.Raise != state.Raise)
        {
            ImGui::SetNextWindowFocus();
            state.Raise = form.Raise;
        }
        ImGui::SetNextWindowSizeConstraints(Px(160.0f, 90.0f), ImVec2(FLT_MAX, FLT_MAX));
        const std::string label = (form.Caption.empty() ? std::string(" ") : form.Caption) + "###scriptform" + std::to_string(form.Id);
        bool open = true;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Page);
        const bool visible = ImGui::Begin(label.c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        ImGui::PopStyleColor();
        if (visible)
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            DrawControls(form.Controls, origin, available);
        }
        const ImVec2 position = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        ImGui::End();
        if (Differs(position, state.Position) || Differs(size, state.Size))
        {
            state.Position = position;
            state.Size = size;
            lua.SetFormBounds(form.Id, static_cast<int>(position.x), static_cast<int>(position.y), static_cast<int>(size.x - chrome.x), static_cast<int>(size.y - chrome.y));
        }
        if (!open)
            lua.CloseForm(form.Id);
    }

    ImVec2 ScriptForms::NaturalSize(const Lua::ScriptControl& control) const
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const ImVec2 text = ImGui::CalcTextSize(control.Text.c_str());
        const float frame = ImGui::GetFrameHeight();
        if (control.Kind == "label")
            return ImVec2(text.x, std::max(text.y, ImGui::GetTextLineHeight()));
        if (control.Kind == "button")
            return ImVec2(std::max(text.x + style.FramePadding.x * 2.0f, Px(75.0f)), frame);
        if (control.Kind == "checkbox")
            return ImVec2(frame + style.ItemInnerSpacing.x + text.x, frame);
        if (control.Kind == "edit" || control.Kind == "combobox")
            return ImVec2(Px(120.0f), frame);
        if (control.Kind == "memo" || control.Kind == "listbox")
            return ImVec2(Px(150.0f), Px(90.0f));
        if (control.Kind == "splitter")
            return ImVec2(Px(5.0f), Px(5.0f));
        return ImVec2(Px(185.0f), Px(105.0f));
    }

    void ScriptForms::DrawControls(const std::vector<Lua::ScriptControl>& controls, ImVec2 origin, ImVec2 size)
    {
        ImVec2 min = origin;
        ImVec2 max(origin.x + size.x, origin.y + size.y);
        std::vector<ImRect> rects(controls.size());
        const auto extent = [&](const Lua::ScriptControl& control)
            {
                const ImVec2 natural = NaturalSize(control);
                return ImVec2(control.Width > 0 ? static_cast<float>(control.Width) : natural.x, control.Height > 0 ? static_cast<float>(control.Height) : natural.y);
            };
        for (std::size_t i = 0; i < controls.size(); ++i)
        {
            const Lua::ScriptControl& control = controls[i];
            const ImVec2 wanted = extent(control);
            if (control.Align == kAlignTop)
            {
                rects[i] = ImRect(min, ImVec2(max.x, std::min(max.y, min.y + wanted.y)));
                min.y = rects[i].Max.y;
            }
            else if (control.Align == kAlignBottom)
            {
                rects[i] = ImRect(ImVec2(min.x, std::max(min.y, max.y - wanted.y)), max);
                max.y = rects[i].Min.y;
            }
        }
        for (std::size_t i = 0; i < controls.size(); ++i)
        {
            const Lua::ScriptControl& control = controls[i];
            const ImVec2 wanted = extent(control);
            if (control.Align == kAlignLeft)
            {
                rects[i] = ImRect(min, ImVec2(std::min(max.x, min.x + wanted.x), max.y));
                min.x = rects[i].Max.x;
            }
            else if (control.Align == kAlignRight)
            {
                rects[i] = ImRect(ImVec2(std::max(min.x, max.x - wanted.x), min.y), max);
                max.x = rects[i].Min.x;
            }
        }
        for (std::size_t i = 0; i < controls.size(); ++i)
        {
            const Lua::ScriptControl& control = controls[i];
            if (control.Align == kAlignClient)
                rects[i] = ImRect(min, max);
            else if (control.Align < kAlignTop || control.Align > kAlignClient)
            {
                const ImVec2 wanted = extent(control);
                const ImVec2 at(origin.x + static_cast<float>(control.Left), origin.y + static_cast<float>(control.Top));
                rects[i] = ImRect(at, ImVec2(at.x + wanted.x, at.y + wanted.y));
            }
        }
        for (std::size_t i = 0; i < controls.size(); ++i)
        {
            const ImRect& rect = rects[i];
            if (rect.GetWidth() <= 0.0f || rect.GetHeight() <= 0.0f)
                continue;
            DrawControl(controls[i], rect.Min, rect.GetSize());
        }
    }

    void ScriptForms::DrawControl(const Lua::ScriptControl& control, ImVec2 position, ImVec2 size)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (g_seenControls)
            g_seenControls->insert(control.Id);
        ImGui::SetCursorScreenPos(position);
        ImGui::PushID(control.Id);
        ImGui::BeginDisabled(!control.Enabled);
        if (control.Kind == "label")
        {
            ImGui::PushClipRect(position, ImVec2(position.x + size.x, position.y + size.y), true);
            ImGui::TextUnformatted(control.Text.c_str());
            ImGui::PopClipRect();
        }
        else if (control.Kind == "button")
        {
            if (ImGui::Button(control.Text.c_str(), size))
                lua.FireFormEvent(control.Id, "click");
        }
        else if (control.Kind == "checkbox")
        {
            bool checked = control.Checked;
            if (ImGui::Checkbox(control.Text.c_str(), &checked))
            {
                lua.SetFormValue(control.Id, "checked", checked ? "1" : "0");
                lua.FireFormEvent(control.Id, "change");
            }
        }
        else if (control.Kind == "edit" || control.Kind == "memo")
        {
            ControlState& state = m_controls[control.Id];
            if (!state.Editing)
                state.Text = control.Text;
            const ImGuiInputTextFlags flags = control.ReadOnly ? ImGuiInputTextFlags_ReadOnly : 0;
            bool changed = false;
            if (control.Kind == "edit")
            {
                ImGui::SetNextItemWidth(size.x);
                changed = InputText("##edit", state.Text, flags);
            }
            else
            {
                changed = InputMultiline("##memo", state.Text, size, flags);
            }
            state.Editing = ImGui::IsItemActive();
            if (changed)
            {
                lua.SetFormValue(control.Id, "text", state.Text);
                lua.FireFormEvent(control.Id, "change");
            }
        }
        else if (control.Kind == "listbox")
        {
            DrawList(control, size);
        }
        else if (control.Kind == "combobox")
        {
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
        }
        else if (control.Kind == "panel" || control.Kind == "groupbox")
        {
            ImVec2 inner = position;
            ImVec2 innerSize = size;
            if (control.Kind == "groupbox")
            {
                const float text = ImGui::GetTextLineHeight();
                const float pad = Px(6.0f);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRect(ImVec2(position.x, position.y + text * 0.5f), ImVec2(position.x + size.x, position.y + size.y), ImGui::GetColorU32(ManifoldTheme::Border), Px(3.0f));
                if (!control.Text.empty())
                {
                    const ImVec2 at(position.x + pad * 1.5f, position.y);
                    const ImVec2 extent = ImGui::CalcTextSize(control.Text.c_str());
                    draw->AddRectFilled(ImVec2(at.x - Px(3.0f), at.y), ImVec2(at.x + extent.x + Px(3.0f), at.y + text), ImGui::GetColorU32(ManifoldTheme::Page));
                    draw->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), control.Text.c_str());
                }
                inner = ImVec2(position.x + pad, position.y + text + pad * 0.5f);
                innerSize = ImVec2(std::max(0.0f, size.x - pad * 2.0f), std::max(0.0f, size.y - text - pad * 1.5f));
            }
            DrawControls(control.Children, inner, innerSize);
            ImGui::SetCursorScreenPos(position);
            ImGui::Dummy(size);
        }
        else
        {
            ImGui::Dummy(size);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    void ScriptForms::DrawList(const Lua::ScriptControl& control, ImVec2 size)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        ControlState& state = m_controls[control.Id];
        const int count = static_cast<int>(control.Items.size());
        int index = control.ItemIndex;
        const bool scroll = index != state.ItemIndex || state.Editing;
        state.Editing = false;
        const auto pick = [&](int next)
            {
                index = next;
                lua.SetFormValue(control.Id, "itemindex", std::to_string(next));
                lua.FireFormEvent(control.Id, "select");
            };
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
                    state.Editing = true;
                }
            }
            ImGui::EndListBox();
        }
        state.ItemIndex = index;
    }
}
