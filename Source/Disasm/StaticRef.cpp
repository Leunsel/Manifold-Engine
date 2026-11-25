#include "Disasm/StaticRef.h"

#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Target/Process.h"

#include "Zydis.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Manifold::Disasm
{
    namespace
    {
        // ∑ Below 64 KB Windows maps nothing a program can name, and a number up there is a
        //   count or a flag, not an address, even when page 0x10000 happens to be mapped.
        constexpr std::uint64_t kLowestAddress = 0x10000;

        struct Candidate
        {
            StaticKind Kind = StaticKind::Memory;
            bool Relative = false;
            bool Indexed = false;
            std::uint8_t FieldOffset = 0;
            std::uint8_t FieldSize = 0;
            std::int64_t FieldValue = 0;
            int OperandSize = 0;
            std::uint64_t Address = 0;
        };

        std::string Normalize(std::string_view word)
        {
            std::string text;
            for (const char c : word)
            {
                if (c == ' ' || c == '\t' || c == '_' || c == '-' || c == '.' || c == '/')
                    continue;
                text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
                text = text.substr(1, text.size() - 2);
            return text;
        }

        bool OneOf(const std::string& text, std::initializer_list<std::string_view> words)
        {
            return std::find(words.begin(), words.end(), text) != words.end();
        }

        // ∑ Decimal like the table-file command it replaces (Lua tonumber), and like the sizes
        //   of readmem and alloc: these are byte counts inside one instruction. $ and 0x are hex,
        //   # is decimal, and a word with hex letters is hex.
        std::optional<std::uint64_t> SmallNumber(const std::string& text)
        {
            if (text.empty())
                return std::nullopt;
            int base = 10;
            std::string digits = text;
            if (digits.starts_with("0x"))
            {
                base = 16;
                digits = digits.substr(2);
            }
            else if (digits.starts_with('$'))
            {
                base = 16;
                digits = digits.substr(1);
            }
            else if (digits.starts_with('#'))
                digits = digits.substr(1);
            else if (std::all_of(digits.begin(), digits.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }) &&
                     !std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
                base = 16;
            if (digits.empty() || digits.size() > 8)
                return std::nullopt;
            std::uint64_t value = 0;
            for (const char c : digits)
            {
                const int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::isxdigit(static_cast<unsigned char>(c)) ? std::tolower(c) - 'a' + 10 : 99);
                if (digit >= base)
                    return std::nullopt;
                value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
            }
            return value;
        }

        std::string Place(std::uint64_t address, const std::string& text)
        {
            return Hex(address) + (text.empty() ? std::string() : " (" + text + ")");
        }

        std::string FieldList(const std::vector<Candidate>& candidates)
        {
            std::vector<std::string> parts;
            for (const Candidate& candidate : candidates)
                parts.push_back(std::string(StaticKindName(candidate.Kind)) + " at byte " + std::to_string(candidate.FieldOffset));
            return parts.empty() ? std::string("none") : Join(parts, ", ");
        }

        const char* RegisterText(ZydisRegister reg)
        {
            const char* text = ZydisRegisterGetString(reg);
            return text ? text : "?";
        }
    }

    std::string_view StaticKindName(StaticKind kind)
    {
        switch (kind)
        {
        case StaticKind::Auto: return "auto";
        case StaticKind::Memory: return "memory";
        case StaticKind::Branch: return "branch";
        case StaticKind::Immediate: return "immediate";
        }
        return "auto";
    }

    bool ParseStaticOption(std::string_view word, StaticOptions& options, std::string& error)
    {
        const std::string text = Normalize(word);
        if (text.empty())
            return true;
        const auto setKind = [&](StaticKind kind) {
            if (options.Kind != StaticKind::Auto && options.Kind != kind)
            {
                error = "two kinds given (" + std::string(StaticKindName(options.Kind)) + " and " + std::string(StaticKindName(kind)) + ")";
                return false;
            }
            options.Kind = kind;
            return true;
        };
        if (text == "auto")
            return true;
        if (OneOf(text, { "memory", "mem", "operand", "memoryoperand" }))
            return setKind(StaticKind::Memory);
        if (OneOf(text, { "branch", "call", "jump", "jmp", "jcc" }))
            return setKind(StaticKind::Branch);
        if (OneOf(text, { "immediate", "imm", "constant", "offset", "imm32", "imm64" }))
            return setKind(StaticKind::Immediate);
        const auto setEncoding = [&](StaticEncoding encoding) {
            if (options.Encoding != StaticEncoding::Auto && options.Encoding != encoding)
            {
                error = "both relative and absolute given";
                return false;
            }
            options.Encoding = encoding;
            return true;
        };
        if (OneOf(text, { "rip", "riprelative", "relative", "rel", "rel32", "disp32" }))
            return setEncoding(StaticEncoding::Relative);
        if (OneOf(text, { "absolute", "abs", "abs32", "x86", "moffs", "moffs32" }))
            return setEncoding(StaticEncoding::Absolute);
        if (OneOf(text, { "address", "addr", "operandaddress", "slot", "static", "raw" }))
        {
            if (options.Output == StaticOutput::Pointer)
            {
                error = "both address and pointer given";
                return false;
            }
            return true;
        }
        if (OneOf(text, { "pointer", "ptr", "value", "deref", "dereference", "target" }))
        {
            options.Output = StaticOutput::Pointer;
            return true;
        }
        if (OneOf(text, { "first", "firsthit", "notunique" }))
        {
            options.FirstHit = true;
            return true;
        }
        if (const auto number = SmallNumber(text))
        {
            if (!options.FieldOffset)
            {
                if (*number > 14)
                {
                    error = "the field offset " + std::to_string(*number) + " lies beyond the longest instruction (15 bytes)";
                    return false;
                }
                options.FieldOffset = number;
                return true;
            }
            if (!options.Length)
            {
                if (*number == 0 || *number > 15)
                {
                    error = "the instruction length must be 1 to 15, not " + std::to_string(*number);
                    return false;
                }
                options.Length = number;
                return true;
            }
            error = "a third number (" + std::string(word) + "); only the field offset and the instruction length are numbers";
            return false;
        }
        error = "unknown option \"" + std::string(Trim(word)) + "\" (memory, branch, immediate, rip, absolute, address, pointer, first, or the field offset and length)";
        return false;
    }

    Result<StaticRef> ResolveStaticBytes(bool is64Bit, std::uint64_t address, const std::uint8_t* bytes, std::size_t size, const StaticOptions& options,
        const std::function<bool(std::uint64_t)>& mapped, const std::function<bool(std::uint64_t)>& image,
        const std::function<std::optional<std::uint64_t>(std::uint64_t)>& readPointer)
    {
        if (size == 0)
            return Status::Fail(ErrorCode::StaticUnreadable, Hex(address));
        ZydisDecoder decoder{};
        ZydisDecoderInit(&decoder, is64Bit ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32, is64Bit ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        const std::size_t available = std::min<std::size_t>(size, ZYDIS_MAX_INSTRUCTION_LENGTH);
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, available, &instruction, operands)))
        {
            const std::string what = HexBytes(std::span<const std::uint8_t>(bytes, std::min<std::size_t>(available, 8)));
            return Status::Fail(ErrorCode::StaticUnreadable, Hex(address) + " holds " + what + ", which is no instruction" +
                                                             (available < ZYDIS_MAX_INSTRUCTION_LENGTH ? " (only " + std::to_string(available) + " bytes readable)" : ""));
        }
        const Disassembler disassembler(is64Bit);
        const std::string text = disassembler.Decode(address, bytes, available).Text();
        const std::string where = Place(address, text);
        const std::uint64_t next = address + instruction.length;
        const std::uint64_t mask = is64Bit ? ~0ull : 0xFFFFFFFFull;

        std::vector<Candidate> memory;
        std::vector<Candidate> branches;
        std::vector<Candidate> immediates;
        std::vector<std::string> rejected;
        int immediateIndex = 0;
        for (ZyanU8 i = 0; i < instruction.operand_count_visible; ++i)
        {
            const ZydisDecodedOperand& operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY)
            {
                if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM && operand.mem.type != ZYDIS_MEMOP_TYPE_AGEN)
                    continue;
                const bool rip = operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
                const bool noBase = operand.mem.base == ZYDIS_REGISTER_NONE;
                const bool noIndex = operand.mem.index == ZYDIS_REGISTER_NONE;
                // ∑ fs: and gs: count from the thread's own block (TEB, TLS), so [gs:30] is a
                //   different place in every thread, never a static.
                if (operand.mem.segment == ZYDIS_REGISTER_FS || operand.mem.segment == ZYDIS_REGISTER_GS)
                {
                    rejected.push_back(std::string("its memory operand counts from ") + RegisterText(operand.mem.segment) + ", the thread block, which differs per thread");
                    continue;
                }
                // ∑ lea computes numbers too: lea eax,[ecx*4+10] is arithmetic, not a table.
                //   An index without a base is a table only when the operand is read or written.
                const bool table = noBase && !noIndex && operand.mem.type == ZYDIS_MEMOP_TYPE_MEM && instruction.raw.disp.size >= 32;
                if (!rip && !(noBase && noIndex) && !table)
                {
                    std::string reg = noBase ? RegisterText(operand.mem.index) : RegisterText(operand.mem.base);
                    rejected.push_back("its memory operand counts from the register " + reg + ", so the address is known only while the code runs");
                    continue;
                }
                Candidate candidate;
                candidate.Kind = StaticKind::Memory;
                candidate.Relative = rip;
                candidate.Indexed = table;
                candidate.FieldOffset = instruction.raw.disp.offset;
                candidate.FieldSize = static_cast<std::uint8_t>(instruction.raw.disp.size / 8);
                candidate.FieldValue = instruction.raw.disp.value;
                candidate.OperandSize = operand.size / 8;
                if (table)
                    candidate.Address = static_cast<std::uint64_t>(operand.mem.disp.value) & mask;
                else
                {
                    ZyanU64 target = 0;
                    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
                        continue;
                    candidate.Address = target & mask;
                }
                memory.push_back(candidate);
            }
            else if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            {
                const int index = immediateIndex++;
                const auto& raw = instruction.raw.imm[std::min(index, 1)];
                if (operand.imm.is_relative)
                {
                    ZyanU64 target = 0;
                    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, address, &target)))
                        continue;
                    Candidate candidate;
                    candidate.Kind = StaticKind::Branch;
                    candidate.Relative = true;
                    candidate.FieldOffset = raw.offset;
                    candidate.FieldSize = static_cast<std::uint8_t>(raw.size / 8);
                    candidate.FieldValue = raw.value.s;
                    candidate.Address = target & mask;
                    branches.push_back(candidate);
                    continue;
                }
                // ∑ An address needs at least 32 bits; an imm8 or imm16 (push 10, add eax,1) never is one.
                if (raw.size < 32)
                    continue;
                const int bits = operand.size ? std::min<int>(operand.size, 64) : 64;
                std::uint64_t value = operand.imm.value.u;
                if (bits < 64)
                    value &= (1ull << bits) - 1;
                Candidate candidate;
                candidate.Kind = StaticKind::Immediate;
                candidate.FieldOffset = raw.offset;
                candidate.FieldSize = static_cast<std::uint8_t>(raw.size / 8);
                candidate.FieldValue = raw.value.s;
                candidate.OperandSize = operand.size / 8;
                candidate.Address = value & mask;
                immediates.push_back(candidate);
            }
        }

        // ∑ The field offset of the older command picks the number it names; a number that is
        //   no field of this instruction is the mistake that command made silently (byte 3 of
        //   mov [rip+x],imm32 is the displacement, byte 7 the immediate).
        std::vector<Candidate> all;
        all.insert(all.end(), memory.begin(), memory.end());
        all.insert(all.end(), branches.begin(), branches.end());
        all.insert(all.end(), immediates.begin(), immediates.end());
        if (options.Length && *options.Length != instruction.length)
            return Status::Fail(ErrorCode::StaticMismatch, where + " is " + std::to_string(instruction.length) + " bytes long, not " + std::to_string(*options.Length) +
                                                           "; drop the length, the instruction is decoded");

        std::vector<Candidate> chosen;
        if (options.FieldOffset)
        {
            for (const Candidate& candidate : all)
            {
                if (candidate.FieldOffset == *options.FieldOffset && (options.Kind == StaticKind::Auto || candidate.Kind == options.Kind))
                    chosen.push_back(candidate);
            }
            if (chosen.empty())
                return Status::Fail(ErrorCode::StaticMismatch, where + " has no address field at byte " + std::to_string(*options.FieldOffset) + "; its fields: " + FieldList(all));
        }
        else if (options.Kind == StaticKind::Memory)
            chosen = memory;
        else if (options.Kind == StaticKind::Branch)
            chosen = branches;
        else if (options.Kind == StaticKind::Immediate)
            chosen = immediates;
        else if (!memory.empty())
            chosen = memory;
        else if (!branches.empty())
            chosen = branches;
        else
        {
            // ∑ Auto takes a number only when it lands in a module: mov eax,10000 is a count,
            //   mov ecx,offset table (x86) or mov rax,imm64 into the image an address.
            for (const Candidate& candidate : immediates)
            {
                if (candidate.Address >= kLowestAddress && image && image(candidate.Address))
                    chosen.push_back(candidate);
            }
            if (chosen.empty() && !immediates.empty())
                rejected.push_back("its number " + Hex(immediates.front().Address) + " points into no module (add \"immediate\" to take it anyway)");
        }

        if (chosen.empty())
        {
            std::string why;
            if (options.Kind != StaticKind::Auto)
                why = "it has no " + std::string(StaticKindName(options.Kind)) + " address" + (all.empty() ? "" : "; its fields: " + FieldList(all));
            else if (!rejected.empty())
                why = rejected.front();
            else if (instruction.operand_count_visible == 0)
                why = "it has no operands";
            else
                why = "it has no memory operand, branch target or address-sized number";
            return Status::Fail(ErrorCode::StaticNoAddress, where + ": " + why);
        }
        if (chosen.size() > 1)
            return Status::Fail(ErrorCode::StaticAmbiguous, where + ": " + FieldList(chosen) + "; give the field offset to pick one");
        const Candidate pick = chosen.front();

        const bool relative = pick.Relative;
        if (options.Encoding == StaticEncoding::Relative && !relative)
            return Status::Fail(ErrorCode::StaticMismatch, where + " names " + Hex(pick.Address) + " absolutely, not relative to the next instruction; drop \"rip\"");
        if (options.Encoding == StaticEncoding::Absolute && relative)
            return Status::Fail(ErrorCode::StaticMismatch, where + " names " + Hex(pick.Address) + " relative to the next instruction, not absolutely; drop \"absolute\"");

        // ∑ A script's own hook in the place: jmp rel32 into allocated memory, or the 14-byte
        //   jmp [rip+0] relay with the target right behind it. That happens when the script ran
        //   before and its [DISABLE] part did not, and the "static" would be the hook's cave.
        const bool unconditionalJump = instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
        if (unconditionalJump && options.Kind == StaticKind::Auto)
        {
            std::optional<std::uint64_t> destination;
            if (pick.Kind == StaticKind::Branch)
                destination = pick.Address;
            else if (pick.Kind == StaticKind::Memory && pick.Relative && pick.Address == next && readPointer)
                destination = readPointer(pick.Address);
            if (destination && image && !image(*destination))
                return Status::Fail(ErrorCode::StaticHooked, where + " jumps to " + Hex(*destination) +
                                                             ", outside every module; switch the script that hooked it off, or add \"branch\" to take the jump target");
        }

        if (pick.Address < kLowestAddress || (mapped && !mapped(pick.Address)))
            return Status::Fail(ErrorCode::StaticUnmapped, Hex(pick.Address) + " from " + where);

        StaticRef ref;
        ref.Instruction = address;
        ref.Length = instruction.length;
        ref.Text = text;
        ref.Kind = pick.Kind;
        ref.Relative = relative;
        ref.Indexed = pick.Indexed;
        ref.FieldOffset = pick.FieldOffset;
        ref.FieldSize = pick.FieldSize;
        ref.FieldValue = pick.FieldValue;
        ref.OperandSize = pick.OperandSize;
        ref.Address = pick.Address;
        if (readPointer)
            ref.Pointer = readPointer(pick.Address);
        if (pick.Indexed)
            ref.Notes.push_back("the address is the start of a table the code indexes");
        if (options.Kind == StaticKind::Auto && pick.Kind == StaticKind::Memory && !immediates.empty())
            ref.Notes.push_back("the instruction also holds the number " + Hex(immediates.front().Address) + " (add \"immediate\" to take it)");
        if (options.Output == StaticOutput::Pointer)
        {
            if (!ref.Pointer)
                return Status::Fail(ErrorCode::StaticPointerUnreadable, Hex(pick.Address) + " from " + where);
            ref.Value = *ref.Pointer;
        }
        else
            ref.Value = ref.Address;
        return ref;
    }

    Result<StaticRef> ResolveStatic(const Target::Process& process, std::uint64_t address, const StaticOptions& options)
    {
        std::array<std::uint8_t, ZYDIS_MAX_INSTRUCTION_LENGTH> bytes{};
        const std::size_t read = process.Read(address, bytes.data(), bytes.size());
        if (read == 0)
            return Status::Fail(ErrorCode::StaticUnreadable, Hex(address) + " is not readable");
        const auto mapped = [&](std::uint64_t at) {
            const auto region = process.RegionAt(at);
            return region && region->Committed();
        };
        const auto image = [&](std::uint64_t at) { return process.ModuleAt(at).has_value(); };
        const auto pointer = [&](std::uint64_t at) { return process.ReadPointer(at); };
        return ResolveStaticBytes(process.Is64Bit(), address, bytes.data(), read, options, mapped, image, pointer);
    }
}
