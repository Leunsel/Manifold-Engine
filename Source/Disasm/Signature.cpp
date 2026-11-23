#include "Disasm/Signature.h"

#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Scan/Scanner.h"
#include "Scan/Workers.h"
#include "Target/Process.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <tuple>

namespace Manifold::Disasm
{
    namespace
    {
        constexpr std::uint64_t kChunkBytes = 2ull * 1024 * 1024;
        constexpr std::size_t kAnchorWindow = 8;
        constexpr std::uint64_t kSampleBytes = 64 * 1024;
        constexpr int kSamples = 16;

        struct Probe
        {
            std::vector<std::uint8_t> Bytes;
            std::vector<std::uint8_t> Mask;
            std::uint64_t Self = 0;
            std::size_t Anchor = 0;
            bool Anchored = true;
            std::size_t Cap = 0;
            std::atomic<std::size_t> Need{ 0 };
        };

        struct Chunk
        {
            std::uint64_t Base = 0;
            std::uint64_t Size = 0;
            std::uint64_t ReadEnd = 0;
        };

        struct Space
        {
            std::uint64_t Low = 0;
            std::uint64_t High = 0;
            Scan::PatternFilter Filter;
        };

        void RaiseTo(std::atomic<std::size_t>& value, std::size_t wanted)
        {
            std::size_t current = value.load(std::memory_order_relaxed);
            while (current < wanted && !value.compare_exchange_weak(current, wanted, std::memory_order_relaxed))
            {
            }
        }

        std::vector<Chunk> Chunks(const Target::Process& process, const Space& space, std::size_t overlap)
        {
            std::vector<Chunk> chunks;
            for (const Target::RegionInfo& region : process.Regions(std::max(space.Low, process.LowestAddress()), std::min(space.High, process.HighestAddress())))
            {
                if (!region.Committed() || !region.Readable() || region.Guarded() || !space.Filter.Accepts(region))
                    continue;
                const std::uint64_t start = std::max(region.Base, space.Low);
                const std::uint64_t end = std::min(region.End(), space.High);
                for (std::uint64_t at = start; at < end; at += kChunkBytes)
                {
                    Chunk chunk;
                    chunk.Base = at;
                    chunk.Size = std::min(kChunkBytes, end - at);
                    chunk.ReadEnd = std::min(at + chunk.Size + overlap, region.End());
                    chunks.push_back(chunk);
                }
            }
            return chunks;
        }

        // How often each byte value occurs, from a few slices spread over the
        // memory searched: the anchor of a probe is its rarest fixed byte, so the
        // search looks at as few places as possible.
        std::array<std::uint32_t, 256> Histogram(const Target::Process& process, const std::vector<Chunk>& chunks)
        {
            std::array<std::uint32_t, 256> counts{};
            if (chunks.empty())
                return counts;
            std::vector<std::uint8_t> sample(kSampleBytes);
            const std::size_t step = std::max<std::size_t>(1, chunks.size() / kSamples);
            for (std::size_t index = 0; index < chunks.size(); index += step)
            {
                const std::size_t got = process.Read(chunks[index].Base, sample.data(), static_cast<std::size_t>(std::min<std::uint64_t>(kSampleBytes, chunks[index].Size)));
                for (std::size_t i = 0; i < got; ++i)
                    ++counts[sample[i]];
            }
            return counts;
        }

        void ChooseAnchor(Probe& probe, const std::array<std::uint32_t, 256>& counts)
        {
            probe.Anchored = false;
            const std::size_t window = std::min(probe.Bytes.size(), kAnchorWindow);
            for (std::size_t i = 0; i < probe.Bytes.size(); ++i)
            {
                if (probe.Mask[i] != 0xFF)
                    continue;
                if (!probe.Anchored || (i < window && counts[probe.Bytes[i]] < counts[probe.Bytes[probe.Anchor]]))
                {
                    probe.Anchor = i;
                    probe.Anchored = true;
                }
                if (i >= window)
                    break;
            }
        }

        std::size_t MatchLength(const Probe& probe, const std::uint8_t* data, std::size_t available)
        {
            const std::size_t limit = std::min(probe.Bytes.size(), available);
            std::size_t i = 0;
            while (i < limit && ((data[i] ^ probe.Bytes[i]) & probe.Mask[i]) == 0)
                ++i;
            return i;
        }

