#pragma once

#include "App/Commands.h"
#include "Debug/Trace.h"
#include "Disasm/AssemblySearch.h"
#include "Disasm/CodeDissect.h"
#include "Target/PatchScan.h"
#include "Lua/LuaEngine.h"
#include "UI/AddressActions.h"
#include "UI/BackgroundTask.h"
#include "UI/CodeEditor.h"
#include "UI/Templates.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Manifold::UI
{
    class ToolWindowBase : public ToolWindow
    {
    public:
        bool Closed() const override { return !m_open && !Busy(); }
        void Show() { m_open = true; m_bringToFront = true; }

    protected:
        virtual bool Busy() const { return false; }

        bool BeginWindow(const char* title, ImVec2 size, ImGuiWindowFlags flags = 0, ImVec2 minimum = ImVec2(260.0f, 160.0f))
        {
            if (m_bringToFront)
            {
                ImGui::SetNextWindowFocus();
                m_bringToFront = false;
            }
            if (BeginToolWindow(title, &m_open, size, flags, minimum))
                return true;
            EndToolWindow();
            return false;
        }

        bool m_open = true;
        bool m_bringToFront = true;
    };

    class MainWindow
    {
    public:
        explicit MainWindow(Workspace& workspace);
        void Draw();
        void FocusScanValue() { m_focusValue = true; }

    private:
        void TitleBar();
        void ProcessBar();
        void ScanArea(float height);
        void ResultsCard(float width, float height);
        void ScanCard(float width, float height);
        void AddressListCard(float height);
        void StatusBar();
        void RecordRow(Table::MemoryRecord& record, int depth, int& row, bool match = true);
        void BuildRows();
        bool RowMatches(const Table::MemoryRecord& record) const;
        void ListKeys();
        void SelectRow(int index, bool extend);
        void StartInlineEdit(Table::MemoryRecord& record);
        void InlineValue(Table::MemoryRecord& record);
        void ProgressLine(float right);
        std::function<void()> StickyHeader();
        void DropOnRecord(Table::MemoryRecord& record, int depth);
        std::vector<Table::MemoryRecord*> DraggedRecords(int draggedId);
        void RecordMenu(Table::MemoryRecord& record);
        void ValueDialog();
        bool ValueChoices(const std::vector<Table::DropDownEntry>& entries);
        void AddAddressDialog();
        void DescriptionDialog();
        void HotkeyHandling();
        void DoFirstScan();
        void DoNextScan();
        void NewScan();
        Scan::ScanOptions CurrentOptions() const;
        std::vector<Table::MemoryRecord*> SelectedRecords();
        void SaveOptions();
        void OpenValueDialog(Table::MemoryRecord& record);
        void MainShortcuts();
        void ToggleCompactMode();
        void ToggleReleaseMode();
        void OpenAddDialog();
        void AddScriptRecord();
        void ResultMenu(std::uint64_t index);
        void RunResultCommand(Command command, std::uint64_t index);
        void ResultValueDialog();
        void SaveResultsDialog();
        void RefreshScriptMenu();
        bool ScriptMenuMerged(const Lua::ScriptMenuItem& item) const;
        void ScriptMenuInto(const char* merge, int& opened, int& clicked);
        void ScriptMenuShortcuts();
        std::string ResultText(const Scan::ScanRecord& record, std::span<const std::uint8_t> bytes);
        void AddResult(const Scan::ScanRecord& record);
        Status FillCustom(Scan::ScanOptions& options, const std::string& name) const;
        std::string ScanTypeLabel() const;
        Table::MemoryRecord* FocusRecord();
        void WriteValues(const std::vector<int>& ids, const std::string& text);
        void UndoValueChange();
        void DeleteSelectedRecords(bool ask);
        void RunRecordCommand(Command command, Table::MemoryRecord* record);
        void DetectMono();
        void MonoMenu();

        struct ValueUndo
        {
            int Id = -1;
            std::string Text;
        };

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_release = false;
        std::vector<ValueUndo> m_valueUndo;
        struct ResultTarget
        {
            std::uint64_t Address = 0;
            Scan::ValueType Type = Scan::ValueType::FourBytes;
        };

        bool m_resultValueDialog = false;
        std::string m_resultValueText;
        std::vector<ResultTarget> m_resultValueTargets;
        bool m_saveResultsDialog = false;
        std::string m_saveResultsName;
        std::vector<Lua::ScriptMenuItem> m_scriptMenu;
        std::uint64_t m_scriptMenuGeneration = 0;
        bool m_monoDetected = false;
        std::uint64_t m_monoGeneration = ~0ull;
        double m_monoCheckAt = 0.0;
        double m_monoRefreshAt = 0.0;

        int m_typeIndex = 2;
        Scan::ScanKind m_scanKind = Scan::ScanKind::Single;
        std::string m_customName;
        Scan::CompareTo m_compareTo = Scan::CompareTo::Previous;
        std::string m_savedName;
        int m_compareIndex = 0;
        std::string m_value;
        std::string m_value2;
        bool m_hex = false;
        bool m_unicode = false;
        bool m_caseSensitive = true;
        int m_floatMode = 0;
        int m_writable = 1;
        int m_executable = 2;
        int m_copyOnWrite = 0;
        bool m_fastScan = true;
        int m_alignment = 4;
        bool m_pause = false;
        bool m_private = true;
        bool m_image = true;
        bool m_mapped = false;
        std::string m_start = "0";
        std::string m_stop = "7FFFFFFFFFFF";
        bool m_focusValue = false;
        float m_topHeight = 0.0f;
        float m_resultsShare = 0.58f;

        std::set<std::uint64_t> m_selectedResults;
        std::uint64_t m_lastClickedResult = 0;
        std::map<std::uint64_t, std::string> m_liveValues;
        double m_liveAt = -1.0;
        int m_liveScan = -1;

        int m_lastClickedRecord = -1;
        int m_dragRecord = 0;

        bool m_valueDialog = false;
        std::string m_valueText;
        std::vector<int> m_valueTargets;
        std::string m_valueSearch;
        std::string m_valueCurrent;
        std::string m_valueFiltered;
        std::vector<int> m_valueVisible;
        std::size_t m_valueEntries = 0;
        int m_valueCursor = -1;
        int m_valueScroll = 0;
        float m_valueColumn = 0.0f;
        bool m_addDialog = false;
        std::string m_addAddress;
        std::string m_addDescription;
        int m_addType = 2;
        bool m_descriptionDialog = false;
        std::string m_descriptionText;
        int m_descriptionTarget = 0;
        std::string m_tableFilter;
        struct ListRow
        {
            int Id = 0;
            int Depth = 0;
            bool Match = true;
        };
        struct RowSpot
        {
            int Id = 0;
            float Top = 0.0f;
        };
        std::vector<ListRow> m_rows;
        std::vector<RowSpot> m_spots;
        int m_anchorRecord = -1;
        int m_scrollToRecord = -1;
        int m_inlineEdit = -1;
        std::string m_inlineText;
        bool m_inlineFocus = false;
        bool m_filterActiveOnly = false;
        int m_filterKind = 0;
        std::string m_recentChecked;
        bool m_recentExists = false;
        double m_recentCheckedAt = -100.0;
    };

    class ProcessPicker : public ToolWindow
    {
    public:
        explicit ProcessPicker(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show();

    private:
        void Refresh();

        Workspace& m_workspace;
        bool m_open = true;
        std::vector<Target::ProcessEntry> m_list;
        std::string m_filter;
        bool m_windowsOnly = false;
        std::uint32_t m_selected = 0;
        bool m_focusFilter = true;
    };

    class MemoryViewer : public ToolWindow
    {
    public:
        MemoryViewer(Workspace& workspace, int number);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show(std::uint64_t address, bool hexView);
        void Select(std::uint64_t address);
        void ShowHex(std::uint64_t address, std::uint64_t start, std::uint64_t stop);
        void Raise();
        int Number() const { return m_number; }

    private:
        enum RegionMode
        {
            RegionSave,
            RegionLoad,
            RegionFill
        };

        struct MemoryEdit
        {
            std::uint32_t Process = 0;
            std::uint64_t Address = 0;
            std::vector<std::uint8_t> Original;
        };

        void MenuBar();
        void Toolbar();
        void Disassembly(float height);
        void HexView(float height);
        void Registers(float width, float height, const Debug::BreakInfo& info, bool running);
        void Stack(float width, float height, const Debug::BreakInfo& info, bool running);
        Debug::BreakInfo ShownBreak() const;
        bool RegisterLine(const std::string& id, std::string_view name, const std::string& editText, float rowHeight, float nameWidth, std::string& edited, ImVec2& valueMin, ImVec2& valueMax);
        void CodeMenu(const Disasm::Instruction& instruction);
        void HexMenu(std::uint64_t address);
        void BookmarkItems();
        void GotoDialog();
        void CommentDialog();
        void AssembleDialog();
        void FindDialog();
        void AllocateDialog();
        void InjectDialog();
        void RegionDialog();
        void ThreadDialog();
        void OpenRegionDialog(int mode);
        void Navigate(std::uint64_t address, bool remember = true);
        void HandleCodeKeys();
        void HandleHexKeys();
        void Shortcuts();
        void ScriptMenuBar();
        void ScriptShortcuts(std::vector<ImGuiKeyChord>& taken);
        void PublishSelection();
        void RunCommand(Command command);
        void Bookmark(bool set, int slot);
        Status WriteBytes(std::uint64_t address, std::span<const std::uint8_t> bytes);
        void UndoEdit();
        void Nop(const Disasm::Instruction& instruction);
        int HexValueSize() const;
        Scan::ValueType HexValueType() const;

        std::vector<MemoryEdit> m_undo;
        std::array<std::uint64_t, 10> m_bookmarks{};
        std::vector<std::uint64_t> m_hexHistory;
        int m_regionMode = RegionSave;
        bool m_regionOpen = false;
        std::string m_regionAddress;
        std::string m_regionSize = "1000";
        std::string m_regionFill = "90";
        std::string m_regionError;
        bool m_threadOpen = false;
        std::string m_threadAddress;
        std::string m_threadParameter = "0";
        std::string m_threadError;

        Workspace& m_workspace;
        Engine& m_engine;
        TemplateRunner m_templates;
        int m_number = 1;
        std::string m_title;
        bool m_open = true;
        bool m_bringToFront = false;

        std::uint64_t m_top = 0;
        std::uint64_t m_selected = 0;
        std::uint64_t m_selectionEnd = 0;
        std::vector<std::uint64_t> m_history;
        std::vector<Disasm::Instruction> m_lines;
        int m_visibleLines = 30;
        bool m_followBreaks = true;
        float m_registersWidth = 0.0f;
        Debug::BreakInfo m_heldBreak;
        bool m_registersRunning = false;
        std::uint64_t m_conditionSerial = 0;
        std::uint64_t m_conditionAddress = 0;
        Disasm::Instruction m_conditionInstruction;
        int m_vectorFormat = 0;
        std::string m_registerEdit;
        std::string m_registerEditText;
        bool m_registerEditFocus = false;
        std::uint64_t m_registerSerial = 0;
        bool m_havePreviousRegisters = false;
        Disasm::RegisterValues m_previousRegisters;
        Disasm::RegisterValues m_lastRegisters;
        Debug::VectorRegisters m_previousVectors;
        Debug::VectorRegisters m_lastVectors;
        std::uint64_t m_lastBreak = 0;
        float m_codeShare = 0.62f;
        bool m_focusCode = false;
        float m_addressWidth = 0.0f;
        float m_bytesWidth = 0.0f;
        float m_opcodeWidth = 0.0f;
        float m_dragStartWidth = 0.0f;
        bool m_addressAuto = true;
        std::uint32_t m_addressProcess = 0;

        std::uint64_t m_hexTop = 0;
        std::uint64_t m_hexSelected = 0;
        std::uint64_t m_hexMarkStart = 0;
        std::uint64_t m_hexMarkStop = 0;
        std::vector<Lua::ScriptMenuItem> m_scriptPopup;
        std::vector<Lua::ScriptMenuItem> m_scriptMenu;
        std::uint64_t m_scriptMenuGeneration = 0;
        int m_hexNibble = 0;
        bool m_hexFocus = false;
        int m_hexDisplay = 0;
        int m_hexColumns = 16;
        bool m_hexEditing = false;
        std::vector<std::uint8_t> m_hexPrevious;
        std::uint64_t m_hexPreviousTop = 0;
        std::vector<double> m_hexChangedAt;
        std::string m_valueBuffer;

        bool m_gotoOpen = false;
        bool m_gotoHex = false;
        std::string m_gotoText;
        std::string m_gotoError;
        bool m_assembleOpen = false;
        std::uint64_t m_assembleAddress = 0;
        std::string m_assembleText;
        std::string m_assembleError;
        bool m_assembleNopFill = true;
        bool m_findOpen = false;
        std::string m_findText;
        std::string m_findError;
        bool m_allocateOpen = false;
        std::string m_allocateSize = "1000";
        bool m_injectOpen = false;
        std::string m_injectPath;
        std::map<std::uint64_t, std::vector<std::uint8_t>> m_nopped;
        std::weak_ptr<Target::Process> m_noppedProcess;

        ImGuiID m_viewport = 0;
        bool m_commentOpen = false;
        bool m_commentHeader = false;
        std::uint64_t m_commentAddress = 0;
        std::string m_commentText;
    };

    class StructureWindow : public ToolWindow
    {
    public:
        StructureWindow(Workspace& workspace, int number);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show(std::uint64_t address, const std::string& structure = {});

    private:
        struct Column
        {
            AddressField Address;
            std::uint64_t Value = 0;
            bool Valid = false;
        };

        struct Block
        {
            std::vector<std::uint64_t> Bases;
            std::string Path;
        };

        struct Row
        {
            Table::StructDefinition* Definition = nullptr;
            int Field = 0;
            int Depth = 0;
            int Block = 0;
            bool Expanded = false;
            int Twin = 0;
        };

        struct Snapshot
        {
            int Block = 0;
            std::size_t Column = 0;
            std::uint64_t Start = 0;
            std::vector<std::uint8_t> Bytes;
            std::vector<std::uint8_t> Valid;
            bool Whole = false;
        };

        void Toolbar();
        void MenuBar();
        static bool IsWindowCommand(Command command);
        void RunCommand(Command command);
        void FieldShortcuts(int pointerSize);
        void RunFieldCommand(Command command, Table::StructDefinition& definition, Table::StructField& field, std::uint64_t address, const std::string& path, int pointerSize);
        void BuildRows(Table::StructDefinition& definition, std::vector<std::uint64_t> bases, const std::string& path, int depth);
        int FindEditRow() const;
        void ReadVisible(int first, int last);
        bool ReadField(int block, std::size_t column, std::uint64_t address, std::uint8_t* out, std::size_t size) const;
        void DrawRow(int index, int pointerSize);
        void FieldMenu(Table::StructDefinition& definition, Table::StructField& field, const std::vector<std::uint64_t>& addresses, const std::string& path);
        Table::StructDefinition& Guess(const std::string& name, std::uint64_t address, int size);
        void Fill(Table::StructDefinition* definition, std::uint64_t address, int size, bool mono);
        void LookupName(std::uint64_t address, bool containing, std::function<void(std::optional<Lua::StructureLookup>)> done);
        void ExpandPointer(Table::StructDefinition& parent, Table::StructField& field, std::uint64_t target, const std::string& path);
        void PrepareNew(std::uint64_t address);
        std::string FieldText(const Table::StructField& field, std::span<const std::uint8_t> bytes, std::uint64_t address, int pointerSize);
        Status WriteBits(const Table::StructField& field, std::uint64_t address, const std::string& text);
        Table::StructDefinition* Current();

        Workspace& m_workspace;
        Engine& m_engine;
        [[maybe_unused]] int m_number = 1;
        std::string m_title;
        bool m_open = true;
        std::vector<Column> m_columns;
        std::string m_structure;
        std::set<std::string> m_expanded;
        std::vector<Block> m_blocks;
        std::vector<Row> m_rows;
        std::vector<Snapshot> m_snapshots;
        int m_guessSize = 4096;
        bool m_newOpen = false;
        std::string m_newName;
        std::uint64_t m_newFrom = 0;
        std::uint64_t m_newStart = 0;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        std::uint64_t m_lookupSerial = 0;
        bool m_lookupRunning = false;
        std::string m_suggestedName;
        std::set<const Table::StructDefinition*> m_filling;
        std::set<std::string> m_childLookups;
        bool m_renameOpen = false;
        std::string m_renameText;
        std::string m_editPath;
        std::string m_editText;
        std::string m_selectedPath;
        bool m_editName = false;
        std::string m_addOffset;
        std::string m_pendingStructure;
        int m_pendingOffset = 0;
        bool m_pendingInsert = false;
    };

    class ScriptWindow : public ToolWindow
    {
    public:
        ScriptWindow(Workspace& workspace, int number, std::string script, int recordId, bool lua);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        int RecordId() const { return m_recordId; }
        void Focus() { m_bringToFront = true; }

    private:
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
        std::string m_text;
        std::string m_saved;
        std::filesystem::path m_path;
        int m_recordId = 0;
        bool m_lua = false;
        bool m_open = true;
        bool m_bringToFront = true;
        CodeEditorState m_editor;
        std::string m_result;
        bool m_resultOk = true;
        int m_task = 0;
        bool m_stopped = false;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        TemplateRunner m_templates;
    };

    class WatchWindow : public ToolWindow
    {
    public:
        WatchWindow(Workspace& workspace, int watchId, Debug::WatchKind kind, std::uint64_t address, int size);
        ~WatchWindow() override;
        void Draw() override;
        bool Closed() const override { return !m_open; }

    private:
        Workspace& m_workspace;
        Engine& m_engine;
        std::string m_title;
        bool m_open = true;
        int m_watchId = 0;
        Debug::WatchKind m_kind;
        [[maybe_unused]] std::uint64_t m_address = 0;
        [[maybe_unused]] int m_size = 4;
        Debug::Watch m_copy;
        int m_selected = -1;
        std::map<std::uint64_t, std::string> m_text;
    };

    class LuaWindow : public ToolWindow
    {
    public:
        explicit LuaWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        void Execute();
        void MarkError(const Status& status);
        void FunctionList();
        void RunCommand(Command command);
        void RememberShowOnPrint(bool show);

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_open = true;
        bool m_bringToFront = true;
        int m_task = 0;
        bool m_stopped = false;
        std::shared_ptr<int> m_alive = std::make_shared<int>(0);
        std::string m_text;
        std::filesystem::path m_path;
        CodeEditorState m_editor;
        CodeAssist m_assist;
        float m_outputShare = 0.35f;
        std::uint64_t m_seenOutput = 0;
        bool m_followOutput = true;
        bool m_showFunctions = false;
        float m_functionsWidth = 0.0f;
        std::string m_functionFilter;
        std::string m_listedFilter;
        double m_listedAt = -1.0;
        std::vector<CodeSuggestion> m_listed;
    };

    class LogWindow : public ToolWindow
    {
    public:
        explicit LogWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        Workspace& m_workspace;
        bool m_open = true;
        bool m_bringToFront = true;
        int m_level = 1;
        std::string m_search;
        bool m_follow = true;
        std::uint64_t m_generation = 0;
        std::vector<LogEntry> m_entries;
    };

    class SettingsList;

    class SettingsWindow : public ToolWindow
    {
    public:
        static constexpr int KeyboardPage = 5;

        explicit SettingsWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show(int page = -1)
        {
            m_open = true;
            m_bringToFront = true;
            if (page >= 0)
            {
                m_page = page;
                m_search.clear();
                m_scrollTop = true;
            }
        }

    private:
        void Navigation(float width, float height);
        void PageHeader(Settings& settings);
        bool GeneralPage(SettingsList& list, Settings& settings);
        bool AppearancePage(SettingsList& list, Settings& settings);
        bool ScannerPage(SettingsList& list, Settings& settings);
        bool ScriptsPage(SettingsList& list, Settings& settings);
        bool DebuggerPage(SettingsList& list, Settings& settings);
        bool ShortcutsPage(SettingsList& list, Settings& settings);

        Workspace& m_workspace;
        bool m_open = true;
        bool m_bringToFront = true;
        int m_page = 0;
        bool m_scrollTop = false;
        bool m_allImGuiColors = false;
        bool m_themeReset = false;
        bool m_shortcutsReset = false;
        int m_captureCommand = -1;
        int m_captureSlot = 0;
        std::string m_search;
        std::string m_autoAttach;
        std::string m_saveError;
        double m_themeSaveAt = 0.0;
        double m_savedAt = -100.0;
    };

    class RecordEditor : public ToolWindow
    {
    public:
        RecordEditor(Workspace& workspace, int recordId);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        int RecordId() const { return m_recordId; }
        void Focus() { m_bringToFront = true; }

    private:
        void Load(const Table::MemoryRecord& record);
        void Apply(Table::MemoryRecord& record);
        void AddressSection(Table::MemoryRecord& record);
        void ValueSection();
        void DropDownSection(Table::MemoryRecord& record);
        void ChildrenSection(Table::MemoryRecord& record);
        void HotkeySection();
        void LookSection();
        void CopyAddress(const char* id, std::uint64_t address);

        Workspace& m_workspace;
        Engine& m_engine;
        int m_recordId = 0;
        std::string m_title;
        bool m_open = true;
        bool m_bringToFront = true;
        std::string m_description;
        std::string m_address;
        std::vector<std::string> m_offsets;
        int m_type = 2;
        int m_length = 10;
        bool m_unicode = false;
        bool m_zeroTerminate = true;
        int m_bitStart = 0;
        int m_bitLength = 1;
        bool m_hex = false;
        bool m_signed = true;
        Table::RecordOptions m_options;
        int m_freeze = 0;
        std::string m_dropDown;
        std::string m_dropDownLink;
        int m_dropDownSource = 0;
        bool m_hasAddress = false;
        std::string m_copied;
        double m_copiedAt = -10.0;
        bool m_async = false;
        bool m_dropDownOnly = false;
        bool m_dropDownDescription = false;
        std::vector<Table::Hotkey> m_hotkeys;
        bool m_capturing = false;
        int m_captureIndex = -1;
        std::vector<int> m_capture;
        float m_color[3] = { 1.0f, 1.0f, 1.0f };
        bool m_hasColor = false;
        std::string m_comment;
        std::string m_error;
    };

    class SymbolsWindow : public ToolWindow
    {
    public:
        explicit SymbolsWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        Workspace& m_workspace;
        bool m_open = true;
        bool m_bringToFront = true;
        std::string m_name;
        std::string m_address;
        std::string m_error;
        std::string m_filter;
        [[maybe_unused]] int m_tab = 0;
        std::string m_module;
    };

    class TableFilesWindow : public ToolWindow
    {
    public:
        explicit TableFilesWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        void Select(const std::string& name);
        void Import();
        void ImportFolder();
        void AddFiles(const std::vector<std::filesystem::path>& paths);
        void NewTextFile();
        void Export(const Table::TableFile& file);

        Workspace& m_workspace;
        bool m_open = true;
        bool m_bringToFront = true;
        std::string m_filter;
        std::string m_selected;
        bool m_renaming = false;
        bool m_renameFocus = false;
        std::string m_rename;
        std::string m_text;
        std::string m_textFor;
        bool m_textDirty = false;
        CodeEditorState m_editor;
    };

    class RegionsWindow : public ToolWindow
    {
    public:
        explicit RegionsWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; m_dirty = true; }

    private:
        Workspace& m_workspace;
        bool m_open = true;
        bool m_bringToFront = true;
        bool m_dirty = true;
        bool m_committedOnly = true;
        std::string m_filter;
        std::vector<Target::RegionInfo> m_regions;
    };

    struct ModuleScope
    {
        int Kind = 0;
        std::string Module;
    };

    inline constexpr int kScopeAllMemory = -2;
    inline constexpr int kScopeExecutable = -1;

    void ModuleScopeCombo(const char* id, Engine& engine, ModuleScope& scope, float width, bool memory = false);
    std::vector<Target::ModuleInfo> ScopeModules(Engine& engine, const ModuleScope& scope);
    std::string ScopeName(Engine& engine, const ModuleScope& scope);

    class CodeDissectWindow : public ToolWindowBase
    {
    public:
        enum Tab
        {
            Strings,
            Functions,
            References
        };

        explicit CodeDissectWindow(Workspace& workspace);
        ~CodeDissectWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Show(int tab, std::uint64_t address);

    private:
        void Start();
        void Collect();
        void Toolbar();
        void StringsTab(const Disasm::DissectResult& result, float height);
        void FunctionsTab(const Disasm::DissectResult& result, float height);
        void ReferencesTab(const Disasm::DissectResult& result, float height);
        void ReferenceList(const Disasm::DissectResult& result);

        Workspace& m_workspace;
        int m_tab = Strings;
        int m_wantTab = -1;
        ModuleScope m_scope;
        std::string m_filter;
        std::string m_referenceText;
        std::uint64_t m_selected = 0;
        std::shared_ptr<const Disasm::DissectResult> m_sorted;
        std::vector<std::size_t> m_byCalls;
        std::vector<std::size_t> m_visible;
        std::string m_visibleFilter;
        std::shared_ptr<const Disasm::DissectResult> m_visibleFor;
        BackgroundTask<std::shared_ptr<Disasm::DissectResult>, Disasm::DissectProgress> m_task;
        std::uint64_t m_generation = 0;
    };

    class AsmSearchWindow : public ToolWindowBase
    {
    public:
        explicit AsmSearchWindow(Workspace& workspace);
        ~AsmSearchWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }

    private:
        void Start();
        void Collect();
        void Results();

        Workspace& m_workspace;
        bool m_focus = true;
        ModuleScope m_scope;
        std::string m_pattern;
        std::string m_searched;
        std::shared_ptr<Target::Process> m_process;
        BackgroundTask<Disasm::AsmSearchResult, Disasm::AsmSearchProgress> m_task;
        std::optional<Disasm::AsmSearchResult> m_result;
    };

    std::optional<Scan::Pattern> BytePattern(std::string_view text);

    class ByteSearchWindow : public ToolWindowBase
    {
    public:
        explicit ByteSearchWindow(Workspace& workspace);
        ~ByteSearchWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }
        void Search(const std::string& pattern);

    private:
        struct Hit
        {
            std::uint64_t Address = 0;
            bool Code = false;
        };

        struct Outcome
        {
            std::vector<Hit> Hits;
            std::size_t Length = 0;
            bool Truncated = false;
            bool Cancelled = false;
            double Seconds = 0.0;
        };

        void Start();
        void Collect();
        void Keys();
        void Results();
        void Reveal(int row, bool raise);

        Workspace& m_workspace;
        bool m_focus = true;
        ModuleScope m_scope;
        std::string m_pattern;
        std::string m_error;
        std::string m_searched;
        std::string m_searchedPattern;
        std::shared_ptr<Target::Process> m_process;
        BackgroundTask<Outcome, Scan::PatternProgress> m_task;
        std::optional<Outcome> m_result;
        int m_selected = -1;
        bool m_scrollToSelected = false;
    };

    class PatchScanWindow : public ToolWindowBase
    {
    public:
        explicit PatchScanWindow(Workspace& workspace);
        ~PatchScanWindow() override;
        void Draw() override;
        bool Busy() const override { return m_task.Running(); }

    private:
        void Start();
        void Collect();
        void Notes();
        void Results();
        void PatchRow(std::size_t index);
        std::vector<Disasm::Instruction> Listing(std::uint64_t address, std::span<const std::uint8_t> bytes, std::uint64_t size, int limit) const;

        Workspace& m_workspace;
        bool m_windowsModules = false;
        bool m_allNotes = false;
        std::string m_filter;
        std::shared_ptr<Target::Process> m_scanned;
        BackgroundTask<Target::PatchScanResult, Target::PatchScanProgress> m_task;
        std::optional<Target::PatchScanResult> m_result;
        double m_seconds = 0.0;
        int m_restore = -1;
    };

    class BreakpointWindow : public ToolWindow
    {
    public:
        explicit BreakpointWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; }
        void Edit(std::uint64_t address, Debug::BreakpointType type, int size, ImGuiID viewport = 0);

    private:
        void EditDialog();
        void Apply();

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_open = true;
        bool m_bringToFront = true;
        std::uint64_t m_generation = 0;
        std::vector<Debug::Breakpoint> m_list;

        bool m_editOpen = false;
        bool m_editExisting = false;
        std::uint64_t m_editAddress = 0;
        int m_editType = 0;
        int m_editSize = 1;
        std::string m_editAddressText;
        std::string m_editCondition;
        std::string m_editChanges;
        std::string m_editThread;
        bool m_editBreak = true;
        std::string m_editError;
        ImGuiID m_selfViewport = 0;
        ImGuiID m_modalViewport = 0;
        void DrawWindow();
    };

    class ThreadListWindow : public ToolWindow
    {
    public:
        explicit ThreadListWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; m_refreshAt = 0.0; }

    private:
        struct Row
        {
            std::uint32_t Id = 0;
            std::uint64_t Start = 0;
            std::uint64_t Teb = 0;
            int Priority = 0;
            bool Suspended = false;
            bool Hidden = false;
            std::uint64_t Rip = 0;
            bool HaveRip = false;
        };

        void Refresh();

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_open = true;
        bool m_bringToFront = true;
        double m_refreshAt = 0.0;
        std::uint32_t m_process = 0;
        std::vector<Row> m_rows;
        std::uint32_t m_selected = 0;
    };

    class TraceWindow : public ToolWindow
    {
    public:
        TraceWindow(Workspace& workspace, int number);
        ~TraceWindow() override;
        void Draw() override;
        bool Closed() const override { return !m_open; }
        int Number() const { return m_number; }
        void Start(std::uint64_t address, ImGuiID viewport = 0);

    private:
        void StartDialog();
        void Begin();
        void Poll();
        void Stop();
        void RowText(const Debug::TraceStep& step, int index);

        Workspace& m_workspace;
        Engine& m_engine;
        int m_number = 1;
        std::string m_title;
        bool m_open = true;
        bool m_bringToFront = true;

        bool m_startOpen = false;
        std::uint64_t m_address = 0;
        std::string m_addressText;
        std::string m_countText = "2000";
        std::string m_condition;
        std::string m_stopCondition;
        bool m_stepOver = false;
        std::string m_error;

        bool m_running = false;
        std::uint64_t m_generation = 0;
        std::vector<Debug::TraceStep> m_steps;
        int m_selected = -1;
        std::map<std::uint64_t, std::string> m_text;
        ImGuiID m_selfViewport = 0;
        ImGuiID m_modalViewport = 0;
        void DrawWindow();
    };

    class MonoWindow : public ToolWindow
    {
    public:
        explicit MonoWindow(Workspace& workspace);
        void Draw() override;
        bool Closed() const override { return !m_open; }
        void Show() { m_open = true; m_bringToFront = true; }

    private:
        void RebuildFilter();
        void SelectClass(int index);
        void MethodsPane();
        void FieldsPane();

        Workspace& m_workspace;
        Engine& m_engine;
        bool m_open = true;
        bool m_bringToFront = true;
        std::uint64_t m_generation = ~0ull;
        std::vector<Target::MonoClass> m_classes;
        std::vector<int> m_filtered;
        std::string m_filter;
        std::string m_lastFilter = "\x01";
        int m_selected = -1;
        std::string m_selectedName;
        std::vector<std::string> m_methods;
        std::vector<Target::MonoField> m_fields;
        std::map<std::string, std::uint64_t> m_resolved;
        float m_listWidth = 0.0f;
    };
}
