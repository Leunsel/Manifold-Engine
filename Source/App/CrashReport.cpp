#include "App/CrashReport.h"

#include "Core/Text.h"
#include "Disasm/Disassembler.h"

#include <algorithm>

namespace Manifold
{
    namespace
    {
        constexpr std::uint64_t kLookBackMs = 30000;
        constexpr std::uint64_t kLateMs = 2000;
        constexpr std::uint64_t kQuietExitMs = 10000;
        constexpr std::uint64_t kEarlierMs = 2000;
        constexpr std::uint64_t kBehindPatch = 16;
        constexpr std::uint64_t kNearPatch = 0x800;
        constexpr std::size_t kStackShown = 12;

        bool Contains(const ScriptRange& range, std::uint64_t address)
        {
            return address >= range.Begin && address < range.End;
        }

        bool OnAt(const ScriptFootprint& footprint, std::uint64_t tick)
        {
            return footprint.OnTick <= tick && (footprint.OffTick == 0 || footprint.OffTick > tick);
        }

        struct Hit
        {
            std::size_t Footprint = 0;
            const ScriptRange* Range = nullptr;
        };

        // ∑ Freed memory can come back as another script's allocation: the
        //   script that was on at the moment wins, then the one switched off
        //   last before it.
        std::optional<Hit> Find(const std::vector<ScriptFootprint>& footprints, std::uint64_t address, std::uint64_t tick, std::optional<ScriptRangeKind> kind = std::nullopt)
        {
            std::optional<Hit> best;
            std::uint64_t bestOff = 0;
            for (std::size_t i = 0; i < footprints.size(); ++i)
            {
                for (const ScriptRange& range : footprints[i].Ranges)
                {
                    if (!Contains(range, address) || (kind && range.Kind != *kind))
                        continue;
                    if (OnAt(footprints[i], tick))
                        return Hit{ i, &range };
                    if (footprints[i].OffTick && footprints[i].OffTick <= tick && footprints[i].OffTick >= bestOff)
                    {
                        best = Hit{ i, &range };
                        bestOff = footprints[i].OffTick;
                    }
                }
            }
            return best;
        }

        std::optional<Hit> FindBehindPatch(const std::vector<ScriptFootprint>& footprints, std::uint64_t address, std::uint64_t tick)
        {
            for (std::size_t i = 0; i < footprints.size(); ++i)
            {
                if (!OnAt(footprints[i], tick))
                    continue;
                for (const ScriptRange& range : footprints[i].Ranges)
                {
                    if (range.Kind == ScriptRangeKind::Patch && address >= range.End && address < range.End + kBehindPatch)
                        return Hit{ i, &range };
                }
            }
            return std::nullopt;
        }

        std::string Offset(const std::string& name, std::uint64_t base, std::uint64_t address)
        {
            return address == base ? name : name + Format("+%llX", static_cast<unsigned long long>(address - base));
        }

        std::string WhereIn(const ScriptFootprint& footprint, const ScriptRange& range, std::uint64_t address, const CrashContext& context)
        {
            auto label = footprint.Labels.upper_bound(address);
            if (label != footprint.Labels.begin())
            {
                --label;
                if (Contains(range, label->first))
                    return Offset(label->second, label->first, address);
            }
            if (range.Kind == ScriptRangeKind::Allocation && !range.Name.empty())
                return Offset(range.Name, range.Begin, address);
            return context.Describe ? context.Describe(address) : Hex(address);
        }

        std::string Describe(const CrashContext& context, std::uint64_t address, std::uint64_t tick)
        {
            if (const auto hit = Find(context.Footprints, address, tick))
                return "\"" + context.Footprints[hit->Footprint].Label + "\" " + WhereIn(context.Footprints[hit->Footprint], *hit->Range, address, context);
            return context.Describe ? context.Describe(address) : Hex(address);
        }

        std::string Seconds(double seconds)
        {
            return seconds < 1.0 ? std::string("less than a second") : seconds < 1.5 ? std::string("1 second") : Format("%.0f seconds", seconds);
        }

