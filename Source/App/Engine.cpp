#include "App/Engine.h"
#include "Debug/VehProtocol.h"

#include "App/Sounds.h"

#include "Core/Paths.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <cctype>
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
        : m_settings(settings), m_settingsStore(settings, Paths::SettingsFile()), m_themes(Paths::ThemesFolder()), m_disassembler64(std::make_unique<Disasm::Disassembler>(true)), m_disassembler32(std::make_unique<Disasm::Disassembler>(false))
    {
        m_target.Symbols().Rebase().Load(settings.Rebases);
        m_lua = std::make_unique<Lua::LuaEngine>(*this);
        m_unreal.RegisterTypes(*m_lua);
        m_unreal.SetReadyHandler([this]() { m_lua->DispatchEvent("unrealReady"); });
        m_rtti.SetOtherEngines([this](std::uint64_t address) { return UnrealClaims(address) || m_lua->EngineScriptClaims(address); });
        m_debugger.SetLogDebugStrings(settings.LogDebugStrings);
        m_debugger.SetPreferHardware(settings.UseHardwareExecuteBreakpoints);
        m_debugger.SetForeignBreakpoints([this](std::uint64_t address) { return m_coverage.Owns(address); });
        m_target.Mono().SetPauseCheck([this] { return m_debugger.Attached() && m_debugger.Broken(); });
        m_writes.SetGuard({ [this](const Target::Process& process, std::uint64_t address, std::uint8_t* buffer, std::size_t size) { return m_debugger.ReadAround(process, address, buffer, size); },
            [this](const Target::Process& process, std::uint64_t address, std::span<const std::uint8_t> bytes) { return m_debugger.WriteAround(process, address, bytes); } });
    }

    void Engine::SetRebase(std::string_view module, std::optional<Target::RebaseRule> rule)
    {
        Target::Rebases& rebases = m_target.Symbols().Rebase();
        if (rebases.Rule(module) == rule)
            return;
        rebases.Set(module, rule);
        m_settings.Rebases = rebases.Saved();
        SaveOptionsSoon();
    }

    Engine::~Engine()
    {
        m_work.Stop();
        CancelActivations();
        m_activation.WaitForActivation();
        m_tracer.Stop();
        m_codeFilter.Reset();
        m_coverage.Leave();
        m_crashWatch.Leave();
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
        m_coverage.Attach(m_target.Process());
        m_crashLedger.Reset();
        m_crashWatchFailed = false;
        m_crashWatchWanted = false;
        if (m_crashWatch.Attach(m_target.Process()))
            m_crashWatchWanted = true;
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
        m_codeFilter.Reset();
        m_coverage.Leave();
        m_crashWatch.Leave();
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
        m_scriptRegionsChanged = false;
        m_debugger.SetExceptionRegions(mode == 2 && m_settings.BreakInScriptMemory ? ScriptRegions(m_crashLedger.Footprints(), Debug::Veh::kMaxRegions)
                                                                                    : std::vector<std::pair<std::uint64_t, std::uint64_t>>{});
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
        // ∑ With the scripts' memory a region of its own is optional (CE: "automatically add allocated memory as watched regions").
        if (first.empty() && m_settings.BreakInScriptMemory)
        {
            m_debugger.SetExceptionBreak(Debug::ExceptionBreak::InRegion, 0, 0);
            return Status::Success();
        }
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

    Asm::AaResult Engine::AutoAssemble(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool checkOnly, const std::string& owner, const Table::MemoryRecord* record,
        std::string_view logTitle)
    {
        return Assemble(script, section, memory, checkOnly, owner, record, nullptr, false, logTitle);
    }

    ScriptCheck Engine::CheckScript(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool holdLuaBack)
    {
        ScriptCheck check;
        check.Result = Assemble(script, section, memory, true, "check", nullptr, m_tables.ScriptsTrusted() && !holdLuaBack ? nullptr : &check.LuaHeldBack, true);
        return check;
    }

    Asm::AaResult Engine::Assemble(std::string_view script, Asm::AaSection section, Asm::AaMemory& memory, bool checkOnly, const std::string& owner, const Table::MemoryRecord* record,
        int* luaHeldBack, bool scanInCheck, std::string_view logTitle)
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
        options.LogTitle = logTitle.empty() ? AutoAssemblerLogTitle(owner, record) : std::string(logTitle);
        if (!m_tables.Table().FilePath.empty())
            options.IncludeFolder = std::filesystem::path(Utf8ToWide(m_tables.Table().FilePath)).parent_path();
        else
            options.IncludeFolder = Paths::AutorunFolder();
        if (checkOnly || !host.Process)
            return Asm::RunAutoAssembler(script, host, memory, options);
        // ∑ Every run that changes the game notes what it put there, so a
        //   crash can be traced to it; the first one that writes code starts
        //   the crash watch (on the main thread, from Tick).
        const Asm::AaMemory before = memory;
        const std::uint64_t started = ::GetTickCount64();  // a createthread in the script may crash the game before the run returns
        Asm::AaResult result = Asm::RunAutoAssembler(script, host, memory, options);
        if (result.Ok() && (!result.Writes.empty() || memory.Allocations.size() != before.Allocations.size()))
        {
            const std::string label = ScriptLabel(options.LogTitle, owner);
            m_crashLedger.Note(label, recordId, std::hash<std::string_view>{}(script), section, result, before, memory, started);
            for (const Asm::AaWarning& warning : result.Warnings)
                {
                std::string text = warning.Text;
                if (!text.empty())
                    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
                Notify(LogLevel::Warning, "\"" + label + "\": " + text);
            }
            if (section != Asm::AaSection::Disable && !result.Writes.empty() && m_settings.CrashWatch && !Target::Process::DryRun())
                m_crashWatchWanted = true;
            m_scriptRegionsChanged = true;
        }
        return result;
    }

    void Engine::StartCrashWatch()
    {
        if (!m_crashWatchWanted.exchange(false))
            return;
        if (!m_settings.CrashWatch || m_crashWatchFailed || !m_target.Process() || !m_target.Process()->Alive() || m_crashWatch.Running() || Target::Process::DryRun() ||
            m_work.Busy("crashwatch"))
            return;
        // ∑ In the background: loading the agent and the remote call that starts it took
        //   2.4 s in Isaac, and on the main thread every activation queued behind it waited.
        const std::uint64_t generation = ProcessGeneration();
        m_work.Start({ .Title = "Starting the crash watch", .Group = "crashwatch", .Cancellable = false, .Quiet = true }, [this, generation](WorkProgress&) -> BackgroundWork::Finish
            {
                const Status started = m_crashWatch.Start();
                return [this, started, generation]
                    {
                        if (started || ProcessGeneration() != generation)
                            return;
                        m_crashWatchFailed = true;
                        LOG_WARN("Crash", started.Text);
                    };
            });
    }

    bool Engine::ExamineEnd()
    {
        const std::shared_ptr<Target::Process> process = m_target.Process();
        if (!process)
            return false;
        DWORD code = 0;
        if (!::GetExitCodeProcess(process->Handle(), &code) || code == STILL_ACTIVE)
            return false;
        std::uint64_t endTick = ::GetTickCount64();
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        if (::GetProcessTimes(process->Handle(), &created, &exited, &kernel, &user))
        {
            FILETIME now{};
            ::GetSystemTimeAsFileTime(&now);
            const std::uint64_t nowTime = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            const std::uint64_t exitTime = (static_cast<std::uint64_t>(exited.dwHighDateTime) << 32) | exited.dwLowDateTime;
            if (exitTime && exitTime <= nowTime)
            {
                const std::uint64_t ago = (nowTime - exitTime) / 10000;
                endTick = endTick > ago ? endTick - ago : 0;
            }
        }
        const Target::CrashCapture capture = m_crashWatch.Capture();
        if (!capture.Watching && capture.Records.empty() && !ExceptionExitCode(code))
            return false;
        CrashContext context;
        context.ExitCode = code;
        context.EndTick = endTick;
        context.Process = process->Name();
        context.Footprints = m_crashLedger.Footprints();
        // ∑ The modules as the crash watch last saw them name what the process's own list may have lost.
        context.Describe = [this, known = m_crashWatch.Modules()](std::uint64_t address)
            {
                std::string text = Describe(address);
                if (text != Hex(address) || !known)
                    return text;
                for (const Target::ModuleInfo& module : *known)
                {
                    if (address >= module.Base && address < module.Base + module.Size)
                        return module.Name + Format("+%llX", static_cast<unsigned long long>(address - module.Base));
                }
                return text;
            };
        context.Disassembler = &Disassembler();
        CrashReport report = AttributeCrash(capture, context);
        if (!report.Crashed)
            return false;
        RuntimeLog::WriteDetailed(LogLevel::Error, "Crash", __FILE__, __FUNCTION__, __LINE__, report.Headline, report.Details);
        m_notifications.Post(LogLevel::Error, report.Headline, Now());
        {
            std::lock_guard lock(m_crashLock);
            m_lastCrash = std::move(report);
        }
        return true;
    }

    std::optional<CrashReport> Engine::LastCrash() const
    {
        std::lock_guard lock(m_crashLock);
        return m_lastCrash;
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
        const Status theme = m_themes.SaveActive(m_settings.Theme);
        const Status saved = m_settingsStore.Flush();
        return theme.Ok() ? saved : theme;
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
            Notify(SaveOptionsNow());
        if (m_themes.TakeChanged())
            m_lua->DispatchEvent("themeChanged");
        const double now = Now();
        {
            PROFILE_ZONE("Finished background jobs");
            m_work.Drain();
        }
        m_tables.TickSaves(now);
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
            if (m_target.Process()->Alive())
            {
                if (m_crashWatch.Running())
                    m_crashWatch.KeepModules(m_target.Process()->Modules());
            }
            else
            {
                m_managed.NoteExplorer(true);
                if (!ExamineEnd())
                    Notify(LogLevel::Warning, ErrorMessage(ErrorCode::ProcessExited));
                CloseProcess();
            }
        }
        if (m_crashWatchWanted && m_target.Process())
        {
            PROFILE_ZONE("Crash watch");
            StartCrashWatch();
        }
        if (m_scriptRegionsChanged && m_debugger.Attached() && m_settings.BreakOnException == 2 && m_settings.BreakInScriptMemory)
            ApplyExceptionBreak();

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
                if (info.Exception.Raised)
                {
                    if (const auto script = ScriptAt(m_crashLedger.Footprints(), info.Address, ::GetTickCount64(), [this](std::uint64_t address) { return Describe(address); }))
                        Notify(LogLevel::Warning, Format("Stopped in the code of \"%s\" (%s): %s. The game waits in the debugger; without it, this would likely have crashed the game.",
                                                      script->Label.c_str(), script->Where.c_str(), Debug::DescribeException(info.Exception).c_str()));
                }
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
