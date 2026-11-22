#include "App/ActivationRunner.h"

#include "App/Engine.h"
#include "App/Sounds.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <chrono>
#include <future>

namespace Manifold
{
    namespace
    {
        thread_local bool t_activationWorker = false;

        std::string OwnerOf(const Table::MemoryRecord& record)
        {
            return "record:" + std::to_string(record.Id);
        }
    }

    struct ActivationRunner::RunningActivation
    {
        int RecordId = 0;
        bool Active = false;
        bool FromScript = false;
        Asm::AaMemory Memory;
        Asm::AaMemory Before;
        double StartedAt = 0.0;
        double QueuedAt = 0.0;
        int ViaParent = -1;
        std::shared_ptr<Target::Process> Process;
        std::future<Asm::AaResult> Result;
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
            if (entry.Writes.size() < kTraceWrites)
                entry.Writes.push_back(write);
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

    Status ActivationRunner::ActivateTree(Table::MemoryRecord& record, bool active, int viaParent, bool fromScript)
    {
        if (record.Active == active)
            return Status::Success();
        ActivationTraceEntry trace = TraceStart(record, active, fromScript, viaParent);
        trace.Async = false;
        if (!m_engine.Lua().BeforeActivation(record, active))
        {
            trace.Result = ActivationTraceEntry::Outcome::Vetoed;
            Traced(std::move(trace));
            return Status::Success();
        }
        if (!active && record.Options.DeactivateChildren)
        {
            for (auto it = record.Children.rbegin(); it != record.Children.rend(); ++it)
            {
                const Status status = ActivateTree(**it, false, record.Id, fromScript);
                if (!status)
                    m_engine.Notify(status);
            }
        }
        const double started = m_engine.Now();
        if (record.Kind == Table::RecordKind::Script)
        {
            const Asm::AaMemory before = record.ScriptMemory;
            const Asm::AaResult result = m_engine.AutoAssemble(record.Script, active ? Asm::AaSection::Enable : Asm::AaSection::Disable, record.ScriptMemory, false, OwnerOf(record), &record);
            trace.Seconds = m_engine.Now() - started;
            TraceScript(trace, result, before, record.ScriptMemory);
            Traced(std::move(trace));
            if (!result.Ok())
            {
                record.LastError = result.Error.Text;
                m_engine.Lua().AfterActivation(record, active, false);
                return result.Error;
            }
            record.LastError.clear();
            record.Active = active;
            if (!active)
                record.ScriptMemory.Values.clear();
            LOG_SUCCESS("Table", std::string(active ? "Enabled " : "Disabled ") + record.Description);
        }
        else
        {
            const Status plain = ActivatePlain(record, active);
            trace.Seconds = m_engine.Now() - started;
            if (!plain)
            {
                trace.Result = ActivationTraceEntry::Outcome::Failed;
                trace.Error = plain.Text;
            }
            Traced(std::move(trace));
            if (!plain)
            {
                m_engine.Lua().AfterActivation(record, active, false);
                return plain;
            }
        }

        if (active && record.Options.ActivateChildren)
        {
            for (const auto& child : record.Children)
            {
                const Status status = ActivateTree(*child, true, record.Id, fromScript);
                if (!status)
                    m_engine.Notify(status);
            }
        }
        if (!record.LuaOnActivate.empty() && active)
            RunRecordLua(record.LuaOnActivate, "OnActivate");
        if (!record.LuaOnDeactivate.empty() && !active)
            RunRecordLua(record.LuaOnDeactivate, "OnDeactivate");
        m_engine.Lua().AfterActivation(record, active, true);
        return Status::Success();
    }

    Status ActivationRunner::ActivatePlain(Table::MemoryRecord& record, bool active)
    {
        if (record.Kind == Table::RecordKind::Value && active)
        {
            if (!m_engine.Process())
                return Status::Fail(ErrorCode::NoProcess);
            std::vector<std::uint8_t> bytes = m_engine.ReadRecord(record);
            if (bytes.empty())
                return Status::Fail(ErrorCode::ReadFailed, record.AddressOk ? Hex(record.ResolvedAddress) : record.Address);
            record.FrozenBytes = std::move(bytes);
        }
        record.Active = active;
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
            job.Active = active;
            job.FromScript = fromScript;
            job.ViaParent = record.Id;
            job.QueuedAt = m_engine.Now();
            m_activations.push_front(job);
            ++m_pending[id];
            ++m_progressTotal;
        }
    }

    void ActivationRunner::FinishRecordActivation(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        if (active && record.Options.ActivateChildren)
            QueueChildren(record, active, fromScript);
        if (!record.LuaOnActivate.empty() && active)
            RunRecordLua(record.LuaOnActivate, "OnActivate");
        if (!record.LuaOnDeactivate.empty() && !active)
            RunRecordLua(record.LuaOnDeactivate, "OnDeactivate");
        m_engine.Lua().AfterActivation(record, active, true);
    }

