#include "App/ActivationRunner.h"

#include "App/Engine.h"
#include "App/Sounds.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>

namespace Manifold
{
    namespace
    {
        thread_local bool t_activationWorker = false;

        std::string OwnerOf(int recordId)
        {
            return "record:" + std::to_string(recordId);
        }

        std::vector<int> ChildIds(const Table::MemoryRecord& record, bool lastFirst)
        {
            std::vector<int> ids;
            ids.reserve(record.Children.size());
            for (const auto& child : record.Children)
                ids.push_back(child->Id);
            if (lastFirst)
                std::reverse(ids.begin(), ids.end());
            return ids;
        }

        // Marks a record as being switched for as long as it lives; a mark that was there already is left to its owner.
        class SwitchingMark
        {
        public:
            SwitchingMark(std::unordered_set<int>& switching, int recordId) : m_switching(switching), m_id(recordId), m_marked(switching.insert(recordId).second) {}
            ~SwitchingMark()
            {
                if (m_marked)
                    m_switching.erase(m_id);
            }
            SwitchingMark(const SwitchingMark&) = delete;
            SwitchingMark& operator=(const SwitchingMark&) = delete;

        private:
            std::unordered_set<int>& m_switching;
            int m_id;
            bool m_marked;
        };

        Status Vetoed(const Table::MemoryRecord& record, bool active)
        {
            return Status::Fail(ErrorCode::RecordSwitchRefused, "a handler (onMemRecPreExecute or the record's " + std::string(active ? "onactivate" : "ondeactivate") +
                ") kept \"" + record.Description + "\" from switching " + (active ? "on" : "off"));
        }
    }

    struct ActivationRunner::RunningActivation
    {
        int RecordId = 0;
        std::uint64_t Identity = 0;   // the table the record belongs to; a replaced table may reuse the id
        std::string Description;
        std::string Script;
        bool Active = false;
        bool FromScript = false;
        Asm::AaMemory Memory;
        Asm::AaMemory Before;
        double StartedAt = 0.0;
        double QueuedAt = 0.0;
        int ViaParent = -1;
        std::shared_ptr<Target::Process> Process;
        std::future<Asm::AaResult> Result;
        std::atomic<bool> Finished{ false };
    };

    ActivationRunner::ActivationRunner(Engine& engine) : m_engine(engine)
    {
    }

    ActivationRunner::~ActivationRunner() = default;

    ActivationTraceEntry ActivationRunner::TraceStart(const Table::MemoryRecord& record, bool active, bool fromScript, int viaParent) const
    {
        ActivationTraceEntry entry;
        entry.RecordId = record.Id;
        entry.Description = record.Description;
        entry.Kind = record.Kind;
        entry.Active = active;
        entry.Async = m_async;
        entry.FromScript = fromScript;
        entry.ViaParent = viaParent;
        return entry;
    }

    void ActivationRunner::TraceScript(ActivationTraceEntry& entry, const Asm::AaResult& result, const Asm::AaMemory& before, const Asm::AaMemory& after) const
    {
        if (!result.Ok())
        {
            entry.Result = ActivationTraceEntry::Outcome::Failed;
            entry.Error = result.Error.Text;
            entry.ErrorLine = result.Line;
        }
        entry.WriteCount = result.Writes.size();
        for (const Asm::AaWrite& write : result.Writes)
        {
            entry.BytesWritten += write.Bytes.size();
            if (entry.Writes.size() >= kTraceWrites)
                continue;
            // ∑ a prefix is enough to recognise a write; whole code caves in 500 entries added up to megabytes
            Asm::AaWrite kept;
            kept.Address = write.Address;
            kept.Line = write.Line;
            kept.Bytes.assign(write.Bytes.begin(), write.Bytes.begin() + static_cast<std::ptrdiff_t>(std::min(write.Bytes.size(), kTraceWriteBytes)));
            entry.Writes.push_back(std::move(kept));
            entry.WriteSizes.push_back(write.Bytes.size());
        }
        const auto sameAllocation = [](const Asm::AaAllocation& a, const Asm::AaAllocation& b) { return a.Name == b.Name && a.Address == b.Address; };
        for (const Asm::AaAllocation& allocation : after.Allocations)
        {
            if (std::none_of(before.Allocations.begin(), before.Allocations.end(), [&](const Asm::AaAllocation& old) { return sameAllocation(old, allocation); }))
                entry.Allocated.push_back(allocation);
        }
        for (const Asm::AaAllocation& allocation : before.Allocations)
        {
            if (std::none_of(after.Allocations.begin(), after.Allocations.end(), [&](const Asm::AaAllocation& now) { return sameAllocation(now, allocation); }))
                entry.Released.push_back(allocation);
        }
        for (const std::string& name : after.Registered)
        {
            if (std::find(before.Registered.begin(), before.Registered.end(), name) == before.Registered.end())
                entry.Registered.push_back(name);
        }
        for (const std::string& name : before.Registered)
        {
            if (std::find(after.Registered.begin(), after.Registered.end(), name) == after.Registered.end())
                entry.Unregistered.push_back(name);
        }
        entry.Notes = result.Notes;
        entry.Steps = result.Log;
    }