        const char* const kNames64[] = { "rip", "rsp", "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
        const char* const kNames32[] = { "eip", "esp", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp" };
    }

    std::optional<CrashBlame> ScriptAt(const std::vector<ScriptFootprint>& footprints, std::uint64_t address, std::uint64_t tick,
        const std::function<std::string(std::uint64_t)>& describe)
    {
        const auto hit = Find(footprints, address, tick);
        if (!hit)
            return std::nullopt;
        CrashContext context;
        context.Describe = describe;
        const ScriptFootprint& footprint = footprints[hit->Footprint];
        CrashBlame blame;
        blame.Label = footprint.Label;
        blame.RecordId = footprint.RecordId;
        blame.Link = !OnAt(footprint, tick) ? CrashLink::RanFreed : hit->Range->Kind == ScriptRangeKind::Patch ? CrashLink::InPatch : CrashLink::InCode;
        blame.Address = address;
        blame.Where = WhereIn(footprint, *hit->Range, address, context);
        return blame;
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ScriptRegions(const std::vector<ScriptFootprint>& footprints, std::size_t limit)
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        for (const ScriptFootprint& footprint : footprints)
        {
            if (footprint.OffTick != 0)
                continue;
            for (const ScriptRange& range : footprint.Ranges)
                ranges.emplace_back(range.Begin, range.End);
        }
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
        for (const auto& range : ranges)
        {
            if (!merged.empty() && range.first <= merged.back().second)
                merged.back().second = std::max(merged.back().second, range.second);
            else
                merged.push_back(range);
        }
        // ∑ Over the limit the closest neighbours join, so every range stays covered.
        while (merged.size() > limit && merged.size() > 1)
        {
            std::size_t best = 0;
            for (std::size_t i = 1; i + 1 < merged.size(); ++i)
            {
                if (merged[i + 1].first - merged[i].second < merged[best + 1].first - merged[best].second)
                    best = i;
            }
            merged[best].second = merged[best + 1].second;
            merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(best + 1));
        }
        return merged;
    }

    void CrashLedger::Reset()
    {
        std::lock_guard lock(m_lock);
        m_footprints.clear();
    }

    std::string ScriptLabel(std::string_view title, std::string_view owner)
    {
        if (const std::size_t from = title.find(" from \""); from != std::string_view::npos && title.size() > from + 8 && title.back() == '"')
            return std::string(title.substr(from + 7, title.size() - from - 8));
        if (title.size() >= 2 && title.front() == '"' && title.back() == '"')
            return std::string(title.substr(1, title.size() - 2));
        if (!title.empty())
            return std::string(title);
        if (owner == "window")
            return "Auto Assembler window";
        if (owner == "lua" || owner == "scratch")
            return "autoAssemble";
        return owner.empty() ? std::string("Auto Assembler") : std::string(owner);
    }

