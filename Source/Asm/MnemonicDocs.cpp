#include "Asm/MnemonicDocs.h"

#include "Core/Text.h"

#include <unordered_map>

namespace Manifold::Asm
{
    namespace
    {
        const std::unordered_map<std::string_view, std::string_view>& Plain()
        {
            static const std::unordered_map<std::string_view, std::string_view> table = {
                { "mov", "Copies the source into the destination." },
                { "movzx", "Copies a smaller source into a larger destination and fills the upper bits with zeros." },
                { "movsx", "Copies a smaller source into a larger destination and extends its sign." },
                { "movsxd", "Copies a 32-bit source into a 64-bit register and extends its sign." },
                { "movabs", "Copies a 64-bit constant or a value at a 64-bit address." },
                { "lea", "Computes the address of the memory operand and stores the address itself, without reading memory. Often used for arithmetic." },
                { "xchg", "Swaps the two operands. With a memory operand it is atomic." },
                { "push", "Lowers the stack pointer and stores the operand on the stack." },
                { "pop", "Loads the value on top of the stack into the operand and raises the stack pointer." },
                { "pushfq", "Pushes the flags register." },
                { "popfq", "Pops the flags register." },
                { "pushad", "Pushes all 32-bit general registers." },
                { "popad", "Pops all 32-bit general registers." },
                { "add", "Adds the source to the destination." },
                { "adc", "Adds the source and the carry flag to the destination." },
                { "sub", "Subtracts the source from the destination." },
                { "sbb", "Subtracts the source and the carry flag from the destination." },
                { "inc", "Adds 1 to the operand. The carry flag stays as it is." },
                { "dec", "Subtracts 1 from the operand. The carry flag stays as it is." },
                { "neg", "Negates the operand (two's complement)." },
                { "imul", "Multiplies signed integers." },
                { "mul", "Multiplies unsigned integers; the result goes to rdx:rax (or edx:eax, dx:ax, ax)." },
                { "idiv", "Divides signed integers: rdx:rax by the operand, quotient in rax, remainder in rdx." },
                { "div", "Divides unsigned integers: rdx:rax by the operand, quotient in rax, remainder in rdx." },
                { "cmp", "Subtracts the source from the destination only to set the flags; neither operand changes. Usually followed by a conditional jump." },
                { "test", "ANDs the two operands only to set the flags; neither operand changes. `test eax,eax` checks for zero." },
                { "and", "Bitwise AND of destination and source." },
                { "or", "Bitwise OR of destination and source." },
                { "xor", "Bitwise exclusive OR. `xor eax,eax` sets the register to zero." },
                { "not", "Inverts every bit of the operand." },
                { "shl", "Shifts left; the bits shifted in are zero. Multiplies by 2 per step." },
                { "sal", "Shifts left; the same as shl." },
                { "shr", "Shifts right; the bits shifted in are zero. Unsigned division by 2 per step." },
                { "sar", "Shifts right and keeps the sign. Signed division by 2 per step, rounded down." },
                { "rol", "Rotates the bits left." },
                { "ror", "Rotates the bits right." },
                { "rcl", "Rotates the bits left through the carry flag." },
                { "rcr", "Rotates the bits right through the carry flag." },
                { "shld", "Shifts the destination left and fills it with bits from the source." },
                { "shrd", "Shifts the destination right and fills it with bits from the source." },
                { "bt", "Copies one bit of the operand into the carry flag." },
                { "bts", "Copies one bit into the carry flag and sets it." },
                { "btr", "Copies one bit into the carry flag and clears it." },
                { "btc", "Copies one bit into the carry flag and inverts it." },
                { "bsf", "Finds the lowest set bit." },
                { "bsr", "Finds the highest set bit." },
                { "tzcnt", "Counts the zero bits below the lowest set bit." },
                { "lzcnt", "Counts the zero bits above the highest set bit." },
                { "popcnt", "Counts the set bits." },
                { "bswap", "Reverses the byte order of a register." },
                { "cbw", "Extends the sign of al into ax." },
                { "cwde", "Extends the sign of ax into eax." },
                { "cdqe", "Extends the sign of eax into rax." },
                { "cwd", "Extends the sign of ax into dx:ax." },
                { "cdq", "Extends the sign of eax into edx:eax, before a signed division." },
                { "cqo", "Extends the sign of rax into rdx:rax, before a signed division." },
                { "jmp", "Jumps to the target." },
                { "call", "Pushes the return address and jumps to the target." },
                { "ret", "Pops the return address and jumps there; with an operand it also removes that many bytes of arguments." },
                { "jrcxz", "Jumps if rcx is zero." },
                { "jecxz", "Jumps if ecx is zero." },
                { "loop", "Decreases rcx and jumps while it is not zero." },
                { "nop", "Does nothing. Longer forms are used as padding." },
                { "int3", "Breakpoint: raises a debug exception. Also used as padding between functions." },
                { "int", "Raises a software interrupt." },
                { "ud2", "Raises an invalid-opcode exception on purpose; marks code that must not be reached." },
                { "hlt", "Halts the processor; privileged." },
                { "syscall", "Calls the operating system kernel." },
                { "leave", "Restores the stack pointer from the frame pointer and pops the frame pointer, at the end of a function." },
                { "enter", "Sets up a stack frame." },
                { "clc", "Clears the carry flag." },
                { "stc", "Sets the carry flag." },
                { "cmc", "Inverts the carry flag." },
                { "cld", "Clears the direction flag, so string instructions count up." },
                { "std", "Sets the direction flag, so string instructions count down." },
                { "lahf", "Loads the low flags into ah." },
                { "sahf", "Stores ah into the low flags." },
                { "movs", "Copies memory from [rsi] to [rdi] and advances both; with rep, rcx times." },
                { "movsb", "Copies one byte from [rsi] to [rdi]; with rep, rcx bytes." },
                { "movsw", "Copies one word from [rsi] to [rdi]; with rep, rcx words." },
                { "movsq", "Copies one qword from [rsi] to [rdi]; with rep, rcx qwords." },
                { "stosb", "Stores al at [rdi] and advances it; with rep it fills rcx bytes." },
                { "stosw", "Stores ax at [rdi] and advances it; with rep it fills rcx words." },
                { "stosd", "Stores eax at [rdi] and advances it; with rep it fills rcx dwords." },
                { "stosq", "Stores rax at [rdi] and advances it; with rep it fills rcx qwords." },
                { "lodsb", "Loads the byte at [rsi] into al and advances rsi." },
                { "scasb", "Compares al with the byte at [rdi] and advances rdi; with repne it searches." },
                { "cmpsb", "Compares the bytes at [rsi] and [rdi] and advances both; with repe it compares rcx bytes." },
                { "cmpxchg", "Compares the accumulator with the destination; if equal, writes the source there, else loads the destination into the accumulator. With lock it is atomic." },
                { "cmpxchg8b", "Compares edx:eax with 8 bytes in memory and swaps in ecx:ebx if equal." },
                { "cmpxchg16b", "Compares rdx:rax with 16 bytes in memory and swaps in rcx:rbx if equal." },
                { "xadd", "Swaps the operands and stores their sum in the destination. With lock it is atomic." },
                { "pause", "Hints a spin-wait loop to the processor." },
                { "mfence", "Orders all earlier loads and stores before later ones." },
                { "lfence", "Orders earlier loads before later ones." },
                { "sfence", "Orders earlier stores before later ones." },
                { "cpuid", "Reports processor information in eax, ebx, ecx and edx." },
                { "rdtsc", "Reads the time-stamp counter into edx:eax." },
                { "rdtscp", "Reads the time-stamp counter into edx:eax and the processor id into ecx." },
                { "prefetcht0", "Hints that the memory will be needed soon." },
                { "prefetchnta", "Hints that the memory will be needed once." },
                { "movd", "Copies 32 bits between a general register or memory and an SSE register." },
                { "movq", "Copies 64 bits between a general register, memory and an SSE register." },
                { "movaps", "Copies 16 bytes of floats; memory must be 16-byte aligned." },
                { "movups", "Copies 16 bytes of floats; memory may be unaligned." },
                { "movapd", "Copies 16 bytes of doubles; memory must be 16-byte aligned." },
                { "movupd", "Copies 16 bytes of doubles; memory may be unaligned." },
                { "movdqa", "Copies 16 bytes of integers; memory must be 16-byte aligned." },
                { "movdqu", "Copies 16 bytes of integers; memory may be unaligned." },
                { "movss", "Copies one float (32-bit). From memory it clears the rest of the register." },
                { "movsd", "Copies one double (64-bit). From memory it clears the rest of the register. (Without operands it is the string copy movsd.)" },
                { "movlps", "Copies two floats to or from the low half of an SSE register." },
                { "movhps", "Copies two floats to or from the high half of an SSE register." },
                { "movlpd", "Copies one double to or from the low half of an SSE register." },
                { "movhpd", "Copies one double to or from the high half of an SSE register." },
                { "movhlps", "Copies the high two floats of the source into the low half of the destination." },
                { "movlhps", "Copies the low two floats of the source into the high half of the destination." },
                { "movmskps", "Copies the sign bits of four floats into a general register." },
                { "shufps", "Picks floats from both operands by an immediate pattern." },
                { "unpcklps", "Interleaves the low floats of both operands." },
                { "unpckhps", "Interleaves the high floats of both operands." },
                { "comiss", "Compares two floats and sets ZF, PF and CF like an unsigned compare; signals on any NaN." },
                { "ucomiss", "Compares two floats and sets ZF, PF and CF like an unsigned compare; PF = 1 means unordered (NaN)." },
                { "comisd", "Compares two doubles and sets ZF, PF and CF like an unsigned compare; signals on any NaN." },
                { "ucomisd", "Compares two doubles and sets ZF, PF and CF like an unsigned compare; PF = 1 means unordered (NaN)." },
                { "roundss", "Rounds one float with the rounding mode in the immediate." },
                { "roundsd", "Rounds one double with the rounding mode in the immediate." },
                { "pxor", "Bitwise exclusive OR of 128 bits. `pxor xmm0,xmm0` sets the register to zero." },
                { "por", "Bitwise OR of 128 bits." },
                { "pand", "Bitwise AND of 128 bits." },
                { "pandn", "Inverts the destination, then ANDs it with the source." },
                { "pshufd", "Picks dwords of the source by an immediate pattern." },
                { "pshufb", "Picks bytes of the destination by the indexes in the source." },
                { "pcmpeqb", "Compares bytes for equality; equal bytes become 0xFF." },
                { "pcmpeqd", "Compares dwords for equality; equal dwords become all ones." },
                { "pmovmskb", "Copies the sign bit of each byte into a general register." },
                { "punpcklbw", "Interleaves the low bytes of both operands." },
                { "fld", "Pushes a value onto the x87 floating-point stack." },
                { "fst", "Stores the top of the x87 stack." },
                { "fstp", "Stores the top of the x87 stack and pops it." },
                { "fild", "Converts an integer and pushes it onto the x87 stack." },
                { "fistp", "Stores the top of the x87 stack as an integer and pops it." },
                { "fadd", "Adds on the x87 stack." },
                { "fsub", "Subtracts on the x87 stack." },
                { "fmul", "Multiplies on the x87 stack." },
                { "fdiv", "Divides on the x87 stack." },
                { "fxch", "Swaps two x87 stack registers." },
                { "fcomip", "Compares two x87 values, sets ZF, PF and CF and pops." },
                { "fldz", "Pushes 0.0 onto the x87 stack." },
                { "fld1", "Pushes 1.0 onto the x87 stack." },
                { "vzeroupper", "Clears the upper halves of all ymm registers, to avoid a penalty when switching from AVX to SSE code." },
                { "vbroadcastss", "Copies one float into every lane." },
                { "vperm2f128", "Picks 128-bit halves from both sources." },
                { "vinsertf128", "Inserts 128 bits into a ymm register." },
                { "vextractf128", "Extracts 128 bits of a ymm register." },
                { "vfmadd213ss", "Fused multiply-add of one float: a*b + c in one rounding." },
                { "vfmadd231ss", "Fused multiply-add of one float: b*c + a in one rounding." },
                { "vfmadd132ss", "Fused multiply-add of one float: a*c + b in one rounding." },
            };
            return table;
        }