    void ActivationRunner::PumpActivations(double budgetSeconds)
    {
        if (m_running)
        {
            if (m_running->Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
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
        Table::MemoryRecord* record = m_engine.Table().FindById(recordId);
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
        ActivationTraceEntry trace = TraceStart(*record, active, fromScript, job.ViaParent);
        const double started = m_engine.Now();
        trace.Waited = job.QueuedAt > 0.0 ? started - job.QueuedAt : 0.0;
        if (!m_engine.Lua().BeforeActivation(*record, active))
        {
            trace.Result = ActivationTraceEntry::Outcome::Vetoed;
            Traced(std::move(trace));
            Unpend(recordId);
            return;
        }
        m_progressCurrent = record->Description;
        if (record->Kind != Table::RecordKind::Script)
        {
            const Status status = ActivatePlain(*record, active);
            trace.Seconds = m_engine.Now() - started;
            if (!status)
            {
                trace.Result = ActivationTraceEntry::Outcome::Failed;
                trace.Error = status.Text;
            }
            Traced(std::move(trace));
            Unpend(recordId);
            if (!status)
            {
                m_engine.Lua().AfterActivation(*record, active, false);
                m_engine.Notify(status);
                if (!fromScript)
                    Sounds::Play(Sounds::Cue::Error);
                return;
            }
            FinishRecordActivation(*record, active, fromScript);
            m_engine.Table().Refresh();
            return;
        }

        auto running = std::make_unique<RunningActivation>();
        running->RecordId = recordId;
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
        options.Owner = OwnerOf(*record);
        options.IncludeFolder = m_engine.Table().FilePath.empty() ? Paths::AutorunFolder() : std::filesystem::path(Utf8ToWide(m_engine.Table().FilePath)).parent_path();
        Asm::AaMemory* memory = &running->Memory;
        std::shared_ptr<Target::Process> process = m_engine.Process();
        running->Result = std::async(std::launch::async, [this, script = record->Script, options, memory, process, recordId]()
            {
                SetThreadName(L"Script activation");
                t_activationWorker = true;
                Asm::AaHost host;
                host.Process = process.get();
                host.Symbols = &m_engine.Symbols();
                host.RunLua = [this, recordId](std::string_view code, bool syntaxCheck, std::string& output) { return RunLuaForWorker(code, recordId, syntaxCheck, output); };
                host.IsCustomCommand = [this](std::string_view name)
                    {
                        bool known = false;
                        RunOnMainAndWait([&]() { known = m_engine.Lua().IsAutoAssemblerCommand(name); });
                        return known;
                    };
                host.RunCustomCommand = [this, recordId](std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output)
                    {
                        Status status = Status::Success();
                        RunOnMainAndWait([&]()
                            {
                                m_workerLuaActive = true;
                                const Lua::ScriptGlobals saved = m_engine.Lua().SaveScriptGlobals();
                                m_engine.Lua().SetScriptGlobals(m_engine.Table().FindById(recordId), syntaxCheck);
                                status = m_engine.Lua().RunAutoAssemblerCommand(name, parameters, syntaxCheck, output);
                                m_engine.Lua().RestoreScriptGlobals(saved);
                                m_workerLuaActive = false;
                            });
                        return status;
                    };
                Asm::AaResult result = Asm::RunAutoAssembler(script, host, *memory, options);
                t_activationWorker = false;
                return result;
            });
        m_running = std::move(running);
    }

    void ActivationRunner::FinishActivation()
    {
        std::unique_ptr<RunningActivation> running = std::move(m_running);
        const Asm::AaResult result = running->Result.get();
        Unpend(running->RecordId);
        Table::MemoryRecord* record = m_engine.Table().FindById(running->RecordId);
        if (!record)
            return;
        ActivationTraceEntry trace = TraceStart(*record, running->Active, running->FromScript, running->ViaParent);
        trace.Async = true;
        trace.Seconds = m_engine.Now() - running->StartedAt;
        trace.Waited = running->QueuedAt > 0.0 ? running->StartedAt - running->QueuedAt : 0.0;
        TraceScript(trace, result, running->Before, running->Memory);
        Traced(std::move(trace));
        record->ScriptMemory = std::move(running->Memory);
        if (!result.Ok())
        {
            record->LastError = result.Error.Text;
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
        LOG_SUCCESS("Table", std::string(running->Active ? "Enabled " : "Disabled ") + record->Description);
        FinishRecordActivation(*record, running->Active, running->FromScript);
        m_engine.Table().Refresh();
    }

    void ActivationRunner::WaitForActivation()
    {
        if (!m_running || m_workerLuaActive || !Manifold::OnMainThread())
            return;
        const bool previous = m_waitingForWorker;
        m_waitingForWorker = true;
        while (m_running->Result.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
        {
            DrainMainWork();
            m_engine.Lua().RunTasks(0.005);
        }
        m_waitingForWorker = previous;
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

    Status ActivationRunner::RunLuaForWorker(std::string_view code, int recordId, bool syntaxCheck, std::string& output)
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
                        [this, saved, recordId, syntaxCheck]()
                        {
                            *saved = m_engine.Lua().SaveScriptGlobals();
                            m_engine.Lua().SetScriptGlobals(m_engine.Table().FindById(recordId), syntaxCheck);
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
                m_engine.Lua().SetScriptGlobals(m_engine.Table().FindById(recordId), syntaxCheck);
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
        if (m_running && Manifold::OnMainThread())
            WaitForActivation();
        const Status status = ActivateTree(record, active, -1, fromScript);
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
        for (int id : BottomToTop())
        {
            Table::MemoryRecord* record = m_engine.Table().FindById(id);
            if (!record || !record->Active)
                continue;
            const Status status = SetActive(*record, false);
            if (!status && record->Kind == Table::RecordKind::Script)
                failures.push_back(record->Description + ": " + status.Text);
            else if (record->Kind != Table::RecordKind::Script)
                record->Active = false;
        }
        if (failures.empty())
            LOG_INFO("Table", "Turned off every active record");
        else
            LOG_WARN("Table", std::to_string(failures.size()) + " scripts could not be turned off");
        return failures;
    }
}