    void ActivationRunner::Traced(ActivationTraceEntry entry)
    {
        entry.Serial = ++m_traceSerial;
        entry.At = m_engine.Now();
        m_trace.push_back(std::move(entry));
        while (m_trace.size() > kTraceLength)
            m_trace.pop_front();
    }

    std::vector<QueuedActivation> ActivationRunner::Queue() const
    {
        std::vector<QueuedActivation> queue;
        if (m_running)
            queue.push_back({ m_running->RecordId, m_running->Active, m_running->FromScript, true, m_running->ViaParent, m_running->QueuedAt });
        for (const ActivationJob& job : m_activations)
            queue.push_back({ job.RecordId, job.Active, job.FromScript, false, job.ViaParent, job.QueuedAt });
        return queue;
    }

    bool ActivationRunner::Switching(int recordId) const
    {
        return m_switching.count(recordId) > 0 || (m_running && m_running->RecordId == recordId && m_running->Identity == m_engine.Table().Identity());
    }

    Status ActivationRunner::LastOutcome(int recordId) const
    {
        if (m_outcomesIdentity != m_engine.Table().Identity())
            return Status::Success();
        const auto found = m_outcomes.find(recordId);
        return found == m_outcomes.end() ? Status::Success() : found->second;
    }

    void ActivationRunner::NoteOutcome(int recordId, const Status& status)
    {
        const std::uint64_t identity = m_engine.Table().Identity();
        if (m_outcomesIdentity != identity)
        {
            m_outcomes.clear();
            m_outcomesIdentity = identity;
        }
        m_outcomes[recordId] = status;
    }

    Table::MemoryRecord* ActivationRunner::Refind(int recordId, std::uint64_t identity)
    {
        if (m_engine.Table().Identity() != identity)
            return nullptr;
        return m_engine.Table().FindById(recordId);
    }

    Status ActivationRunner::Vanished(int recordId, std::uint64_t identity, const std::string& description, const char* during)
    {
        const bool replaced = m_engine.Table().Identity() != identity;
        LOG_WARN("Table", Format("\"%s\" (record %d) went away %s (%s); its switch stops there", description.c_str(), recordId, during,
            replaced ? "the table was replaced" : "the record was deleted"));
        return Status::Fail(ErrorCode::TableRecordGone, "\"" + description + "\"");
    }

    void ActivationRunner::ReleaseOrphan(int recordId, const std::string& description, const std::string& script, Asm::AaMemory memory, bool on,
        const std::shared_ptr<Target::Process>& process)
    {
        if (!process || process != m_engine.Process() || !process->Alive())
        {
            LOG_INFO("Table", "\"" + description + "\" went away during its switch; the process it ran in is closed, so nothing of it is left to take out");
            return;
        }
        if (on)
        {
            const Asm::AaResult undone = m_engine.AutoAssemble(script, Asm::AaSection::Disable, memory, false, OwnerOf(recordId), nullptr);
            if (undone.Ok())
                LOG_WARN("Table", "\"" + description + "\" went away while it was switched on; its [DISABLE] part ran to take its changes out of the game again");
            else
                m_engine.Notify(LogLevel::Error, "\"" + description + "\" went away while it was switched on, and its [DISABLE] part failed (" + undone.Error.Text +
                                                     "): its changes stay in the game");
        }
        for (const std::string& name : memory.Registered)
            m_engine.Symbols().Unregister(name);
        if (!memory.Registered.empty())
            LOG_INFO("Table", "Unregistered the symbols \"" + description + "\" left: " + Join(memory.Registered, ", "));
        if (!memory.Allocations.empty())
        {
            std::vector<std::string> left;
            for (const Asm::AaAllocation& allocation : memory.Allocations)
                left.push_back(allocation.Name + " at " + Hex(allocation.Address));
            LOG_WARN("Table", Format("%zu allocation%s of \"%s\" stay in the game (its script does not dealloc %s): %s", memory.Allocations.size(),
                memory.Allocations.size() == 1 ? "" : "s", description.c_str(), memory.Allocations.size() == 1 ? "it" : "them", Join(left, ", ").c_str()));
        }
    }

