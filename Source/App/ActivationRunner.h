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
#include <unordered_set>
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
        std::vector<Asm::AaWrite> Writes;   // the first kTraceWrites of them, each cut to its first kTraceWriteBytes bytes
        std::vector<std::size_t> WriteSizes; // the full size of each of Writes
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

        // fromScript marks a switch Lua asked for, for the activation trace. A record that is being switched right now (its own
        // handler or {$lua} asks again) is queued after that switch in asynchronous mode and refused otherwise.
        Status SetActive(Table::MemoryRecord& record, bool active, bool fromScript = false);
        void RequestActive(Table::MemoryRecord& record, bool active, bool fromScript = false);
        bool ActivationPending(int recordId) const { return m_pending.count(recordId) > 0; }
        // Whether the record is in the middle of a switch: its handlers, script or children run right now.
        bool Switching(int recordId) const;
        // How the record's last switch in this table ended: Success, the script's error, the veto of a handler or the refusal of
        // a switch during its own switch. Success when none is known.
        Status LastOutcome(int recordId) const;
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
        static constexpr std::size_t kTraceWriteBytes = 32;
        const std::deque<ActivationTraceEntry>& Trace() const { return m_trace; }
        std::uint64_t TraceSerial() const { return m_traceSerial; }
        void ClearTrace() { m_trace.clear(); }
        // The jobs waiting in asynchronous mode, the running one first.
        std::vector<QueuedActivation> Queue() const;

    private:
        struct ActivationJob
        {
            int RecordId = 0;
            std::uint64_t Identity = 0;   // the table it was queued for; a replaced table may reuse the id
            bool Active = false;
            bool FromScript = false;
            bool ChildrenDone = false;
            int ViaParent = -1;
            double QueuedAt = 0.0;
        };
        struct RunningActivation;

        // ∑ Everything that can run Lua or wait (handlers, {$lua}, OnActivate, children) may delete records or replace the
        //   table, so a switch carries the record's id and the table's identity across it and finds the record again after.
        Status ActivateTree(int recordId, bool active, int viaParent = -1, bool fromScript = false);
        Status SwitchTree(int recordId, std::uint64_t identity, bool active, int viaParent, bool fromScript);
        // A value or group switch; record is found again after the read and
        // is nullptr (a TableRecordGone status) when Lua deleted it meanwhile.
        Status ActivatePlain(Table::MemoryRecord*& record, int recordId, std::uint64_t identity, bool active);
        void FinishRecordActivation(int recordId, std::uint64_t identity, bool active, bool fromScript);
        Table::MemoryRecord* Refind(int recordId, std::uint64_t identity);
        Status Vanished(int recordId, std::uint64_t identity, const std::string& description, const char* during);
        // A script whose record went away while it was switched: its [DISABLE] part runs when its changes are in the game, and
        // the symbols it left are unregistered; what still stays is logged.
        void ReleaseOrphan(int recordId, const std::string& description, const std::string& script, Asm::AaMemory memory, bool on,
            const std::shared_ptr<Target::Process>& process);
        void NoteOutcome(int recordId, const Status& status);
        void StartActivation(const ActivationJob& job);
        void FinishActivation();
        void Unpend(int recordId);
        void QueueChildren(Table::MemoryRecord& record, bool active, bool fromScript);
        ActivationTraceEntry TraceStart(const Table::MemoryRecord& record, bool active, bool fromScript, int viaParent) const;
        void TraceScript(ActivationTraceEntry& entry, const Asm::AaResult& result, const Asm::AaMemory& before, const Asm::AaMemory& after) const;
        void Traced(ActivationTraceEntry entry);
        void RunRecordLua(const std::string& code, const char* name);
        Status RunLuaForWorker(std::string_view code, int recordId, std::uint64_t identity, bool syntaxCheck, std::string& output);

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
        std::unordered_set<int> m_switching;
        std::unordered_map<int, Status> m_outcomes;
        std::uint64_t m_outcomesIdentity = 0;
    };
}
