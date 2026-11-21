#pragma once

#include "Templates/TemplateCheck.h"
#include "Templates/TemplateFiles.h"
#include "Templates/TemplateLibrary.h"
#include "UI/CodeEditor.h"
#include "UI/Widgets.h"
#include "UI/ToolWindowBase.h"
#include "UI/Workspace.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::UI
{
    // The Templates window: the template library on the left, the template
    // being edited in the middle (script, settings form, settings file) and
    // the script it generates on the right, regenerated while typing.
    class TemplateManager : public ToolWindowBase
    {
    public:
        explicit TemplateManager(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }
        void Select(const std::string& id);
        void NewTemplate(const std::string& script);

    private:
        bool EscapeCloses() const override { return false; }
        struct Draft
        {
            std::string Id;
            bool IsNew = false;
            bool BuiltIn = false;
            std::string Script;
            std::string SavedScript;
            Templates::SettingsForm Form;
            std::string SettingsText;
            std::string SavedSettingsText;
            std::string OriginalCategory;
            bool RawEdited = false;
            bool SettingsHadComments = false;
            std::string SettingsError;
        };

        enum Tab
        {
            ScriptTabIndex,
            SettingsTabIndex,
            SettingsFileTabIndex
        };

        struct Problem
        {
            std::string Text;
            std::string Detail;
            bool Error = false;
            int Line = 0;
            int Tab = ScriptTabIndex;
        };

        void Menu();
        void Shortcuts();
        void LibraryPane();
        void Tree();
        void EditorPane();
        void EditorHeader();
        void ScriptTab();
        void SettingsTab();
        void RawTab();
        void ValuesPanel(float width, float height);
        void PreviewPane();
        void PreviewInputs();
        void ProblemList(float height);
        void RefreshPreview();
        void JumpTo(const Problem& problem);
        void Popups();
        void Load(const Templates::TemplateInfo& info);
        void ResetEditors(bool readOnly);
        void FormChanged();
        bool Dirty() const;
        bool Save();
        void Revert();
        void Duplicate(const Templates::TemplateInfo& info);
        void EditCopy();
        void ImportFolder();
        void OpenFolder();
        void CheckAll();
        void Reload();
        void RequestSelect(const std::string& id, bool fresh, const std::string& script = {});
        void Message(std::string text);
        const Templates::TemplateInfo* Current();
        Templates::Library& Library();
        std::filesystem::path Root() const;

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_closing = false;
        std::optional<Draft> m_draft;
        std::string m_filter;
        std::string m_valueFilter;
        CodeEditorState m_scriptEditor;
        CodeEditorState m_rawEditor;
        CodeEditorState m_previewEditor;
        CodeAssist m_asmAssist;
        CodeAssist m_valueAssist;

        bool m_showLibrary = true;
        bool m_showValues = true;
        bool m_showPreview = true;
        float m_libraryWidth = 0.0f;
        float m_previewWidth = 0.0f;
        float m_valuesWidth = 0.0f;
        float m_problemsHeight = 0.0f;
        int m_tab = ScriptTabIndex;
        int m_requestTab = -1;

        std::string m_pendingId;
        bool m_pendingNew = false;
        std::string m_pendingScript;
        Dialog m_unsavedDialog;
        Dialog m_deleteDialog;
        Dialog m_reportDialog;
        Dialog m_messageDialog;
        std::string m_message;
        std::vector<Templates::CheckEntry> m_report;

        std::string m_previewText;
        bool m_previewFailed = false;
        std::string m_previewSignature;
        std::string m_seenSignature;
        std::vector<Problem> m_problems;
        bool m_preview14 = false;
        double m_changedAt = 0.0;
        std::map<std::string, std::string> m_previewInputs;
    };
}
