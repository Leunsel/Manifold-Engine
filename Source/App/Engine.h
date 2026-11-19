#pragma once

#include "App/ActivationRunner.h"
#include "App/ManagedSession.h"
#include "App/Notifications.h"
#include "App/Settings.h"
#include "App/TableRuntime.h"
#include "App/TargetSession.h"
#include "App/UnrealSession.h"
#include "App/WriteHistory.h"
#include "App/SettingsStore.h"
#include "Asm/AutoAssembler.h"
#include "Core/BackgroundWork.h"
#include "Core/Errors.h"
#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"
#include "Debug/Debugger.h"
#include "Debug/Trace.h"
#include "Disasm/CodeDissect.h"
#include "Disasm/Disassembler.h"
#include "Disasm/FunctionBounds.h"
#include "Scan/Scanner.h"
#include "Table/AddressList.h"
#include "Table/CheatTable.h"
#include "Table/Structures.h"
#include "Target/CodeMap.h"
#include "Target/Process.h"
#include "Target/PdbSymbols.h"
#include "Target/Mono.h"
#include "Target/Symbols.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Lua
{
    class LuaEngine;
}

namespace Manifold
{
    enum class UiRequestKind
    {
        MemoryViewer,
        MemoryViewerSelect,
        MemoryViewerHex,
        MemoryViewerRaise,
        StructureDissect,
        AutoAssembler,
        LuaEngine,
        Mono,
        Quit
    };

    struct UiRequest
    {
        UiRequestKind Kind = UiRequestKind::MemoryViewer;
        std::uint64_t Address = 0;
        std::string Text;
        std::uint64_t Start = 0;
        std::uint64_t Stop = 0;
    };

    class Engine
    {
    public:
        explicit Engine(Settings& settings);
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        Settings& Options() { return m_settings; }
        void SaveOptionsSoon();
        Status SaveOptionsNow();
        const SettingsStore& OptionsStore() const { return m_settingsStore; }
        BackgroundWork& Work() { return m_work; }
        std::vector<WorkInfo> BusyWork() const;
        void CancelWork(int id);
        double Now() const;

        Status OpenProcess(ProcessId id);
        Status OpenProcessByName(std::string_view name);
        void CloseProcess();
        const std::shared_ptr<Target::Process>& Process() const { return m_target.Process(); }
        bool HasProcess() const { return m_target.Attached(); }
        std::uint64_t ProcessGeneration() const { return m_target.Generation(); }
        Target::Symbols& Symbols() { return m_target.Symbols(); }
        TargetSession& Session() { return m_target; }
        void LoadSymbols(bool windowsSymbols);
        bool SymbolsActive() const { return m_target.Pdb().Active(); }
        bool SymbolsLoaded() const { return m_target.Pdb().Loaded(); }
        bool WaitForSymbols(unsigned timeoutMs) { return m_target.Pdb().WaitUntilLoaded(timeoutMs); }
        int SymbolsModulesDone() const { return m_target.Pdb().ModulesDone(); }
        int SymbolsModulesTotal() const { return m_target.Pdb().ModulesTotal(); }
        const Disasm::Disassembler& Disassembler() const;
        // Known function starts and ends for the disassembler (unwind tables,
        // Mono methods, allocation starts); null without a process.
        const Target::CodeMap* CodeMap() const { return m_target.Attached() ? &m_target.CodeMap() : nullptr; }
        std::shared_ptr<const Disasm::DissectResult> CodeDissect() const { return m_codeDissect; }
        void SetCodeDissect(std::shared_ptr<const Disasm::DissectResult> result) { m_codeDissect = std::move(result); }
        std::optional<Disasm::FunctionSpan> FunctionAround(std::uint64_t address);
        std::uint64_t SymbolEnd(std::uint64_t start);
        bool FunctionStartAt(std::uint64_t address);
        Scan::Scanner& Scanner() { return m_scanner; }
        Debug::Debugger& Debugger() { return m_debugger; }
        Debug::Tracer& Tracer() { return m_tracer; }
        bool Tracing() const { return m_tracer.Running(); }
        Status EnsureDebugger();
        ManagedSession& Managed() { return m_managed; }
        const ManagedSession& Managed() const { return m_managed; }
        UnrealSession& Unreal() { return m_unreal; }
        const UnrealSession& Unreal() const { return m_unreal; }
        WriteHistory& Writes() { return m_writes; }
        Target::MonoBridge& Mono() { return m_target.Mono(); }
        bool MonoPresent() const { return m_target.Mono().Present(); }
        void LookUpProcessLater(std::string name, std::function<void(std::optional<ProcessId>)> found);
        static constexpr int kSymbolsWork = -100;
        Table::AddressList& Table() { return m_tables.Table(); }
        Table::StructureStore& Structures() { return m_tables.Structures(); }
        TableRuntime& Tables() { return m_tables; }
        ActivationRunner& Activation() { return m_activation; }
        Lua::LuaEngine& Lua() { return *m_lua; }

        std::optional<std::uint64_t> Evaluate(std::string_view expression, Status* error = nullptr) const;
        std::string Describe(std::uint64_t address) const { return m_target.Symbols().Describe(address); }

