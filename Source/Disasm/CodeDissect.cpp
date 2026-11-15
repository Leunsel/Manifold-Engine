#include "Disasm/CodeDissect.h"

#include "Zydis.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace Manifold::Disasm
{
    namespace
    {
        constexpr std::uint64_t kChunk = 0x100000;
        constexpr std::uint64_t kPage = 0x1000;
        constexpr std::size_t kMaxString = 512;
        constexpr std::size_t kWindow = 0x10000;

        class Spans
        {
        public:
            explicit Spans(std::vector<CodeSpan> spans) : m_spans(std::move(spans))
            {
                std::sort(m_spans.begin(), m_spans.end(), [](const CodeSpan& a, const CodeSpan& b) { return a.Start < b.Start; });
            }

            bool Contains(std::uint64_t address) const
            {
                auto it = std::upper_bound(m_spans.begin(), m_spans.end(), address, [](std::uint64_t value, const CodeSpan& span) { return value < span.Start; });
                if (it == m_spans.begin())
                    return false;
                --it;
                return address < it->End;
            }

        private:
            std::vector<CodeSpan> m_spans;
        };

        bool Printable(std::uint8_t c)
        {
            return (c >= 0x20 && c < 0x7F) || c == '\t' || c == '\n' || c == '\r';
        }

        bool ValidUtf8(std::string_view text)
        {
            for (std::size_t i = 0; i < text.size();)
            {
                const auto c = static_cast<std::uint8_t>(text[i]);
                int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
                if (extra < 0 || (c >= 0x80 && c < 0xC2) || i + static_cast<std::size_t>(extra) >= text.size() + (extra == 0 ? 1 : 0))
                    return false;
                for (int k = 1; k <= extra; ++k)
                {
                    if ((static_cast<std::uint8_t>(text[i + static_cast<std::size_t>(k)]) & 0xC0) != 0x80)
                        return false;
                }
                i += static_cast<std::size_t>(extra) + 1;
            }
            return true;
        }

        int LettersAndDigits(std::string_view text)
        {
            int count = 0;
            for (char c : text)
            {
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                    ++count;
            }
            return count;
        }

        void AppendUtf8(std::string& text, std::uint32_t code)
        {
            if (code < 0x80)
            {
                text += static_cast<char>(code);
            }
            else if (code < 0x800)
            {
                text += static_cast<char>(0xC0 | (code >> 6));
                text += static_cast<char>(0x80 | (code & 0x3F));
            }
            else
            {
                text += static_cast<char>(0xE0 | (code >> 12));
                text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                text += static_cast<char>(0x80 | (code & 0x3F));
            }
        }

        class Window
        {
        public:
            explicit Window(const Target::Process& process) : m_process(process), m_bytes(kWindow) {}

            std::span<const std::uint8_t> At(std::uint64_t address)
            {
                if (address < m_start || address + kMaxString * 2 + 2 > m_start + m_valid)
                {
                    m_start = address & ~(kPage - 1);
                    m_valid = m_process.Read(m_start, m_bytes.data(), m_bytes.size());
                }
                if (address < m_start || address >= m_start + m_valid)
                    return {};
                const std::size_t offset = static_cast<std::size_t>(address - m_start);
                return std::span<const std::uint8_t>(m_bytes.data() + offset, m_valid - offset);
            }

        private:
            const Target::Process& m_process;
            std::vector<std::uint8_t> m_bytes;
            std::uint64_t m_start = 0;
            std::size_t m_valid = 0;
        };

        void Add(std::vector<CodeRef>& refs, DissectStats& stats, std::uint64_t target, std::uint64_t from, RefKind kind)
        {
            refs.push_back({ target, (from & 0x00FFFFFFFFFFFFFFull) | (static_cast<std::uint64_t>(kind) << 56) });
            switch (kind)
            {
            case RefKind::Call: ++stats.Calls; break;
            case RefKind::Jump: ++stats.Jumps; break;
            case RefKind::Conditional: ++stats.Conditionals; break;
            case RefKind::Read: ++stats.Reads; break;
            case RefKind::Write: ++stats.Writes; break;
            case RefKind::Address: ++stats.Addresses; break;
            }
        }
    }

    const char* RefKindName(RefKind kind)
    {
        switch (kind)
        {
        case RefKind::Call: return "call";
        case RefKind::Jump: return "jump";
        case RefKind::Conditional: return "conditional jump";
        case RefKind::Read: return "read";
        case RefKind::Write: return "write";
        case RefKind::Address: return "address";
        }
        return "";
    }

    bool ReadStringAt(std::span<const std::uint8_t> bytes, std::string& text, bool& wide)
    {
        text.clear();
        wide = false;
        std::size_t length = 0;
        bool high = false;
        while (length < bytes.size() && length < kMaxString && bytes[length] != 0 && (Printable(bytes[length]) || bytes[length] >= 0x80))
        {
            high = high || bytes[length] >= 0x80;
            ++length;
        }
        const bool ended = length == kMaxString || (length < bytes.size() && bytes[length] == 0);
        if (length >= 4 && ended)
        {
            text.assign(reinterpret_cast<const char*>(bytes.data()), length);
            if ((!high || ValidUtf8(text)) && LettersAndDigits(text) >= 2)
                return true;
        }
        text.clear();
        std::size_t units = 0;
        while ((units + 1) * 2 <= bytes.size() && units < kMaxString)
        {
            const std::uint32_t code = static_cast<std::uint32_t>(bytes[units * 2]) | (static_cast<std::uint32_t>(bytes[units * 2 + 1]) << 8);
            if (code == 0)
                break;
            const bool ok = (code < 0x80 && Printable(static_cast<std::uint8_t>(code))) || (code >= 0xA0 && code < 0xD800) || (code >= 0xE000 && code < 0xFFFE);
            if (!ok)
            {
                text.clear();
                return false;
            }
            AppendUtf8(text, code);
            ++units;
        }
        const bool wideEnded = units == kMaxString || ((units + 1) * 2 <= bytes.size() && bytes[units * 2] == 0 && bytes[units * 2 + 1] == 0);
        if (units >= 4 && wideEnded && LettersAndDigits(text) >= 2)
        {
            wide = true;
            return true;
        }
        text.clear();
        return false;
    }

    std::span<const CodeRef> DissectResult::To(std::uint64_t target) const
    {
        return Between(target, target + 1);
    }

    std::span<const CodeRef> DissectResult::Between(std::uint64_t start, std::uint64_t end) const
    {
        const auto first = std::lower_bound(Refs.begin(), Refs.end(), start, [](const CodeRef& ref, std::uint64_t value) { return ref.Target < value; });
        const auto last = std::lower_bound(first, Refs.end(), end, [](const CodeRef& ref, std::uint64_t value) { return ref.Target < value; });
        return std::span<const CodeRef>(Refs.data() + (first - Refs.begin()), static_cast<std::size_t>(last - first));
    }

    bool DissectResult::Covers(std::uint64_t address) const
    {
        return std::any_of(Spans.begin(), Spans.end(), [&](const CodeSpan& span) { return address >= span.Start && address < span.End; });
    }

    std::vector<CodeSpan> ExecutableSpans(const Target::Process& process, const std::vector<Target::ModuleInfo>& modules)
    {
        std::vector<CodeSpan> spans;
        for (const Target::ModuleInfo& module : modules)
        {
            for (const Target::RegionInfo& region : process.Regions(module.Base, module.Base + module.Size))
            {
                if (!region.Committed() || !region.Executable() || !region.Readable())
                    continue;
                const std::uint64_t start = std::max(region.Base, module.Base);
                const std::uint64_t end = std::min(region.Base + region.Size, module.Base + module.Size);
                if (start >= end)
                    continue;
                if (!spans.empty() && spans.back().End == start)
                    spans.back().End = end;
                else
                    spans.push_back({ start, end });
            }
        }
        return spans;
    }

    std::shared_ptr<DissectResult> DissectCode(const Target::Process& process, const std::vector<Target::ModuleInfo>& modules, DissectProgress* progress)
    {
        const auto started = std::chrono::steady_clock::now();
        auto result = std::make_shared<DissectResult>();
        result->Modules = modules;
        result->Is64Bit = process.Is64Bit();
        result->Spans = ExecutableSpans(process, modules);

        std::vector<CodeSpan> loaded;
        if (const Target::ModuleList list = process.Modules())
        {
            for (const Target::ModuleInfo& module : *list)
                loaded.push_back({ module.Base, module.Base + module.Size });
        }
        for (const Target::ModuleInfo& module : modules)
            loaded.push_back({ module.Base, module.Base + module.Size });
        const Spans inModules(std::move(loaded));

        DissectStats& stats = result->Stats;
        for (const CodeSpan& span : result->Spans)
            stats.CodeBytes += span.End - span.Start;
        if (progress)
        {
            progress->Phase = 0;
            progress->Done = 0;
            progress->Total = stats.CodeBytes;
        }
        result->Refs.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(stats.CodeBytes / 6, 64ull << 20)));

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, result->Is64Bit ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32, result->Is64Bit ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        std::vector<std::uint8_t> buffer(kChunk);
        std::uint64_t done = 0;
        for (const CodeSpan& span : result->Spans)
        {
            std::uint64_t at = span.Start;
            while (at < span.End)
            {
                if (progress && progress->Cancel.load())
                {
                    result->Cancelled = true;
                    break;
                }
                const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, span.End - at));
                const std::size_t got = process.Read(at, buffer.data(), wanted);
                if (got == 0)
                {
                    const std::uint64_t next = std::min(span.End, (at & ~(kPage - 1)) + kPage);
                    done += next - at;
                    at = next;
                    continue;
                }
                std::size_t position = 0;
                const bool last = at + got >= span.End;
                while (position < got)
                {
                    const std::size_t available = got - position;
                    if (available < ZYDIS_MAX_INSTRUCTION_LENGTH && !last)
                        break;
                    const std::uint64_t address = at + position;
                    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, buffer.data() + position, available, &instruction, operands)))
                    {
                        ++stats.Invalid;
                        ++position;
                        continue;
                    }
                    ++stats.Instructions;
                    RefKind branch = RefKind::Read;
                    bool isBranch = true;
                    switch (instruction.meta.category)
                    {
                    case ZYDIS_CATEGORY_CALL: branch = RefKind::Call; break;
                    case ZYDIS_CATEGORY_UNCOND_BR: branch = RefKind::Jump; break;
                    case ZYDIS_CATEGORY_COND_BR: branch = RefKind::Conditional; break;
                    default: isBranch = false; break;
                    }
                    for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
                    {
                        const ZydisDecodedOperand& operand = operands[i];
                        if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                        {
                            if (operand.imm.is_relative)
                            {
                                ZyanU64 target = 0;
                                if (isBranch && ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)) && inModules.Contains(target))
                                    Add(result->Refs, stats, target, address, branch);
                            }
                            else if (operand.size >= 32)
                            {
                                const std::uint64_t value = result->Is64Bit ? operand.imm.value.u : (operand.imm.value.u & 0xFFFFFFFFull);
                                if (value > 0x10000 && inModules.Contains(value))
                                    Add(result->Refs, stats, value, address, RefKind::Address);
                            }
                        }
                        else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.type == ZYDIS_MEMOP_TYPE_MEM)
                        {
                            const bool rip = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
                            if (!rip && !(operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE))
                                continue;
                            ZyanU64 target = 0;
                            if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)) || !inModules.Contains(target))
                                continue;
                            RefKind kind = RefKind::Read;
                            if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA)
                                kind = RefKind::Address;
                            else if (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)
                                kind = RefKind::Write;
                            Add(result->Refs, stats, target, address, kind);
                        }
                        else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.type == ZYDIS_MEMOP_TYPE_AGEN)
                        {
                            const bool rip = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
                            ZyanU64 target = 0;
                            if ((rip || (operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE)) &&
                                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)) && inModules.Contains(target))
                                Add(result->Refs, stats, target, address, RefKind::Address);
                        }
                    }
                    position += instruction.length;
                }
                if (position == 0)
                    position = 1;
                at += position;
                done += position;
                if (progress)
                    progress->Done = done;
            }
            if (result->Cancelled)
                break;
        }

        if (progress)
            progress->Phase = 1;
        std::sort(result->Refs.begin(), result->Refs.end(), [](const CodeRef& a, const CodeRef& b) { return a.Target != b.Target ? a.Target < b.Target : a.Packed < b.Packed; });
        result->Refs.erase(std::unique(result->Refs.begin(), result->Refs.end(), [](const CodeRef& a, const CodeRef& b) { return a.Target == b.Target && a.Packed == b.Packed; }), result->Refs.end());

        if (progress)
            progress->Phase = 2;
        Window window(process);
        std::string text;
        bool wide = false;
        for (std::size_t i = 0; i < result->Refs.size() && !result->Cancelled;)
        {
            const std::uint64_t target = result->Refs[i].Target;
            std::uint32_t calls = 0;
            std::uint32_t data = 0;
            std::size_t j = i;
            for (; j < result->Refs.size() && result->Refs[j].Target == target; ++j)
            {
                const RefKind kind = result->Refs[j].Kind();
                if (kind == RefKind::Call)
                    ++calls;
                else if (kind == RefKind::Read || kind == RefKind::Address)
                    ++data;
            }
            if (calls)
                result->Functions.push_back({ target, calls });
            if (data && !result->Covers(target) && ReadStringAt(window.At(target), text, wide))
                result->Strings.push_back({ target, text, wide, data });
            i = j;
            if (progress && progress->Cancel.load())
                result->Cancelled = true;
        }
        stats.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (progress)
            progress->Phase = 3;
        return result;
    }
}
