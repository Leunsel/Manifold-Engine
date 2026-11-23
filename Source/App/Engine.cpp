#include "App/Engine.h"

#include "App/Sounds.h"

#include "Core/Paths.h"
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
        m_unreal.RegisterTypes(*m_lua);
        m_unreal.SetReadyHandler([this]() { m_lua->DispatchEvent("unrealReady"); });
        m_rtti.SetOtherEngines([this](std::uint64_t address) { return UnrealClaims(address) || m_lua->EngineScriptClaims(address); });
        m_debugger.SetLogDebugStrings(settings.LogDebugStrings);
        m_debugger.SetPreferHardware(settings.UseHardwareExecuteBreakpoints);
        m_target.Mono().SetPauseCheck([this] { return m_debugger.Attached() && m_debugger.Broken(); });
        m_writes.SetGuard({ [this](const Target::Process& process, std::uint64_t address, std::uint8_t* buffer, std::size_t size) { return m_debugger.ReadAround(process, address, buffer, size); },
            [this](const Target::Process& process, std::uint64_t address, std::span<const std::uint8_t> bytes) { return m_debugger.WriteAround(process, address, bytes); } });
    }

    Engine::~Engine()
    {
        m_work.Stop();
        CancelActivations();
        m_activation.WaitForActivation();
        m_tracer.Stop();
        m_debugger.Detach();
        m_speedhack.Leave();
        m_target.Mono().Stop();
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

    std::optional<Disasm::FunctionSpan> Engine::FunctionAround(std::uint64_t address)
    {
        constexpr std::uint64_t kReach = 0x10000;
        if (!HasProcess() || address == 0)
            return std::nullopt;
        const Target::CodeMap* map = CodeMap();
        const bool managed = MonoPresent();
        const std::shared_ptr<const Disasm::DissectResult> dissect = m_codeDissect;
        Disasm::FunctionClues clues;
        clues.Exact = [&](std::uint64_t at) -> std::optional<Disasm::FunctionSpan>
            {
                if (map)
                {
                    if (const auto function = map->FunctionAt(at))
                        return Disasm::FunctionSpan{ function->first, function->second, 0, Disasm::FunctionSource::Unwind };
                }
                if (managed)
                {
                    if (const auto method = Mono().MethodRange(at))
                        return Disasm::FunctionSpan{ method->first, method->second, 0, Disasm::FunctionSource::Managed };
                }
                return std::nullopt;
            };
        clues.KnownStart = [&](std::uint64_t at)
        {
            if (Symbols().ExactName(at) && !Symbols().LabelOnly(at))
                return true;
            if (!dissect)
                return false;
            const std::span<const Disasm::CodeRef> references = dissect->To(at);
            return std::any_of(references.begin(), references.end(), [](const Disasm::CodeRef& reference)
                {
                    return reference.Kind() == Disasm::RefKind::Call;
                });
        };
        clues.Floor = address > kReach ? address - kReach : 0;
        clues.Ceiling = address + kReach;
        if (const auto module = Process()->ModuleAt(address))
        {
            clues.Floor = std::max(clues.Floor, module->Base);
            clues.Ceiling = std::min(clues.Ceiling, module->Base + module->Size);
        }
        if (map)
        {
            if (const auto section = map->SectionAt(address); section && !section->Code)
            {
                if (!clues.Exact(address))
                    return std::nullopt;
            }
            else if (section)
            {
                if (section->Begin >= clues.Floor)
                {
                    clues.Floor = section->Begin;
                    clues.FloorIsBoundary = true;
                }
                clues.Ceiling = std::min(clues.Ceiling, section->End);
            }
            if (const auto below = map->StartBelow(address + 1, kReach, Target::CodeMap::Anchors::Code); below && *below >= clues.Floor)
            {
                clues.Floor = *below;
                clues.FloorIsBoundary = true;
            }
            if (const auto above = map->BoundaryIn(address, address + kReach, Target::CodeMap::Anchors::Code))
                clues.Ceiling = std::min(clues.Ceiling, *above);
        }
        std::optional<Disasm::FunctionSpan> span = Disasm::FindFunction(Disassembler(), *Process(), address, map, clues);
        if (span && span->Last == 0)
            span->Last = Disassembler().BackUp(*Process(), span->End, 1, map);
        return span;
    }

    std::uint64_t Engine::SymbolEnd(std::uint64_t start)
    {
        constexpr std::uint64_t kReach = 0x1000;
        if (!HasProcess() || start == 0)
            return start;
        const Target::Process& process = *Process();
        const Target::CodeMap* map = CodeMap();
        if (Disassembler().InlineHolder(process, start))
            return start;
        const auto ownRow = [&]
        {
            const std::vector<Disasm::Instruction> row = Disassembler().ReadMany(process, start, 1, nullptr, map);
            return row.empty() ? start : row.front().Next();
        };
        if (Symbols().LabelOnly(start))
        {
            if (map)
            {
                if (const auto function = map->FunctionAt(start); function && function->first == start)
                    return function->second;
            }
            if (MonoPresent())
            {
                if (const auto method = Mono().MethodRange(start); method && method->first == start)
                    return method->second;
            }
            return ownRow();
        }
        if (const std::uint64_t size = Symbols().RegisteredSize(start))
            return std::max(ownRow(), start + size);
        if (map)
        {
            if (const auto function = map->FunctionAt(start))
                return function->second;
            if (const auto section = map->SectionAt(start); section && !section->Code)
                return ownRow();
        }
        if (MonoPresent())
        {
            if (const auto method = Mono().MethodRange(start))
                return method->second;
        }
        return Disasm::CodeExtent(Disassembler(), process, start, start + kReach, map);
    }

    bool Engine::FunctionStartAt(std::uint64_t address)
    {
        if (!HasProcess())
            return false;
        if (const Target::CodeMap* map = CodeMap())
        {
            if (const auto function = map->FunctionAt(address); function && function->first == address)
                return true;
        }
        if (MonoPresent())
        {
            if (const auto method = Mono().MethodRange(address); method && method->first == address)
                return true;
        }
        return Symbols().ExactName(address) && !Symbols().LabelOnly(address);
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
        m_speedhack.Attach(m_target.Process());
        m_rtti.ProcessOpened();
        m_lua->OnProcessOpened();
        m_unreal.ProcessOpened();
        m_tables.RefreshValues(true);
        return Status::Success();
    }

    Status Engine::RunAutorun()
    {
        return m_lua->RunAutorun(Paths::AutorunFolder(), { Paths::ModulesStateFile(), m_settings.HoldNewModules });
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
        m_managed.CancelWork();
        m_tracer.Stop();
        m_debugger.Detach();
        m_speedhack.Leave();
        m_unreal.ProcessClosed();
        m_rtti.ProcessClosed();
        m_managed.NoteExplorer(false);
        m_target.Mono().Stop();
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
        ApplyExceptionBreak();
        Status attached = m_debugger.Attach(m_target.Process());
        if (attached)
            ApplyExceptionBreak();
        return attached;
    }

    Status Engine::ApplyExceptionBreak()
    {
        const int mode = std::clamp(m_settings.BreakOnException, 0, 2);
        if (mode != 2)
        {
            m_debugger.SetExceptionBreak(mode == 1 ? Debug::ExceptionBreak::Always : Debug::ExceptionBreak::Never, 0, 0);
            return Status::Success();
        }
        std::uint64_t start = 0;
        std::uint64_t stop = 0;
        Status status = Status::Success();
        const std::string first = Trim(m_settings.ExceptionRegionStart);
        const std::string last = Trim(m_settings.ExceptionRegionStop);
        if (first.empty())
            status = Status::Fail(ErrorCode::DebugBadExpression, "the region has no start");
        if (status && last.empty() && m_target.Process())
        {
            if (const auto module = m_target.Process()->FindModule(first))
            {
                start = module->Base;
                stop = module->Base + module->Size;
            }
        }
        if (status && !stop)
        {
            if (const auto value = Evaluate(first, &status))
                start = *value;
            if (status && !last.empty())
            {
                if (const auto value = Evaluate(last, &status))
                    stop = *value;
            }
            else if (status)
                status = Status::Fail(ErrorCode::DebugBadExpression, "give the end of the region, or a module name as its start");
        }
        if (status && stop <= start)
            status = Status::Fail(ErrorCode::DebugBadExpression, "the region ends before it starts");
        if (!status)
        {
            m_debugger.SetExceptionBreak(Debug::ExceptionBreak::Never, 0, 0);
            return status;
        }
        m_debugger.SetExceptionBreak(Debug::ExceptionBreak::InRegion, start, stop);
        return Status::Success();
    }

    void Engine::LookUpProcessLater(std::string name, std::function<void(std::optional<ProcessId>)> found)
    {
        m_work.Start({ .Title = "Looking for " + name, .Group = "lookup", .Cancellable = false, .Quiet = true }, [name, found = std::move(found)](WorkProgress&) mutable -> BackgroundWork::Finish
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
        m_work.Start({ .Title = "Looking for processes to open", .Group = "attach", .Cancellable = false, .Quiet = true }, [this, names](WorkProgress&) -> BackgroundWork::Finish
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
        return Assemble(script, section, memory, checkOnly, owner, record, nullptr);
    }

    ScriptCheck Engine::CheckScript(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool holdLuaBack)
    {
        ScriptCheck check;
        check.Result = Assemble(script, section, memory, true, "check", nullptr, m_tables.ScriptsTrusted() && !holdLuaBack ? nullptr : &check.LuaHeldBack, true);
        return check;
    }

    Asm::AaResult Engine::Assemble(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool checkOnly, const std::string& owner, const Table::MemoryRecord* record,
        int* luaHeldBack, bool scanInCheck)
    {
        Asm::AaHost host;
        host.Process = m_target.Process().get();
        host.Symbols = &m_target.Symbols();
        // ∑ The record by its id and table, found again for every block: a {$lua} block may delete it (or replace the table),
        //   and a later block of the same script would then read freed memory through the pointer.
        const bool forRecord = record != nullptr;
        const int recordId = record ? record->Id : 0;
        const std::uint64_t identity = m_tables.Table().Identity();
        const auto currentRecord = [this, forRecord, recordId, identity]() -> const Table::MemoryRecord*
            {
                if (!forRecord || m_tables.Table().Identity() != identity)
                    return nullptr;
                return m_tables.Table().FindById(recordId);
            };
        host.RunLua = [this, currentRecord, luaHeldBack](std::string_view code, bool syntaxCheck, std::string& output) -> Status
            {
                if (luaHeldBack)
                {
                    ++*luaHeldBack;
                    output.clear();
                    return m_lua->Compile(code, "{$lua}");
                }
                const Lua::ScriptGlobals saved = m_lua->SaveScriptGlobals();
                m_lua->SetScriptGlobals(currentRecord(), syntaxCheck);
                const Status status = m_lua->RunWithResult(code, "{$lua}", output);
                m_lua->RestoreScriptGlobals(saved);
                return status;
            };
        host.IsCustomCommand = [this](std::string_view name) { return m_lua->IsAutoAssemblerCommand(name); };
        host.RunCustomCommand = [this, currentRecord, luaHeldBack](std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output) -> Status
            {
                if (luaHeldBack)
                {
                    ++*luaHeldBack;
                    output.clear();
                    return Status::Success();
                }
                const Lua::ScriptGlobals saved = m_lua->SaveScriptGlobals();
                m_lua->SetScriptGlobals(currentRecord(), syntaxCheck);
                const Status status = m_lua->RunAutoAssemblerCommand(name, parameters, syntaxCheck, output);
                m_lua->RestoreScriptGlobals(saved);
                return status;
            };
        Asm::AaOptions options;
        options.Section = section;
        options.CheckOnly = checkOnly;
        options.ScanInCheck = scanInCheck;
        options.Owner = owner;
        options.Log = m_settings.AutoAssemblerLog;
        options.LogTitle = AutoAssemblerLogTitle(owner, record);
        if (!m_tables.Table().FilePath.empty())
            options.IncludeFolder = std::filesystem::path(Utf8ToWide(m_tables.Table().FilePath)).parent_path();
        else
            options.IncludeFolder = Paths::AutorunFolder();
        return Asm::RunAutoAssembler(script, host, memory, options);
    }

    std::string Engine::AutoAssemblerLogTitle(std::string_view owner, const Table::MemoryRecord* record)
    {
        if (record)
            return "\"" + record->Description + "\"";
        if (owner == "window")
            return "Auto Assembler window";
        if (owner == "lua" || owner == "scratch")
            return "autoAssemble";
        return {};
    }

    Asm::AaResult Engine::AutoAssemble(std::string_view script, bool checkOnly)
    {
        return AutoAssemble(script, Asm::HasSections(script) ? Asm::AaSection::Enable : Asm::AaSection::Whole, m_scratchMemory, checkOnly, "scratch");
    }

    void Engine::RequestActive(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        if (!fromScript)
            m_tables.NoteActivationByHand(record);
        m_activation.RequestActive(record, active, fromScript);
    }

    bool Engine::UnrealClaims(std::uint64_t address) const
    {
        const auto runtime = m_unreal.Runtime();
        return runtime && (m_unreal.ArrayAt(address).has_value() || runtime->OwnerOf(address).has_value());
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
        m_backup.Tick(now);
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
                m_managed.NoteExplorer(true);
                Notify(LogLevel::Warning, ErrorMessage(ErrorCode::ProcessExited));
                CloseProcess();
            }
        }

        {
            PROFILE_ZONE("Open processes automatically");
            UpdateAutoAttach();
        }
        m_unreal.Tick();
        {
            PROFILE_ZONE("Frozen values");
            m_tables.UpdateFreezes();
        }
        {
            PROFILE_ZONE("Hotkeys");
            Table::BeginHotkeyFrame();
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
