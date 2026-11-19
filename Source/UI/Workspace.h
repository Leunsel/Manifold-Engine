#pragma once

#include "App/Commands.h"
#include "App/Engine.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
    class SymbolsWindow;
    class RegionsWindow;
    class TableFilesWindow;
    class PatchScanWindow;
    class CodeDissectWindow;
    class AsmSearchWindow;
    class ByteSearchWindow;
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
    class NopSledWindow;
    class WriteHistoryWindow;
    class FlowGraphWindow;

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
        Engine& Core() { return m_engine; }
        Shell& Window() { return m_shell; }
        UiHost& Ui() { return *m_ui; }
        void RequestRestyle() { m_restyle = true; }

        void OpenMemoryViewer(std::uint64_t address, bool hexView = false, bool newWindow = false);
        void OpenStructure(std::uint64_t address, const std::string& structure = {});
        void OpenScript(std::string script, int recordId = kNoRecord, bool lua = false);
        void OpenLua();
        void OpenLog();
        void OpenSettings(int page = -1);
        void OpenProcessPicker();
        void OpenAbout();
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
        void OpenRecordEditor(int recordId);
        void OpenTableDialog(bool merge);
        void OpenLastProcess();
        bool CanOpenLastProcess();
        void StartWatch(Debug::WatchKind kind, std::uint64_t address, int size);
        void OpenBreakpoints();
        void OpenBreakpointEditor(std::uint64_t address, Debug::BreakpointType type, int size, unsigned int viewport = 0);
        void OpenThreadList();
        void OpenTrace(std::uint64_t address, unsigned int viewport = 0);
        // With a class name ("Player", "Game.Player" or "Assembly-CSharp:Game.Player")
        // the window opens on that class in its Classes tab.
        void OpenMono(const std::string& className = {});
        void OpenUnreal(const std::string& type = {}, std::uint64_t address = 0);
        void OpenProfiler(int tab = -1);
        void OpenNopSled();
        void OpenWriteHistory();
        void OpenFlowGraph(std::uint64_t address = 0);
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
        bool SaveTable(bool askName);
        void NewTable();
        void RequestQuit();
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
        void DrawAbout();
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
        std::unique_ptr<ShortcutsWindow> m_shortcuts;
        std::unique_ptr<TemplateManager> m_templateManager;
        std::unique_ptr<ScriptForms> m_scriptForms;
        std::unique_ptr<UiHost> m_ui;
        std::unique_ptr<BreakpointWindow> m_breakpoints;
        std::unique_ptr<ThreadListWindow> m_threadList;
        std::unique_ptr<MonoWindow> m_mono;
        std::unique_ptr<UnrealWindow> m_unrealWindow;
        std::unique_ptr<ProfilerWindow> m_profiler;
        std::unique_ptr<NopSledWindow> m_nopSled;
        std::unique_ptr<WriteHistoryWindow> m_writeHistory;
        std::unique_ptr<FlowGraphWindow> m_flowGraph;
        std::vector<std::unique_ptr<TraceWindow>> m_traces;
        int m_nextTrace = 1;
        MemoryViewer& ActiveMemoryViewer();
        int m_nextWindow = 1;

        struct ConfirmRequest
        {
            std::string Title;
            std::string Text;
            std::string YesLabel;
            std::function<void()> Yes;
            unsigned int Viewport = 0;
        };
        std::vector<ConfirmRequest> m_confirms;
        bool m_quitPrompt = false;
        bool m_quitRemember = false;
        void QuitAfterTable();
        void TurnOffAndQuit();
        void FinishQuit();
        void DrawQuitPrompt();
        bool m_aboutOpen = false;
        bool m_quitConfirmed = false;
        bool m_restyle = false;
        void SecretKeys();
        int m_secretStep = 0;
        int m_versionClicks = 0;
        double m_versionClickedAt = -100.0;
    };

    std::string NextWindowTitle(const char* base, int number);
}
