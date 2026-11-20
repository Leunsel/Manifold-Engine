#pragma once

#include "Text/Json.h"
#include "UI/CodeEditor.h"
#include "UI/ToolWindowBase.h"

#include <optional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class TableFilesWindow : public ToolWindowBase
    {
    public:
        explicit TableFilesWindow(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        bool EscapeCloses() const override { return false; }
        void Select(const std::string& name);
        void Import();
        void ImportFolder();
        void AddFiles(const std::vector<std::filesystem::path>& paths);
        void NewTextFile();
        void Export(const Table::TableFile& file);
        void StartRename(const Table::TableFile& file);
        void Delete(const std::string& name);
        void FileMenu(const Table::TableFile* file);
        void MenuBar();
        void ReformatJson(int indent);
        void ShowJsonProblem(const Text::JsonProblem& problem);
        void JsonStatus();

        Workspace& m_workspace;
        std::string m_filter;
        std::string m_selected;
        bool m_renaming = false;
        bool m_renameFocus = false;
        std::string m_rename;
        std::string m_text;
        std::string m_textFor;
        bool m_textDirty = false;
        CodeEditorState m_editor;
        bool m_jsonChecked = false;
        double m_jsonChangedAt = 0.0;
        std::optional<Text::JsonProblem> m_jsonProblem;
    };
}
