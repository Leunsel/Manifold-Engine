#pragma once

#include "UI/ToolWindowBase.h"
#include "UI/Workspace.h"

namespace Manifold::UI
{
    // Lists what the layout audit found (Layout/Audit.h): per window, what left its place and by how much; hovering a
    // row outlines it on screen.
    class LayoutAuditWindow : public ToolWindowBase
    {
    public:
        explicit LayoutAuditWindow(Workspace& workspace);
        void Draw() override;

    private:
        Workspace& m_workspace;
    };
}
