#include "Disasm/FlowGraph.h"

#include <algorithm>
#include <deque>
#include <map>
#include <string_view>
#include <utility>

namespace Manifold::Disasm
{
    namespace
    {
        struct Pending
        {
            std::uint64_t Target = 0;
            EdgeKind Kind = EdgeKind::Next;
        };

        struct Draft
        {
            FlowBlock Block;
            std::vector<Pending> Next;
        };

        bool StopsExecution(const Instruction& instruction)
        {
            const std::string_view mnemonic = instruction.Mnemonic;
            return mnemonic == "int3" || mnemonic == "ud2" || mnemonic == "hlt" || mnemonic == "int1";
        }
    }

    int FlowGraph::BlockAt(std::uint64_t address) const
    {
        for (std::size_t i = 0; i < Blocks.size(); ++i)
        {
            if (address >= Blocks[i].Start && address < Blocks[i].End)
                return static_cast<int>(i);
        }
        return -1;
    }

    int FlowGraph::BlockStarting(std::uint64_t address) const
    {
        for (std::size_t i = 0; i < Blocks.size(); ++i)
        {
            if (Blocks[i].Start == address)
                return static_cast<int>(i);
        }
        return -1;
    }

    FlowGraph BuildFlowGraph(const Disassembler& disassembler, const Target::Process& process, std::uint64_t entry, const Target::Symbols* symbols,
        const CodeHints* hints, const OtherFunction& otherFunction, const FlowLimits& limits)
    {
        FlowGraph graph;
        graph.Entry = entry;
        std::vector<Draft> drafts;
        std::map<std::uint64_t, std::size_t> byStart;
        std::deque<std::uint64_t> work{ entry };
        int decoded = 0;

        const auto inside = [&](std::uint64_t target)
            {
                if (target == entry)
                    return true;
                const std::uint64_t distance = target > entry ? target - entry : entry - target;
                if (distance > limits.Reach)
                    return false;
                return !(otherFunction && otherFunction(target));
            };
        const auto containing = [&](std::uint64_t address) -> std::optional<std::size_t>
            {
                auto it = byStart.upper_bound(address);
                if (it == byStart.begin())
                    return std::nullopt;
                --it;
                const FlowBlock& block = drafts[it->second].Block;
                if (address >= block.Start && address < block.End)
                    return it->second;
                return std::nullopt;
            };

        while (!work.empty())
        {
            const std::uint64_t start = work.front();
            work.pop_front();
            if (byStart.count(start))
                continue;
            if (static_cast<int>(drafts.size()) >= limits.MaxBlocks || decoded >= limits.MaxInstructions)
            {
                graph.Truncated = true;
                break;
            }
            if (const auto owner = containing(start))
            {
                Draft& old = drafts[*owner];
                const auto cut = std::find_if(old.Block.Lines.begin(), old.Block.Lines.end(), [&](const Instruction& line) { return line.Address == start; });
                if (cut != old.Block.Lines.end())
                {
                    Draft tail;
                    tail.Block.Start = start;
                    tail.Block.End = old.Block.End;
                    tail.Block.Lines.assign(cut, old.Block.Lines.end());
                    tail.Block.Returns = old.Block.Returns;
                    tail.Block.Indirect = old.Block.Indirect;
                    tail.Block.Stops = old.Block.Stops;
                    tail.Block.Truncated = old.Block.Truncated;
                    tail.Next = std::move(old.Next);
                    for (const Instruction& line : tail.Block.Lines)
                    {
                        if (line.Kind == Flow::Call && line.Target)
                            tail.Block.Calls.push_back(*line.Target);
                    }
                    tail.Block.Exits = std::move(old.Block.Exits);
                    old.Block.Lines.erase(cut, old.Block.Lines.end());
                    old.Block.End = start;
                    old.Block.Returns = old.Block.Indirect = old.Block.Stops = old.Block.Truncated = false;
                    old.Block.Calls.clear();
                    for (const Instruction& line : old.Block.Lines)
                    {
                        if (line.Kind == Flow::Call && line.Target)
                            old.Block.Calls.push_back(*line.Target);
                    }
                    old.Next = { Pending{ start, EdgeKind::Next } };
                    byStart[start] = drafts.size();
                    drafts.push_back(std::move(tail));
                    continue;
                }
            }

            Draft draft;
            draft.Block.Start = start;
            std::uint64_t at = start;
            bool open = true;
            while (open)
            {
                const std::vector<Instruction> chunk = disassembler.ReadMany(process, at, 48, symbols, hints);
                if (chunk.empty())
                {
                    draft.Block.Truncated = true;
                    break;
                }
                for (const Instruction& line : chunk)
                {
                    if (line.Address != at)
                        break;
                    if (at != start && (byStart.count(at) || containing(at)))
                    {
                        draft.Next.push_back({ at, EdgeKind::Next });
                        if (!byStart.count(at))
                            work.push_back(at);
                        open = false;
                        break;
                    }
                    if (!line.Valid || decoded >= limits.MaxInstructions)
                    {
                        draft.Block.Truncated = true;
                        graph.Truncated = graph.Truncated || decoded >= limits.MaxInstructions;
                        open = false;
                        break;
                    }
                    draft.Block.Lines.push_back(line);
                    ++decoded;
                    at = line.Next();
                    if (line.Kind == Flow::Return)
                    {
                        draft.Block.Returns = true;
                        open = false;
                    }
                    else if (line.Kind == Flow::Jump)
                    {
                        if (!line.Target)
                            draft.Block.Indirect = true;
                        else if (inside(*line.Target))
                        {
                            draft.Next.push_back({ *line.Target, EdgeKind::Jump });
                            work.push_back(*line.Target);
                        }
                        else
                            draft.Block.Exits.push_back(*line.Target);
                        open = false;
                    }
                    else if (line.Kind == Flow::ConditionalJump)
                    {
                        if (line.Target && inside(*line.Target))
                        {
                            draft.Next.push_back({ *line.Target, EdgeKind::Taken });
                            work.push_back(*line.Target);
                        }
                        else if (line.Target)
                            draft.Block.Exits.push_back(*line.Target);
                        draft.Next.push_back({ at, EdgeKind::NotTaken });
                        work.push_back(at);
                        open = false;
                    }
                    else if (line.Kind == Flow::Call && line.Target)
                    {
                        draft.Block.Calls.push_back(*line.Target);
                    }
                    else if (StopsExecution(line))
                    {
                        draft.Block.Stops = true;
                        open = false;
                    }
                    if (!open)
                        break;
                }
            }
            draft.Block.End = draft.Block.Lines.empty() ? start + 1 : draft.Block.Lines.back().Next();
            byStart[start] = drafts.size();
            drafts.push_back(std::move(draft));
        }

        std::vector<std::size_t> order;
        order.reserve(drafts.size());
        for (const auto& [start, index] : byStart)
            order.push_back(index);
        std::stable_partition(order.begin(), order.end(), [&](std::size_t index) { return drafts[index].Block.Start == entry; });
        std::vector<int> position(drafts.size(), -1);
        for (std::size_t i = 0; i < order.size(); ++i)
            position[order[i]] = static_cast<int>(i);
        for (const std::size_t index : order)
            graph.Blocks.push_back(drafts[index].Block);
        for (const std::size_t index : order)
        {
            for (const Pending& next : drafts[index].Next)
            {
                const auto target = byStart.find(next.Target);
                if (target == byStart.end())
                    continue;
                graph.Edges.push_back({ position[index], position[target->second], next.Kind, false });
            }
        }
        graph.Instructions = decoded;

        std::vector<std::vector<int>> out(graph.Blocks.size());
        for (std::size_t e = 0; e < graph.Edges.size(); ++e)
            out[static_cast<std::size_t>(graph.Edges[e].From)].push_back(static_cast<int>(e));
        std::vector<int> state(graph.Blocks.size(), 0);
        std::vector<std::pair<int, std::size_t>> stack;
        for (std::size_t root = 0; root < graph.Blocks.size(); ++root)
        {
            if (state[root])
                continue;
            stack.push_back({ static_cast<int>(root), 0 });
            state[root] = 1;
            while (!stack.empty())
            {
                auto& [node, next] = stack.back();
                if (next < out[static_cast<std::size_t>(node)].size())
                {
                    FlowEdge& edge = graph.Edges[static_cast<std::size_t>(out[static_cast<std::size_t>(node)][next++])];
                    if (state[static_cast<std::size_t>(edge.To)] == 1)
                    {
                        edge.Back = true;
                        graph.Blocks[static_cast<std::size_t>(edge.To)].LoopHead = true;
                    }
                    else if (state[static_cast<std::size_t>(edge.To)] == 0)
                    {
                        state[static_cast<std::size_t>(edge.To)] = 1;
                        stack.push_back({ edge.To, 0 });
                    }
                    continue;
                }
                state[static_cast<std::size_t>(node)] = 2;
                stack.pop_back();
            }
        }
        return graph;
    }
}
