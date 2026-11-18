#include "App/Engine.h"

#include "App/Sounds.h"

#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>

#include <windows.h>

namespace Manifold
{
    namespace
    {
        constexpr double kAliveCheckSeconds = 1.0;
        constexpr double kAttachCheckSeconds = 1.5;

        constexpr double kTaskBudgetSeconds = 0.010;
        constexpr double kActivationBudgetSeconds = 0.006;
    }

    Engine::Engine(Settings& settings)
        : m_settings(settings), m_settingsStore(settings, Paths::SettingsFile()), m_disassembler64(std::make_unique<Disasm::Disassembler>(true)), m_disassembler32(std::make_unique<Disasm::Disassembler>(false))
    {
        m_lua = std::make_unique<Lua::LuaEngine>(*this);
        m_debugger.SetLogDebugStrings(settings.LogDebugStrings);
        m_debugger.SetPreferHardware(settings.UseHardwareExecuteBreakpoints);
        m_target.Mono().SetPauseCheck([this] { return m_debugger.Attached() && m_debugger.Broken(); });
    }

    Engine::~Engine()
    {
        m_jobs.Stop();
        m_work.Stop();
        CancelActivations();
        m_activation.WaitForActivation();
        m_tracer.Stop();
        m_target.Mono().Stop();
        m_debugger.Detach();
        m_scanner.Reset();
        m_lua.reset();
    }

    double Engine::Now() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    const Disasm::Disassembler& Engine::Disassembler() const
    {
        return (!m_target.Process() || m_target.Process()->Is64Bit()) ? *m_disassembler64 : *m_disassembler32;
    }

    Status Engine::OpenProcess(ProcessId id)
    {
        m_activation.WaitForActivation();
        if (m_target.Process() && m_target.Process()->Id() == id && m_target.Process()->Alive())
        {
            m_target.Reattach();
            LOG_INFO("Process", Format("%s (pid %u) is already open; scripts and symbols stay as they are", m_target.Process()->Name().c_str(), Raw(id)));
            m_lua->OnProcessOpened();
            return Status::Success();
        }
        Result<std::shared_ptr<Target::Process>> process = Target::Process::Open(id);
        if (!process)
        {
            LOG_ERROR("Process", process.Error().Text);
            return process.Error();
        }
        if (m_target.Process())
            CloseProcess();
        m_target.Attach(std::move(process).Value());
        for (const Table::TableSymbol& symbol : m_tables.TableSymbols())
        {
            if (const auto value = Evaluate(symbol.Address))
                m_target.Symbols().Register(symbol.Name, *value, "table");
        }
        m_settings.LastProcess = m_target.Process()->Name();
        LOG_SUCCESS("Process", Format("Opened %s (pid %u, %s)", m_target.Process()->Name().c_str(), Raw(m_target.Process()->Id()), m_target.Process()->Is64Bit() ? "64-bit" : "32-bit"));
        m_lua->OnProcessOpened();
        m_tables.RefreshValues(true);
        return Status::Success();
    }

    Status Engine::OpenProcessByName(std::string_view name)
    {
        const auto id = Target::FindProcessId(name);
        if (!id)
            return Status::Fail(ErrorCode::ProcessNotFound, name);
        return OpenProcess(*id);
    }

    void Engine::CloseProcess()
    {
        if (!m_target.Process())
            return;
        CancelActivations();
        m_activation.WaitForActivation();
        m_work.CancelGroup(kMonoWork);
        m_work.CompleteGroup(kMonoWork);
        m_tracer.Stop();
        m_target.Mono().Stop();
        m_debugger.Detach();
        m_scanner.Reset();
        m_tables.Table().ForEach([](Table::MemoryRecord& record)
            {
                if (record.Kind == Table::RecordKind::Script)
                {
                    record.Active = false;
                    record.ScriptMemory = {};
                }
                record.AddressOk = false;
                record.ValueText = "??";
            });
        LOG_INFO("Process", "Closed " + m_target.Process()->Name());
        m_target.Detach();
        m_codeDissect.reset();
        if (m_lua)
            m_lua->OnProcessClosed();
    }

    void Engine::LoadSymbols(bool windowsSymbols)
    {
        if (m_target.LoadSymbols(windowsSymbols, Now()))
            LOG_INFO("Symbols", windowsSymbols ? "Loading symbols (incl. Windows symbol server)" : "Loading symbols");
    }

