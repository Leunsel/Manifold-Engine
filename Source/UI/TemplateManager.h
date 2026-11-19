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
    class TemplateManager : public ToolWindowBase
    {
    public:
        explicit TemplateManager(Workspace& workspace);
        void Draw() override;
        void Show() { m_open = true; m_bringToFront = true; }
        void Select(const std::string& id);
        void NewTemplate(const std::string& script);

    private:
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

        void Toolbar();
        void Tree();
        void Editor();
        void ScriptTab();
        void SettingsTab();
        void RawTab();
        void ValuesPanel(float width, float height);
        void Preview();
        void Popups();
        void Load(const Templates::TemplateInfo& info);
        void FormChanged();
        bool Dirty() const;
        bool Save();
        void Reload();
        void RequestSelect(const std::string& id, bool fresh, const std::string& script = {});
        void Message(std::string text);
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
        std::string m_previewError;
        std::string m_previewSignature;
        std::string m_seenSignature;
        std::vector<std::string> m_previewWarnings;
        bool m_preview14 = false;
        double m_changedAt = 0.0;
        std::map<std::string, std::string> m_previewInputs;
    };
}