    void CrashLedger::Note(std::string label, int recordId, std::uint64_t key, Asm::AaSection section, const Asm::AaResult& result, const Asm::AaMemory& before,
        const Asm::AaMemory& after, std::uint64_t tick)
    {
        if (!result.Ok())
            return;
        std::lock_guard lock(m_lock);
        const auto current = [&]
        {
            return std::find_if(m_footprints.rbegin(), m_footprints.rend(), [&](const ScriptFootprint& footprint)
                {
                    return footprint.OffTick == 0 && footprint.Label == label && footprint.RecordId == recordId && footprint.Key == key;
                });
        };
        // ∑ A run that writes over the patched bytes of a script that is on switches that
        //   script off, whatever it is called: Lua's autoAssemble(enable) / autoAssemble(disable)
        //   pair has two texts (two keys), a released orphan has no record, a script may be
        //   edited while it is on. Such writes are the original bytes going back, so they make
        //   no footprint of their own either (a crash there later is not this run's doing).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> restored;
        {
            const auto self = current();
            const ScriptFootprint* keep = self != m_footprints.rend() ? &*self : nullptr;
            for (ScriptFootprint& other : m_footprints)
            {
                if (other.OffTick != 0 || &other == keep)
                    continue;
                bool overwritten = false;
                for (const ScriptRange& range : other.Ranges)
                {
                    if (range.Kind != ScriptRangeKind::Patch)
                        continue;
                    for (const Asm::AaWrite& write : result.Writes)
                    {
                        if (!write.Bytes.empty() && write.Address < range.End && write.Address + write.Bytes.size() > range.Begin)
                        {
                            overwritten = true;
                            restored.emplace_back(range.Begin, range.End);
                        }
                    }
                }
                if (overwritten)
                    other.OffTick = tick;
            }
        }
        if (section == Asm::AaSection::Disable)
        {
            if (const auto it = current(); it != m_footprints.rend())
                it->OffTick = tick;
            return;
        }
        ScriptFootprint footprint;
        footprint.Label = label;
        footprint.RecordId = recordId;
        footprint.Key = key;
        footprint.OnTick = tick;
        footprint.Warnings = result.Warnings;
        // ∑ Every db/dw/dd/dq line is a span of its own, so dd 0 and dd (float)3 stay two rows.
        //   Behind the last byte the script wrote into an allocation the rest is empty: data
        //   while it reads zero (Lua may write code there later, which is decoded again).
        for (const auto& [begin, end] : result.DataRanges)
            footprint.Data.push_back({ begin, end, false });
        for (const Asm::AaAllocation& allocation : after.Allocations)
        {
            if (!allocation.Address || !allocation.Size)
                continue;
            const std::uint64_t allocationEnd = allocation.Address + allocation.Size;
            std::uint64_t written = allocation.Address;
            for (const Asm::AaWrite& write : result.Writes)
            {
                if (write.Address >= allocation.Address && write.Address < allocationEnd)
                    written = std::max(written, std::min(allocationEnd, write.Address + write.Bytes.size()));
            }
            if (written < allocationEnd)
                footprint.Data.push_back({ written, allocationEnd, true });
        }
        std::sort(footprint.Data.begin(), footprint.Data.end(), [](const ScriptData& a, const ScriptData& b) { return a.Begin < b.Begin; });
        for (const Asm::AaAllocation& allocation : after.Allocations)
        {
            if (allocation.Address && allocation.Size)
                footprint.Ranges.push_back({ allocation.Address, allocation.Address + allocation.Size, ScriptRangeKind::Allocation, allocation.Name });
        }
        (void)before;
        const auto inAllocation = [&](std::uint64_t address)
        {
            return std::any_of(footprint.Ranges.begin(), footprint.Ranges.end(), [&](const ScriptRange& range) { return range.Kind == ScriptRangeKind::Allocation && Contains(range, address); });
        };
        std::vector<ScriptRange> patches;
        for (const Asm::AaWrite& write : result.Writes)
        {
            if (write.Bytes.empty() || inAllocation(write.Address))
                continue;
            const std::uint64_t end = write.Address + write.Bytes.size();
            // the bytes of a script it switched off going back; a run with memory of its own is
            // another script hooking the same place, and those bytes are its patch now
            if (after.Allocations.empty() && std::any_of(restored.begin(), restored.end(), [&](const auto& range) { return write.Address < range.second && end > range.first; }))
                continue;
            patches.push_back({ write.Address, end, ScriptRangeKind::Patch, {} });
        }
        std::sort(patches.begin(), patches.end(), [](const ScriptRange& a, const ScriptRange& b) { return a.Begin < b.Begin; });
        for (const ScriptRange& patch : patches)
        {
            if (!footprint.Ranges.empty() && footprint.Ranges.back().Kind == ScriptRangeKind::Patch && footprint.Ranges.back().End >= patch.Begin)
                footprint.Ranges.back().End = std::max(footprint.Ranges.back().End, patch.End);
            else
                footprint.Ranges.push_back(patch);
        }
        if (footprint.Ranges.empty())
            return;
        for (const auto* names : { &result.Symbols, &result.Labels })
        {
            for (const auto& [name, address] : *names)
            {
                if (inAllocation(address))
                    footprint.Labels[address] = name;
            }
        }
        if (const auto it = current(); it != m_footprints.rend())
            *it = std::move(footprint);
        else
            m_footprints.push_back(std::move(footprint));
        while (m_footprints.size() > kMaxFootprints)
        {
            const auto off = std::find_if(m_footprints.begin(), m_footprints.end(), [](const ScriptFootprint& old) { return old.OffTick != 0; });
            m_footprints.erase(off != m_footprints.end() ? off : m_footprints.begin());
        }
    }

    std::vector<ScriptFootprint> CrashLedger::Footprints() const
    {
        std::lock_guard lock(m_lock);
        return m_footprints;
    }

