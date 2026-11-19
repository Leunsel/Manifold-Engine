#pragma once

#include "App/WriteHistory.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <unordered_map>

namespace Manifold::UI
{
    class WriteHistoryWindow : public ToolWindowBase
    {
    public:
        explicit WriteHistoryWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;

    private:
        struct KnownState
        {
            WriteState State = WriteState::InMemory;
            double At = -1.0;
        };

        WriteState StateOf(const MemoryWrite& write);
        void Row(const MemoryWrite& write, WriteState state, bool blocked);
        void RowMenu(const MemoryWrite& write, bool blocked);
        void Toggle(const MemoryWrite& write);
        void UndoAll();
        void Reveal(const MemoryWrite& write, bool code);

        Workspace& m_workspace;
        int m_selected = 0;
        std::unordered_map<int, KnownState> m_states;
        std::uint64_t m_statesRevision = ~0ull;
    };
}
