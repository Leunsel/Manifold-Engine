#pragma once

#include "App/Commands.h"
#include "Lua/UiForm.h"
#include "UI/CodeEditor.h"
#include "UI/ToolWindowBase.h"
#include "UI/UiHost.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    // Lays out a manifold.ui window by hand and writes the Lua around it: the
    // layout goes to a .form.lua file that manifold.ui.form.load builds, or
    // into a script that builds the window with plain manifold.ui calls.
    class FormEditorWindow : public ToolWindowBase
    {
    public:
        explicit FormEditorWindow(Workspace& workspace);
        ~FormEditorWindow() override;
        void Draw() override;
        bool OpenFile(const std::filesystem::path& path);

    private:
        enum class Pending
        {
            None,
            New,
            Open,
            Close,
        };

        enum class Drop
        {
            Before,
            Inside,
            After,
        };

        bool EscapeCloses() const override { return false; }

        void Menu();
        void Shortcuts();
        void RunCommand(Command command);
        void Toolbar();
        void Palette();
        void Outline();
        void OutlineNode(int id, int depth);
        void NodeMenu(int id);
        void Preview();
        void PreviewOverlay(ImVec2 min, ImVec2 max);
        void Properties();
        void PropertyRow(Lua::UiNode& node, const Lua::FormProp& prop);
        void HandlerRow(Lua::UiNode& node, const Lua::FormHandler& handler);
        void NameRow(Lua::UiNode& node);
        void CodePane(float height);
        void Popups();

        void Remember(ImGuiID source = 0);
        void Changed();
        void Undo();
        void Redo();
        void Select(int id);
        void Add(Lua::UiKind kind);
        void AddInto(Lua::UiKind kind, int parent, int index);
        bool DropAt(int target, Drop where, int& parent, int& index) const;
        void Move(int delta);
        void Wrap(int id, Lua::UiKind kind);
        void Delete();
        void Duplicate();
        void ToggleTry();
        void RunScript();

        void Reset(Lua::FormDocument form, std::filesystem::path path);
        void AskThen(Pending pending);
        void DoPending();
        bool Save(bool as);
        void Export();
        void Open();
        std::string Title() const;
        std::string ScriptFileName() const;
        const std::string& Code();
        int Container(int id) const;
        int TabOf(int id) const;

        Workspace& m_workspace;
        Engine& m_engine;
        Lua::FormDocument m_form;
        Lua::FormDocument m_try;
        std::unique_ptr<UiHost> m_host;
        std::unique_ptr<UiHost> m_tryHost;
        UiDesign m_design;
        bool m_trying = false;
        int m_selected = 0;
        int m_hovered = 0;
        int m_lastTab = 0;
        bool m_scrollOutline = false;

        std::deque<Lua::FormDocument> m_undo;
        std::deque<Lua::FormDocument> m_redo;
        ImGuiID m_lastSource = 0;

        std::filesystem::path m_path;
        bool m_dirty = false;
        Pending m_pending = Pending::None;
        Dialog m_unsavedDialog;
        Dialog m_problemsDialog;

        int m_style = 0;
        std::string m_code;
        std::uint64_t m_revision = 1;
        std::uint64_t m_codeRevision = 0;
        int m_codeStyle = -1;
        CodeEditorState m_codeEditor;
        bool m_showCode = true;

        float m_leftWidth = 0.0f;
        float m_rightWidth = 0.0f;
        float m_codeHeight = 0.0f;
        std::string m_iconFilter;
    };
}
