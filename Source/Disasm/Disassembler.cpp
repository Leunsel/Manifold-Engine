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
            for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
            {
                if (operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[i].imm.is_relative && result.Target)
                    replace(*result.Target);
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

    std::vector<Instruction> Disassembler::ReadMany(const Target::Process& process, std::uint64_t address, int count, const Target::Symbols* symbols) const
    {
        if (count <= 0)
            return {};
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(count) * 15 + 16);
        std::vector<bool> readable(buffer.size(), false);
        for (std::size_t page = 0; page < buffer.size();)
        {
            const std::uint64_t at = address + page;
            const std::size_t chunk = std::min<std::size_t>(buffer.size() - page, 0x1000 - static_cast<std::size_t>(at & 0xFFF));
            if (process.ReadExact(at, buffer.data() + page, chunk))
                std::fill(readable.begin() + static_cast<std::ptrdiff_t>(page), readable.begin() + static_cast<std::ptrdiff_t>(page + chunk), true);
            page += chunk;
        }
        return DecodeMany(buffer, readable, address, count, symbols);
    }

    std::vector<Instruction> Disassembler::DecodeMany(const std::vector<std::uint8_t>& buffer, const std::vector<bool>& readable, std::uint64_t address, int count,
        const Target::Symbols* symbols) const
    {
        std::vector<Instruction> list;
        list.reserve(static_cast<std::size_t>(std::max(0, count)));
        std::size_t offset = 0;
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
                continue;
            }
            std::size_t available = 0;
            while (offset + available < buffer.size() && readable[offset + available] && available < 16)
                ++available;
            Instruction instruction = Decode(at, buffer.data() + offset, available, symbols);
            offset += instruction.Length ? instruction.Length : 1;
            list.push_back(std::move(instruction));
        }
        return list;
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
            while (at < address)
            {
                ZydisDecodedInstruction instruction{};
                const std::size_t offset = static_cast<std::size_t>(at - base);
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&m_state->Decoder, nullptr, buffer + offset, size - offset, &instruction)))
                {
                    valid = false;
                    break;
                }
                previous = at;
                at += instruction.length;
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

    std::uint64_t Disassembler::BackUp(const Target::Process& process, std::uint64_t address, int lines) const
    {
        const auto region = process.RegionAt(address);
        const std::uint64_t floor = region ? region->Base : 0;
        std::uint64_t at = address;
        for (int i = 0; i < lines && at > floor; ++i)
        {
            const auto previous = PreviousStart(process, at);
            const std::uint64_t next = previous ? *previous : (at > 0 ? at - 1 : at);
            if (next < floor)
            {
                at = floor;
                break;
            }
            at = next;
        }
        return at;
    }

    std::string Disassembler::RegisterName(int zydisRegister)
    {
        const char* name = ZydisRegisterGetString(static_cast<ZydisRegister>(zydisRegister));
        return name ? name : "";
    }
}