    Status ActivationRunner::ActivateTree(int recordId, bool active, int viaParent, bool fromScript)
    {
        Table::MemoryRecord* record = m_engine.Table().FindById(recordId);
        if (!record || record->Active == active)
            return Status::Success();
        // ∑ A record's own handler, OnActivate code or {$lua} that switches it again would run a second switch of the same
        //   record inside the first; it waits for the first in asynchronous mode and is refused otherwise.
        if (m_switching.count(recordId))
        {
            if (m_async)
            {
                LOG_INFO("Table", "\"" + record->Description + "\" is being switched; the new switch waits until that one is done");
                RequestActive(*record, active, fromScript);
                return Status::Success();
            }
            const Status refused = Status::Fail(ErrorCode::RecordSwitching, "\"" + record->Description + "\" is being switched already and cannot be switched again from its own activation");
            LOG_WARN("Table", refused.Text);
            return refused;
        }
        const SwitchingMark mark(m_switching, recordId);
        return SwitchTree(recordId, m_engine.Table().Identity(), active, viaParent, fromScript);
    }

    Status ActivationRunner::SwitchTree(int recordId, std::uint64_t identity, bool active, int viaParent, bool fromScript)
    {
        Table::MemoryRecord* record = m_engine.Table().FindById(recordId);
        const std::string description = record->Description;
        ActivationTraceEntry trace = TraceStart(*record, active, fromScript, viaParent);
        trace.Async = false;
        const auto gone = [&](const char* during)
            {
                trace.Result = ActivationTraceEntry::Outcome::Failed;
                trace.Error = std::string("went away ") + during;
                Traced(std::move(trace));
                return Vanished(recordId, identity, description, during);
            };
        const bool proceed = m_engine.Lua().BeforeActivation(*record, active);
        if (!(record = Refind(recordId, identity)))
            return gone("in its onMemRecPreExecute or on(de)activate handler");
        if (!proceed)
        {
            trace.Result = ActivationTraceEntry::Outcome::Vetoed;
            Traced(std::move(trace));
            NoteOutcome(recordId, Vetoed(*record, active));
            return Status::Success();
        }
        if (!active && record->Options.DeactivateChildren)
        {
            // ∑ by a copied list of ids: a child's Lua may add or delete its siblings
            for (int child : ChildIds(*record, true))
            {
                const Status status = ActivateTree(child, false, recordId, fromScript);
                if (!status)
                    m_engine.Notify(status);
                if (!Refind(recordId, identity))
                    return gone("while its children were switched off");
            }
            record = Refind(recordId, identity);
        }
        const double started = m_engine.Now();
        if (record->Kind == Table::RecordKind::Script)
        {
            // ∑ assembled into a copy: a {$lua} block may delete the record, and its memory with it, while the script runs
            const Asm::AaMemory before = record->ScriptMemory;
            Asm::AaMemory memory = record->ScriptMemory;
            const std::string script = record->Script;
            const std::shared_ptr<Target::Process> process = m_engine.Process();
            const Asm::AaResult result = m_engine.AutoAssemble(script, active ? Asm::AaSection::Enable : Asm::AaSection::Disable, memory, false, OwnerOf(recordId), record);
            trace.Seconds = m_engine.Now() - started;
            TraceScript(trace, result, before, memory);
            if (!(record = Refind(recordId, identity)))
            {
                const Status status = gone("while its script ran");
                // its changes are in the game after an [ENABLE] that worked or a [DISABLE] that failed
                ReleaseOrphan(recordId, description, script, std::move(memory), result.Ok() == active, process);
                return status;
            }
            Traced(std::move(trace));
            record->ScriptMemory = std::move(memory);
            if (!result.Ok())
            {
                record->LastError = result.Error.Text;
                NoteOutcome(recordId, result.Error);
                m_engine.Lua().AfterActivation(*record, active, false);
                return result.Error;
            }
            record->LastError.clear();
            record->Active = active;
            if (!active)
                record->ScriptMemory.Values.clear();
            LOG_SUCCESS("Table", std::string(active ? "Enabled " : "Disabled ") + description);
        }
        else
        {
            const Status plain = ActivatePlain(record, recordId, identity, active);
            trace.Seconds = m_engine.Now() - started;
            if (!plain)
            {
                trace.Result = ActivationTraceEntry::Outcome::Failed;
                trace.Error = plain.Text;
            }
            Traced(std::move(trace));
            if (!record)
                return plain;
            if (!plain)
            {
                NoteOutcome(recordId, plain);
                m_engine.Lua().AfterActivation(*record, active, false);
                return plain;
            }
        }
        NoteOutcome(recordId, Status::Success());

        if (active && record->Options.ActivateChildren)
        {
            for (int child : ChildIds(*record, false))
            {
                const Status status = ActivateTree(child, true, recordId, fromScript);
                if (!status)
                    m_engine.Notify(status);
                if (!Refind(recordId, identity))
                    return Vanished(recordId, identity, description, "while its children were switched on");
            }
            record = Refind(recordId, identity);
        }
        const std::string code = active ? record->LuaOnActivate : record->LuaOnDeactivate;
        if (!code.empty())
        {
            RunRecordLua(code, active ? "OnActivate" : "OnDeactivate");
            if (!(record = Refind(recordId, identity)))
                return Vanished(recordId, identity, description, active ? "in its OnActivate code" : "in its OnDeactivate code");
        }
        m_engine.Lua().AfterActivation(*record, active, true);
        return Status::Success();
    }

