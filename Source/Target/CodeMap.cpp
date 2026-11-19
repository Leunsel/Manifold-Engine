#include "Target/CodeMap.h"

#include "Target/Mono.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_map>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kPage = 0x1000;
        // Below an address outside every method, the end of the method before it
        // is looked for this far down (Mono pads methods to a small alignment and
        // keeps trampolines between them). Probes are kMonoStride apart: every
        // method is longer than that, so none fits between two probes.
        constexpr std::uint64_t kMonoProbe = 64;
        constexpr std::uint64_t kMonoStride = 8;
        constexpr std::uint32_t kMaxUnwindBytes = 64 * 1024 * 1024;
        constexpr std::uint32_t kMaxUnwindInfo = 4 + 256 * 2 + 12;
        constexpr std::uint8_t kChainedInfo = 0x4;
        constexpr int kMaxChain = 8;
        constexpr WORD kMaxSections = 96;

        struct UnwindEntry
        {
            std::uint32_t Begin;
            std::uint32_t End;
            std::uint32_t Unwind;
        };
        static_assert(sizeof(UnwindEntry) == 12);
    }

    void CodeMap::Attach(std::shared_ptr<Process> process, MonoBridge* mono, const Symbols* symbols)
    {
        std::lock_guard lock(m_lock);
        m_process = std::move(process);
        m_mono = mono;
        m_symbols = symbols;
        m_tables.clear();
        m_last.reset();
    }

    void CodeMap::Detach()
    {
        Attach(nullptr, nullptr, nullptr);
    }

    std::shared_ptr<const CodeMap::Table> CodeMap::LoadTable(std::uint64_t base, std::uint64_t size) const
    {
        auto table = std::make_shared<Table>();
        table->Base = base;
        table->Size = size;
        std::shared_ptr<Process> process;
        {
            std::lock_guard lock(m_lock);
            process = m_process;
        }
        if (!process)
            return table;
        table->Sections = ReadSections(*process, base, size);
        if (!process->Is64Bit())
            return table;
        for (const auto& [begin, end] : UnwindFunctions(*process, base, size))
            table->Functions.push_back({ begin, end });
        return table;
    }

    std::vector<CodeMap::Section> CodeMap::ReadSections(const Process& process, std::uint64_t base, std::uint64_t size)
    {
        std::vector<Section> sections;
        IMAGE_DOS_HEADER dos{};
        if (!process.ReadExact(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || static_cast<std::uint64_t>(dos.e_lfanew) >= size)
            return sections;
        const std::uint64_t nt = base + static_cast<std::uint64_t>(dos.e_lfanew);
        DWORD signature = 0;
        IMAGE_FILE_HEADER file{};
        DWORD alignment = 0;
        const std::uint64_t optional = nt + sizeof(signature) + sizeof(file);
        if (!process.ReadExact(nt, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE || !process.ReadExact(nt + sizeof(signature), &file, sizeof(file)) ||
            file.NumberOfSections == 0 || file.NumberOfSections > kMaxSections ||
            !process.ReadExact(optional + offsetof(IMAGE_OPTIONAL_HEADER64, SectionAlignment), &alignment, sizeof(alignment)))
            return sections;
        if (alignment == 0 || (alignment & (alignment - 1)) != 0)
            alignment = static_cast<DWORD>(kPage);
        std::vector<IMAGE_SECTION_HEADER> headers(file.NumberOfSections);
        if (!process.ReadExact(optional + file.SizeOfOptionalHeader, headers.data(), headers.size() * sizeof(IMAGE_SECTION_HEADER)))
            return sections;
        for (const IMAGE_SECTION_HEADER& header : headers)
        {
            const std::uint64_t span = header.Misc.VirtualSize ? header.Misc.VirtualSize : header.SizeOfRawData;
            if (span == 0 || header.VirtualAddress >= size)
                continue;
            const std::uint64_t end = std::min<std::uint64_t>(size, (static_cast<std::uint64_t>(header.VirtualAddress) + span + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1));
            sections.push_back({ header.VirtualAddress, static_cast<std::uint32_t>(end), (header.Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) != 0 });
        }
        std::sort(sections.begin(), sections.end(), [](const Section& a, const Section& b)
            {
                return a.Begin < b.Begin;
            });
        return sections;
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> CodeMap::UnwindFunctions(const Process& process, std::uint64_t base, std::uint64_t size)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> functions;
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!process.ReadExact(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return functions;
        if (!process.ReadExact(base + static_cast<std::uint64_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
            return functions;
        const IMAGE_DATA_DIRECTORY directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(UnwindEntry) || directory.Size > kMaxUnwindBytes ||
            static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size > size)
            return functions;
        std::vector<std::uint8_t> table(directory.Size / sizeof(UnwindEntry) * sizeof(UnwindEntry));
        if (!process.ReadExact(base + directory.VirtualAddress, table.data(), table.size()))
            return functions;
        std::uint32_t low = UINT32_MAX;
        std::uint32_t high = 0;
        for (std::size_t offset = 0; offset + sizeof(UnwindEntry) <= table.size(); offset += sizeof(UnwindEntry))
        {
            UnwindEntry entry{};
            std::memcpy(&entry, table.data() + offset, sizeof(entry));
            const std::uint32_t at = entry.Unwind & ~1u;
            if (at == 0 || at >= size || (entry.Unwind & 1u))
                continue;
            low = std::min(low, at);
            high = std::max(high, at);
        }
        std::vector<std::uint8_t> info;
        if (low <= high)
        {
            const std::uint64_t span = std::min<std::uint64_t>(static_cast<std::uint64_t>(high - low) + kMaxUnwindInfo, size - low);
            if (span <= kMaxUnwindBytes)
            {
                info.resize(static_cast<std::size_t>(span));
                info.resize(process.Read(base + low, info.data(), info.size()));
            }
        }
        return FunctionsFromUnwind(table, directory.VirtualAddress, info, low <= high ? low : 0, size);
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> CodeMap::FunctionsFromUnwind(std::span<const std::uint8_t> table, std::uint32_t tableRva, std::span<const std::uint8_t> info,
        std::uint32_t infoRva, std::uint64_t size)
    {
        std::vector<UnwindEntry> entries(table.size() / sizeof(UnwindEntry));
        if (!entries.empty())
            std::memcpy(entries.data(), table.data(), entries.size() * sizeof(UnwindEntry));
        std::sort(entries.begin(), entries.end(), [](const UnwindEntry& a, const UnwindEntry& b) { return a.Begin < b.Begin; });

        const auto entryAt = [&](std::uint32_t rva) -> std::optional<UnwindEntry>
            {
                UnwindEntry entry{};
                if (rva >= tableRva && rva - tableRva + sizeof(entry) <= table.size())
                {
                    std::memcpy(&entry, table.data() + (rva - tableRva), sizeof(entry));
                    return entry;
                }
                if (rva >= infoRva && rva - infoRva + sizeof(entry) <= info.size())
                {
                    std::memcpy(&entry, info.data() + (rva - infoRva), sizeof(entry));
                    return entry;
                }
                return std::nullopt;
            };
        const auto parentOf = [&](const UnwindEntry& entry) -> std::optional<UnwindEntry>
            {
                if (entry.Unwind & 1u)
                    return entryAt(entry.Unwind & ~1u);
                if (entry.Unwind < infoRva)
                    return std::nullopt;
                const std::size_t offset = entry.Unwind - infoRva;
                if (offset + 4 > info.size() || !((info[offset] >> 3) & kChainedInfo))
                    return std::nullopt;
                const std::size_t codes = (static_cast<std::size_t>(info[offset + 2]) + 1) & ~std::size_t(1);
                return entryAt(static_cast<std::uint32_t>(entry.Unwind + 4 + codes * 2));
            };

        std::vector<std::pair<std::uint32_t, std::uint32_t>> functions;
        functions.reserve(entries.size());
        std::unordered_map<std::uint32_t, std::size_t> roots;
        for (const UnwindEntry& entry : entries)
        {
            if (entry.Begin >= entry.End || entry.End > size)
                continue;
            std::uint32_t root = entry.Begin;
            UnwindEntry current = entry;
            for (int depth = 0; depth < kMaxChain; ++depth)
            {
                const std::optional<UnwindEntry> parent = parentOf(current);
                if (!parent || parent->Begin == current.Begin)
                    break;
                root = parent->Begin;
                current = *parent;
            }
            if (root != entry.Begin)
            {
                const auto found = roots.find(root);
                if (found != roots.end() && functions[found->second].second == entry.Begin)
                {
                    functions[found->second].second = entry.End;
                    continue;
                }
            }
            roots[entry.Begin] = functions.size();
            functions.emplace_back(entry.Begin, entry.End);
        }
        return functions;
    }

    std::shared_ptr<const CodeMap::Table> CodeMap::TableFor(std::uint64_t address) const
    {
        std::shared_ptr<Process> process;
        {
            std::lock_guard lock(m_lock);
            if (!m_process)
                return nullptr;
            if (m_last && address >= m_last->Base && address - m_last->Base < m_last->Size)
                return m_last;
            process = m_process;
        }
        const ModuleList modules = process->Modules();
        const ModuleInfo* module = nullptr;
        for (const ModuleInfo& candidate : *modules)
        {
            if (candidate.Contains(address))
            {
                module = &candidate;
                break;
            }
        }
        if (!module)
            return nullptr;
        {
            std::lock_guard lock(m_lock);
            if (const auto found = m_tables.find(module->Base); found != m_tables.end() && found->second->Size == module->Size)
            {
                m_last = found->second;
                return m_last;
            }
        }
        // A 32-bit process has no unwind table; the empty table still marks the
        // module, so its addresses are not handed to the Mono lookup.
        std::shared_ptr<const Table> table = LoadTable(module->Base, module->Size);
        std::lock_guard lock(m_lock);
        m_tables[module->Base] = table;
        m_last = table;
        return table;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> CodeMap::FunctionAt(std::uint64_t address) const
    {
        const auto table = TableFor(address);
        if (!table || table->Functions.empty())
            return std::nullopt;
        const auto rva = static_cast<std::uint32_t>(address - table->Base);
        auto it = std::upper_bound(table->Functions.begin(), table->Functions.end(), rva,
            [](std::uint32_t value, const Function& function) { return value < function.Begin; });
        if (it == table->Functions.begin())
            return std::nullopt;
        --it;
        if (rva >= it->End)
            return std::nullopt;
        return std::make_pair(table->Base + it->Begin, table->Base + it->End);
    }

    std::size_t CodeMap::FunctionCount(std::uint64_t moduleBase) const
    {
        const auto table = TableFor(moduleBase);
        return table ? table->Functions.size() : 0;
    }

    std::optional<CodeMap::ImageSpan> CodeMap::SectionAt(std::uint64_t address) const
    {
        const auto table = TableFor(address);
        if (!table)
            return std::nullopt;
        const std::uint64_t rva = address - table->Base;
        for (const Section& section : table->Sections)
        {
            if (rva >= section.Begin && rva < section.End)
                return ImageSpan{ table->Base + section.Begin, table->Base + section.End, section.Code };
        }
        const std::uint64_t headers = table->Sections.empty() ? std::min(kPage, table->Size) : table->Sections.front().Begin;
        if (rva < headers)
            return ImageSpan{ table->Base, table->Base + headers, false };
        return std::nullopt;
    }

    std::optional<std::uint64_t> CodeMap::NamedIn(std::uint64_t from, std::uint64_t to) const
    {
        const Symbols* symbols = nullptr;
        {
            std::lock_guard lock(m_lock);
            symbols = m_symbols;
        }
        return symbols ? symbols->NamedIn(from, to) : std::nullopt;
    }

    std::optional<std::uint64_t> CodeMap::NamedBelow(std::uint64_t address, std::uint64_t floor) const
    {
        const Symbols* symbols = nullptr;
        {
            std::lock_guard lock(m_lock);
            symbols = m_symbols;
        }
        return symbols ? symbols->NamedBelow(address, floor) : std::nullopt;
    }

    std::optional<std::pair<std::uint64_t, std::uint64_t>> CodeMap::MonoRange(std::uint64_t address) const
    {
        MonoBridge* mono = nullptr;
        {
            std::lock_guard lock(m_lock);
            mono = m_mono;
        }
        if (!mono || !mono->Present())
            return std::nullopt;
        return mono->MethodRange(address);
    }

    bool CodeMap::AllocationStart(std::uint64_t address) const
    {
        std::shared_ptr<Process> process;
        {
            std::lock_guard lock(m_lock);
            process = m_process;
        }
        if (!process)
            return false;
        const auto region = process->RegionAt(address);
        return region && region->Committed() && region->AllocationBase == address;
    }

    std::optional<std::uint64_t> CodeMap::BoundaryIn(std::uint64_t from, std::uint64_t to) const
    {
        return BoundaryIn(from, to, Anchors::CodeAndNames);
    }

    std::optional<std::uint64_t> CodeMap::StartBelow(std::uint64_t address, std::uint64_t reach) const
    {
        return StartBelow(address, reach, Anchors::CodeAndNames);
    }

    std::optional<std::uint64_t> CodeMap::BoundaryIn(std::uint64_t from, std::uint64_t to, Anchors anchors) const
    {
        if (to <= from + 1)
            return std::nullopt;
        std::optional<std::uint64_t> best;
        const auto consider = [&](std::uint64_t value)
            {
                if (value > from && value < to && (!best || value < *best))
                    best = value;
            };
        bool inside = false;
        if (const auto table = TableFor(from))
        {
            // Inside a module: the next function start, or the end of the
            // function the window begins in. JIT code is never inside a module,
            // so the Mono runtime is not asked here.
            const auto rva = static_cast<std::uint32_t>(from - table->Base);
            auto next = std::upper_bound(table->Functions.begin(), table->Functions.end(), rva,
                [](std::uint32_t value, const Function& function) { return value < function.Begin; });
            if (next != table->Functions.end())
                consider(table->Base + next->Begin);
            if (next != table->Functions.begin())
            {
                const Function& holder = *std::prev(next);
                if (rva < holder.End)
                {
                    consider(table->Base + holder.End);
                    inside = true;
                }
            }
        }
        else
        {
            // A window is at most 16 bytes and every method is longer, so a method
            // that starts inside it still holds the window's last byte.
            if (const auto range = MonoRange(from))
            {
                consider(range->second);
                inside = true;
            }
            if (const auto range = MonoRange(to - 1))
                consider(range->first);
        }
        if (anchors == Anchors::CodeAndNames && !inside)
        {
            if (const auto named = NamedIn(from, to))
                consider(*named);
        }
        const std::uint64_t page = (to - 1) & ~(kPage - 1);
        if (page > from && AllocationStart(page))
            consider(page);
        return best;
    }

    std::optional<std::uint64_t> CodeMap::StartBelow(std::uint64_t address, std::uint64_t reach, Anchors anchors) const
    {
        if (address == 0)
            return std::nullopt;
        const std::uint64_t floor = address > reach ? address - reach : 0;
        const std::uint64_t last = address - 1;
        std::optional<std::uint64_t> best;
        const auto consider = [&](std::uint64_t value)
            {
                if (value < address && value >= floor && (!best || value > *best))
                    best = value;
            };
        if (const auto table = TableFor(last))
        {
            // The function holding the byte before the address starts the sweep;
            // in a gap (padding, or a leaf function without an entry) the end of
            // the function below it does.
            const auto rva = static_cast<std::uint32_t>(last - table->Base);
            auto it = std::upper_bound(table->Functions.begin(), table->Functions.end(), rva,
                [](std::uint32_t value, const Function& function) { return value < function.Begin; });
            if (it != table->Functions.begin())
            {
                const Function& holder = *std::prev(it);
                if (rva < holder.End)
                {
                    consider(table->Base + holder.Begin);
                    return best;
                }
                consider(table->Base + holder.End);
            }
            else
            {
                consider(table->Base);
            }
            if (anchors == Anchors::CodeAndNames)
            {
                if (const auto named = NamedBelow(address, floor))
                    consider(*named);
            }
            return best;
        }
        bool inside = false;
        if (const auto range = MonoRange(last))
        {
            consider(range->first);
            inside = true;
        }
        else
        {
            for (std::uint64_t back = 1 + kMonoStride; back <= kMonoProbe && back <= last; back += kMonoStride)
            {
                if (const auto below = MonoRange(address - back))
                {
                    consider(below->second);
                    break;
                }
            }
        }
        if (const auto run = RunStart(last, floor))
            consider(*run);
        if (anchors == Anchors::CodeAndNames && !inside)
        {
            if (const auto named = NamedBelow(address, floor))
                consider(*named);
        }
        return best;
    }

    std::optional<std::uint64_t> CodeMap::RunStart(std::uint64_t address, std::uint64_t floor) const
    {
        std::shared_ptr<Process> process;
        {
            std::lock_guard lock(m_lock);
            process = m_process;
        }
        if (!process)
            return std::nullopt;
        // The first byte of an allocation, or the first readable page after an
        // unreadable gap in it, is where its code begins. A region query reports
        // from the page of the address onward (not where the region starts, and
        // a JIT chunk flipping pages between RW and RX is split anyway), so the
        // start is found page by page, no further down than the sweep reaches.
        const auto region = process->RegionAt(address);
        if (!region || !region->Readable())
            return std::nullopt;
        const std::uint64_t allocation = region->AllocationBase;
        std::uint64_t page = address & ~(kPage - 1);
        while (page > allocation)
        {
            if (page < floor)
                return std::nullopt;
            const auto previous = process->RegionAt(page - 1);
            if (!previous || !previous->Readable() || previous->AllocationBase != allocation)
                return page;
            page -= kPage;
        }
        return page;
    }
}
