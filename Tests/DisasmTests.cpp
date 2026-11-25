#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Asm/Assembler.h"
#include "Asm/MnemonicDocs.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Lua/LuaEngine.h"
#include "Target/CodeMap.h"
#include "Target/Process.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        // Method ranges the way the Mono runtime reports them.
        class RangeHints final : public Disasm::CodeHints
        {
        public:
            std::vector<std::pair<std::uint64_t, std::uint64_t>> Ranges;

            std::optional<std::uint64_t> BoundaryIn(std::uint64_t from, std::uint64_t to) const override
            {
                std::optional<std::uint64_t> best;
                for (const auto& [start, end] : Ranges)
                {
                    for (const std::uint64_t value : { start, end })
                    {
                        if (value > from && value < to && (!best || value < *best))
                            best = value;
                    }
                }
                return best;
            }

            std::optional<std::uint64_t> StartBelow(std::uint64_t address, std::uint64_t reach) const override
            {
                std::optional<std::uint64_t> best;
                const std::uint64_t last = address - 1;
                for (const auto& [start, end] : Ranges)
                {
                    const std::uint64_t value = start <= last && last < end ? start : end;
                    if (value <= last && address - value <= reach && (!best || value > *best))
                        best = value;
                }
                return best;
            }
        };

        // The prolog and first lines of a JIT-compiled Mono method, and the
        // trampoline and zero padding that sat in front of it in the owner's
        // screenshot (GameplayData:CoinsSet).
        const std::uint8_t kMethod[] = {
            0x55,
            0x48, 0x8B, 0xEC,
            0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00,
            0x48, 0x89, 0x75, 0xF8,
            0x48, 0x89, 0x4D, 0xA0,
            0x66, 0x66, 0x90,
            0x49, 0xBB, 0xF0, 0xD5, 0x2A, 0xA5, 0x31, 0x01, 0x00, 0x00,
            0x41, 0xFF, 0xD3,
            0x48, 0x8B, 0xF0,
            0x48, 0x85, 0xC0,
            0xC3,
        };
        const std::uint64_t kMethodStarts[] = { 0x0, 0x1, 0x4, 0xB, 0xF, 0x13, 0x16, 0x20, 0x23, 0x26, 0x29 };
        const std::uint8_t kTrampoline[] = {
            0x49, 0xBB, 0x20, 0x02, 0xFF, 0x39, 0x2F, 0x01, 0x00, 0x00,
            0x41, 0xFF, 0xD3,
            0x08, 0x58, 0x6D, 0xF5, 0xA4, 0x31, 0x01, 0x00, 0x00,
        };
        const std::uint8_t kSmallMethod[] = {
            0x55, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3,
        };

        constexpr std::uint64_t kChunk = 0x10000;
        constexpr std::uint64_t kSmallAt = 0x100;
        constexpr std::uint64_t kTrampolineAt = 0x1BD;
        constexpr std::uint64_t kMethodAt = 0x1E0;

        // Two allocations back to back, both readable: like two Mono code chunks.
        bool AdjacentChunks(std::uint8_t*& first, std::uint8_t*& second)
        {
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                void* probe = ::VirtualAlloc(nullptr, kChunk * 2, MEM_RESERVE, PAGE_NOACCESS);
                if (!probe)
                    return false;
                ::VirtualFree(probe, 0, MEM_RELEASE);
                auto* a = static_cast<std::uint8_t*>(::VirtualAlloc(probe, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                auto* b = a ? static_cast<std::uint8_t*>(::VirtualAlloc(static_cast<std::uint8_t*>(probe) + kChunk, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : nullptr;
                if (a && b)
                {
                    first = a;
                    second = b;
                    return true;
                }
                if (a)
                    ::VirtualFree(a, 0, MEM_RELEASE);
            }
            return false;
        }

        void Resync(Engine& engine, std::uint8_t* first, std::uint8_t* second)
        {
            const Target::Process& process = *engine.Process();
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            std::memset(first, 0, kChunk);
            std::memset(second, 0, kChunk);
            std::memcpy(second + kSmallAt, kSmallMethod, sizeof(kSmallMethod));
            std::memcpy(second + kTrampolineAt, kTrampoline, sizeof(kTrampoline));
            std::memcpy(second + kMethodAt, kMethod, sizeof(kMethod));
            const auto base = reinterpret_cast<std::uint64_t>(second);
            const std::uint64_t method = base + kMethodAt;
            RangeHints hints;
            hints.Ranges = { { base + kSmallAt, base + kSmallAt + sizeof(kSmallMethod) }, { method, method + sizeof(kMethod) } };

            // The bug: decoding from one byte too early, "00 55 48" swallows push rbp.
            const auto blind = disassembler.ReadMany(process, method - 1, 4);
            CHECK(blind.size() == 4 && blind[0].Length == 3);
            CHECK(std::none_of(blind.begin(), blind.end(), [&](const Disasm::Instruction& line) { return line.Address == method; }));

            // With the method start known, the stray byte becomes its own line.
            const auto cut = disassembler.ReadMany(process, method - 1, 4, nullptr, &hints);
            CHECK(cut.size() == 4 && cut[0].Length == 1 && cut[0].Mnemonic == "db");
            CHECK(cut.size() == 4 && cut[1].Address == method && cut[1].Mnemonic == "push");
            CHECK(cut.size() == 4 && cut[2].Address == method + 1 && cut[2].Text() == "mov rbp,rsp");

            // Scrolling up from the prolog by any number of lines lands on a top
            // whose listing still meets the method at its first byte.
            bool everyTopMeetsMethod = true;
            bool everyStepExact = true;
            for (int lines = 1; lines <= 60; ++lines)
            {
                const std::uint64_t top = disassembler.BackUp(process, method, lines, &hints);
                const auto listing = disassembler.ReadMany(process, top, lines + 1, nullptr, &hints);
                if (std::none_of(listing.begin(), listing.end(), [&](const Disasm::Instruction& line) { return line.Address == method; }))
                    everyTopMeetsMethod = false;
                if (listing.size() != static_cast<std::size_t>(lines + 1) || listing.back().Address != method)
                    everyStepExact = false;
            }
            CHECK(everyTopMeetsMethod);
            CHECK(everyStepExact);

            // Inside the method the lines above come from its start, not a guess.
            CHECK_EQ(disassembler.BackUp(process, method + kMethodStarts[6], 3, &hints), method + kMethodStarts[3]);
            CHECK_EQ(disassembler.BackUp(process, method + kMethodStarts[10], 10, &hints), method);
            CHECK_EQ(disassembler.BackUp(process, method + kMethodStarts[2], 4, &hints) < method, true);
            const auto starts = disassembler.Starts(process, method, method + sizeof(kMethod), &hints);
            CHECK(starts.size() == std::size(kMethodStarts) && std::equal(starts.begin(), starts.end(), std::begin(kMethodStarts),
                [&](std::uint64_t at, std::uint64_t offset) { return at == method + offset; }));

            // The neighbouring readable allocation is reachable: no region floor.
            CHECK(disassembler.BackUp(process, base, 3) < base);
            CHECK(disassembler.BackUp(process, base, 3, &hints) < base);
            // A page start is no floor either (a region query reports the page of
            // the address as its base, which stopped every scroll there before).
            const auto page = reinterpret_cast<std::uint64_t>(first) + 0x1000;
            CHECK_EQ(disassembler.BackUp(process, page, 3), page - 6);
            CHECK_EQ(disassembler.BackUp(process, disassembler.BackUp(process, page, 1), 3), page - 8);
        }

        void UnreadableFloor(Engine& engine)
        {
            // Reserved but not committed memory in front: the listing starts at
            // the allocation's committed part, as injected code does.
            auto* reserve = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, kChunk * 2, MEM_RESERVE, PAGE_NOACCESS));
            CHECK(reserve != nullptr);
            if (!reserve)
                return;
            auto* committed = static_cast<std::uint8_t*>(::VirtualAlloc(reserve + kChunk, kChunk, MEM_COMMIT, PAGE_READWRITE));
            CHECK(committed != nullptr);
            if (committed)
            {
                std::memcpy(committed, kMethod, sizeof(kMethod));
                const auto start = reinterpret_cast<std::uint64_t>(committed);
                const Disasm::Disassembler& disassembler = engine.Disassembler();
                CHECK_EQ(disassembler.BackUp(*engine.Process(), start, 5), start);
                CHECK_EQ(disassembler.BackUp(*engine.Process(), start, 5, engine.CodeMap()), start);
                CHECK_EQ(disassembler.BackUp(*engine.Process(), start + kMethodStarts[4], 3, engine.CodeMap()), start + kMethodStarts[1]);
                const Target::CodeMap& map = *engine.CodeMap();
                CHECK_EQ(map.StartBelow(start + 0x20, 0x10000).value_or(0), start);
            }
            ::VirtualFree(reserve, 0, MEM_RELEASE);
        }

        void UnwindTable(Engine& engine, std::uint8_t* first, std::uint8_t* second)
        {
            const Target::CodeMap* map = engine.CodeMap();
            CHECK(map != nullptr);
            if (!map)
                return;
            // This test function saves registers and calls others, so it has an
            // entry in the test executable's unwind table.
            const auto function = reinterpret_cast<std::uint64_t>(&UnwindTable);
            const auto range = map->FunctionAt(function + 1);
            CHECK(range && range->first == function && range->second > function + 1);
            CHECK_EQ(map->BoundaryIn(function - 1, function + 8).value_or(0), function);
            CHECK_EQ(map->StartBelow(function + 8, 0x10000).value_or(0), function);
            if (const auto self = engine.Process()->ModuleAt(function))
                CHECK(map->FunctionCount(self->Base) > 100);

            // Allocation starts are fixed points too.
            const auto a = reinterpret_cast<std::uint64_t>(first);
            const auto b = reinterpret_cast<std::uint64_t>(second);
            CHECK_EQ(map->BoundaryIn(b - 4, b + 8).value_or(0), b);
            CHECK_EQ(map->StartBelow(b + 0x20, 0x10000).value_or(0), b);
            // A region query starts at the page of the address, so the start of
            // the allocation is found page by page, also across a page whose
            // protection differs (a JIT chunk switching pages between RW and RX).
            DWORD old = 0;
            CHECK(::VirtualProtect(first + 0x8000, 0x1000, PAGE_READONLY, &old));
            const std::uint64_t below = map->StartBelow(b, 0x10000).value_or(0);
            CHECK_MSG(below == a, Hex(below) + " for " + Hex(a));
            CHECK_EQ(map->StartBelow(b - 0x100, 0x10000).value_or(0), a);
            CHECK_EQ(map->StartBelow(a + 0x9010, 0x10000).value_or(0), a);
            CHECK_EQ(map->StartBelow(b, 0x4000).has_value(), false);
            CHECK_EQ(engine.Disassembler().BackUp(*engine.Process(), b, 3, map), b - 6);
            CHECK_EQ(engine.Disassembler().BackUp(*engine.Process(), a + 0x9000, 0x4800, map), a);
            ::VirtualProtect(first + 0x8000, 0x1000, old, &old);
        }
    }

    namespace
    {
        struct LiveCounts
        {
            int Checked = 0;
            int Failed = 0;
        };

        void PrintListing(Engine& engine, std::uint64_t from, std::uint64_t until)
        {
            const Target::Process& process = *engine.Process();
            const auto lines = engine.Disassembler().ReadMany(process, from, static_cast<int>(until - from) + 1, nullptr, engine.CodeMap());
            for (const Disasm::Instruction& line : lines)
            {
                if (line.Address >= until)
                    break;
                if (const auto name = engine.Mono().MethodStartNameAt(line.Address))
                    std::printf("        ---- %s\n", name->c_str());
                std::printf("  %s  %-24s %s\n", Hex(line.Address, 12).c_str(), line.BytesText().c_str(), line.Text().c_str());
            }
        }

        // The checks of the synthetic test, run on real code: misaligned tops,
        // backing up by any number of lines, and a long mouse-wheel scroll.
        void CheckAround(Engine& engine, const std::string& label, std::uint64_t start, int maxLines, int wheelSteps, LiveCounts& counts)
        {
            const Target::Process& process = *engine.Process();
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            const Target::CodeMap* map = engine.CodeMap();
            int blindMisses = 0;
            int misses = 0;
            for (std::uint64_t back = 1; back <= 0x40; ++back)
            {
                const auto meets = [&](const std::vector<Disasm::Instruction>& lines)
                    { return std::any_of(lines.begin(), lines.end(), [&](const Disasm::Instruction& line) { return line.Address == start; }); };
                if (!meets(disassembler.ReadMany(process, start - back, 48)))
                    ++blindMisses;
                if (!meets(disassembler.ReadMany(process, start - back, 48, nullptr, map)))
                    ++misses;
            }
            int inexact = 0;
            for (int lines = 1; lines <= maxLines; ++lines)
            {
                const std::uint64_t top = disassembler.BackUp(process, start, lines, map);
                const auto listing = disassembler.ReadMany(process, top, lines + 1, nullptr, map);
                if (listing.size() != static_cast<std::size_t>(lines + 1) || listing.back().Address != start)
                    ++inexact;
            }
            int stuck = 0;
            int brokenSteps = 0;
            int skippedStarts = 0;
            int monoStarts = 0;
            std::uint64_t top = start;
            const auto begin = std::chrono::steady_clock::now();
            for (int step = 0; step < wheelSteps; ++step)
            {
                const std::uint64_t next = disassembler.BackUp(process, top, 3, map);
                if (next >= top)
                {
                    ++stuck;
                    break;
                }
                const auto lines = disassembler.ReadMany(process, next, static_cast<int>(top - next) + 1, nullptr, map);
                if (std::none_of(lines.begin(), lines.end(), [&](const Disasm::Instruction& line) { return line.Address == top; }))
                    ++brokenSteps;
                for (const Disasm::Instruction& line : lines)
                {
                    if (line.Address >= top)
                        break;
                    if (const auto range = engine.Mono().MethodRange(line.Address); range && range->first > next && range->first < top)
                    {
                        ++monoStarts;
                        if (std::none_of(lines.begin(), lines.end(), [&](const Disasm::Instruction& other) { return other.Address == range->first; }))
                            ++skippedStarts;
                    }
                }
                top = next;
            }
            const double wheelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            const auto frameBegin = std::chrono::steady_clock::now();
            for (int frame = 0; frame < 100; ++frame)
                disassembler.ReadMany(process, top, 46, &engine.Symbols(), map);
            const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameBegin).count() / 100.0;
            std::printf("%s at %s\n", label.c_str(), Hex(start, 12).c_str());
            std::printf("  misaligned tops 1..64 bytes above: %d miss the start without hints, %d with\n", blindMisses, misses);
            std::printf("  backing up 1..%d lines: %d listings miss the start\n", maxLines, inexact);
            std::printf("  wheel: %d steps of 3 lines up to %s, stuck %d, steps not meeting the old top %d, method starts passed %d (skipped %d), %.2f ms per step\n",
                wheelSteps, Hex(top, 12).c_str(), stuck, brokenSteps, monoStarts, skippedStarts, wheelMs / std::max(1, wheelSteps));
            std::printf("  one listing frame (46 lines with symbols): %.3f ms\n", frameMs);
            counts.Checked += 4;
            counts.Failed += (misses ? 1 : 0) + (inexact ? 1 : 0) + (stuck ? 1 : 0) + ((brokenSteps || skippedStarts) ? 1 : 0);
        }
    }

    // `--disasm-check --process=CloverPit [--method=Class:Method]...` runs the
    // resync checks on a live Mono game. It starts the Mono worker thread in
    // the target (like the Mono window does) and otherwise only reads.
    int RunDisasmLive(const std::vector<std::string>& arguments)
    {
        std::string name;
        std::vector<std::string> methods;
        for (const std::string& argument : arguments)
        {
            if (argument.rfind("--process=", 0) == 0)
                name = argument.substr(10);
            else if (argument.rfind("--method=", 0) == 0)
                methods.push_back(argument.substr(9));
        }
        if (methods.empty())
            methods = { "GameplayData:CoinsSet", "GameplayData:CoinsAdd" };
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        Settings settings;
        Engine engine(settings);
        const Status opened = engine.OpenProcessByName(name);
        if (!opened)
        {
            std::printf("could not open %s: %s\n", name.c_str(), opened.Text.c_str());
            return 2;
        }
        const Status mono = engine.Managed().Ensure();
        if (!mono)
        {
            std::printf("no Mono: %s\n", mono.Text.c_str());
            engine.CloseProcess();
            return 2;
        }
        std::printf("opened %s (pid %lu), Mono in %s\n", engine.Process()->Name().c_str(), static_cast<unsigned long>(engine.Process()->Id()), engine.Mono().Module().c_str());
        LiveCounts counts;
        for (const std::string& method : methods)
        {
            const auto address = engine.Mono().AddressOf(method);
            if (!address)
            {
                std::printf("%s: not found\n", method.c_str());
                ++counts.Failed;
                continue;
            }
            std::printf("\n");
            PrintListing(engine, engine.Disassembler().BackUp(*engine.Process(), *address, 30, engine.CodeMap()), *address + 0x18);
            CheckAround(engine, method, *address, 150, 200, counts);
        }
        if (const auto module = engine.Process()->FindModule(engine.Mono().Module()))
        {
            const std::vector<Target::ExportSymbol> exports = engine.Symbols().ExportsOf(*module);
            std::printf("\n%s: %zu functions in the unwind table, %zu exports\n", module->Name.c_str(), engine.CodeMap()->FunctionCount(module->Base), exports.size());
            int sampled = 0;
            for (std::size_t i = 0; i < exports.size() && sampled < 6; i += std::max<std::size_t>(1, exports.size() / 6))
            {
                const std::uint64_t function = exports[i].Address;
                const auto range = engine.CodeMap()->FunctionAt(function);
                if (!range || range->first != function)
                    continue;
                ++sampled;
                CheckAround(engine, module->Name + "!" + exports[i].Name, function, 60, 60, counts);
            }
        }
        engine.CloseProcess();
        std::printf("\n%d live checks, %d failed\n", counts.Checked, counts.Failed);
        return counts.Failed;
    }

    namespace
    {
        void ChainedUnwind()
        {
            struct Entry
            {
                std::uint32_t Begin;
                std::uint32_t End;
                std::uint32_t Unwind;
            };
            const Entry table[] = { { 0x1000, 0x1040, 0x5000 }, { 0x1040, 0x1100, 0x5010 }, { 0x1100, 0x1180, 0x5030 }, { 0x1200, 0x1300, 0x5050 }, { 0x2000, 0x2010, 0x5060 } };
            std::vector<std::uint8_t> info(0x100, 0);
            const auto put = [&info](std::uint32_t at, std::uint8_t flags, std::uint8_t codes, const Entry* parent)
                {
                    const std::size_t offset = at - 0x5000;
                    info[offset] = static_cast<std::uint8_t>(1 | (flags << 3));
                    info[offset + 2] = codes;
                    if (parent)
                        std::memcpy(info.data() + offset + 4 + ((codes + 1u) & ~1u) * 2, parent, sizeof(Entry));
                };
            put(0x5000, 0, 2, nullptr);
            put(0x5010, 4, 0, &table[0]);
            put(0x5030, 4, 1, &table[1]);
            put(0x5050, 0, 0, nullptr);
            put(0x5060, 4, 0, &table[0]);
            const auto functions = Target::CodeMap::FunctionsFromUnwind({ reinterpret_cast<const std::uint8_t*>(table), sizeof(table) }, 0x4000, info, 0x5000, 0x10000);
            const std::vector<std::pair<std::uint32_t, std::uint32_t>> expected = { { 0x1000, 0x1180 }, { 0x1200, 0x1300 }, { 0x2000, 0x2010 } };
            CHECK(functions == expected);
            const std::uint8_t indirect[] = { 0x00, 0x30, 0x00, 0x00, 0x40, 0x30, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00 };
            std::vector<std::uint8_t> both(std::begin(indirect), std::end(indirect));
            const Entry rootEntry = { 0x2F00, 0x3000, 0x5000 };
            both.insert(both.begin(), reinterpret_cast<const std::uint8_t*>(&rootEntry), reinterpret_cast<const std::uint8_t*>(&rootEntry) + sizeof(rootEntry));
            const auto chainedByPointer = Target::CodeMap::FunctionsFromUnwind(both, 0x4000, info, 0x5000, 0x10000);
            CHECK(chainedByPointer == (std::vector<std::pair<std::uint32_t, std::uint32_t>>{ { 0x2F00, 0x3040 } }));
        }

        // ∑ The watch window wrote RAX ... RIP for a 32-bit game (Terraria). A 32-bit target has EAX ... EIP,
        //   8 digits and no R8 to R15.
        void RegisterNames()
        {
            Disasm::RegisterValues r;
            r.Rax = 0x47733BE8;
            r.Rcx = 0x47733BBC;
            r.Rsp = 0x022FE110;
            r.Rip = 0x3052714F;
            r.R8 = 0x1234;
            std::string narrow;
            const std::vector<std::string> lines32 = Disasm::RegisterLines(r, false);
            for (const std::string& line : lines32)
                narrow += line + "\n";
            CHECK_MSG(lines32.size() == 3, narrow);
            CHECK_MSG(narrow.find("EAX 47733BE8") != std::string::npos && narrow.find("ECX 47733BBC") != std::string::npos && narrow.find("ESP 022FE110") != std::string::npos, narrow);
            CHECK_MSG(narrow.find("EIP 3052714F") != std::string::npos, narrow);
            CHECK_MSG(narrow.find('R') == std::string::npos, "a 64-bit register name for a 32-bit target: " + narrow);
            std::string wide;
            const std::vector<std::string> lines64 = Disasm::RegisterLines(r, true);
            for (const std::string& line : lines64)
                wide += line + "\n";
            CHECK_MSG(lines64.size() == 5 && wide.find("RAX 0000000047733BE8") != std::string::npos && wide.find("R8  0000000000001234") != std::string::npos &&
                wide.find("RIP 000000003052714F") != std::string::npos, wide);
        }

        void InstructionHelp()
        {
            CHECK(Asm::MnemonicSummary("MOV").rfind("Copies the source", 0) == 0);
            CHECK(Asm::MnemonicSummary("jne") == "Jumps if not equal (ZF = 0), else goes on.");
            CHECK(Asm::MnemonicSummary("jg").find("greater, signed") != std::string::npos);
            CHECK(Asm::MnemonicSummary("setb").rfind("Sets the byte to 1 if below", 0) == 0);
            CHECK(Asm::MnemonicSummary("cmovz").find("zero (ZF = 1)") != std::string::npos);
            CHECK(Asm::MnemonicSummary("addss") == "Adds the low float (32-bit).");
            CHECK(Asm::MnemonicSummary("mulpd") == "Multiplies all doubles (64-bit).");
            CHECK(Asm::MnemonicSummary("vpaddd") == "AVX form of `paddd`: Adds dwords.");
            CHECK(Asm::MnemonicSummary("paddusw") == "Adds words with unsigned saturation.");
            CHECK(Asm::MnemonicSummary("pcmpgtq") == "Compares signed qwords; greater lanes become all ones.");
            CHECK(Asm::MnemonicSummary("pmovzxbd") == "Zero-extends packed bytes to dwords.");
            CHECK(Asm::MnemonicSummary("vpmovqd").find("Narrows qwords to dwords") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vpermt2w").find("Picks words from two tables") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vpshldvq").find("filling from a second source") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vpcompressd").find("Packs the dwords selected by a mask") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vpcomuw").find("AMD XOP") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vpshlb").find("AMD XOP") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vfmadd231ps").find("dst = src2*src3 + dst") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vfnmsub132sd").find("dst = -(dst*src3) - src2") != std::string::npos);
            CHECK(Asm::MnemonicSummary("kortestw") == "ORs two mask registers and sets ZF and CF (AVX-512, 16 bits).");
            CHECK(Asm::MnemonicSummary("fisubr") == "Subtracts st0 from an integer in memory, into st0.");
            CHECK(Asm::MnemonicSummary("faddp") == "Adds two x87 values and pops.");
            CHECK(Asm::MnemonicSummary("fcmovnbe").find("above") != std::string::npos);
            CHECK(Asm::MnemonicSummary("ccmpnle").find("greater, signed") != std::string::npos);
            CHECK(Asm::MnemonicSummary("cmpbexadd").find("below or equal") != std::string::npos);
            CHECK(Asm::MnemonicSummary("lodsq") == "Loads the qword at [rsi] into rax and advances rsi.");
            CHECK(Asm::MnemonicSummary("pushfq") == "Pushes the 64-bit flags register.");
            CHECK(Asm::MnemonicSummary("vmovdqa32").find("AVX-512 form of `movdqa` for 32-bit elements") != std::string::npos);
            CHECK(Asm::MnemonicSummary("sha256rnds2").find("SHA") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vmlaunch").find("virtualization") != std::string::npos);
            CHECK(Asm::MnemonicSummary("vmovss").find("AVX form of `movss`") == 0);
            CHECK(Asm::MnemonicSummary("xorps").find("sets the register to zero") != std::string::npos);
            CHECK(Asm::MnemonicSummary("cvttss2si") == "Converts a float to an integer, cutting off the fraction (rounding toward zero).");
            CHECK(Asm::MnemonicSummary("cvtsi2sd").rfind("Converts an integer to a double", 0) == 0);
            CHECK(Asm::MnemonicSummary("vaddss").rfind("AVX form", 0) == 0);
            CHECK(Asm::MnemonicSummary("vmovss").find("Copies one float") != std::string::npos);
            int texts = 0;
            for (const std::string& mnemonic : Asm::KnownMnemonics())
                texts += Asm::MnemonicSummary(mnemonic).empty() ? 0 : 1;
            CHECK_MSG(texts * 100 >= static_cast<int>(Asm::KnownMnemonics().size()) * 98, std::to_string(texts) + " mnemonics have a text");
            CHECK(Asm::MnemonicSummary("jmp") == "Jumps to the target.");
            CHECK(Asm::MnemonicSummary("jx").empty() && Asm::MnemonicSummary("setx").empty() && Asm::MnemonicSummary("").empty());
            CHECK(Asm::MnemonicSummary("sets").rfind("Sets the byte to 1 if negative", 0) == 0);
            CHECK(Asm::MnemonicSummary("addsx").empty() && Asm::MnemonicSummary("cvtxx2si").empty());

            int described = 0;
            const std::vector<std::string> mnemonics = Asm::KnownMnemonics();
            for (const char* common : { "mov", "lea", "add", "sub", "cmp", "test", "jmp", "call", "ret", "push", "pop", "xor", "and", "or", "imul", "movss", "movsd",
                     "movaps", "movups", "addss", "subss", "mulss", "divss", "comiss", "cvtsi2ss", "cvttss2si", "movzx", "movsx", "movsxd", "inc", "dec", "nop", "int3",
                     "je", "jne", "jz", "jnz", "jl", "jg", "jle", "jge", "ja", "jb", "jbe", "jae", "sete", "setne", "cmove", "cmovne", "shl", "shr", "sar", "neg", "not",
                     "cdq", "cqo", "idiv", "div", "mul", "xchg", "cmpxchg", "lock", "vmovss", "vaddss", "pxor", "movd", "movq", "sqrtss", "minss", "maxss", "andps" })
            {
                if (std::find(mnemonics.begin(), mnemonics.end(), std::string(common)) == mnemonics.end())
                    continue;
                CHECK_MSG(!Asm::MnemonicSummary(common).empty(), std::string("no text for ") + common);
                ++described;
            }
            CHECK(described > 60);

            Disasm::Disassembler disassembler(true);
            const std::uint8_t load[] = { 0x8B, 0x43, 0x10 };
            const Disasm::Instruction mov = disassembler.Decode(0x1000, load, sizeof(load));
            const auto facts = disassembler.Facts(mov);
            CHECK(facts && facts->Operands.size() == 2);
            if (facts && facts->Operands.size() == 2)
            {
                CHECK(facts->Operands[0].Written && !facts->Operands[0].Read && !facts->Operands[0].Memory && facts->Operands[0].Text == "eax");
                CHECK(facts->Operands[1].Read && facts->Operands[1].Memory && facts->Operands[1].Size == 4);
                CHECK(facts->FlagsModified == 0 && facts->FlagsTested == 0);
                const std::vector<std::string> lines = Disasm::ExplainFacts(*facts);
                CHECK(lines.size() == 2 && lines[0] == "Writes eax" && lines[1].rfind("Reads 4 bytes at ", 0) == 0 && lines[1].find("rbx+10") != std::string::npos);
                CHECK(!facts->Category.empty());
            }
            const std::uint8_t compare[] = { 0x83, 0xF8, 0x05 };
            const auto cmp = disassembler.Facts(disassembler.Decode(0x1000, compare, sizeof(compare)));
            CHECK(cmp && (cmp->FlagsModified & 0x40) && !cmp->Operands.empty() && !cmp->Operands[0].Written);
            if (cmp)
            {
                const std::vector<std::string> lines = Disasm::ExplainFacts(*cmp);
                CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& line) { return line.rfind("Changes the flags", 0) == 0 && line.find("ZF") != std::string::npos; }));
                CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& line) { return line == "Uses the constant 5"; }));
            }
            const std::uint8_t branch[] = { 0x75, 0x02 };
            const auto jne = disassembler.Facts(disassembler.Decode(0x1000, branch, sizeof(branch)));
            CHECK(jne && (jne->FlagsTested & 0x40) && jne->FlagsModified == 0);
            CHECK(Disasm::FlagNames(0x1 | 0x40 | 0x800) == "CF ZF OF");
            Disasm::Instruction invalid;
            CHECK(!disassembler.Facts(invalid));
        }
    }

    void AddressByteMasks()
    {
        const auto bits = [](bool wide, std::initializer_list<std::uint8_t> bytes, bool pointers = false)
            {
                Disasm::Disassembler disassembler(wide);
                const std::vector<std::uint8_t> code(bytes);
                const Disasm::Instruction line = disassembler.Decode(0x140001000, code.data(), code.size());
                return disassembler.AddressBytes(line, pointers ? std::function<bool(std::uint64_t)>([](std::uint64_t value) { return value >= 0x400000 && value < 0x500000; }) : std::function<bool(std::uint64_t)>());
            };
        CHECK_EQ(bits(true, { 0xE8, 0x10, 0x20, 0x30, 0x00 }), 0x1E);
        CHECK_EQ(bits(true, { 0xEB, 0x10 }), 0);
        CHECK_EQ(bits(true, { 0x0F, 0x84, 0x10, 0x20, 0x00, 0x00 }), 0x3C);
        CHECK_EQ(bits(true, { 0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x00 }), 0x78);
        CHECK_EQ(bits(true, { 0x48, 0x8B, 0x81, 0x10, 0x20, 0x00, 0x00 }), 0);
        CHECK_EQ(bits(true, { 0xB8, 0x10, 0x00, 0x40, 0x00 }, true), 0);
        CHECK_EQ(bits(false, { 0xA1, 0x10, 0x00, 0x40, 0x00 }), 0x1E);
        CHECK_EQ(bits(false, { 0x8B, 0x0D, 0x10, 0x00, 0x40, 0x00 }), 0x3C);
        CHECK_EQ(bits(false, { 0x68, 0x10, 0x00, 0x40, 0x00 }, true), 0x1E);
        CHECK_EQ(bits(false, { 0x68, 0x10, 0x00, 0x00, 0x00 }, true), 0);
        CHECK_EQ(bits(false, { 0x8B, 0x81, 0x10, 0x00, 0x40, 0x00 }, true), 0x3C);
        CHECK_EQ(bits(false, { 0x8B, 0x81, 0x10, 0x00, 0x00, 0x00 }, true), 0);
    }

    int RunDisasmTests()
    {
        const int before = Totals().Failures;
        AddressByteMasks();
        RegisterNames();
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
        CHECK_MSG(opened.Ok(), opened.Text);
        if (!opened)
            return Totals().Failures - before;
        std::uint8_t* first = nullptr;
        std::uint8_t* second = nullptr;
        CHECK(AdjacentChunks(first, second));
        if (first && second)
        {
            Resync(engine, first, second);
            UnwindTable(engine, first, second);
            ::VirtualFree(first, 0, MEM_RELEASE);
            ::VirtualFree(second, 0, MEM_RELEASE);
        }
        UnreadableFloor(engine);
        ChainedUnwind();
        InstructionHelp();
        engine.CloseProcess();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("disasm", "Disassembler", 30, [](const SuiteContext&) { return RunDisasmTests(); });
    MANIFOLD_MODE("--disasm-check", [](const std::vector<std::string>& arguments)
        {
            for (const std::string& other : arguments)
            {
                if (other.rfind("--process=", 0) == 0)
                    return RunDisasmLive(arguments);
            }
            const int failed = RunDisasmTests();
            std::printf("%d checks, %d failed\n", Totals().Checks, failed);
            return failed;
        });
    MANIFOLD_MODE("--mnemonic-coverage", [](const std::vector<std::string>& arguments)
        {
            const bool list = std::find(arguments.begin(), arguments.end(), "--list") != arguments.end();
            if (std::find(arguments.begin(), arguments.end(), "--show") != arguments.end())
            {
                for (const std::string& mnemonic : Asm::KnownMnemonics())
                    std::printf("%s\t%s\n", mnemonic.c_str(), Asm::MnemonicSummary(mnemonic).c_str());
                return 0;
            }
            const std::vector<std::string> mnemonics = Asm::KnownMnemonics();
            int described = 0;
            for (const std::string& mnemonic : mnemonics)
            {
                if (!Asm::MnemonicSummary(mnemonic).empty())
                    ++described;
                else if (list)
                    std::printf("%s\n", mnemonic.c_str());
            }
            std::printf("%d of %zu mnemonics have a text\n", described, mnemonics.size());
            return 0;
        });
}