    Status ActivationRunner::ActivatePlain(Table::MemoryRecord*& record, int recordId, std::uint64_t identity, bool active)
    {
        if (record->Kind == Table::RecordKind::Value && active)
        {
            if (!m_engine.Process())
                return Status::Fail(ErrorCode::NoProcess);
            // ∑ Reading resolves the address, which may run Lua (a symbol
            //   lookup callback, a {$lua} symbol) that deletes the record or
            //   replaces the table: found again by id afterwards.
            const std::string description = record->Description;
            std::vector<std::uint8_t> bytes = m_engine.ReadRecord(*record);
            if (!(record = Refind(recordId, identity)))
                return Vanished(recordId, identity, description, "while its value was read");
            if (bytes.empty())
                return Status::Fail(ErrorCode::ReadFailed, record->AddressOk ? Hex(record->ResolvedAddress) : record->Address);
            record->FrozenBytes = std::move(bytes);
        }
        record->Active = active;
        return Status::Success();
    }

    bool ActivationRunner::OnActivationWorker()
    {
        return t_activationWorker;
    }

    void ActivationRunner::RunRecordLua(const std::string& code, const char* name)
    {
        if (!m_async)
        {
            m_engine.Lua().Run(code, name);
            return;
        }
        m_engine.Lua().StartTask(code, name, {});
    }

    void ActivationRunner::RunTableScript(const std::string& script)
    {
        if (!m_async)
        {
            const Status run = m_engine.Lua().Run(script, "table script");
            if (!run)
            {
                m_engine.Notify(Status::Fail(ErrorCode::LuaTableScriptFailed, run.Text));
                Sounds::Play(Sounds::Cue::Error);
            }
            return;
        }
        m_tableTaskStarted = m_engine.Now();
        m_tableTaskStopped = false;
        auto id = std::make_shared<int>(0);
        const int started = m_engine.Lua().StartTask(script, "table script", [this, id](const Status& status, const std::string&)
            {
                const bool early = *id == 0;
                const bool current = early || m_tableTask == *id;
                if (current && !early)
                    m_tableTask = 0;
                if (status)
                    LOG_INFO("Table", Format("The table's Lua script finished after %.1f s", m_engine.Now() - m_tableTaskStarted));
                else if (current && m_tableTaskStopped)
                    LOG_INFO("Table", "The table's Lua script was stopped");
                else if (current)
                {
                    m_engine.Notify(Status::Fail(ErrorCode::LuaTableScriptFailed, status.Text));
                    Sounds::Play(Sounds::Cue::Error);
                }
            });
        *id = started;
        m_tableTask = started;
    }

    void ActivationRunner::StopTableScript()
    {
        if (m_tableTask && m_engine.Lua().TaskAlive(m_tableTask))
        {
            m_tableTaskStopped = true;
            m_engine.Lua().StopTask(m_tableTask);
        }
    }

    void ActivationRunner::RequestActive(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        if (!m_async)
        {
            const Status status = SetActive(record, active, fromScript);
            if (!status)
            {
                m_engine.Notify(status);
                if (!fromScript)
                    Sounds::Play(Sounds::Cue::Error);
            }
            return;
        }
        if (!ActivationPending(record.Id) && record.Active == active)
            return;
        for (ActivationJob& job : m_activations)
        {
            if (job.RecordId == record.Id)
            {
                job.Active = active;
                job.FromScript = job.FromScript || fromScript;
                return;
            }
        }
        ActivationJob job;
        job.RecordId = record.Id;
        job.Identity = m_engine.Table().Identity();
        job.Active = active;
        job.FromScript = fromScript;
        job.QueuedAt = m_engine.Now();
        m_activations.push_back(job);
        ++m_pending[record.Id];
        ++m_progressTotal;
    }

    void ActivationRunner::Unpend(int recordId)
    {
        const auto found = m_pending.find(recordId);
        if (found != m_pending.end() && --found->second <= 0)
            m_pending.erase(found);
        ++m_progressDone;
    }

