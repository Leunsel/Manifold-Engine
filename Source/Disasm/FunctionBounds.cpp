#include "Disasm/FunctionBounds.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace Manifold::Disasm
{
    namespace
    {
        constexpr int kChunk = 64;
        constexpr int kMaxGraphs = 32;
        constexpr std::uint64_t kAlignment = 16;
        constexpr std::uint64_t kAhead = 0x4000;

        enum class Evidence
        {
            Weak,
            Boundary,
            Strong
        };

        struct Candidate
        {
            std::uint64_t Address = 0;
            Evidence Kind = Evidence::Weak;
        };

        struct Sweep
        {
            std::vector<Candidate> Found;
            std::uint64_t LastReal = 0;
            bool OnPadding = false;
        };

        bool IsTrap(const Instruction& line)
        {
            return line.Mnemonic == "int3";
        }

        bool IsPadding(const Instruction& line)
        {
            return line.Valid && (line.IsNop || line.Mnemonic == "nop" || IsTrap(line));
        }

        bool ZeroPair(const Instruction& line)
        {
            return line.Valid && line.Length == 2 && line.Bytes[0] == 0 && line.Bytes[1] == 0;
        }

        bool EndsFlow(const Instruction& line)
        {
            const std::string_view mnemonic = line.Mnemonic;
            return line.Kind == Flow::Return || line.Kind == Flow::Jump || line.Kind == Flow::Interrupt || mnemonic == "ud2" || mnemonic == "hlt";
        }

        Sweep Candidates(const Disassembler& disassembler, const Target::Process& process, std::uint64_t address, std::uint64_t until, const CodeHints* hints,
            const FunctionClues& clues)
        {
            Sweep sweep;
            std::vector<Candidate>& found = sweep.Found;
            bool ended = clues.FloorIsBoundary;
            bool padded = false;
            int traps = clues.FloorIsBoundary ? 2 : 0;
            bool first = clues.FloorIsBoundary;
            std::uint64_t at = clues.Floor;
            while (at < until)
            {
                const std::vector<Instruction> chunk = disassembler.ReadMany(process, at, kChunk, nullptr, hints);
                if (chunk.empty())
                    break;
                for (const Instruction& line : chunk)
                {
                    if (line.Address >= until)
                    {
                        at = line.Address;
                        break;
                    }
                    at = line.Next();
                    if (line.Address == address)
                        sweep.OnPadding = IsPadding(line);
                    else if (line.Address < address && line.Valid && !IsPadding(line))
                        sweep.LastReal = line.Address;
                    if (!line.Valid)
                    {
                        ended = true;
                        padded = false;
                        traps = 0;
                        first = false;
                        continue;
                    }
                    if (IsPadding(line))
                    {
                        padded = true;
                        traps += IsTrap(line) ? 1 : 0;
                        continue;
                    }
                    const bool aligned = line.Address % kAlignment == 0;
                    const bool known = clues.KnownStart && clues.KnownStart(line.Address);
                    if (known || first || (ended && traps >= 2 && aligned))
                        found.push_back({ line.Address, Evidence::Strong });
                    else if (ended && padded && traps == 0 && aligned)
                        found.push_back({ line.Address, Evidence::Boundary });
                    else if ((padded && (ended || traps > 0)) || (ended && aligned))
                        found.push_back({ line.Address, Evidence::Weak });
                    ended = EndsFlow(line);
                    padded = false;
                    traps = 0;
                    first = false;
                }
            }
            return sweep;
        }
    }

    std::optional<FunctionSpan> FindFunction(const Disassembler& disassembler, const Target::Process& process, std::uint64_t address, const CodeHints* hints,
        const FunctionClues& clues)
    {
        if (clues.Exact)
        {
            if (const auto exact = clues.Exact(address))
                return exact;
        }
        if (address < clues.Floor || address >= clues.Ceiling)
            return std::nullopt;

        const Sweep sweep = Candidates(disassembler, process, address, std::min(clues.Ceiling, address + kAhead), hints, clues);
        const std::vector<Candidate>& candidates = sweep.Found;
        const auto above = std::upper_bound(candidates.begin(), candidates.end(), address, [](std::uint64_t value, const Candidate& candidate) { return value < candidate.Address; });
        const auto reachable = static_cast<std::size_t>(above - candidates.begin());
        std::vector<std::uint64_t> starts;
        for (const Candidate& candidate : candidates)
        {
            if (candidate.Kind != Evidence::Weak)
                starts.push_back(candidate.Address);
        }
        const auto build = [&](std::uint64_t entry, bool full)
            {
                const auto other = [&](std::uint64_t target, EdgeKind kind)
                    {
                        if (target < clues.Floor || target >= clues.Ceiling)
                            return true;
                        if (target == entry)
                            return false;
                        if (clues.KnownStart && clues.KnownStart(target))
                            return true;
                        return kind == EdgeKind::Jump && std::binary_search(starts.begin(), starts.end(), target);
                    };
                FlowLimits limits;
                limits.Reach = clues.Ceiling - clues.Floor;
                limits.StopAt = full ? 0 : address;
                return BuildFlowGraph(disassembler, process, entry, nullptr, hints, other, limits);
            };

        std::optional<std::size_t> chosen;
        int graphs = 0;
        for (std::size_t i = reachable; i-- > 0 && graphs < kMaxGraphs;)
        {
            ++graphs;
            if (build(candidates[i].Address, false).BlockAt(address) >= 0)
                chosen = i;
            else if (candidates[i].Kind != Evidence::Weak)
                break;
        }
        if (!chosen)
        {
            if (sweep.OnPadding && sweep.LastReal)
                return FindFunction(disassembler, process, sweep.LastReal, hints, clues);
            return std::nullopt;
        }
        const FlowGraph graph = build(candidates[*chosen].Address, true);
        if (std::any_of(graph.Blocks.begin(), graph.Blocks.end(), [&](const FlowBlock& block) { return block.End > clues.Ceiling; }))
            return std::nullopt;
        FunctionSpan span;
        span.Start = candidates[*chosen].Address;
        span.Source = candidates[*chosen].Kind == Evidence::Strong ? FunctionSource::Padding : FunctionSource::Guess;
        span.End = span.Start + 1;
        span.Last = span.Start;
        for (const FlowBlock& block : graph.Blocks)
        {
            if (block.End > span.End && !block.Lines.empty())
            {
                span.End = block.End;
                span.Last = block.Lines.back().Address;
            }
        }
        return span;
    }

    std::uint64_t CodeExtent(const Disassembler& disassembler, const Target::Process& process, std::uint64_t start, std::uint64_t limit, const CodeHints* hints)
    {
        std::uint64_t end = start;
        std::uint64_t pending = 0;
        while (end < limit)
        {
            const std::vector<Instruction> chunk = disassembler.ReadMany(process, end, kChunk, nullptr, hints);
            if (chunk.empty())
                break;
            for (const Instruction& line : chunk)
            {
                if (line.Address >= limit || (line.Address != start && (!line.Valid || IsTrap(line) || ZeroPair(line))))
                    return end;
                end = line.Next();
                const bool branch = line.Kind == Flow::Jump || line.Kind == Flow::ConditionalJump;
                if (branch && line.Target && *line.Target > line.Address && *line.Target < limit)
                    pending = std::max(pending, *line.Target);
                if (EndsFlow(line) && pending < end)
                    return end;
            }
        }
        return end;
    }

    std::optional<std::uint64_t> DataTail(const Disassembler& disassembler, const Target::Process& process, std::uint64_t start, std::uint64_t end,
        const CodeHints* hints)
    {
        // ∑ A JIT puts a method's float and double constants behind its code and counts them in the
        //   code size (Mono: GameplayData:InterestRateSet reads 100.0f from +B0 and +C0, behind the
        //   ret at +AB). The code reads them through absolute or RIP-relative operands, so the lowest
        //   such target ahead of the reading instruction is where the data starts: an indirect jump
        //   (switch) cannot cut it short the way a reachability walk would.
        constexpr std::uint64_t kMaxSpan = 0x40000;
        constexpr std::uint64_t kFiller = 16;
        if (end <= start || end - start > kMaxSpan)
            return std::nullopt;
        std::uint64_t data = end;
        std::uint64_t at = start;
        while (at < data)
        {
            const std::vector<Instruction> chunk = disassembler.ReadMany(process, at, kChunk, nullptr, hints);
            if (chunk.empty())
                break;
            for (const Instruction& line : chunk)
            {
                if (line.Address >= data)
                    break;
                at = line.Next();
                if (!line.Valid || !line.Memory.Present || !line.Memory.Read || !line.Memory.Absolute || line.Kind != Flow::None)
                    continue;
                const std::uint64_t target = *line.Memory.Absolute;
                if (target >= line.Next() && target < data)
                    data = target;
            }
        }
        if (data >= end)
            return std::nullopt;
        // ∑ The few bytes between the last ret and the first constant are the JIT's too, not code. Only
        //   behind a ret: behind an indirect jmp the cases of a switch may follow.
        const std::uint64_t code = CodeExtent(disassembler, process, start, data, hints);
        if (code > start && code < data && data - code <= kFiller)
        {
            const std::uint64_t last = disassembler.BackUp(process, code, 1, hints);
            const Instruction ending = disassembler.Read(process, last);
            if (ending.Valid && ending.Kind == Flow::Return && ending.Next() == code)
                return code;
        }
        return data;
    }

    const char* FunctionSourceText(FunctionSource source)
    {
        switch (source)
        {
        case FunctionSource::Unwind: return "from the module's unwind data";
        case FunctionSource::Managed: return "from the managed runtime";
        case FunctionSource::Padding: return "found by searching back: it is aligned after int3 padding or a known start, a path from there leads here, and the function before it does not lead here";
        case FunctionSource::Guess: return "guessed: it follows a return or nop padding and a path from there leads here, but there is no aligned int3 padding before it";
        }
        return "";
    }
}
