#pragma once

#include "App/Commands.h"
#include "UI/CodeEditor.h"
#include "UI/Templates.h"
#include "UI/ToolWindowBase.h"

#include <string>

namespace Manifold::UI
{
    class ScriptWindow : public ToolWindowBase
    {
    public:
        ScriptWindow(Workspace& workspace, int number, std::string script, int recordId, bool lua);
        void Draw() override;
        int RecordId() const { return m_recordId; }
        void Focus() { m_bringToFront = true; }
        const std::string& WindowId() const { return m_windowId; }
        bool Placed() const { return m_placed; }

    private:
        bool EscapeCloses() const override { return false; }
        void Menu();
        void Run(bool disable);
        void Check();
        void SaveToRecord();
        void OpenFile();
        void SaveFile(bool askName);
        void AssignToTable();
        void RunCommand(Command command);
        void Shortcuts();

        Workspace& m_workspace;
        Engine& m_engine;
        std::string m_title;
        std::string m_windowId;
        static constexpr int kDockTries = 30;
        bool m_placed = false;
        int m_dockTries = 0;
        std::string m_text;
        std::string m_saved;
        std::filesystem::path m_path;
        int m_recordId = 0;
        bool m_lua = false;
        CodeEditorState m_editor;
        std::string m_result;
        bool m_resultOk = true;
        int m_task = 0;
        bool m_stopped = false;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        TemplateRunner m_templates;
    };
}