        struct Condition
        {
            std::string_view Suffix;
            std::string_view Meaning;
        };

        constexpr Condition kConditions[] = {
            { "o", "overflow (OF = 1)" }, { "no", "no overflow (OF = 0)" },
            { "b", "below, unsigned (CF = 1)" }, { "c", "carry (CF = 1)" }, { "nae", "below, unsigned (CF = 1)" },
            { "ae", "above or equal, unsigned (CF = 0)" }, { "nb", "above or equal, unsigned (CF = 0)" }, { "nc", "no carry (CF = 0)" },
            { "e", "equal (ZF = 1)" }, { "z", "zero (ZF = 1)" }, { "ne", "not equal (ZF = 0)" }, { "nz", "not zero (ZF = 0)" },
            { "be", "below or equal, unsigned (CF = 1 or ZF = 1)" }, { "na", "below or equal, unsigned (CF = 1 or ZF = 1)" },
            { "a", "above, unsigned (CF = 0 and ZF = 0)" }, { "nbe", "above, unsigned (CF = 0 and ZF = 0)" },
            { "s", "negative (SF = 1)" }, { "ns", "not negative (SF = 0)" },
            { "p", "parity even (PF = 1)" }, { "pe", "parity even (PF = 1)" }, { "np", "parity odd (PF = 0)" }, { "po", "parity odd (PF = 0)" },
            { "l", "less, signed (SF != OF)" }, { "nge", "less, signed (SF != OF)" }, { "ge", "greater or equal, signed (SF = OF)" }, { "nl", "greater or equal, signed (SF = OF)" },
            { "le", "less or equal, signed (ZF = 1 or SF != OF)" }, { "ng", "less or equal, signed (ZF = 1 or SF != OF)" },
            { "g", "greater, signed (ZF = 0 and SF = OF)" }, { "nle", "greater, signed (ZF = 0 and SF = OF)" },
        };