    std::optional<ScriptData> CrashLedger::DataAt(std::uint64_t address) const
    {
        std::lock_guard lock(m_lock);
        for (auto it = m_footprints.rbegin(); it != m_footprints.rend(); ++it)
        {
            if (it->OffTick != 0)
                continue;
            auto span = std::upper_bound(it->Data.begin(), it->Data.end(), address, [](std::uint64_t at, const ScriptData& data) { return at < data.Begin; });
            if (span == it->Data.begin())
                continue;
            --span;
            if (address < span->End)
                return *span;
        }
        return std::nullopt;
    }

    const char* CrashLinkText(CrashLink link)
    {
        switch (link)
        {
        case CrashLink::InCode: return "in its code";
        case CrashLink::InPatch: return "in bytes it patched";
        case CrashLink::BehindPatch: return "right behind its patch";
        case CrashLink::RanFreed: return "in its code after it was switched off";
        case CrashLink::Unbalanced: return "its hook leaves the stack unbalanced";
        case CrashLink::NearPatch: return "shortly behind its patch";
        case CrashLink::Accessed: return "accessing its memory";
        case CrashLink::OnStack: return "called from its code";
        case CrashLink::Earlier: return "an exception in its code shortly before";
        }
        return "";
    }

    bool ExceptionExitCode(std::uint32_t code)
    {
        return (code & 0xF0000000u) == 0xC0000000u || code == 0x80000003u || code == 0x80000002u;
    }

    std::string ExceptionName(std::uint32_t code)
    {
        switch (code)
        {
        case 0xC0000005u: return "access violation";
        case 0xC000001Du: return "illegal instruction";
        case 0xC0000096u: return "privileged instruction";
        case 0xC0000094u: return "integer division by zero";
        case 0xC0000095u: return "integer overflow";
        case 0xC00000FDu: return "stack overflow";
        case 0xC000008Cu: return "array bounds exceeded";
        case 0xC0000006u: return "in-page error";
        case 0xC0000025u: return "noncontinuable exception";
        case 0xC0000026u: return "invalid disposition";
        case 0x80000002u: return "misaligned data";
        case 0x80000003u: return "breakpoint";
        case 0xC000008Eu: return "float division by zero";
        case 0xC0000090u: return "invalid float operation";
        case 0xC0000091u: return "float overflow";
        case 0xC0000092u: return "float stack check";
        case 0xC0000374u: return "heap corruption";
        case 0xC0000409u: return "stack buffer overrun (fail fast)";
        case 0xC00002B4u: return "float faults";
        case 0xC00002B5u: return "float traps";
        default: return Format("exception %08X", code);
        }
    }

    std::string ExceptionText(const Target::CrashRecord& record, bool is64Bit)
    {
        std::string text = ExceptionName(record.Code);
        if (record.Code == 0xC0000005u && record.Info.size() >= 2)
        {
            const char* access = record.Info[0] == 1 ? "writing" : record.Info[0] == 8 ? "executing" : "reading";
            text += std::string(" ") + access + " " + (is64Bit ? Format("%016llX", static_cast<unsigned long long>(record.Info[1])) : Format("%08llX", static_cast<unsigned long long>(record.Info[1])));
        }
        return text;
    }

