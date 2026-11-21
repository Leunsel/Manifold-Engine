#pragma once

#include "Lua/AutorunModules.h"
#include "UI/ToolWindowBase.h"

#include <string>

namespace Manifold::UI
{
    class ModulesWindow : public ToolWindowBase
    {
    public:
        explicit ModulesWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;

    private:
        enum class Action
        {
            None,
            Enable,
            Disable,
            Reload,
            Allow,
            ReloadAll
        };

        void Row(const Autorun::Module& module);
        void Details(const Autorun::Module& module);
        void Ask(Action action, const std::string& name);
        void Perform();

        Workspace& m_workspace;
        std::string m_selected;
        Action m_action = Action::None;
        std::string m_actionName;
        float m_detailHeight = 0.0f;
    };
}
