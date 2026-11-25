#pragma once

#include "Target/Process.h"

#include <string>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    // The Rebase submenu for a module, and the popup that asks for a base of
    // its own. Draw() belongs at window level, outside the menu that opened it.
    class RebasePrompt
    {
    public:
        void MenuItems(Engine& engine, const Target::ModuleInfo& module);
        void Draw(Engine& engine);

    private:
        std::string m_module;
        std::string m_text;
        std::string m_error;
        bool m_open = false;
    };

    // "game.exe shown at 140000000" for the module, or empty when not rebased.
    std::string RebaseNote(Engine& engine, const Target::ModuleInfo& module);
}