    void ActivationRunner::DropQueued(int recordId)
    {
        for (auto it = m_activations.begin(); it != m_activations.end();)
        {
            if (it->RecordId != recordId)
            {
                ++it;
                continue;
            }
            Unpend(recordId);
            --m_progressDone;
            --m_progressTotal;
            it = m_activations.erase(it);
        }
    }

    void ActivationRunner::CancelActivations()
    {
        for (const ActivationJob& job : m_activations)
        {
            Unpend(job.RecordId);
            --m_progressDone;
            --m_progressTotal;
        }
        m_activations.clear();
    }

    ActivationProgress ActivationRunner::Progress() const
    {
        ActivationProgress progress;
        progress.Done = m_progressDone;
        progress.Total = m_progressTotal;
        progress.Current = m_progressCurrent;
        progress.TableScript = m_tableTask != 0 && m_engine.Lua().TaskAlive(m_tableTask);
        progress.TableScriptSeconds = progress.TableScript ? m_engine.Now() - m_tableTaskStarted : 0.0;
        return progress;
    }

    bool ActivationRunner::Working() const
    {
        return m_running || !m_activations.empty() || m_engine.Lua().HasTasks();
    }

    void ActivationRunner::QueueChildren(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        std::vector<int> ids;
        for (const auto& child : record.Children)
            ids.push_back(child->Id);
        if (active)
            std::reverse(ids.begin(), ids.end());
        for (int id : ids)
        {
            ActivationJob job;
            job.RecordId = id;
            job.Identity = m_engine.Table().Identity();
            job.Active = active;
            job.FromScript = fromScript;
            job.ViaParent = record.Id;
            job.QueuedAt = m_engine.Now();
            m_activations.push_front(job);
            ++m_pending[id];
            ++m_progressTotal;
        }
    }

    void ActivationRunner::FinishRecordActivation(int recordId, std::uint64_t identity, bool active, bool fromScript)
    {
        Table::MemoryRecord* record = Refind(recordId, identity);
        if (!record)
            return;
        if (active && record->Options.ActivateChildren)
            QueueChildren(*record, active, fromScript);
        const std::string code = active ? record->LuaOnActivate : record->LuaOnDeactivate;
        if (!code.empty())
        {
            const std::string description = record->Description;
            RunRecordLua(code, active ? "OnActivate" : "OnDeactivate");
            if (!(record = Refind(recordId, identity)))
            {
                Vanished(recordId, identity, description, active ? "in its OnActivate code" : "in its OnDeactivate code");
                return;
            }
        }
        m_engine.Lua().AfterActivation(*record, active, true);
    }