        void ResolveRecord(Table::MemoryRecord& record) { m_tables.ResolveRecord(record); }
        std::vector<std::uint8_t> ReadRecord(Table::MemoryRecord& record) { return m_tables.ReadRecord(record); }
        std::string FormatRecord(const Table::MemoryRecord& record, std::span<const std::uint8_t> bytes) const { return m_tables.FormatRecord(record, bytes); }
        const std::vector<Table::DropDownEntry>& DropDownOf(const Table::MemoryRecord& record) const { return m_tables.DropDownOf(record); }
        void ApplyHotkey(Table::MemoryRecord& record, const Table::Hotkey& hotkey) { m_tables.ApplyHotkey(record, hotkey); }
        Status WriteRecord(Table::MemoryRecord& record, std::string_view text, bool recursive = true) { return m_tables.WriteRecord(record, text, recursive); }
        Status AddToRecord(Table::MemoryRecord& record, double delta) { return m_tables.AddToRecord(record, delta); }
        Status SetActive(Table::MemoryRecord& record, bool active) { return m_activation.SetActive(record, active); }
        void RequestActive(Table::MemoryRecord& record, bool active, bool fromScript = false) { m_activation.RequestActive(record, active, fromScript); }
        bool ActivationPending(int recordId) const { return m_activation.ActivationPending(recordId); }
        bool InsideActivation() const { return m_activation.InsideActivation(); }
        void DropQueued(int recordId) { m_activation.DropQueued(recordId); }
        void CancelActivations() { m_activation.CancelActivations(); }
        ActivationProgress Progress() const { return m_activation.Progress(); }
        bool Working() const { return m_activation.Working(); }
        void SetAsync(bool async) { m_activation.SetAsync(async); }
        bool Async() const { return m_activation.Async(); }
        static bool OnActivationWorker() { return ActivationRunner::OnActivationWorker(); }
        void RunOnMainAndWait(const std::function<void()>& work) { m_activation.RunOnMainAndWait(work); }
        void StopTableScript() { m_activation.StopTableScript(); }
        std::vector<Table::MemoryRecord*> ActiveScripts() { return m_activation.ActiveScripts(); }
        std::vector<std::string> DeactivateAll() { return m_activation.DeactivateAll(); }
        void RequestDeactivateAll() { m_activation.RequestDeactivateAll(); }
        void RefreshValues(bool force) { m_tables.RefreshValues(force); }
        void RefreshRecord(int id) { m_tables.RefreshRecord(id); }
        bool NeedsValue(const Table::MemoryRecord& record) const { return m_tables.NeedsValue(record); }
        void SetVisibleRecords(std::vector<int> ids) { m_tables.SetVisibleRecords(std::move(ids)); }

        Asm::AaResult AutoAssemble(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool checkOnly, const std::string& owner, const Table::MemoryRecord* record = nullptr);
        Asm::AaResult AutoAssemble(std::string_view script, bool checkOnly);

        Status LoadTable(const std::filesystem::path& path, bool merge) { return m_tables.LoadTable(path, merge); }
        Status SaveTable(const std::filesystem::path& path) { return m_tables.SaveTable(path); }
        void NewTable() { m_tables.NewTable(); }
        std::vector<Table::TableSymbol> TableSymbols() const { return m_tables.TableSymbols(); }
        bool TableScriptWaiting() const { return m_tables.TableScriptWaiting(); }
        void AnswerTableScript(bool run) { m_tables.AnswerTableScript(run); }

        void Notify(LogLevel level, std::string text);
        void Notify(const Status& status, LogLevel level = LogLevel::Error);
        NotificationCenter& Notifications() { return m_notifications; }

        void Request(UiRequest request) { m_requests.push_back(std::move(request)); }
        std::vector<UiRequest> TakeRequests();

        void Tick();

        Asm::AaMemory& ScratchMemory() { return m_scratchMemory; }

        std::uint64_t SelectedCodeAddress = 0;
        std::uint64_t SelectedHexAddress = 0;
        bool MemoryViewerVisible = false;

    private:
        void UpdateAutoAttach();
        Settings& m_settings;
        SettingsStore m_settingsStore;
        TargetSession m_target;
        std::unique_ptr<Disasm::Disassembler> m_disassembler64;
        std::unique_ptr<Disasm::Disassembler> m_disassembler32;
        Scan::Scanner m_scanner;
        Debug::Debugger m_debugger;
        Debug::Tracer m_tracer{ m_debugger };
        TableRuntime m_tables{ *this };
        std::unique_ptr<Lua::LuaEngine> m_lua;
        Asm::AaMemory m_scratchMemory;
        std::shared_ptr<const Disasm::DissectResult> m_codeDissect;

        NotificationCenter m_notifications;
        std::vector<UiRequest> m_requests;
        double m_lastAttachCheck = -1.0;
        double m_lastAliveCheck = -1.0;
        std::uint64_t m_lastBreakSerial = 0;

        ActivationRunner m_activation{ *this };
        bool m_attachLookup = false;
        BackgroundWork m_work;
        WriteHistory m_writes;
        ManagedSession m_managed{ m_target, m_work, m_settings, [this](LogLevel level, std::string text) { Notify(level, std::move(text)); } };
        UnrealSession m_unreal{ m_target, m_work, [this](LogLevel level, std::string text) { Notify(level, std::move(text)); } };
    };
}
