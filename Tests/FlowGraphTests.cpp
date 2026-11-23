#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/GraphLayout.h"
#include "Core/Text.h"
#include "Disasm/FlowGraph.h"
#include "Disasm/FunctionBounds.h"
#include "Lua/LuaEngine.h"
#include "Target/CodeMap.h"
#include "Target/Process.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        struct GapTally
        {
            int Leaves = 0;
            int Inside = 0;
            int None = 0;
            int Wrong = 0;
        };

        const char* SourceName(Disasm::FunctionSource source)
        {
            switch (source)
            {
            case Disasm::FunctionSource::Unwind: return "unwind data";
            case Disasm::FunctionSource::Managed: return "managed method";
            case Disasm::FunctionSource::Padding: return "padding";
            default: return "guess";
            }
        }

        const std::uint8_t kDiamondLoop[] = {
            0x31, 0xC0, 0x85, 0xC9, 0x74, 0x07, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xEB, 0x05, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xFF, 0xC8, 0x75, 0xFC, 0xC3,
        };

        const std::uint8_t kTailJump[] = { 0xE8, 0x3B, 0x00, 0x00, 0x00, 0xEB, 0x39 };
        constexpr std::size_t kTailAt = 0x100;
        constexpr std::size_t kCalleeAt = 0x140;
        constexpr std::size_t kIndirectAt = 0x180;

        bool HasEdge(const Disasm::FlowGraph& graph, int from, int to, Disasm::EdgeKind kind, bool back)
        {
            return std::any_of(graph.Edges.begin(), graph.Edges.end(), [&](const Disasm::FlowEdge& edge) { return edge.From == from && edge.To == to && edge.Kind == kind && edge.Back == back; });
        }

        void DiamondAndLoop(Engine& engine, std::uint8_t* code)
        {
            const auto base = reinterpret_cast<std::uint64_t>(code);
            const Disasm::FlowGraph graph = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base, nullptr, nullptr, {});
            CHECK(graph.Entry == base);
            CHECK(!graph.Truncated);
            CHECK(graph.Instructions == 9);
            CHECK(graph.Blocks.size() == 5);
            CHECK(graph.Edges.size() == 6);
            if (graph.Blocks.size() != 5)
                return;
            const std::uint64_t starts[] = { 0x0, 0x6, 0xD, 0x12, 0x16 };
            const std::uint64_t ends[] = { 0x6, 0xD, 0x12, 0x16, 0x17 };
            for (std::size_t b = 0; b < 5; ++b)
            {
                CHECK(graph.Blocks[b].Start == base + starts[b]);
                CHECK(graph.Blocks[b].End == base + ends[b]);
                CHECK(graph.BlockStarting(base + starts[b]) == static_cast<int>(b));
                CHECK(graph.BlockAt(base + ends[b] - 1) == static_cast<int>(b));
            }
            CHECK(graph.BlockAt(base + 0x17) == -1);
            CHECK(graph.Blocks[0].Lines.size() == 3);
            CHECK(graph.Blocks[2].Lines.size() == 1);
            CHECK(graph.Blocks[3].Lines.size() == 2);
            CHECK(HasEdge(graph, 0, 2, Disasm::EdgeKind::Taken, false));
            CHECK(HasEdge(graph, 0, 1, Disasm::EdgeKind::NotTaken, false));
            CHECK(HasEdge(graph, 1, 3, Disasm::EdgeKind::Jump, false));
            CHECK(HasEdge(graph, 2, 3, Disasm::EdgeKind::Next, false));
            CHECK(HasEdge(graph, 3, 3, Disasm::EdgeKind::Taken, true));
            CHECK(HasEdge(graph, 3, 4, Disasm::EdgeKind::NotTaken, false));
            CHECK(graph.Blocks[3].LoopHead);
            CHECK(std::count_if(graph.Blocks.begin(), graph.Blocks.end(), [](const Disasm::FlowBlock& block) { return block.LoopHead; }) == 1);
            CHECK(graph.Blocks[4].Returns);
            CHECK(!graph.Blocks[0].Returns && !graph.Blocks[3].Returns);

            Disasm::FlowLimits small;
            small.MaxInstructions = 4;
            const Disasm::FlowGraph cut = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base, nullptr, nullptr, {}, small);
            CHECK(cut.Truncated);
            CHECK(cut.Instructions <= 4);
            CHECK(!cut.Blocks.empty() && cut.Blocks[0].Start == base);
            for (const Disasm::FlowEdge& edge : cut.Edges)
                CHECK(edge.From >= 0 && edge.To >= 0 && static_cast<std::size_t>(edge.From) < cut.Blocks.size() && static_cast<std::size_t>(edge.To) < cut.Blocks.size());

            const Disasm::FlowGraph middle = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + 0x12, nullptr, nullptr, {});
            CHECK(middle.Blocks.size() == 2);
            CHECK(!middle.Blocks.empty() && middle.Blocks[0].Start == base + 0x12 && middle.Blocks[0].LoopHead);
        }

        void TailJumpsAndIndirect(Engine& engine, std::uint8_t* code)
        {
            const auto base = reinterpret_cast<std::uint64_t>(code);
            const std::uint64_t callee = base + kCalleeAt;
            const auto other = [&](std::uint64_t target, Disasm::EdgeKind) { return target == callee; };
            const Disasm::FlowGraph tail = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kTailAt, nullptr, nullptr, other);
            CHECK(tail.Blocks.size() == 1);
            CHECK(tail.Edges.empty());
            if (!tail.Blocks.empty())
            {
                CHECK(tail.Blocks[0].Calls == std::vector<std::uint64_t>{ callee });
                CHECK(tail.Blocks[0].Exits == std::vector<std::uint64_t>{ callee });
                CHECK(!tail.Blocks[0].Returns);
            }

            const Disasm::FlowGraph inlined = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kTailAt, nullptr, nullptr, {});
            CHECK(inlined.Blocks.size() == 2);
            CHECK(HasEdge(inlined, 0, 1, Disasm::EdgeKind::Jump, false));
            CHECK(inlined.Blocks.size() == 2 && inlined.Blocks[1].Returns);

            Disasm::FlowLimits shortReach;
            shortReach.Reach = 0x10;
            const Disasm::FlowGraph reach = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kTailAt, nullptr, nullptr, {}, shortReach);
            CHECK(reach.Blocks.size() == 1);
            CHECK(!reach.Blocks.empty() && reach.Blocks[0].Exits == std::vector<std::uint64_t>{ callee });

            const Disasm::FlowGraph indirect = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kIndirectAt, nullptr, nullptr, {});
            CHECK(indirect.Blocks.size() == 1);
            CHECK(!indirect.Blocks.empty() && indirect.Blocks[0].Indirect && indirect.Blocks[0].Lines.size() == 1);

            const Disasm::FlowGraph trap = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kIndirectAt + 2, nullptr, nullptr, {});
            CHECK(trap.Blocks.size() == 1);
            CHECK(!trap.Blocks.empty() && trap.Blocks[0].Stops);
        }

        const std::uint8_t kRelativeSwitch[] = {
            0x83, 0xF9, 0x03, 0x77, 0x3B, 0x48, 0x8D, 0x15, 0xF4, 0xFD, 0xFF, 0xFF, 0x8B, 0x84, 0x8A, 0x80, 0x02, 0x00, 0x00, 0x48, 0x03, 0xC2, 0xFF, 0xE0,
        };
        const std::uint8_t kPointerSwitch[] = { 0x83, 0xF9, 0x02, 0x73, 0x2B, 0x48, 0x8D, 0x15, 0x74, 0x00, 0x00, 0x00, 0xFF, 0x24, 0xCA };
        constexpr std::size_t kRelativeAt = 0x200;
        constexpr std::size_t kRelativeTable = 0x280;
        constexpr std::size_t kPointerAt = 0x300;
        constexpr std::size_t kPointerTable = 0x380;

        void SwitchTables(Engine& engine, std::uint8_t* code)
        {
            const auto base = reinterpret_cast<std::uint64_t>(code);
            std::memcpy(code + kRelativeAt, kRelativeSwitch, sizeof(kRelativeSwitch));
            code[0x220] = code[0x224] = code[0x229] = code[0x240] = 0xC3;
            code[0x228] = 0x90;
            const std::int32_t offsets[] = { 0x220, 0x224, 0x228, 0x220 };
            std::memcpy(code + kRelativeTable, offsets, sizeof(offsets));
            std::memcpy(code + kPointerAt, kPointerSwitch, sizeof(kPointerSwitch));
            code[0x310] = code[0x314] = code[0x330] = 0xC3;
            const std::uint64_t pointers[] = { base + 0x310, base + 0x314 };
            std::memcpy(code + kPointerTable, pointers, sizeof(pointers));

            const auto edgeTo = [&](const Disasm::FlowGraph& graph, int from, std::uint64_t target) {
                const int to = graph.BlockStarting(target);
                return to >= 0 && HasEdge(graph, from, to, Disasm::EdgeKind::Jump, false);
            };
            const Disasm::FlowGraph relative = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kRelativeAt, nullptr, nullptr, {});
            CHECK_EQ(relative.Blocks.size(), 6u);
            const int dispatch = relative.BlockStarting(base + kRelativeAt + 5);
            CHECK(dispatch >= 0);
            if (dispatch >= 0)
            {
                const Disasm::FlowBlock& block = relative.Blocks[static_cast<std::size_t>(dispatch)];
                CHECK(!block.Indirect && block.Cases == 3 && block.Lines.size() == 4);
                CHECK(edgeTo(relative, dispatch, base + 0x220) && edgeTo(relative, dispatch, base + 0x224) && edgeTo(relative, dispatch, base + 0x228));
                CHECK(!edgeTo(relative, dispatch, base + 0x240));
            }
            CHECK(relative.BlockStarting(base + 0x240) >= 0);

            const Disasm::FlowGraph pointer = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kPointerAt, nullptr, nullptr, {});
            CHECK_EQ(pointer.Blocks.size(), 5u);
            const int jump = pointer.BlockStarting(base + kPointerAt + 5);
            CHECK(jump >= 0);
            if (jump >= 0)
            {
                CHECK(!pointer.Blocks[static_cast<std::size_t>(jump)].Indirect && pointer.Blocks[static_cast<std::size_t>(jump)].Cases == 2);
                CHECK(edgeTo(pointer, jump, base + 0x310) && edgeTo(pointer, jump, base + 0x314));
            }

            const Disasm::FlowGraph unbounded = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kPointerAt + 5, nullptr, nullptr, {});
            CHECK(unbounded.Blocks.size() == 1 && unbounded.Blocks[0].Indirect && unbounded.Blocks[0].Cases == 0);

            const std::int32_t intoTable = static_cast<std::int32_t>(kRelativeTable);
            std::memcpy(code + kRelativeTable + 8, &intoTable, sizeof(intoTable));
            const Disasm::FlowGraph damaged = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kRelativeAt, nullptr, nullptr, {});
            const int refused = damaged.BlockStarting(base + kRelativeAt + 5);
            CHECK(refused >= 0 && damaged.Blocks[static_cast<std::size_t>(refused)].Indirect && damaged.Blocks[static_cast<std::size_t>(refused)].Cases == 0);
            CHECK_EQ(damaged.Blocks.size(), 3u);

            Disasm::FlowLimits nearby;
            nearby.Reach = 0x12;
            const Disasm::FlowGraph tight = Disasm::BuildFlowGraph(engine.Disassembler(), *engine.Process(), base + kPointerAt, nullptr, nullptr, {}, nearby);
            const int outside = tight.BlockStarting(base + kPointerAt + 5);
            CHECK(outside >= 0 && tight.Blocks[static_cast<std::size_t>(outside)].Indirect);
        }

        bool Overlap(const Graph::Point& a, const Graph::Size& as, const Graph::Point& b, const Graph::Size& bs)
        {
            return a.X < b.X + bs.Width && b.X < a.X + as.Width && a.Y < b.Y + bs.Height && b.Y < a.Y + as.Height;
        }

        void CheckLayout(const std::vector<Graph::Size>& sizes, const std::vector<Graph::Edge>& edges, const Graph::Result& result)
        {
            CHECK(result.Positions.size() == sizes.size());
            CHECK(result.Routes.size() == edges.size());
            if (result.Positions.size() != sizes.size() || result.Routes.size() != edges.size())
                return;
            std::size_t overlaps = 0;
            std::size_t outside = 0;
            float minX = 1e30f;
            float minY = 1e30f;
            for (std::size_t a = 0; a < sizes.size(); ++a)
            {
                minX = std::min(minX, result.Positions[a].X);
                minY = std::min(minY, result.Positions[a].Y);
                outside += result.Positions[a].X + sizes[a].Width > result.Width + 0.01f || result.Positions[a].Y + sizes[a].Height > result.Height + 0.01f ? 1 : 0;
                for (std::size_t b = a + 1; b < sizes.size(); ++b)
                    overlaps += Overlap(result.Positions[a], sizes[a], result.Positions[b], sizes[b]) ? 1 : 0;
            }
            CHECK(overlaps == 0);
            CHECK(outside == 0);
            CHECK(minX >= -0.01f && minY >= -0.01f);
            std::size_t shortRoutes = 0;
            std::size_t loosePorts = 0;
            std::size_t diagonals = 0;
            std::size_t upward = 0;
            for (std::size_t e = 0; e < edges.size(); ++e)
            {
                const std::vector<Graph::Point>& route = result.Routes[e];
                if (route.size() < 2)
                {
                    ++shortRoutes;
                    continue;
                }
                const Graph::Point from = result.Positions[static_cast<std::size_t>(edges[e].From)];
                const Graph::Point to = result.Positions[static_cast<std::size_t>(edges[e].To)];
                const Graph::Size fromSize = sizes[static_cast<std::size_t>(edges[e].From)];
                const Graph::Size toSize = sizes[static_cast<std::size_t>(edges[e].To)];
                const bool leaves = std::abs(route.front().Y - (from.Y + fromSize.Height)) < 0.01f && route.front().X >= from.X && route.front().X <= from.X + fromSize.Width;
                const bool arrives = std::abs(route.back().Y - to.Y) < 0.01f && route.back().X >= to.X && route.back().X <= to.X + toSize.Width;
                loosePorts += leaves && arrives ? 0 : 1;
                for (std::size_t p = 1; p < route.size(); ++p)
                    diagonals += std::abs(route[p].X - route[p - 1].X) > 0.01f && std::abs(route[p].Y - route[p - 1].Y) > 0.01f ? 1 : 0;
                if (!result.Back[e] && edges[e].From != edges[e].To)
                    upward += result.Layers[static_cast<std::size_t>(edges[e].To)] > result.Layers[static_cast<std::size_t>(edges[e].From)] ? 0 : 1;
            }
            CHECK(shortRoutes == 0);
            CHECK(loosePorts == 0);
            CHECK(diagonals == 0);
            CHECK(upward == 0);
        }

        constexpr std::size_t kSearchAt = 0x400;

        const std::pair<std::size_t, std::vector<std::uint8_t>> kSearchCode[] = {
            { 0x10, { 0x85, 0xC9, 0x74, 0x03, 0x31, 0xC0, 0xC3, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 } },
            { 0x20, { 0x31, 0xC0, 0xC3 } },
            { 0x30, { 0x85, 0xC9, 0x75, 0x06, 0xE8, 0xE7, 0xFF, 0xFF, 0xFF, 0xCC, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3 } },
            { 0x50, { 0x85, 0xC9, 0x74, 0x0C, 0x31, 0xC0, 0x48, 0x83, 0xC0, 0x01, 0x48, 0x83, 0xC0, 0x01, 0x50, 0xC3, 0xB8, 0x03, 0x00, 0x00, 0x00, 0xC3 } },
            { 0x70, { 0x31, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 } },
            { 0x80, { 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3 } },
            { 0x90, { 0x31, 0xC0, 0xEB, 0x0C } },
            { 0xA0, { 0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3 } },
        };

        void FunctionSearch(Engine& engine, std::uint8_t* code)
        {
            for (const auto& [offset, bytes] : kSearchCode)
                std::memcpy(code + kSearchAt + offset, bytes.data(), bytes.size());
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(code) + kSearchAt;
            Disasm::FunctionClues clues;
            clues.Floor = base;
            clues.Ceiling = base + 0x100;
            clues.FloorIsBoundary = true;
            const auto find = [&](std::uint64_t offset) { return Disasm::FindFunction(engine.Disassembler(), *engine.Process(), base + offset, nullptr, clues); };
            const auto expect = [&](std::uint64_t offset, std::uint64_t start, std::uint64_t end, std::uint64_t last, Disasm::FunctionSource source)
                {
                    const auto span = find(offset);
                    CHECK_MSG(span.has_value(), "offset " + std::to_string(offset));
                    if (!span)
                        return;
                    CHECK_MSG(span->Start == base + start, "offset " + std::to_string(offset) + " start +" + std::to_string(span->Start - base));
                    CHECK_MSG(span->End == base + end, "offset " + std::to_string(offset) + " end +" + std::to_string(span->End - base));
                    CHECK_MSG(span->Last == base + last, "offset " + std::to_string(offset) + " last +" + std::to_string(span->Last - base));
                    CHECK_MSG(span->Source == source, "offset " + std::to_string(offset));
                };
            expect(0x17, 0x10, 0x1D, 0x1C, Disasm::FunctionSource::Padding);
            expect(0x10, 0x10, 0x1D, 0x1C, Disasm::FunctionSource::Padding);
            expect(0x21, 0x20, 0x23, 0x22, Disasm::FunctionSource::Padding);
            expect(0x1D, 0x10, 0x1D, 0x1C, Disasm::FunctionSource::Padding);
            expect(0x3A, 0x30, 0x40, 0x3F, Disasm::FunctionSource::Padding);
            expect(0x60, 0x50, 0x66, 0x65, Disasm::FunctionSource::Padding);
            expect(0x80, 0x80, 0x86, 0x85, Disasm::FunctionSource::Guess);
            expect(0x92, 0x90, 0x94, 0x92, Disasm::FunctionSource::Padding);
            expect(0xA0, 0xA0, 0xA6, 0xA5, Disasm::FunctionSource::Padding);
            CHECK(!find(0x08).has_value());

            clues.KnownStart = [&](std::uint64_t address) { return address == base + 0x3A; };
            expect(0x3F, 0x3A, 0x40, 0x3F, Disasm::FunctionSource::Padding);
            clues.KnownStart = {};

            Disasm::FunctionClues blind = clues;
            blind.Floor = base + 0x10;
            blind.FloorIsBoundary = false;
            CHECK(!Disasm::FindFunction(engine.Disassembler(), *engine.Process(), base + 0x17, nullptr, blind).has_value());

            clues.Exact = [&](std::uint64_t address) -> std::optional<Disasm::FunctionSpan>
                {
                    if (address == base + 0x17)
                        return Disasm::FunctionSpan{ base + 0x12, base + 0x1D, base + 0x1C, Disasm::FunctionSource::Unwind };
                    return std::nullopt;
                };
            expect(0x17, 0x12, 0x1D, 0x1C, Disasm::FunctionSource::Unwind);
        }

        constexpr std::size_t kHashAt = 0x600;

        void HashBeforeFunction(Engine& engine, std::uint8_t* code)
        {
            const std::uint8_t before[] = { 0x31, 0xC0, 0xC3 };
            const std::uint8_t hash[] = { 0x71, 0x38, 0x56, 0x12, 0x14, 0xBF, 0x23, 0x89 };
            const std::uint8_t function[] = { 0xB8, 0x09, 0x00, 0x00, 0x00, 0xC3 };
            const std::uint8_t leaf[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
            std::memcpy(code + kHashAt, before, sizeof(before));
            std::memcpy(code + kHashAt + 0x08, hash, sizeof(hash));
            std::memcpy(code + kHashAt + 0x10, function, sizeof(function));
            std::memcpy(code + kHashAt + 0x100, before, sizeof(before));
            std::memcpy(code + kHashAt + 0x108, leaf, sizeof(leaf));
            std::memcpy(code + kHashAt + 0x110, function, sizeof(function));
            const std::uint64_t base = reinterpret_cast<std::uint64_t>(code) + kHashAt;
            const auto find = [&](std::uint64_t offset)
                {
                    const std::uint64_t group = offset & ~std::uint64_t(0xFF);
                    Disasm::FunctionClues clues;
                    clues.Floor = base + group + sizeof(before);
                    clues.FloorIsBoundary = true;
                    clues.Ceiling = base + group + 0x10;
                    return Disasm::FindFunction(engine.Disassembler(), *engine.Process(), base + offset, nullptr, clues);
                };
            for (const std::uint64_t offset : { 0x08, 0x0A, 0x0E })
            {
                const auto span = find(offset);
                CHECK_MSG(!span.has_value(), "8 bytes before a known function at +" + std::to_string(offset) + " were taken for a function at +" +
                    (span ? std::to_string(span->Start - base) + "..+" + std::to_string(span->End - base) : std::string()));
            }
            const auto real = find(0x10A);
            CHECK(real && real->Start == base + 0x108 && real->End == base + 0x10E && real->Source == Disasm::FunctionSource::Padding);
        }

        void PrintLines(Engine& engine, const Target::ModuleInfo& module, std::uint64_t from, std::uint64_t until)
        {
            for (const Disasm::Instruction& line : engine.Disassembler().ReadMany(*engine.Process(), from, 10))
            {
                if (line.Address >= until)
                    break;
                std::printf("        %s+%llX  %-24s %s\n", module.Name.c_str(), static_cast<unsigned long long>(line.Address - module.Base), line.BytesText().c_str(), line.Text().c_str());
            }
        }

        GapTally GapLeaves(Engine& engine, const Target::ModuleInfo& module, int limit, int shown)
        {
            GapTally tally;
            const auto functions = Target::CodeMap::UnwindFunctions(*engine.Process(), module.Base, module.Size);
            for (std::size_t f = 0; f + 1 < functions.size() && tally.Leaves < limit; ++f)
            {
                const std::uint64_t gap = module.Base + functions[f].second;
                const std::uint64_t next = module.Base + functions[f + 1].first;
                if (next <= gap + 4)
                    continue;
                std::optional<std::uint64_t> code;
                for (const Disasm::Instruction& line : engine.Disassembler().ReadMany(*engine.Process(), gap, 16))
                {
                    if (line.Address >= next)
                        break;
                    if (line.Valid && !line.IsNop && line.Mnemonic != "nop" && line.Mnemonic != "int3")
                    {
                        code = line.Address;
                        break;
                    }
                }
                if (!code)
                    continue;
                ++tally.Leaves;
                const auto leaf = engine.FunctionAround(*code);
                const bool inside = leaf && leaf->Start >= gap && leaf->Start <= *code && leaf->End > *code && leaf->End <= next && leaf->Source != Disasm::FunctionSource::Unwind;
                if (inside)
                {
                    ++tally.Inside;
                    continue;
                }
                if (leaf)
                    ++tally.Wrong;
                else
                    ++tally.None;
                if (shown-- <= 0)
                    continue;
                const auto offset = [&](std::uint64_t address) { return static_cast<unsigned long long>(address - module.Base); };
                if (leaf)
                    std::printf("    gap %s+%llX..%llX, code at +%llX: found +%llX..%llX (%s)\n", module.Name.c_str(), offset(gap), offset(next), offset(*code), offset(leaf->Start),
                        offset(leaf->End), SourceName(leaf->Source));
                else
                    std::printf("    gap %s+%llX..%llX, code at +%llX: not found\n", module.Name.c_str(), offset(gap), offset(next), offset(*code));
                PrintLines(engine, module, gap > module.Base + 8 ? gap - 8 : gap, next);
            }
            return tally;
        }

        void FunctionOfExport(Engine& engine)
        {
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            const auto proc = reinterpret_cast<std::uint64_t>(ntdll ? ::GetProcAddress(ntdll, "RtlEqualUnicodeString") : nullptr);
            CHECK(proc != 0);
            if (!proc)
                return;
            const auto span = engine.FunctionAround(proc + 0x10);
            CHECK(span.has_value());
            if (!span)
                return;
            CHECK(span->Source == Disasm::FunctionSource::Unwind);
            CHECK(span->Start == proc);
            CHECK(span->End > proc + 0x10);
            CHECK(span->Last > span->Start && span->Last < span->End);
            CHECK(engine.Disassembler().Read(*engine.Process(), span->Last, nullptr).Next() == span->End);

            const auto module = engine.Process()->ModuleAt(proc);
            CHECK(module.has_value());
            if (!module)
                return;
            const GapTally gaps = GapLeaves(engine, *module, 40, 10);
            std::printf("  %d of %d leaf functions between unwind entries of %s found inside their gap, %d not found, %d found wrong\n", gaps.Inside, gaps.Leaves,
                module->Name.c_str(), gaps.None, gaps.Wrong);
            CHECK(gaps.Leaves > 0);
            CHECK_MSG(gaps.Wrong == 0, "a function found for code between unwind entries reaches outside its gap");
            CHECK_MSG(gaps.Inside * 4 >= gaps.Leaves * 3, "fewer than three quarters of the leaf functions between unwind entries were found");
        }

        void LayoutShapes()
        {
            CHECK(Graph::LayeredLayout({}, {}, 0).Positions.empty());

            const std::vector<Graph::Size> one{ { 120.0f, 40.0f } };
            const Graph::Result single = Graph::LayeredLayout(one, {}, 0);
            CHECK(single.Positions.size() == 1 && single.Positions[0].X == 0.0f && single.Positions[0].Y == 0.0f);
            CHECK(single.Width == 120.0f && single.Height == 40.0f);

            const std::vector<Graph::Size> sizes{ { 200.0f, 60.0f }, { 140.0f, 90.0f }, { 260.0f, 40.0f }, { 180.0f, 70.0f }, { 100.0f, 30.0f } };
            const std::vector<Graph::Edge> edges{ { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 3, 1 }, { 3, 3 }, { 3, 4 }, { 0, 4 } };
            const Graph::Result diamond = Graph::LayeredLayout(sizes, edges, 0);
            CheckLayout(sizes, edges, diamond);
            CHECK(diamond.Layers == (std::vector<int>{ 0, 1, 1, 2, 3 }));
            CHECK(diamond.Back == (std::vector<bool>{ false, false, false, false, true, true, false, false }));
            CHECK(diamond.Positions[1].Y == diamond.Positions[2].Y);
            CHECK(diamond.Positions[3].Y >= diamond.Positions[1].Y + 90.0f);
            CHECK(diamond.Routes[7].size() > 4);
            const float side = std::max({ diamond.Positions[1].X + 140.0f, diamond.Positions[2].X + 260.0f, diamond.Positions[3].X + 180.0f });
            CHECK(diamond.Routes[4].size() == 6 && diamond.Routes[4][2].X > side);

            std::mt19937 random(1234);
            std::vector<Graph::Size> many;
            std::vector<Graph::Edge> links;
            for (int v = 0; v < 600; ++v)
            {
                many.push_back({ 80.0f + static_cast<float>(random() % 300), 30.0f + static_cast<float>(random() % 200) });
                if (v > 0)
                    links.push_back({ static_cast<int>(random() % static_cast<unsigned>(v)), v });
                if (v > 4 && random() % 3 == 0)
                    links.push_back({ v, static_cast<int>(random() % static_cast<unsigned>(v)) });
                if (v > 4 && random() % 4 == 0)
                    links.push_back({ static_cast<int>(random() % static_cast<unsigned>(v)), v });
            }
            const auto started = std::chrono::steady_clock::now();
            const Graph::Result large = Graph::LayeredLayout(many, links, 0);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            std::printf("  layout of %zu blocks and %zu edges took %.1f ms\n", many.size(), links.size(), ms);
            CheckLayout(many, links, large);
        }

        void MethodConstants(Engine& engine)
        {
            // A JIT method shaped like Mono's: code, a ret, a few bytes of the JIT's, then the float
            // constants the code reads RIP-relative, all inside the method's code size.
            static constexpr std::uint8_t kMethod[] = {
                0x55,                                      // +00 push rbp
                0x48, 0x8B, 0xEC,                          // +01 mov rbp,rsp
                0xF3, 0x0F, 0x10, 0x05, 0x14, 0, 0, 0,     // +04 movss xmm0,[+20]
                0x85, 0xC0,                                // +0C test eax,eax
                0x74, 0x08,                                // +0E jz +18
                0xF3, 0x0F, 0x10, 0x05, 0x0C, 0, 0, 0,     // +10 movss xmm0,[+24]
                0x5D,                                      // +18 pop rbp
                0xC3,                                      // +19 ret
                0xCB, 0x01, 0x00, 0x00, 0x00, 0x00,        // +1A the JIT's bytes
                0x00, 0x00, 0xC8, 0x42,                    // +20 100.0f
                0x00, 0x00, 0x80, 0x3F,                    // +24 1.0f
            };
            // A switch: the cases behind the indirect jmp are code, only the constant is data.
            static constexpr std::uint8_t kSwitch[] = {
                0xF3, 0x0F, 0x10, 0x05, 0x08, 0, 0, 0,     // +00 movss xmm0,[+10]
                0xFF, 0xE0,                                // +08 jmp rax
                0x31, 0xC0,                                // +0A xor eax,eax
                0xC3,                                      // +0C ret
                0xCC, 0xCC, 0xCC,                          // +0D
                0x00, 0x00, 0x80, 0x3F,                    // +10 1.0f
            };
            auto* memory = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            CHECK(memory != nullptr);
            if (!memory)
                return;
            std::memset(memory, 0xCC, 0x1000);
            std::memcpy(memory, kMethod, sizeof(kMethod));
            std::memcpy(memory + 0x100, kSwitch, sizeof(kSwitch));
            const auto base = reinterpret_cast<std::uint64_t>(memory);
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            const Target::Process& process = *engine.Process();

            const auto tail = Disasm::DataTail(disassembler, process, base, base + sizeof(kMethod), nullptr);
            CHECK_MSG(tail && *tail == base + 0x1A, "the data behind a JIT method starts right after its ret");
            CHECK(!Disasm::DataTail(disassembler, process, base, base + 0x1A, nullptr));
            const auto cases = Disasm::DataTail(disassembler, process, base + 0x100, base + 0x100 + sizeof(kSwitch), nullptr);
            CHECK_MSG(cases && *cases == base + 0x110, "the cases behind a switch's indirect jmp stay code");
            ::VirtualFree(memory, 0, MEM_RELEASE);
        }
    }

    int RunFlowGraphTests()
    {
        const int before = Totals().Failures;
        LayoutShapes();
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
        CHECK_MSG(opened.Ok(), opened.Text);
        if (!opened)
            return Totals().Failures - before;
        auto* code = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(code != nullptr);
        if (code)
        {
            std::memset(code, 0xCC, 0x1000);
            std::memcpy(code, kDiamondLoop, sizeof(kDiamondLoop));
            std::memcpy(code + kTailAt, kTailJump, sizeof(kTailJump));
            code[kCalleeAt] = 0xC3;
            code[kIndirectAt] = 0xFF;
            code[kIndirectAt + 1] = 0xE0;
            DiamondAndLoop(engine, code);
            FunctionSearch(engine, code);
            HashBeforeFunction(engine, code);
            TailJumpsAndIndirect(engine, code);
            SwitchTables(engine, code);
            ::VirtualFree(code, 0, MEM_RELEASE);
        }
        FunctionOfExport(engine);
        MethodConstants(engine);
        engine.CloseProcess();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    namespace
    {
        struct SearchTally
        {
            int Right = 0;
            int Early = 0;
            int Late = 0;
            int Missing = 0;
        };

        int RunFunctionSearchCheck(const std::vector<std::string>& arguments)
        {
            int perModule = 150;
            bool boundary = false;
            bool dump = false;
            for (const std::string& argument : arguments)
            {
                if (argument.rfind("--functions=", 0) == 0)
                    perModule = std::max(1, std::atoi(argument.c_str() + 12));
                if (argument == "--boundary")
                    boundary = true;
                if (argument == "--dump")
                    dump = true;
            }
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            const Status opened = engine.OpenProcess(MakeProcessId(::GetCurrentProcessId()));
            if (!opened)
            {
                std::printf("%s\n", opened.Text.c_str());
                return 1;
            }
            const Target::Process& process = *engine.Process();
            const Disasm::Disassembler& disassembler = engine.Disassembler();
            const char* names[] = { "ntdll.dll", "KERNELBASE.dll", "ucrtbase.dll", "Manifold-Engine.Tests.exe" };
            if (std::find(arguments.begin(), arguments.end(), "--gaps") != arguments.end())
            {
                GapTally all;
                for (const Target::ModuleInfo& module : *process.Modules())
                {
                    if (Lower(WideToUtf8(module.Path)).find("\\system32\\") == std::string::npos &&
                        std::none_of(std::begin(names), std::end(names), [&](const char* name) { return EqualsNoCase(module.Name, name); }))
                        continue;
                    const GapTally gaps = GapLeaves(engine, module, perModule, dump ? 12 : 0);
                    if (gaps.Leaves == 0)
                        continue;
                    std::printf("  %-28s leaf functions %4d  inside %4d  not found %3d  wrong %3d\n", module.Name.c_str(), gaps.Leaves, gaps.Inside, gaps.None, gaps.Wrong);
                    all.Leaves += gaps.Leaves;
                    all.Inside += gaps.Inside;
                    all.None += gaps.None;
                    all.Wrong += gaps.Wrong;
                }
                std::printf("%d leaf functions between unwind entries: inside %d, not found %d, wrong %d\n", all.Leaves, all.Inside, all.None, all.Wrong);
                engine.CloseProcess();
                return all.Wrong == 0 ? 0 : 1;
            }
            SearchTally total;
            int worst = 0;
            const auto started = std::chrono::steady_clock::now();
            for (const Target::ModuleInfo& module : *process.Modules())
            {
                if (std::none_of(std::begin(names), std::end(names), [&](const char* name) { return EqualsNoCase(module.Name, name); }))
                    continue;
                const auto functions = Target::CodeMap::UnwindFunctions(process, module.Base, module.Size);
                if (functions.size() < 2)
                    continue;
                SearchTally tally;
                const std::size_t step = std::max<std::size_t>(1, functions.size() / static_cast<std::size_t>(perModule));
                for (std::size_t f = 1; f < functions.size(); f += step)
                {
                    const std::uint64_t begin = module.Base + functions[f].first;
                    const std::uint64_t end = module.Base + functions[f].second;
                    const std::vector<std::uint64_t> starts = disassembler.Starts(process, begin, end);
                    if (starts.empty())
                        continue;
                    for (const std::uint64_t address : { starts.front(), starts[starts.size() / 2], starts.back() })
                    {
                        Disasm::FunctionClues clues;
                        clues.Floor = boundary ? module.Base + functions[f - 1].second : std::max(module.Base, address > 0x10000 ? address - 0x10000 : 0);
                        clues.FloorIsBoundary = boundary;
                        clues.Ceiling = std::min(module.Base + module.Size, address + 0x10000);
                        const auto span = Disasm::FindFunction(disassembler, process, address, nullptr, clues);
                        if (!span && dump && worst < 24)
                        {
                            ++worst;
                            std::printf("    %s+%llX: none, unwind data says %s+%llX..%llX (%llu bytes, %zu instructions)\n", module.Name.c_str(), static_cast<unsigned long long>(address - module.Base),
                                module.Name.c_str(), static_cast<unsigned long long>(begin - module.Base), static_cast<unsigned long long>(end - module.Base),
                                static_cast<unsigned long long>(end - begin), starts.size());
                            if (address == begin)
                            {
                                for (const Disasm::Instruction& line : disassembler.ReadMany(process, begin - 0x18, 16))
                                {
                                    if (line.Address > begin + 4)
                                        break;
                                    std::printf("      %s+%llX  %s  %s\n", module.Name.c_str(), static_cast<unsigned long long>(line.Address - module.Base), line.BytesText().c_str(), line.Text().c_str());
                                }
                            }
                        }
                        if (!span)
                            ++tally.Missing;
                        else if (span->Start == begin)
                            ++tally.Right;
                        else if (span->Start < begin)
                            ++tally.Early;
                        else
                            ++tally.Late;
                        if (span && span->Start != begin && worst < 12)
                        {
                            ++worst;
                            std::printf("    %s+%llX: found %s+%llX (%s), unwind data says %s+%llX..%llX\n", module.Name.c_str(), static_cast<unsigned long long>(address - module.Base),
                                module.Name.c_str(), static_cast<unsigned long long>(span->Start - module.Base), span->Source == Disasm::FunctionSource::Guess ? "guess" : "padding",
                                module.Name.c_str(), static_cast<unsigned long long>(begin - module.Base), static_cast<unsigned long long>(end - module.Base));
                            if (dump)
                            {
                                for (const Disasm::Instruction& line : disassembler.ReadMany(process, span->Start > begin + 0x30 ? span->Start - 0x30 : begin, 24))
                                {
                                    if (line.Address > span->Start + 8)
                                        break;
                                    std::printf("      %s+%llX  %s\n", module.Name.c_str(), static_cast<unsigned long long>(line.Address - module.Base), line.Text().c_str());
                                }
                            }
                        }
                    }
                }
                std::printf("  %-28s right %4d  too early %3d  too late %3d  none %3d\n", module.Name.c_str(), tally.Right, tally.Early, tally.Late, tally.Missing);
                total.Right += tally.Right;
                total.Early += tally.Early;
                total.Late += tally.Late;
                total.Missing += tally.Missing;
            }
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const int all = total.Right + total.Early + total.Late + total.Missing;
            std::printf("%d searches in %.1f s: right %d, too early %d, too late %d, none %d\n", all, seconds, total.Right, total.Early, total.Late, total.Missing);
            engine.CloseProcess();
            return 0;
        }
    }

    MANIFOLD_SUITE("flowgraph", "Flow graph", 35, [](const SuiteContext&) { return RunFlowGraphTests(); });
    MANIFOLD_MODE("--function-search-check", [](const std::vector<std::string>& arguments) { return RunFunctionSearchCheck(arguments); });
}
