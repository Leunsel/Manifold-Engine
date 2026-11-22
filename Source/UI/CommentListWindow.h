#pragma once

#include "Table/CommentList.h"
#include "UI/ToolWindowBase.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Manifold::UI
{
    // Every comment and header the user put on disassembler lines in this table: jump there, edit, remove with undo.
    class CommentListWindow : public ToolWindowBase
    {
    public:
        explicit CommentListWindow(Workspace& workspace) : m_workspace(workspace) {}
        void Draw() override;

    private:
        void Row(const std::string& key, const std::vector<std::string>& keys, int index);
        void RowMenu(const std::string& key, const std::vector<std::string>& keys);
        void Click(const std::vector<std::string>& keys, int index);
        void Reveal(const std::string& key);
        void Remove(std::vector<std::string> keys);
        void Undo();
        void EditDialog();
        void Keys(const std::vector<std::string>& keys);
        std::vector<std::string> SelectedIn(const std::vector<std::string>& keys) const;

        Workspace& m_workspace;
        std::string m_filter;
        std::set<std::string> m_selected;
        std::string m_anchor;
        std::vector<std::vector<Table::RemovedNote>> m_undo;
        std::vector<std::string> m_removeLater;
        std::uint64_t m_tableIdentity = 0;

        bool m_editOpen = false;
        bool m_editHeader = false;
        std::string m_editKey;
        std::string m_editText;
    };
}
