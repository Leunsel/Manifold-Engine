#include "Scan/StaticPointers.h"

#include "Target/Process.h"

#include <algorithm>
#include <cstring>

namespace Manifold::Scan
{
    void MatchPointers(std::span<const std::uint8_t> bytes, std::uint64_t base, int pointerSize, std::uint64_t low, std::uint64_t high, std::vector<std::uint64_t>& out)
    {
        const std::size_t step = static_cast<std::size_t>(pointerSize);
        std::size_t at = static_cast<std::size_t>((step - base % step) % step);
        if (pointerSize == 8)
        {
            for (; at + 8 <= bytes.size(); at += 8)
            {
                std::uint64_t value;
                std::memcpy(&value, bytes.data() + at, 8);
                if (value >= low && value <= high)
                    out.push_back(base + at);
            }
            return;
        }
        for (; at + 4 <= bytes.size(); at += 4)
        {
            std::uint32_t value;
            std::memcpy(&value, bytes.data() + at, 4);
            if (value >= low && value <= high)
                out.push_back(base + at);
        }
    }

    std::vector<StaticPointer> FindStaticPointers(const Target::Process& process, const StaticPointerSearch& search, PatternProgress* progress, bool* truncated)
    {
        std::vector<StaticPointer> found;
        if (truncated)
            *truncated = false;
        const std::uint64_t low = search.Target > search.MaxOffset ? search.Target - search.MaxOffset : 0x10000;
        const std::uint64_t high = search.Target;
        const int pointerSize = process.PointerSize();
        const Target::ModuleList modules = process.Modules();
        if (!modules || high < 0x10000)
            return found;

        struct Span
        {
            const Target::ModuleInfo* Module;
            std::uint64_t Base;
            std::uint64_t Size;
        };
        std::vector<Span> spans;
        std::uint64_t total = 0;
        for (const Target::ModuleInfo& module : *modules)
        {
            if (module.System && !search.SystemModules)
                continue;
            for (const Target::RegionInfo& region : process.Regions(module.Base, module.Base + module.Size))
            {
                if (!region.Committed() || !region.Writable() || region.Guarded())
                    continue;
                const std::uint64_t start = std::max(region.Base, module.Base);
                const std::uint64_t stop = std::min(region.End(), module.Base + module.Size);
                if (stop > start)
                {
                    spans.push_back({ &module, start, stop - start });
                    total += stop - start;
                }
            }
        }
        if (progress)
        {
            progress->Total = std::max<std::uint64_t>(1, total);
            progress->Done = 0;
        }

        constexpr std::size_t kChunk = 1 << 20;
        std::vector<std::uint8_t> buffer;
        std::vector<std::uint64_t> addresses;
        for (const Span& span : spans)
        {
            for (std::uint64_t offset = 0; offset < span.Size; offset += kChunk)
            {
                if (progress && progress->Stopped())
                    return found;
                const std::size_t size = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, span.Size - offset));
                buffer.resize(size);
                const std::uint64_t at = span.Base + offset;
                const std::size_t read = process.Read(at, buffer.data(), size);
                addresses.clear();
                MatchPointers(std::span<const std::uint8_t>(buffer.data(), read), at, pointerSize, low, high, addresses);
                for (const std::uint64_t address : addresses)
                {
                    StaticPointer pointer;
                    pointer.Address = address;
                    std::memcpy(&pointer.Value, buffer.data() + (address - at), static_cast<std::size_t>(pointerSize));
                    pointer.Module = span.Module->Name;
                    pointer.ModuleBase = span.Module->Base;
                    found.push_back(std::move(pointer));
                    if (found.size() >= search.Limit)
                    {
                        if (truncated)
                            *truncated = true;
                        break;
                    }
                }
                if (progress)
                    progress->Done += size;
                if (found.size() >= search.Limit)
                    break;
            }
            if (found.size() >= search.Limit)
                break;
        }
        std::stable_sort(found.begin(), found.end(), [&](const StaticPointer& a, const StaticPointer& b) { return a.Offset(search.Target) < b.Offset(search.Target); });
        return found;
    }
}
