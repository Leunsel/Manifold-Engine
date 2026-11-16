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

        constexpr float kCardCorner = 6.0f;

        float ScriptPixels(float points)
        {
            return std::max(6.0f, points * 96.0f / 72.0f);
        }

        int PushListColors(const Lua::ScriptControl& control, ImGuiCol background, bool card)
        {
            if (card)
            {
                ImGui::PushStyleColor(background, ManifoldTheme::Card);
                ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
                return 2;
            }
            if (control.Color >= 0)
            {
                ImGui::PushStyleColor(background, BgrVec(control.Color));
                return 1;
            }
            if (background != ImGuiCol_FrameBg)
            {
                ImGui::PushStyleColor(background, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                return 1;
            }
            return 0;
        }

        constexpr float kUiPoints = Fonts::kSize * 72.0f / 96.0f;

        void StripColors(Lua::ScriptControl& control)
        {
            control.Font.Points = kUiPoints;
            if (control.Kind != "paintbox" && control.Kind != "image")
            {
                control.Color = -1;
                control.BevelColor = -1;
            }
            control.Font.Color = -1;
            if (control.Kind != "memo" && control.Kind != "synedit")
                control.Font.Mono = false;
            for (Lua::ScriptControl& child : control.Children)
                StripColors(child);
        }

        void DrawCard(ImVec2 min, ImVec2 max)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::Card), Px(kCardCorner));
            draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::CardBorder), Px(kCardCorner));
        }

        bool Passive(const std::string& kind)
        {
            return kind == "panel" || kind == "label" || kind == "groupbox" || kind == "image" || kind == "paintbox" || kind == "radiogroup";
        }

        ImU32 CanvasColor(std::uint32_t value, ImU32 fallback)
        {
            if (value == 0x1FFFFFFFu)
                return 0;
            if (value > 0xFFFFFFu)
                return fallback;
            return Bgr(static_cast<int>(value));
        }

        struct VirtualKey
        {
            ImGuiKey Key;
            int Code;
        };

        const std::vector<VirtualKey>& VirtualKeys()
        {
            static const std::vector<VirtualKey> keys = []
                {
                    std::vector<VirtualKey> list = {
                        { ImGuiKey_LeftArrow, 0x25 }, { ImGuiKey_UpArrow, 0x26 }, { ImGuiKey_RightArrow, 0x27 }, { ImGuiKey_DownArrow, 0x28 },
                        { ImGuiKey_Escape, 0x1B }, { ImGuiKey_Enter, 0x0D }, { ImGuiKey_KeypadEnter, 0x0D }, { ImGuiKey_Space, 0x20 },
                        { ImGuiKey_Tab, 0x09 }, { ImGuiKey_Backspace, 0x08 }, { ImGuiKey_Delete, 0x2E }, { ImGuiKey_Insert, 0x2D },
                        { ImGuiKey_Home, 0x24 }, { ImGuiKey_End, 0x23 }, { ImGuiKey_PageUp, 0x21 }, { ImGuiKey_PageDown, 0x22 },
                        { ImGuiKey_KeypadAdd, 0x6B }, { ImGuiKey_KeypadSubtract, 0x6D }, { ImGuiKey_Equal, 0xBB }, { ImGuiKey_Minus, 0xBD },
                    };
                    for (int i = 0; i < 26; ++i)
                        list.push_back({ static_cast<ImGuiKey>(ImGuiKey_A + i), 0x41 + i });
                    for (int i = 0; i < 10; ++i)
                    {
                        list.push_back({ static_cast<ImGuiKey>(ImGuiKey_0 + i), 0x30 + i });
                        list.push_back({ static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i), 0x60 + i });
                    }
                    for (int i = 0; i < 12; ++i)
                        list.push_back({ static_cast<ImGuiKey>(ImGuiKey_F1 + i), 0x70 + i });
                    return list;
                }();
            return keys;
        }

        bool MeasureScriptText(std::string_view text, float points, bool bold, bool mono, float& width, float& height)
        {
            if (!ImGui::GetCurrentContext())
                return false;
            ImFontAtlas* atlas = ImGui::GetIO().Fonts;
            ImFont* font = mono ? Fonts::Mono() : bold ? Fonts::Bold() : (atlas && !atlas->Fonts.empty() ? atlas->Fonts[0] : nullptr);
            if (!font)
                return false;
            const float px = ScriptPixels(points);
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
        ImGui::PushFont(face, ScriptPixels(font.Points));
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
        const bool appLook = m_engine.Options().ScriptWindowsAppLook;
        if (lua.FormsGeneration() != m_generation || now - m_snapshotAt > 0.5 || appLook != m_appLook)
        {
            m_byId.clear();
            m_forms = lua.ShownForms();
            m_generation = lua.FormsGeneration();
            m_snapshotAt = now;
            m_appLook = appLook;
            if (appLook)
            {
                for (Lua::ScriptForm& form : m_forms)
                {
                    form.Color = -1;
                    form.Font.Color = -1;
                    form.Font.Mono = false;
                    form.Font.Points = kUiPoints;
                    for (Lua::ScriptControl& control : form.Controls)
                        StripColors(control);
                }
            }
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
        m_mins.clear();
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
        const bool raise = FocusRequested();
        const bool visible = ImGui::Begin(label.c_str(), &open, flags);
        ImGui::PopStyleColor();
        RaisePlatformWindow(raise);
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
            m_hitWheel = 0;
            m_surfaces.clear();
            m_cards = form.Color < 0;
            ImGui::SetCursorScreenPos(origin);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##background", ImVec2(std::max(1.0f, available.x), std::max(1.0f, available.y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            m_background = ImGui::GetItemID();
            PushScriptFont(form.Font);
            DrawControls(form.Controls, origin, { origin, ImVec2(origin.x + available.x, origin.y + available.y) }, 0);
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            Interact(state, hovered);
            if (form.KeyDown)
                Keys(form);
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

    void ScriptForms::Keys(const Lua::ScriptForm& form)
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput)
            return;
        for (const VirtualKey& key : VirtualKeys())
        {
            if (ImGui::IsKeyPressed(key.Key))
                m_engine.Lua().FireFormEvent(form.Id, "keydown", { key.Code });
        }
    }

    void ScriptForms::Interact(FormState& state, bool hovered)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        const ImGuiIO& io = ImGui::GetIO();
        const Lua::ScriptControl* hit = hovered ? m_hit : nullptr;
        const int hitId = hit ? hit->Id : 0;
        const auto local = [&](int id)
            {
                const auto found = m_mins.find(id);
                const ImVec2 min = found != m_mins.end() ? found->second : ImVec2(0.0f, 0.0f);
                return std::pair<int, int>(static_cast<int>(std::floor((io.MousePos.x - min.x) / m_scale)), static_cast<int>(std::floor((io.MousePos.y - min.y) / m_scale)));
            };
        const ImGuiID hoveredItem = ImGui::GetHoveredID();
        const bool overSurface = hoveredItem == 0 || hoveredItem == m_background || m_surfaces.count(hoveredItem) > 0;
        for (int button = 0; button < 3; ++button)
        {
            if (hit && hit->Mouse && overSurface && ImGui::IsMouseClicked(button))
            {
                const auto [x, y] = local(hitId);
                lua.FireFormEvent(hitId, "mousedown", { button, x, y });
                state.Capture = hitId;
                state.CaptureButtons |= 1 << button;
            }
            if (state.Capture && (state.CaptureButtons & (1 << button)) && ImGui::IsMouseReleased(button))
            {
                const auto [x, y] = local(state.Capture);
                lua.FireFormEvent(state.Capture, "mouseup", { button, x, y });
                state.CaptureButtons &= ~(1 << button);
                if (!state.CaptureButtons)
                    state.Capture = 0;
            }
        }
        const int moveTarget = state.Capture ? state.Capture : (hit && hit->Mouse ? hitId : 0);
        if (moveTarget && Differs(io.MousePos, state.LastMouse))
        {
            const auto [x, y] = local(moveTarget);
            lua.FireFormEvent(moveTarget, "mousemove", { x, y });
        }
        state.LastMouse = io.MousePos;
        if (hit && m_hitWheel && io.MouseWheel != 0.0f)
        {
            const auto [x, y] = local(m_hitWheel);
            const int notches = std::max(1, static_cast<int>(std::round(std::abs(io.MouseWheel))));
            for (int i = 0; i < notches; ++i)
                lua.FireFormEvent(m_hitWheel, io.MouseWheel > 0.0f ? "wheelup" : "wheeldown", { x, y });
        }
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
        const bool free = passive && overSurface;
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

    void ScriptForms::DrawControls(const std::vector<Lua::ScriptControl>& controls, ImVec2 origin, Clip clip, int popup, int wheel)
    {
        const float s = m_scale;
        for (const Lua::ScriptControl& control : controls)
        {
            const ImVec2 min(std::floor(origin.x + static_cast<float>(control.X) * s), std::floor(origin.y + static_cast<float>(control.Y) * s));
            const ImVec2 max(std::floor(min.x + static_cast<float>(control.W) * s), std::floor(min.y + static_cast<float>(control.H) * s));
            DrawControl(control, min, max, clip, control.Popup ? control.Popup : popup, control.Wheel ? control.Id : wheel);
        }
    }

    void ScriptForms::DrawCanvas(int id, ImVec2 origin, Clip clip, int depth)
    {
        const Lua::ScriptCanvas* canvas = m_engine.Lua().FindCanvas(id);
        if (!canvas || depth > 6)
            return;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float s = m_scale;
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 frame = ImGui::GetColorU32(ImGuiCol_FrameBg);
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        ImFont* regular = atlas && !atlas->Fonts.empty() ? atlas->Fonts[0] : ImGui::GetFont();
        const auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };
        std::vector<ImVec2> points;
        draw->PushClipRect(clip.Min, clip.Max, true);
        for (const Lua::CanvasCommand& command : canvas->Commands)
        {
            const ImU32 pen = CanvasColor(command.Pen, text);
            const ImU32 brush = CanvasColor(command.Brush, frame);
            const float width = std::max(1.0f, command.PenWidth * s);
            const ImVec2 a = at(command.X1, command.Y1);
            const ImVec2 b = at(command.X2, command.Y2);
            switch (command.Op)
            {
            case Lua::CanvasOp::Line:
                if (!command.PenClear)
                    draw->AddLine(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x + 0.5f, b.y + 0.5f), pen, width);
                break;
            case Lua::CanvasOp::Rect:
                if (!command.BrushClear)
                    draw->AddRectFilled(a, b, brush);
                if (!command.PenClear)
                    draw->AddRect(ImVec2(a.x + width * 0.5f, a.y + width * 0.5f), ImVec2(b.x - width * 0.5f, b.y - width * 0.5f), pen, 0.0f, 0, width);
                break;
            case Lua::CanvasOp::FillRect:
                if (!command.BrushClear)
                    draw->AddRectFilled(a, b, brush);
                break;
            case Lua::CanvasOp::Ellipse:
            {
                const ImVec2 center((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                const ImVec2 radius(std::abs(b.x - a.x) * 0.5f, std::abs(b.y - a.y) * 0.5f);
                if (!command.BrushClear)
                    draw->AddEllipseFilled(center, radius, brush);
                if (!command.PenClear)
                    draw->AddEllipse(center, ImVec2(std::max(0.5f, radius.x - width * 0.5f), std::max(0.5f, radius.y - width * 0.5f)), pen, 0.0f, 0, width);
                break;
            }
            case Lua::CanvasOp::RoundRect:
            {
                const float rounding = std::min(command.Rx, command.Ry) * 0.5f * s;
                if (!command.BrushClear)
                    draw->AddRectFilled(a, b, brush, rounding);
                if (!command.PenClear)
                    draw->AddRect(ImVec2(a.x + width * 0.5f, a.y + width * 0.5f), ImVec2(b.x - width * 0.5f, b.y - width * 0.5f), pen, rounding, 0, width);
                break;
            }
            case Lua::CanvasOp::Pixel:
                draw->AddRectFilled(a, ImVec2(a.x + s, a.y + s), pen);
                break;
            case Lua::CanvasOp::Text:
            {
                if (command.Ref < 0 || command.Ref >= static_cast<int>(canvas->Texts.size()))
                    break;
                const std::string& value = canvas->Texts[static_cast<std::size_t>(command.Ref)];
                ImFont* font = command.Mono ? Fonts::Mono() : command.Bold ? Fonts::Bold() : regular;
                if (!font)
                    break;
                const float size = std::max(6.0f, command.Points * 96.0f / 72.0f) * s;
                if (!command.BrushClear)
                {
                    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, value.c_str(), value.c_str() + value.size());
                    draw->AddRectFilled(a, ImVec2(a.x + extent.x + (command.Bold && command.Mono ? 1.0f : 0.0f), a.y + extent.y), brush);
                }
                const ImU32 color = CanvasColor(command.Pen, text);
                draw->AddText(font, size, a, color, value.c_str(), value.c_str() + value.size());
                if (command.Bold && command.Mono)
                    draw->AddText(font, size, ImVec2(a.x + 1.0f, a.y), color, value.c_str(), value.c_str() + value.size());
                break;
            }
            case Lua::CanvasOp::Draw:
                if (command.Ref != id)
                    DrawCanvas(command.Ref, a, clip, depth + 1);
                break;
            case Lua::CanvasOp::Polygon:
            case Lua::CanvasOp::Polyline:
            {
                points.clear();
                for (int i = 0; i < command.Count; ++i)
                {
                    const std::size_t at2 = static_cast<std::size_t>(command.Ref + i * 2);
                    if (at2 + 1 >= canvas->Coordinates.size())
                        break;
                    points.push_back(at(canvas->Coordinates[at2], canvas->Coordinates[at2 + 1]));
                }
                if (points.size() < 2)
                    break;
                if (command.Op == Lua::CanvasOp::Polygon && !command.BrushClear && points.size() >= 3)
                    draw->AddConcavePolyFilled(points.data(), static_cast<int>(points.size()), brush);
                if (!command.PenClear)
                    draw->AddPolyline(points.data(), static_cast<int>(points.size()), pen, command.Op == Lua::CanvasOp::Polygon ? ImDrawFlags_Closed : ImDrawFlags_None, width);
                break;
            }
            }
        }
        draw->PopClipRect();
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

    void ScriptForms::DrawControl(const Lua::ScriptControl& control, ImVec2 min, ImVec2 max, Clip clip, int popup, int wheel)
    {
        m_seen.insert(control.Id);
        m_mins[control.Id] = min;
        const Clip inner{ ImVec2(std::max(min.x, clip.Min.x), std::max(min.y, clip.Min.y)), ImVec2(std::min(max.x, clip.Max.x), std::min(max.y, clip.Max.y)) };
        if (inner.Max.x <= inner.Min.x || inner.Max.y <= inner.Min.y)
            return;
        if (Inside(ImGui::GetIO().MousePos, inner.Min, inner.Max))
        {
            m_hit = &control;
            m_hitPopup = popup;
            m_hitWheel = wheel;
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
            const bool card = Carded(control) && control.BevelColor < 0 && control.Font.Color < 0;
            if (card)
            {
                if (kind != "panel" || control.Bevel != 0)
                    DrawCard(min, max);
                if (kind == "panel")
                {
                    DrawCaption(control.Text, control.Font, control.Enabled, 2, 1, false, min, max);
                }
                else if (!control.Text.empty())
                {
                    PushScriptFont(control.Font);
                    draw->AddText(ImVec2(min.x + Px(10.0f), min.y + Px(2.0f)), ImGui::GetColorU32(ManifoldTheme::Muted), control.Text.c_str());
                    ImGui::PopFont();
                }
            }
            else
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
                {
                    DrawCaption(control.Text, control.Font, control.Enabled, 2, 1, false, min, max);
                }
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
            }
            DrawControls(control.Children, min, inner, popup, wheel);
        }
        else if (kind == "paintbox" || kind == "image")
        {
            if (control.Color >= 0)
                draw->AddRectFilled(min, max, Bgr(control.Color));
            if (control.Canvas)
                DrawCanvas(control.Canvas, min, inner, 0);
            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton("##surface", ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)),
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            m_surfaces.insert(ImGui::GetItemID());
            DrawControls(control.Children, min, inner, popup, wheel);
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
            const float inset = control.Frame && Carded(control) ? std::round(Px(2.0f)) : 0.0f;
            DrawList(control, ImVec2(min.x + inset, min.y + inset), ImVec2(std::max(1.0f, size.x - 2.0f * inset), std::max(1.0f, size.y - 2.0f * inset)));
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
        else if (kind == "treeview" || kind == "listview")
        {
            const float inset = control.Frame && Carded(control) ? std::round(Px(2.0f)) : 0.0f;
            const ImVec2 at(min.x + inset, min.y + inset);
            const ImVec2 room(std::max(1.0f, size.x - 2.0f * inset), std::max(1.0f, size.y - 2.0f * inset));
            if (kind == "treeview")
                DrawTree(control, at, room, popup);
            else
                DrawListView(control, at, room, popup);
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
        const bool card = Carded(control);
        int colors = PushScrollColors(control);
        colors += PushListColors(control, ImGuiCol_FrameBg, card);
        if (control.Font.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, BgrVec(control.Font.Color));
            ++colors;
        }
        int vars = 0;
        if (card)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, control.Frame ? Px(kCardCorner) : 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, control.Frame ? 1.0f : 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, PxWhole(8.0f, 6.0f));
            vars = 3;
        }
        ImGui::SetCursorScreenPos(min);
        const bool open = ImGui::BeginListBox("##list", size);
        ImGui::PopStyleVar(vars);
        if (open)
        {
            ImGuiListClipper clipper;
            clipper.Begin(count);
            if (scroll && index >= 0 && index < count)
                clipper.IncludeItemByIndex(index);
            const ImU32 stripe = ImGui::GetColorU32(ImGuiCol_TableRowBgAlt);
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    ImGui::PushID(i);
                    const bool selected = i == index;
                    if (card && i % 2 == 1)
                    {
                        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
                        const float edge = ImGui::GetCurrentWindow()->WindowPadding.x;
                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(rowMin.x - edge, rowMin.y - ImGui::GetStyle().ItemSpacing.y * 0.5f),
                            ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x + edge, rowMin.y + rowHeight - ImGui::GetStyle().ItemSpacing.y * 0.5f), stripe);
                    }
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
        const bool card = Carded(control);
        int colors = PushScrollColors(control);
        colors += PushListColors(control, ImGuiCol_ChildBg, card);
        if (control.Font.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, BgrVec(control.Font.Color));
            ++colors;
        }
        ImGui::SetCursorScreenPos(min);
        ImGuiChildFlags childFlags = control.Frame ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
        if (card)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, control.Frame ? Px(kCardCorner) : 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(8.0f, 6.0f));
            childFlags |= ImGuiChildFlags_AlwaysUseWindowPadding;
        }
        const bool open = ImGui::BeginChild("##tree", size, childFlags);
        if (card)
            ImGui::PopStyleVar(2);
        if (open)
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

    void ScriptForms::DrawListView(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size, int popup)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        ControlState& state = m_controls[control.Id];
        const int count = static_cast<int>(control.Rows.size());
        int index = control.ItemIndex;
        const bool scroll = index != state.ItemIndex || state.Scroll;
        state.Scroll = false;
        const auto pick = [&](int next)
            {
                if (next == index || next < 0 || next >= count)
                    return;
                const int old = index;
                index = next;
                lua.SetFormValue(control.Id, "itemindex", std::to_string(next));
                lua.FireFormEvent(control.Id, "itemselect", { old });
            };
        std::vector<int> columns;
        for (int c = 0; c < static_cast<int>(control.Columns.size()); ++c)
        {
            if (control.Columns[static_cast<std::size_t>(c)].Visible)
                columns.push_back(c);
        }
        const bool report = control.ViewStyle == 3 && !columns.empty() && columns.size() <= 64;

        const bool card = Carded(control);
        int colors = PushScrollColors(control);
        colors += PushListColors(control, ImGuiCol_ChildBg, card);
        if (control.Font.Color >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, BgrVec(control.Font.Color));
            ++colors;
        }
        ImGui::SetCursorScreenPos(min);
        ImGuiChildFlags childFlags = control.Frame ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
        if (card)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, control.Frame ? Px(kCardCorner) : 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(8.0f, 6.0f));
            childFlags |= ImGuiChildFlags_AlwaysUseWindowPadding;
        }
        const bool open = ImGui::BeginChild("##listview", size, childFlags, report ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
        if (card)
            ImGui::PopStyleVar(2);
        if (open)
        {
            PushScriptFont(control.Font);
            const auto row = [&](int i)
                {
                    const Lua::ScriptRow& entry = control.Rows[static_cast<std::size_t>(i)];
                    ImGui::PushID(entry.Id ? entry.Id : i);
                    const bool selected = i == index;
                    const ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap | (report ? ImGuiSelectableFlags_SpanAllColumns : 0);
                    const float startX = ImGui::GetCursorPosX();
                    const bool clicked = ImGui::Selectable("##row", selected, flags);
                    const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                    const bool doubleClicked = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    if (selected && scroll)
                        ImGui::SetScrollHereY();
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(startX);
                    if (control.CheckBoxes)
                    {
                        bool checked = entry.Checked;
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                        if (ImGui::Checkbox("##check", &checked))
                        {
                            lua.SetFormValue(entry.Id, "checked", checked ? "1" : "0");
                            lua.FireFormEvent(control.Id, "itemcheck", { entry.Id });
                        }
                        ImGui::PopStyleVar();
                        ImGui::SameLine();
                    }
                    const auto cell = [&](const std::string& text, int alignment)
                        {
                            if (alignment != 0)
                            {
                                const float room = ImGui::GetContentRegionAvail().x;
                                const float width = ImGui::CalcTextSize(text.c_str()).x;
                                if (width < room)
                                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (alignment == 1 ? room - width : (room - width) * 0.5f));
                            }
                            ImGui::TextUnformatted(text.c_str());
                        };
                    cell(entry.Text, report ? control.Columns[static_cast<std::size_t>(columns.front())].Alignment : 0);
                    if (report)
                    {
                        for (std::size_t shown = 1; shown < columns.size(); ++shown)
                        {
                            const int source = columns[shown] - 1;
                            ImGui::TableSetColumnIndex(static_cast<int>(shown));
                            if (source >= 0 && source < static_cast<int>(entry.Sub.size()))
                                cell(entry.Sub[static_cast<std::size_t>(source)], control.Columns[static_cast<std::size_t>(columns[shown])].Alignment);
                        }
                    }
                    if (clicked)
                    {
                        pick(i);
                        lua.FireFormEvent(control.Id, "click");
                        if (doubleClicked)
                            lua.FireFormEvent(control.Id, "dblclick");
                    }
                    if (rightClicked)
                    {
                        pick(i);
                        RequestPopup(popup);
                    }
                    ImGui::PopID();
                };

            if (report)
            {
                ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;
                if (control.GridLines)
                    flags |= ImGuiTableFlags_BordersInnerH;
                if (ImGui::BeginTable("##rows", static_cast<int>(columns.size()), flags, ImGui::GetContentRegionAvail()))
                {
                    ImGui::TableSetupScrollFreeze(0, control.Headers ? 1 : 0);
                    for (std::size_t shown = 0; shown < columns.size(); ++shown)
                    {
                        const Lua::ScriptColumn& column = control.Columns[static_cast<std::size_t>(columns[shown])];
                        const std::string label = column.Caption + "##column" + std::to_string(columns[shown]);
                        ImGui::TableSetupColumn(label.c_str(), ImGuiTableColumnFlags_WidthStretch, std::max(1.0f, static_cast<float>(column.Width)));
                    }
                    if (control.Headers)
                    {
                        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                        for (std::size_t shown = 0; shown < columns.size(); ++shown)
                        {
                            ImGui::TableSetColumnIndex(static_cast<int>(shown));
                            ImGui::TableHeader(ImGui::TableGetColumnName(static_cast<int>(shown)));
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                                lua.FireFormEvent(control.Id, "columnclick", { columns[shown] });
                        }
                    }
                    ImGuiListClipper clipper;
                    clipper.Begin(count);
                    if (scroll && index >= 0 && index < count)
                        clipper.IncludeItemByIndex(index);
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            row(i);
                        }
                    }
                    ImGui::EndTable();
                }
            }
            else
            {
                ImGuiListClipper clipper;
                clipper.Begin(count);
                if (scroll && index >= 0 && index < count)
                    clipper.IncludeItemByIndex(index);
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        row(i);
                }
            }

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                RequestPopup(popup);
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && count > 0)
            {
                const int page = std::max(1, static_cast<int>(size.y / std::max(1.0f, ImGui::GetTextLineHeightWithSpacing())) - 2);
                int next = index;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                    next = index < 0 ? 0 : std::min(index + 1, count - 1);
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                    next = index < 0 ? 0 : std::max(index - 1, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
                    next = std::min(std::max(index, 0) + page, count - 1);
                if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
                    next = std::max(std::max(index, 0) - page, 0);
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
                    next = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_End, false))
                    next = count - 1;
                if (next != index)
                {
                    pick(next);
                    lua.FireFormEvent(control.Id, "click");
                    state.Scroll = true;
                }
                if (index >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                    lua.FireFormEvent(control.Id, "dblclick");
            }
            ImGui::PopFont();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(colors);
        state.ItemIndex = index;
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
