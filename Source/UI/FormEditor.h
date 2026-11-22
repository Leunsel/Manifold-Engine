#pragma once

#include "App/Commands.h"
#include "Lua/UiForm.h"
#include "Table/CeForms.h"
#include "UI/CodeEditor.h"
#include "UI/ToolWindowBase.h"
#include "UI/UiHost.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace Manifold
{
    class Engine;
}

namespace Manifold::UI
{
    // Lays out a manifold.ui window by hand and writes the Lua around it: the
    // form lives in its script as a table that manifold.ui.form.build makes
    // the window from, next to the handlers' code (one file). Older forms in
    // a .form.lua file with a linked script still open and can be converted.
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
            Template,
            Import,
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
        void Breadcrumb();
        void PaletteTile(Lua::UiKind kind, float width);
        void Outline();
        void OutlineNode(int id, int depth, std::vector<int>& path);
        void NodeMenu(int id);
        void Preview();
        void PreviewOverlay(ImVec2 min, ImVec2 max, int window);
        void PreviewFrame(Lua::FormDocument& form, int window, ImVec2 origin, ImVec2 areaMin, ImVec2 areaMax);
        ImVec2 FramePlace(int window) const;
        void SetFramePlace(int window, ImVec2 place);
        void JumpLinks(ImVec2 areaMin, ImVec2 areaMax);
        ImDrawList* Marks() const;
        void TryEvents(ImVec2 areaMin, ImVec2 areaMax);
        void AddModal(int opener = 0);
        struct DropSpot
        {
            int Target = 0;
            Drop Where = Drop::Inside;
        };
        DropSpot SpotAt(ImVec2 mouse, int moving) const;
        void DrawSpot(ImDrawList* draw, const DropSpot& spot) const;
        const UiDesign::Rect* RectOf(int id) const;
        bool Within(int node, int ancestor) const;
        void DropPayload(const ImGuiPayload* payload, const DropSpot& spot);
        void SyncImages(Lua::FormDocument& form);
        std::filesystem::path ResolveImage(const std::string& source) const;
        void Copy();
        void Cut();
        void Paste();
        void PasteText(const std::string& text);
        void TemplateMenu();
        void SnippetMenu();
        void SaveAsTemplate();
        std::filesystem::path TemplateFolder() const;
        std::filesystem::path LinkedScript() const;
        void SetLink(const std::filesystem::path& script);
        void LinkScript();
        void OpenLinked(int line = 0);
        void OpenHandler(Lua::UiNode& node, const Lua::FormHandler& handler);
        void FunctionChips(ImVec2 origin, ImVec2 areaMin, ImVec2 areaMax);
        void EnsureScript();
        bool SaveScript();
        bool ScriptDirty() const;
        bool Dirty() const;
        bool AddFunctions(const std::string& only = {});
        void ShowFunction(const std::string& name);
        void ScriptPane();
        void ImportFile();
        void ImportTable();
        void ImportChoices(std::vector<Table::CeFormEntry> entries, std::string source);
        void ImportEntry(const Table::CeFormEntry& entry);
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

        void Reset(Lua::FormDocument form, std::filesystem::path path, bool oneFile);
        void ConvertToOneFile();
        void ExportStandalone();
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
        int m_pressNode = 0;
        bool m_pressing = false;
        struct Frame
        {
            ImVec2 Min;
            ImVec2 Max;
            float Title = 0.0f;
        };
        std::map<int, Frame> m_frames;
        std::vector<std::pair<ImVec2, ImVec2>> m_grips;
        int m_gripWindow = 0;
        bool m_frameMoved = false; // a title drag or grip drag has changed something (one undo step, none for a click)
        bool m_gripping = false;
        int m_overlayWindow = 0;
        int m_flashWindow = 0;
        double m_flashUntil = 0.0;
        ImVec2 m_pan = ImVec2(0.0f, 0.0f);
        ImVec2 m_areaSize = ImVec2(0.0f, 0.0f);
        ImDrawList* m_marks = nullptr; // the marks layer of the frame being drawn (see PreviewFrame)
        std::map<int, ImDrawList*> m_frameMarks;                 // per window: its frame's marks layer, this frame only
        std::map<int, std::pair<ImVec2, ImVec2>> m_frameClips;   // per window: where its marks may draw
        ImVec2 m_surfaceMin, m_surfaceMax;                       // the visible work surface (tags may sit beside a form)
        ImGuiID m_previewMenu = 0;
        std::string m_activeChip;
        std::string m_hoveredChip;
        bool m_chipDragged = false;
        std::map<std::string, ImVec2> m_chipPlaces;
        std::string m_scriptDisk;
        // ∑ One file: m_path is the script itself, holding the form as
        // "local form = { ... }"; ScriptText shows that table as
        // Lua::kFormStandIn, and saving puts the table back. Otherwise
        // (an older .form.lua) the handlers live in a linked script.
        bool m_oneFile = true;
        std::string m_fileDisk; // one file: the whole file as last read or written
        bool m_crlf = false;    // one file: the file had CRLF line ends / a UTF-8 BOM; writing keeps them
        bool m_bom = false;
        double m_lockNoteUntil = 0.0;
        std::filesystem::path m_scriptPath;
        std::uint64_t m_scriptStamp = 0;
        double m_nextScriptPoll = 0.0;
        bool m_scriptConflict = false;
        CodeEditorState m_scriptEditor;
        CodeAssist m_assist;
        int m_codeTab = 0;
        std::set<std::string> m_badImages;
        std::string m_paletteFilter;
        float m_paletteHeight = 0.0f;
        std::vector<Lua::FormProblem> m_problems;
        std::set<int> m_problemNodes;
        bool m_scrollOutline = false;
        std::set<int> m_collapsed;
        float m_outlineX = 0.0f;
        float m_outlineWidth = 0.0f;
        int m_outlineRows = 0;
        ImVec2 m_menuSpacing = ImVec2(8.0f, 4.0f);
        int m_renameNode = 0;       // the widget whose name field is being typed into
        std::string m_renameFrom;   // its name when typing started

        std::deque<Lua::FormDocument> m_undo;
        std::deque<Lua::FormDocument> m_redo;
        ImGuiID m_lastSource = 0;

        std::filesystem::path m_path;
        bool m_dirty = false;
        Pending m_pending = Pending::None;
        std::string m_pendingTemplate;
        Lua::FormDocument m_pendingImport;
        Dialog m_unsavedDialog;
        Dialog m_problemsDialog;
        Dialog m_importDialog;
        Dialog m_notesDialog;
        std::vector<Table::CeFormEntry> m_importChoices;
        std::string m_importSource;
        int m_importChoice = 0;
        std::vector<std::string> m_importNotes;
        int m_importConverted = 0;

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
        std::string m_recordFilter;
    };
}