        std::string_view ConditionOf(std::string_view suffix)
        {
            for (const Condition& condition : kConditions)
            {
                if (condition.Suffix == suffix)
                    return condition.Meaning;
            }
            return {};
        }

        std::string_view LaneText(std::string_view suffix)
        {
            if (suffix == "ss")
                return "one float (32-bit) in the low lane";
            if (suffix == "sd")
                return "one double (64-bit) in the low lane";
            if (suffix == "ps")
                return "every float lane";
            if (suffix == "pd")
                return "every double lane";
            return {};
        }

        std::string_view KindText(std::string_view code)
        {
            if (code == "ss")
                return "a float";
            if (code == "sd")
                return "a double";
            if (code == "si")
                return "an integer";
            if (code == "ps")
                return "floats";
            if (code == "pd")
                return "doubles";
            if (code == "dq")
                return "integers";
            if (code == "pi")
                return "MMX integers";
            return {};
        }

        std::string Lanes(std::string_view mnemonic)
        {
            static const std::unordered_map<std::string_view, std::string_view> operations = {
                { "add", "Adds" }, { "sub", "Subtracts" }, { "mul", "Multiplies" }, { "div", "Divides" }, { "min", "Takes the minimum of" }, { "max", "Takes the maximum of" },
                { "sqrt", "Takes the square root of" }, { "rcp", "Approximates the reciprocal of" }, { "rsqrt", "Approximates the reciprocal square root of" },
                { "and", "ANDs the bits of" }, { "andn", "Inverts the destination, then ANDs the bits of" }, { "or", "ORs the bits of" }, { "xor", "XORs the bits of" },
                { "cmp", "Compares (by the predicate in the immediate)" },
            };
            if (mnemonic.size() < 4)
                return {};
            const std::string_view lane = LaneText(mnemonic.substr(mnemonic.size() - 2));
            if (lane.empty())
                return {};
            const auto found = operations.find(mnemonic.substr(0, mnemonic.size() - 2));
            if (found == operations.end())
                return {};
            std::string text = std::string(found->second) + " " + std::string(lane) + ".";
            if (found->first == "xor" && mnemonic.size() == 5)
                text += " `xorps xmm0,xmm0` sets the register to zero.";
            return text;
        }

