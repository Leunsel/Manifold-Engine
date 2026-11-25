#include "Disasm/Disassembler.h"

#include "Core/Text.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

#pragma warning(push, 0)
#include "Zydis.h"
#pragma warning(pop)

#include <algorithm>
#include <cstring>

namespace Manifold::Disasm
{
    namespace
    {
        constexpr std::uint64_t kLookBack = 48;
        // How far below an address a known instruction start may sit and still
        // be swept forward from when backing up (a long Mono method or a big
        // native function fits; beyond that the backward guess takes over).
        constexpr std::uint64_t kSweepReach = 0x10000;
        constexpr std::size_t kPointer = 8;
        constexpr std::size_t kWindow = 16;

        std::size_t Readable(const std::vector<bool>& readable, std::size_t offset)
        {
            std::size_t available = 0;
            while (offset + available < readable.size() && readable[offset + available] && available < kWindow)
                ++available;
            return available;
        }

        bool ZeroPair(const std::uint8_t* bytes, std::size_t length)
        {
            return length == 2 && bytes[0] == 0 && bytes[1] == 0;
        }

        bool LoneZero(const std::uint8_t* bytes, std::size_t window, bool zeros)
        {
            return zeros && window >= 2 && bytes[0] == 0 && bytes[1] != 0;
        }

        std::size_t InlinePointer(const ZydisDecodedInstruction& instruction, const std::uint8_t* bytes, std::size_t raw)
        {
            if (instruction.machine_mode != ZYDIS_MACHINE_MODE_LONG_64 || instruction.opcode_map != ZYDIS_OPCODE_MAP_DEFAULT || instruction.opcode != 0xFF ||
                !(instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM) || instruction.raw.modrm.mod != 0 || instruction.raw.modrm.rm != 5 || instruction.operand_width != 64)
                return 0;
            const std::size_t length = instruction.length;
            const ZyanU8 kind = instruction.raw.modrm.reg;
            std::size_t folded = 0;
            if ((kind == 2 || kind == 4) && instruction.raw.disp.value == 0)
                folded = kPointer;
            else if (kind == 2 && instruction.raw.disp.value == 2 && raw >= length + 2 && bytes[length] == 0xEB && bytes[length + 1] == 0x08)
                folded = 2 + kPointer;
            return folded && length + folded <= raw && length + folded <= kWindow ? folded : 0;
        }

        void ReadSpan(const Target::Process& process, std::uint64_t address, std::vector<std::uint8_t>& buffer, std::vector<bool>& readable)
        {
            readable.assign(buffer.size(), false);
            for (std::size_t page = 0; page < buffer.size();)
            {
                const std::uint64_t at = address + page;
                const std::size_t chunk = std::min<std::size_t>(buffer.size() - page, 0x1000 - static_cast<std::size_t>(at & 0xFFF));
                if (process.ReadExact(at, buffer.data() + page, chunk))
                    std::fill(readable.begin() + static_cast<std::ptrdiff_t>(page), readable.begin() + static_cast<std::ptrdiff_t>(page + chunk), true);
                page += chunk;
            }
        }

