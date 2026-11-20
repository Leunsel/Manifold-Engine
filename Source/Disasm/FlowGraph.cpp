#include "Disasm/FlowGraph.h"

#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
#include <cstring>
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

        constexpr std::size_t kMaxCases = 1024;
        constexpr std::size_t kLookBack = 12;

        std::string_view FirstOperand(const Instruction& line)
        {
            const std::string_view text = line.Operands;
            return text.substr(0, text.find(','));
        }

        std::optional<std::uint64_t> LoadedAddress(const Instruction& line, bool wide)
        {
            if (line.Mnemonic != "lea")
                return std::nullopt;
            std::size_t at = 0;
            while (wide && at < line.Length && (line.Bytes[at] & 0xF0) == 0x40)
                ++at;
            if (at + 6 != line.Length || line.Bytes[at] != 0x8D || (line.Bytes[at + 1] & 0xC7) != 0x05)
                return std::nullopt;
            std::int32_t displacement = 0;
            std::memcpy(&displacement, &line.Bytes[at + 2], sizeof(displacement));
            if (!wide)
                return static_cast<std::uint32_t>(displacement);
            return line.Next() + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement));
        }

        std::optional<std::size_t> CompareImmediate(const Instruction& line)
        {
            if (line.Mnemonic != "cmp" || line.Length < 2)
                return std::nullopt;
            std::size_t at = 0;
            bool narrow = false;
            for (; at + 1 < line.Length && (line.Bytes[at] == 0x66 || (line.Bytes[at] & 0xF0) == 0x40); ++at)
                narrow = narrow || line.Bytes[at] == 0x66;
            const std::uint8_t opcode = line.Bytes[at];
            std::uint32_t value = 0;
            if (opcode == 0x83 || opcode == 0x80 || opcode == 0x3C)
            {
                value = line.Bytes[line.Length - 1];
                if (value >= 0x80)
                    return std::nullopt;
            }
            else if (opcode == 0x81 || opcode == 0x3D)
            {
                const std::size_t width = narrow ? 2 : 4;
                if (line.Length < width + 1)
                    return std::nullopt;
                for (std::size_t i = 0; i < width; ++i)
                    value |= static_cast<std::uint32_t>(line.Bytes[line.Length - width + i]) << (8 * i);
            }
            else
                return std::nullopt;
            return value <= kMaxCases ? std::optional<std::size_t>(value) : std::nullopt;
        }

        std::optional<std::size_t> CasesBehind(const std::vector<Instruction>& lines, bool taken)
        {
            if (lines.size() < 2 || lines.back().Kind != Flow::ConditionalJump)
                return std::nullopt;
            const std::string_view jump = lines.back().Mnemonic;
            const bool leavesAbove = jump == "ja" || jump == "jnbe";
            const bool leavesAboveOrEqual = jump == "jae" || jump == "jnb" || jump == "jnc";
            const bool entersBelowOrEqual = jump == "jbe" || jump == "jna";
            const bool entersBelow = jump == "jb" || jump == "jc" || jump == "jnae";
            if (taken ? !(entersBelowOrEqual || entersBelow) : !(leavesAbove || leavesAboveOrEqual))
                return std::nullopt;
            for (std::size_t back = 2; back <= lines.size() && back <= 5; ++back)
            {
                if (const auto bound = CompareImmediate(lines[lines.size() - back]))
                    return *bound + (leavesAbove || entersBelowOrEqual ? 1u : 0u);
            }
            return std::nullopt;
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

        const auto inside = [&](std::uint64_t target, EdgeKind kind)
            {
                if (target == entry)
                    return true;
                const std::uint64_t distance = target > entry ? target - entry : entry - target;
                if (distance > limits.Reach)
                    return false;
                return !(otherFunction && otherFunction(target, kind));
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

        const auto loadedAddress = [&](int reg, const std::vector<Instruction>& own) -> std::optional<std::uint64_t>
            {
                if (!reg)
                    return std::nullopt;
                const std::string name = Disassembler::RegisterName(reg);
                const auto value = [&](const Instruction& line) -> std::optional<std::uint64_t>
                    {
                        if (!EqualsNoCase(FirstOperand(line), name))
                            return std::nullopt;
                        return LoadedAddress(line, process.Is64Bit());
                    };
                for (auto line = own.rbegin(); line != own.rend(); ++line)
                {
                    if (const auto found = value(*line))
                        return found;
                }
                std::optional<std::uint64_t> only;
                for (const Draft& draft : drafts)
                {
                    for (const Instruction& line : draft.Block.Lines)
                    {
                        const auto found = value(line);
                        if (found && only && *only != *found)
                            return std::nullopt;
                        if (found)
                            only = found;
                    }
                }
                return only;
            };
        const auto caseCount = [&](std::uint64_t start) -> std::optional<std::size_t>
            {
                if (const auto before = start ? containing(start - 1) : std::nullopt; before && drafts[*before].Block.End == start)
                {
                    if (const auto count = CasesBehind(drafts[*before].Block.Lines, false))
                        return count;
                }
                for (const Draft& draft : drafts)
                {
                    const bool enters = std::any_of(draft.Next.begin(), draft.Next.end(), [&](const Pending& next) { return next.Target == start && next.Kind == EdgeKind::Taken; });
                    if (const auto count = enters ? CasesBehind(draft.Block.Lines, true) : std::nullopt)
                        return count;
                }
                return std::nullopt;
            };
        const auto switchTargets = [&](const Draft& draft) -> std::vector<std::uint64_t>
            {
                const std::vector<Instruction>& lines = draft.Block.Lines;
                const Instruction& jump = lines.back();
                const bool wide = process.Is64Bit();
                const int pointer = wide ? 8 : 4;
                const auto narrowed = [&](std::uint64_t address) { return wide ? address : address & 0xFFFFFFFFull; };
                std::size_t loadAt = lines.size() - 1;
                bool relative = false;
                if (jump.Memory.Present)
                {
                    if (!jump.Memory.Index || jump.Memory.Scale != pointer || jump.Memory.Size != pointer)
                        return {};
                }
                else
                {
                    bool added = false;
                    bool found = false;
                    for (std::size_t back = 1; back < lines.size() && back <= kLookBack && !found; ++back)
                    {
                        const Instruction& line = lines[lines.size() - 1 - back];
                        if (line.Mnemonic == "add")
                            added = true;
                        else if ((line.Mnemonic == "mov" || line.Mnemonic == "movsxd") && line.Memory.Present && !line.Memory.Written && line.Memory.Size == 4 && line.Memory.Index &&
                            line.Memory.Scale == 4)
                        {
                            loadAt = lines.size() - 1 - back;
                            found = true;
                        }
                    }
                    if (!found || !added)
                        return {};
                    relative = true;
                }
                const Instruction& load = lines[loadAt];
                std::uint64_t base = 0;
                if (load.Memory.Base)
                {
                    const auto address = loadedAddress(load.Memory.Base, lines);
                    if (!address)
                        return {};
                    base = *address;
                }
                else if (relative)
                    return {};
                const std::uint64_t table = narrowed(base + static_cast<std::uint64_t>(load.Memory.Displacement));

                std::optional<std::size_t> count = caseCount(draft.Block.Start);
                if (!count || *count == 0)
                    return {};
                for (std::size_t back = 1; back <= loadAt && back <= kLookBack; ++back)
                {
                    const Instruction& line = lines[loadAt - back];
                    if (line.Mnemonic != "movzx" || !line.Memory.Present || line.Memory.Size != 1)
                        continue;
                    std::optional<std::uint64_t> indexBase = loadedAddress(line.Memory.Base, lines);
                    if (!indexBase)
                        indexBase = loadedAddress(line.Memory.Index, lines);
                    if (!indexBase && (wide || static_cast<std::uint32_t>(line.Memory.Displacement) < 0x10000u))
                        break;
                    std::vector<std::uint8_t> indexes(*count);
                    if (!process.ReadExact(narrowed(indexBase.value_or(0) + static_cast<std::uint64_t>(line.Memory.Displacement)), indexes.data(), indexes.size()))
                        return {};
                    count = static_cast<std::size_t>(*std::max_element(indexes.begin(), indexes.end())) + 1;
                    break;
                }

                const std::size_t entry = relative ? 4u : static_cast<std::size_t>(pointer);
                std::vector<std::uint64_t> targets;
                for (std::size_t i = 0; i < *count; ++i)
                {
                    const std::uint64_t at = table + i * entry;
                    std::uint64_t target = 0;
                    if (relative)
                    {
                        const auto offset = process.ReadValue<std::int32_t>(at);
                        if (!offset)
                            return {};
                        target = base + static_cast<std::uint64_t>(static_cast<std::int64_t>(*offset));
                    }
                    else if (wide)
                    {
                        const auto value = process.ReadValue<std::uint64_t>(at);
                        if (!value)
                            return {};
                        target = *value;
                    }
                    else
                    {
                        const auto value = process.ReadValue<std::uint32_t>(at);
                        if (!value)
                            return {};
                        target = *value;
                    }
                    if (!target || (target >= table && target < table + *count * entry) || !inside(target, EdgeKind::Jump))
                        return {};
                    if (std::find(targets.begin(), targets.end(), target) == targets.end())
                        targets.push_back(target);
                }
                return targets;
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
                    tail.Block.Cases = std::exchange(old.Block.Cases, 0);
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
                        {
                            const std::vector<std::uint64_t> cases = switchTargets(draft);
                            draft.Block.Indirect = cases.empty();
                            draft.Block.Cases = static_cast<int>(cases.size());
                            for (const std::uint64_t target : cases)
                            {
                                draft.Next.push_back({ target, EdgeKind::Jump });
                                work.push_back(target);
                            }
                        }
                        else if (inside(*line.Target, EdgeKind::Jump))
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
                        if (line.Target && inside(*line.Target, EdgeKind::Taken))
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
            const bool reached = limits.StopAt && limits.StopAt >= start && limits.StopAt < draft.Block.End;
            byStart[start] = drafts.size();
            drafts.push_back(std::move(draft));
            if (reached)
            {
                graph.Truncated = true;
                break;
            }
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
