#pragma once

#include "Disasm/Disassembler.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Manifold::Target
{
    class Process;
    class MonoBridge;

    // Where code is known to start, for the disassembler: the functions in a
    // 64-bit module's unwind table (.pdata), the JIT code ranges of Mono methods
    // and the first byte of every allocation. These give a listing fixed points,
    // so scrolling through padding, trampolines or data between functions does
    // not shift the instruction grid into the next function's prolog.
    class CodeMap final : public Disasm::CodeHints
    {
    public:
        void Attach(std::shared_ptr<Process> process, MonoBridge* mono);
        void Detach();

        std::optional<std::uint64_t> BoundaryIn(std::uint64_t from, std::uint64_t to) const override;
        std::optional<std::uint64_t> StartBelow(std::uint64_t address, std::uint64_t reach) const override;

        // The [begin, end) range of the function in a module's unwind table that
        // holds an address. Leaf functions have no entry, so they are not found.
        std::optional<std::pair<std::uint64_t, std::uint64_t>> FunctionAt(std::uint64_t address) const;
        std::size_t FunctionCount(std::uint64_t moduleBase) const;

        // The [begin, end) RVAs of a 64-bit module's unwind table, sorted by
        // begin; empty for a 32-bit module or one without the table.
        static std::vector<std::pair<std::uint32_t, std::uint32_t>> UnwindFunctions(const Process& process, std::uint64_t base, std::uint64_t size);
        static std::vector<std::pair<std::uint32_t, std::uint32_t>> FunctionsFromUnwind(std::span<const std::uint8_t> table, std::uint32_t tableRva, std::span<const std::uint8_t> info,
            std::uint32_t infoRva, std::uint64_t size);

    private:
        struct Function
        {
            std::uint32_t Begin = 0;
            std::uint32_t End = 0;
        };
        struct Table
        {
            std::uint64_t Base = 0;
            std::uint64_t Size = 0;
            std::vector<Function> Functions;
        };

        std::shared_ptr<const Table> TableFor(std::uint64_t address) const;
        std::shared_ptr<const Table> LoadTable(std::uint64_t base, std::uint64_t size) const;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> MonoRange(std::uint64_t address) const;
        bool AllocationStart(std::uint64_t address) const;
        std::optional<std::uint64_t> RunStart(std::uint64_t address, std::uint64_t floor) const;

        std::shared_ptr<Process> m_process;
        MonoBridge* m_mono = nullptr;
        mutable std::mutex m_lock;
        mutable std::unordered_map<std::uint64_t, std::shared_ptr<const Table>> m_tables;
        mutable std::shared_ptr<const Table> m_last;
    };
}
