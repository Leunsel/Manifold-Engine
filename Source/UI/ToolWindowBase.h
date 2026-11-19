#pragma once

#include "UI/Widgets.h"
#include "UI/Workspace.h"

namespace Manifold::UI
{
    class ToolWindowBase : public ToolWindow
    {
    public:
        bool Closed() const override { return !m_open && !Busy(); }
        void Show() { m_open = true; m_bringToFront = true; }

    protected:
        virtual bool Busy() const { return false; }

        bool BeginWindow(const char* title, ImVec2 size, ImGuiWindowFlags flags = 0, ImVec2 minimum = ImVec2(260.0f, 160.0f))
        {
            if (m_bringToFront)
            {
                ImGui::SetNextWindowFocus();
                m_bringToFront = false;
            }
            if (BeginToolWindow(title, &m_open, size, flags, minimum))
                return true;
            EndToolWindow();
            return false;
        }

        bool m_open = true;
        bool m_bringToFront = true;
    };
}
