#pragma once

#include "App/Settings.h"
#include "Asm/AutoAssembler.h"
#include "Core/Errors.h"
#include "Core/RuntimeLog.h"
#include "Debug/Debugger.h"
#include "Disasm/CodeDissect.h"
#include "Disasm/Disassembler.h"
#include "Scan/Scanner.h"
#include "Table/AddressList.h"
#include "Table/CheatTable.h"
#include "Table/Structures.h"
#include "Target/Process.h"
#include "Target/PdbSymbols.h"
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
    struct Notice
    {
        LogLevel Level = LogLevel::Info;
        std::string Text;
        double At = 0.0;
        int Id = 0;
    };

    enum class UiRequestKind
    {
        MemoryViewer,
        MemoryViewerSelect,
        MemoryViewerHex,
        MemoryViewerRaise,
        StructureDissect,
        AutoAssembler,
        LuaEngine,
        Quit
    };

    struct ActivationProgress
    {
        int Done = 0;
        int Total = 0;
        std::string Current;
        bool TableScript = false;
        double TableScriptSeconds = 0.0;
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
        double Now() const;

        Status OpenProcess(std::uint32_t id);
        Status OpenProcessByName(std::string_view name);
        void CloseProcess();
        const std::shared_ptr<Target::Process>& Process() const { return m_process; }
        bool HasProcess() const { return m_process != nullptr; }
        std::uint64_t ProcessGeneration() const { return m_processGeneration; }
        Target::Symbols& Symbols() { return m_symbols; }
        void LoadSymbols(bool windowsSymbols);
        bool SymbolsActive() const { return m_pdbSymbols.Active(); }
        bool SymbolsLoaded() const { return m_pdbSymbols.Loaded(); }
        bool WaitForSymbols(unsigned timeoutMs) { return m_pdbSymbols.WaitUntilLoaded(timeoutMs); }
        int SymbolsModulesDone() const { return m_pdbSymbols.ModulesDone(); }
        int SymbolsModulesTotal() const { return m_pdbSymbols.ModulesTotal(); }
        const Disasm::Disassembler& Disassembler() const;
        std::shared_ptr<const Disasm::DissectResult> CodeDissect() const { return m_codeDissect; }
        void SetCodeDissect(std::shared_ptr<const Disasm::DissectResult> result) { m_codeDissect = std::move(result); }
        Scan::Scanner& Scanner() { return m_scanner; }
        Debug::Debugger& Debugger() { return m_debugger; }
        Status EnsureDebugger();
        Table::AddressList& Table() { return m_table; }
        Table::StructureStore& Structures() { return m_structures; }
        Lua::LuaEngine& Lua() { return *m_lua; }

        std::optional<std::uint64_t> Evaluate(std::string_view expression, Status* error = nullptr) const;
        std::string Describe(std::uint64_t address) const { return m_symbols.Describe(address); }

        void ResolveRecord(Table::MemoryRecord& record);
        std::vector<std::uint8_t> ReadRecord(Table::MemoryRecord& record);
        std::string FormatRecord(const Table::MemoryRecord& record, std::span<const std::uint8_t> bytes) const;
        const std::vector<Table::DropDownEntry>& DropDownOf(const Table::MemoryRecord& record) const;
        void ApplyHotkey(Table::MemoryRecord& record, const Table::Hotkey& hotkey);
        Status WriteRecord(Table::MemoryRecord& record, std::string_view text, bool recursive = true);
        Status AddToRecord(Table::MemoryRecord& record, double delta);
        Status SetActive(Table::MemoryRecord& record, bool active);
        void RequestActive(Table::MemoryRecord& record, bool active, bool fromScript = false);
        bool ActivationPending(int recordId) const { return m_pending.count(recordId) > 0; }
        bool InsideActivation() const { return m_running != nullptr && m_workerLuaActive; }
        void DropQueued(int recordId);
        void CancelActivations();
        ActivationProgress Progress() const;
        bool Working() const;
        void SetAsync(bool async) { m_async = async; }
        bool Async() const { return m_async; }
        bool OnMainThread() const { return std::this_thread::get_id() == m_mainThread; }
        static bool OnActivationWorker();
        void RunOnMainAndWait(const std::function<void()>& work);
        void StopTableScript();
        std::vector<Table::MemoryRecord*> ActiveScripts();
        std::vector<std::string> DeactivateAll();
        void RequestDeactivateAll();
        void RefreshValues(bool force);

        Asm::AaResult AutoAssemble(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool checkOnly, const std::string& owner, const Table::MemoryRecord* record = nullptr);
        Asm::AaResult AutoAssemble(std::string_view script, bool checkOnly);

        Status LoadTable(const std::filesystem::path& path, bool merge);
        Status SaveTable(const std::filesystem::path& path);
        void NewTable();
        std::vector<Table::TableSymbol> TableSymbols() const;
        bool TableScriptWaiting() const { return m_tableScriptWaiting; }
        void AnswerTableScript(bool run);

        void Notify(LogLevel level, std::string text);
        void Notify(const Status& status, LogLevel level = LogLevel::Error);
        std::deque<Notice>& Notices() { return m_notices; }

        void Request(UiRequest request) { m_requests.push_back(std::move(request)); }
        std::vector<UiRequest> TakeRequests();

        void Tick();

        Asm::AaMemory& ScratchMemory() { return m_scratchMemory; }

        std::uint64_t SelectedCodeAddress = 0;
        std::uint64_t SelectedHexAddress = 0;
        bool MemoryViewerVisible = false;

    private:
        void UpdateFreezes();
        void UpdateHotkeys();
        void UpdateAutoAttach();
        Status ActivateTree(Table::MemoryRecord& record, bool active);
        Status ActivatePlain(Table::MemoryRecord& record, bool active);
        void FinishRecordActivation(Table::MemoryRecord& record, bool active, bool fromScript);
        void PumpActivations(double budgetSeconds);
        struct ActivationJob;
        void StartActivation(const ActivationJob& job);
        std::vector<int> BottomToTop();
        void FinishActivation();
        void WaitForActivation();
        void DrainMainWork();
        void Unpend(int recordId);
        void QueueChildren(Table::MemoryRecord& record, bool active, bool fromScript);
        void RunRecordLua(const std::string& code, const char* name);
        void RunTableScript(const std::string& script);
        Status RunLuaForWorker(std::string_view code, int recordId, bool syntaxCheck, std::string& output);

        struct ActivationJob
        {
            int RecordId = 0;
            bool Active = false;
            bool FromScript = false;
            bool ChildrenDone = false;
        };
        struct RunningActivation;

        Settings& m_settings;
        std::shared_ptr<Target::Process> m_process;
        std::uint64_t m_processGeneration = 0;
        Target::Symbols m_symbols;
        Target::PdbSymbols m_pdbSymbols;
        std::unique_ptr<Disasm::Disassembler> m_disassembler64;
        std::unique_ptr<Disasm::Disassembler> m_disassembler32;
        Scan::Scanner m_scanner;
        Debug::Debugger m_debugger;
        Table::AddressList m_table;
        Table::StructureStore m_structures;
        std::unique_ptr<Lua::LuaEngine> m_lua;
        Asm::AaMemory m_scratchMemory;
        std::shared_ptr<const Disasm::DissectResult> m_codeDissect;

        std::deque<Notice> m_notices;
        int m_nextNotice = 1;
        std::vector<UiRequest> m_requests;
        double m_lastValues = -1.0;
        double m_lastFreeze = -1.0;
        double m_lastAttachCheck = -1.0;
        double m_lastAliveCheck = -1.0;
        std::uint64_t m_lastBreakSerial = 0;
        bool m_tableScriptWaiting = false;
        std::string m_pendingTableScript;
        std::vector<Table::TableSymbol> m_tableSymbols;

        bool m_async = false;
        std::thread::id m_mainThread = std::this_thread::get_id();
        std::deque<ActivationJob> m_activations;
        std::unordered_map<int, int> m_pending;
        std::unique_ptr<RunningActivation> m_running;
        int m_progressDone = 0;
        int m_progressTotal = 0;
        std::string m_progressCurrent;
        std::mutex m_mainLock;
        std::deque<std::function<void()>> m_mainWork;
        bool m_waitingForWorker = false;
        bool m_workerLuaActive = false;
        int m_tableTask = 0;
        bool m_tableTaskStopped = false;
        double m_tableTaskStarted = 0.0;
    };
}
