#pragma once

#include "Core/Errors.h"
#include "Core/MainThread.h"
#include "Table/AddressList.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Manifold
{
    class Engine;

    struct ActivationProgress
    {
        int Done = 0;
        int Total = 0;
        std::string Current;
        bool TableScript = false;
        double TableScriptSeconds = 0.0;
    };

    // One switch of a record on or off, for the activation trace of the Address List Debug window. Kept for every switch
    // (the window may open later), in a ring of kTraceLength entries.
    struct ActivationTraceEntry
    {
        enum class Outcome
        {
            Done,
            Failed,
            Vetoed
        };

        std::uint64_t Serial = 0;
        double At = 0.0;          // Engine::Now when it finished
        double Seconds = 0.0;     // the work itself: assembling and writing, or reading the value to freeze
        double Waited = 0.0;      // time in the queue before it started (asynchronous mode only)
        int RecordId = 0;
        std::string Description;
        Table::RecordKind Kind = Table::RecordKind::Value;
        bool Active = false;
        Outcome Result = Outcome::Done;
        std::string Error;
        int ErrorLine = 0;
        bool Async = false;
        bool FromScript = false;
        int ViaParent = -1;       // the record that switched it together with its children; -1 when it was switched itself
        std::size_t WriteCount = 0;
        std::size_t BytesWritten = 0;
        std::vector<Asm::AaWrite> Writes;   // the first kTraceWrites of them
        std::vector<Asm::AaAllocation> Allocated;
        std::vector<Asm::AaAllocation> Released;
        std::vector<std::string> Registered;
        std::vector<std::string> Unregistered;
        std::vector<std::string> Notes;
    };

    struct QueuedActivation
    {
        int RecordId = 0;
        bool Active = false;
        bool FromScript = false;
        bool Running = false;
        int ViaParent = -1;
        double QueuedAt = 0.0;
    };

    class ActivationRunner
    {
    public:
        explicit ActivationRunner(Engine& engine);
        ~ActivationRunner();

        ActivationRunner(const ActivationRunner&) = delete;
        ActivationRunner& operator=(const ActivationRunner&) = delete;

        // fromScript marks a switch Lua asked for, for the activation trace.
        Status SetActive(Table::MemoryRecord& record, bool active, bool fromScript = false);
        void RequestActive(Table::MemoryRecord& record, bool active, bool fromScript = false);
        bool ActivationPending(int recordId) const { return m_pending.count(recordId) > 0; }
        bool InsideActivation() const { return m_running != nullptr && m_workerLuaActive; }
        void DropQueued(int recordId);
        void CancelActivations();
        ActivationProgress Progress() const;
        bool Working() const;
        void SetAsync(bool async) { m_async = async; }
        bool Async() const { return m_async; }
        static bool OnActivationWorker();
        void RunOnMainAndWait(const std::function<void()>& work);
        void StopTableScript();
        std::vector<Table::MemoryRecord*> ActiveScripts();
        std::vector<std::string> DeactivateAll();
        void RequestDeactivateAll();

        void RunTableScript(const std::string& script);
        void WaitForActivation();
        void DrainMainWork();
        void PumpActivations(double budgetSeconds);
        std::vector<int> BottomToTop();

        static constexpr std::size_t kTraceLength = 500;
        static constexpr std::size_t kTraceWrites = 64;
        const std::deque<ActivationTraceEntry>& Trace() const { return m_trace; }
        std::uint64_t TraceSerial() const { return m_traceSerial; }
        void ClearTrace() { m_trace.clear(); }
        // The jobs waiting in asynchronous mode, the running one first.
        std::vector<QueuedActivation> Queue() const;

    private:
        struct ActivationJob
        {
            int RecordId = 0;
            bool Active = false;
            bool FromScript = false;
            bool ChildrenDone = false;
            int ViaParent = -1;
            double QueuedAt = 0.0;
        };
        struct RunningActivation;

        Status ActivateTree(Table::MemoryRecord& record, bool active, int viaParent = -1, bool fromScript = false);
        Status ActivatePlain(Table::MemoryRecord& record, bool active);
        void FinishRecordActivation(Table::MemoryRecord& record, bool active, bool fromScript);
        void StartActivation(const ActivationJob& job);
        void FinishActivation();
        void Unpend(int recordId);
        void QueueChildren(Table::MemoryRecord& record, bool active, bool fromScript);
        ActivationTraceEntry TraceStart(const Table::MemoryRecord& record, bool active, bool fromScript, int viaParent) const;
        void TraceScript(ActivationTraceEntry& entry, const Asm::AaResult& result, const Asm::AaMemory& before, const Asm::AaMemory& after) const;
        void Traced(ActivationTraceEntry entry);
        void RunRecordLua(const std::string& code, const char* name);
        Status RunLuaForWorker(std::string_view code, int recordId, bool syntaxCheck, std::string& output);

        Engine& m_engine;
        bool m_async = false;
        std::deque<ActivationJob> m_activations;
        std::unordered_map<int, int> m_pending;
        std::unique_ptr<RunningActivation> m_running;
        int m_progressDone = 0;
        int m_progressTotal = 0;
        std::string m_progressCurrent;
        MainThreadQueue m_mainQueue;
        bool m_waitingForWorker = false;
        bool m_workerLuaActive = false;
        int m_tableTask = 0;
        bool m_tableTaskStopped = false;
        double m_tableTaskStarted = 0.0;
        std::deque<ActivationTraceEntry> m_trace;
        std::uint64_t m_traceSerial = 0;
    };
}
