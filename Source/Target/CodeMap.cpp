#include "Target/CodeMap.h"

#include "Target/Mono.h"
#include "Target/Process.h"

#include <algorithm>
#include <iterator>

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

        struct UnwindEntry
        {
            std::uint32_t Begin;
            std::uint32_t End;
            std::uint32_t Unwind;
        };
        static_assert(sizeof(UnwindEntry) == 12);
    }

    void CodeMap::Attach(std::shared_ptr<Process> process, MonoBridge* mono)
    {
        std::lock_guard lock(m_lock);
        m_process = std::move(process);
        m_mono = mono;
        m_tables.clear();
        m_last.reset();
    }

    void CodeMap::Detach()
    {
        Attach(nullptr, nullptr);
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
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!process->ReadExact(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return table;
        if (!process->ReadExact(base + static_cast<std::uint64_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
            return table;
        const IMAGE_DATA_DIRECTORY directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(UnwindEntry) || directory.Size > kMaxUnwindBytes ||
            static_cast<std::uint64_t>(directory.VirtualAddress) + directory.Size > size)
            return table;
        std::vector<UnwindEntry> entries(directory.Size / sizeof(UnwindEntry));
        if (!process->ReadExact(base + directory.VirtualAddress, entries.data(), entries.size() * sizeof(UnwindEntry)))
            return table;
        table->Functions.reserve(entries.size());
        for (const UnwindEntry& entry : entries)
        {
            if (entry.Begin < entry.End && entry.End <= size)
                table->Functions.push_back({ entry.Begin, entry.End });
        }
        std::sort(table->Functions.begin(), table->Functions.end(), [](const Function& a, const Function& b) { return a.Begin < b.Begin; });
        return table;
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
        std::shared_ptr<const Table> table = process->Is64Bit() ? LoadTable(module->Base, module->Size) : nullptr;
        if (!table)
        {
            auto empty = std::make_shared<Table>();
            empty->Base = module->Base;
            empty->Size = module->Size;
            table = std::move(empty);
        }
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
        if (to <= from + 1)
            return std::nullopt;
        std::optional<std::uint64_t> best;
        const auto consider = [&](std::uint64_t value)
            {
                if (value > from && value < to && (!best || value < *best))
                    best = value;
            };
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
                    consider(table->Base + holder.End);
            }
        }
        else
        {
            // A window is at most 16 bytes and every method is longer, so a method
            // that starts inside it still holds the window's last byte.
            if (const auto range = MonoRange(from))
                consider(range->second);
            if (const auto range = MonoRange(to - 1))
                consider(range->first);
        }
        const std::uint64_t page = (to - 1) & ~(kPage - 1);
        if (page > from && AllocationStart(page))
            consider(page);
        return best;
    }

    std::optional<std::uint64_t> CodeMap::StartBelow(std::uint64_t address, std::uint64_t reach) const
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
                consider(table->Base + (rva < holder.End ? holder.Begin : holder.End));
            }
            return best;
        }
        if (const auto range = MonoRange(last))
        {
            consider(range->first);
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
