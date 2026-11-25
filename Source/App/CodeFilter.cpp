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
#include <cstring>
#include <map>
#include <span>
#include <utility>

#include <windows.h>

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

        constexpr std::size_t kScanChunk = 1u << 20;

        // Every slot of the module that the loader fixed up when it moved the
        // image: absolute addresses. In data these are the vtables, function
        // tables and callbacks; in 32-bit code the immediates (push offset
        // f). Empty when the module has no relocations.
        std::vector<std::uint64_t> RelocatedSlots(const Target::Process& process, const Target::ModuleInfo& module)
        {
            std::vector<std::uint64_t> slots;
            IMAGE_DOS_HEADER dos{};
            if (!process.ReadExact(module.Base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
                return slots;
            DWORD rva = 0;
            DWORD size = 0;
            if (process.Is64Bit())
            {
                IMAGE_NT_HEADERS64 nt{};
                if (!process.ReadExact(module.Base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
                    return slots;
                rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
                size = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
            }
            else
            {
                IMAGE_NT_HEADERS32 nt{};
                if (!process.ReadExact(module.Base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
                    return slots;
                rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
                size = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
            }
            if (!rva || size < 8 || static_cast<std::uint64_t>(rva) + size > module.Size)
                return slots;
            std::vector<std::uint8_t> blob(size);
            if (process.Read(module.Base + rva, blob.data(), blob.size()) != blob.size())
                return slots;
            const unsigned wanted = process.Is64Bit() ? IMAGE_REL_BASED_DIR64 : IMAGE_REL_BASED_HIGHLOW;
            std::size_t at = 0;
            while (at + 8 <= blob.size())
            {
                std::uint32_t page = 0;
                std::uint32_t length = 0;
                std::memcpy(&page, blob.data() + at, 4);
                std::memcpy(&length, blob.data() + at + 4, 4);
                if (length < 8 || at + length > blob.size())
                    break;
                for (std::size_t entry = at + 8; entry + 2 <= at + length; entry += 2)
                {
                    std::uint16_t value = 0;
                    std::memcpy(&value, blob.data() + entry, 2);
                    if ((value >> 12) == wanted && page + (value & 0xFFFu) < module.Size)
                        slots.push_back(module.Base + page + (value & 0xFFFu));
                }
                at += length;
            }
            std::sort(slots.begin(), slots.end());
            return slots;
        }

        template <class Count>
        void PointersAt(const Target::Process& process, std::span<const std::uint64_t> slots, Count&& count)
        {
            const std::size_t size = static_cast<std::size_t>(process.PointerSize());
            std::vector<std::uint8_t> page(0x1000 + 8);
            std::uint64_t loaded = ~0ull;
            std::size_t got = 0;
            for (const std::uint64_t slot : slots)
            {
                const std::uint64_t base = slot & ~0xFFFull;
                if (base != loaded)
                {
                    got = process.Read(base, page.data(), page.size());
                    loaded = base;
                }
                const std::size_t offset = static_cast<std::size_t>(slot - base);
                if (offset + size > got)
                    continue;
                std::uint64_t value = 0;
                std::memcpy(&value, page.data() + offset, size);
                count(value);
            }
        }

        // Without relocations: every aligned pointer-sized value in the
        // module's data that points into its code.
        template <class Count>
        void ScanData(const Target::Process& process, const Target::ModuleInfo& module, Count&& count)
        {
            const std::size_t size = static_cast<std::size_t>(process.PointerSize());
            std::vector<std::uint8_t> buffer(kScanChunk);
            for (const Target::RegionInfo& region : process.Regions(module.Base, module.Base + module.Size))
            {
                if (!region.Committed() || !region.Readable() || region.Executable() || region.Guarded())
                    continue;
                const std::uint64_t start = std::max(region.Base, module.Base);
                const std::uint64_t end = std::min(region.End(), module.Base + module.Size);
                for (std::uint64_t at = start; at < end; at += kScanChunk)
                {
                    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(kScanChunk, end - at));
                    const std::size_t got = process.Read(at, buffer.data(), wanted);
                    for (std::size_t i = 0; i + size <= got; i += size)
                    {
                        std::uint64_t value = 0;
                        std::memcpy(&value, buffer.data() + i, size);
                        count(value);
                    }
                }
            }
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
        add(SourcePointer, "Pointer");
        add(SourceImport, "Import");
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
        const std::uint64_t generation = m_engine.ProcessGeneration();
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
            std::shared_ptr<Disasm::DissectResult> dissected;
            if (calls)
            {
                dissected = Disasm::DissectCode(*process, { module }, progress);
                if (dissected && dissected->Cancelled)
                    return Failed("the search for functions was cancelled");
            }
            if (calls)
            {
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
            // ∑ Functions only reached through a pointer: virtual methods
            //   (their vtables), callbacks and function tables. A direct call
            //   never names them, and a leaf one has no unwind entry either.
            //   A target must start an unwind function or, outside of one,
            //   decode cleanly and sit on a 16-byte boundary or after padding,
            //   which leaves out the case labels of switch tables.
            if (options.Pointers)
            {
                std::map<std::uint64_t, std::uint32_t> pointers;
                const auto counter = [&](std::uint64_t value)
                    {
                        if (code.Contains(value))
                            ++pointers[value];
                    };
                const std::vector<std::uint64_t> slots = RelocatedSlots(*process, module);
                if (!slots.empty())
                    PointersAt(*process, slots, counter);
                else
                    ScanData(*process, module, counter);
                if (dissected)
                {
                    for (const Disasm::CodeRef& ref : dissected->Refs)
                    {
                        if (ref.Kind() == Disasm::RefKind::Address)
                            counter(ref.Target);
                    }
                }
                for (const auto& [target, references] : pointers)
                {
                    if (!module.Contains(target))
                        continue;
                    const auto* holding = functions.Holding(target);
                    if (holding && holding->first != target)
                    {
                        ++count.Rejected;
                        continue;
                    }
                    if (!PlausibleStart(*process, disassembler, target, 0, holding != nullptr))
                    {
                        ++count.Rejected;
                        continue;
                    }
                    add(target, SourcePointer);
                    ++count.Pointers;
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
        // ∑ Collecting takes seconds on a big game; when another process (or the same game
        //   started again) was opened meanwhile, these addresses are not its functions, and
        //   arming them would put int3s into the middle of its instructions.
        if (m_engine.ProcessGeneration() != generation || m_engine.Process() != process)
            return Failed("another process was opened while the functions were collected; collect them again");
        m_candidates = std::move(candidates);
        m_undo.clear();
        m_history.clear();
        m_report = report;
        m_prepared = true;
        m_skipped = 0;
        LOG_INFO("CodeFilter", Format("%zu functions in %zu modules ready for the code filter (%.2f s)", report.Total, report.Modules.size(), report.Seconds));
        return report;
    }

    namespace
    {
        constexpr std::size_t kImportProblems = 20;

        std::string_view Trimmed(std::string_view text)
        {
            if (text.starts_with("\xEF\xBB\xBF"))
                text.remove_prefix(3);
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
                text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
                text.remove_suffix(1);
            return text;
        }

        // A plain hex number with an optional 0x and ` or ' separators, the
        // common case of a long list, without the expression parser.
        std::optional<std::uint64_t> PlainHex(std::string_view text)
        {
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                text.remove_prefix(2);
            std::uint64_t value = 0;
            int digits = 0;
            for (const char c : text)
            {
                if (c == '`' || c == '\'')
                    continue;
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                if (digit < 0 || ++digits > 16)
                    return std::nullopt;
                value = (value << 4) | static_cast<std::uint64_t>(digit);
            }
            return digits ? std::optional<std::uint64_t>(value) : std::nullopt;
        }
    }

    // ∑ A line is the whole text first (an expression may hold spaces, as in
    //   "game.exe + 1A0"), then its first word, so lists with a second column
    //   (Copy the List, CE's "address - name") read too. An address must be
    //   executable, decode and not hold an int3 already; outside a module is
    //   fine (JIT code), Manifold's own library is not.
    Result<CodeImportReport> CodeFilter::Import(std::string_view text, ImportMode mode)
    {
        const auto started = std::chrono::steady_clock::now();
        const std::shared_ptr<Target::Process> process = m_engine.Process();
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        {
            std::lock_guard lock(m_lock);
            if (m_running)
                return Failed("a round is still running");
            if (mode == ImportMode::Add && !m_history.empty())
                return Failed("rounds have run already; addresses can only be added before the first round");
        }
        process->RefreshModules();
        const Disasm::Disassembler& disassembler = m_engine.Disassembler();
        Debug::Debugger& debugger = m_engine.Debugger();
        CodeImportReport report;
        std::map<std::uint64_t, std::uint8_t> found;
        const auto reject = [&](std::size_t line, std::string_view shown, std::string_view why)
            {
                ++report.Rejected;
                if (report.Problems.size() < kImportProblems)
                    report.Problems.push_back(Format("line %zu, %.*s: %.*s", line, static_cast<int>(std::min<std::size_t>(shown.size(), 60)), shown.data(),
                        static_cast<int>(why.size()), why.data()));
            };
        std::uint64_t lastPage = ~0ull;
        bool lastCode = false;
        std::size_t lineNumber = 0;
        std::size_t at = 0;
        while (at <= text.size())
        {
            const std::size_t end = std::min(text.find('\n', at), text.size());
            const std::string_view line = Trimmed(text.substr(at, end - at));
            at = end + 1;
            ++lineNumber;
            if (line.empty() || line.front() == '#' || line.front() == ';' || line.starts_with("//"))
                continue;
            ++report.Lines;
            std::optional<std::uint64_t> address = PlainHex(line);
            if (!address)
                address = m_engine.Evaluate(line);
            if (!address)
            {
                const std::size_t space = line.find_first_of(" \t,");
                if (space != std::string_view::npos)
                {
                    const std::string_view word = line.substr(0, space);
                    address = PlainHex(word);
                    if (!address)
                        address = m_engine.Evaluate(word);
                }
            }
            if (!address)
            {
                reject(lineNumber, line, "not an address");
                continue;
            }
            const std::uint64_t page = *address & ~0xFFFull;
            if (page != lastPage)
            {
                const auto region = process->RegionAt(*address);
                lastCode = region && region->Committed() && region->Executable() && !region->Guarded();
                lastPage = page;
            }
            if (!lastCode)
            {
                reject(lineNumber, line, "not in executable memory");
                continue;
            }
            if (const auto module = process->ModuleAt(*address); module && IsAgent(module->Name))
            {
                reject(lineNumber, line, "inside Manifold Engine's own library");
                continue;
            }
            std::uint8_t head = 0;
            if (!process->ReadExact(*address, &head, 1) || head == 0xCC)
            {
                reject(lineNumber, line, head == 0xCC ? "an int3 is there already" : "cannot be read");
                continue;
            }
            if (!disassembler.Read(*process, *address).Valid)
            {
                reject(lineNumber, line, "no instruction starts there");
                continue;
            }
            if ((debugger.Attached() && debugger.HasBreakpoint(*address)) || m_engine.Speed().Owns(*address, 1))
            {
                reject(lineNumber, line, "a breakpoint or the speedhack uses it");
                continue;
            }
            if (found.count(*address))
            {
                ++report.Duplicates;
                continue;
            }
            found[*address] = SourceImport;
        }
        std::lock_guard lock(m_lock);
        if (m_running)
            return Failed("a round is still running");
        const bool add = mode == ImportMode::Add && m_prepared && m_history.empty();
        if (found.empty() && !add)
        {
            if (!report.Lines)
                return Failed("the text holds no addresses");
            return Failed(Format("none of the %zu lines is an address in code", report.Lines) + (report.Problems.empty() ? std::string() : " (" + report.Problems.front() + ")"));
        }
        std::vector<CodeCandidate> candidates;
        if (add)
        {
            for (const CodeCandidate& candidate : m_candidates)
            {
                const auto it = found.find(candidate.Address);
                if (it != found.end())
                {
                    ++report.Duplicates;
                    found.erase(it);
                }
            }
            candidates = m_candidates;
        }
        report.Imported = found.size();
        for (const auto& [address, sources] : found)
            candidates.push_back({ address, sources });
        std::sort(candidates.begin(), candidates.end(), [](const CodeCandidate& a, const CodeCandidate& b) { return a.Address < b.Address; });
        if (candidates.size() > Target::Cover::kMaxEntries)
            return Failed(Format("%zu addresses are more than one round can hold (%u)", candidates.size(), Target::Cover::kMaxEntries));
        CodeFilterReport merged = add ? m_report : CodeFilterReport{};
        std::map<std::string, std::size_t> perModule;
        for (const auto& [address, sources] : found)
        {
            const auto module = process->ModuleAt(address);
            ++perModule[module ? module->Name : std::string("(no module)")];
        }
        for (const auto& [name, imported] : perModule)
        {
            auto it = std::find_if(merged.Modules.begin(), merged.Modules.end(), [&](const CodeModuleCount& count) { return count.Module == name; });
            if (it == merged.Modules.end())
            {
                merged.Modules.push_back({});
                it = std::prev(merged.Modules.end());
                it->Module = name;
            }
            it->Imported += imported;
            it->Total += imported;
        }
        merged.Total = candidates.size();
        report.Total = candidates.size();
        report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        merged.Seconds = report.Seconds;
        m_candidates = std::move(candidates);
        m_report = std::move(merged);
        if (!add)
        {
            m_undo.clear();
            m_history.clear();
        }
        m_prepared = true;
        m_skipped = 0;
        const std::string message = Format("Imported %zu addresses for the code filter (%zu lines, %zu duplicates, %zu rejected), %zu in the list", report.Imported, report.Lines,
            report.Duplicates, report.Rejected, report.Total);
        if (report.Problems.empty())
        {
            LOG_INFO("CodeFilter", message);
        }
        else
        {
            std::string details = "Rejected lines:";
            for (const std::string& problem : report.Problems)
                details += "\n" + problem;
            if (report.Rejected > report.Problems.size())
                details += Format("\nand %zu more", report.Rejected - report.Problems.size());
            RuntimeLog::WriteDetailed(LogLevel::Warning, "CodeFilter", __FILE__, __FUNCTION__, __LINE__, message, details);
        }
        return report;
    }

    std::string CodeFilter::ExportText() const
    {
        const std::vector<CodeCandidate> candidates = Candidates();
        const std::shared_ptr<Target::Process> process = m_engine.Process();
        std::string text = Format("# Manifold Engine code filter: %zu addresses", candidates.size());
        if (process)
        {
            if (const auto main = process->MainModule())
                text += " in " + main->Name;
        }
        text += "\n# One address per line as module+offset, which stays right after the game restarts. Filter > Import Addresses reads it back.\n";
        for (const CodeCandidate& candidate : candidates)
            text += m_engine.Symbols().ModuleOffset(candidate.Address) + "\n";
        return text;
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
