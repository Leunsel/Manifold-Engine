#pragma once

#include "Lua/LuaEngine.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    int DrawScriptMenuItems(const std::vector<Lua::ScriptMenuItem>& items);
    ImGuiKeyChord ScriptChord(const std::string& shortcut);
    void InstallTextMeasurer();

    class ScriptForms
    {
    public:
        explicit ScriptForms(Engine& engine);
        void Draw();

    private:
        struct FormState
        {
            int Raise = 0;
            bool Placed = false;
            ImVec2 Position;
            ImVec2 Size;
            ImVec2 Client;
            int Hovered = 0;
            bool HoveredHasHover = false;
            int Pressed = 0;
            int Capture = 0;
            int CaptureButtons = 0;
            ImVec2 LastMouse;
            bool OpenPopup = false;
            std::vector<Lua::ScriptMenuItem> Popup;
        };

        struct ControlState
        {
            std::string Text;
            bool Editing = false;
            int ItemIndex = -2;
            int Selected = -1;
            bool Scroll = false;
            float DragStart = 0.0f;
        };

        struct Clip
        {
            ImVec2 Min;
            ImVec2 Max;
        };

        void DrawForm(const Lua::ScriptForm& form);
        void DrawMenuBar(int menu);
        void DrawControls(const std::vector<Lua::ScriptControl>& controls, ImVec2 origin, Clip clip, int popup, int wheel = 0);
        void DrawControl(const Lua::ScriptControl& control, ImVec2 min, ImVec2 max, Clip clip, int popup, int wheel);
        void DrawCaption(const std::string& text, const Lua::ScriptFont& font, bool enabled, int alignment, int layout, bool wordWrap, ImVec2 min, ImVec2 max);
        void IndexControls(const std::vector<Lua::ScriptControl>& controls);
        void DrawEdit(const Lua::ScriptControl& control, ImVec2 min, ImVec2 max);
        void DrawList(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size);
        void DrawTree(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size, int popup);
        void DrawNodes(const Lua::ScriptControl& tree, const std::vector<Lua::ScriptNode>& nodes, std::vector<const Lua::ScriptNode*>& visible, bool scroll, int popup);
        void DrawSplitter(const Lua::ScriptControl& control, ImVec2 min, ImVec2 size);
        void DrawCanvas(int id, ImVec2 origin, Clip clip, int depth);
        void Keys(const Lua::ScriptForm& form);
        void SelectNode(const Lua::ScriptControl& tree, int node);
        void RequestPopup(int menu);
        void Interact(FormState& state, bool hovered);
        static void PushScriptFont(const Lua::ScriptFont& font);

        Engine& m_engine;
        std::vector<Lua::ScriptForm> m_forms;
        std::uint64_t m_generation = 0;
        double m_snapshotAt = -1.0;
        std::map<int, FormState> m_formStates;
        std::map<int, ControlState> m_controls;
        std::map<int, std::vector<Lua::ScriptMenuItem>> m_menus;
        std::uint64_t m_menuGeneration = 0;
        std::set<int> m_reported;
        std::set<int> m_seen;
        std::map<int, const Lua::ScriptControl*> m_byId;
        const Lua::ScriptControl* m_hit = nullptr;
        int m_hitPopup = 0;
        int m_hitWheel = 0;
        std::map<int, ImVec2> m_mins;
        std::set<ImGuiID> m_surfaces;
        ImGuiID m_background = 0;
        FormState* m_state = nullptr;
        float m_scale = 1.0f;
    };
}