    void ActivationRunner::PumpActivations(double budgetSeconds)
    {
        if (m_running)
        {
            // ∑ Every name the worker looks up and every custom command waits here for the main thread. Served once a tick,
            //   a script with 140 such trips took a second; served while they keep coming, within the budget, it takes a tick.
            const double serveUntil = m_engine.Now() + budgetSeconds;
            while (m_running && !m_running->Finished && m_engine.Now() < serveUntil && m_mainQueue.WaitForWork(std::chrono::milliseconds(1)))
                DrainMainWork();
            if (m_running && !m_running->Finished && m_running->Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            if (m_running)
                FinishActivation();
        }
        const bool tableScript = m_tableTask && m_engine.Lua().TaskAlive(m_tableTask);
        const double deadline = m_engine.Now() + budgetSeconds;
        while (!m_running && !m_activations.empty() && m_engine.Now() < deadline)
        {
            auto next = m_activations.begin();
            if (tableScript)
            {
                next = std::find_if(m_activations.begin(), m_activations.end(), [](const ActivationJob& job) { return job.FromScript; });
                if (next == m_activations.end())
                    break;
            }
            const ActivationJob job = *next;
            m_activations.erase(next);
            StartActivation(job);
        }
        if (!m_running && m_activations.empty() && m_progressDone >= m_progressTotal)
        {
            m_progressDone = 0;
            m_progressTotal = 0;
            m_progressCurrent.clear();
        }
    }

    std::vector<int> ActivationRunner::BottomToTop()
    {
        std::vector<int> ids;
        for (const Table::MemoryRecord* record : m_engine.Table().Flatten())
            ids.push_back(record->Id);
        std::reverse(ids.begin(), ids.end());
        return ids;
    }

    void ActivationRunner::RequestDeactivateAll()
    {
        if (!m_async)
        {
            const std::vector<std::string> failures = DeactivateAll();
            if (!failures.empty())
                m_engine.Notify(LogLevel::Warning, std::to_string(failures.size()) + (failures.size() == 1 ? " script could not be turned off: " : " scripts could not be turned off: ") + failures.front());
            return;
        }
        CancelActivations();
        for (int id : BottomToTop())
        {
            if (Table::MemoryRecord* record = m_engine.Table().FindById(id); record && (record->Active || ActivationPending(id)))
                RequestActive(*record, false);
        }
    }

    void ActivationRunner::StartActivation(const ActivationJob& job)
    {
        const int recordId = job.RecordId;
        const bool active = job.Active;
        const bool fromScript = job.FromScript;
        Table::MemoryRecord* record = Refind(recordId, job.Identity);
        if (!record || record->Active == active)
        {
            Unpend(recordId);
            return;
        }
        if (!active && !job.ChildrenDone && record->Options.DeactivateChildren)
        {
            const bool childrenOn = std::any_of(record->Children.begin(), record->Children.end(), [&](const auto& child) { return child->Active || ActivationPending(child->Id); });
            if (childrenOn)
            {
                ActivationJob again = job;
                again.ChildrenDone = true;
                m_activations.push_front(again);
                for (const auto& child : record->Children)
                {
                    ActivationJob childJob;
                    childJob.RecordId = child->Id;
                    childJob.Identity = job.Identity;
                    childJob.Active = false;
                    childJob.FromScript = fromScript;
                    childJob.ViaParent = recordId;
                    childJob.QueuedAt = m_engine.Now();
                    m_activations.push_front(childJob);
                    ++m_pending[child->Id];
                    ++m_progressTotal;
                }
                return;
            }
        }
        const std::uint64_t identity = m_engine.Table().Identity();
        const std::string description = record->Description;
        // its handlers run now: a switch of the record they ask for goes in the queue after this one
        const SwitchingMark mark(m_switching, recordId);
        ActivationTraceEntry trace = TraceStart(*record, active, fromScript, job.ViaParent);
        const double started = m_engine.Now();
        trace.Waited = job.QueuedAt > 0.0 ? started - job.QueuedAt : 0.0;
        const bool proceed = m_engine.Lua().BeforeActivation(*record, active);
        if (!(record = Refind(recordId, identity)))
        {
            trace.Result = ActivationTraceEntry::Outcome::Failed;
            trace.Error = "went away in its onMemRecPreExecute or on(de)activate handler";
            Traced(std::move(trace));
            Unpend(recordId);
            Vanished(recordId, identity, description, "in its onMemRecPreExecute or on(de)activate handler");
            return;
        }
        if (!proceed)
        {
            trace.Result = ActivationTraceEntry::Outcome::Vetoed;
            Traced(std::move(trace));
            NoteOutcome(recordId, Vetoed(*record, active));
            Unpend(recordId);
            return;
        }
        m_progressCurrent = record->Description;
        if (record->Kind != Table::RecordKind::Script)
        {
            const Status status = ActivatePlain(record, recordId, identity, active);
            trace.Seconds = m_engine.Now() - started;
            if (!status)
            {
                trace.Result = ActivationTraceEntry::Outcome::Failed;
                trace.Error = status.Text;
            }
            Traced(std::move(trace));
            Unpend(recordId);
            if (!record)
                return;
            NoteOutcome(recordId, status);
            if (!status)
            {
                m_engine.Lua().AfterActivation(*record, active, false);
                m_engine.Notify(status);
                if (!fromScript)
                    Sounds::Play(Sounds::Cue::Error);
                return;
            }
            FinishRecordActivation(recordId, identity, active, fromScript);
            m_engine.Table().Refresh();
            return;
        }

        auto running = std::make_unique<RunningActivation>();
        running->RecordId = recordId;
        running->Identity = identity;
        running->Description = description;
        running->Script = record->Script;
        running->Active = active;
        running->FromScript = fromScript;
        running->Memory = record->ScriptMemory;
        running->Before = record->ScriptMemory;
        running->StartedAt = started;
        running->QueuedAt = job.QueuedAt;
        running->ViaParent = job.ViaParent;
        running->Process = m_engine.Process();
        Asm::AaOptions options;
        options.Section = active ? Asm::AaSection::Enable : Asm::AaSection::Disable;
        options.CheckOnly = false;
        options.Owner = OwnerOf(recordId);
        options.Log = m_engine.Options().AutoAssemblerLog;
        options.LogTitle = Engine::AutoAssemblerLogTitle(options.Owner, record);
        options.IncludeFolder = m_engine.Table().FilePath.empty() ? Paths::AutorunFolder() : std::filesystem::path(Utf8ToWide(m_engine.Table().FilePath)).parent_path();
        Asm::AaMemory* memory = &running->Memory;
        std::shared_ptr<Target::Process> process = m_engine.Process();
        running->Result = std::async(std::launch::async, [this, script = record->Script, options, memory, process, recordId, identity, finished = &running->Finished]()
            {
                SetThreadName(L"Script activation");
                t_activationWorker = true;
                Asm::AaHost host;
                host.Process = process.get();
                host.Symbols = &m_engine.Symbols();
                host.RunLua = [this, recordId, identity](std::string_view code, bool syntaxCheck, std::string& output) { return RunLuaForWorker(code, recordId, identity, syntaxCheck, output); };
                host.IsCustomCommand = [this](std::string_view name)
                    {
                        bool known = false;
                        RunOnMainAndWait([&]() { known = m_engine.Lua().IsAutoAssemblerCommand(name); });
                        return known;
                    };
                host.RunCustomCommand = [this, recordId, identity](std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output)
                    {
                        Status status = Status::Success();
                        RunOnMainAndWait([&]()
                            {
                                m_workerLuaActive = true;
                                const Lua::ScriptGlobals saved = m_engine.Lua().SaveScriptGlobals();
                                m_engine.Lua().SetScriptGlobals(Refind(recordId, identity), syntaxCheck);
                                status = m_engine.Lua().RunAutoAssemblerCommand(name, parameters, syntaxCheck, output);
                                m_engine.Lua().RestoreScriptGlobals(saved);
                                m_workerLuaActive = false;
                            });
                        return status;
                    };
                Asm::AaResult result = Asm::RunAutoAssembler(script, host, *memory, options);
                t_activationWorker = false;
                // ∑ wakes the main thread's wait for the next request, which otherwise runs out only after a timer step (15.6 ms)
                *finished = true;
                m_mainQueue.Post([] {});
                return result;
            });
        m_running = std::move(running);
    }

    void ActivationRunner::FinishActivation()
    {
        std::unique_ptr<RunningActivation> running = std::move(m_running);
        const Asm::AaResult result = running->Result.get();
        Unpend(running->RecordId);
        const SwitchingMark mark(m_switching, running->RecordId);
        // ∑ by id and table: a replaced table may have a record with the same id, which must not get this script's memory
        Table::MemoryRecord* record = Refind(running->RecordId, running->Identity);
        ActivationTraceEntry trace;
        if (record)
            trace = TraceStart(*record, running->Active, running->FromScript, running->ViaParent);
        else
        {
            trace.RecordId = running->RecordId;
            trace.Description = running->Description;
            trace.Kind = Table::RecordKind::Script;
            trace.Active = running->Active;
            trace.FromScript = running->FromScript;
            trace.ViaParent = running->ViaParent;
        }
        trace.Async = true;
        trace.Seconds = m_engine.Now() - running->StartedAt;
        trace.Waited = running->QueuedAt > 0.0 ? running->StartedAt - running->QueuedAt : 0.0;
        TraceScript(trace, result, running->Before, running->Memory);
        if (!record)
        {
            // ∑ Never dropped: the hooks and allocations of an [ENABLE] that worked would stay in the game with nothing left
            //   to take them out again.
            trace.Notes.push_back("the record went away while its script ran; the log says what was taken out of the game again and what stays");
            Traced(std::move(trace));
            Vanished(running->RecordId, running->Identity, running->Description, "while its script ran in the background");
            ReleaseOrphan(running->RecordId, running->Description, running->Script, std::move(running->Memory), result.Ok() == running->Active, running->Process);
            m_engine.Table().Refresh();
            return;
        }
        Traced(std::move(trace));
        record->ScriptMemory = std::move(running->Memory);
        if (!result.Ok())
        {
            record->LastError = result.Error.Text;
            NoteOutcome(running->RecordId, result.Error);
            m_engine.Lua().AfterActivation(*record, running->Active, false);
            m_engine.Notify(result.Error);
            if (!running->FromScript)
                Sounds::Play(Sounds::Cue::Error);
            m_engine.Table().Refresh();
            return;
        }
        record->LastError.clear();
        record->Active = running->Active;
        if (!running->Active)
            record->ScriptMemory.Values.clear();
        NoteOutcome(running->RecordId, Status::Success());
        LOG_SUCCESS("Table", std::string(running->Active ? "Enabled " : "Disabled ") + record->Description);
        FinishRecordActivation(running->RecordId, running->Identity, running->Active, running->FromScript);
        m_engine.Table().Refresh();
    }

    void ActivationRunner::WaitForActivation()
    {
        if (!m_running || m_workerLuaActive || !Manifold::OnMainThread())
            return;
        const bool previous = m_waitingForWorker;
        m_waitingForWorker = true;
        // ∑ a Lua task run while waiting may wait for (and finish) the same activation itself
        const RunningActivation* waited = m_running.get();
        while (m_running.get() == waited && m_running->Result.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
        {
            DrainMainWork();
            m_engine.Lua().RunTasks(0.005);
        }
        m_waitingForWorker = previous;
        if (m_running.get() == waited)
            FinishActivation();
    }

    void ActivationRunner::RunOnMainAndWait(const std::function<void()>& work)
    {
        if (Manifold::OnMainThread())
        {
            work();
            return;
        }
        std::promise<void> finished;
        std::future<void> future = finished.get_future();
        m_mainQueue.Post([&]()
            {
                const bool serving = m_workerLuaActive;
                m_workerLuaActive = true;
                work();
                m_workerLuaActive = serving;
                finished.set_value();
            });
        future.wait();
    }

    void ActivationRunner::DrainMainWork()
    {
        m_mainQueue.Drain();
    }

    Status ActivationRunner::RunLuaForWorker(std::string_view code, int recordId, std::uint64_t identity, bool syntaxCheck, std::string& output)
    {
        std::promise<Status> finished;
        std::future<Status> future = finished.get_future();
        std::string result;
        const std::string text(code);
        m_mainQueue.Post([&, this]()
            {
                if (m_async && !m_waitingForWorker)
                {
                    auto saved = std::make_shared<Lua::ScriptGlobals>();
                    m_engine.Lua().StartTask(text, "{$lua}", [&](const Status& status, const std::string& value)
                        {
                            result = value;
                            finished.set_value(status);
                        },
                        [this, saved, recordId, identity, syntaxCheck]()
                        {
                            *saved = m_engine.Lua().SaveScriptGlobals();
                            m_engine.Lua().SetScriptGlobals(Refind(recordId, identity), syntaxCheck);
                            m_workerLuaActive = true;
                        },
                        [this, saved]()
                        {
                            m_engine.Lua().RestoreScriptGlobals(*saved);
                            m_workerLuaActive = false;
                        });
                    return;
                }
                m_workerLuaActive = true;
                const Lua::ScriptGlobals saved = m_engine.Lua().SaveScriptGlobals();
                m_engine.Lua().SetScriptGlobals(Refind(recordId, identity), syntaxCheck);
                const Status status = m_engine.Lua().RunWithResult(text, "{$lua}", result);
                m_engine.Lua().RestoreScriptGlobals(saved);
                m_workerLuaActive = false;
                finished.set_value(status);
            });
        const Status status = future.get();
        output = result;
        return status;
    }

    Status ActivationRunner::SetActive(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        const int recordId = record.Id;
        const std::uint64_t identity = m_engine.Table().Identity();
        const std::string description = record.Description;
        if (m_running && Manifold::OnMainThread())
        {
            // finishing the running activation runs Lua, which may delete this record
            WaitForActivation();
            if (!Refind(recordId, identity))
                return Vanished(recordId, identity, description, "while the running activation finished");
        }
        // ∑ Still running: this comes from that activation's own {$lua} or custom command. A sync switch of the record whose
        //   script the worker is assembling right now would run the two at once; it goes in the queue after it instead.
        if (m_running && m_running->RecordId == recordId && m_running->Identity == identity)
        {
            LOG_INFO("Table", "\"" + description + "\" is being switched in the background; the new switch waits until that one is done");
            RequestActive(*Refind(recordId, identity), active, fromScript);
            return Status::Success();
        }
        const Status status = ActivateTree(recordId, active, -1, fromScript);
        m_engine.Table().Refresh();
        return status;
    }

    std::vector<Table::MemoryRecord*> ActivationRunner::ActiveScripts()
    {
        std::vector<Table::MemoryRecord*> active;
        if (!m_engine.Process() || !m_engine.Process()->Alive())
            return active;
        m_engine.Table().ForEach([&](Table::MemoryRecord& record)
            {
                if (record.Kind == Table::RecordKind::Script && record.Active)
                    active.push_back(&record);
            });
        return active;
    }

    std::vector<std::string> ActivationRunner::DeactivateAll()
    {
        CancelActivations();
        WaitForActivation();
        std::vector<std::string> failures;
        const std::uint64_t identity = m_engine.Table().Identity();
        for (int id : BottomToTop())
        {
            // a switch's Lua may replace the table: the ids left are those of the old one
            if (m_engine.Table().Identity() != identity)
                break;
            Table::MemoryRecord* record = m_engine.Table().FindById(id);
            if (!record || !record->Active)
                continue;
            const bool script = record->Kind == Table::RecordKind::Script;
            const std::string description = record->Description;
            const Status status = SetActive(*record, false);
            if (!status && script)
                failures.push_back(description + ": " + status.Text);
            else if (!script)
            {
                if (Table::MemoryRecord* again = Refind(id, identity))
                    again->Active = false;
            }
        }
        if (failures.empty())
            LOG_INFO("Table", "Turned off every active record");
        else
            LOG_WARN("Table", std::to_string(failures.size()) + " scripts could not be turned off");
        return failures;
    }
}
