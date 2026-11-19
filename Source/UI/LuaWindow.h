#pragma once

#include "App/Commands.h"
#include "Lua/ConsoleHistory.h"
#include "UI/CodeEditor.h"
#include "UI/ConsoleView.h"
#include "UI/DocBrowser.h"
#include "UI/ToolWindowBase.h"
#include "UI/Workspace.h"

#include <filesystem>
#include <memory>
#include <string>

namespace Manifold
{
    class Engine;
    struct Status;
}

namespace Manifold::UI
{
    class LuaWindow : public ToolWindowBase
    {
    public:
        explicit LuaWindow(Workspace& workspace);
        void Draw() override;
        void Show()
        {
            m_open = true;
            m_bringToFront = true;
        }

    private:
        bool EscapeCloses() const override { return false; }
        enum class Tab
        {
            Console,
            Script
        };

        void RunCommand(Command command);
        void ExecuteScript();
        void Submit();
        void Stop();
        void MarkError(const Status& status);
        void RememberShowOnPrint(bool show);
        void SaveViewSettings();
        void SelectTab(Tab tab);
        void DrawMenu();
        void DrawConsole();
        void DrawScript();
        void DrawFooter(bool script);
        ConsoleActions Actions();
        bool Running();

        Workspace& m_workspace;
        Engine& m_engine;
        Tab m_tab = Tab::Console;
        bool m_selectTab = false;
        int m_task = 0;
        int m_inputTask = 0;
        bool m_stopped = false;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        std::string m_text;
        std::string m_savedText;
        std::filesystem::path m_path;
        CodeEditorState m_editor;
        CodeAssist m_assist;
        std::string m_input;
        CodeEditorState m_inputEditor;
        Lua::ConsoleHistory m_history;
        ConsoleView m_view;
        ConsoleView m_scriptView;
        ConsoleFilter m_filter;
        DocBrowser m_docs;
        float m_docsWidth = 0.0f;
        float m_scriptShare = 0.62f;
    };
}
