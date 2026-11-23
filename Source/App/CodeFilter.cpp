#include "App/CodeFilter.h"

#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/CodeDissect.h"
#include "Disasm/Disassembler.h"
#include "Disasm/FlowGraph.h"
#include "Target/CodeMap.h"
#include "Target/Coverage.h"
#include "Target/CoverageProtocol.h"

#include <algorithm>
#include <map>
#include <utility>

namespace Manifold
{
    namespace
    {
        constexpr int kCheckedInstructions = 3;

        Status Failed(std::string_view detail)
        {
            return Status::Fail(ErrorCode::CodeFilterFailed, detail);
        }

        bool IsAgent(std::string_view name)
        {
            const std::string lower = Lower(name);
            return lower == "manifoldagent.dll" || lower == "manifoldagent32.dll";
        }

        struct Ranges
        {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> Items;

            bool Contains(std::uint64_t address) const
            {
                auto it = std::upper_bound(Items.begin(), Items.end(), address, [](std::uint64_t at, const auto& range) { return at < range.first; });
                if (it == Items.begin())
                    return false;
                --it;
                return address >= it->first && address < it->second;
            }

            // The range that holds the address, as [begin, end).
            const std::pair<std::uint64_t, std::uint64_t>* Holding(std::uint64_t address) const
            {
                auto it = std::upper_bound(Items.begin(), Items.end(), address, [](std::uint64_t at, const auto& range) { return at < range.first; });
                if (it == Items.begin())
                    return nullptr;
                --it;
                return address >= it->first && address < it->second ? &*it : nullptr;
            }
        };

        // A call target that the unwind table does not vouch for must decode
        // as a few plain instructions; without an unwind table (32-bit) it
        // also needs a second caller, a 16-byte alignment or padding before
        // it, so a "call" decoded out of data does not get an int3 in the
        // middle of an instruction.
        bool PlausibleStart(const Target::Process& process, const Disasm::Disassembler& disassembler, std::uint64_t target, std::uint32_t calls, bool unwindKnown)
        {
            std::uint8_t head[2] = {};
            if (!process.ReadExact(target, head, sizeof(head)) || (head[0] == 0 && head[1] == 0) || head[0] == 0xCC)
                return false;
            std::uint64_t at = target;
            for (int i = 0; i < kCheckedInstructions; ++i)
            {
                const Disasm::Instruction instruction = disassembler.Read(process, at);
                if (!instruction.Valid || instruction.Kind == Disasm::Flow::Interrupt)
                    return false;
                if (instruction.Kind == Disasm::Flow::Return || instruction.Kind == Disasm::Flow::Jump)
                    break;
                at = instruction.Next();
            }
            if (unwindKnown)
                return true;
            std::uint8_t before = 0;
            const bool padded = process.ReadExact(target - 1, &before, 1) && (before == 0xCC || before == 0x90 || before == 0xC3);
            return calls >= 2 || (target & 0xF) == 0 || padded;
        }
    }

    std::string CodeSourceText(std::uint8_t sources)
    {
        std::string text;
        const auto add = [&](std::uint8_t bit, const char* name)
            {
                if (!(sources & bit))
                    return;
                if (!text.empty())
                    text += ", ";
                text += name;
            };
        add(SourceUnwind, "Unwind");
        add(SourceExport, "Export");
        add(SourceCall, "Call");
        add(SourceBlock, "Block");
        return text;
    }

    const char* FilterRoundText(FilterRound round)
    {
        switch (round)
        {
        case FilterRound::FilterOut: return "Filter Out";
        case FilterRound::KeepOnly: return "Keep Only";
        case FilterRound::Refine: return "Refine to Blocks";
        }
        return "";
    }

    std::vector<Target::ModuleInfo> CodeFilter::DefaultModules(const Target::Process& process)
    {
        std::vector<Target::ModuleInfo> modules;
        const Target::ModuleList list = process.Modules();
        if (!list)
            return modules;
        for (const Target::ModuleInfo& module : *list)
        {
            if (module.System || IsAgent(module.Name))
                continue;
            modules.push_back(module);
        }
        return modules;
    }

