#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Asm/Assembler.h"
#include "Asm/MnemonicDocs.h"
#include "Core/Text.h"
#include "Disasm/CheatEngineExtra.h"
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
#include <limits>
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

        // ∑ Issue #18: gs:[0x30] and fs:[0x30] resolved to 0x30, without the segment base. Operands without
        //   FS or GS, RIP-relative ones and the register forms are the controls.
        void SegmentBaseEffectiveAddress()
        {
            const auto effective = [](bool is64, const std::vector<std::uint8_t>& bytes, const Disasm::RegisterValues& r) {
                const Disasm::Disassembler disassembler(is64);
                const Disasm::Instruction line = disassembler.Decode(0x1000, bytes.data(), bytes.size());
                return line.Valid ? disassembler.EffectiveAddress(line, r) : std::nullopt;
            };
            const auto check = [](const std::optional<std::uint64_t>& got, std::uint64_t expected, const char* what) {
                CHECK_MSG(got == expected, std::string(what) + ": " + (got ? Hex(*got) : std::string("none")) + " instead of " + Hex(expected));
            };
            Disasm::RegisterValues r64;
            r64.GsBase = 0x71000000;
            r64.Rax = 0x76543210;
            check(effective(true, { 0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00 }, r64), 0x71000030, "mov rax, gs:[0x30]");
            check(effective(true, { 0x65, 0x48, 0x8B, 0x40, 0x30 }, r64), 0xE7543240, "mov rax, gs:[rax+0x30]");
            check(effective(true, { 0x8B, 0x04, 0x25, 0x34, 0x12, 0x00, 0x00 }, r64), 0x1234, "mov eax, [0x1234]");
            check(effective(true, { 0x48, 0xA1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 }, r64), 0x1122334455667788, "mov rax, [0x1122334455667788]");
            check(effective(true, { 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 }, r64), 0x1017, "mov rax, [rip+0x10]");
            Disasm::RegisterValues r32;
            r32.FsBase = 0x71000000;
            r32.Rax = 0x1000;
            check(effective(false, { 0x64, 0x8B, 0x0D, 0x30, 0x00, 0x00, 0x00 }, r32), 0x71000030, "mov ecx, fs:[0x30]");
            check(effective(false, { 0x64, 0xA1, 0x18, 0x00, 0x00, 0x00 }, r32), 0x71000018, "mov eax, fs:[0x18]");
            check(effective(false, { 0x64, 0x8B, 0x48, 0x30 }, r32), 0x71001030, "mov ecx, fs:[eax+0x30]");
            check(effective(false, { 0xA1, 0x34, 0x12, 0x00, 0x00 }, r32), 0x1234, "mov eax, [0x1234]");
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

    namespace
    {
        // ∑ A table found its AOB but could not read the static address behind it: it reads the target with
        //   opcode:match("%[([A-Fa-f0-9]+)%]"), and our Lua disassemble() wrote the memory view's
        //   "mov rcx,[<module>+offset]". Cheat Engine's Lua disassemble() shows no modules or symbols,
        //   so every target is plain hex, for mov and lea alike.
        void LuaTextHasPlainAddresses(Engine& engine)
        {
            std::string answer;
            const Status ran = engine.Lua().RunWithResult(R"LUA(
                local self = getAddress('Manifold-Engine.Tests.exe')
                if not self or self == 0 then return 'no module' end
                local at = self + 0x1000
                local want = string.format('%X', at + 7 + 0x2000)
                for _, bytes in ipairs({ '48 8B 0D 00 20 00 00', '48 8D 0D 00 20 00 00' }) do
                  local line = disassembleBytes(bytes, at)
                  local extra, opcode = splitDisassembledString(line)
                  local hex = opcode and opcode:match('%[([A-Fa-f0-9]+)%]')
                  if hex ~= want then return bytes .. ' gave ' .. tostring(line) .. ', wanted [' .. want .. ']' end
                end
                return 'ok')LUA", "lua disassembly text", answer);
            CHECK_MSG(ran.Ok(), ran.Text);
            CHECK_MSG(answer == "ok", answer);
        }

        // ∑ The owner ran these 18 instructions through Cheat Engine 7.6's Lua disassemble() (64-bit target,
        //   block at 02840000, one instruction per 0x10). Tables parse this text, so ours has to be the same
        //   character for character: bytes grouped by instruction part (a trailing space when a ModRM byte has
        //   no immediate after it), addresses with at least 8 digits, immediates as wide as encoded, imm8 and
        //   disp8 signed with 2 digits, "ret " with its space.
        void LuaTextLikeCheatEngine(Engine& engine)
        {
            struct Case
            {
                const char* Bytes;
                const char* Expected;
            };
            const Case cases[] = {
                { "48 8B 05 6E A8 04 01", "02840000 - 48 8B 05 6EA80401  - mov rax,[0388A875]" },
                { "48 8D 0D 6F B2 7B 00", "02840010 - 48 8D 0D 6FB27B00  - lea rcx,[02FFB286]" },
                { "0F 28 F3", "02840020 - 0F28 F3  - movaps xmm6,xmm3" },
                { "48 83 EC 28", "02840030 - 48 83 EC 28 - sub rsp,28" },
                { "B8 64 00 00 00", "02840040 - B8 64000000 - mov eax,00000064" },
                { "48 B8 88 77 66 55 44 33 22 11", "02840050 - 48 B8 8877665544332211 - mov rax,1122334455667788" },
                { "68 0A 00 00 00", "02840060 - 68 0A000000 - push 0000000A" },
                { "8B 44 24 10", "02840070 - 8B 44 24 10  - mov eax,[rsp+10]" },
                { "8B 84 24 10 01 00 00", "02840080 - 8B 84 24 10010000  - mov eax,[rsp+00000110]" },
                { "C7 41 10 01 00 00 00", "02840090 - C7 41 10 01000000 - mov [rcx+10],00000001" },
                { "83 7D F8 80", "028400A0 - 83 7D F8 80 - cmp dword ptr [rbp-08],-80" },
                { "FF 90 30 01 00 00", "028400B0 - FF 90 30010000  - call qword ptr [rax+00000130]" },
                { "E8 10 00 00 00", "028400C0 - E8 10000000 - call 028400D5" },
                { "EB 10", "028400D0 - EB 10 - jmp 028400E2" },
                { "74 F0", "028400E0 - 74 F0 - je 028400D2" },
                { "F3 0F 10 05 10 00 00 00", "028400F0 - F3 0F10 05 10000000  - movss xmm0,[02840108]" },
                { "66 0F 1F 44 00 00", "02840100 - 66 0F1F 44 00 00  - nop word ptr [rax+rax+00]" },
                { "C3", "02840110 - C3 - ret " },
                { "66 90", "02840120 - 66 90 - nop 2" },
            };
            for (std::size_t i = 0; i < std::size(cases); ++i)
            {
                const std::uint64_t at = 0x02840000ull + i * 0x10;
                std::string answer;
                const Status ran = engine.Lua().RunWithResult(Format("return disassembleBytes('%s', 0x%llX)", cases[i].Bytes, static_cast<unsigned long long>(at)),
                    "ce text", answer);
                CHECK_MSG(ran.Ok(), ran.Text);
                CHECK_MSG(answer == cases[i].Expected, Format("case %zu: |%s| instead of |%s|", i + 1, answer.c_str(), cases[i].Expected));
            }
            std::string split;
            const Status ran = engine.Lua().RunWithResult(R"LUA(
                local extra, opcode, bytes, address = splitDisassembledString(disassembleBytes('48 8B 05 6E A8 04 01', 0x02840000))
                local e2, o2, b2, a2 = splitDisassembledString(disassembleBytes('C3', 0x02840110))
                return string.format('|%s|%s|%s|%s|%s|%s|%s|%s|', type(extra), opcode, bytes, address, e2, o2, b2, a2))LUA", "ce split", split);
            CHECK_MSG(ran.Ok(), ran.Text);
            CHECK_EQ(split, std::string("|string|mov rax,[0388A875]|48 8B 05 6EA80401 |02840000||ret |C3|02840110|"));
        }
    }

    namespace
    {
        // A made-up process for Disasm::CheatEngineExtra: blocks of memory, and names for some values.
        struct FakeMemory
        {
            std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> Blocks;
            std::vector<std::pair<std::uint64_t, std::string>> Names;
            bool Is64Bit = true;

            std::vector<std::uint8_t>* At(std::uint64_t address, std::size_t& offset)
            {
                for (auto& [base, bytes] : Blocks)
                {
                    if (address >= base && address - base < bytes.size())
                    {
                        offset = static_cast<std::size_t>(address - base);
                        return &bytes;
                    }
                }
                return nullptr;
            }

            void Put(std::uint64_t address, const void* data, std::size_t size)
            {
                std::size_t offset = 0;
                auto* bytes = At(address, offset);
                if (bytes && offset + size <= bytes->size())
                    std::memcpy(bytes->data() + offset, data, size);
            }

            Disasm::CheatEngineMemory View()
            {
                Disasm::CheatEngineMemory memory;
                memory.Is64Bit = Is64Bit;
                memory.ReadPrefix = [this](std::uint64_t address, void* buffer, std::size_t size) -> std::size_t
                {
                    std::size_t offset = 0;
                    auto* bytes = At(address, offset);
                    if (!bytes)
                        return 0;
                    const std::size_t got = std::min(size, bytes->size() - offset);
                    std::memcpy(buffer, bytes->data() + offset, got);
                    return got;
                };
                memory.Read = [this](std::uint64_t address, void* buffer, std::size_t size)
                {
                    std::size_t offset = 0;
                    auto* bytes = At(address, offset);
                    if (!bytes || offset + size > bytes->size())
                        return false;
                    std::memcpy(buffer, bytes->data() + offset, size);
                    return true;
                };
                memory.Committed = [this](std::uint64_t address)
                {
                    std::size_t offset = 0;
                    return At(address, offset) != nullptr;
                };
                memory.Name = [this](std::uint64_t value)
                {
                    for (const auto& [named, text] : Names)
                    {
                        if (named == value)
                            return text;
                    }
                    return Hex(value);
                };
                return memory;
            }
        };

        std::string ExtraOf(FakeMemory& fake, const std::string& line)
        {
            const Disasm::CheatEngineMemory memory = fake.View();
            return Disasm::SplitCheatEngineLine(line, &memory).Extra;
        }

        // A Lua string literal that keeps every byte (a long string would turn "\r\n" into "\n").
        std::string LuaLiteral(const std::string& text)
        {
            std::string literal = "\"";
            for (const char c : text)
            {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 32 || c == '"' || c == '\\')
                    literal += Format("\\%03u", static_cast<unsigned>(byte));
                else
                    literal += c;
            }
            return literal + "\"";
        }

        // ∑ Free Pascal's FloatToStr decides whether CE takes four bytes for a float (a short text, or one
        //   without a decimal point); Format('%.4f') writes the value. Both round their own way.
        void PascalNumbers()
        {
            CHECK_EQ(Disasm::PascalFloatToStr(1.5f, true), std::string("1.5"));
            CHECK_EQ(Disasm::PascalFloatToStr(0.1f, true), std::string("0.1000000015"));
            CHECK_EQ(Disasm::PascalFloatToStr(-2.25f, true), std::string("-2.25"));
            CHECK_EQ(Disasm::PascalFloatToStr(100000.0f, true), std::string("100000"));
            CHECK_EQ(Disasm::PascalFloatToStr(0.0078125, false), std::string("0.0078125"));
            CHECK_EQ(Disasm::PascalFloatToStr(123.456, false), std::string("123.456"));
            CHECK(Disasm::PascalFloatToStr(0.000001f, true).find('E') != std::string::npos);
            CHECK(Disasm::PascalFloatToStr(0.00001, false).find('E') == std::string::npos);
            CHECK(Disasm::PascalFloatToStr(1e20, false).find('E') != std::string::npos);
            CHECK_EQ(Disasm::PascalFixed4(1.5), std::string("1.5000"));
            CHECK_EQ(Disasm::PascalFixed4(0.0078125), std::string("0.0078"));
            CHECK_EQ(Disasm::PascalFixed4(123.456), std::string("123.4560"));
            CHECK_EQ(Disasm::PascalFixed4(-2.5), std::string("-2.5000"));
            CHECK_EQ(Disasm::PascalFixed4(-0.00001), std::string("0.0000"));
            CHECK_EQ(Disasm::PascalFixed4(0.00006), std::string("0.0001"));
            CHECK_EQ(Disasm::PascalFixed4(99999.99999), std::string("100000.0000"));
            CHECK_EQ(Disasm::PascalFixed4(1.03125), std::string("1.0313"));
            // GRISU1_F2A_AGRESSIVE_ROUNDUP: ...4999999 rounds up.
            CHECK_EQ(Disasm::PascalFixed4(0.12344999999999999), std::string("0.1235"));
            CHECK_EQ(Disasm::PascalFixed4(std::numeric_limits<double>::quiet_NaN()), std::string("Nan"));
            CHECK_EQ(Disasm::PascalFixed4(-std::numeric_limits<double>::infinity()), std::string("-Inf"));
        }

        // ∑ Cheat Engine's splitDisassembledString guesses what the instruction refers to (issue #3). The
        //   texts are CE's own from the owner's captures; the memory is the capture script's test block.
        void CheatEngineExtraRules()
        {
            FakeMemory wide;
            constexpr std::uint64_t kWide = 0x1E4BEDE0000ull;
            wide.Blocks.push_back({ kWide, std::vector<std::uint8_t>(0x2000) });
            const std::uint8_t data1[] = { 0x78, 0x56, 0x34, 0x12 };
            const std::uint8_t data2[] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
            const std::uint8_t data3[] = { 0x00, 0x00, 0xC0, 0x3F };
            wide.Put(kWide + 0x1800, data1, sizeof(data1));
            wide.Put(kWide + 0x1810, data2, sizeof(data2));
            wide.Put(kWide + 0x1820, data3, sizeof(data3));

            struct Case
            {
                const char* Line;
                const char* Extra;
            };
            const Case wideCases[] = {
                { "1E4BEDE0BE0 - 65 48 8B 04 25 30000000  - mov rax,gs:[00000030]", "" },
                { "1E4BEDE0C00 - 64 A1 0000000000000000 - mov eax,fs:[00000000]", "" },
                { "1E4BEDE0C20 - 64 8B 0D 30000000  - mov ecx,fs:[1E4BEDE0C57]", "[00000000]" },
                { "1E4BEDE1160 - F3 0F10 05 B8060000  - movss xmm0,[1E4BEDE1820]", "[(float)1.5000]" },
                { "1E4BEDE1180 - F2 0F10 05 88060000  - movsd xmm0,[1E4BEDE1810]", "[55667788]" },
                { "1E4BEDE11A0 - 8B 05 5A060000  - mov eax,[1E4BEDE1800]", "[12345678]" },
                { "1E4BEDE11C0 - 48 8B 05 49060000  - mov rax,[1E4BEDE1810]", "[55667788]" },
                { "1E4BEDE11E0 - 66 8B 05 19060000  - mov ax,[1E4BEDE1800]", "[5678]" },
                { "1E4BEDE1200 - 8A 05 FA050000  - mov al,[1E4BEDE1800]", "[78]" },
                { "1E4BEDE1220 - 0FB6 05 D9050000  - movzx eax,byte ptr [1E4BEDE1800]", "[78]" },
                { "1E4BEDE1240 - 8D 05 BA050000  - lea eax,[1E4BEDE1800]", "[12345678]" },
                { "1E4BEDE1260 - 83 3D 99050000 05 - cmp dword ptr [1E4BEDE1800],05", "" },
                { "1E4BEDE1280 - 89 05 7A050000  - mov [1E4BEDE1800],eax", "" },
                { "1E4BEDE12A0 - C7 05 56050000 01000000 - mov [1E4BEDE1800],00000001", "" },
                { "1E4BEDE12C0 - FF 15 4A050000  - call qword ptr [1E4BEDE1810]", "" },
                { "1E4BEDE12E0 - D9 05 3A050000  - fld dword ptr [1E4BEDE1820]", "[(float)1.5000]" },
                { "1E4BEDE1300 - F3 0F5A 05 18050000  - cvtss2sd xmm0,[1E4BEDE1820]", "[(float)1.5000]" },
                { "1E4BEDE0000 - 70 10 - jo 1E4BEDE0012", "" },
                { "1E4BEDE0000 - 8B 44 88 10  - mov eax,[rax+rcx*4+10]", "" },
                { "1E4BEDE0000 - C3 - ret ", "" },
            };
            for (const Case& c : wideCases)
            {
                const std::string extra = ExtraOf(wide, c.Line);
                CHECK_MSG(extra == c.Extra, Format("64-bit %s: |%s| instead of |%s|", c.Line, extra.c_str(), c.Extra));
            }

            FakeMemory narrow;
            narrow.Is64Bit = false;
            constexpr std::uint64_t kNarrow = 0x30D10000ull;
            narrow.Blocks.push_back({ kNarrow, std::vector<std::uint8_t>(0x2000) });
            narrow.Put(kNarrow + 0x1800, data1, sizeof(data1));
            narrow.Put(kNarrow + 0x1810, data2, sizeof(data2));
            narrow.Put(kNarrow + 0x1820, data3, sizeof(data3));
            narrow.Names.push_back({ 0x55667788, "Fake.Export+3958" });
            const char text[] = "path/to/file.png\nsecond line";
            narrow.Put(kNarrow + 0x1830, text, sizeof(text));
            const std::uint8_t thunk[] = { 0xFF, 0x25, 0x10, 0x18, 0xD1, 0x30 };
            narrow.Put(kNarrow + 0x1900, thunk, sizeof(thunk));
            const Case narrowCases[] = {
                { "30D11180 - F2 0F10 05 1018D130  - movsd xmm0,[30D11810]", "[Fake.Export+3958]" },
                { "30D112C0 - FF 15 1018D130  - call dword ptr [30D11810]", "[Fake.Export+3958]" },
                { "30D11260 - 83 3D 0018D130 05 - cmp dword ptr [30D11800],05", "" },
                { "30D10C00 - 64 A1 00000000 - mov eax,fs:[00000000]", "" },
                { "30D10000 - 68 3018D130 - push 30D11830", "[\"path/to/file.png\nsecond line\"]" },
                { "30D10000 - B8 3018D130 - mov eax,30D11830", "[\"path/to/file.png\nsecond line\"]" },
                { "30D10000 - E9 FB180000 - jmp 30D11900", "[->Fake.Export+3958]" },
                { "30D10000 - 75 0E - jne 30D11810", "[Fake.Export+3958]" },
                { "30D10000 - 75 0E - jne 30D11800", "" },
                { "30d10000 - 8B 05 0018D130  - mov eax,[30D11800] : a comment", "[12345678]" },
            };
            for (const Case& c : narrowCases)
            {
                const std::string extra = ExtraOf(narrow, c.Line);
                CHECK_MSG(extra == c.Extra, Format("32-bit %s: |%s| instead of |%s|", c.Line, extra.c_str(), c.Extra));
            }

            const Disasm::CheatEngineMemory view = narrow.View();
            const Disasm::CheatEngineSplit split = Disasm::SplitCheatEngineLine("30d10000 - 8B 05 0018D130  - mov eax,[30D11800] : a comment", &view);
            CHECK_EQ(split.Address, std::string("30D10000"));
            CHECK_EQ(split.Bytes, std::string("8B 05 0018D130 "));
            CHECK_EQ(split.Opcode, std::string("mov eax,[30D11800]"));
            CHECK_EQ(Disasm::SplitCheatEngineLine("30D11180 - F2 0F10 05 1018D130  - movsd xmm0,[30D11810]", nullptr).Extra, std::string());
            CHECK_EQ(Disasm::SplitCheatEngineLine("no dashes here", nullptr).Opcode, std::string());
        }

        // ∑ The owner's Cheat Engine 7.6 capture of the extra field (Tools/ce-capture.lua, issue #3): every
        //   test-block line with CE's fourth value, the block's data area and the names CE gave values in it.
        //   The test rebuilds the block line by line, as the script wrote it, and must guess what CE guessed.
        void CheatEngineExtraCapture()
        {
            struct Data
            {
                bool Is64;
                std::uint64_t Address;
                const char* Bytes;
            };
            struct Name
            {
                bool Is64;
                std::uint64_t Value;
                const char* Text;
            };
            struct Line
            {
                bool Is64;
                std::uint64_t Address;
                const char* Bytes;
                const char* Text;
                const char* Extra;
            };
#define CE_EXTRA_NAME(is64, value, text)
#define CE_EXTRA_LINE(is64, address, bytes, text, extra)
#define CE_EXTRA_DATA(is64, address, bytes) { is64, address, bytes },
            static const Data kData[] = {
#include "CheatEngineExtra.inc"
                { false, 0, nullptr },
            };
#undef CE_EXTRA_DATA
#define CE_EXTRA_DATA(is64, address, bytes)
#undef CE_EXTRA_NAME
#define CE_EXTRA_NAME(is64, value, text) { is64, value, text },
            static const Name kNames[] = {
#include "CheatEngineExtra.inc"
                { false, 0, nullptr },
            };
#undef CE_EXTRA_NAME
#define CE_EXTRA_NAME(is64, value, text)
#undef CE_EXTRA_LINE
#define CE_EXTRA_LINE(is64, address, bytes, text, extra) { is64, address, bytes, text, extra },
            static const Line kLines[] = {
#include "CheatEngineExtra.inc"
                { false, 0, nullptr, nullptr, nullptr },
            };
#undef CE_EXTRA_LINE
#undef CE_EXTRA_NAME
#undef CE_EXTRA_DATA
            const auto bytesOf = [](const char* hex)
            {
                std::vector<std::uint8_t> bytes;
                for (std::size_t i = 0; hex[i] && hex[i + 1]; i += 2)
                    bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
                return bytes;
            };
            int checked = 0;
            for (const bool is64 : { true, false })
            {
                FakeMemory fake;
                fake.Is64Bit = is64;
                for (const Line& line : kLines)
                {
                    if (!line.Bytes || line.Is64 != is64)
                        continue;
                    if (fake.Blocks.empty())
                        fake.Blocks.push_back({ line.Address, std::vector<std::uint8_t>(0x2000) });
                }
                if (fake.Blocks.empty())
                    continue;
                for (const Data& data : kData)
                {
                    if (data.Bytes && data.Is64 == is64)
                    {
                        const auto bytes = bytesOf(data.Bytes);
                        fake.Put(data.Address, bytes.data(), bytes.size());
                    }
                }
                for (const Name& name : kNames)
                {
                    if (name.Text && name.Is64 == is64)
                        fake.Names.push_back({ name.Value, name.Text });
                }
                for (const Line& line : kLines)
                {
                    if (!line.Bytes || line.Is64 != is64)
                        continue;
                    const auto bytes = bytesOf(line.Bytes);
                    fake.Put(line.Address, bytes.data(), bytes.size());
                    const std::string extra = ExtraOf(fake, line.Text);
                    CHECK_MSG(extra == line.Extra, Format("%s-bit %s: |%s| instead of |%s|", is64 ? "64" : "32", line.Text, extra.c_str(), line.Extra));
                    ++checked;
                }
            }
            CHECK_MSG(checked > 150, Format("only %d captured lines", checked));
        }

        // The real path: Lua's splitDisassembledString in the tests' own process, on memory of its own.
        void CheatEngineExtraLive(Engine& engine)
        {
            auto* block = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            CHECK(block != nullptr);
            if (!block)
                return;
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(block);
            const std::uint8_t dword[] = { 0x78, 0x56, 0x34, 0x12 };
            const std::uint8_t qword[] = { 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
            const float single = 1.5f;
            const char ascii[] = "Hello, World!\r\nLine two";
            const wchar_t wide[] = L"Unicode text";
            const double small = 0.0078125;
            const double plain = 123.456;
            const float pair[] = { 0.1f, 0.7f };
            const float nicePair[] = { 0.1f, 0.5f };
            const std::uint8_t counter[] = { 0x05, 'a', 'b', 'c' };
            std::memcpy(block + 0x800, dword, sizeof(dword));
            std::memcpy(block + 0x810, qword, sizeof(qword));
            std::memcpy(block + 0x820, &single, sizeof(single));
            std::memcpy(block + 0x830, ascii, sizeof(ascii));
            std::memcpy(block + 0x880, wide, sizeof(wide));
            std::memcpy(block + 0x8C0, &small, sizeof(small));
            std::memcpy(block + 0x8D0, &plain, sizeof(plain));
            std::memcpy(block + 0x8E0, pair, sizeof(pair));
            std::memcpy(block + 0x940, nicePair, sizeof(nicePair));
            std::memcpy(block + 0x8F0, counter, sizeof(counter));
            const std::uint8_t thunk[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            const std::uint64_t pointee = base + 0x1000;
            std::memcpy(block + 0x900, thunk, sizeof(thunk));
            std::memcpy(block + 0x906, &pointee, sizeof(pointee));
            CHECK(engine.Lua().Run(Format("registerSymbol('ceExtraTarget', 0x%llX)", static_cast<unsigned long long>(pointee)), "ce extra symbol").Ok());

            const auto at = [&](std::uint64_t offset) { return Hex(base + offset, 8); };
            struct Case
            {
                std::string Line;
                std::string Extra;
            };
            const std::string code = at(0x100);
            const Case cases[] = {
                { code + " - 8B 05 00000000  - mov eax,[" + at(0x800) + "]", "[12345678]" },
                { code + " - 48 8B 05 00000000  - mov rax,[" + at(0x810) + "]", "[55667788]" },
                { code + " - F3 0F10 05 00000000  - movss xmm0,[" + at(0x820) + "]", "[(float)1.5000]" },
                { code + " - 66 8B 05 00000000  - mov ax,[" + at(0x800) + "]", "[5678]" },
                { code + " - 8D 05 00000000  - lea eax,[" + at(0x800) + "]", "[12345678]" },
                { code + " - 89 05 00000000  - mov [" + at(0x800) + "],eax", "" },
                { code + " - 48 8B 0D 00000000  - mov rcx,[" + at(0x830) + "]", "[\"Hello, World!\r\nLine two\"]" },
                { code + " - 48 B8 0000000000000000 - mov rax," + Hex(base + 0x830, 16), "[\"Hello, World!\r\nLine two\"]" },
                { code + " - 48 8B 0D 00000000  - mov rcx,[" + at(0x880) + "]", "[\"\"Unicode text\"\"]" },
                { code + " - 8B 0D 00000000  - mov ecx,[" + at(0x8C0) + "]", "[(double)0.0078]" },
                { code + " - 8B 0D 00000000  - mov ecx,[" + at(0x8D0) + "]", "[(double)123.4560]" },
                // ∑ 0.1f then 0.7f: bytes 4..7 (33 33 33 3F) pass CE's text test, so the second float does not
                //   count and the eight bytes stay a double. After 0.5f (00 00 00 3F) the first stays a float.
                { code + " - 8B 0D 00000000  - mov ecx,[" + at(0x8E0) + "]", "[(double)0.0003]" },
                { code + " - 8B 0D 00000000  - mov ecx,[" + at(0x940) + "]", "[(float)0.1000]" },
                { code + " - 8B 0D 00000000  - mov ecx,[" + at(0x8F0) + "]", "[63626105]" },
                { code + " - 8B 0D 00000000  - mov ecx,[" + at(0x801) + "]", "[00123456]" },
                { code + " - DD 05 00000000  - fld qword ptr [" + at(0x8C0) + "]", "[(double)0.0078]" },
                { code + " - E9 00000000 - jmp " + at(0x900), "[->ceExtraTarget]" },
                { code + " - E8 00000000 - call " + at(0x900), "[->ceExtraTarget]" },
                { code + " - E9 00000000 - jmp " + at(0x1000), "" },
                { code + " - FF 15 00000000  - call qword ptr [" + at(0x810) + "]", "" },
                { code + " - 8B 05 00000000  - mov eax,[" + at(0x800) + "] : not the extra", "[12345678]" },
            };
            for (const Case& c : cases)
            {
                std::string answer;
                const Status done = engine.Lua().RunWithResult("return (splitDisassembledString(" + LuaLiteral(c.Line) + "))", "ce extra", answer);
                CHECK_MSG(done.Ok(), done.Text);
                CHECK_MSG(answer == c.Extra, Format("%s: |%s| instead of |%s|", c.Line.c_str(), answer.c_str(), c.Extra.c_str()));
            }
            CHECK(engine.Lua().Run("unregisterSymbol('ceExtraTarget')", "ce extra symbol").Ok());
            ::VirtualFree(block, 0, MEM_RELEASE);
        }
    }

    namespace
    {
        // ∑ The owner's second Cheat Engine 7.6 capture, made in a 64-bit and a 32-bit game: about 140 test
        //   instructions per game, written into a block CE allocated (no game code is kept here). Each line must
        //   come out exactly as CE's Lua disassemble() wrote it. Left out on purpose (issue #1): the lines where
        //   CE's own decoder is wrong, VEX/EVEX, invalid bytes and the comma faults (pshufd xmm0,xmm01B).
        void CheatEngineCapture()
        {
            struct Line
            {
                bool Is64;
                std::uint64_t Address;
                const char* Bytes;
                const char* Expected;
            };
            static const Line kLines[] = {
#include "CheatEngineCapture.inc"
            };
            const Disasm::Disassembler wide(true);
            const Disasm::Disassembler narrow(false);
            for (const Line& line : kLines)
            {
                std::uint8_t bytes[16]{};
                for (std::size_t i = 0; i < sizeof(bytes); ++i)
                    bytes[i] = static_cast<std::uint8_t>(std::stoi(std::string(line.Bytes + i * 2, 2), nullptr, 16));
                const std::string text = (line.Is64 ? wide : narrow).CheatEngineLine(line.Address, bytes, sizeof(bytes));
                CHECK_MSG(text == line.Expected, std::string(line.Is64 ? "64" : "32") + "-bit |" + text + "| instead of |" + line.Expected + "|");
            }
        }
    }

    int RunDisasmTests()
    {
        const int before = Totals().Failures;
        AddressByteMasks();
        RegisterNames();
        SegmentBaseEffectiveAddress();
        CheatEngineCapture();
        PascalNumbers();
        CheatEngineExtraRules();
        CheatEngineExtraCapture();
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
        LuaTextHasPlainAddresses(engine);
        LuaTextLikeCheatEngine(engine);
        CheatEngineExtraLive(engine);
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
