#pragma once

#include "App/Palette.h"

#include <functional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class Workspace;

    // Ctrl+K: one box to run any command of the main window, open a tool
    // window or a settings page, open a recent table, or switch a record of
    // the address list, by typing a few letters of its name.
    class CommandPalette
    {
    public:
        explicit CommandPalette(Workspace& workspace);

        void Toggle();
        void Open(std::string query = {});
        bool IsOpen() const { return m_open; }
        void Draw();

        // What the palette offers right now, for the tests and the layout sweep.
        const std::vector<PaletteItem>& Items() const { return m_items; }
        // Runs an item as Enter (or Alt+Enter, `alternate`) would; false when it is gone.
        bool Run(const std::string& key, bool alternate = false);

    private:
        struct Entry
        {
            std::function<void()> Main;
            std::function<void()> Alternate;  // records: show in the address list
        };

        void Build();
        void Rank();
        void Close();
        void Contents();
        void RunPending();
        void Choose(std::size_t match, bool alternate);
        void Row(std::size_t row, float width, float height);

        Workspace& m_workspace;
        bool m_open = false;
        bool m_appearing = false;
        bool m_closing = false;
        bool m_inPopup = false;
        std::function<void()> m_pending;
        std::string m_query;
        std::string m_ranked = "\x01";
        std::vector<PaletteItem> m_items;
        std::vector<Entry> m_entries;
        std::vector<PaletteMatch> m_matches;
        int m_selected = 0;
        int m_scroll = 0;
        float m_mouseY = -1.0f;
    };
}