    Status Engine::EnsureDebugger()
    {
        if (m_debugger.Attached())
            return Status::Success();
        if (!m_target.Process())
            return Status::Fail(ErrorCode::NoProcess);
        m_debugger.SetLogDebugStrings(m_settings.LogDebugStrings);
        m_debugger.SetPreferHardware(m_settings.UseHardwareExecuteBreakpoints);
        m_debugger.SetBackend(m_settings.UseInProcessDebugger ? Debug::Backend::Veh : Debug::Backend::Windows);
        return m_debugger.Attach(m_target.Process());
    }

    Status Engine::EnsureMono()
    {
        m_work.CompleteGroup(kMonoWork);
        if (m_target.Mono().Present())
            return Status::Success();
        if (!m_target.Process())
            return Status::Fail(ErrorCode::NoProcess);
        const Status started = m_target.Mono().Start(m_target.Process());
        if (started)
            m_target.RefreshSymbolProvider();
        return started;
    }

    Status Engine::ReloadMonoNow()
    {
        m_work.CompleteGroup(kMonoWork);
        if (!m_target.Mono().Present())
            return EnsureMono();
        return m_target.Mono().Refresh();
    }

    void Engine::LoadMono(bool fromScript)
    {
        if (m_target.Mono().Present() || MonoLoading())
            return;
        if (!m_target.Process())
        {
            if (!fromScript)
                Notify(Status::Fail(ErrorCode::NoProcess));
            return;
        }
        const std::shared_ptr<Target::Process> process = m_target.Process();
        const std::uint64_t generation = m_target.Generation();
        m_work.Start({ "Loading the Mono runtime", kMonoWork, true, false }, [this, process, generation, fromScript](WorkProgress& progress) -> BackgroundWork::Finish
            {
                const Status status = m_target.Mono().Start(process, &progress);
                const bool cancelled = progress.Cancelled();
                return [this, status, cancelled, generation, fromScript]() { FinishMono(status, cancelled, true, fromScript, generation); };
            });
    }

    void Engine::ReloadMono(bool fromScript)
    {
        if (MonoLoading())
            return;
        if (!m_target.Mono().Present())
        {
            LoadMono(fromScript);
            return;
        }
        const std::uint64_t generation = m_target.Generation();
        m_work.Start({ "Reloading the Mono classes", kMonoWork, true, false }, [this, generation, fromScript](WorkProgress& progress) -> BackgroundWork::Finish
            {
                const Status status = m_target.Mono().Refresh(&progress);
                const bool cancelled = progress.Cancelled();
                return [this, status, cancelled, generation, fromScript]() { FinishMono(status, cancelled, false, fromScript, generation); };
            });
    }

    void Engine::StopMono(bool fromScript)
    {
        m_work.CancelGroup(kMonoWork);
        m_work.CompleteGroup(kMonoWork);
        if (!m_target.Mono().Present())
            return;
        m_target.Mono().Stop();
        m_target.RefreshSymbolProvider();
        LOG_INFO("Mono", "Shut down the Mono helper in the game");
        if (!fromScript)
            Notify(LogLevel::Success, "Shut down Mono: the helper in the game has ended");
    }

    void Engine::FinishMono(const Status& status, bool cancelled, bool starting, bool fromScript, std::uint64_t generation)
    {
        if (generation != m_target.Generation())
            return;
        if (status)
        {
            if (starting)
                m_target.RefreshSymbolProvider();
            if (!fromScript)
                Notify(LogLevel::Success, Format("%s %zu Mono classes", starting ? "Loaded" : "Reloaded", m_target.Mono().ClassCount()));
            return;
        }
        if (cancelled)
        {
            LOG_INFO("Mono", starting ? "Loading the Mono runtime was cancelled" : "Reloading the Mono classes was cancelled; the earlier list stays");
            return;
        }
        if (fromScript)
            LOG_WARN("Mono", status.Text);
        else
            Notify(status);
    }

    void Engine::LookUpProcessLater(std::string name, std::function<void(std::optional<ProcessId>)> found)
    {
        m_work.Start({ "Looking for " + name, "lookup", false, true }, [name, found = std::move(found)](WorkProgress&) mutable -> BackgroundWork::Finish
            {
                const std::optional<ProcessId> id = Target::FindProcessId(name);
                return [found = std::move(found), id]() { found(id); };
            });
    }

