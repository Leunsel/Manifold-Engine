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
        constexpr std::size_t kMaxNotices = 6;
        constexpr double kAliveCheckSeconds = 1.0;
        constexpr double kAttachCheckSeconds = 1.5;

        constexpr double kTaskBudgetSeconds = 0.010;
        constexpr double kActivationBudgetSeconds = 0.006;

        thread_local bool t_activationWorker = false;

        std::string OwnerOf(const Table::MemoryRecord& record)
        {
            return "record:" + std::to_string(record.Id);
        }
    }

    struct Engine::RunningActivation
    {
        int RecordId = 0;
        bool Active = false;
        bool FromScript = false;
        Asm::AaMemory Memory;
        std::shared_ptr<Target::Process> Process;
        std::future<Asm::AaResult> Result;
    };

    Engine::Engine(Settings& settings)
        : m_settings(settings), m_disassembler64(std::make_unique<Disasm::Disassembler>(true)), m_disassembler32(std::make_unique<Disasm::Disassembler>(false))
    {
        m_lua = std::make_unique<Lua::LuaEngine>(*this);
        m_debugger.SetLogDebugStrings(settings.LogDebugStrings);
        m_debugger.SetPreferHardware(settings.UseHardwareExecuteBreakpoints);
        m_mono.SetPauseCheck([this] { return m_debugger.Attached() && m_debugger.Broken(); });
    }

    Engine::~Engine()
    {
        m_jobs.Stop();
        m_work.Stop();
        CancelActivations();
        WaitForActivation();
        m_tracer.Stop();
        m_mono.Stop();
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
        return (!m_process || m_process->Is64Bit()) ? *m_disassembler64 : *m_disassembler32;
    }

    Status Engine::OpenProcess(std::uint32_t id)
    {
        WaitForActivation();
        if (m_process && m_process->Id() == id && m_process->Alive())
        {
            m_process->RefreshModules();
            m_symbols.Attach(m_process);
            m_codeMap.Attach(m_process, &m_mono);
            LOG_INFO("Process", Format("%s (pid %u) is already open; scripts and symbols stay as they are", m_process->Name().c_str(), id));
            m_lua->OnProcessOpened();
            return Status::Success();
        }
        Status status;
        std::shared_ptr<Target::Process> process = Target::Process::Open(id, status);
        if (!process)
        {
            LOG_ERROR("Process", status.Text);
            return status;
        }
        if (m_process)
            CloseProcess();
        m_process = std::move(process);
        m_symbols.Attach(m_process);
        m_codeMap.Attach(m_process, &m_mono);
        for (const Table::TableSymbol& symbol : m_tableSymbols)
        {
            if (const auto value = Evaluate(symbol.Address))
                m_symbols.Register(symbol.Name, *value, "table");
        }
        ++m_processGeneration;
        m_settings.LastProcess = m_process->Name();
        LOG_SUCCESS("Process", Format("Opened %s (pid %u, %s)", m_process->Name().c_str(), m_process->Id(), m_process->Is64Bit() ? "64-bit" : "32-bit"));
        m_lua->OnProcessOpened();
        RefreshValues(true);
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
        if (!m_process)
            return;
        CancelActivations();
        WaitForActivation();
        m_work.CancelGroup(kMonoWork);
        m_work.CompleteGroup(kMonoWork);
        m_tracer.Stop();
        m_mono.Stop();
        m_debugger.Detach();
        m_scanner.Reset();
        m_table.ForEach([](Table::MemoryRecord& record)
            {
                if (record.Kind == Table::RecordKind::Script)
                {
                    record.Active = false;
                    record.ScriptMemory = {};
                }
                record.AddressOk = false;
                record.ValueText = "??";
            });
        LOG_INFO("Process", "Closed " + m_process->Name());
        m_mono.Stop();
        m_pdbActive = false;
        m_symbols.SetSymbolProvider({}, {});
        m_pdbSymbols.End();
        m_symbols.Detach();
        m_codeMap.Detach();
        m_process.reset();
        m_codeDissect.reset();
        ++m_processGeneration;
        if (m_lua)
            m_lua->OnProcessClosed();
    }

    void Engine::LoadSymbols(bool windowsSymbols)
    {
        if (!m_process)
            return;
        m_process->RefreshModules();
        std::vector<Target::ModuleInfo> list;
        if (const auto modules = m_process->Modules())
            list.assign(modules->begin(), modules->end());
        if (!m_pdbSymbols.Begin(m_process->Handle(), std::move(list), windowsSymbols))
            return;
        m_symbolsStarted = Now();
        m_pdbActive = true;
        RefreshSymbolProvider();
        LOG_INFO("Symbols", windowsSymbols ? "Loading symbols (incl. Windows symbol server)" : "Loading symbols");
    }

    Status Engine::EnsureDebugger()
    {
        if (m_debugger.Attached())
            return Status::Success();
        if (!m_process)
            return Status::Fail(ErrorCode::NoProcess);
        m_debugger.SetLogDebugStrings(m_settings.LogDebugStrings);
        m_debugger.SetPreferHardware(m_settings.UseHardwareExecuteBreakpoints);
        m_debugger.SetBackend(m_settings.UseInProcessDebugger ? Debug::Backend::Veh : Debug::Backend::Windows);
        return m_debugger.Attach(m_process);
    }

    Status Engine::EnsureMono()
    {
        m_work.CompleteGroup(kMonoWork);
        if (m_mono.Present())
            return Status::Success();
        if (!m_process)
            return Status::Fail(ErrorCode::NoProcess);
        const Status started = m_mono.Start(m_process);
        if (started)
            RefreshSymbolProvider();
        return started;
    }

    Status Engine::ReloadMonoNow()
    {
        m_work.CompleteGroup(kMonoWork);
        if (!m_mono.Present())
            return EnsureMono();
        return m_mono.Refresh();
    }

    void Engine::LoadMono(bool fromScript)
    {
        if (m_mono.Present() || MonoLoading())
            return;
        if (!m_process)
        {
            if (!fromScript)
                Notify(Status::Fail(ErrorCode::NoProcess));
            return;
        }
        const std::shared_ptr<Target::Process> process = m_process;
        const std::uint64_t generation = m_processGeneration;
        const bool il2cpp = Target::MonoBridge::DetectRuntime(*process) == Target::ManagedRuntime::Il2Cpp;
        m_work.Start({ il2cpp ? "Loading the IL2CPP runtime" : "Loading the Mono runtime", kMonoWork, true, false }, [this, process, generation, fromScript](WorkProgress& progress) -> BackgroundWork::Finish
            {
                const Status status = m_mono.Start(process, &progress);
                const bool cancelled = progress.Cancelled();
                return [this, status, cancelled, generation, fromScript]() { FinishMono(status, cancelled, true, fromScript, generation); };
            });
    }

    void Engine::ReloadMono(bool fromScript)
    {
        if (MonoLoading())
            return;
        if (!m_mono.Present())
        {
            LoadMono(fromScript);
            return;
        }
        const std::uint64_t generation = m_processGeneration;
        m_work.Start({ m_mono.IsIl2Cpp() ? "Reloading the IL2CPP classes" : "Reloading the Mono classes", kMonoWork, true, false }, [this, generation, fromScript](WorkProgress& progress) -> BackgroundWork::Finish
            {
                const Status status = m_mono.Refresh(&progress);
                const bool cancelled = progress.Cancelled();
                return [this, status, cancelled, generation, fromScript]() { FinishMono(status, cancelled, false, fromScript, generation); };
            });
    }

    void Engine::StopMono(bool fromScript)
    {
        m_work.CancelGroup(kMonoWork);
        m_work.CompleteGroup(kMonoWork);
        if (!m_mono.Present())
            return;
        const std::string runtime = Target::MonoBridge::RuntimeName(m_mono.Runtime());
        m_mono.Stop();
        RefreshSymbolProvider();
        LOG_INFO("Mono", "Shut down the " + runtime + " helper in the game");
        if (!fromScript)
            Notify(LogLevel::Success, "Shut down " + runtime + ": the helper in the game has ended");
    }

    void Engine::FinishMono(const Status& status, bool cancelled, bool starting, bool fromScript, std::uint64_t generation)
    {
        if (generation != m_processGeneration)
            return;
        if (status)
        {
            if (starting)
                RefreshSymbolProvider();
            if (!fromScript)
                Notify(LogLevel::Success, Format("%s %zu %s classes", starting ? "Loaded" : "Reloaded", m_mono.ClassCount(), Target::MonoBridge::RuntimeName(m_mono.Runtime())));
            return;
        }
        if (cancelled)
        {
            LOG_INFO("Mono", starting ? "Loading the managed runtime was cancelled" : "Reloading the classes was cancelled; the earlier list stays");
            return;
        }
        if (fromScript)
            LOG_WARN("Mono", status.Text);
        else
            Notify(status);
    }

    void Engine::LookUpProcessLater(std::string name, std::function<void(std::uint32_t)> found)
    {
        m_work.Start({ "Looking for " + name, "lookup", false, true }, [name, found = std::move(found)](WorkProgress&) mutable -> BackgroundWork::Finish
            {
                const std::uint32_t id = Target::FindProcessId(name).value_or(0);
                return [found = std::move(found), id]() { found(id); };
            });
    }

    std::vector<WorkInfo> Engine::BusyWork() const
    {
        std::vector<WorkInfo> list = m_work.List();
        if (m_pdbSymbols.Active() && !m_pdbSymbols.Loaded())
        {
            WorkInfo symbols;
            symbols.Id = kSymbolsWork;
            symbols.Title = "Loading symbols";
            symbols.Group = "symbols";
            symbols.Done = m_pdbSymbols.ModulesDone();
            symbols.Total = m_pdbSymbols.ModulesTotal();
            symbols.Fraction = symbols.Total > 0 ? std::clamp(static_cast<double>(symbols.Done) / static_cast<double>(symbols.Total), 0.0, 1.0) : -1.0;
            symbols.Text = symbols.Total > 0 ? Format("Module %lld of %lld", static_cast<long long>(std::min(symbols.Done + 1, symbols.Total)), static_cast<long long>(symbols.Total))
                                             : std::string("Starting the symbol handler");
            symbols.Seconds = Now() - m_symbolsStarted;
            symbols.Cancellable = true;
            symbols.Cancelling = m_pdbSymbols.Stopping();
            list.push_back(std::move(symbols));
        }
        return list;
    }

    void Engine::CancelWork(int id)
    {
        if (id == kSymbolsWork)
            m_pdbSymbols.Cancel();
        else
            m_work.Cancel(id);
    }

    void Engine::RefreshSymbolProvider()
    {
        const bool pdb = m_pdbActive;
        if (!pdb && !m_mono.Present())
        {
            m_symbols.SetSymbolProvider({}, {});
            return;
        }
        m_symbols.SetSymbolProvider(
            [this, pdb](std::string_view name) -> std::optional<std::uint64_t>
            {
                if (pdb)
                    if (const auto address = m_pdbSymbols.Resolve(name))
                        return address;
                if (m_mono.Present())
                    if (const auto address = m_mono.AddressOf(name))
                        return address;
                return std::nullopt;
            },
            [this, pdb](std::uint64_t address) -> std::optional<std::string>
            {
                if (pdb)
                    if (const auto name = m_pdbSymbols.Describe(address))
                        return name;
                // JIT-compiled Mono methods live outside every loaded PE module
                // (the runtime allocates its own code pages), so only for an
                // address in no module is a Mono lookup worth the remote-call
                // cost; the common in-module path never touches the runtime, and
                // MethodNameAt caches so repeat lines are free. IL2CPP methods
                // are GameAssembly.dll's own code and are looked up in the
                // bridge's sorted table, without a remote call for the range.
                if (MonoPresent() && m_process && (m_mono.IsIl2Cpp() || !m_process->ModuleAt(address)))
                {
                    if (const auto name = m_mono.MethodNameAt(address))
                    {
                        // Inside a method the offset stays, so the text
                        // evaluates back to the same address.
                        if (const auto range = m_mono.MethodRange(address); range && range->first < address)
                            return *name + "+" + Hex(address - range->first);
                        return name;
                    }
                }
                return std::nullopt;
            });
    }

    std::optional<std::uint64_t> Engine::Evaluate(std::string_view expression, Status* error) const
    {
        const Target::Evaluation result = m_symbols.Evaluate(expression);
        if (!result.Ok())
        {
            if (error)
                *error = result.Error;
            return std::nullopt;
        }
        return result.Value;
    }

    void Engine::ResolveRecord(Table::MemoryRecord& record)
    {
        record.AddressOk = false;
        const bool addressHeader = record.Kind == Table::RecordKind::Group && record.AddressGroupHeader;
        if ((record.Kind != Table::RecordKind::Value && !addressHeader) || !m_process)
            return;
        Status error;
        std::optional<std::uint64_t> base;
        const std::string_view written = TrimView(record.Address);
        if (!written.empty() && (written.front() == '+' || written.front() == '-') && record.Parent)
        {
            ResolveRecord(*record.Parent);
            if (!record.Parent->AddressOk)
            {
                record.AddressError = record.Parent->AddressError.empty() ? ErrorMessage(ErrorCode::ExprUnknownSymbol, record.Parent->Description) : record.Parent->AddressError;
                return;
            }
            const auto delta = Evaluate(written.substr(1), &error);
            if (delta)
                base = written.front() == '+' ? record.Parent->ResolvedAddress + *delta : record.Parent->ResolvedAddress - *delta;
        }
        else
        {
            base = Evaluate(record.Address, &error);
        }
        if (!base)
        {
            record.AddressError = error.Text;
            return;
        }
        std::uint64_t address = *base;
        for (const std::string& offsetText : record.Offsets)
        {
            const auto pointer = m_process->ReadPointer(address);
            if (!pointer)
            {
                record.AddressError = ErrorMessage(ErrorCode::ExprPointerUnreadable, Hex(address));
                return;
            }
            Status offsetError;
            const auto offset = Evaluate(offsetText.empty() ? "0" : offsetText, &offsetError);
            if (!offset)
            {
                record.AddressError = offsetError.Text;
                return;
            }
            address = *pointer + *offset;
        }
        record.ResolvedAddress = address;
        record.AddressOk = true;
        record.AddressError.clear();
    }

    std::vector<std::uint8_t> Engine::ReadRecord(Table::MemoryRecord& record)
    {
        ResolveRecord(record);
        if (!record.AddressOk)
            return {};
        const Lua::CustomType* custom = record.CustomTypeName.empty() ? nullptr : m_lua->FindCustomType(record.CustomTypeName);
        const int size = custom ? custom->ByteCount() : record.Size(m_process->PointerSize());
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(1, size)));
        if (!m_process->ReadExact(record.ResolvedAddress, bytes.data(), bytes.size()))
            return {};
        return bytes;
    }

    std::string Engine::FormatRecord(const Table::MemoryRecord& record, std::span<const std::uint8_t> bytes) const
    {
        if (bytes.empty())
            return "??";
        const int pointerSize = m_process ? m_process->PointerSize() : 8;
        std::string text;
        const Lua::CustomType* custom = record.CustomTypeName.empty() ? nullptr : m_lua->FindCustomType(record.CustomTypeName);
        if (custom)
        {
            if (!m_lua->CustomToText(*custom, bytes, record.ResolvedAddress, text))
                return "??";
            for (const Table::DropDownEntry& entry : DropDownOf(record))
            {
                if (EqualsNoCase(entry.Value, text))
                    return record.DropDownDescriptionOnly ? entry.Text : text + " : " + entry.Text;
            }
            return text;
        }
        text = Scan::FormatValue(record.Type, bytes, record.Format(pointerSize));
        if (record.Type == Scan::ValueType::Pointer && m_process)
        {
            std::uint64_t pointer = 0;
            std::memcpy(&pointer, bytes.data(), std::min<std::size_t>(bytes.size(), static_cast<std::size_t>(pointerSize)));
            const std::string described = m_symbols.Describe(pointer);
            if (described != Hex(pointer))
                text = "P->" + described;
        }
        for (const Table::DropDownEntry& entry : DropDownOf(record))
        {
            if (EqualsNoCase(entry.Value, text))
                return record.DropDownDescriptionOnly ? entry.Text : text + " : " + entry.Text;
        }
        return text;
    }

    const std::vector<Table::DropDownEntry>& Engine::DropDownOf(const Table::MemoryRecord& record) const
    {
        if (record.DropDownLink.empty())
            return record.DropDown;
        std::string link = Trim(record.DropDownLink);
        if (link.size() >= 2 && link.front() == '"' && link.back() == '"')
            link = link.substr(1, link.size() - 2);
        if (Table::MemoryRecord* linked = const_cast<Table::AddressList&>(m_table).FindByDescription(link); linked && linked != &record)
            return linked->DropDown;
        return record.DropDown;
    }

    Status Engine::WriteRecord(Table::MemoryRecord& record, std::string_view text, bool recursive)
    {
        if (record.Kind == Table::RecordKind::Value)
        {
            if (!m_process)
                return Status::Fail(ErrorCode::NoProcess);
            ResolveRecord(record);
            if (!record.AddressOk)
                return Status::Fail(ErrorCode::ExprSyntax, record.AddressError.empty() ? record.Address : record.AddressError);
            std::string valueText(TrimView(text));
            for (const Table::DropDownEntry& entry : DropDownOf(record))
            {
                if (EqualsNoCase(entry.Text, valueText))
                    valueText = entry.Value;
            }
            Scan::ValueFormat format = record.Format(m_process->PointerSize());
            const Lua::CustomType* custom = record.CustomTypeName.empty() ? nullptr : m_lua->FindCustomType(record.CustomTypeName);
            if (custom)
            {
                std::vector<std::uint8_t> converted;
                if (!m_lua->CustomFromText(*custom, valueText, record.ResolvedAddress, converted))
                    return Status::Fail(ErrorCode::ScanBadValue, valueText);
                const Status written = m_process->Write(record.ResolvedAddress, converted);
                if (!written)
                    return written;
                if (record.Active)
                    record.FrozenBytes = converted;
                return Status::Success();
            }
            auto bytes = Scan::ParseValue(record.Type, valueText, format);
            if (!bytes)
                return Status::Fail(ErrorCode::ScanBadValue, valueText);
            if (record.Type == Scan::ValueType::String)
            {
                if (record.ZeroTerminate)
                {
                    bytes->push_back(0);
                    if (record.Unicode)
                        bytes->push_back(0);
                }
            }
            if (record.Type == Scan::ValueType::Binary)
            {
                const int size = record.Size(m_process->PointerSize());
                std::vector<std::uint8_t> current(static_cast<std::size_t>(size));
                if (!m_process->ReadExact(record.ResolvedAddress, current.data(), current.size()))
                    return Status::Fail(ErrorCode::ReadFailed, Hex(record.ResolvedAddress));
                std::uint64_t whole = 0;
                std::memcpy(&whole, current.data(), std::min<std::size_t>(8, current.size()));
                std::uint64_t value = 0;
                std::memcpy(&value, bytes->data(), 8);
                const std::uint64_t mask = (record.BitLength >= 64 ? ~0ull : ((1ull << record.BitLength) - 1)) << record.BitStart;
                whole = (whole & ~mask) | ((value << record.BitStart) & mask);
                bytes->assign(current.size(), 0);
                std::memcpy(bytes->data(), &whole, std::min<std::size_t>(8, bytes->size()));
            }
            const Status written = m_process->Write(record.ResolvedAddress, *bytes);
            if (!written)
                return written;
            if (record.Active)
                record.FrozenBytes = *bytes;
            LOG_DEBUG("Table", "Wrote " + valueText + " to " + record.Description);
        }
        if (recursive && record.Options.RecursiveSetValue)
        {
            for (const auto& child : record.Children)
            {
                const Status status = WriteRecord(*child, text, true);
                if (!status && child->Kind == Table::RecordKind::Value)
                    return status;
            }
        }
        return Status::Success();
    }

    Status Engine::AddToRecord(Table::MemoryRecord& record, double delta)
    {
        if (record.Kind != Table::RecordKind::Value || !m_process)
            return Status::Fail(ErrorCode::NoProcess);
        const std::vector<std::uint8_t> bytes = ReadRecord(record);
        if (bytes.empty())
            return Status::Fail(ErrorCode::ReadFailed, Hex(record.ResolvedAddress));
        const auto value = Scan::AsDouble(record.Type, bytes, record.ShowSigned);
        if (!value)
            return Status::Fail(ErrorCode::ScanBadValue, record.Description);
        const std::vector<std::uint8_t> next = Scan::FromDouble(record.Type, *value + delta, m_process->PointerSize());
        const Status written = m_process->Write(record.ResolvedAddress, next);
        if (written && record.Active)
            record.FrozenBytes = next;
        return written;
    }

    Status Engine::ActivateTree(Table::MemoryRecord& record, bool active)
    {
        if (record.Active == active)
            return Status::Success();
        if (!m_lua->BeforeActivation(record, active))
            return Status::Success();
        if (!active && record.Options.DeactivateChildren)
        {
            for (auto it = record.Children.rbegin(); it != record.Children.rend(); ++it)
            {
                const Status status = ActivateTree(**it, false);
                if (!status)
                    Notify(status);
            }
        }
        if (record.Kind == Table::RecordKind::Script)
        {
            const Asm::AaResult result = AutoAssemble(record.Script, active ? Asm::AaSection::Enable : Asm::AaSection::Disable, record.ScriptMemory, false, OwnerOf(record), &record);
            if (!result.Ok())
            {
                record.LastError = result.Error.Text;
                m_lua->AfterActivation(record, active, false);
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
                m_lua->AfterActivation(record, active, false);
                return plain;
            }
        }

        if (active && record.Options.ActivateChildren)
        {
            for (const auto& child : record.Children)
            {
                const Status status = ActivateTree(*child, true);
                if (!status)
                    Notify(status);
            }
        }
        if (!record.LuaOnActivate.empty() && active)
            RunRecordLua(record.LuaOnActivate, "OnActivate");
        if (!record.LuaOnDeactivate.empty() && !active)
            RunRecordLua(record.LuaOnDeactivate, "OnDeactivate");
        m_lua->AfterActivation(record, active, true);
        return Status::Success();
    }

    Status Engine::ActivatePlain(Table::MemoryRecord& record, bool active)
    {
        if (record.Kind == Table::RecordKind::Value && active)
        {
            if (!m_process)
                return Status::Fail(ErrorCode::NoProcess);
            std::vector<std::uint8_t> bytes = ReadRecord(record);
            if (bytes.empty())
                return Status::Fail(ErrorCode::ReadFailed, record.AddressOk ? Hex(record.ResolvedAddress) : record.Address);
            record.FrozenBytes = std::move(bytes);
        }
        record.Active = active;
        return Status::Success();
    }

    bool Engine::OnActivationWorker()
    {
        return t_activationWorker;
    }

    void Engine::RunRecordLua(const std::string& code, const char* name)
    {
        if (!m_async)
        {
            m_lua->Run(code, name);
            return;
        }
        m_lua->StartTask(code, name, {});
    }

    void Engine::RunTableScript(const std::string& script)
    {
        if (!m_async)
        {
            const Status run = m_lua->Run(script, "table script");
            if (!run)
            {
                Notify(Status::Fail(ErrorCode::LuaTableScriptFailed, run.Text));
                Sounds::Play(Sounds::Cue::Error);
            }
            return;
        }
        m_tableTaskStarted = Now();
        m_tableTaskStopped = false;
        auto id = std::make_shared<int>(0);
        const int started = m_lua->StartTask(script, "table script", [this, id](const Status& status, const std::string&)
            {
                const bool early = *id == 0;
                const bool current = early || m_tableTask == *id;
                if (current && !early)
                    m_tableTask = 0;
                if (status)
                    LOG_INFO("Table", Format("The table's Lua script finished after %.1f s", Now() - m_tableTaskStarted));
                else if (current && m_tableTaskStopped)
                    LOG_INFO("Table", "The table's Lua script was stopped");
                else if (current)
                {
                    Notify(Status::Fail(ErrorCode::LuaTableScriptFailed, status.Text));
                    Sounds::Play(Sounds::Cue::Error);
                }
            });
        *id = started;
        m_tableTask = started;
    }

    void Engine::StopTableScript()
    {
        if (m_tableTask && m_lua->TaskAlive(m_tableTask))
        {
            m_tableTaskStopped = true;
            m_lua->StopTask(m_tableTask);
        }
    }

    void Engine::RequestActive(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        if (!m_async)
        {
            const Status status = SetActive(record, active);
            if (!status)
            {
                Notify(status);
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

    void Engine::Unpend(int recordId)
    {
        const auto found = m_pending.find(recordId);
        if (found != m_pending.end() && --found->second <= 0)
            m_pending.erase(found);
        ++m_progressDone;
    }

    void Engine::DropQueued(int recordId)
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

    void Engine::CancelActivations()
    {
        for (const ActivationJob& job : m_activations)
        {
            Unpend(job.RecordId);
            --m_progressDone;
            --m_progressTotal;
        }
        m_activations.clear();
    }

    ActivationProgress Engine::Progress() const
    {
        ActivationProgress progress;
        progress.Done = m_progressDone;
        progress.Total = m_progressTotal;
        progress.Current = m_progressCurrent;
        progress.TableScript = m_tableTask != 0 && m_lua->TaskAlive(m_tableTask);
        progress.TableScriptSeconds = progress.TableScript ? Now() - m_tableTaskStarted : 0.0;
        return progress;
    }

    bool Engine::Working() const
    {
        return m_running || !m_activations.empty() || m_lua->HasTasks();
    }

    void Engine::QueueChildren(Table::MemoryRecord& record, bool active, bool fromScript)
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

    void Engine::FinishRecordActivation(Table::MemoryRecord& record, bool active, bool fromScript)
    {
        if (active && record.Options.ActivateChildren)
            QueueChildren(record, active, fromScript);
        if (!record.LuaOnActivate.empty() && active)
            RunRecordLua(record.LuaOnActivate, "OnActivate");
        if (!record.LuaOnDeactivate.empty() && !active)
            RunRecordLua(record.LuaOnDeactivate, "OnDeactivate");
        m_lua->AfterActivation(record, active, true);
    }

    void Engine::PumpActivations(double budgetSeconds)
    {
        if (m_running)
        {
            if (m_running->Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            FinishActivation();
        }
        const bool tableScript = m_tableTask && m_lua->TaskAlive(m_tableTask);
        const double deadline = Now() + budgetSeconds;
        while (!m_running && !m_activations.empty() && Now() < deadline)
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

    std::vector<int> Engine::BottomToTop()
    {
        std::vector<int> ids;
        for (const Table::MemoryRecord* record : m_table.Flatten())
            ids.push_back(record->Id);
        std::reverse(ids.begin(), ids.end());
        return ids;
    }

    void Engine::RequestDeactivateAll()
    {
        if (!m_async)
        {
            const std::vector<std::string> failures = DeactivateAll();
            if (!failures.empty())
                Notify(LogLevel::Warning, std::to_string(failures.size()) + (failures.size() == 1 ? " script could not be turned off: " : " scripts could not be turned off: ") + failures.front());
            return;
        }
        CancelActivations();
        for (int id : BottomToTop())
        {
            if (Table::MemoryRecord* record = m_table.FindById(id); record && (record->Active || ActivationPending(id)))
                RequestActive(*record, false);
        }
    }

    void Engine::StartActivation(const ActivationJob& job)
    {
        const int recordId = job.RecordId;
        const bool active = job.Active;
        const bool fromScript = job.FromScript;
        Table::MemoryRecord* record = m_table.FindById(recordId);
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
        if (!m_lua->BeforeActivation(*record, active))
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
                m_lua->AfterActivation(*record, active, false);
                Notify(status);
                if (!fromScript)
                    Sounds::Play(Sounds::Cue::Error);
                return;
            }
            FinishRecordActivation(*record, active, fromScript);
            m_table.Refresh();
            return;
        }

        auto running = std::make_unique<RunningActivation>();
        running->RecordId = recordId;
        running->Active = active;
        running->FromScript = fromScript;
        running->Memory = record->ScriptMemory;
        running->Process = m_process;
        Asm::AaOptions options;
        options.Section = active ? Asm::AaSection::Enable : Asm::AaSection::Disable;
        options.CheckOnly = false;
        options.Owner = OwnerOf(*record);
        options.IncludeFolder = m_table.FilePath.empty() ? Paths::AutorunFolder() : std::filesystem::path(Utf8ToWide(m_table.FilePath)).parent_path();
        Asm::AaMemory* memory = &running->Memory;
        std::shared_ptr<Target::Process> process = m_process;
        running->Result = std::async(std::launch::async, [this, script = record->Script, options, memory, process, recordId]()
            {
                t_activationWorker = true;
                Asm::AaHost host;
                host.Process = process.get();
                host.Symbols = &m_symbols;
                host.RunLua = [this, recordId](std::string_view code, bool syntaxCheck, std::string& output) { return RunLuaForWorker(code, recordId, syntaxCheck, output); };
                host.IsCustomCommand = [this](std::string_view name)
                    {
                        bool known = false;
                        RunOnMainAndWait([&]() { known = m_lua->IsAutoAssemblerCommand(name); });
                        return known;
                    };
                host.RunCustomCommand = [this, recordId](std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output)
                    {
                        Status status = Status::Success();
                        RunOnMainAndWait([&]()
                            {
                                m_workerLuaActive = true;
                                const Lua::ScriptGlobals saved = m_lua->SaveScriptGlobals();
                                m_lua->SetScriptGlobals(m_table.FindById(recordId), syntaxCheck);
                                status = m_lua->RunAutoAssemblerCommand(name, parameters, syntaxCheck, output);
                                m_lua->RestoreScriptGlobals(saved);
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

    void Engine::FinishActivation()
    {
        std::unique_ptr<RunningActivation> running = std::move(m_running);
        const Asm::AaResult result = running->Result.get();
        Unpend(running->RecordId);
        Table::MemoryRecord* record = m_table.FindById(running->RecordId);
        if (!record)
            return;
        record->ScriptMemory = std::move(running->Memory);
        if (!result.Ok())
        {
            record->LastError = result.Error.Text;
            m_lua->AfterActivation(*record, running->Active, false);
            Notify(result.Error);
            if (!running->FromScript)
                Sounds::Play(Sounds::Cue::Error);
            m_table.Refresh();
            return;
        }
        record->LastError.clear();
        record->Active = running->Active;
        if (!running->Active)
            record->ScriptMemory.Values.clear();
        LOG_SUCCESS("Table", std::string(running->Active ? "Enabled " : "Disabled ") + record->Description);
        FinishRecordActivation(*record, running->Active, running->FromScript);
        m_table.Refresh();
    }

    void Engine::WaitForActivation()
    {
        if (!m_running || m_workerLuaActive || !OnMainThread())
            return;
        const bool previous = m_waitingForWorker;
        m_waitingForWorker = true;
        while (m_running->Result.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
        {
            DrainMainWork();
            m_lua->RunTasks(0.005);
        }
        m_waitingForWorker = previous;
        FinishActivation();
    }

    void Engine::RunOnMainAndWait(const std::function<void()>& work)
    {
        if (OnMainThread())
        {
            work();
            return;
        }
        std::promise<void> finished;
        std::future<void> future = finished.get_future();
        {
            std::lock_guard lock(m_mainLock);
            m_mainWork.push_back([&]()
                {
                    const bool serving = m_workerLuaActive;
                    m_workerLuaActive = true;
                    work();
                    m_workerLuaActive = serving;
                    finished.set_value();
                });
        }
        future.wait();
    }

    void Engine::DrainMainWork()
    {
        for (;;)
        {
            std::function<void()> work;
            {
                std::lock_guard lock(m_mainLock);
                if (m_mainWork.empty())
                    return;
                work = std::move(m_mainWork.front());
                m_mainWork.pop_front();
            }
            work();
        }
    }

    Status Engine::RunLuaForWorker(std::string_view code, int recordId, bool syntaxCheck, std::string& output)
    {
        std::promise<Status> finished;
        std::future<Status> future = finished.get_future();
        std::string result;
        const std::string text(code);
        {
            std::lock_guard lock(m_mainLock);
            m_mainWork.push_back([&, this]()
                {
                    if (m_async && !m_waitingForWorker)
                    {
                        auto saved = std::make_shared<Lua::ScriptGlobals>();
                        m_lua->StartTask(text, "{$lua}", [&](const Status& status, const std::string& value)
                            {
                                result = value;
                                finished.set_value(status);
                            },
                            [this, saved, recordId, syntaxCheck]()
                            {
                                *saved = m_lua->SaveScriptGlobals();
                                m_lua->SetScriptGlobals(m_table.FindById(recordId), syntaxCheck);
                                m_workerLuaActive = true;
                            },
                            [this, saved]()
                            {
                                m_lua->RestoreScriptGlobals(*saved);
                                m_workerLuaActive = false;
                            });
                        return;
                    }
                    m_workerLuaActive = true;
                    const Lua::ScriptGlobals saved = m_lua->SaveScriptGlobals();
                    m_lua->SetScriptGlobals(m_table.FindById(recordId), syntaxCheck);
                    const Status status = m_lua->RunWithResult(text, "{$lua}", result);
                    m_lua->RestoreScriptGlobals(saved);
                    m_workerLuaActive = false;
                    finished.set_value(status);
                });
        }
        const Status status = future.get();
        output = result;
        return status;
    }

    Status Engine::SetActive(Table::MemoryRecord& record, bool active)
    {
        if (m_running && OnMainThread())
            WaitForActivation();
        const Status status = ActivateTree(record, active);
        m_table.Refresh();
        return status;
    }

    std::vector<Table::MemoryRecord*> Engine::ActiveScripts()
    {
        std::vector<Table::MemoryRecord*> active;
        if (!m_process || !m_process->Alive())
            return active;
        m_table.ForEach([&](Table::MemoryRecord& record)
            {
                if (record.Kind == Table::RecordKind::Script && record.Active)
                    active.push_back(&record);
            });
        return active;
    }

    std::vector<std::string> Engine::DeactivateAll()
    {
        CancelActivations();
        WaitForActivation();
        std::vector<std::string> failures;
        for (int id : BottomToTop())
        {
            Table::MemoryRecord* record = m_table.FindById(id);
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

    void Engine::RefreshValues(bool force)
    {
        const double now = Now();
        if (!force && now - m_lastValues < m_settings.UpdateIntervalMs / 1000.0)
            return;
        m_lastValues = now;
        std::vector<int> ids;
        if (m_visibleKnown)
        {
            ids = m_visibleRecords;
            const std::vector<int> watched = m_lua->RecordsWithEvent("onvaluechanged");
            ids.insert(ids.end(), watched.begin(), watched.end());
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        }
        else
        {
            m_table.ForEach([&](Table::MemoryRecord& record)
                {
                    if (record.Kind == Table::RecordKind::Value)
                        ids.push_back(record.Id);
                });
        }
        for (int id : ids)
            RefreshRecord(id, now);
    }

    void Engine::RefreshRecord(int id, double now)
    {
        Table::MemoryRecord* record = m_table.FindById(id);
        if (!record || record->Kind != Table::RecordKind::Value)
            return;
        record->RefreshedAt = now;
        if (!m_process)
        {
            record->ValueText = "??";
            return;
        }
        const std::vector<std::uint8_t> bytes = ReadRecord(*record);
        if (bytes.empty())
        {
            record->ValueText = "??";
            record->LastBytes.clear();
            return;
        }
        std::string text = FormatRecord(*record, bytes);
        m_lua->DisplayValue(*record, text);
        record = m_table.FindById(id);
        if (!record)
            return;
        if (!record->LastBytes.empty() && record->LastBytes != bytes)
        {
            record->ValueChanged = true;
            record->ChangedAt = now;
            if (m_lua->HasRecordEvent(id, "onvaluechanged"))
            {
                m_lua->ValueChanged(*record, record->ValueText, text);
                record = m_table.FindById(id);
                if (!record)
                    return;
            }
        }
        record->LastBytes = bytes;
        record->ValueText = std::move(text);
    }

    void Engine::SetVisibleRecords(std::vector<int> ids)
    {
        std::sort(ids.begin(), ids.end());
        m_visibleRecords = std::move(ids);
        m_visibleKnown = true;
    }

    bool Engine::NeedsValue(const Table::MemoryRecord& record) const
    {
        return record.Kind == Table::RecordKind::Value && record.RefreshedAt < m_lastValues;
    }

    void Engine::RefreshRecord(int id)
    {
        RefreshRecord(id, Now());
    }

    void Engine::UpdateFreezes()
    {
        if (!m_process)
            return;
        const double now = Now();
        if (now - m_lastFreeze < m_settings.FreezeIntervalMs / 1000.0)
            return;
        m_lastFreeze = now;
        m_table.ForEach([&](Table::MemoryRecord& record)
            {
                if (record.Kind != Table::RecordKind::Value || !record.Active || record.FrozenBytes.empty())
                    return;
                ResolveRecord(record);
                if (!record.AddressOk)
                    return;
                if (record.Freeze != Table::FreezeMode::Normal)
                {
                    std::vector<std::uint8_t> current(record.FrozenBytes.size());
                    if (!m_process->ReadExact(record.ResolvedAddress, current.data(), current.size()))
                        return;
                    const auto now = Scan::AsDouble(record.Type, current, record.ShowSigned);
                    const auto frozen = Scan::AsDouble(record.Type, record.FrozenBytes, record.ShowSigned);
                    if (now && frozen)
                    {
                        if (record.Freeze == Table::FreezeMode::AllowIncrease && *now > *frozen)
                        {
                            record.FrozenBytes = current;
                            return;
                        }
                        if (record.Freeze == Table::FreezeMode::AllowDecrease && *now < *frozen)
                        {
                            record.FrozenBytes = current;
                            return;
                        }
                    }
                }
                m_process->Write(record.ResolvedAddress, record.FrozenBytes);
            });
    }

    void Engine::ApplyHotkey(Table::MemoryRecord& record, const Table::Hotkey& hotkey)
    {
        Status status;
        switch (hotkey.Action)
        {
        case Table::HotkeyAction::Toggle: RequestActive(record, !record.Active); break;
        case Table::HotkeyAction::ToggleAllowIncrease:
        case Table::HotkeyAction::ToggleAllowDecrease:
            if (!record.Active)
                record.Freeze = hotkey.Action == Table::HotkeyAction::ToggleAllowIncrease ? Table::FreezeMode::AllowIncrease : Table::FreezeMode::AllowDecrease;
            RequestActive(record, !record.Active);
            break;
        case Table::HotkeyAction::Activate: RequestActive(record, true); break;
        case Table::HotkeyAction::Deactivate: RequestActive(record, false); break;
        case Table::HotkeyAction::SetValue: status = WriteRecord(record, hotkey.Value); break;
        case Table::HotkeyAction::Increase:
        case Table::HotkeyAction::Decrease:
        {
            const auto delta = ParseDouble(hotkey.Value.empty() ? "1" : hotkey.Value);
            status = delta ? AddToRecord(record, hotkey.Action == Table::HotkeyAction::Increase ? *delta : -*delta) : Status::Fail(ErrorCode::ScanBadValue, hotkey.Value);
            break;
        }
        }
        if (!status)
            Notify(status);
        else
            LOG_INFO("Hotkey", Table::KeysText(hotkey.Keys) + ": " + Table::HotkeyActionName(hotkey.Action) + " " + record.Description);
    }

    void Engine::UpdateHotkeys()
    {
        const auto down = [](int key) { return (::GetAsyncKeyState(key) & 0x8000) != 0; };
        std::vector<std::pair<Table::MemoryRecord*, Table::Hotkey>> fired;
        m_table.ForEach([&](Table::MemoryRecord& record)
            {
                for (Table::Hotkey& hotkey : record.Hotkeys)
                {
                    if (hotkey.Keys.empty())
                        continue;
                    const bool all = std::all_of(hotkey.Keys.begin(), hotkey.Keys.end(), down);
                    if (all && !hotkey.WasDown)
                        fired.emplace_back(&record, hotkey);
                    hotkey.WasDown = all;
                }
            });
        for (auto& [record, hotkey] : fired)
            ApplyHotkey(*record, hotkey);
    }

    void Engine::UpdateAutoAttach()
    {
        if (!m_settings.AutoAttachEnabled || m_settings.AutoAttach.empty())
            return;
        const double now = Now();
        if (now - m_lastAttachCheck < kAttachCheckSeconds)
            return;
        m_lastAttachCheck = now;
        if ((m_process && m_process->Alive()) || m_attachLookup)
            return;
        m_attachLookup = true;
        const std::vector<std::string> names = m_settings.AutoAttach;
        m_work.Start({ "Looking for processes to open", "attach", false, true }, [this, names](WorkProgress&) -> BackgroundWork::Finish
            {
                std::uint32_t id = 0;
                std::string matched;
                for (const std::string& name : names)
                {
                    const std::string trimmed = Trim(name);
                    if (trimmed.empty())
                        continue;
                    if (const auto found = Target::FindProcessId(trimmed))
                    {
                        id = *found;
                        matched = trimmed;
                        break;
                    }
                }
                return [this, id, matched]()
                    {
                        m_attachLookup = false;
                        if (!id || !m_settings.AutoAttachEnabled || (m_process && m_process->Alive()))
                            return;
                        const Status status = OpenProcess(id);
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
        host.Process = m_process.get();
        host.Symbols = &m_symbols;
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
        if (!m_table.FilePath.empty())
            options.IncludeFolder = std::filesystem::path(Utf8ToWide(m_table.FilePath)).parent_path();
        else
            options.IncludeFolder = Paths::AutorunFolder();
        return Asm::RunAutoAssembler(script, host, memory, options);
    }

    Asm::AaResult Engine::AutoAssemble(std::string_view script, bool checkOnly)
    {
        return AutoAssemble(script, Asm::HasSections(script) ? Asm::AaSection::Enable : Asm::AaSection::Whole, m_scratchMemory, checkOnly, "scratch");
    }

    std::vector<Table::TableSymbol> Engine::TableSymbols() const
    {
        return m_tableSymbols;
    }

    void Engine::NewTable()
    {
        CancelActivations();
        WaitForActivation();
        StopTableScript();
        for (int id : BottomToTop())
        {
            Table::MemoryRecord* record = m_table.FindById(id);
            if (record && record->Kind == Table::RecordKind::Script && record->Active)
                SetActive(*record, false);
        }
        m_table.Clear();
        m_lua->ForgetRecords();
        m_structures.Clear();
        m_tableSymbols.clear();
        m_tableScriptWaiting = false;
    }

    Status Engine::LoadTable(const std::filesystem::path& path, bool merge)
    {
        Table::LoadedTable loaded;
        const Status status = Table::LoadTable(path, loaded);
        if (!status)
        {
            LOG_ERROR("Table", status.Text);
            return status;
        }
        if (!merge)
        {
            NewTable();
            m_table.FilePath = WideToUtf8(path.wstring());
            m_table.TableLuaScript = loaded.LuaScript;
            m_table.TableComment = loaded.Comments;
            m_table.DisasmNotes = std::move(loaded.DisasmNotes);
            m_table.Files = std::move(loaded.Files);
            m_table.ExtraXml = std::move(loaded.ExtraXml);
            m_table.LoadedVersion = loaded.Version;
            m_tableSymbols = loaded.Symbols;
        }
        else
        {
            m_tableSymbols.insert(m_tableSymbols.end(), loaded.Symbols.begin(), loaded.Symbols.end());
            for (Table::TableFile& file : loaded.Files)
            {
                if (!m_table.FindFile(file.Name))
                    m_table.Files.push_back(std::move(file));
            }
        }
        for (auto& record : loaded.Records)
            m_table.Add(std::move(record));
        const std::function<void(Table::MemoryRecord&)> fix = [&](Table::MemoryRecord& record)
            {
                m_table.NoteId(record.Id);
                for (auto& child : record.Children)
                {
                    child->Parent = &record;
                    fix(*child);
                }
            };
        for (auto& record : m_table.Roots())
            fix(*record);
        for (auto& structure : loaded.Structures)
        {
            if (!m_structures.Find(structure->Name))
                m_structures.Items.push_back(std::move(structure));
        }
        ++m_structures.Generation;
        for (const Table::TableSymbol& symbol : loaded.Symbols)
        {
            if (const auto value = Evaluate(symbol.Address))
                m_symbols.Register(symbol.Name, *value, "table");
        }
        for (const std::string& warning : loaded.Warnings)
            LOG_WARN("Table", warning);
        m_settings.AddRecentTable(WideToUtf8(path.wstring()));
        m_table.SetClean();
        LOG_SUCCESS("Table", "Loaded " + WideToUtf8(path.filename().wstring()) + " with " + std::to_string(m_table.Count()) + " records");

        if (!merge && !loaded.LuaScript.empty())
        {
            if (m_settings.TableScripts == TableScriptPolicy::Always)
                RunTableScript(loaded.LuaScript);
            else if (m_settings.TableScripts == TableScriptPolicy::Ask)
            {
                m_pendingTableScript = loaded.LuaScript;
                m_tableScriptWaiting = true;
            }
        }
        RefreshValues(true);
        return Status::Success();
    }

    void Engine::AnswerTableScript(bool run)
    {
        m_tableScriptWaiting = false;
        if (run)
            RunTableScript(m_pendingTableScript);
        m_pendingTableScript.clear();
    }

    Status Engine::SaveTable(const std::filesystem::path& path)
    {
        std::vector<Table::TableSymbol> symbols = m_tableSymbols;
        const Status status = Table::SaveTable(path, m_table, m_structures, symbols);
        if (!status)
        {
            LOG_ERROR("Table", status.Text);
            return status;
        }
        m_table.FilePath = WideToUtf8(path.wstring());
        m_table.SetClean();
        m_settings.AddRecentTable(m_table.FilePath);
        LOG_SUCCESS("Table", "Saved " + WideToUtf8(path.filename().wstring()));
        return status;
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
        Notice notice;
        notice.Level = level;
        notice.Text = std::move(text);
        notice.At = Now();
        notice.Id = m_nextNotice++;
        m_notices.push_back(std::move(notice));
        while (m_notices.size() > kMaxNotices)
            m_notices.pop_front();
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
        const double now = Now();
        {
            PROFILE_ZONE("Finished background jobs");
            m_jobs.Drain();
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

        if (m_process && now - m_lastAliveCheck > kAliveCheckSeconds)
        {
            PROFILE_ZONE("Process check");
            m_lastAliveCheck = now;
            if (!m_process->Alive())
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
            UpdateFreezes();
        }
        {
            PROFILE_ZONE("Hotkeys");
            UpdateHotkeys();
        }
        {
            PROFILE_ZONE("Address list values");
            RefreshValues(false);
        }
        {
            PROFILE_ZONE("Lua timers");
            m_lua->Tick(now);
        }
        {
            PROFILE_ZONE("Work for the main thread");
            DrainMainWork();
        }
        if (m_async)
        {
            {
                PROFILE_ZONE("Lua tasks");
                m_lua->RunTasks(kTaskBudgetSeconds);
            }
            {
                PROFILE_ZONE("Work for the main thread");
                DrainMainWork();
            }
            PROFILE_ZONE("Script activations");
            PumpActivations(kActivationBudgetSeconds);
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