        void MeasureChunk(Probe& probe, const Chunk& chunk, const std::uint8_t* buffer, std::size_t got)
        {
            if (probe.Need.load(std::memory_order_relaxed) >= probe.Cap)
                return;
            const std::size_t starts = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.Size, got));
            std::size_t local = 0;
            if (!probe.Anchored)
            {
                for (std::size_t p = 0; p < starts && local < probe.Cap; ++p)
                {
                    if (chunk.Base + p != probe.Self)
                        local = std::max(local, MatchLength(probe, buffer + p, got - p) + 1);
                }
                RaiseTo(probe.Need, local);
                return;
            }
            const std::size_t anchor = probe.Anchor;
            const std::uint8_t wanted = probe.Bytes[anchor];
            std::size_t from = anchor;
            const std::size_t to = std::min(starts + anchor, got);
            while (from < to)
            {
                const void* hit = std::memchr(buffer + from, wanted, to - from);
                if (!hit)
                    break;
                const std::size_t q = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - buffer);
                from = q + 1;
                const std::size_t p = q - anchor;
                if (chunk.Base + p == probe.Self)
                    continue;
                const std::size_t length = MatchLength(probe, buffer + p, got - p) + 1;
                if (length > local)
                {
                    local = length;
                    if (local >= probe.Cap)
                        break;
                }
            }
            RaiseTo(probe.Need, local);
        }

        // One pass over the memory searched for every probe at once: each chunk
        // is read once and every probe looks at it while it is in the cache.
        bool Measure(const Target::Process& process, const Space& space, const std::vector<Probe*>& probes, Scan::PatternProgress* progress, std::uint64_t* searched)
        {
            if (probes.empty())
                return true;
            std::size_t longest = 0;
            for (const Probe* probe : probes)
                longest = std::max(longest, probe->Bytes.size());
            const std::vector<Chunk> chunks = Chunks(process, space, longest);
            const std::array<std::uint32_t, 256> counts = Histogram(process, chunks);
            for (Probe* probe : probes)
            {
                ChooseAnchor(*probe, counts);
                probe->Need = 0;
            }
            if (progress)
                progress->Total += chunks.size();
            std::atomic<std::size_t> next{ 0 };
            std::atomic<std::uint64_t> read{ 0 };
            const int threads = chunks.size() > 4 ? Scan::WorkerCount(0) : 1;
            Scan::RunWorkers(std::min<int>(threads, static_cast<int>(std::max<std::size_t>(1, chunks.size()))), L"Signature worker", [&](int)
                {
                    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kChunkBytes) + longest + 16);
                    for (;;)
                    {
                        const std::size_t item = next.fetch_add(1);
                        if (item >= chunks.size() || (progress && progress->Stopped()))
                            break;
                        const Chunk& chunk = chunks[item];
                        const std::size_t got = process.Read(chunk.Base, buffer.data(), static_cast<std::size_t>(chunk.ReadEnd - chunk.Base));
                        if (progress)
                            ++progress->Done;
                        if (got == 0)
                            continue;
                        read += std::min<std::uint64_t>(got, chunk.Size);
                        for (Probe* probe : probes)
                            MeasureChunk(*probe, chunk, buffer.data(), got);
                    }
                });
            if (searched)
                *searched = std::max(*searched, read.load());
            if (progress && progress->Stopped())
                return false;
            // A probe anchored past its first byte only saw the places that match
            // at the anchor. That is the whole answer when the length it found
            // reaches past the anchor; otherwise it is measured again from byte 0.
            std::vector<Probe*> again;
            for (Probe* probe : probes)
            {
                if (probe->Anchored && probe->Anchor > 0 && probe->Need <= probe->Anchor)
                {
                    probe->Anchor = 0;
                    again.push_back(probe);
                }
            }
            if (!again.empty())
            {
                next = 0;
                for (Probe* probe : again)
                    probe->Need = 0;
                Scan::RunWorkers(chunks.size() > 4 ? Scan::WorkerCount(0) : 1, L"Signature worker", [&](int)
                    {
                        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kChunkBytes) + longest + 16);
                        for (;;)
                        {
                            const std::size_t item = next.fetch_add(1);
                            if (item >= chunks.size() || (progress && progress->Stopped()))
                                break;
                            const Chunk& chunk = chunks[item];
                            const std::size_t got = process.Read(chunk.Base, buffer.data(), static_cast<std::size_t>(chunk.ReadEnd - chunk.Base));
                            if (got == 0)
                                continue;
                            for (Probe* probe : again)
                                MeasureChunk(*probe, chunk, buffer.data(), got);
                        }
                    });
            }
            for (Probe* probe : probes)
                RaiseTo(probe->Need, 1);
            return !(progress && progress->Stopped());
        }

        bool IsTerminator(const Instruction& line)
        {
            if (line.Kind == Flow::Return || line.Kind == Flow::Jump)
                return true;
            return EqualsNoCase(line.Mnemonic, "int3") || EqualsNoCase(line.Mnemonic, "ud2") || EqualsNoCase(line.Mnemonic, "hlt");
        }

        std::uint64_t Magnitude(std::int64_t value)
        {
            return value < 0 ? static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
        }

        void Classify(const Disassembler& disassembler, const Instruction& line, const SignatureOptions& options, const std::function<bool(std::uint64_t)>& isAddress,
            std::vector<SignatureByte>& out)
        {
            const std::size_t first = out.size();
            for (std::size_t i = 0; i < line.Length; ++i)
                out.push_back({ line.Bytes[i], false, ByteRole::Opcode });
            const auto layout = disassembler.Layout(line);
            if (!layout)
                return;
            for (std::size_t i = 0; i < layout->PrefixEnd; ++i)
                out[first + i].Role = ByteRole::Prefix;
            if (layout->ModRm < line.Length)
                out[first + layout->ModRm].Role = ByteRole::ModRm;
            if (layout->Sib < line.Length)
                out[first + layout->Sib].Role = ByteRole::ModRm;
            for (const InstructionField& field : layout->Fields)
            {
                ByteRole role = ByteRole::Immediate;
                bool wild = false;
                const std::uint64_t unsignedValue = field.Size >= 8 ? static_cast<std::uint64_t>(field.Value) : static_cast<std::uint64_t>(field.Value) & ((1ull << (8 * field.Size)) - 1);
                switch (field.Kind)
                {
                case FieldKind::Relative:
                    role = ByteRole::Branch;
                    wild = options.Branches == MaskMode::All || (options.Branches == MaskMode::Partial && field.Size >= 2);
                    break;
                case FieldKind::Displacement:
                    if (field.RipRelative || field.Absolute || (field.Size >= 4 && isAddress(unsignedValue)))
                    {
                        role = ByteRole::Address;
                        wild = options.MaskAddresses;
                    }
                    else
                    {
                        role = ByteRole::Offset;
                        wild = options.Offsets == MaskMode::All || (options.Offsets == MaskMode::Partial && field.Size >= 4);
                    }
                    break;
                case FieldKind::Immediate:
                    if (field.Size >= 4 && isAddress(unsignedValue))
                    {
                        role = ByteRole::Address;
                        wild = options.MaskAddresses;
                    }
                    else
                        wild = options.Immediates == MaskMode::All || (options.Immediates == MaskMode::Partial && Magnitude(field.Value) >= options.ImmediateThreshold);
                    break;
                }
                for (std::size_t i = field.Offset; i < static_cast<std::size_t>(field.Offset) + field.Size && i < line.Length; ++i)
                {
                    out[first + i].Role = role;
                    out[first + i].Wild = wild;
                }
            }
        }

        struct Built
        {
            std::uint64_t Start = 0;
            std::vector<SignatureByte> Bytes;
            std::vector<SignatureLine> Lines;
            std::optional<std::uint64_t> FunctionEnd;
        };

        Built Build(const Target::Process& process, const Disassembler& disassembler, std::uint64_t start, const SignatureOptions& options, bool code,
            const std::function<bool(std::uint64_t)>& isAddress)
        {
            Built built;
            built.Start = start;
            const std::size_t maxBytes = static_cast<std::size_t>(std::clamp(options.MaxBytes, 1, 4096));
            std::vector<std::uint8_t> buffer(maxBytes + 16);
            buffer.resize(process.Read(start, buffer.data(), buffer.size()));
            if (!code)
            {
                const std::size_t usable = std::min(buffer.size(), maxBytes);
                for (std::size_t i = 0; i < usable; ++i)
                    built.Bytes.push_back({ buffer[i], false, ByteRole::Data });
                return built;
            }
            std::size_t offset = 0;
            while (built.Bytes.size() < maxBytes && offset < buffer.size())
            {
                const Instruction line = disassembler.Decode(start + offset, buffer.data() + offset, buffer.size() - offset);
                if (!line.Valid || line.Length == 0 || offset + line.Length > buffer.size())
                    break;
                const std::size_t before = built.Bytes.size();
                Classify(disassembler, line, options, isAddress, built.Bytes);
                if (built.Bytes.size() > maxBytes)
                    built.Bytes.resize(maxBytes);
                built.Lines.push_back({ line.Address, line.Length, line.Text() });
                offset += line.Length;
                if (before < built.Bytes.size() && IsTerminator(line))
                {
                    if (options.StopAtFunctionEnd)
                        break;
                    if (!built.FunctionEnd)
                        built.FunctionEnd = line.Address;
                }
            }
            return built;
        }

        std::unique_ptr<Probe> MakeProbe(const Built& built)
        {
            auto probe = std::make_unique<Probe>();
            probe->Self = built.Start;
            for (const SignatureByte& byte : built.Bytes)
            {
                probe->Bytes.push_back(byte.Wild ? 0 : byte.Value);
                probe->Mask.push_back(byte.Wild ? 0x00 : 0xFF);
            }
            probe->Cap = probe->Bytes.size() + 1;
            return probe;
        }

        std::vector<std::uint64_t> StartsAround(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, int maxOffset)
        {
            std::vector<std::uint64_t> starts;
            const std::uint64_t reach = static_cast<std::uint64_t>(std::clamp(maxOffset, 1, 4096));
            std::uint64_t current = address;
            for (int guard = 0; guard < 4096; ++guard)
            {
                const auto previous = disassembler.PreviousStart(process, current);
                if (!previous || *previous >= current || address - *previous > reach)
                    break;
                const Instruction line = disassembler.Read(process, *previous);
                if (!line.Valid || line.Next() != current || IsTerminator(line) || line.IsNop)
                    break;
                starts.push_back(*previous);
                current = *previous;
            }
            const Instruction first = disassembler.Read(process, address);
            if (first.Valid && !IsTerminator(first))
            {
                current = first.Next();
                for (int guard = 0; guard < 4096 && current - address <= reach; ++guard)
                {
                    const Instruction line = disassembler.Read(process, current);
                    if (!line.Valid)
                        break;
                    starts.push_back(current);
                    if (IsTerminator(line))
                        break;
                    current = line.Next();
                }
            }
            return starts;
        }

        std::string Signed(std::int64_t value)
        {
            if (value == 0)
                return {};
            return value < 0 ? "-" + Hex(Magnitude(value), 2) : "+" + Hex(static_cast<std::uint64_t>(value), 2);
        }
    }

    std::size_t Signature::Wildcards() const
    {
        return static_cast<std::size_t>(std::count_if(Bytes.begin(), Bytes.end(), [](const SignatureByte& byte) { return byte.Wild; }));
    }

    Scan::Pattern Signature::Pattern() const
    {
        Scan::Pattern pattern;
        for (const SignatureByte& byte : Bytes)
        {
            pattern.Bytes.push_back(byte.Wild ? 0 : byte.Value);
            pattern.Mask.push_back(byte.Wild ? 0x00 : 0xFF);
        }
        return pattern;
    }

    std::string Signature::ScopeText() const
    {
        std::string text = Scope == SignatureScope::Module && !Module.empty() ? Module : "the whole process";
        return CodeOnly ? text + " (code only)" : text;
    }

    std::size_t UniqueLength(const Target::Process& process, const Scan::Pattern& pattern, std::uint64_t self, std::uint64_t low, std::uint64_t high, bool codeOnly,
        Scan::PatternProgress* progress)
    {
        if (pattern.Empty())
            return 1;
        Probe probe;
        probe.Bytes = pattern.Bytes;
        probe.Mask = pattern.Mask;
        probe.Self = self;
        probe.Cap = pattern.Bytes.size() + 1;
        Space space;
        space.Low = low;
        space.High = high;
        if (codeOnly)
            space.Filter.Executable = Scan::Tristate::Yes;
        Measure(process, space, { &probe }, progress, nullptr);
        return probe.Need;
    }

    std::optional<Signature> MakeSignature(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, const SignatureOptions& options,
        std::string& error, Scan::PatternProgress* progress)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto region = process.RegionAt(address);
        if (!region || !region->Committed() || !region->Readable())
        {
            error = Hex(address) + " cannot be read.";
            return std::nullopt;
        }
        Signature signature;
        signature.Address = address;
        signature.Code = region->Executable();
        signature.CodeOnly = options.CodeOnly && signature.Code;
        const auto module = process.ModuleAt(address);
        if (module)
        {
            signature.Module = module->Name;
            signature.ModuleBase = module->Base;
        }
        Space space;
        if (signature.CodeOnly)
            space.Filter.Executable = Scan::Tristate::Yes;
        signature.Scope = options.Scope;
        if (options.Range)
        {
            space.Low = options.Range->first;
            space.High = options.Range->second;
        }
        else if (options.Scope == SignatureScope::Module && module)
        {
            space.Low = module->Base;
            space.High = module->Base + module->Size;
        }
        else
        {
            signature.Scope = SignatureScope::Process;
            space.Low = process.LowestAddress();
            space.High = process.HighestAddress();
        }
        if (address < space.Low || address >= space.High)
        {
            error = Hex(address) + " lies outside the memory searched.";
            return std::nullopt;
        }

        const auto isAddress = [&process](std::uint64_t value) { return value >= 0x10000 && process.ModuleAt(value).has_value(); };
        std::vector<Built> builds;
        builds.push_back(Build(process, disassembler, address, options, signature.Code, isAddress));
        if (builds.front().Bytes.empty())
        {
            error = "No instruction could be decoded at " + Hex(address) + ".";
            return std::nullopt;
        }
        if (options.AllowOffset && signature.Code)
        {
            for (const std::uint64_t start : StartsAround(process, disassembler, address, options.MaxOffset))
            {
                Built built = Build(process, disassembler, start, options, true, isAddress);
                if (!built.Bytes.empty())
                    builds.push_back(std::move(built));
            }
        }

        std::vector<std::unique_ptr<Probe>> probes;
        for (const Built& built : builds)
            probes.push_back(MakeProbe(built));
        if (!Measure(process, space, { probes.front().get() }, progress, &signature.SearchedBytes))
        {
            error = "Cancelled.";
            return std::nullopt;
        }
        const std::size_t minimum = static_cast<std::size_t>(std::max(options.MinBytes, 0));
        const auto lengthOf = [&](std::size_t index) -> std::optional<std::size_t>
            {
                const std::size_t need = probes[index]->Need;
                const std::size_t available = probes[index]->Bytes.size();
                if (need > available)
                    return std::nullopt;
                return std::min(std::max(need, minimum), available);
            };
        const std::optional<std::size_t> direct = lengthOf(0);
        const std::size_t saving = static_cast<std::size_t>(std::max(options.MinSaving, 0));
        std::vector<bool> pruned(probes.size(), false);
        if (probes.size() > 1)
        {
            // An offset only pays when it saves MinSaving bytes, so a start stops
            // being measured as soon as it cannot get that short any more.
            std::vector<Probe*> others;
            for (std::size_t index = 1; index < probes.size(); ++index)
            {
                Probe& probe = *probes[index];
                if (direct)
                    probe.Cap = std::min(probe.Cap, *direct > saving ? *direct - saving + 1 : std::size_t(1));
                if (probe.Cap > 1)
                    others.push_back(&probe);
                else
                    pruned[index] = true;
            }
            if (!Measure(process, space, others, progress, &signature.SearchedBytes))
            {
                error = "Cancelled.";
                return std::nullopt;
            }
            for (std::size_t index = 1; index < probes.size(); ++index)
            {
                Probe& probe = *probes[index];
                if (pruned[index] || (probe.Cap <= probe.Bytes.size() && probe.Need >= probe.Cap))
                {
                    pruned[index] = true;
                    probe.Need = probe.Bytes.size() + 1;
                }
            }
        }

        signature.Candidates.push_back({ address, 0, direct.value_or(0), direct.has_value(), false, false });
        std::optional<std::size_t> bestOffset;
        std::size_t bestOffsetLength = 0;
        // Shorter wins; at the same length a pattern that holds the address itself
        // (it stops matching when that instruction changes), then the nearer start.
        const auto rank = [&](std::size_t which, std::size_t length)
            {
                const std::uint64_t start = builds[which].Start;
                const bool covers = start <= address && address < start + length;
                return std::make_tuple(length, covers ? 0 : 1, Magnitude(static_cast<std::int64_t>(address - start)));
            };
        for (std::size_t index = 1; index < probes.size(); ++index)
        {
            const auto length = lengthOf(index);
            signature.Candidates.push_back({ builds[index].Start, static_cast<std::int64_t>(address - builds[index].Start), length.value_or(0), length.has_value(), false,
                pruned[index] });
            if (length && (!bestOffset || rank(index, *length) < rank(*bestOffset, bestOffsetLength)))
            {
                bestOffset = index;
                bestOffsetLength = *length;
            }
        }
        std::optional<std::size_t> chosen;
        std::size_t chosenLength = 0;
        if (direct)
        {
            chosen = 0;
            chosenLength = *direct;
        }
        if (bestOffset && (!direct || bestOffsetLength + saving <= *direct))
        {
            chosen = bestOffset;
            chosenLength = bestOffsetLength;
        }
        std::stable_sort(signature.Candidates.begin() + 1, signature.Candidates.end(), [](const SignatureCandidate& a, const SignatureCandidate& b) { return a.Start < b.Start; });
        if (!chosen)
        {
            const std::size_t available = probes.front()->Bytes.size();
            error = "No unique signature within " + std::to_string(available) + " bytes: another place in " + signature.ScopeText() + " matches all of them" +
                (options.AllowOffset ? ", also from the instructions around it." : ". An AoB offset, more bytes or fewer wildcards may help.");
            return std::nullopt;
        }

        const Built& winner = builds[*chosen];
        signature.Start = winner.Start;
        signature.Offset = static_cast<std::int64_t>(address - winner.Start);
        signature.Bytes.assign(winner.Bytes.begin(), winner.Bytes.begin() + static_cast<std::ptrdiff_t>(chosenLength));
        while (signature.Bytes.size() > probes[*chosen]->Need && !signature.Bytes.empty() && signature.Bytes.back().Wild)
            signature.Bytes.pop_back();
        for (SignatureCandidate& candidate : signature.Candidates)
            candidate.Chosen = candidate.Start == winner.Start;
        const std::uint64_t end = winner.Start + signature.Bytes.size();
        for (const SignatureLine& line : winner.Lines)
        {
            if (line.Address < end)
                signature.Lines.push_back(line);
        }
        if (winner.FunctionEnd)
        {
            const auto last = std::find_if(winner.Lines.begin(), winner.Lines.end(), [&](const SignatureLine& line) { return line.Address == *winner.FunctionEnd; });
            if (last != winner.Lines.end() && last->Address + last->Length < end)
                signature.FunctionEnd = winner.FunctionEnd;
        }

        const std::vector<std::uint64_t> hits = Scan::FindPattern(process, signature.Pattern(), space.Low, space.High, space.Filter, 2);
        signature.Verified = hits.size() == 1 && hits.front() == signature.Start;
        signature.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return signature;
    }

    const char* WildcardToken(SignatureWildcard wildcard)
    {
        switch (wildcard)
        {
        case SignatureWildcard::Single: return "?";
        case SignatureWildcard::DoubleStar: return "**";
        case SignatureWildcard::Star: return "*";
        default: return "??";
        }
    }

    std::string SignatureText(const std::vector<SignatureByte>& bytes, const SignatureFormatting& formatting)
    {
        std::string wild = WildcardToken(formatting.Wildcard);
        if (!formatting.Spaces && wild.size() == 1)
            wild += wild;
        std::string text;
        text.reserve(bytes.size() * 3);
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            if (i && formatting.Spaces)
                text += ' ';
            text += bytes[i].Wild ? wild : Hex(bytes[i].Value, 2);
        }
        return text;
    }

    std::string FormatSignature(const Signature& signature, SignatureFormat format, const SignatureFormatting& formatting)
    {
        switch (format)
        {
        case SignatureFormat::CodeMask:
        {
            std::string code;
            std::string mask;
            for (const SignatureByte& byte : signature.Bytes)
            {
                code += "\\x" + Hex(byte.Wild ? 0 : byte.Value, 2);
                mask += byte.Wild ? '?' : 'x';
            }
            return "\"" + code + "\", \"" + mask + "\"";
        }
        case SignatureFormat::ByteArray:
        {
            std::string array = "{ ";
            std::string mask;
            for (std::size_t i = 0; i < signature.Bytes.size(); ++i)
            {
                array += (i ? ", 0x" : "0x") + Hex(signature.Bytes[i].Wild ? 0 : signature.Bytes[i].Value, 2);
                mask += signature.Bytes[i].Wild ? '?' : 'x';
            }
            return array + " }, \"" + mask + "\"";
        }
        case SignatureFormat::AutoAssembler:
        {
            SignatureFormatting aa = formatting;
            if (aa.Wildcard == SignatureWildcard::Single)
                aa.Wildcard = SignatureWildcard::Double;
            const std::string label = Trim(formatting.Label).empty() ? std::string("INJECT") : Trim(formatting.Label);
            std::string line = signature.Scope == SignatureScope::Module && !signature.Module.empty()
                ? "aobscanmodule(" + label + "," + signature.Module + "," + SignatureText(signature.Bytes, aa) + ")"
                : "aobscan(" + label + "," + SignatureText(signature.Bytes, aa) + ")";
            if (signature.Offset != 0)
                line += " // " + label + Signed(signature.Offset);
            return line;
        }
        default:
            return SignatureText(signature.Bytes, formatting);
        }
    }

    const char* SignatureFormatName(SignatureFormat format)
    {
        switch (format)
        {
        case SignatureFormat::CodeMask: return "Code + Mask";
        case SignatureFormat::ByteArray: return "C Array";
        case SignatureFormat::AutoAssembler: return "Auto Assembler";
        default: return "AoB";
        }
    }

    const char* ByteRoleName(ByteRole role)
    {
        switch (role)
        {
        case ByteRole::Prefix: return "prefix";
        case ByteRole::ModRm: return "ModRM/SIB";
        case ByteRole::Address: return "address";
        case ByteRole::Branch: return "branch distance";
        case ByteRole::Offset: return "offset";
        case ByteRole::Immediate: return "immediate";
        case ByteRole::Data: return "data";
        default: return "opcode";
        }
    }

    namespace
    {
        bool WildChar(char c)
        {
            return c == '?' || c == '*' || c == '.';
        }

        bool Ignored(std::string_view line)
        {
            const std::string trimmed = Trim(line);
            return StartsWithNoCase(trimmed, "Address of signature") || trimmed.starts_with("//") || trimmed.starts_with("--") || trimmed.starts_with("#") ||
                trimmed.starts_with(";");
        }

        int HexDigit(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        bool AddToken(std::string token, Scan::Pattern& pattern, std::string& error)
        {
            if (token.size() > 2 && (token[0] == '0') && (token[1] == 'x' || token[1] == 'X'))
                token = token.substr(2);
            else if (token.size() > 1 && (token.back() == 'h' || token.back() == 'H') && HexDigit(token[0]) >= 0)
                token.pop_back();
            if (token.empty())
                return true;
            if (std::all_of(token.begin(), token.end(), WildChar))
            {
                pattern.Bytes.push_back(0);
                pattern.Mask.push_back(0);
                return true;
            }
            if (token.size() == 1 && HexDigit(token[0]) >= 0)
            {
                pattern.Bytes.push_back(static_cast<std::uint8_t>(HexDigit(token[0])));
                pattern.Mask.push_back(0xFF);
                return true;
            }
            if (token.size() % 2 != 0)
            {
                error = "'" + token + "' has an odd number of hex digits.";
                return false;
            }
            for (std::size_t i = 0; i < token.size(); i += 2)
            {
                std::uint8_t value = 0;
                std::uint8_t mask = 0;
                for (const char c : { token[i], token[i + 1] })
                {
                    value = static_cast<std::uint8_t>(value << 4);
                    mask = static_cast<std::uint8_t>(mask << 4);
                    if (WildChar(c))
                        continue;
                    const int digit = HexDigit(c);
                    if (digit < 0)
                    {
                        error = "'" + token + "' is not a byte.";
                        return false;
                    }
                    value = static_cast<std::uint8_t>(value | digit);
                    mask = static_cast<std::uint8_t>(mask | 0xF);
                }
                pattern.Bytes.push_back(value);
                pattern.Mask.push_back(mask);
            }
            return true;
        }

        std::vector<std::uint8_t> CString(std::string_view text)
        {
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i + 3 < text.size(); ++i)
            {
                if (text[i] != '\\' || (text[i + 1] != 'x' && text[i + 1] != 'X'))
                    continue;
                const int high = HexDigit(text[i + 2]);
                const int low = HexDigit(text[i + 3]);
                if (high >= 0 && low >= 0)
                {
                    bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
                    i += 3;
                }
            }
            return bytes;
        }
    }

    std::optional<ParsedSignature> ParseSignature(std::string_view input, std::string& error)
    {
        ParsedSignature parsed;
        std::string body;
        std::size_t start = 0;
        while (start <= input.size())
        {
            std::size_t end = input.find_first_of("\r\n", start);
            if (end == std::string_view::npos)
                end = input.size();
            const std::string_view line = input.substr(start, end - start);
            if (!Ignored(line))
            {
                std::string kept(line);
                const std::size_t comment = kept.find("//");
                if (comment != std::string::npos)
                    kept.resize(comment);
                body += kept;
                body += '\n';
            }
            start = end + 1;
        }
        if (Trim(body).empty())
        {
            error = Trim(input).empty() ? "Nothing was given." : "Nothing in there reads as a signature.";
            return std::nullopt;
        }

        std::string lowered = Lower(body);
        const std::size_t aob = lowered.find("aobscan");
        if (aob != std::string::npos)
        {
            const std::size_t open = body.find('(', aob);
            const std::size_t close = open == std::string::npos ? std::string::npos : body.find(')', open);
            if (open != std::string::npos && close != std::string::npos)
            {
                const std::string command = Lower(Trim(std::string_view(body).substr(aob, open - aob)));
                std::vector<std::string> arguments;
                std::size_t from = open + 1;
                while (from <= close)
                {
                    std::size_t comma = body.find(',', from);
                    if (comma == std::string::npos || comma > close)
                        comma = close;
                    arguments.push_back(Trim(std::string_view(body).substr(from, comma - from)));
                    from = comma + 1;
                }
                if (command == "aobscanmodule" && arguments.size() >= 3)
                    parsed.Module = arguments[1];
                if (arguments.size() >= 2)
                {
                    parsed.Source = command;
                    body = arguments.back();
                    if (command == "aobscanregion" && arguments.size() >= 4)
                        body = arguments[3];
                }
            }
        }

        Scan::Pattern& pattern = parsed.Pattern;
        const std::size_t quote = body.find('"');
        const std::vector<std::uint8_t> code = CString(body);
        if (!code.empty())
        {
            parsed.Source = "code";
            std::string mask;
            const std::size_t closing = quote == std::string::npos ? std::string::npos : body.find('"', quote + 1);
            const std::size_t maskOpen = closing == std::string::npos ? std::string::npos : body.find('"', closing + 1);
            const std::size_t maskClose = maskOpen == std::string::npos ? std::string::npos : body.find('"', maskOpen + 1);
            if (maskClose != std::string::npos)
                mask = body.substr(maskOpen + 1, maskClose - maskOpen - 1);
            if (!mask.empty())
            {
                if (mask.find_first_not_of("xX?.*") != std::string::npos)
                {
                    error = "The mask may only be made of x and ?.";
                    return std::nullopt;
                }
                if (mask.size() != code.size())
                {
                    error = "The byte string is " + std::to_string(code.size()) + " bytes long and the mask " + std::to_string(mask.size()) + " characters.";
                    return std::nullopt;
                }
            }
            std::size_t zeros = 0;
            for (std::size_t i = 0; i < code.size(); ++i)
            {
                const bool wild = !mask.empty() && WildChar(mask[i]);
                pattern.Bytes.push_back(wild ? 0 : code[i]);
                pattern.Mask.push_back(wild ? 0 : 0xFF);
                zeros += mask.empty() && code[i] == 0 ? 1 : 0;
            }
            if (zeros)
                parsed.Notes.push_back("The byte string came without a mask, so its " + std::to_string(zeros) + " zero bytes are real zeros, not wildcards.");
        }
        else
        {
            if (parsed.Source.empty())
                parsed.Source = "aob";
            std::string token;
            const auto flush = [&]() -> bool
                {
                    const bool ok = AddToken(token, pattern, error);
                    token.clear();
                    return ok;
                };
            const std::string_view separators = " \t\r\n,;{}[]()\"'";
            for (const char c : body)
            {
                if (separators.find(c) != std::string_view::npos)
                {
                    if (!flush())
                        return std::nullopt;
                }
                else
                    token += c;
            }
            if (!flush())
                return std::nullopt;
        }
        if (pattern.Empty())
        {
            error = "Nothing in there reads as a signature.";
            return std::nullopt;
        }
        for (const std::uint8_t mask : pattern.Mask)
        {
            if (mask == 0xFF)
                ++parsed.Fixed;
            else if (mask == 0)
                ++parsed.Wildcards;
            else
                ++parsed.Partial;
        }
        if (parsed.Fixed == 0 && parsed.Partial == 0)
        {
            error = "The pattern is nothing but wildcards.";
            return std::nullopt;
        }
        if (pattern.Mask.front() != 0xFF)
            parsed.Notes.push_back("The pattern starts with a wildcard, so a hit points at the wildcard, not at the first known byte.");
        for (std::size_t i = 0; i < pattern.Bytes.size(); ++i)
        {
            if (i)
                parsed.Text += ' ';
            const std::uint8_t mask = pattern.Mask[i];
            const std::uint8_t value = pattern.Bytes[i];
            if (mask == 0xFF)
                parsed.Text += Hex(value, 2);
            else if (mask == 0)
                parsed.Text += "??";
            else
            {
                const char digits[] = "0123456789ABCDEF";
                parsed.Text += (mask & 0xF0) ? digits[value >> 4] : '?';
                parsed.Text += (mask & 0x0F) ? digits[value & 0xF] : '?';
            }
        }
        return parsed;
    }
}