    std::vector<WorkInfo> Engine::BusyWork() const
    {
        std::vector<WorkInfo> list = m_work.List();
        if (m_target.Pdb().Active() && !m_target.Pdb().Loaded())
        {
            WorkInfo symbols;
            symbols.Id = kSymbolsWork;
            symbols.Title = "Loading symbols";
            symbols.Group = "symbols";
            symbols.Done = m_target.Pdb().ModulesDone();
            symbols.Total = m_target.Pdb().ModulesTotal();
            symbols.Fraction = symbols.Total > 0 ? std::clamp(static_cast<double>(symbols.Done) / static_cast<double>(symbols.Total), 0.0, 1.0) : -1.0;
            symbols.Text = symbols.Total > 0 ? Format("Module %lld of %lld", static_cast<long long>(std::min(symbols.Done + 1, symbols.Total)), static_cast<long long>(symbols.Total))
                                             : std::string("Starting the symbol handler");
            symbols.Seconds = Now() - m_target.SymbolsStarted();
            symbols.Cancellable = true;
            symbols.Cancelling = m_target.Pdb().Stopping();
            list.push_back(std::move(symbols));
        }
        return list;
    }

    void Engine::CancelWork(int id)
    {
        if (id == kSymbolsWork)
            m_target.Pdb().Cancel();
        else
            m_work.Cancel(id);
    }

    std::optional<std::uint64_t> Engine::Evaluate(std::string_view expression, Status* error) const
    {
        const Target::Evaluation result = m_target.Symbols().Evaluate(expression);
        if (!result.Ok())
        {
            if (error)
                *error = result.Error;
            return std::nullopt;
        }
        return result.Value;
    }

    void Engine::UpdateAutoAttach()
    {
        if (!m_settings.AutoAttachEnabled || m_settings.AutoAttach.empty())
            return;
        const double now = Now();
        if (now - m_lastAttachCheck < kAttachCheckSeconds)
            return;
        m_lastAttachCheck = now;
        if ((m_target.Process() && m_target.Process()->Alive()) || m_attachLookup)
            return;
        m_attachLookup = true;
        const std::vector<std::string> names = m_settings.AutoAttach;
        m_work.Start({ "Looking for processes to open", "attach", false, true }, [this, names](WorkProgress&) -> BackgroundWork::Finish
            {
                std::optional<ProcessId> id;
                std::string matched;
                for (const std::string& name : names)
                {
                    const std::string trimmed = Trim(name);
                    if (trimmed.empty())
                        continue;
                    if (const auto found = Target::FindProcessId(trimmed))
                    {
                        id = found;
                        matched = trimmed;
                        break;
                    }
                }
                return [this, id, matched]()
                    {
                        m_attachLookup = false;
                        if (!id || !m_settings.AutoAttachEnabled || (m_target.Process() && m_target.Process()->Alive()))
                            return;
                        const Status status = OpenProcess(*id);
                        if (status)
                            Notify(LogLevel::Success, "Auto attached to " + matched);
                        else
                            Notify(status);
                    };
            });
    }

    Asm::AaResult Engine::AutoAssemble(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool checkOnly, const std::string& owner, const Table::MemoryRecord* record)
    {
        Asm::AaHost host;
        host.Process = m_target.Process().get();
        host.Symbols = &m_target.Symbols();
        host.RunLua = [this, record](std::string_view code, bool syntaxCheck, std::string& output) -> Status
            {
                const Lua::ScriptGlobals saved = m_lua->SaveScriptGlobals();
                m_lua->SetScriptGlobals(record, syntaxCheck);
                const Status status = m_lua->RunWithResult(code, "{$lua}", output);
                m_lua->RestoreScriptGlobals(saved);
                return status;
            };
        host.IsCustomCommand = [this](std::string_view name) { return m_lua->IsAutoAssemblerCommand(name); };
        host.RunCustomCommand = [this, record](std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output) -> Status
            {
                const Lua::ScriptGlobals saved = m_lua->SaveScriptGlobals();
                m_lua->SetScriptGlobals(record, syntaxCheck);
                const Status status = m_lua->RunAutoAssemblerCommand(name, parameters, syntaxCheck, output);
                m_lua->RestoreScriptGlobals(saved);
                return status;
            };
        Asm::AaOptions options;
        options.Section = section;
        options.CheckOnly = checkOnly;
        options.Owner = owner;
        if (!m_tables.Table().FilePath.empty())
            options.IncludeFolder = std::filesystem::path(Utf8ToWide(m_tables.Table().FilePath)).parent_path();
        else
            options.IncludeFolder = Paths::AutorunFolder();
        return Asm::RunAutoAssembler(script, host, memory, options);
    }

