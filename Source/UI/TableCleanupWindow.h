#pragma once

#include "Table/TableCleanup.h"
#include "UI/ToolWindowBase.h"

#include <set>
#include <string>

namespace Manifold::UI
{
    // Table > Clean Up for Release: what the open table carries that nothing
    // uses (structures, symbols, files, empty groups, comments), to tick off
    // and leave out of a copy. The open table and its file stay as they are.
    class TableCleanupWindow : public ToolWindowBase
    {
    public:
        explicit TableCleanupWindow(Workspace& workspace);
        void Draw() override;
        void Look();

    private:
        void Summary();
        void List();
        void Footer();
        void Measure();
        void SaveCopy();
        std::vector<Table::CleanupItem> Chosen() const;

        Workspace& m_workspace;
        std::string m_original;
        Table::CleanupPlan m_plan;
        std::set<std::string> m_chosen;
        std::size_t m_cleanedBytes = 0;
        bool m_measured = false;
        std::string m_error;
        std::uint64_t m_lookedAt = 0;
    };
}