    Result<CodeFilterReport> CodeFilter::Prepare(const CodeFilterOptions& options, Disasm::DissectProgress* progress)
    {
        const auto started = std::chrono::steady_clock::now();
        const std::shared_ptr<Target::Process> process = m_engine.Process();
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        {
            std::lock_guard lock(m_lock);
            if (m_running)
                return Failed("a round is still running");
        }
        process->RefreshModules();
        std::vector<Target::ModuleInfo> modules;
        if (options.Modules.empty())
        {
            modules = DefaultModules(*process);
        }
        else
        {
            for (const std::string& name : options.Modules)
            {
                const auto module = process->FindModule(name);
                if (!module)
                    return Status::Fail(ErrorCode::ModuleNotFound, name);
                if (IsAgent(module->Name))
                    return Failed(module->Name + " is Manifold Engine's own library");
                modules.push_back(*module);
            }
        }
        if (modules.empty())
            return Failed("there is no module to look at");
        const bool is64Bit = process->Is64Bit();
        const bool calls = options.Calls.value_or(!is64Bit);
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        Target::Symbols& symbols = m_engine.Symbols();

        CodeFilterReport report;
        std::map<std::uint64_t, std::uint8_t> found;
        for (const Target::ModuleInfo& module : modules)
        {
            CodeModuleCount count;
            count.Module = module.Name;
            Ranges code;
            for (const Disasm::CodeSpan& span : Disasm::ExecutableSpans(*process, { module }))
                code.Items.emplace_back(span.Start, span.End);
            Ranges functions;
            if (is64Bit)
            {
                for (const auto& [begin, end] : Target::CodeMap::UnwindFunctions(*process, module.Base, module.Size))
                    functions.Items.emplace_back(module.Base + begin, module.Base + end);
                std::sort(functions.Items.begin(), functions.Items.end());
            }
            const auto add = [&](std::uint64_t address, std::uint8_t source)
                {
                    std::uint8_t& sources = found[address];
                    sources |= source;
                };
            if (options.Unwind)
            {
                for (const auto& [begin, end] : functions.Items)
                {
                    if (code.Contains(begin))
                    {
                        add(begin, SourceUnwind);
                        ++count.Unwind;
                    }
                }
            }
            if (options.Exports)
            {
                for (const Target::ExportSymbol& symbol : symbols.ExportsOf(module))
                {
                    if (!module.Contains(symbol.Address) || !code.Contains(symbol.Address))
                        continue;
                    const auto* holding = functions.Holding(symbol.Address);
                    if (holding && holding->first != symbol.Address)
                    {
                        ++count.Rejected;
                        continue;
                    }
                    if (!holding && !PlausibleStart(*process, disassembler, symbol.Address, 2, is64Bit))
                    {
                        ++count.Rejected;
                        continue;
                    }
                    add(symbol.Address, SourceExport);
                    ++count.Exports;
                }
            }
            if (calls)
            {
                const std::shared_ptr<Disasm::DissectResult> dissected = Disasm::DissectCode(*process, { module }, progress);
                if (dissected && dissected->Cancelled)
                    return Failed("the search for functions was cancelled");
                for (const Disasm::ReferencedFunction& function : dissected ? dissected->Functions : std::vector<Disasm::ReferencedFunction>{})
                {
                    if (!module.Contains(function.Address) || !code.Contains(function.Address))
                        continue;
                    const auto* holding = functions.Holding(function.Address);
                    if (holding && holding->first != function.Address)
                    {
                        ++count.Rejected;
                        continue;
                    }
                    if (!PlausibleStart(*process, disassembler, function.Address, function.Calls, holding != nullptr))
                    {
                        ++count.Rejected;
                        continue;
                    }
                    add(function.Address, SourceCall);
                    ++count.Calls;
                }
            }
            report.Modules.push_back(std::move(count));
        }
        std::vector<CodeCandidate> candidates;
        candidates.reserve(found.size());
        Debug::Debugger& debugger = m_engine.Debugger();
        for (const auto& [address, sources] : found)
        {
            if (debugger.Attached() && debugger.HasBreakpoint(address))
                continue;
            if (m_engine.Speed().Owns(address, 1))
                continue;
            candidates.push_back({ address, sources });
        }
        for (CodeModuleCount& count : report.Modules)
        {
            const auto module = process->FindModule(count.Module);
            if (!module)
                continue;
            count.Total = static_cast<std::size_t>(std::count_if(candidates.begin(), candidates.end(), [&](const CodeCandidate& candidate) { return module->Contains(candidate.Address); }));
        }
        if (candidates.size() > Target::Cover::kMaxEntries)
            return Failed(Format("%zu functions are more than one round can hold (%u); choose fewer modules", candidates.size(), Target::Cover::kMaxEntries));
        report.Total = candidates.size();
        report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::lock_guard lock(m_lock);
        m_candidates = std::move(candidates);
        m_undo.clear();
        m_history.clear();
        m_report = report;
        m_prepared = true;
        m_skipped = 0;
        LOG_INFO("CodeFilter", Format("%zu functions in %zu modules ready for the code filter (%.2f s)", report.Total, report.Modules.size(), report.Seconds));
        return report;
    }

