#pragma once

#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <map>
#include <string>

namespace Manifold::UI
{
    class WatchWindow : public ToolWindow
    {
    public:
        WatchWindow(Workspace& workspace, int watchId, Debug::WatchKind kind, std::uint64_t address, int size);
        ~WatchWindow() override;
        void Draw() override;
        bool Closed() const override { return !m_open; }
        // Re-registers this watch on the debugger's current backend (used after a
        // backend switch, e.g. moving to the in-process debugger for hidden threads).
        void Rearm();

    private:
        void EntryMenu(const Debug::WatchEntry& entry);
        void CopyList() const;

        Workspace& m_workspace;
        Engine& m_engine;
        std::string m_title;
        bool m_open = true;
        int m_watchId = 0;
        Debug::WatchKind m_kind;
        std::uint64_t m_address = 0;
        int m_size = 4;
        Debug::Watch m_copy;
        int m_selected = -1;
        std::map<std::uint64_t, std::string> m_text;
    };
}
