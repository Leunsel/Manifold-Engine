#pragma once

#include "App/WriteHistory.h"
#include "UI/ToolWindowBase.h"

namespace Manifold::UI
{
    class WriteHistoryWindow : public ToolWindowBase
    {
    public:
        explicit WriteHistoryWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;

    private:
        void Row(const MemoryWrite& write, WriteState state);
        void RowMenu(const MemoryWrite& write);
        void Toggle(const MemoryWrite& write);
        void UndoAll();
        void Reveal(const MemoryWrite& write, bool code);

        Workspace& m_workspace;
        int m_selected = 0;
    };
}