        std::string Conversion(std::string_view mnemonic)
        {
            if (mnemonic.rfind("cvt", 0) != 0)
                return {};
            const bool truncate = mnemonic.rfind("cvtt", 0) == 0;
            const std::string_view body = mnemonic.substr(truncate ? 4 : 3);
            const std::size_t two = body.find('2');
            if (two == std::string_view::npos)
                return {};
            const std::string_view from = KindText(body.substr(0, two));
            const std::string_view to = KindText(body.substr(two + 1));
            if (from.empty() || to.empty())
                return {};
            return "Converts " + std::string(from) + " to " + std::string(to) + (truncate ? ", cutting off the fraction (rounding toward zero)." : ", rounding by the current mode.");
        }
    }

    std::string MnemonicSummary(std::string_view mnemonic)
    {
        const std::string name = Lower(Trim(mnemonic));
        if (name.empty())
            return {};
        if (const auto found = Plain().find(name); found != Plain().end())
            return std::string(found->second);
        if (name.size() > 1 && name[0] == 'j')
        {
            if (const std::string_view meaning = ConditionOf(std::string_view(name).substr(1)); !meaning.empty())
                return "Jumps if " + std::string(meaning) + ", else goes on.";
        }
        if (name.rfind("set", 0) == 0)
        {
            if (const std::string_view meaning = ConditionOf(std::string_view(name).substr(3)); !meaning.empty())
                return "Sets the byte to 1 if " + std::string(meaning) + ", else to 0.";
        }
        if (name.rfind("cmov", 0) == 0)
        {
            if (const std::string_view meaning = ConditionOf(std::string_view(name).substr(4)); !meaning.empty())
                return "Copies the source into the destination if " + std::string(meaning) + ".";
        }
        if (std::string text = Lanes(name); !text.empty())
            return text;
        if (std::string text = Conversion(name); !text.empty())
            return text;
        if (name.size() > 1 && name[0] == 'v')
        {
            const std::string inner = MnemonicSummary(std::string_view(name).substr(1));
            if (!inner.empty())
                return "AVX form, with a separate destination: " + inner;
        }
        return {};
    }
}
