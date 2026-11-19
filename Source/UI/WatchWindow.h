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

    private:
        Workspace& m_workspace;
        Engine& m_engine;
        std::string m_title;
        bool m_open = true;
        int m_watchId = 0;
        Debug::WatchKind m_kind;
        [[maybe_unused]] std::uint64_t m_address = 0;
        [[maybe_unused]] int m_size = 4;
        Debug::Watch m_copy;
        int m_selected = -1;
        std::map<std::uint64_t, std::string> m_text;
    };
}