    Asm::AaResult Engine::AutoAssemble(std::string_view script, bool checkOnly)
    {
        return AutoAssemble(script, Asm::HasSections(script) ? Asm::AaSection::Enable : Asm::AaSection::Whole, m_scratchMemory, checkOnly, "scratch");
    }

    void Engine::Notify(LogLevel level, std::string text)
    {
        switch (level)
        {
        case LogLevel::Error: LOG_ERROR("Notice", text); break;
        case LogLevel::Warning: LOG_WARN("Notice", text); break;
        case LogLevel::Success: LOG_SUCCESS("Notice", text); break;
        default: LOG_INFO("Notice", text); break;
        }
        m_notifications.Post(level, std::move(text), Now());
    }

    void Engine::SaveOptionsSoon()
    {
        m_settingsStore.Changed(Now());
    }

    Status Engine::SaveOptionsNow()
    {
        return m_settingsStore.Flush();
    }

    void Engine::Notify(const Status& status, LogLevel level)
    {
        if (!status.Ok())
            Notify(level, status.Text);
    }

    std::vector<UiRequest> Engine::TakeRequests()
    {
        std::vector<UiRequest> requests;
        requests.swap(m_requests);
        return requests;
    }

    void Engine::Tick()
    {
        MANIFOLD_ASSERT_MAIN();
        if (m_settingsStore.Due(Now()))
            Notify(m_settingsStore.Flush());
        const double now = Now();
        {
            PROFILE_ZONE("Finished background jobs");
            m_work.Drain();
        }
        const int scansBefore = m_scanner.ScanNumber();
        {
            PROFILE_ZONE("Scanner");
            if (m_scanner.Poll())
            {
                if (m_scanner.ScanNumber() != scansBefore)
                    Sounds::Play(Sounds::Cue::Success);
                else if (!m_scanner.LastError().Ok())
                    Sounds::Play(Sounds::Cue::Error);
            }
        }

        if (m_target.Process() && now - m_lastAliveCheck > kAliveCheckSeconds)
        {
            PROFILE_ZONE("Process check");
            m_lastAliveCheck = now;
            if (!m_target.Process()->Alive())
            {
                Notify(LogLevel::Warning, ErrorMessage(ErrorCode::ProcessExited));
                CloseProcess();
            }
        }

        {
            PROFILE_ZONE("Open processes automatically");
            UpdateAutoAttach();
        }
        {
            PROFILE_ZONE("Frozen values");
            m_tables.UpdateFreezes();
        }
        {
            PROFILE_ZONE("Hotkeys");
            m_tables.UpdateHotkeys();
        }
        {
            PROFILE_ZONE("Address list values");
            m_tables.RefreshValues(false);
        }
        {
            PROFILE_ZONE("Lua timers");
            m_lua->Tick(now);
        }
        {
            PROFILE_ZONE("Work for the main thread");
            m_activation.DrainMainWork();
        }
        if (m_activation.Async())
        {
            {
                PROFILE_ZONE("Lua tasks");
                m_lua->RunTasks(kTaskBudgetSeconds);
            }
            {
                PROFILE_ZONE("Work for the main thread");
                m_activation.DrainMainWork();
            }
            PROFILE_ZONE("Script activations");
            m_activation.PumpActivations(kActivationBudgetSeconds);
        }

        if (m_debugger.Attached())
        {
            PROFILE_ZONE("Debugger");
            const Debug::BreakInfo info = m_debugger.Break();
            if (info.Broken && info.Serial != m_lastBreakSerial)
            {
                m_lastBreakSerial = info.Serial;
                if (info.BreakpointAddress && m_lua->HasBreakpointCallback(info.BreakpointAddress))
                {
                    if (m_lua->RunBreakpointCallback(info.BreakpointAddress))
                        m_debugger.Resume(Debug::ResumeMode::Continue);
                    else
                        Request({ UiRequestKind::MemoryViewer, info.Address, {} });
                }
                else
                {
                    Request({ UiRequestKind::MemoryViewer, info.Address, {} });
                }
            }
        }
    }
}