    Status CodeFilter::Start(FilterRound kind)
    {
        std::lock_guard lock(m_lock);
        if (!m_prepared)
            return Failed("no functions were collected yet");
        if (m_running)
            return Failed("a round is already running");
        if (kind == FilterRound::Refine)
            return Failed("refining is not a round; call Refine");
        if (m_candidates.empty())
            return Failed("no functions are left");
        std::vector<std::uint64_t> addresses;
        addresses.reserve(m_candidates.size());
        for (const CodeCandidate& candidate : m_candidates)
            addresses.push_back(candidate.Address);
        Target::Coverage& coverage = m_engine.CodeCoverage();
        if (Status armed = coverage.Arm(addresses); !armed)
        {
            coverage.Disarm();
            return armed;
        }
        m_skipped = 0;
        if (const auto entries = coverage.Entries())
        {
            std::vector<std::uint64_t> skipped;
            for (const Target::CoverEntry& entry : *entries)
            {
                if (entry.State == Target::CoverState::Skipped)
                    skipped.push_back(entry.Address);
            }
            if (!skipped.empty())
            {
                std::erase_if(m_candidates, [&](const CodeCandidate& candidate) { return std::binary_search(skipped.begin(), skipped.end(), candidate.Address); });
                m_skipped = skipped.size();
            }
        }
        const auto counts = coverage.Counts();
        m_hitBase = counts ? counts->Hits : 0;
        m_kind = kind;
        m_running = true;
        m_started = std::chrono::steady_clock::now();
        return Status::Success();
    }

    Status CodeFilter::Stop()
    {
        std::lock_guard lock(m_lock);
        if (!m_running)
            return Failed("no round is running");
        Target::Coverage& coverage = m_engine.CodeCoverage();
        const Result<std::vector<std::uint64_t>> hits = coverage.Hits();
        const Status disarmed = coverage.Disarm();
        m_running = false;
        if (!hits)
            return hits.Error();
        if (!disarmed)
            LOG_WARN("CodeFilter", disarmed.Text);
        RoundRecord record;
        record.Kind = m_kind;
        record.Before = m_candidates.size() + m_skipped;
        record.Hits = hits->size();
        record.Skipped = m_skipped;
        record.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
        m_undo.push_back(m_candidates);
        if (m_undo.size() > kUndoDepth)
            m_undo.erase(m_undo.begin());
        const auto hit = [&](const CodeCandidate& candidate) { return std::binary_search(hits->begin(), hits->end(), candidate.Address); };
        if (m_kind == FilterRound::FilterOut)
            std::erase_if(m_candidates, hit);
        else
            std::erase_if(m_candidates, [&](const CodeCandidate& candidate) { return !hit(candidate); });
        record.After = m_candidates.size();
        m_history.push_back(record);
        LOG_INFO("CodeFilter", Format("%s: %zu of %zu functions ran, %zu left", FilterRoundText(record.Kind), record.Hits, record.Before, record.After));
        return disarmed;
    }

