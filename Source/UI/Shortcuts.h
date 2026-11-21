#pragma once

#include "App/Commands.h"
#include "UI/ToolWindowBase.h"
#include "UI/Workspace.h"

#include <string>

namespace Manifold::UI
{
    bool Pressed(Command command);
    // For a text field's own keys: plain keys such as F3 count while typing.
    bool PressedWhileTyping(Command command);
    bool ChordPressed(ImGuiKeyChord chord, CommandScope scope);
    int PressedDigit(Command command);
    bool CommandItem(const char* label, Command command, bool selected = false, bool enabled = true);
    bool DangerCommandItem(const char* label, Command command, bool enabled = true);
    std::string WithKeys(const char* text, Command command);
    void SetShortcutCapture(bool capturing);
    ImGuiKeyChord CapturedChord();

    class ShortcutsWindow : public ToolWindowBase
    {
    public:
        explicit ShortcutsWindow(Workspace& workspace);
        void Draw() override;
        void Show()
        {
            m_open = true;
            m_bringToFront = true;
            m_focusSearch = true;
        }

    private:
        Workspace& m_workspace;
        bool m_focusSearch = true;
        bool m_cheatEngineOnly = false;
        std::string m_filter;
    };
}