        std::uint64_t Value(const RegisterValues& r, ZydisRegister reg)
        {
            const ZydisRegister wide = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
            std::uint64_t value = 0;
            switch (wide)
            {
            case ZYDIS_REGISTER_RAX: value = r.Rax; break;
            case ZYDIS_REGISTER_RBX: value = r.Rbx; break;
            case ZYDIS_REGISTER_RCX: value = r.Rcx; break;
            case ZYDIS_REGISTER_RDX: value = r.Rdx; break;
            case ZYDIS_REGISTER_RSI: value = r.Rsi; break;
            case ZYDIS_REGISTER_RDI: value = r.Rdi; break;
            case ZYDIS_REGISTER_RBP: value = r.Rbp; break;
            case ZYDIS_REGISTER_RSP: value = r.Rsp; break;
            case ZYDIS_REGISTER_R8: value = r.R8; break;
            case ZYDIS_REGISTER_R9: value = r.R9; break;
            case ZYDIS_REGISTER_R10: value = r.R10; break;
            case ZYDIS_REGISTER_R11: value = r.R11; break;
            case ZYDIS_REGISTER_R12: value = r.R12; break;
            case ZYDIS_REGISTER_R13: value = r.R13; break;
            case ZYDIS_REGISTER_R14: value = r.R14; break;
            case ZYDIS_REGISTER_R15: value = r.R15; break;
            case ZYDIS_REGISTER_RIP: value = r.Rip; break;
            default: break;
            }
            switch (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg))
            {
            case 8: return (reg >= ZYDIS_REGISTER_AH && reg <= ZYDIS_REGISTER_BH) ? (value >> 8) & 0xFF : value & 0xFF;
            case 16: return value & 0xFFFF;
            case 32: return value & 0xFFFFFFFFull;
            default: return value;
            }
        }
    }

    struct Disassembler::State
    {
        ZydisDecoder Decoder{};
        ZydisFormatter Formatter{};
        ZydisFormatter Sized{};
    };

    namespace
    {
        void SetUpFormatter(ZydisFormatter& formatter, bool forceSize)
        {
            ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_HEX_PREFIX, 0);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_HEX_UPPERCASE, ZYAN_TRUE);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SIZE, forceSize ? ZYAN_TRUE : ZYAN_FALSE);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_IMM_PADDING, ZYDIS_PADDING_DISABLED);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_DISP_PADDING, ZYDIS_PADDING_DISABLED);
        }

        bool NeedsWrittenSize(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands)
        {
            if (instruction.operand_count_visible != 1 || operands[0].type != ZYDIS_OPERAND_TYPE_MEMORY || operands[0].mem.type != ZYDIS_MEMOP_TYPE_MEM)
                return false;
            if (instruction.meta.category == ZYDIS_CATEGORY_PREFETCH)
                return false;
            switch (instruction.mnemonic)
            {
            case ZYDIS_MNEMONIC_CLFLUSH:
            case ZYDIS_MNEMONIC_CLFLUSHOPT:
            case ZYDIS_MNEMONIC_CLWB:
            case ZYDIS_MNEMONIC_CLDEMOTE:
            case ZYDIS_MNEMONIC_INVLPG:
                return false;
            default:
                return true;
            }
        }
    }

    std::string Instruction::BytesText() const
    {
        return HexBytes(std::span<const std::uint8_t>(Bytes.data(), std::min<std::size_t>(Length, Bytes.size())));
    }

    std::string CopyText(std::span<const Instruction> lines, const Target::Symbols& symbols, Target::AddressStyle style)
    {
        std::string text;
        for (const Instruction& line : lines)
            text += symbols.AddressText(line.Address, style) + " - " + line.BytesText() + " - " + line.Text() + "\n";
        return text;
    }

    ConditionInfo EvaluateCondition(std::string_view mnemonic, const RegisterValues& registers, bool is64Bit)
    {
        constexpr int kCarry = 0;
        constexpr int kParity = 2;
        constexpr int kZero = 6;
        constexpr int kSign = 7;
        constexpr int kOverflow = 11;
        ConditionInfo out;
        std::string name = Lower(TrimView(mnemonic));
        if (const std::size_t space = name.rfind(' '); space != std::string::npos)
            name = name.substr(space + 1);
        const auto flag = [&](int bit) { return ((registers.Flags >> bit) & 1) != 0; };
        const std::uint64_t counter = is64Bit ? registers.Rcx : (registers.Rcx & 0xFFFFFFFFull);

        if (name == "jcxz" || name == "jecxz" || name == "jrcxz")
        {
            const std::uint64_t value = name == "jcxz" ? (registers.Rcx & 0xFFFF) : name == "jecxz" ? (registers.Rcx & 0xFFFFFFFFull) : registers.Rcx;
            out.Kind = ConditionKind::Jump;
            out.UsesCounter = true;
            out.Taken = value == 0;
            return out;
        }
        if (name == "loop" || name == "loope" || name == "loopz" || name == "loopne" || name == "loopnz")
        {
            const bool left = counter - 1 != 0;
            out.Kind = ConditionKind::Jump;
            out.UsesCounter = true;
            if (name == "loop")
            {
                out.Taken = left;
            }
            else
            {
                const bool wantZero = name == "loope" || name == "loopz";
                out.FlagsRead = 1u << kZero;
                out.Taken = left && flag(kZero) == wantZero;
            }
            return out;
        }

        std::string code;
        ConditionKind kind = ConditionKind::None;
        if (name.rfind("cmov", 0) == 0)
        {
            code = name.substr(4);
            kind = ConditionKind::Move;
        }
        else if (name.rfind("set", 0) == 0)
        {
            code = name.substr(3);
            kind = ConditionKind::Set;
        }
        else if (name.size() > 1 && name[0] == 'j' && name != "jmp")
        {
            code = name.substr(1);
            kind = ConditionKind::Jump;
        }
        else
        {
            return out;
        }
        static const std::pair<const char*, const char*> kAliases[] = { { "c", "b" }, { "nae", "b" }, { "nc", "nb" }, { "ae", "nb" }, { "e", "z" }, { "ne", "nz" },
            { "na", "be" }, { "a", "nbe" }, { "pe", "p" }, { "po", "np" }, { "nge", "l" }, { "ge", "nl" }, { "ng", "le" }, { "g", "nle" } };
        for (const auto& [from, to] : kAliases)
        {
            if (code == from)
            {
                code = to;
                break;
            }
        }
        bool negate = false;
        if (code.size() > 1 && code[0] == 'n')
        {
            negate = true;
            code.erase(0, 1);
        }
        bool value = false;
        std::uint32_t mask = 0;
        if (code == "o")
        {
            value = flag(kOverflow);
            mask = 1u << kOverflow;
        }
        else if (code == "b")
        {
            value = flag(kCarry);
            mask = 1u << kCarry;
        }
        else if (code == "z")
        {
            value = flag(kZero);
            mask = 1u << kZero;
        }
        else if (code == "be")
        {
            value = flag(kCarry) || flag(kZero);
            mask = (1u << kCarry) | (1u << kZero);
        }
        else if (code == "s")
        {
            value = flag(kSign);
            mask = 1u << kSign;
        }
        else if (code == "p")
        {
            value = flag(kParity);
            mask = 1u << kParity;
        }
        else if (code == "l")
        {
            value = flag(kSign) != flag(kOverflow);
            mask = (1u << kSign) | (1u << kOverflow);
        }
        else if (code == "le")
        {
            value = flag(kZero) || flag(kSign) != flag(kOverflow);
            mask = (1u << kZero) | (1u << kSign) | (1u << kOverflow);
        }
        else
        {
            return out;
        }
        out.Kind = kind;
        out.Taken = negate ? !value : value;
        out.FlagsRead = mask;
        return out;
    }

    Disassembler::Disassembler(bool is64Bit) : m_state(new State()), m_is64Bit(is64Bit)
    {
        ZydisDecoderInit(&m_state->Decoder, is64Bit ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32, is64Bit ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
        SetUpFormatter(m_state->Formatter, false);
        SetUpFormatter(m_state->Sized, true);
    }

    Disassembler::~Disassembler()
    {
        delete m_state;
    }

    Instruction Disassembler::Decode(std::uint64_t address, const std::uint8_t* bytes, std::size_t available, const Target::Symbols* symbols) const
    {
        return DecodeWindow(address, bytes, available, available, symbols);
    }

    Instruction Disassembler::DecodeWindow(std::uint64_t address, const std::uint8_t* bytes, std::size_t available, std::size_t raw, const Target::Symbols* symbols) const
    {
        Instruction result;
        result.Address = address;
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (available == 0 || !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&m_state->Decoder, bytes, available, &instruction, operands)))
        {
            result.Length = 1;
            if (available > 0)
                result.Bytes[0] = bytes[0];
            result.Mnemonic = available > 0 ? "db" : "??";
            result.Operands = available > 0 ? Hex(bytes[0], 2) : "";
            return result;
        }

        result.Valid = true;
        result.Length = instruction.length;
        std::memcpy(result.Bytes.data(), bytes, std::min<std::size_t>(instruction.length, result.Bytes.size()));

        char text[256];
        const ZydisFormatter& formatter = NeedsWrittenSize(instruction, operands) ? m_state->Sized : m_state->Formatter;
        if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, text, sizeof(text), address, nullptr)))
        {
            static const char* kPrefixes[] = { "lock ", "rep ", "repe ", "repz ", "repne ", "repnz ", "xacquire ", "xrelease ", "bnd ", "notrack " };
            const std::string full = text;
            std::size_t at = 0;
            for (bool again = true; again;)
            {
                again = false;
                for (const char* prefix : kPrefixes)
                {
                    if (full.compare(at, std::strlen(prefix), prefix) == 0)
                    {
                        at += std::strlen(prefix);
                        again = true;
                    }
                }
            }
            const std::size_t split = instruction.operand_count_visible > 0 ? full.find(' ', at) : std::string::npos;
            if (split == std::string::npos)
            {
                result.Mnemonic = full;
            }
            else
            {
                result.Mnemonic = full.substr(0, split);
                result.Operands = ReplaceAll(Trim(full.substr(split + 1)), ", ", ",");
            }
        }

        switch (instruction.meta.category)
        {
        case ZYDIS_CATEGORY_COND_BR: result.Kind = Flow::ConditionalJump; break;
        case ZYDIS_CATEGORY_UNCOND_BR: result.Kind = Flow::Jump; break;
        case ZYDIS_CATEGORY_CALL: result.Kind = Flow::Call; break;
        case ZYDIS_CATEGORY_RET: result.Kind = Flow::Return; break;
        case ZYDIS_CATEGORY_INTERRUPT: result.Kind = Flow::Interrupt; break;
        case ZYDIS_CATEGORY_NOP: result.IsNop = true; break;
        default: break;
        }
        if (instruction.mnemonic == ZYDIS_MNEMONIC_INT3)
            result.Kind = Flow::Interrupt;

        for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
        {
            const ZydisDecodedOperand& operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative)
            {
                ZyanU64 target = 0;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
                    result.Target = target;
            }
            else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.type == ZYDIS_MEMOP_TYPE_MEM)
            {
                MemoryOperand& memory = result.Memory;
                memory.Present = true;
                memory.Read = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0;
                memory.Written = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
                memory.Size = operand.size / 8;
                memory.Base = operand.mem.base;
                memory.Index = operand.mem.index;
                memory.Scale = operand.mem.scale;
                memory.Displacement = operand.mem.disp.value;
                memory.Segment = operand.mem.segment;
                memory.RipRelative = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
                if (memory.RipRelative || (operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE))
                {
                    ZyanU64 target = 0;
                    if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
                        memory.Absolute = target;
                }
                if ((result.Kind == Flow::Jump || result.Kind == Flow::Call) && memory.Absolute)
                {
                    if (symbols && symbols->Target())
                    {
                        if (const auto pointer = symbols->Target()->ReadPointer(*memory.Absolute))
                            result.Target = pointer;
                    }
                }
            }
        }
        if (const std::size_t folded = InlinePointer(instruction, bytes, raw))
        {
            std::uint64_t pointer = 0;
            std::memcpy(&pointer, bytes + instruction.length + folded - kPointer, kPointer);
            result.Length = static_cast<std::uint8_t>(instruction.length + folded);
            result.InlineBytes = static_cast<std::uint8_t>(folded);
            std::memcpy(result.Bytes.data(), bytes, result.Length);
            result.Target = pointer;
            result.Operands = Hex(pointer);
        }
        if (symbols)
        {
            const auto replace = [&](std::uint64_t value)
                {
                    const std::string written = Hex(value);
                    const std::string name = symbols->Describe(value);
                    if (name == written)
                        return;
                    std::size_t at = result.Operands.find(written);
                    while (at != std::string::npos)
                    {
                        const bool left = at == 0 || !IsHexDigit(result.Operands[at - 1]);
                        const bool right = at + written.size() >= result.Operands.size() || !IsHexDigit(result.Operands[at + written.size()]);
                        if (left && right)
                        {
                            result.Operands.replace(at, written.size(), name);
                            at = result.Operands.find(written, at + name.size());
                        }
                        else
                            at = result.Operands.find(written, at + 1);
                    }
                };
            if (result.Memory.Absolute)
                replace(*result.Memory.Absolute);
            if (result.InlineBytes && result.Target)
                replace(*result.Target);
            for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
            {
                if (operands[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
                    continue;
                if (operands[i].imm.is_relative)
                {
                    if (result.Target)
                        replace(*result.Target);
                    continue;
                }
                // An absolute immediate that lands on a user-space address may be
                // a function pointer loaded before an indirect call, as Mono does
                // with "mov r11, <method>; call r11". Replace it when it resolves
                // to a known symbol; the value gate skips small constants and the
                // sign-extended negatives of ordinary arithmetic, and replace()
                // itself only substitutes when a name is actually found.
                const std::uint64_t value = operands[i].imm.value.u;
                if (value >= 0x10000 && value < 0x0000800000000000ull)
                    replace(value);
            }
        }
        return result;
    }

    Instruction Disassembler::Read(const Target::Process& process, std::uint64_t address, const Target::Symbols* symbols) const
    {
        std::uint8_t bytes[16]{};
        const std::size_t got = process.Read(address, bytes, sizeof(bytes));
        if (got == 0)
        {
            Instruction unreadable;
            unreadable.Address = address;
            unreadable.Length = 1;
            unreadable.Mnemonic = "??";
            return unreadable;
        }
        return Decode(address, bytes, got, symbols);
    }

    std::vector<Instruction> Disassembler::ReadMany(const Target::Process& process, std::uint64_t address, int count, const Target::Symbols* symbols,
        const CodeHints* hints) const
    {
        if (count <= 0)
            return {};
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(count) * kWindow + kWindow);
        std::vector<bool> readable;
        ReadSpan(process, address, buffer, readable);
        return DecodeMany(buffer, readable, address, count, symbols, hints);
    }

    std::size_t Disassembler::Window(const std::vector<bool>& readable, std::size_t offset, std::uint64_t at, const CodeHints* hints) const
    {
        std::size_t available = Readable(readable, offset);
        // An instruction that would run across a known start is wrong: the bytes
        // in front of it are padding or data. Decoding only up to that start
        // turns them into short lines (or db) and the next line begins exactly there.
        if (hints && available > 1)
        {
            if (const auto boundary = hints->BoundaryIn(at, at + available); boundary && *boundary > at && *boundary < at + available)
                available = static_cast<std::size_t>(*boundary - at);
        }
        return available;
    }

    std::vector<Instruction> Disassembler::DecodeMany(const std::vector<std::uint8_t>& buffer, const std::vector<bool>& readable, std::uint64_t address, int count,
        const Target::Symbols* symbols, const CodeHints* hints) const
    {
        std::vector<Instruction> list;
        list.reserve(static_cast<std::size_t>(std::max(0, count)));
        std::size_t offset = 0;
        bool zeros = false;
        for (int i = 0; i < count; ++i)
        {
            const std::uint64_t at = address + offset;
            if (offset >= buffer.size() || !readable[offset])
            {
                Instruction unreadable;
                unreadable.Address = at;
                unreadable.Length = 1;
                unreadable.Mnemonic = "??";
                list.push_back(unreadable);
                ++offset;
                zeros = false;
                continue;
            }
            const std::uint8_t* bytes = buffer.data() + offset;
            const std::size_t window = Window(readable, offset, at, hints);
            Instruction instruction = DecodeWindow(at, bytes, LoneZero(bytes, window, zeros) ? 1 : window, Readable(readable, offset), symbols);
            zeros = instruction.Valid && ZeroPair(bytes, instruction.Length);
            offset += instruction.Length ? instruction.Length : 1;
            list.push_back(std::move(instruction));
        }
        return list;
    }

    std::vector<std::uint64_t> Disassembler::Starts(const Target::Process& process, std::uint64_t from, std::uint64_t to, const CodeHints* hints) const
    {
        std::vector<std::uint64_t> starts;
        if (to <= from)
            return starts;
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(to - from) + 16);
        std::vector<bool> readable;
        ReadSpan(process, from, buffer, readable);
        std::size_t offset = 0;
        bool zeros = false;
        while (from + offset < to)
        {
            starts.push_back(from + offset);
            std::size_t length = 1;
            bool pair = false;
            if (readable[offset])
            {
                const std::uint8_t* bytes = buffer.data() + offset;
                const std::size_t window = Window(readable, offset, from + offset, hints);
                if (const std::size_t step = LoneZero(bytes, window, zeros) ? 1 : StepLength(bytes, window, Readable(readable, offset)))
                {
                    length = step;
                    pair = ZeroPair(bytes, step);
                }
            }
            zeros = pair;
            offset += length;
        }
        return starts;
    }

    std::size_t Disassembler::StepLength(const std::uint8_t* bytes, std::size_t window, std::size_t raw) const
    {
        ZydisDecodedInstruction instruction{};
        if (window == 0 || !ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&m_state->Decoder, nullptr, bytes, window, &instruction)) || instruction.length == 0)
            return 0;
        return instruction.length + InlinePointer(instruction, bytes, raw);
    }

    std::optional<std::uint64_t> Disassembler::InlineHolder(const Target::Process& process, std::uint64_t address) const
    {
        if (!m_is64Bit || address < kWindow)
            return std::nullopt;
        std::uint8_t bytes[kWindow * 2]{};
        const std::size_t got = process.Read(address - kWindow, bytes, sizeof(bytes));
        if (got < kWindow)
            return std::nullopt;
        for (std::size_t back = 1; back < kWindow; ++back)
        {
            const std::size_t offset = kWindow - back;
            const std::size_t raw = std::min(got - offset, kWindow);
            ZydisDecodedInstruction instruction{};
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&m_state->Decoder, nullptr, bytes + offset, raw, &instruction)))
                continue;
            const std::size_t folded = InlinePointer(instruction, bytes + offset, raw);
            if (folded && back >= instruction.length && back < instruction.length + folded)
                return address - back;
        }
        return std::nullopt;
    }

    std::string FlagNames(std::uint32_t mask)
    {
        static constexpr std::pair<std::uint32_t, const char*> kFlags[] = { { ZYDIS_CPUFLAG_CF, "CF" }, { ZYDIS_CPUFLAG_PF, "PF" }, { ZYDIS_CPUFLAG_AF, "AF" },
            { ZYDIS_CPUFLAG_ZF, "ZF" }, { ZYDIS_CPUFLAG_SF, "SF" }, { ZYDIS_CPUFLAG_TF, "TF" }, { ZYDIS_CPUFLAG_IF, "IF" }, { ZYDIS_CPUFLAG_DF, "DF" },
            { ZYDIS_CPUFLAG_OF, "OF" } };
        std::string text;
        for (const auto& [bit, name] : kFlags)
        {
            if (mask & bit)
                text += (text.empty() ? "" : " ") + std::string(name);
        }
        return text;
    }

    std::vector<std::string> ExplainFacts(const InstructionFacts& facts)
    {
        std::vector<std::string> lines;
        for (const OperandFact& operand : facts.Operands)
        {
            if (operand.Text.empty())
                continue;
            if (operand.Immediate)
            {
                lines.push_back("Uses the constant " + operand.Text);
                continue;
            }
            const char* verb = operand.Read && operand.Written ? "Reads and writes" : operand.Written ? "Writes" : operand.Read ? "Reads" : "Uses";
            if (operand.Memory && operand.Size > 0)
                lines.push_back(std::string(verb) + " " + std::to_string(operand.Size) + (operand.Size == 1 ? " byte at " : " bytes at ") + operand.Text);
            else
                lines.push_back(std::string(verb) + " " + operand.Text);
        }
        if (facts.FlagsTested)
            lines.push_back("Reads the flags " + FlagNames(facts.FlagsTested));
        if (const std::uint32_t changed = facts.FlagsModified | facts.FlagsCleared | facts.FlagsSet)
        {
            std::string text = "Changes the flags " + FlagNames(changed);
            if (facts.FlagsCleared)
                text += " (clears " + FlagNames(facts.FlagsCleared) + ")";
            if (facts.FlagsSet)
                text += " (sets " + FlagNames(facts.FlagsSet) + ")";
            lines.push_back(text);
        }
        if (facts.FlagsUndefined)
            lines.push_back("Leaves undefined: " + FlagNames(facts.FlagsUndefined));
        return lines;
    }

    std::optional<InstructionFacts> Disassembler::Facts(const Instruction& line) const
    {
        if (!line.Valid || line.Length == 0)
            return std::nullopt;
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&m_state->Decoder, line.Bytes.data(), std::min<std::size_t>(line.Length, line.Bytes.size()), &instruction, operands)))
            return std::nullopt;
        InstructionFacts facts;
        if (const char* category = ZydisCategoryGetString(instruction.meta.category))
            facts.Category = category;
        if (const char* isa = ZydisISASetGetString(instruction.meta.isa_set))
            facts.IsaSet = isa;
        for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
        {
            const ZydisDecodedOperand& operand = operands[i];
            OperandFact fact;
            char text[128] = "";
            if (ZYAN_SUCCESS(ZydisFormatterFormatOperand(&m_state->Formatter, &instruction, &operand, text, sizeof(text), line.Address, nullptr)))
                fact.Text = text;
            fact.Read = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0;
            fact.Written = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
            fact.Memory = operand.type == ZYDIS_OPERAND_TYPE_MEMORY && operand.mem.type != ZYDIS_MEMOP_TYPE_AGEN;
            fact.Immediate = operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
            fact.Size = operand.size / 8;
            facts.Operands.push_back(std::move(fact));
        }
        if (instruction.cpu_flags)
        {
            facts.FlagsTested = instruction.cpu_flags->tested;
            facts.FlagsModified = instruction.cpu_flags->modified;
            facts.FlagsCleared = instruction.cpu_flags->set_0;
            facts.FlagsSet = instruction.cpu_flags->set_1;
            facts.FlagsUndefined = instruction.cpu_flags->undefined;
        }
        return facts;
    }

    std::uint16_t Disassembler::AddressBytes(const Instruction& line, const std::function<bool(std::uint64_t)>& isAddress) const
    {
        if (!line.Valid || line.Length == 0)
            return 0;
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        const std::size_t length = std::min<std::size_t>(line.Length, line.Bytes.size());
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&m_state->Decoder, line.Bytes.data(), length, &instruction, operands)))
            return 0;
        std::uint16_t bits = 0;
        const auto mark = [&](unsigned offset, unsigned size)
            {
                for (unsigned i = offset; i < offset + size / 8 && i < 16; ++i)
                    bits = static_cast<std::uint16_t>(bits | (1u << i));
            };
        if (instruction.raw.disp.size == 32 && (instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM))
        {
            const bool noBase = instruction.raw.modrm.mod == 0 && (instruction.raw.modrm.rm == 5 || (instruction.raw.modrm.rm == 4 && instruction.raw.sib.base == 5));
            const std::uint64_t value = static_cast<std::uint32_t>(instruction.raw.disp.value);
            if (m_is64Bit ? (instruction.raw.modrm.mod == 0 && instruction.raw.modrm.rm == 5) : (noBase || (isAddress && isAddress(value))))
                mark(instruction.raw.disp.offset, 32);
        }
        else if (instruction.raw.disp.size >= 32 && !(instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM))
        {
            mark(instruction.raw.disp.offset, instruction.raw.disp.size);
        }
        for (const auto& immediate : instruction.raw.imm)
        {
            if (immediate.size == 0)
                continue;
            if (immediate.is_relative)
            {
                if (immediate.size >= 32)
                    mark(immediate.offset, immediate.size);
            }
            else if (immediate.size >= 32 && isAddress && isAddress(immediate.size == 64 ? immediate.value.u : static_cast<std::uint32_t>(immediate.value.u)))
            {
                if (immediate.size == 64 || !m_is64Bit)
                    mark(immediate.offset, immediate.size);
            }
        }
        return bits;
    }

    std::optional<InstructionLayout> Disassembler::Layout(const Instruction& line) const
    {
        if (!line.Valid || line.Length == 0)
            return std::nullopt;
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        const std::size_t length = std::min<std::size_t>(line.Length, line.Bytes.size());
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&m_state->Decoder, line.Bytes.data(), length, &instruction, operands)) || instruction.length != length)
            return std::nullopt;
        InstructionLayout layout;
        layout.Length = static_cast<std::uint8_t>(length);
        unsigned prefixEnd = instruction.raw.prefix_count;
        if (instruction.attributes & ZYDIS_ATTRIB_HAS_REX)
            prefixEnd = std::max<unsigned>(prefixEnd, instruction.raw.rex.offset + 1u);
        if (instruction.attributes & ZYDIS_ATTRIB_HAS_VEX)
            prefixEnd = std::max<unsigned>(prefixEnd, instruction.raw.vex.offset + instruction.raw.vex.size);
        if (instruction.attributes & ZYDIS_ATTRIB_HAS_EVEX)
            prefixEnd = std::max<unsigned>(prefixEnd, instruction.raw.evex.offset + 4u);
        if (instruction.attributes & ZYDIS_ATTRIB_HAS_XOP)
            prefixEnd = std::max<unsigned>(prefixEnd, instruction.raw.xop.offset + 3u);
        layout.PrefixEnd = static_cast<std::uint8_t>(std::min<unsigned>(prefixEnd, static_cast<unsigned>(length)));
        if (instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM)
            layout.ModRm = instruction.raw.modrm.offset;
        if (instruction.attributes & ZYDIS_ATTRIB_HAS_SIB)
            layout.Sib = instruction.raw.sib.offset;
        for (std::uint8_t i = 0; i < instruction.operand_count; ++i)
        {
            if (operands[i].encoding == ZYDIS_OPERAND_ENCODING_MODRM_REG)
                layout.RegisterInReg = true;
        }
        const auto read = [&](unsigned offset, unsigned bytes)
            {
                std::uint64_t value = 0;
                for (unsigned i = 0; i < bytes && offset + i < length; ++i)
                    value |= static_cast<std::uint64_t>(line.Bytes[offset + i]) << (8 * i);
                if (bytes > 0 && bytes < 8 && (value >> (8 * bytes - 1)) & 1)
                    value |= ~0ull << (8 * bytes);
                return static_cast<std::int64_t>(value);
            };
        if (instruction.raw.disp.size)
        {
            InstructionField field;
            field.Offset = instruction.raw.disp.offset;
            field.Size = static_cast<std::uint8_t>(instruction.raw.disp.size / 8);
            field.Kind = FieldKind::Displacement;
            field.Value = read(field.Offset, field.Size);
            const bool modrm = (instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0;
            const bool sib = (instruction.attributes & ZYDIS_ATTRIB_HAS_SIB) != 0;
            if (!modrm)
                field.Absolute = true;
            else if (instruction.raw.modrm.mod == 0 && instruction.raw.modrm.rm == 5 && instruction.address_width != 16)
            {
                field.RipRelative = m_is64Bit;
                field.Absolute = !m_is64Bit;
            }
            else if (sib && instruction.raw.modrm.mod == 0 && instruction.raw.sib.base == 5)
                field.Absolute = true;
            layout.Fields.push_back(field);
        }
        for (const auto& immediate : instruction.raw.imm)
        {
            if (immediate.size == 0)
                continue;
            InstructionField field;
            field.Offset = immediate.offset;
            field.Size = static_cast<std::uint8_t>(immediate.size / 8);
            field.Kind = immediate.is_relative ? FieldKind::Relative : FieldKind::Immediate;
            field.Value = read(field.Offset, field.Size);
            layout.Fields.push_back(field);
        }
        std::sort(layout.Fields.begin(), layout.Fields.end(), [](const InstructionField& a, const InstructionField& b) { return a.Offset < b.Offset; });
        return layout;
    }

    std::optional<std::uint64_t> Disassembler::EffectiveAddress(const Instruction& instruction, const RegisterValues& registers) const
    {
        if (!instruction.Memory.Present)
            return std::nullopt;
        const MemoryOperand& memory = instruction.Memory;
        if (memory.Absolute)
            return memory.Absolute;
        std::uint64_t address = static_cast<std::uint64_t>(memory.Displacement);
        if (memory.Base != ZYDIS_REGISTER_NONE)
            address += Value(registers, static_cast<ZydisRegister>(memory.Base));
        if (memory.Index != ZYDIS_REGISTER_NONE)
            address += Value(registers, static_cast<ZydisRegister>(memory.Index)) * static_cast<std::uint64_t>(memory.Scale ? memory.Scale : 1);
        if (memory.Segment == ZYDIS_REGISTER_FS)
            address += registers.FsBase;
        else if (memory.Segment == ZYDIS_REGISTER_GS)
            address += registers.GsBase;
        if (!m_is64Bit)
            address &= 0xFFFFFFFFull;
        return address;
    }

    std::optional<std::uint64_t> Disassembler::PreviousStart(const Target::Process& process, std::uint64_t address) const
    {
        if (address < kLookBack)
            return std::nullopt;
        std::uint8_t buffer[kLookBack + 16]{};
        const std::uint64_t start = address - kLookBack;
        if (process.Read(start, buffer, sizeof(buffer)) == 0)
            return std::nullopt;
        return PreviousStart(buffer, sizeof(buffer), start, address);
    }

    std::optional<std::uint64_t> Disassembler::PreviousStart(const std::uint8_t* buffer, std::size_t size, std::uint64_t base, std::uint64_t address) const
    {
        if (address <= base || address - base > size)
            return std::nullopt;
        std::optional<std::uint64_t> best;
        int bestChain = -1;
        for (std::uint64_t from = base; from < address; ++from)
        {
            std::uint64_t at = from;
            std::uint64_t previous = 0;
            int chain = 0;
            bool valid = true;
            bool zeros = false;
            while (at < address)
            {
                const std::size_t offset = static_cast<std::size_t>(at - base);
                const std::uint8_t* bytes = buffer + offset;
                const std::size_t raw = std::min(size - offset, kWindow);
                const std::size_t length = LoneZero(bytes, raw, zeros) ? 1 : StepLength(bytes, size - offset, raw);
                if (length == 0)
                {
                    valid = false;
                    break;
                }
                zeros = ZeroPair(bytes, length);
                previous = at;
                at += length;
                ++chain;
            }
            if (valid && at == address && chain > bestChain)
            {
                bestChain = chain;
                best = previous;
            }
        }
        return best;
    }

    std::uint64_t Disassembler::BackUp(const Target::Process& process, std::uint64_t address, int lines, const CodeHints* hints) const
    {
        std::uint64_t at = address;
        while (lines > 0 && at > 0)
        {
            // Only unreadable memory ends the listing: an allocation with nothing
            // mapped in front of it (fresh injection memory) still starts at its
            // first byte, while the readable neighbour of a code chunk or section
            // stays reachable with the functions around it.
            if (!process.IsReadable(at - 1))
                break;
            if (hints)
            {
                // From a known start the instructions are decoded, not guessed:
                // inside a function from its first byte, in padding from the end
                // of the function before it.
                if (const auto anchor = hints->StartBelow(at, kSweepReach); anchor && *anchor < at && at - *anchor <= kSweepReach)
                {
                    const std::uint64_t from = InlineHolder(process, *anchor).value_or(*anchor);
                    const std::vector<std::uint64_t> starts = Starts(process, from, at, hints);
                    if (starts.size() >= static_cast<std::size_t>(lines))
                        return starts[starts.size() - static_cast<std::size_t>(lines)];
                    lines -= static_cast<int>(starts.size());
                    at = from;
                    continue;
                }
            }
            const auto previous = PreviousStart(process, at);
            at = previous ? *previous : at - 1;
            --lines;
        }
        return at;
    }

    std::string Disassembler::RegisterName(int zydisRegister)
    {
        const char* name = ZydisRegisterGetString(static_cast<ZydisRegister>(zydisRegister));
        return name ? name : "";
    }

    std::vector<std::string> RegisterLines(const RegisterValues& r, bool is64Bit)
    {
        // ∑ The watch window wrote RAX ... RIP for a 32-bit game too (Terraria, owner report).
        const int digits = is64Bit ? 16 : 8;
        const char p = is64Bit ? 'R' : 'E';
        const auto h = [&](std::uint64_t value) { return Hex(is64Bit ? value : value & 0xFFFFFFFFull, digits); };
        std::vector<std::string> lines;
        lines.push_back(Format("%cAX %s  %cBX %s  %cCX %s  %cDX %s", p, h(r.Rax).c_str(), p, h(r.Rbx).c_str(), p, h(r.Rcx).c_str(), p, h(r.Rdx).c_str()));
        lines.push_back(Format("%cSI %s  %cDI %s  %cBP %s  %cSP %s", p, h(r.Rsi).c_str(), p, h(r.Rdi).c_str(), p, h(r.Rbp).c_str(), p, h(r.Rsp).c_str()));
        if (is64Bit)
        {
            lines.push_back(Format("R8  %s  R9  %s  R10 %s  R11 %s", h(r.R8).c_str(), h(r.R9).c_str(), h(r.R10).c_str(), h(r.R11).c_str()));
            lines.push_back(Format("R12 %s  R13 %s  R14 %s  R15 %s", h(r.R12).c_str(), h(r.R13).c_str(), h(r.R14).c_str(), h(r.R15).c_str()));
        }
        lines.push_back(Format("%cIP %s", p, h(r.Rip).c_str()));
        return lines;
    }

    namespace
    {
        std::string ByteGroup(const std::uint8_t* bytes, std::size_t from, std::size_t to)
        {
            std::string text;
            for (std::size_t i = from; i < to; ++i)
                text += Hex(bytes[i], 2);
            return text;
        }

        std::string SignedHex(std::int64_t value, int digits)
        {
            return value < 0 ? "-" + Hex(static_cast<std::uint64_t>(-value), digits) : "+" + Hex(static_cast<std::uint64_t>(value), digits);
        }

        // Cheat Engine's names, from its own tables (owner's capture, a 64-bit and a 32-bit game): each condition family
        // has its own list, so a short jump is "jna"/"jle" and a near one "jbe"/"jng", and cmov 4A is "cmovpe" next to "setp".
        std::string CheatEngineMnemonic(const ZydisDecodedInstruction& instruction)
        {
            static const char* const kShortJumps[16] = { "jo", "jno", "jb", "jae", "je", "jne", "jna", "ja", "js", "jns", "jp", "jnp", "jl", "jnl", "jle", "jg" };
            static const char* const kNearJumps[16] = { "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jnl", "jng", "jg" };
            static const char* const kSets[16] = { "seto", "setno", "setb", "setae", "sete", "setne", "setbe", "seta", "sets", "setns", "setp", "setnp", "setl", "setge", "setle", "setg" };
            static const char* const kMoves[16] = { "cmovo", "cmovno", "cmovb", "cmovae", "cmove", "cmovne", "cmovbe", "cmova", "cmovs", "cmovns", "cmovpe", "cmovnp", "cmovl", "cmovge", "cmovle", "cmovg" };
            const ZyanU8 code = instruction.opcode;
            if (instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY)
            {
                if (instruction.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT && code >= 0x70 && code <= 0x7F)
                    return kShortJumps[code - 0x70];
                if (instruction.opcode_map == ZYDIS_OPCODE_MAP_0F && code >= 0x80 && code <= 0x8F)
                    return kNearJumps[code - 0x80];
                if (instruction.opcode_map == ZYDIS_OPCODE_MAP_0F && code >= 0x90 && code <= 0x9F)
                    return kSets[code - 0x90];
                if (instruction.opcode_map == ZYDIS_OPCODE_MAP_0F && code >= 0x40 && code <= 0x4F)
                    return kMoves[code - 0x40];
            }
            switch (instruction.mnemonic)
            {
            case ZYDIS_MNEMONIC_INT3: return "int 3";
            case ZYDIS_MNEMONIC_JRCXZ: return "jecxz";
            case ZYDIS_MNEMONIC_MOVSXD: return "movsxd ";
            case ZYDIS_MNEMONIC_XLAT: return "xlatb";
            default: return ZydisMnemonicGetString(instruction.mnemonic);
            }
        }

        const char* SizeKeyword(ZyanU16 bits)
        {
            switch (bits)
            {
            case 8: return "byte ptr ";
            case 16: return "word ptr ";
            case 32: return "dword ptr ";
            case 64: return "qword ptr ";
            case 80: return "tword ptr ";
            case 128: return "xmmword ptr ";
            case 256: return "ymmword ptr ";
            case 512: return "zmmword ptr ";
            default: return "";
            }
        }
    }

    // ∑ Built to the character from Cheat Engine 7.6 output the owner captured: 18 reference lines, then a second
    //   capture with about 140 test instructions and 12,500 lines of real code in a 64-bit and a 32-bit game
    //   (DisasmTests LuaTextLikeCheatEngine and CheatEngineCapture). CE's own decoding faults are NOT copied (owner's
    //   decision, issue #1): missing or doubled commas (pshufd xmm0,xmm01B, pslld xmm4,,0D), AVX-512 shown as
    //   "bound", wrong VEX names and byte groups, 32-bit-only opcodes in 64-bit code (push es, popad), "db -01".
    std::string Disassembler::CheatEngineLine(std::uint64_t address, const std::uint8_t* bytes, std::size_t available) const
    {
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        if (available == 0)
            return Hex(address, 8) + " - ?? - ??";
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&m_state->Decoder, bytes, available, &instruction, operands)))
            return Hex(address, 8) + " - " + Hex(bytes[0], 2) + " - db " + Hex(bytes[0], 2);
        const ZydisDecodedInstructionRaw& raw = instruction.raw;

        // Bytes: each prefix alone, the opcode bytes together, then ModRM, SIB, displacement and each immediate
        // as one group. A ModRM instruction without an immediate keeps a space at the end ("05 6EA80401  - ").
        std::vector<std::string> groups;
        std::size_t opcodeStart = raw.prefix_count;
        for (std::size_t i = 0; i < raw.prefix_count; ++i)
            groups.push_back(Hex(bytes[i], 2));
        std::size_t encodedStart = 0;
        std::size_t encodedSize = 0;
        switch (instruction.encoding)
        {
        case ZYDIS_INSTRUCTION_ENCODING_VEX: encodedStart = raw.vex.offset; encodedSize = raw.vex.size; break;
        case ZYDIS_INSTRUCTION_ENCODING_EVEX: encodedStart = raw.evex.offset; encodedSize = 4; break;
        case ZYDIS_INSTRUCTION_ENCODING_MVEX: encodedStart = raw.mvex.offset; encodedSize = 4; break;
        case ZYDIS_INSTRUCTION_ENCODING_XOP: encodedStart = raw.xop.offset; encodedSize = 3; break;
        default: break;
        }
        if (encodedSize)
        {
            groups.push_back(ByteGroup(bytes, encodedStart, encodedStart + encodedSize));
            opcodeStart = encodedStart + encodedSize;
        }
        const bool legacyDefault = instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY && instruction.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT;
        // An x87 register form (D9 E8 fld1) is one opcode group in CE: "D9E8 - fld1 ".
        const bool x87Register = legacyDefault && instruction.opcode >= 0xD8 && instruction.opcode <= 0xDF &&
                                 (instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 && raw.modrm.mod == 3;
        const bool modrm = (instruction.attributes & ZYDIS_ATTRIB_HAS_MODRM) != 0 && !x87Register;
        const bool sib = (instruction.attributes & ZYDIS_ATTRIB_HAS_SIB) != 0;
        std::size_t opcodeEnd = instruction.length;
        if (x87Register)
            opcodeEnd = raw.modrm.offset + 1u;
        else if (modrm)
            opcodeEnd = raw.modrm.offset;
        else if (raw.disp.size)
            opcodeEnd = raw.disp.offset;
        else if (raw.imm[0].size)
            opcodeEnd = raw.imm[0].offset;
        if (opcodeEnd > opcodeStart)
            groups.push_back(ByteGroup(bytes, opcodeStart, opcodeEnd));
        if (modrm)
            groups.push_back(Hex(bytes[raw.modrm.offset], 2));
        if (sib)
            groups.push_back(Hex(bytes[raw.sib.offset], 2));
        if (raw.disp.size)
            groups.push_back(ByteGroup(bytes, raw.disp.offset, raw.disp.offset + raw.disp.size / 8u));
        else if (sib)
            groups.push_back("");  // CE keeps the empty displacement after a SIB byte: "48 89 3C 24   - ", "66 83 3C 48  00"
        bool immediate = false;
        for (const auto& imm : raw.imm)
        {
            if (!imm.size)
                continue;
            groups.push_back(ByteGroup(bytes, imm.offset, imm.offset + imm.size / 8u));
            immediate = true;
        }
        std::string column;
        for (const std::string& group : groups)
            column += (column.empty() ? "" : " ") + group;
        if (modrm && !immediate)
            column += " ";

        // Parameters.
        // Prefixes as CE writes them, from the bytes: every segment prefix but ds shows on the memory operand, also the ones
        // a 64-bit CPU ignores ("nop word ptr cs:[...]"); F3 is repe and F2 repne in front of any instruction unless
        // the byte belongs to the opcode, as in movss ("repe dec eax", "repne add [rax+63],ecx").
        const char* segment = nullptr;
        bool lockPrefix = false;
        bool repePrefix = false;
        bool repnePrefix = false;
        for (std::size_t i = 0; i < raw.prefix_count; ++i)
        {
            const bool mandatory = raw.prefixes[i].type == ZYDIS_PREFIX_TYPE_MANDATORY;
            switch (raw.prefixes[i].value)
            {
            case 0x26: segment = "es:"; break;
            case 0x2E: segment = "cs:"; break;
            case 0x36: segment = "ss:"; break;
            case 0x3E: break;  // ds is the default segment, which CE does not name: "add [rcx-37],bl"
            case 0x64: segment = "fs:"; break;
            case 0x65: segment = "gs:"; break;
            case 0xF0: lockPrefix = true; break;
            case 0xF3: repePrefix = repePrefix || !mandatory; break;
            case 0xF2: repnePrefix = repnePrefix || !mandatory; break;
            default: break;
            }
        }
        // On string instructions Zydis counts F3/F2 as part of the opcode; CE writes them as repe/repne there too.
        repePrefix = repePrefix || (instruction.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE)) != 0;
        repnePrefix = repnePrefix || (instruction.attributes & ZYDIS_ATTRIB_HAS_REPNE) != 0;
        const bool nop = instruction.mnemonic == ZYDIS_MNEMONIC_NOP;
        const ZydisMnemonic mnemonic = instruction.mnemonic;
        // The count register of a shift sets no size: "shl byte ptr [rdx-1D],cl".
        const bool shift = mnemonic == ZYDIS_MNEMONIC_ROL || mnemonic == ZYDIS_MNEMONIC_ROR || mnemonic == ZYDIS_MNEMONIC_RCL || mnemonic == ZYDIS_MNEMONIC_RCR ||
                           mnemonic == ZYDIS_MNEMONIC_SHL || mnemonic == ZYDIS_MNEMONIC_SHR || mnemonic == ZYDIS_MNEMONIC_SAR;
        bool registerShown = false;
        for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
            registerShown = registerShown || (operands[i].type == ZYDIS_OPERAND_TYPE_REGISTER && !nop && !shift);
        // A register sets the size, except for a zero or sign extension. Without one CE writes byte, word and qword
        // but leaves dword out ("mov [rcx],00000001", "rol [rcx],48", "lock inc [rcx]"), except for the 83 group,
        // float loads and stores (D8/D9), call, jmp, nop and movsxd: "cmp dword ptr [rbp-08],-80", "nop dword ptr [eax]";
        // push and pop leave it out too: "push [ebp-000266B4]".
        const bool extends = mnemonic == ZYDIS_MNEMONIC_MOVZX || mnemonic == ZYDIS_MNEMONIC_MOVSX || mnemonic == ZYDIS_MNEMONIC_MOVSXD;
        const bool keepDword = (legacyDefault && (instruction.opcode == 0x83 || instruction.opcode == 0xD8 || instruction.opcode == 0xD9 || instruction.opcode == 0xDB)) ||
                               mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_JMP || mnemonic == ZYDIS_MNEMONIC_NOP ||
                               mnemonic == ZYDIS_MNEMONIC_MOVSXD;
        std::vector<std::string> parameters;
        std::size_t immIndex = 0;
        for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
        {
            const ZydisDecodedOperand& operand = operands[i];
            switch (operand.type)
            {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                if (nop)
                    break;
                if (ZydisRegisterGetClass(operand.reg.value) == ZYDIS_REGCLASS_X87)
                    parameters.push_back("st(" + std::to_string(operand.reg.value - ZYDIS_REGISTER_ST0) + ")");
                else
                    parameters.push_back(ZydisRegisterGetString(operand.reg.value));
                break;
            case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            {
                const std::size_t index = std::min<std::size_t>(immIndex++, 1);
                if (operand.imm.is_relative)
                {
                    ZyanU64 target = 0;
                    ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target);
                    parameters.push_back(Hex(target, 8));
                    break;
                }
                const auto& imm = raw.imm[index];
                const std::uint64_t value = imm.value.u;
                switch (imm.size)
                {
                case 8:
                {
                    const auto small = static_cast<std::int8_t>(value & 0xFF);
                    // CE writes every two-digit number signed (inttohexs(value,2)), not only the sign-extended ones:
                    // "rol ebp,-2C", "int -3B".
                    parameters.push_back(small < 0 ? "-" + Hex(static_cast<std::uint64_t>(-static_cast<int>(small)), 2) : Hex(value & 0xFF, 2));
                    break;
                }
                case 16: parameters.push_back(Hex(value & 0xFFFF, 4)); break;
                case 32:
                    // A 64-bit operation sign-extends its imm32, and CE shows the extended value: "mov rax,FFFFFFFFFFFFFFFF",
                    // but "mov qword ptr [rcx],00000001".
                    if (imm.is_signed && instruction.operand_width == 64)
                        parameters.push_back(Hex(static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value & 0xFFFFFFFFull))), 8));
                    else
                        parameters.push_back(Hex(value & 0xFFFFFFFFull, 8));
                    break;
                case 64: parameters.push_back(Hex(value, 16)); break;
                default: parameters.push_back(Hex(operand.imm.value.u)); break;
                }
                break;
            }
            case ZYDIS_OPERAND_TYPE_MEMORY:
            {
                std::string text;
                if (operand.mem.type == ZYDIS_MEMOP_TYPE_MEM && (extends || !registerShown) && (operand.size != 32 || keepDword))
                    text += SizeKeyword(operand.size);
                if (segment)
                    text += segment;
                const bool ripRelative = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
                if (ripRelative || (operand.mem.base == ZYDIS_REGISTER_NONE && operand.mem.index == ZYDIS_REGISTER_NONE))
                {
                    ZyanU64 target = 0;
                    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
                        target = static_cast<ZyanU64>(operand.mem.disp.value);
                    text += "[" + Hex(target, 8) + "]";
                }
                else
                {
                    std::string inside;
                    if (operand.mem.base != ZYDIS_REGISTER_NONE)
                        inside += ZydisRegisterGetString(operand.mem.base);
                    if (operand.mem.index != ZYDIS_REGISTER_NONE)
                    {
                        inside += (inside.empty() ? "" : "+") + std::string(ZydisRegisterGetString(operand.mem.index));
                        if (operand.mem.scale > 1)
                            inside += "*" + std::to_string(operand.mem.scale);
                    }
                    if (raw.disp.size)
                        inside += SignedHex(operand.mem.disp.value, raw.disp.size == 8 ? 2 : raw.disp.size == 16 ? 4 : 8);
                    text += "[" + inside + "]";
                }
                parameters.push_back(text);
                break;
            }
            case ZYDIS_OPERAND_TYPE_POINTER:
                parameters.push_back(Hex(operand.ptr.segment, 4) + ":" + Hex(operand.ptr.offset, 8));
                break;
            default:
                break;
            }
        }
        std::string opcode;
        if (lockPrefix)
            opcode += "lock ";
        if (repePrefix)
            opcode += "repe ";  // CE writes repe for every F3: "repe stosd", "repe movsb"
        if (repnePrefix)
            opcode += "repne ";
        opcode += CheatEngineMnemonic(instruction);
        if (legacyDefault && instruction.mnemonic == ZYDIS_MNEMONIC_XCHG && instruction.opcode >= 0x91 && instruction.opcode <= 0x97 && parameters.size() == 2)
            std::swap(parameters[0], parameters[1]);  // CE names the accumulator first: "xchg eax,ecx"
        std::string joined;
        for (const std::string& parameter : parameters)
            joined += (joined.empty() ? "" : ",") + parameter;
        return Hex(address, 8) + " - " + column + " - " + opcode + " " + joined;
    }
}