    // ∑ Every block start comes from decoding the function from its start,
    //   so it is an instruction boundary as long as the function start is one.
    //   Blocks that are already blocks stay as they are; a function's first
    //   block keeps the function's own sources.
    Result<std::size_t> CodeFilter::Refine(std::size_t maxFunctions)
    {
        const std::shared_ptr<Target::Process> process = m_engine.Process();
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        std::lock_guard lock(m_lock);
        if (!m_prepared)
            return Failed("no functions were collected yet");
        if (m_running)
            return Failed("a round is still running");
        const std::size_t functions = static_cast<std::size_t>(std::count_if(m_candidates.begin(), m_candidates.end(), [](const CodeCandidate& candidate) { return candidate.Function == 0; }));
        if (functions == 0)
            return Failed("every candidate is a block already");
        if (functions > maxFunctions)
            return Failed(Format("%zu functions are left; narrow them down to %zu or fewer first", functions, maxFunctions));
        const auto started = std::chrono::steady_clock::now();
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        const Target::Symbols& symbols = m_engine.Symbols();
        const Target::CodeMap* hints = m_engine.CodeMap();
        const auto other = [&](std::uint64_t target, Disasm::EdgeKind) { return m_engine.FunctionStartAt(target); };
        std::map<std::uint64_t, CodeCandidate> blocks;
        for (const CodeCandidate& candidate : m_candidates)
        {
            if (candidate.Function != 0)
            {
                blocks.emplace(candidate.Address, candidate);
                continue;
            }
            const Disasm::FlowGraph graph = Disasm::BuildFlowGraph(disassembler, *process, candidate.Address, &symbols, hints, other);
            CodeCandidate entry = candidate;
            entry.Function = candidate.Address;
            blocks[candidate.Address] = entry;
            for (const Disasm::FlowBlock& block : graph.Blocks)
            {
                if (block.Start == candidate.Address || block.Lines.empty())
                    continue;
                CodeCandidate part;
                part.Address = block.Start;
                part.Sources = SourceBlock;
                part.Function = candidate.Address;
                blocks.emplace(block.Start, part);
            }
        }
        RoundRecord record;
        record.Kind = FilterRound::Refine;
        record.Before = m_candidates.size();
        m_undo.push_back(m_candidates);
        if (m_undo.size() > kUndoDepth)
            m_undo.erase(m_undo.begin());
        m_candidates.clear();
        m_candidates.reserve(blocks.size());
        for (auto& [address, candidate] : blocks)
            m_candidates.push_back(candidate);
        record.After = m_candidates.size();
        record.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        m_history.push_back(record);
        LOG_INFO("CodeFilter", Format("Refined %zu functions into %zu blocks", functions, record.After));
        return record.After;
    }

    Status CodeFilter::Cancel()
    {
        std::lock_guard lock(m_lock);
        if (!m_running)
            return Status::Success();
        m_running = false;
        return m_engine.CodeCoverage().Disarm();
    }

    bool CodeFilter::Undo()
    {
        std::lock_guard lock(m_lock);
        if (m_running || m_undo.empty())
            return false;
        m_candidates = std::move(m_undo.back());
        m_undo.pop_back();
        if (!m_history.empty())
            m_history.pop_back();
        return true;
    }

    void CodeFilter::Reset()
    {
        std::lock_guard lock(m_lock);
        if (m_running)
            m_engine.CodeCoverage().Disarm();
        m_running = false;
        m_prepared = false;
        m_candidates.clear();
        m_undo.clear();
        m_history.clear();
        m_report = {};
        m_skipped = 0;
    }

    bool CodeFilter::Prepared() const
    {
        std::lock_guard lock(m_lock);
        return m_prepared;
    }

    bool CodeFilter::Running() const
    {
        std::lock_guard lock(m_lock);
        return m_running;
    }

    std::optional<FilterRound> CodeFilter::RunningKind() const
    {
        std::lock_guard lock(m_lock);
        if (!m_running)
            return std::nullopt;
        return m_kind;
    }

    std::size_t CodeFilter::Remaining() const
    {
        std::lock_guard lock(m_lock);
        return m_candidates.size();
    }

    std::vector<CodeCandidate> CodeFilter::Candidates() const
    {
        std::lock_guard lock(m_lock);
        return m_candidates;
    }

    std::vector<RoundRecord> CodeFilter::History() const
    {
        std::lock_guard lock(m_lock);
        return m_history;
    }

    CodeFilterReport CodeFilter::Report() const
    {
        std::lock_guard lock(m_lock);
        return m_report;
    }

    std::uint64_t CodeFilter::LiveHits() const
    {
        std::lock_guard lock(m_lock);
        if (!m_running)
            return 0;
        const auto counts = m_engine.CodeCoverage().Counts();
        return counts && counts->Hits >= m_hitBase ? counts->Hits - m_hitBase : 0;
    }

    double CodeFilter::RoundSeconds() const
    {
        std::lock_guard lock(m_lock);
        if (!m_running)
            return 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count();
    }
}
