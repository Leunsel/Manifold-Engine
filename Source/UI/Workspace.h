#pragma once

#include "App/Commands.h"
#include "App/Engine.h"
#include "UI/CodeEditor.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::UI
{
    inline constexpr int kNoRecord = -2;
    inline constexpr int kTableLuaScript = -1;

    class Shell
    {
    public:
        virtual void Minimise() = 0;
        virtual void ToggleMaximised() = 0;
        virtual bool Maximised() const = 0;
        virtual void Close() = 0;
        virtual void AddDragArea(float left, float top, float right, float bottom) = 0;
        virtual float CornerRadius() const = 0;
        virtual void* NativeWindow() const = 0;
        virtual void* Device() const = 0;
        virtual void RestartElevated() = 0;
        // closes like Close and starts the program again once everything is saved
        virtual void RestartAfterUpdate(const std::string& from) = 0;
        virtual void Restyle() = 0;

    protected:
        Shell() = default;
        Shell(const Shell&) = default;
        Shell& operator=(const Shell&) = default;
        ~Shell() = default;
    };

    class ToolWindow
    {
    public:
        virtual ~ToolWindow() = default;
        virtual void Draw() = 0;
        virtual bool Closed() const = 0;
    };

    class MainWindow;
    class MemoryViewer;
    class ScriptWindow;
    class StructureWindow;
    class WatchWindow;
    class LuaWindow;
    class LogWindow;
    class SettingsWindow;
    class ProcessPicker;
    class RecordEditor;

    enum class RecordField
    {
        None,
        Description,
        Address,
        Type,
    };
    class SymbolsWindow;
    class RegionsWindow;
    class TableFilesWindow;
    class PatchScanWindow;
    class CodeDissectWindow;
    class AsmSearchWindow;
    class ByteSearchWindow;
    class StaticPointersWindow;
    class SignatureWindow;
    class CodeFilterWindow;
    class TableCleanupWindow;
    class StructImportWindow;
    class CommandPalette;
    class UpdateDialog;
    class PointerScanWindow;
    class ShortcutsWindow;
    class TemplateManager;
    class ScriptForms;
    class UiHost;
    class BreakpointWindow;
    class ThreadListWindow;
    class TraceWindow;
    class MonoWindow;
    class UnrealWindow;
    class ProfilerWindow;
    class LayoutAuditWindow;
    class NopSledWindow;
    class WriteHistoryWindow;
    class CommentListWindow;
    class RecordDebugWindow;
    class FormEditorWindow;
    class ModulesWindow;
    class FlowGraphWindow;
    class OpcodeWindow;

    struct FileFilter
    {
        const wchar_t* Name;
        const wchar_t* Pattern;
    };

    class Workspace
    {
    public:
        Workspace(Engine& engine, Shell& shell);
        ~Workspace();

        Workspace(const Workspace&) = delete;
        Workspace& operator=(const Workspace&) = delete;

        void Draw();
        bool CanRun(Command command);
        void Run(Command command);
        // Every command of the main window and the global ones, as its menus run them (the command palette).
        bool CanRunAnywhere(Command command);
        void RunAnywhere(Command command);
        CommandPalette& Palette() { return *m_palette; }
        UpdateDialog& Updates() { return *m_updates; }
        Engine& Core() { return m_engine; }
        Shell& Window() { return m_shell; }
        UiHost& Ui() { return *m_ui; }
        void RequestRestyle() { m_restyle = true; }

        void OpenMemoryViewer(std::uint64_t address, bool hexView = false, bool newWindow = false);
        void OpenStructure(std::uint64_t address, const std::string& structure = {}, bool newWindow = false);
        std::uint32_t StructureDock(const StructureWindow& opening);
        void FindStaticPointers(std::uint64_t address);
        void OpenStaticPointers();
        void OpenSignatureMaker(std::uint64_t address = 0, bool make = false);
        void CopySignature(std::uint64_t address);
        void OpenSignatureMaker(const std::string& expression);
        void OpenCodeFilter(bool collect = false);
        void OpenTableCleanup();
        // Structures > Import: a file dialog, the clipboard, or (for --open=structimport:<path>) that file.
        void OpenStructImport(bool clipboard, const std::filesystem::path& file = {});
        void OpenPointerScan(std::uint64_t address = 0);
        void OpenScript(std::string script, int recordId = kNoRecord, bool lua = false);
        ScriptWindow* ScriptFile(const std::filesystem::path& path);
        ScriptWindow* OpenScriptFile(const std::filesystem::path& path, int line = 0);
        std::uint32_t ScriptEditorDock(const ScriptWindow& opening);
        // A window by its --open name ("memory", "settings", "templates:<id>", ...); false for an unknown name.
        bool OpenNamed(const std::string& window);
        // Every name OpenNamed knows without an argument (the layout sweep opens them one by one).
        static const std::vector<std::string>& WindowNames();
        void OpenLua();
        void OpenLog();
        void OpenSettings(int page = -1);
        void OpenProcessPicker();
        void OpenAbout(bool credits = false);
        void OpenSymbols();
        void OpenRegions();
        void OpenTableFiles();
        void OpenPatchScan();
        void OpenCodeDissect(int tab = -1, std::uint64_t address = 0);
        void OpenAsmSearch();
        void OpenByteSearch(const std::string& pattern = {});
        void RevealInMemoryViewer(std::uint64_t address, std::uint64_t length, bool code, bool raise);
        void OpenShortcuts();
        void OpenTemplateManager(const std::string& id = {}, std::optional<std::string> script = std::nullopt);
        void OpenRecordEditor(int recordId, RecordField field = RecordField::None);
        void OpenTableDialog(bool merge);
        void OpenLastProcess();
        bool CanOpenLastProcess();
        void StartWatch(Debug::WatchKind kind, std::uint64_t address, int size);
        // Switches the debugger to the in-process (VEH) backend and re-arms every
        // open watch there, so watches that ran into a hidden main thread on the
        // Windows backend can see those accesses. No-op once already on VEH.
        void UseVehForWatches();
        void OpenBreakpoints();
        void OpenBreakpointEditor(std::uint64_t address, Debug::BreakpointType type, int size, unsigned int viewport = 0);
        void OpenThreadList();
        void OpenTrace(std::uint64_t address, unsigned int viewport = 0);
        // With a class name ("Player", "Game.Player" or "Assembly-CSharp:Game.Player")
        // the window opens on that class in its Classes tab.
        void OpenMono(const std::string& className = {});
        void OpenUnreal(const std::string& type = {}, std::uint64_t address = 0);
        void OpenProfiler(int tab = -1);
        void OpenLayoutProblems();
        void OpenNopSled();
        void OpenWriteHistory();
        void OpenCommentList();
        // The Address List Debug window, on that record when one is given.
        void OpenRecordDebug(int recordId = -1);
        // The same window on one of its tabs: records, check, trace or internals; false for another name.
        bool OpenRecordDebugPage(std::string_view page);
        // Selects the record in the address list and scrolls there, opening the groups above it.
        void RevealRecord(int recordId);
        void OpenFormEditor(const std::filesystem::path& path = {});
        void OpenModules();
        void OpenFlowGraph(std::uint64_t address = 0);
        void OpenOpcodeConverter(std::uint64_t address = 0, std::span<const std::uint8_t> bytes = {});
        bool Animating() const;
        bool OnlySpinnersAnimating() const;
        void AddAddress(std::uint64_t address, Scan::ValueType type, std::string description = {});
        void Confirm(std::string title, std::string text, std::function<void()> yes, std::string yesLabel = "Yes");
        void Report(const Status& status);
        void NoProcessState(const char* icon, std::string_view text);

        std::optional<std::filesystem::path> OpenFileDialog(const std::vector<FileFilter>& filters, const std::filesystem::path& folder = {});
        std::optional<std::filesystem::path> SaveFileDialog(const std::vector<FileFilter>& filters, const std::string& name, const wchar_t* extension);
        std::vector<std::filesystem::path> OpenFilesDialog(const std::vector<FileFilter>& filters, const std::filesystem::path& folder = {});
        std::optional<std::filesystem::path> FolderDialog();

        bool OpenTable(const std::filesystem::path& path, bool merge);
        void DropFiles(const std::vector<std::filesystem::path>& files, bool merge, void* window = nullptr, float x = 0.0f, float y = 0.0f);
        bool SaveTable(bool askName);
        void OpenBackupDialog();
        void OpenBackup(const std::filesystem::path& path);
        void OfferBackups(std::vector<Backups::Abandoned> abandoned);
        const std::vector<Backups::Abandoned>& Abandoned() const { return m_abandoned; }
        void BackUpNow();
        void OpenBackupFolder();
        void NewTable();
        // restartFrom: the version an update replaced; the program starts again after the quit
        void RequestQuit(std::string restartFrom = {});
        static std::string WithoutCodes(std::string_view text);
        void TurnOffEverything();
        bool QuitConfirmed() const { return m_quitConfirmed; }

        void CopyRecords(const std::vector<int>& ids);
        void PasteRecords(int parentId);

        std::uint64_t LastCodeAddress = 0;
        int ActiveViewer = 0;

    private:
        void HandleRequests();
        void HandleShortcuts();
        void DrawNotices();
        void DrawConfirm();
        void DrawTableScriptPrompt();
        std::string m_tableScriptShown;
        CodeEditorState m_tableScriptView;
        int m_tableScriptNext = 0;
        void DrawAbout();
        void DrawAboutProgram();
        void DrawCredits();
        void DrawWindows();

        Engine& m_engine;
        Shell& m_shell;
        double m_lastProcessCheckedAt = -100.0;
        std::optional<ProcessId> m_lastProcessId;
        bool m_lastProcessLookup = false;
        void RefreshLastProcess();
        std::unique_ptr<MainWindow> m_main;
        std::vector<std::unique_ptr<MemoryViewer>> m_memoryViewers;
        std::vector<std::unique_ptr<StructureWindow>> m_structures;
        std::vector<std::unique_ptr<ScriptWindow>> m_scripts;
        std::vector<std::unique_ptr<WatchWindow>> m_watches;
        std::vector<std::unique_ptr<RecordEditor>> m_editors;
        std::unique_ptr<LuaWindow> m_lua;
        std::uint64_t m_seenPrints = 0;
        std::unique_ptr<LogWindow> m_log;
        std::unique_ptr<SettingsWindow> m_settings;
        std::unique_ptr<ProcessPicker> m_picker;
        std::unique_ptr<SymbolsWindow> m_symbols;
        std::unique_ptr<RegionsWindow> m_regions;
        std::unique_ptr<TableFilesWindow> m_tableFiles;
        std::unique_ptr<PatchScanWindow> m_patchScan;
        std::unique_ptr<CodeDissectWindow> m_codeDissect;
        std::unique_ptr<AsmSearchWindow> m_asmSearch;
        std::unique_ptr<ByteSearchWindow> m_byteSearch;
        std::unique_ptr<StaticPointersWindow> m_staticPointers;
        std::unique_ptr<SignatureWindow> m_signatures;
        std::unique_ptr<CodeFilterWindow> m_codeFilter;
        std::unique_ptr<TableCleanupWindow> m_tableCleanup;
        std::unique_ptr<StructImportWindow> m_structImport;
        std::unique_ptr<CommandPalette> m_palette;
        std::unique_ptr<UpdateDialog> m_updates;
        std::string m_restartFrom;
        std::unique_ptr<PointerScanWindow> m_pointerScan;
        std::unique_ptr<ShortcutsWindow> m_shortcuts;
        std::unique_ptr<TemplateManager> m_templateManager;
        std::unique_ptr<ScriptForms> m_scriptForms;
        std::unique_ptr<UiHost> m_ui;
        std::unique_ptr<BreakpointWindow> m_breakpoints;
        std::unique_ptr<ThreadListWindow> m_threadList;
        std::unique_ptr<MonoWindow> m_mono;
        std::unique_ptr<UnrealWindow> m_unrealWindow;
        std::unique_ptr<ProfilerWindow> m_profiler;
        std::unique_ptr<LayoutAuditWindow> m_layoutProblems;
        std::unique_ptr<NopSledWindow> m_nopSled;
        std::unique_ptr<WriteHistoryWindow> m_writeHistory;
        std::unique_ptr<CommentListWindow> m_commentList;
        std::unique_ptr<RecordDebugWindow> m_recordDebug;
        std::unique_ptr<FormEditorWindow> m_formEditor;
        std::unique_ptr<ModulesWindow> m_modules;
        std::unique_ptr<FlowGraphWindow> m_flowGraph;
        std::unique_ptr<OpcodeWindow> m_opcodes;
        std::vector<std::unique_ptr<TraceWindow>> m_traces;
        int m_nextTrace = 1;
        MemoryViewer& ActiveMemoryViewer();
        int m_nextWindow = 1;
        std::uint32_t m_scriptDock = 0;
        int m_scriptDockFrame = -1;
        std::uint32_t m_structureDock = 0;
        int m_structureDockFrame = -1;
        std::uint32_t DockGroup(const std::vector<std::string>& others, std::uint32_t& dock, int& frame);

        struct ConfirmRequest
        {
            std::string Title;
            std::string Text;
            std::string YesLabel;
            std::function<void()> Yes;
            unsigned int Viewport = 0;
        };
        std::vector<ConfirmRequest> m_confirms;
        std::vector<Backups::Abandoned> m_abandoned;
        bool m_quitPrompt = false;
        bool m_quitRemember = false;
        bool UnsavedTable();
        void QuitAfterTable();
        void TurnOffAndQuit();
        void FinishQuit();
        void DrawQuitPrompt();
        bool m_aboutOpen = false;
        bool m_aboutCredits = false;    // About shows the credits instead of the program
        bool m_quitConfirmed = false;
        bool m_restyle = false;
        void SecretKeys();
        int m_secretStep = 0;
        int m_versionClicks = 0;
        double m_versionClickedAt = -100.0;
    };

    std::string NextWindowTitle(const char* base, int number);
}