    CrashReport AttributeCrash(const Target::CrashCapture& capture, const CrashContext& context)
    {
        CrashReport report;
        report.ExitCode = context.ExitCode;
        report.Process = context.Process;
        report.Watched = capture.Watching || !capture.Records.empty();
        const bool exceptionExit = ExceptionExitCode(context.ExitCode);
        const std::string who = context.Process.empty() ? std::string("The game") : context.Process;

        std::vector<const Target::CrashRecord*> recent;
        for (const Target::CrashRecord& record : capture.Records)
        {
            if (!context.EndTick || (record.Tick + kLookBackMs >= context.EndTick && record.Tick <= context.EndTick + kLateMs))
                recent.push_back(&record);
        }
        const Target::CrashRecord* fatal = nullptr;
        for (auto it = recent.rbegin(); it != recent.rend() && exceptionExit; ++it)
        {
            if ((*it)->Code == context.ExitCode)
            {
                fatal = *it;
                break;
            }
        }
        if (!fatal && !recent.empty())
            fatal = recent.back();
        report.Exact = fatal && fatal->Code == context.ExitCode;
        const std::uint64_t at = fatal ? fatal->Tick : context.EndTick;

        std::map<std::size_t, CrashBlame> blames;
        const auto blame = [&](std::size_t index, CrashLink link, std::uint64_t address, std::string where, int weight)
            {
                CrashBlame& slot = blames[index];
                if (slot.Weight >= weight)
                    return;
                slot.Label = context.Footprints[index].Label;
                slot.RecordId = context.Footprints[index].RecordId;
                slot.Link = link;
                slot.Address = address;
                slot.Where = std::move(where);
                slot.Weight = weight;
            };
        if (fatal)
        {
            const std::uint64_t ip = fatal->Ip();
            if (const auto hit = Find(context.Footprints, ip, at))
            {
                const ScriptFootprint& footprint = context.Footprints[hit->Footprint];
                const bool freed = !OnAt(footprint, at);
                const CrashLink link = freed ? CrashLink::RanFreed : hit->Range->Kind == ScriptRangeKind::Patch ? CrashLink::InPatch : CrashLink::InCode;
                blame(hit->Footprint, link, ip, WhereIn(footprint, *hit->Range, ip, context), freed ? 90 : link == CrashLink::InCode ? 100 : 95);
            }
            else if (const auto behind = FindBehindPatch(context.Footprints, ip, at))
            {
                blame(behind->Footprint, CrashLink::BehindPatch, ip, (context.Describe ? context.Describe(ip) : Hex(ip)), 70);
            }
            if (fatal->Code == 0xC0000005u && fatal->Info.size() >= 2)
            {
                if (const auto hit = Find(context.Footprints, fatal->Info[1], at, ScriptRangeKind::Allocation))
                    blame(hit->Footprint, CrashLink::Accessed, fatal->Info[1], WhereIn(context.Footprints[hit->Footprint], *hit->Range, fatal->Info[1], context), 50);
            }
            for (std::size_t index = 0; index < context.Footprints.size(); ++index)
            {
                const ScriptFootprint& footprint = context.Footprints[index];
                if (!OnAt(footprint, at))
                    continue;
                for (const Asm::AaWarning& warning : footprint.Warnings)
                {
                    if (warning.Text.find("stack") != std::string::npos)
                    {
                        blame(index, CrashLink::Unbalanced, ip, warning.Text, 45);  // below an access into another script's memory (50): that is direct evidence
                        break;
                    }
                }
                for (const ScriptRange& range : footprint.Ranges)
                {
                    if (range.Kind == ScriptRangeKind::Patch && ip >= range.End + kBehindPatch && ip < range.End + kNearPatch)
                    {
                        blame(index, CrashLink::NearPatch, ip, Format("%llX bytes behind its patch at %s", static_cast<unsigned long long>(ip - range.Begin),
                            (context.Describe ? context.Describe(range.Begin) : Hex(range.Begin)).c_str()), 35);
                        break;
                    }
                }
            }
            for (std::size_t slot = 0; slot < fatal->Stack.size(); ++slot)
            {
                if (const auto hit = Find(context.Footprints, fatal->Stack[slot], at, ScriptRangeKind::Allocation))
                    blame(hit->Footprint, CrashLink::OnStack, fatal->Stack[slot], WhereIn(context.Footprints[hit->Footprint], *hit->Range, fatal->Stack[slot], context), 40 - static_cast<int>(std::min<std::size_t>(slot, 20)));
            }
            for (const Target::CrashRecord* earlier : recent)
            {
                if (earlier == fatal || earlier->Tick + kEarlierMs < fatal->Tick || earlier->Tick > fatal->Tick)
                    continue;
                if (const auto hit = Find(context.Footprints, earlier->Ip(), earlier->Tick))
                    blame(hit->Footprint, CrashLink::Earlier, earlier->Ip(), WhereIn(context.Footprints[hit->Footprint], *hit->Range, earlier->Ip(), context), 30);
            }
        }
        for (auto& [index, entry] : blames)
            report.Blames.push_back(std::move(entry));
        std::stable_sort(report.Blames.begin(), report.Blames.end(), [](const CrashBlame& a, const CrashBlame& b) { return a.Weight > b.Weight; });

        // ∑ Without an exception exit code a game may have ended on purpose
        //   (its own crash handler, or the player): only a recent exception in
        //   a script's code makes that a crash worth naming.
        if (!exceptionExit)
        {
            if (!fatal || report.Blames.empty() || (context.EndTick && fatal->Tick + kQuietExitMs < context.EndTick))
                return report;
        }
        report.Crashed = true;
        if (fatal && context.EndTick && context.EndTick > fatal->Tick)
            report.SecondsBeforeEnd = static_cast<double>(context.EndTick - fatal->Tick) / 1000.0;

        std::uint64_t lastTick = 0;
        for (const ScriptFootprint& footprint : context.Footprints)
        {
            if (OnAt(footprint, at) && std::find(report.SwitchedOn.begin(), report.SwitchedOn.end(), footprint.Label) == report.SwitchedOn.end())
                report.SwitchedOn.push_back(footprint.Label);
            for (const std::uint64_t tick : { footprint.OnTick, footprint.OffTick })
            {
                if (tick && tick <= at && tick >= lastTick)
                {
                    lastTick = tick;
                    report.LastSwitched = footprint.Label;
                    report.LastSwitchedSeconds = static_cast<double>(at - tick) / 1000.0;
                }
            }
        }

        const bool is64Bit = capture.Is64Bit;
        if (fatal)
            report.Fatal = *fatal;
        const std::string what = fatal ? ExceptionText(*fatal, is64Bit) : ExceptionName(context.ExitCode);
        const std::string place = fatal ? (context.Describe ? context.Describe(fatal->Ip()) : Hex(fatal->Ip())) : std::string();
        if (!report.Blames.empty())
        {
            const CrashBlame& top = report.Blames.front();
            switch (top.Link)
            {
            case CrashLink::InCode:
                report.Headline = Format("%s crashed in the code of \"%s\" (%s): %s.", who.c_str(), top.Label.c_str(), top.Where.c_str(), what.c_str());
                break;
            case CrashLink::InPatch:
                report.Headline = Format("%s crashed in the bytes \"%s\" patched (%s): %s.", who.c_str(), top.Label.c_str(), top.Where.c_str(), what.c_str());
                break;
            case CrashLink::BehindPatch:
                report.Headline = Format("%s crashed right behind the patch of \"%s\" (%s): %s. Its jump back probably lands inside an instruction.", who.c_str(), top.Label.c_str(),
                    top.Where.c_str(), what.c_str());
                break;
            case CrashLink::RanFreed:
                report.Headline = Format("%s crashed running code of \"%s\" after it was switched off (%s): %s. Something still jumps into its freed memory.", who.c_str(),
                    top.Label.c_str(), top.Where.c_str(), what.c_str());
                break;
            case CrashLink::Unbalanced:
            {
                const std::string first = top.Where.substr(0, top.Where.find(". "));
                report.Headline = Format("%s crashed in %s: %s. \"%s\" is the likely cause: %s.", who.c_str(), place.c_str(), what.c_str(), top.Label.c_str(), first.c_str());
                break;
            }
            case CrashLink::NearPatch:
                report.Headline = Format("%s crashed in %s: %s. That is %s of \"%s\": the script's code did not fail itself, but what it changed (a register, the stack, a value) may have.",
                    who.c_str(), place.c_str(), what.c_str(), top.Where.c_str(), top.Label.c_str());
                break;
            case CrashLink::Accessed:
                report.Headline = Format("%s crashed in %s accessing memory of \"%s\" (%s): %s.", who.c_str(), place.c_str(), top.Label.c_str(), top.Where.c_str(), what.c_str());
                break;
            case CrashLink::OnStack:
                report.Headline = Format("%s crashed in %s, called from the code of \"%s\" (%s): %s.", who.c_str(), place.c_str(), top.Label.c_str(), top.Where.c_str(), what.c_str());
                break;
            case CrashLink::Earlier:
                report.Headline = Format("%s crashed in %s: %s. Shortly before, \"%s\" caused an exception in its code (%s).", who.c_str(), place.c_str(), what.c_str(),
                    top.Label.c_str(), top.Where.c_str());
                break;
            }
        }
        else if (fatal)
        {
            report.Headline = Format("%s crashed in %s: %s. No script's code was involved", who.c_str(), place.c_str(), what.c_str());
            if (!report.LastSwitched.empty())
                report.Headline += Format("; the last script switched was \"%s\", %s before.", report.LastSwitched.c_str(), Seconds(report.LastSwitchedSeconds).c_str());
            else
                report.Headline += ".";
        }
        else
        {
            report.Headline = Format("%s ended with %s (%08X)%s", who.c_str(), what.c_str(), context.ExitCode,
                report.Watched ? "; the crash watch noted no exception before it." : "; no crash watch was in the game, so where it happened is not known.");
        }

        std::string details;
        if (fatal)
        {
            details += Format("Exception: %s at %s, thread %u", what.c_str(), Describe(context, fatal->Ip(), at).c_str(), fatal->ThreadId);
            details += Format("\nExit code: %08X (%s)", context.ExitCode,
                report.Exact ? "the same exception" : exceptionExit ? ExceptionName(context.ExitCode).c_str() : "the game ended itself");
            if (report.SecondsBeforeEnd > 0.0)
                details += Format(", %.1f s after the exception", report.SecondsBeforeEnd);
            if (!fatal->Bytes.empty())
            {
                std::string bytes;
                for (const std::uint8_t byte : fatal->Bytes)
                    bytes += Format("%02X ", byte);
                details += "\nBytes at the address: " + bytes;
                if (context.Disassembler)
                {
                    const Disasm::Instruction instruction = context.Disassembler->Decode(fatal->Ip(), fatal->Bytes.data(), fatal->Bytes.size());
                    if (instruction.Valid)
                        details += " (" + instruction.Text() + ")";
                }
            }
            else
            {
                details += "\nThe address could not be read: the code was not there (freed memory, or a jump into nowhere).";
            }
            details += "\nRegisters:";
            const std::size_t count = is64Bit ? std::size(kNames64) : std::size(kNames32);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::uint64_t value = fatal->Registers[i];
                details += Format("%s%s=%s", i % 6 == 0 ? "\n  " : "  ", is64Bit ? kNames64[i] : kNames32[i],
                    is64Bit ? Format("%016llX", static_cast<unsigned long long>(value)).c_str() : Format("%08llX", static_cast<unsigned long long>(value)).c_str());
            }
            std::size_t shown = 0;
            std::string stack;
            for (std::size_t slot = 0; slot < fatal->Stack.size() && shown < kStackShown; ++slot)
            {
                const std::uint64_t value = fatal->Stack[slot];
                const std::string name = Describe(context, value, at);
                if (name.empty() || name == Hex(value))
                    continue;
                stack += Format("\n  [sp+%zX] %s", slot * (is64Bit ? 8 : 4), name.c_str());
                ++shown;
            }
            if (!stack.empty())
                details += "\nOn the stack:" + stack;
        }
        else
        {
            details += Format("Exit code: %08X (%s)", context.ExitCode, ExceptionName(context.ExitCode).c_str());
        }
        if (!report.Blames.empty())
        {
            details += "\nScripts involved:";
            for (const CrashBlame& entry : report.Blames)
                details += Format("\n  \"%s\": %s, %s", entry.Label.c_str(), CrashLinkText(entry.Link), entry.Where.c_str());
        }
        details += "\nScripts on at the time: " + (report.SwitchedOn.empty() ? std::string("none") : "\"" + Join(report.SwitchedOn, "\", \"") + "\"");
        if (!report.LastSwitched.empty())
            details += Format("\nLast switched: \"%s\", %s before", report.LastSwitched.c_str(), Seconds(report.LastSwitchedSeconds).c_str());
        const std::size_t others = recent.size() - (fatal ? 1 : 0);
        if (others)
        {
            details += Format("\nOther exceptions in the last %llu s: %zu", static_cast<unsigned long long>(kLookBackMs / 1000), others);
            std::size_t listed = 0;
            for (auto it = recent.rbegin(); it != recent.rend() && listed < 5; ++it)
            {
                if (*it == fatal)
                    continue;
                details += Format("\n  %s at %s", ExceptionText(**it, is64Bit).c_str(), Describe(context, (*it)->Ip(), (*it)->Tick).c_str());
                ++listed;
            }
        }
        if (!report.Watched)
            details += "\nThe crash watch was not in the game (it starts when a script changes the game's code).";
        report.Details = std::move(details);
        return report;
    }
}
