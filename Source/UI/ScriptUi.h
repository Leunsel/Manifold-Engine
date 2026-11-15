#pragma once

#include "Lua/LuaEngine.h"

#include "imgui.h"

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

    class ScriptForms
    {
    public:
        explicit ScriptForms(Engine& engine);
        void Draw();

    private:
        struct FormState
        {
            int Raise = 0;
            ImVec2 Position;
            ImVec2 Size;
            bool Placed = false;
        };

        struct ControlState
        {
            std::string Text;
            bool Editing = false;
            int ItemIndex = -2;
        };

        void DrawForm(const Lua::ScriptForm& form);
        void DrawControls(const std::vector<Lua::ScriptControl>& controls, ImVec2 origin, ImVec2 size);
        void DrawControl(const Lua::ScriptControl& control, ImVec2 position, ImVec2 size);
        void DrawList(const Lua::ScriptControl& control, ImVec2 size);
        ImVec2 NaturalSize(const Lua::ScriptControl& control) const;

        Engine& m_engine;
        std::map<int, FormState> m_forms;
        std::map<int, ControlState> m_controls;
        std::set<int> m_reported;
    };
}
