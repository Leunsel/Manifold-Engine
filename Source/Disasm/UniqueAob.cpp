#include "Disasm/UniqueAob.h"

#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"

#include <algorithm>

namespace Manifold::Disasm
{
    namespace
    {
        constexpr std::size_t kHitLimit = 64;
        constexpr std::size_t kMinimumFixed = 4;

        struct Candidate
        {
            std::vector<std::uint8_t> Bytes;
            std::vector<std::uint8_t> Mask;
            std::vector<std::size_t> Ends;
        };

        Candidate Build(const Target::Process& process, const Disassembler& disassembler, std::uint64_t start, std::size_t maxLength, bool code)
        {
            Candidate candidate;
            std::vector<std::uint8_t> buffer(maxLength + 16);
            buffer.resize(process.Read(start, buffer.data(), buffer.size()));
            const auto isAddress = [&](std::uint64_t value) { return value >= 0x10000 && process.ModuleAt(value).has_value(); };
            std::size_t offset = 0;
            while (code && offset < maxLength && offset < buffer.size())
            {
                const Instruction line = disassembler.Decode(start + offset, buffer.data() + offset, buffer.size() - offset);
                const std::size_t length = line.Length;
                if (!line.Valid || length == 0 || offset + length > buffer.size())
                    break;
                const std::uint16_t loose = disassembler.AddressBytes(line, isAddress);
                for (std::size_t i = 0; i < length; ++i)
                {
                    candidate.Bytes.push_back(buffer[offset + i]);
                    candidate.Mask.push_back(i < 16 && (loose & (1u << i)) ? 0x00 : 0xFF);
                }
                offset += length;
                candidate.Ends.push_back(offset);
            }
            if (candidate.Ends.empty())
            {
                const std::size_t usable = std::min(buffer.size(), maxLength);
                candidate.Bytes.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(usable));
                candidate.Mask.assign(usable, 0xFF);
                for (std::size_t end = 4; end <= usable; end += 4)
                    candidate.Ends.push_back(end);
            }
            return candidate;
        }

        std::size_t Fixed(const Candidate& candidate, std::size_t length)
        {
            return static_cast<std::size_t>(std::count(candidate.Mask.begin(), candidate.Mask.begin() + static_cast<std::ptrdiff_t>(length), std::uint8_t(0xFF)));
        }

        std::optional<std::size_t> UniqueLength(const Target::Process& process, const Candidate& candidate, std::uint64_t start, std::uint64_t low, std::uint64_t high,
            const Scan::PatternFilter& filter)
        {
            if (candidate.Ends.empty() || candidate.Mask.front() != 0xFF)
                return std::nullopt;
            const std::size_t total = candidate.Ends.back();
            for (const std::size_t wanted : { std::size_t(8), std::size_t(16), std::size_t(32), total })
            {
                const auto end = std::find_if(candidate.Ends.begin(), candidate.Ends.end(), [&](std::size_t each) { return each >= wanted; });
                const std::size_t seed = end == candidate.Ends.end() ? total : *end;
                if (Fixed(candidate, seed) < kMinimumFixed)
                {
                    if (seed == total)
                        return std::nullopt;
                    continue;
                }
                Scan::Pattern search;
                search.Bytes.assign(candidate.Bytes.begin(), candidate.Bytes.begin() + static_cast<std::ptrdiff_t>(seed));
                search.Mask.assign(candidate.Mask.begin(), candidate.Mask.begin() + static_cast<std::ptrdiff_t>(seed));
                const std::vector<std::uint64_t> hits = Scan::FindPattern(process, search, low, high, filter, kHitLimit);
                if (hits.size() >= kHitLimit)
                {
                    if (seed == total)
                        return std::nullopt;
                    continue;
                }
                if (std::find(hits.begin(), hits.end(), start) == hits.end())
                    return std::nullopt;
                std::size_t needed = seed;
                std::vector<std::uint8_t> other(total);
                for (const std::uint64_t hit : hits)
                {
                    if (hit == start)
                        continue;
                    const std::size_t got = process.Read(hit, other.data(), other.size());
                    std::size_t same = 0;
                    while (same < got && same < total && (candidate.Mask[same] == 0 || other[same] == candidate.Bytes[same]))
                        ++same;
                    if (same >= total)
                        return std::nullopt;
                    needed = std::max(needed, same + 1);
                }
                const auto fit = std::find_if(candidate.Ends.begin(), candidate.Ends.end(), [&](std::size_t each) { return each >= needed; });
                if (fit == candidate.Ends.end())
                    return std::nullopt;
                return *fit;
            }
            return std::nullopt;
        }
    }

    std::optional<UniqueAob> FindUniqueAob(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, const UniqueAobOptions& options)
    {
        const auto region = process.RegionAt(address);
        if (!region || !region->Committed() || !region->Readable())
            return std::nullopt;
        const bool code = region->Executable();
        Scan::PatternFilter filter;
        if (code)
            filter.Executable = Scan::Tristate::Yes;
        std::uint64_t low = process.LowestAddress();
        std::uint64_t high = process.HighestAddress();
        if (!options.WholeProcess)
        {
            if (const auto module = process.ModuleAt(address))
            {
                low = module->Base;
                high = module->Base + module->Size;
            }
        }
        const std::size_t maxLength = std::clamp<std::size_t>(options.MaxLength, 8, 256);
        std::uint64_t start = address;
        for (int step = 0; step <= options.BackSteps; ++step)
        {
            const Candidate candidate = Build(process, disassembler, start, maxLength, code);
            if (const auto length = UniqueLength(process, candidate, start, low, high, filter))
            {
                std::size_t kept = *length;
                while (kept > 1 && candidate.Mask[kept - 1] == 0)
                    --kept;
                UniqueAob found;
                found.Start = start;
                found.Offset = static_cast<int>(address - start);
                found.Bytes.assign(candidate.Bytes.begin(), candidate.Bytes.begin() + static_cast<std::ptrdiff_t>(kept));
                found.Mask.assign(candidate.Mask.begin(), candidate.Mask.begin() + static_cast<std::ptrdiff_t>(kept));
                for (std::size_t i = 0; i < kept; ++i)
                    found.Text += (i ? " " : "") + (found.Mask[i] ? Hex(found.Bytes[i], 2) : std::string("??"));
                return found;
            }
            if (!code)
                break;
            const auto previous = disassembler.PreviousStart(process, start);
            if (!previous || *previous >= start || start - *previous > 15 || address - *previous > 128)
                break;
            start = *previous;
        }
        return std::nullopt;
    }
}
