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
        std::shared_ptr<Target::Process> Process;
        std::future<Asm::AaResult> Result;
    };

    ActivationRunner::ActivationRunner(Engine& engine) : m_engine(engine)
    {
    }

    ActivationRunner::~ActivationRunner() = default;

    Status ActivationRunner::ActivateTree(Table::MemoryRecord& record, bool active)
    {
        if (record.Active == active)
            return Status::Success();
        if (!m_engine.Lua().BeforeActivation(record, active))
            return Status::Success();
        if (!active && record.Options.DeactivateChildren)
        {
            for (auto it = record.Children.rbegin(); it != record.Children.rend(); ++it)
            {
                const Status status = ActivateTree(**it, false);
                if (!status)
                    m_engine.Notify(status);
            }
        }
        if (record.Kind == Table::RecordKind::Script)
        {
            const Asm::AaResult result = m_engine.AutoAssemble(record.Script, active ? Asm::AaSection::Enable : Asm::AaSection::Disable, record.ScriptMemory, false, OwnerOf(record), &record);
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
                const Status status = ActivateTree(*child, true);
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
            const Status status = SetActive(record, active);
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
        m_activations.push_back({ record.Id, active, fromScript });
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
            m_activations.push_front({ id, active, fromScript });
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
                    m_activations.push_front({ child->Id, false, fromScript });
                    ++m_pending[child->Id];
                    ++m_progressTotal;
                }
                return;
            }
        }
        if (!m_engine.Lua().BeforeActivation(*record, active))
        {
            Unpend(recordId);
            return;
        }
        m_progressCurrent = record->Description;
        if (record->Kind != Table::RecordKind::Script)
        {
            const Status status = ActivatePlain(*record, active);
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
        if (!m_running || m_workerLuaActive || !m_engine.OnMainThread())
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
        if (m_engine.OnMainThread())
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

    Status ActivationRunner::SetActive(Table::MemoryRecord& record, bool active)
    {
        if (m_running && m_engine.OnMainThread())
            WaitForActivation();
        const Status status = ActivateTree(record, active);
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
