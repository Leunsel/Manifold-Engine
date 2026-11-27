#pragma once

#include "App/Commands.h"
#include "Asm/AutoAssembler.h"
#include "UI/CodeEditor.h"
#include "UI/Templates.h"
#include "UI/ToolWindowBase.h"

#include <string>

namespace Manifold::UI
{
    // Auto Assembler completion: commands, mnemonics, registers, the target's
    // modules and registered symbols.
    CodeAssist MakeAsmAssist(Engine& engine);

    class ScriptWindow : public ToolWindowBase
    {
    public:
        ScriptWindow(Workspace& workspace, int number, std::string script, int recordId, bool lua);
        void Draw() override;
        int RecordId() const { return m_recordId; }
        std::uint64_t TableIdentity() const { return m_tableIdentity; }
        void Focus() { m_bringToFront = true; }
        const std::string& WindowId() const { return m_windowId; }
        bool Placed() const { return m_placed; }
        const std::filesystem::path& Path() const { return m_path; }
        const std::string& Text() const { return m_text; }
        bool FileChanged() const { return m_text != m_fileText; }
        void SetFile(const std::filesystem::path& path);
        void ReplaceText(std::string text);
        void GotoLine(int line);

    private:
        bool EscapeCloses() const override { return false; }
        void Menu();
        void Run(bool disable);
        void Check();
        void AddWarnings(const Asm::AaResult& result);
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
        std::string m_fileText;
        std::filesystem::path m_path;
        int m_recordId = 0;
        std::uint64_t m_tableIdentity = 0;
        bool m_lua = false;
        CodeEditorState m_editor;
        CodeAssist m_luaAssist;
        CodeAssist m_asmAssist;
        std::string m_result;
        bool m_resultOk = true;
        std::string m_shownError;
        int m_errorLine = 0;
        bool m_errorOpen = false;
        int m_task = 0;
        bool m_stopped = false;
        void ErrorPopup();
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        TemplateRunner m_templates;
    };
}
