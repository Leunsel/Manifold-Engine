#include "Asm/MnemonicDocs.h"

#include "Asm/Assembler.h"
#include "Core/Text.h"

#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
                { "movbe", "Copies a value and reverses its byte order (big-endian load or store)." },
                { "lea", "Computes the address of the memory operand and stores the address itself, without reading memory. Often used for arithmetic." },
                { "xchg", "Swaps the two operands. With a memory operand it is atomic." },
                { "push", "Lowers the stack pointer and stores the operand on the stack." },
                { "pop", "Loads the value on top of the stack into the operand and raises the stack pointer." },
                { "push2", "Pushes two registers at once (APX)." },
                { "pop2", "Pops two registers at once (APX)." },
                { "push2p", "Pushes two registers at once, hinting that a matching pop2p follows (APX)." },
                { "pop2p", "Pops two registers at once, pairing with an earlier push2p (APX)." },
                { "pushp", "Pushes a register, hinting that a matching popp follows (APX)." },
                { "popp", "Pops a register, pairing with an earlier pushp (APX)." },
                { "pusha", "Pushes all 16-bit general registers (32-bit code only)." },
                { "pushad", "Pushes all 32-bit general registers (32-bit code only)." },
                { "popa", "Pops all 16-bit general registers (32-bit code only)." },
                { "popad", "Pops all 32-bit general registers (32-bit code only)." },
                { "pushf", "Pushes the 16-bit flags register." },
                { "pushfd", "Pushes the 32-bit flags register." },
                { "pushfq", "Pushes the 64-bit flags register." },
                { "popf", "Pops the 16-bit flags register." },
                { "popfd", "Pops the 32-bit flags register." },
                { "popfq", "Pops the 64-bit flags register." },
                { "add", "Adds the source to the destination." },
                { "adc", "Adds the source and the carry flag to the destination." },
                { "adcx", "Adds with the carry flag and changes only CF, for long additions in parallel with adox." },
                { "adox", "Adds with the overflow flag as carry and changes only OF, for long additions in parallel with adcx." },
                { "sub", "Subtracts the source from the destination." },
                { "sbb", "Subtracts the source and the carry flag from the destination." },
                { "inc", "Adds 1 to the operand. The carry flag stays as it is." },
                { "dec", "Subtracts 1 from the operand. The carry flag stays as it is." },
                { "neg", "Negates the operand (two's complement)." },
                { "imul", "Multiplies signed integers." },
                { "imulzu", "Multiplies signed integers and clears the upper bits of the destination (APX)." },
                { "mul", "Multiplies unsigned integers; the result goes to rdx:rax (or edx:eax, dx:ax, ax)." },
                { "mulx", "Multiplies unsigned rdx (or edx) by the source into two destinations without touching the flags (BMI2)." },
                { "idiv", "Divides signed integers: rdx:rax by the operand, quotient in rax, remainder in rdx." },
                { "div", "Divides unsigned integers: rdx:rax by the operand, quotient in rax, remainder in rdx." },
                { "cmp", "Subtracts the source from the destination only to set the flags; neither operand changes. Usually followed by a conditional jump." },
                { "test", "ANDs the two operands only to set the flags; neither operand changes. `test eax,eax` checks for zero." },
                { "and", "Bitwise AND of destination and source." },
                { "or", "Bitwise OR of destination and source." },
                { "xor", "Bitwise exclusive OR. `xor eax,eax` sets the register to zero." },
                { "not", "Inverts every bit of the operand." },
                { "andn", "ANDs the source with the inverted first operand (BMI1)." },
                { "bextr", "Extracts a run of bits given by start and length (BMI1)." },
                { "blsi", "Keeps only the lowest set bit (BMI1)." },
                { "blsmsk", "Sets every bit up to and including the lowest set bit (BMI1)." },
                { "blsr", "Clears the lowest set bit (BMI1)." },
                { "bzhi", "Clears the bits from a given position upward (BMI2)." },
                { "pdep", "Scatters the low bits of the source to the positions set in a mask (BMI2)." },
                { "pext", "Gathers the bits at the positions set in a mask into the low bits (BMI2)." },
                { "rorx", "Rotates right by a constant without touching the flags (BMI2)." },
                { "sarx", "Shifts right arithmetically by a register count without touching the flags (BMI2)." },
                { "shlx", "Shifts left by a register count without touching the flags (BMI2)." },
                { "shrx", "Shifts right logically by a register count without touching the flags (BMI2)." },
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
                { "crc32", "Adds the source to a CRC-32C checksum." },
                { "cbw", "Extends the sign of al into ax." },
                { "cwde", "Extends the sign of ax into eax." },
                { "cdqe", "Extends the sign of eax into rax." },
                { "cwd", "Extends the sign of ax into dx:ax." },
                { "cdq", "Extends the sign of eax into edx:eax, before a signed division." },
                { "cqo", "Extends the sign of rax into rdx:rax, before a signed division." },
                { "aaa", "Adjusts al after adding unpacked BCD digits (32-bit code only)." },
                { "aas", "Adjusts al after subtracting unpacked BCD digits (32-bit code only)." },
                { "aam", "Splits al into two unpacked BCD digits after a multiplication (32-bit code only)." },
                { "aad", "Joins two unpacked BCD digits in ax before a division (32-bit code only)." },
                { "daa", "Adjusts al after adding packed BCD digits (32-bit code only)." },
                { "das", "Adjusts al after subtracting packed BCD digits (32-bit code only)." },
                { "salc", "Sets al to 0xFF if the carry flag is set, else to 0 (undocumented, 32-bit code only)." },
                { "setalc", "Sets al to 0xFF if the carry flag is set, else to 0 (undocumented, 32-bit code only)." },
                { "xlat", "Loads the byte at [rbx+al] into al (table lookup)." },
                { "xlatb", "Loads the byte at [rbx+al] into al (table lookup)." },
                { "jmp", "Jumps to the target." },
                { "jmpabs", "Jumps to a 64-bit absolute address (APX)." },
                { "call", "Pushes the return address and jumps to the target." },
                { "ret", "Pops the return address and jumps there; with an operand it also removes that many bytes of arguments." },
                { "retn", "Returns within the same code segment; the same as ret." },
                { "retf", "Returns to another code segment (far return)." },
                { "iret", "Returns from an interrupt handler (16-bit)." },
                { "iretd", "Returns from an interrupt handler (32-bit)." },
                { "iretq", "Returns from an interrupt handler (64-bit)." },
                { "jrcxz", "Jumps if rcx is zero." },
                { "jecxz", "Jumps if ecx is zero." },
                { "jcxz", "Jumps if cx is zero." },
                { "loop", "Decreases rcx and jumps while it is not zero." },
                { "loope", "Decreases rcx and jumps while it is not zero and ZF = 1." },
                { "loopz", "Decreases rcx and jumps while it is not zero and ZF = 1." },
                { "loopne", "Decreases rcx and jumps while it is not zero and ZF = 0." },
                { "loopnz", "Decreases rcx and jumps while it is not zero and ZF = 0." },
                { "nop", "Does nothing. Longer forms are used as padding." },
                { "int3", "Breakpoint: raises a debug exception. Also used as padding between functions." },
                { "int1", "Raises a debug exception (ICEBP)." },
                { "icebp", "Raises a debug exception (int1)." },
                { "int", "Raises a software interrupt." },
                { "into", "Raises an overflow interrupt if OF = 1 (32-bit code only)." },
                { "ud0", "Raises an invalid-opcode exception on purpose." },
                { "ud1", "Raises an invalid-opcode exception on purpose." },
                { "ud2", "Raises an invalid-opcode exception on purpose; marks code that must not be reached." },
                { "hlt", "Halts the processor; privileged." },
                { "syscall", "Calls the operating system kernel." },
                { "sysret", "Returns from the kernel to user code after syscall; privileged." },
                { "sysenter", "Calls the operating system kernel (older fast system call)." },
                { "sysexit", "Returns from the kernel after sysenter; privileged." },
                { "swapgs", "Swaps the gs base with a kernel value; privileged." },
                { "leave", "Restores the stack pointer from the frame pointer and pops the frame pointer, at the end of a function." },
                { "enter", "Sets up a stack frame." },
                { "endbr64", "Marks a valid target of an indirect jump or call (CET); does nothing otherwise." },
                { "endbr32", "Marks a valid target of an indirect jump or call in 32-bit code (CET)." },
                { "clc", "Clears the carry flag." },
                { "stc", "Sets the carry flag." },
                { "cmc", "Inverts the carry flag." },
                { "cld", "Clears the direction flag, so string instructions count up." },
                { "std", "Sets the direction flag, so string instructions count down." },
                { "cli", "Disables interrupts; privileged." },
                { "sti", "Enables interrupts; privileged." },
                { "clac", "Clears the AC flag, so the kernel may not touch user memory (SMAP)." },
                { "stac", "Sets the AC flag, so the kernel may touch user memory (SMAP)." },
                { "lahf", "Loads the low flags into ah." },
                { "sahf", "Stores ah into the low flags." },
                { "movs", "Copies memory from [rsi] to [rdi] and advances both; with rep, rcx times." },
                { "movsd", "Copies one double (64-bit) with SSE; from memory it clears the rest of the register. Without operands it is the string copy of a dword from [rsi] to [rdi]." },
                { "cmpsd", "Compares one double with SSE by the predicate in the immediate. Without operands it is the string compare of the dwords at [rsi] and [rdi]." },
                { "cmpxchg", "Compares the accumulator with the destination; if equal, writes the source there, else loads the destination into the accumulator. With lock it is atomic." },
                { "cmpxchg8b", "Compares edx:eax with 8 bytes in memory and swaps in ecx:ebx if equal." },
                { "cmpxchg16b", "Compares rdx:rax with 16 bytes in memory and swaps in rcx:rbx if equal." },
                { "xadd", "Swaps the operands and stores their sum in the destination. With lock it is atomic." },
                { "aadd", "Adds a register to memory atomically, without returning the old value (RAO-INT)." },
                { "aand", "ANDs a register into memory atomically (RAO-INT)." },
                { "aor", "ORs a register into memory atomically (RAO-INT)." },
                { "axor", "XORs a register into memory atomically (RAO-INT)." },
                { "pause", "Hints a spin-wait loop to the processor." },
                { "mfence", "Orders all earlier loads and stores before later ones." },
                { "lfence", "Orders earlier loads before later ones." },
                { "sfence", "Orders earlier stores before later ones." },
                { "serialize", "Waits until every earlier instruction is finished." },
                { "cpuid", "Reports processor information in eax, ebx, ecx and edx." },
                { "rdtsc", "Reads the time-stamp counter into edx:eax." },
                { "rdtscp", "Reads the time-stamp counter into edx:eax and the processor id into ecx." },
                { "rdpid", "Reads the processor id." },
                { "rdpmc", "Reads a performance counter." },
                { "rdpru", "Reads a processor register (AMD)." },
                { "rdrand", "Loads a hardware random number; CF = 1 when it worked." },
                { "rdseed", "Loads a hardware random seed; CF = 1 when it worked." },
                { "rdpkru", "Reads the memory protection keys of user pages." },
                { "wrpkru", "Writes the memory protection keys of user pages." },
                { "rdfsbase", "Reads the fs base address." },
                { "rdgsbase", "Reads the gs base address." },
                { "wrfsbase", "Writes the fs base address." },
                { "wrgsbase", "Writes the gs base address." },
                { "xgetbv", "Reads an extended control register (which state the OS saves)." },
                { "xsetbv", "Writes an extended control register; privileged." },
                { "in", "Reads from an I/O port; privileged." },
                { "out", "Writes to an I/O port; privileged." },
                { "invd", "Discards the caches without writing them back; privileged." },
                { "wbinvd", "Writes back and discards the caches; privileged." },
                { "invlpg", "Discards the TLB entry of one page; privileged." },
                { "invpcid", "Discards TLB entries of an address space; privileged." },
                { "invlpgb", "Discards TLB entries on all processors (AMD); privileged." },
                { "tlbsync", "Waits for invlpgb to finish everywhere (AMD); privileged." },
                { "clts", "Clears the task-switched flag; privileged." },
                { "lgdt", "Loads the global descriptor table register; privileged." },
                { "lidt", "Loads the interrupt descriptor table register; privileged." },
                { "lldt", "Loads the local descriptor table register; privileged." },
                { "ltr", "Loads the task register; privileged." },
                { "lmsw", "Loads the machine status word; privileged." },
                { "sgdt", "Stores the global descriptor table register." },
                { "sidt", "Stores the interrupt descriptor table register." },
                { "sldt", "Stores the local descriptor table register." },
                { "str", "Stores the task register." },
                { "smsw", "Stores the machine status word." },
                { "lar", "Loads the access rights of a segment selector." },
                { "lsl", "Loads the limit of a segment selector." },
                { "verr", "Checks whether a segment can be read." },
                { "verw", "Checks whether a segment can be written; also used to clear CPU buffers." },
                { "lds", "Loads a far pointer into ds and a register (32-bit code only)." },
                { "les", "Loads a far pointer into es and a register (32-bit code only)." },
                { "lfs", "Loads a far pointer into fs and a register." },
                { "lgs", "Loads a far pointer into gs and a register." },
                { "lss", "Loads a far pointer into ss and a register." },
                { "lkgs", "Loads the kernel gs base from a selector; privileged." },
                { "arpl", "Adjusts the privilege level of a selector (32-bit code only)." },
                { "bound", "Raises an exception if an index is outside two bounds (32-bit code only)." },
                { "rsm", "Returns from system management mode." },
                { "monitor", "Arms address monitoring for mwait." },
                { "mwait", "Waits until the monitored address is written or an interrupt arrives." },
                { "monitorx", "Arms address monitoring for mwaitx (AMD)." },
                { "mwaitx", "Waits with a timeout until the monitored address is written (AMD)." },
                { "umonitor", "Arms address monitoring for umwait from user code." },
                { "umwait", "Waits in user code until the monitored address is written or a deadline passes." },
                { "tpause", "Pauses until a deadline in time-stamp counter ticks." },
                { "delay", "Waits a number of cycles (Knights Corner)." },
                { "hreset", "Resets the hardware prediction history." },
                { "ptwrite", "Writes a value into the processor trace." },
                { "xabort", "Aborts a hardware transaction (TSX)." },
                { "xbegin", "Starts a hardware transaction (TSX); on abort it jumps to the fallback." },
                { "xend", "Commits a hardware transaction (TSX)." },
                { "xtest", "Tests whether a hardware transaction runs (TSX)." },
                { "xresldtrk", "Resumes load-address tracking in a transaction (TSX)." },
                { "xsusldtrk", "Suspends load-address tracking in a transaction (TSX)." },
                { "wait", "Waits until pending x87 exceptions are handled." },
                { "fwait", "Waits until pending x87 exceptions are handled." },
                { "emms", "Ends MMX code so the x87 registers can be used again." },
                { "femms", "Ends MMX or 3DNow! code quickly (AMD)." },
                { "clflush", "Writes back and discards the cache line of an address." },
                { "clflushopt", "Writes back and discards the cache line of an address, with weaker ordering than clflush." },
                { "clwb", "Writes back the cache line of an address and may keep it." },
                { "cldemote", "Moves a cache line toward a cache further from the core." },
                { "clzero", "Writes zeros to a whole cache line (AMD)." },
                { "prefetch", "Hints that the memory will be read soon (3DNow!)." },
                { "prefetchw", "Hints that the memory will be written soon." },
                { "prefetchwt1", "Hints that the memory will be written soon, into a farther cache." },
                { "prefetcht0", "Hints that the memory will be needed soon, into all caches." },
                { "prefetcht1", "Hints that the memory will be needed soon, into the level 2 cache and farther." },
                { "prefetcht2", "Hints that the memory will be needed soon, into the level 3 cache and farther." },
                { "prefetchnta", "Hints that the memory will be needed once." },
                { "prefetchit0", "Hints that code at the address will run soon, into all caches." },
                { "prefetchit1", "Hints that code at the address will run soon, into the level 2 cache." },
                { "movd", "Copies 32 bits between a general register or memory and an SSE register." },
                { "movq", "Copies 64 bits between a general register, memory and an SSE register." },
                { "movdq2q", "Copies the low 64 bits of an SSE register to an MMX register." },
                { "movq2dq", "Copies an MMX register into the low 64 bits of an SSE register." },
                { "movaps", "Copies 16 bytes of floats; memory must be 16-byte aligned." },
                { "movups", "Copies 16 bytes of floats; memory may be unaligned." },
                { "movapd", "Copies 16 bytes of doubles; memory must be 16-byte aligned." },
                { "movupd", "Copies 16 bytes of doubles; memory may be unaligned." },
                { "movdqa", "Copies a whole vector register of integers; memory must be aligned to its size." },
                { "movdqu", "Copies a whole vector register of integers; memory may be unaligned." },
                { "lddqu", "Loads 16 unaligned bytes, faster when they cross a cache line." },
                { "movss", "Copies one float (32-bit). From memory it clears the rest of the register." },
                { "movsh", "Copies one half float (16-bit) (AVX-512 FP16)." },
                { "movw", "Copies 16 bits between a general register and an SSE register (AVX-512 FP16)." },
                { "movlps", "Copies two floats to or from the low half of an SSE register." },
                { "movhps", "Copies two floats to or from the high half of an SSE register." },
                { "movlpd", "Copies one double to or from the low half of an SSE register." },
                { "movhpd", "Copies one double to or from the high half of an SSE register." },
                { "movhlps", "Copies the high two floats of the source into the low half of the destination." },
                { "movlhps", "Copies the low two floats of the source into the high half of the destination." },
                { "movddup", "Copies a double into both halves." },
                { "movshdup", "Copies the odd floats into the even and odd lanes." },
                { "movsldup", "Copies the even floats into the even and odd lanes." },
                { "movnti", "Stores a general register, bypassing the cache." },
                { "movntq", "Stores an MMX register, bypassing the cache." },
                { "movntdq", "Stores 16 bytes of integers, bypassing the cache." },
                { "movntdqa", "Loads 16 aligned bytes with a non-temporal hint." },
                { "movntss", "Stores one float, bypassing the cache (AMD)." },
                { "movntsd", "Stores one double, bypassing the cache (AMD)." },
                { "movdiri", "Stores a register with a direct store that bypasses the cache." },
                { "movdir64b", "Copies 64 bytes as one direct store to a device." },
                { "maskmovq", "Stores the bytes of an MMX register whose mask byte has the top bit set." },
                { "maskmovdqu", "Stores the bytes of an SSE register whose mask byte has the top bit set." },
                { "insertps", "Inserts one float into an SSE register and can clear lanes." },
                { "extractps", "Copies one float of an SSE register into a general register or memory." },
                { "comiss", "Compares two floats and sets ZF, PF and CF like an unsigned compare; signals on any NaN." },
                { "ucomiss", "Compares two floats and sets ZF, PF and CF like an unsigned compare; PF = 1 means unordered (NaN)." },
                { "comisd", "Compares two doubles and sets ZF, PF and CF like an unsigned compare; signals on any NaN." },
                { "ucomisd", "Compares two doubles and sets ZF, PF and CF like an unsigned compare; PF = 1 means unordered (NaN)." },
                { "comish", "Compares two half floats and sets ZF, PF and CF; signals on any NaN." },
                { "ucomish", "Compares two half floats and sets ZF, PF and CF; PF = 1 means unordered (NaN)." },
                { "ldmxcsr", "Loads the SSE control and status register (rounding mode, exception masks)." },
                { "stmxcsr", "Stores the SSE control and status register." },
                { "extrq", "Extracts a run of bits from the low qword (SSE4a)." },
                { "insertq", "Inserts a run of bits into the low qword (SSE4a)." },
                { "pxor", "Bitwise exclusive OR of 128 bits. `pxor xmm0,xmm0` sets the register to zero." },
                { "por", "Bitwise OR of 128 bits." },
                { "pand", "Bitwise AND of 128 bits." },
                { "pandn", "Inverts the destination, then ANDs it with the source." },
                { "pshufd", "Picks dwords of the source by an immediate pattern." },
                { "pshufb", "Picks bytes of the destination by the indexes in the source." },
                { "pshufhw", "Picks the high four words by an immediate pattern." },
                { "pshuflw", "Picks the low four words by an immediate pattern." },
                { "pshufw", "Picks the four words of an MMX register by an immediate pattern." },
                { "pshufbitqmb", "Picks bits of each qword by byte indexes into a mask register." },
                { "pmovmskb", "Copies the sign bit of each byte into a general register." },
                { "pmaddwd", "Multiplies signed words and adds neighbouring products into dwords." },
                { "pmaddubsw", "Multiplies unsigned bytes by signed bytes and adds neighbouring products into words with saturation." },
                { "pmuldq", "Multiplies the even signed dwords into qwords." },
                { "pmuludq", "Multiplies the even unsigned dwords into qwords." },
                { "psadbw", "Adds the absolute differences of bytes (sum of absolute differences)." },
                { "dbpsadbw", "Adds absolute differences of byte groups picked by an immediate." },
                { "mpsadbw", "Adds absolute differences of byte groups at several offsets." },
                { "pslldq", "Shifts the whole register left by whole bytes." },
                { "psrldq", "Shifts the whole register right by whole bytes." },
                { "palignr", "Joins two registers and takes a byte-aligned window of them." },
                { "pblendw", "Picks words from two sources by an immediate mask." },
                { "pblendvb", "Picks bytes from two sources by the sign bits of a mask register." },
                { "pblendd", "Picks dwords from two sources by an immediate mask." },
                { "ptest", "ANDs two registers and sets ZF (and CF for the inverted first one) without storing." },
                { "phminposuw", "Finds the smallest unsigned word and its position." },
                { "packsswb", "Narrows signed words to signed bytes with saturation." },
                { "packssdw", "Narrows signed dwords to signed words with saturation." },
                { "packuswb", "Narrows signed words to unsigned bytes with saturation." },
                { "packusdw", "Narrows signed dwords to unsigned words with saturation." },
                { "pclmulqdq", "Multiplies two qwords without carries (carry-less), for CRC and GCM." },
                { "pcmpestri", "Compares strings of explicit length and returns an index." },
                { "pcmpestrm", "Compares strings of explicit length and returns a mask." },
                { "pcmpistri", "Compares zero-terminated strings and returns an index." },
                { "pcmpistrm", "Compares zero-terminated strings and returns a mask." },
                { "pcmov", "Picks bits from two sources by a third (AMD XOP)." },
                { "pperm", "Picks and transforms bytes from two sources (AMD XOP)." },
                { "pternlogd", "Computes any bitwise function of three operands, chosen by an immediate (dwords)." },
                { "pternlogq", "Computes any bitwise function of three operands, chosen by an immediate (qwords)." },
                { "pmultishiftqb", "Picks unaligned bytes from qwords by bit offsets." },
                { "pmadd52luq", "Multiplies 52-bit integers and adds the low 52 bits of the products to qwords." },
                { "pmadd52huq", "Multiplies 52-bit integers and adds the high 52 bits of the products to qwords." },
                { "shufpd", "Picks doubles from both operands by an immediate pattern." },
                { "shufps", "Picks floats from both operands by an immediate pattern." },
                { "unpcklps", "Interleaves the low floats of both operands." },
                { "unpckhps", "Interleaves the high floats of both operands." },
                { "unpcklpd", "Interleaves the low doubles of both operands." },
                { "unpckhpd", "Interleaves the high doubles of both operands." },
                { "movmskps", "Copies the sign bits of four floats into a general register." },
                { "movmskpd", "Copies the sign bits of two doubles into a general register." },
                { "vzeroupper", "Clears the upper halves of all ymm registers, to avoid a penalty when switching from AVX to SSE code." },
                { "vzeroall", "Clears every ymm register." },
                { "pcommit", "Commits earlier stores to persistent memory (obsolete)." },
                { "fld", "Pushes a value onto the x87 floating-point stack." },
                { "fst", "Stores the top of the x87 stack." },
                { "fstp", "Stores the top of the x87 stack and pops it." },
                { "fstpnce", "Stores the top of the x87 stack and pops it (no-exception form)." },
                { "fild", "Converts an integer and pushes it onto the x87 stack." },
                { "fist", "Stores the top of the x87 stack as an integer." },
                { "fistp", "Stores the top of the x87 stack as an integer and pops it." },
                { "fisttp", "Stores the top of the x87 stack as an integer, cutting off the fraction, and pops it." },
                { "fbld", "Pushes a packed BCD number onto the x87 stack." },
                { "fbstp", "Stores the top of the x87 stack as packed BCD and pops it." },
                { "fxch", "Swaps two x87 stack registers." },
                { "fldz", "Pushes 0.0 onto the x87 stack." },
                { "fld1", "Pushes 1.0 onto the x87 stack." },
                { "fldpi", "Pushes pi onto the x87 stack." },
                { "fldl2e", "Pushes log2(e) onto the x87 stack." },
                { "fldl2t", "Pushes log2(10) onto the x87 stack." },
                { "fldlg2", "Pushes log10(2) onto the x87 stack." },
                { "fldln2", "Pushes ln(2) onto the x87 stack." },
                { "fabs", "Makes st0 positive." },
                { "fchs", "Changes the sign of st0." },
                { "fsqrt", "Takes the square root of st0." },
                { "fsin", "Takes the sine of st0." },
                { "fcos", "Takes the cosine of st0." },
                { "fsincos", "Replaces st0 by its sine and pushes its cosine." },
                { "fptan", "Replaces st0 by its tangent and pushes 1.0." },
                { "fpatan", "Computes arctan(st1 / st0) and pops." },
                { "f2xm1", "Computes 2^st0 - 1." },
                { "fyl2x", "Computes st1 * log2(st0) and pops." },
                { "fyl2xp1", "Computes st1 * log2(st0 + 1) and pops." },
                { "fscale", "Multiplies st0 by 2 to the power of st1 (rounded toward zero)." },
                { "fxtract", "Splits st0 into exponent and significand." },
                { "fprem", "Partial remainder of st0 / st1 (rounded toward zero)." },
                { "fprem1", "Partial IEEE remainder of st0 / st1." },
                { "frndint", "Rounds st0 to an integer by the current mode." },
                { "ftst", "Compares st0 with 0.0 and sets the x87 condition codes." },
                { "fxam", "Classifies st0 (zero, NaN, normal…) into the x87 condition codes." },
                { "fcom", "Compares st0 with the operand and sets the x87 condition codes." },
                { "fcomp", "Compares st0 with the operand, sets the x87 condition codes and pops." },
                { "fcompp", "Compares st0 with st1, sets the x87 condition codes and pops twice." },
                { "fucom", "Compares st0 with the operand (quiet on NaN) and sets the x87 condition codes." },
                { "fucomp", "Compares st0 with the operand (quiet on NaN), sets the condition codes and pops." },
                { "fucompp", "Compares st0 with st1 (quiet on NaN), sets the condition codes and pops twice." },
                { "fcomi", "Compares st0 with another x87 register and sets ZF, PF and CF." },
                { "fcomip", "Compares st0 with another x87 register, sets ZF, PF and CF and pops." },
                { "fucomi", "Compares st0 with another x87 register (quiet on NaN) and sets ZF, PF and CF." },
                { "fucomip", "Compares st0 with another x87 register (quiet on NaN), sets ZF, PF and CF and pops." },
                { "ficom", "Compares st0 with an integer and sets the x87 condition codes." },
                { "ficomp", "Compares st0 with an integer, sets the x87 condition codes and pops." },
                { "finit", "Resets the x87 unit." },
                { "fninit", "Resets the x87 unit without waiting for pending exceptions." },
                { "fclex", "Clears the x87 exception flags." },
                { "fnclex", "Clears the x87 exception flags without waiting." },
                { "fldcw", "Loads the x87 control word (rounding mode, precision)." },
                { "fstcw", "Stores the x87 control word." },
                { "fnstcw", "Stores the x87 control word without waiting." },
                { "fstsw", "Stores the x87 status word (condition codes)." },
                { "fnstsw", "Stores the x87 status word without waiting; `fnstsw ax` then `sahf` brings the codes to the flags." },
                { "fldenv", "Loads the x87 environment." },
                { "fstenv", "Stores the x87 environment." },
                { "fnstenv", "Stores the x87 environment without waiting." },
                { "fsave", "Stores the whole x87 state and resets it." },
                { "fnsave", "Stores the whole x87 state without waiting and resets it." },
                { "frstor", "Loads the whole x87 state." },
                { "ffree", "Marks an x87 register as empty." },
                { "ffreep", "Marks an x87 register as empty and pops." },
                { "fincstp", "Rotates the x87 stack top up." },
                { "fdecstp", "Rotates the x87 stack top down." },
                { "fnop", "Does nothing (x87)." },
                { "fdisi8087_nop", "Does nothing on modern processors (old 8087 instruction)." },
                { "feni8087_nop", "Does nothing on modern processors (old 8087 instruction)." },
                { "fsetpm287_nop", "Does nothing on modern processors (old 80287 instruction)." },
                { "vbroadcastss", "Copies one float into every lane." },
                { "vbroadcastsd", "Copies one double into every lane." },
                { "vperm2f128", "Picks 128-bit halves from both sources." },
                { "vperm2i128", "Picks 128-bit halves from both sources." },
                { "vpermilps", "Picks floats within each 128-bit half by indexes." },
                { "vpermilpd", "Picks doubles within each 128-bit half by indexes." },
                { "vpermil2ps", "Picks floats from two sources by indexes (AMD XOP)." },
                { "vpermil2pd", "Picks doubles from two sources by indexes (AMD XOP)." },
                { "vtestps", "Tests the sign bits of floats and sets ZF and CF." },
                { "vtestpd", "Tests the sign bits of doubles and sets ZF and CF." },
                { "valignd", "Joins two registers and shifts them by whole dwords." },
                { "valignq", "Joins two registers and shifts them by whole qwords." },
                { "vpblendd", "Picks dwords from two sources by an immediate mask." },
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

        std::string_view ConditionOf(std::string_view suffix, bool always = false)
        {
            for (const Condition& condition : kConditions)
            {
                if (condition.Suffix == suffix)
                    return condition.Meaning;
            }
            if (always && suffix == "t")
                return "always";
            if (always && suffix == "f")
                return "never";
            return {};
        }

        std::string_view X87Condition(std::string_view suffix)
        {
            if (suffix == "b")
                return "below (CF = 1)";
            if (suffix == "nb")
                return "not below (CF = 0)";
            if (suffix == "e")
                return "equal (ZF = 1)";
            if (suffix == "ne")
                return "not equal (ZF = 0)";
            if (suffix == "be")
                return "below or equal (CF = 1 or ZF = 1)";
            if (suffix == "nbe")
                return "above (CF = 0 and ZF = 0)";
            if (suffix == "u")
                return "unordered (PF = 1)";
            if (suffix == "nu")
                return "ordered (PF = 0)";
            return {};
        }

        std::string Conditional(std::string_view name)
        {
            const auto starts = [&](std::string_view prefix) { return name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix; };
            if (starts("fcmov"))
            {
                if (const std::string_view meaning = X87Condition(name.substr(5)); !meaning.empty())
                    return "Copies an x87 register into st0 if " + std::string(meaning) + ".";
            }
            if (starts("cfcmov"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(6)); !meaning.empty())
                    return "Copies the source if " + std::string(meaning) + "; a false condition never faults on memory (APX).";
            }
            if (starts("setzu"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(5)); !meaning.empty())
                    return "Sets the register to 1 if " + std::string(meaning) + ", else to 0, clearing its upper bits (APX).";
            }
            if (name == "ccmpt" || name == "ctestt")
                return std::string(name == "ccmpt" ? "Compares like cmp" : "Tests like test") + "; the condition is always true (APX).";
            if (name == "ccmpf" || name == "ctestf")
                return "Sets the flags from an immediate; the condition is always false (APX).";
            if (starts("ccmp"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(4), true); !meaning.empty())
                    return "Compares like cmp if " + std::string(meaning) + "; otherwise sets the flags from an immediate (APX).";
            }
            if (starts("ctest"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(5), true); !meaning.empty())
                    return "Tests like test if " + std::string(meaning) + "; otherwise sets the flags from an immediate (APX).";
            }
            if (starts("cmp") && name.size() > 7 && name.substr(name.size() - 4) == "xadd")
            {
                if (const std::string_view meaning = ConditionOf(name.substr(3, name.size() - 7)); !meaning.empty())
                    return "Atomically compares memory with a register and adds a value to the memory if " + std::string(meaning) + " (CMPccXADD).";
            }
            if (starts("j"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(1)); !meaning.empty())
                    return "Jumps if " + std::string(meaning) + ", else goes on.";
            }
            if (starts("set"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(3)); !meaning.empty())
                    return "Sets the byte to 1 if " + std::string(meaning) + ", else to 0.";
            }
            if (starts("cmov"))
            {
                if (const std::string_view meaning = ConditionOf(name.substr(4)); !meaning.empty())
                    return "Copies the source into the destination if " + std::string(meaning) + ".";
            }
            return {};
        }

        std::string_view SizeName(char code)
        {
            switch (code)
            {
            case 'b': return "byte";
            case 'w': return "word";
            case 'd': return "dword";
            case 'q': return "qword";
            default: return {};
            }
        }

        std::string_view Accumulator(char code)
        {
            switch (code)
            {
            case 'b': return "al";
            case 'w': return "ax";
            case 'd': return "eax";
            case 'q': return "rax";
            default: return {};
            }
        }

        std::string StringOperation(std::string_view name)
        {
            if (name.size() < 4)
                return {};
            const char code = name.back();
            const std::string_view size = SizeName(code);
            if (size.empty())
                return {};
            const std::string_view base = name.substr(0, name.size() - 1);
            const std::string unit(size);
            const std::string reg(Accumulator(code));
            if (base == "movs")
                return "Copies a " + unit + " from [rsi] to [rdi] and advances both; with rep, rcx " + unit + "s.";
            if (base == "stos")
                return "Stores " + reg + " at [rdi] and advances it; with rep it fills rcx " + unit + "s.";
            if (base == "lods")
                return "Loads the " + unit + " at [rsi] into " + reg + " and advances rsi.";
            if (base == "scas")
                return "Compares " + reg + " with the " + unit + " at [rdi] and advances rdi; with repne it searches.";
            if (base == "cmps")
                return "Compares the " + unit + "s at [rsi] and [rdi] and advances both; with repe it compares rcx " + unit + "s.";
            if (base == "ins")
                return "Reads a " + unit + " from the I/O port in dx to [rdi]; privileged.";
            if (base == "outs")
                return "Writes the " + unit + " at [rsi] to the I/O port in dx; privileged.";
            return {};
        }

        std::string MaskRegister(std::string_view name)
        {
            if (name.size() < 3 || name[0] != 'k')
                return {};
            static constexpr std::pair<std::string_view, std::string_view> kOperations[] = {
                { "andn", "ANDs a mask register with an inverted one" }, { "and", "ANDs two mask registers" }, { "ortest", "ORs two mask registers and sets ZF and CF" },
                { "or", "ORs two mask registers" }, { "xnor", "XNORs two mask registers" }, { "xor", "XORs two mask registers" }, { "not", "Inverts a mask register" },
                { "mov", "Copies a mask register" }, { "add", "Adds two mask registers" }, { "test", "ANDs two mask registers and sets ZF and CF" },
                { "shiftl", "Shifts a mask register left" }, { "shiftr", "Shifts a mask register right" }, { "unpck", "Joins the low halves of two mask registers" },
            };
            const std::string_view body = name.substr(1);
            for (const auto& [operation, verb] : kOperations)
            {
                if (body.substr(0, operation.size()) != operation)
                    continue;
                const std::string_view width = body.substr(operation.size());
                std::string bits;
                if (width == "b" || width == "bw")
                    bits = "8";
                else if (width == "w" || width == "wd")
                    bits = "16";
                else if (width == "d" || width == "dq")
                    bits = "32";
                else if (width == "q")
                    bits = "64";
                else if (!width.empty())
                    continue;
                return std::string(verb) + (bits.empty() ? std::string(" (AVX-512).") : " (AVX-512, " + bits + " bits).");
            }
            return {};
        }

        std::string_view LaneText(std::string_view suffix)
        {
            if (suffix == "ss")
                return "the low float (32-bit)";
            if (suffix == "sd")
                return "the low double (64-bit)";
            if (suffix == "sh")
                return "the low half float (16-bit)";
            if (suffix == "ps")
                return "all floats (32-bit)";
            if (suffix == "pd")
                return "all doubles (64-bit)";
            if (suffix == "ph")
                return "all half floats (16-bit)";
            return {};
        }

        std::string Fill(std::string_view pattern, std::string_view value)
        {
            std::string text(pattern);
            if (const std::size_t at = text.find('%'); at != std::string::npos)
                text.replace(at, 1, value);
            return text;
        }

        std::string FusedMultiply(std::string_view name)
        {
            if (name.size() < 6 || name[0] != 'f')
                return {};
            std::string_view body = name.substr(1);
            const bool negated = !body.empty() && body[0] == 'n';
            if (negated)
                body.remove_prefix(1);
            static constexpr std::string_view kOperations[] = { "maddsub", "msubadd", "madd", "msub" };
            std::string_view operation;
            for (const std::string_view candidate : kOperations)
            {
                if (body.substr(0, candidate.size()) == candidate)
                {
                    operation = candidate;
                    break;
                }
            }
            if (operation.empty())
                return {};
            body.remove_prefix(operation.size());
            std::string_view order;
            if (body.size() > 3 && std::isdigit(static_cast<unsigned char>(body[0])))
            {
                order = body.substr(0, 3);
                body.remove_prefix(3);
            }
            const std::string_view lanes = LaneText(body);
            if (lanes.empty())
                return {};
            std::string product = "src2*src3";
            std::string addend = "dst";
            if (order == "132")
            {
                product = "dst*src3";
                addend = "src2";
            }
            else if (order == "213")
            {
                product = "src2*dst";
                addend = "src3";
            }
            else if (order.empty())
            {
                product = "src2*src3";
                addend = "src4";
            }
            else if (order != "231")
                return {};
            const std::string signedProduct = negated ? "-(" + product + ")" : product;
            std::string formula;
            if (operation == "madd")
                formula = signedProduct + " + " + addend;
            else if (operation == "msub")
                formula = signedProduct + " - " + addend;
            else if (operation == "maddsub")
                formula = signedProduct + " - " + addend + " in even lanes, + in odd lanes";
            else
                formula = signedProduct + " + " + addend + " in even lanes, - in odd lanes";
            return "Fused multiply-add of " + std::string(lanes) + " with one rounding: dst = " + formula + (order.empty() ? " (AMD FMA4)." : ".");
        }

        std::string FloatLanes(std::string_view name)
        {
            static constexpr std::pair<std::string_view, std::string_view> kOperations[] = {
                { "addsub", "Subtracts in even lanes and adds in odd lanes, for %." }, { "add", "Adds %." }, { "sub", "Subtracts %." }, { "mul", "Multiplies %." },
                { "div", "Divides %." }, { "min", "Takes the minimum of %." }, { "max", "Takes the maximum of %." }, { "sqrt", "Takes the square root of %." },
                { "rsqrt14", "Approximates (14 bits) the reciprocal square root of %." }, { "rsqrt28", "Approximates (28 bits) the reciprocal square root of %." },
                { "rsqrt", "Approximates the reciprocal square root of %." }, { "rcp14", "Approximates (14 bits) the reciprocal of %." },
                { "rcp28", "Approximates (28 bits) the reciprocal of %." }, { "rcp", "Approximates the reciprocal of %." },
                { "andn", "Inverts the first source and ANDs the bits of %." }, { "and", "ANDs the bits of %." }, { "or", "ORs the bits of %." },
                { "xor", "XORs the bits of %." }, { "cmp", "Compares % by the predicate in the immediate." }, { "hadd", "Adds neighbouring pairs of %." },
                { "hsub", "Subtracts neighbouring pairs of %." }, { "round", "Rounds % by the mode in the immediate." },
                { "blendv", "Picks % from two sources by the sign bits of a mask register." }, { "blendm", "Picks % from two sources by a mask register." },
                { "blend", "Picks % from two sources by an immediate mask." }, { "movnt", "Stores %, bypassing the cache." }, { "dp", "Computes a dot product of %." },
                { "getexp", "Extracts the exponent of %." }, { "getmant", "Extracts the mantissa of %." }, { "scalef", "Multiplies % by powers of two." },
                { "reduce", "Keeps the part of % below a chosen precision." }, { "rndscale", "Rounds % to a chosen number of fraction bits." },
                { "range", "Picks the minimum, maximum or magnitude of %." }, { "fixupimm", "Fixes special values (NaN, infinity, zero) of %." },
                { "fpclass", "Tests the class (NaN, infinity, zero…) of % into a mask." }, { "compress", "Packs % selected by a mask together." },
                { "expand", "Spreads values into % selected by a mask." }, { "maskmov", "Loads or stores % by a mask." },
                { "gatherd", "Loads % from scattered addresses (dword indexes)." }, { "gatherq", "Loads % from scattered addresses (qword indexes)." },
                { "scatterd", "Stores % to scattered addresses (dword indexes)." }, { "scatterq", "Stores % to scattered addresses (qword indexes)." },
                { "permi2", "Picks % from two tables by indexes." }, { "permt2", "Picks % from two tables by indexes." }, { "perm", "Picks % by indexes." },
                { "shuf", "Picks % by an immediate pattern." }, { "fcmulc", "Multiplies complex numbers (conjugate) in %." }, { "fmulc", "Multiplies complex numbers in %." },
                { "fcmaddc", "Multiplies and adds complex numbers (conjugate) in %." }, { "fmaddc", "Multiplies and adds complex numbers in %." },
            };
            if (name.size() < 4)
                return {};
            const std::string_view lanes = LaneText(name.substr(name.size() - 2));
            if (lanes.empty())
                return {};
            const std::string_view operation = name.substr(0, name.size() - 2);
            for (const auto& [candidate, verb] : kOperations)
            {
                if (operation != candidate)
                    continue;
                std::string text = Fill(verb, lanes);
                if (candidate == "xor" && name.size() == 5)
                    text += " `xorps xmm0,xmm0` sets the register to zero.";
                return text;
            }
            return {};
        }

        std::string_view KindText(std::string_view code)
        {
            static constexpr std::pair<std::string_view, std::string_view> kKinds[] = {
                { "ss", "a float" }, { "sd", "a double" }, { "sh", "a half float" }, { "si", "an integer" }, { "usi", "an unsigned integer" },
                { "ps", "floats" }, { "pd", "doubles" }, { "ph", "half floats" }, { "psx", "floats" }, { "phx", "half floats" }, { "dq", "dwords" },
                { "udq", "unsigned dwords" }, { "qq", "qwords" }, { "uqq", "unsigned qwords" }, { "w", "words" }, { "uw", "unsigned words" },
                { "pi", "MMX integers" }, { "bf16", "bfloat16 values" },
            };
            for (const auto& [candidate, text] : kKinds)
            {
                if (candidate == code)
                    return text;
            }
            return {};
        }

        std::string Conversion(std::string_view name)
        {
            if (name.substr(0, 3) != "cvt")
                return {};
            std::string_view body = name.substr(3);
            const bool truncate = !body.empty() && body[0] == 't';
            if (truncate)
                body.remove_prefix(1);
            if (body.substr(0, 2) == "ne")
                body.remove_prefix(2);
            const std::size_t two = body.find('2');
            if (two == std::string_view::npos)
                return {};
            std::string_view fromCode = body.substr(0, two);
            if (fromCode.size() > 2 && (fromCode.substr(0, 2) == "ee" || fromCode.substr(0, 2) == "eo"))
                fromCode.remove_prefix(1);
            if (!fromCode.empty() && (fromCode[0] == 'e' || fromCode[0] == 'o') && fromCode.size() > 2)
                fromCode.remove_prefix(1);
            const std::string_view from = KindText(fromCode);
            const std::string_view to = KindText(body.substr(two + 1));
            if (from.empty() || to.empty())
                return {};
            return "Converts " + std::string(from) + " to " + std::string(to) + (truncate ? ", cutting off the fraction (rounding toward zero)." : ", rounding by the current mode.");
        }

        std::string_view LaneNoun(std::string_view code)
        {
            static constexpr std::pair<std::string_view, std::string_view> kLanes[] = {
                { "b", "bytes" }, { "w", "words" }, { "d", "dwords" }, { "q", "qwords" }, { "sb", "signed bytes" }, { "sw", "signed words" },
                { "sd", "signed dwords" }, { "sq", "signed qwords" }, { "ub", "unsigned bytes" }, { "uw", "unsigned words" }, { "ud", "unsigned dwords" },
                { "uq", "unsigned qwords" },
            };
            for (const auto& [candidate, text] : kLanes)
            {
                if (candidate == code)
                    return text;
            }
            return {};
        }

        std::string PackedInteger(std::string_view name)
        {
            if (name.size() < 3 || name[0] != 'p')
                return {};
            const std::string_view body = name.substr(1);
            static constexpr std::pair<std::string_view, std::string_view> kOperations[] = {
                { "addus", "Adds % with unsigned saturation." }, { "adds", "Adds % with signed saturation." }, { "add", "Adds %." },
                { "subus", "Subtracts % with unsigned saturation." }, { "subs", "Subtracts % with signed saturation." }, { "sub", "Subtracts %." },
                { "avg", "Averages unsigned %, rounding up." }, { "abs", "Takes the absolute value of signed %." },
                { "sign", "Negates, keeps or zeroes % by the sign of the source." }, { "cmpeq", "Compares % for equality; equal lanes become all ones." },
                { "cmpgt", "Compares signed %; greater lanes become all ones." }, { "cmpu", "Compares unsigned % by the predicate in the immediate, into a mask." },
                { "cmp", "Compares signed % by the predicate in the immediate, into a mask." }, { "max", "Takes the maximum of %." }, { "min", "Takes the minimum of %." },
                { "mulhrs", "Multiplies signed %, rounds and keeps the high half." }, { "mulhu", "Multiplies unsigned % and keeps the high half." },
                { "mulh", "Multiplies signed % and keeps the high half." }, { "mull", "Multiplies % and keeps the low half." },
                { "sllv", "Shifts each of the % left by its own count." }, { "srlv", "Shifts each of the % right (logical) by its own count." },
                { "srav", "Shifts each of the % right (arithmetic) by its own count." }, { "sll", "Shifts % left." }, { "srl", "Shifts % right (logical)." },
                { "sra", "Shifts % right (arithmetic)." }, { "broadcast", "Copies one of the % into every lane." },
                { "compress", "Packs the % selected by a mask together." }, { "expand", "Spreads % into the lanes selected by a mask." },
                { "popcnt", "Counts the set bits in each of the %." }, { "lzcnt", "Counts the leading zero bits in each of the %." },
                { "conflict", "Finds earlier equal elements among the %." }, { "rolv", "Rotates each of the % left by its own count." },
                { "rorv", "Rotates each of the % right by its own count." }, { "rol", "Rotates % left." }, { "ror", "Rotates % right." },
                { "shldv", "Shifts % left, filling from a second source, each by its own count." },
                { "shrdv", "Shifts % right, filling from a second source, each by its own count." },
                { "shld", "Shifts % left, filling from a second source." }, { "shrd", "Shifts % right, filling from a second source." },
                { "testnm", "ANDs % and sets a mask bit where the result is zero." }, { "testm", "ANDs % and sets a mask bit where the result is not zero." },
                { "blendm", "Picks % from two sources by a mask register." }, { "extr", "Copies one of the % out of an SSE register." },
                { "insr", "Inserts one of the % into an SSE register." }, { "hadds", "Adds neighbouring pairs of signed % with saturation." },
                { "hsubs", "Subtracts neighbouring pairs of signed % with saturation." }, { "hadd", "Adds neighbouring pairs of %." },
                { "hsub", "Subtracts neighbouring pairs of %." }, { "andn", "ANDs % with the inverted first source." }, { "and", "ANDs %." }, { "or", "ORs %." },
                { "xor", "XORs %." }, { "gatherd", "Loads % from scattered addresses (dword indexes)." }, { "gatherq", "Loads % from scattered addresses (qword indexes)." },
                { "scatterd", "Stores % to scattered addresses (dword indexes)." }, { "scatterq", "Stores % to scattered addresses (qword indexes)." },
                { "maskmov", "Loads or stores % by a mask." },
            };
            for (const auto& [operation, verb] : kOperations)
            {
                if (body.substr(0, operation.size()) != operation)
                    continue;
                const std::string_view lanes = LaneNoun(body.substr(operation.size()));
                if (!lanes.empty())
                    return Fill(verb, lanes);
            }
            static constexpr std::pair<std::string_view, std::string_view> kWidening[] = {
                { "movzx", "Zero-extends packed " }, { "movsx", "Sign-extends packed " },
            };
            for (const auto& [operation, verb] : kWidening)
            {
                const std::string_view rest = body.substr(0, operation.size()) == operation ? body.substr(operation.size()) : std::string_view();
                if (rest.size() == 2 && !SizeName(rest[0]).empty() && !SizeName(rest[1]).empty())
                    return std::string(verb) + std::string(SizeName(rest[0])) + "s to " + std::string(SizeName(rest[1])) + "s.";
            }
            static constexpr std::pair<std::string_view, std::string_view> kNarrowing[] = {
                { "movus", ", with unsigned saturation." }, { "movs", ", with signed saturation." }, { "mov", ", cutting off the upper bits." },
            };
            for (const auto& [operation, tail] : kNarrowing)
            {
                if (body.substr(0, operation.size()) != operation)
                    continue;
                const std::string_view rest = body.substr(operation.size());
                if (rest.size() == 2 && !SizeName(rest[0]).empty() && !SizeName(rest[1]).empty())
                    return "Narrows " + std::string(SizeName(rest[0])) + "s to " + std::string(SizeName(rest[1])) + "s" + std::string(tail);
                if (operation == "mov" && rest.size() == 3 && rest.substr(1) == "2m" && !SizeName(rest[0]).empty())
                    return "Copies the sign bit of each " + std::string(SizeName(rest[0])) + " into a mask register.";
                if (operation == "mov" && rest.size() == 3 && rest.substr(0, 2) == "m2" && !SizeName(rest[2]).empty())
                    return "Sets each " + std::string(SizeName(rest[2])) + " to all ones where the mask bit is set, else to zero.";
            }
            static constexpr std::pair<std::string_view, std::string_view> kUnpack[] = { { "unpckl", "low" }, { "unpckh", "high" } };
            for (const auto& [operation, half] : kUnpack)
            {
                if (body.substr(0, operation.size()) != operation)
                    continue;
                const std::string_view rest = body.substr(operation.size());
                const std::string_view from = rest == "bw" ? "bytes" : rest == "wd" ? "words" : rest == "dq" ? "dwords" : rest == "qdq" ? "qwords" : std::string_view();
                if (!from.empty())
                    return "Interleaves the " + std::string(half) + " " + std::string(from) + " of both operands.";
            }
            if (name.substr(0, 6) == "popcnt")
            {
                if (const std::string_view lanes = LaneNoun(name.substr(6)); !lanes.empty())
                    return "Counts the set bits in each of the " + std::string(lanes) + ".";
            }
            if (name.substr(0, 4) == "perm")
            {
                std::string_view rest = name.substr(4);
                const bool twoTables = rest.substr(0, 2) == "i2" || rest.substr(0, 2) == "t2";
                if (twoTables)
                    rest.remove_prefix(2);
                const std::string_view lanes = LaneNoun(rest);
                if (!lanes.empty())
                    return "Picks " + std::string(lanes) + (twoTables ? " from two tables by indexes." : " by indexes.");
            }
            if (body.substr(0, 3) == "dpb" || body.substr(0, 3) == "dpw")
                return std::string("Multiplies groups of ") + (body[2] == 'b' ? "bytes" : "words") + " and adds the products into dwords (dot product, AVX-VNNI)" +
                       (body.back() == 's' ? ", with saturation." : ".");
            return {};
        }

        std::string X87Arithmetic(std::string_view name)
        {
            if (name.size() < 4 || name[0] != 'f')
                return {};
            std::string_view body = name.substr(1);
            const bool integer = body[0] == 'i';
            if (integer)
                body.remove_prefix(1);
            bool pop = false;
            if (!integer && body.size() > 3 && body.back() == 'p')
            {
                pop = true;
                body.remove_suffix(1);
            }
            struct Operation
            {
                std::string_view Name;
                std::string_view Float;
                std::string_view Integer;
            };
            static constexpr Operation kOperations[] = {
                { "add", "Adds two x87 values", "Adds an integer from memory to st0" },
                { "sub", "Subtracts the source from the destination (x87)", "Subtracts an integer from memory from st0" },
                { "subr", "Subtracts the destination from the source, into the destination (x87)", "Subtracts st0 from an integer in memory, into st0" },
                { "mul", "Multiplies two x87 values", "Multiplies st0 by an integer from memory" },
                { "div", "Divides the destination by the source (x87)", "Divides st0 by an integer from memory" },
                { "divr", "Divides the source by the destination, into the destination (x87)", "Divides an integer from memory by st0, into st0" },
            };
            for (const Operation& operation : kOperations)
            {
                if (body == operation.Name)
                    return std::string(integer ? operation.Integer : operation.Float) + (pop ? " and pops." : ".");
            }
            return {};
        }

        struct Family
        {
            std::string_view Prefix;
            std::string_view Text;
        };

        constexpr Family kFamilies[] = {
            { "aes", "One step of AES encryption or decryption (AES-NI)." },
            { "sha", "One step of a SHA hash (SHA extensions)." },
            { "sm3", "One step of the SM3 hash." },
            { "sm4", "One step of the SM4 cipher." },
            { "gf2p8", "Galois field GF(2^8) arithmetic (GFNI), used by crypto code and bit tricks." },
            { "encodekey", "Wraps an AES key into a handle (Key Locker)." },
            { "loadiwkey", "Loads the Key Locker wrapping key; privileged." },
            { "xsave", "Saves the extended processor state (x87, SSE, AVX…) to memory." },
            { "xrstor", "Restores the extended processor state from memory." },
            { "fxsave", "Saves the x87 and SSE state to memory." },
            { "fxrstor", "Restores the x87 and SSE state from memory." },
            { "tile", "AMX tile (matrix register) instruction." },
            { "tdp", "AMX tile dot product." },
            { "ldtilecfg", "Loads the AMX tile configuration." },
            { "sttilecfg", "Stores the AMX tile configuration." },
            { "bnd", "MPX bounds checking (obsolete)." },
            { "xcrypt", "VIA PadLock encryption." },
            { "xsha", "VIA PadLock hash." },
            { "xstore", "VIA PadLock random number." },
            { "montmul", "VIA PadLock Montgomery multiplication." },
            { "lwp", "AMD lightweight profiling." },
            { "llwpcb", "AMD lightweight profiling: loads the control block." },
            { "slwpcb", "AMD lightweight profiling: stores the control block." },
            { "encl", "Intel SGX enclave instruction." },
            { "seam", "Intel TDX module instruction; privileged." },
            { "tdcall", "Calls the Intel TDX module from a trust domain." },
            { "pconfig", "Configures platform features such as memory encryption; privileged." },
            { "pvalidate", "Validates guest memory (AMD SEV-SNP)." },
            { "rmp", "Updates the reverse map table (AMD SEV-SNP); privileged." },
            { "psmash", "Splits a large page in the reverse map table (AMD SEV-SNP); privileged." },
            { "rdmsr", "Reads a model-specific register; privileged." },
            { "wrmsr", "Writes a model-specific register; privileged." },
            { "urdmsr", "Reads a model-specific register from user code, if allowed." },
            { "uwrmsr", "Writes a model-specific register from user code, if allowed." },
            { "wrss", "Writes to the shadow stack (CET)." },
            { "wruss", "Writes to the user shadow stack from the kernel (CET)." },
            { "rdssp", "Reads the shadow stack pointer (CET)." },
            { "incssp", "Moves the shadow stack pointer (CET)." },
            { "setssbsy", "Marks the shadow stack as busy (CET); privileged." },
            { "clrssbsy", "Marks a shadow stack as free (CET); privileged." },
            { "saveprevssp", "Saves the previous shadow stack pointer (CET)." },
            { "rstorssp", "Switches to another shadow stack (CET)." },
            { "clui", "Disables user interrupts." },
            { "stui", "Enables user interrupts." },
            { "testui", "Tests whether user interrupts are enabled." },
            { "senduipi", "Sends a user interrupt to another thread." },
            { "uiret", "Returns from a user interrupt handler." },
            { "erets", "Returns from an event handler to the same privilege level (FRED)." },
            { "eretu", "Returns from an event handler to user code (FRED)." },
            { "enqcmd", "Sends a command to a shared work queue of a device." },
            { "getsec", "Safer-mode extensions (TXT); privileged." },
            { "invept", "Discards cached guest translations (VMX); privileged." },
            { "invvpid", "Discards cached translations of a virtual processor (VMX); privileged." },
            { "invlpga", "Discards a guest TLB entry (AMD SVM); privileged." },
            { "clgi", "Clears the global interrupt flag (AMD SVM); privileged." },
            { "stgi", "Sets the global interrupt flag (AMD SVM); privileged." },
            { "skinit", "Starts a secure launch (AMD SVM); privileged." },
            { "mcommit", "Waits until earlier stores are committed to memory (AMD)." },
            { "spflt", "Knights Corner instruction." },
            { "clevict", "Evicts a cache line (Knights Corner)." },
            { "jknzd", "Jumps if a mask register is not zero (Knights Corner)." },
            { "jkzd", "Jumps if a mask register is zero (Knights Corner)." },
            { "kmerge", "Merges mask registers (Knights Corner)." },
            { "kextract", "Extracts a mask register part (Knights Corner)." },
            { "kconcat", "Joins mask registers (Knights Corner)." },
            { "kandnr", "ANDs a mask with an inverted mask (Knights Corner)." },
            { "pf", "3DNow! instruction (old AMD SIMD on MMX registers)." },
            { "pi2f", "3DNow! conversion of integers to floats." },
            { "pswapd", "Swaps the two dwords of an MMX register (3DNow!)." },
            { "pavgusb", "Averages unsigned bytes (3DNow!)." },
            { "pmulhrw", "Multiplies words and rounds the high half (3DNow!)." },
            { "pbndkb", "Bundles an AES key into a handle (Key Locker)." },
            { "blc", "AMD TBM bit manipulation around the lowest clear bit." },
            { "blsfill", "AMD TBM: sets every bit below the lowest set bit." },
            { "blsic", "AMD TBM: inverts the lowest set bit mask." },
            { "t1mskc", "AMD TBM: masks the trailing ones." },
            { "tzmsk", "AMD TBM: masks the trailing zeros." },
            { "tzcnti", "Counts trailing zeros (Knights Corner)." },
        };

        constexpr Family kWideFamilies[] = {
            { "vm", "" },
            { "vpcom", "Compares lanes by a predicate in the immediate (AMD XOP)." },
            { "vprot", "Rotates lanes by their own counts (AMD XOP)." },
            { "vpsha", "Shifts lanes arithmetically by their own counts (AMD XOP)." },
            { "vpshl", "Shifts lanes logically by their own counts (AMD XOP)." },
            { "vpmacs", "Multiplies and accumulates lanes (AMD XOP)." },
            { "vpmadcs", "Multiplies and accumulates word pairs (AMD XOP)." },
            { "vphadd", "Adds neighbouring lanes into wider lanes (AMD XOP)." },
            { "vphsub", "Subtracts neighbouring lanes into wider lanes (AMD XOP)." },
            { "vfrcz", "Extracts the fraction of floating-point values (AMD XOP)." },
            { "v4f", "Four fused multiply-adds in a row (Xeon Phi 4FMAPS)." },
            { "vp4dp", "Four word dot products in a row (Xeon Phi 4VNNIW)." },
            { "vexp2", "Approximates 2 to the power of each lane (Xeon Phi AVX-512 ER)." },
            { "vexp223", "Approximates 2 to the power of each lane (Knights Corner)." },
            { "vrcp23", "Approximates the reciprocal of each lane (Knights Corner)." },
            { "vrsqrt23", "Approximates the reciprocal square root of each lane (Knights Corner)." },
            { "vlog2", "Approximates the base-2 logarithm of each lane (Knights Corner)." },
            { "vgatherpf", "Prefetches scattered addresses for a gather (Xeon Phi)." },
            { "vscatterpf", "Prefetches scattered addresses for a scatter (Xeon Phi)." },
            { "vloadunpack", "Loads and unpacks lanes (Knights Corner)." },
            { "vpackstore", "Packs and stores lanes (Knights Corner)." },
            { "vprefetch", "Prefetches memory (Knights Corner)." },
            { "vmovnr", "Stores without reading the line first (Knights Corner)." },
            { "vfixupnan", "Fixes NaN lanes (Knights Corner)." },
            { "vgmax", "Takes a special maximum of lanes (Knights Corner)." },
            { "vgmin", "Takes a special minimum of lanes (Knights Corner)." },
            { "vaddn", "Adds and negates lanes (Knights Corner)." },
            { "vsubr", "Subtracts in reverse order (Knights Corner)." },
            { "vscaleps", "Scales floats by powers of two (Knights Corner)." },
            { "vrndfxpnt", "Rounds fixed-point values (Knights Corner)." },
            { "vcvtfxpnt", "Converts fixed-point values (Knights Corner)." },
            { "vpadcd", "Adds dwords with carry (Knights Corner)." },
            { "vpsbbd", "Subtracts dwords with borrow (Knights Corner)." },
            { "vpsbbrd", "Subtracts dwords in reverse with borrow (Knights Corner)." },
            { "vpsubr", "Subtracts dwords in reverse order (Knights Corner)." },
            { "vpaddset", "Adds dwords and sets mask bits (Knights Corner)." },
            { "vpsubset", "Subtracts dwords and sets mask bits (Knights Corner)." },
            { "vpsubrset", "Subtracts dwords in reverse and sets mask bits (Knights Corner)." },
            { "vaddset", "Adds floats and sets mask bits (Knights Corner)." },
            { "vfmadd233", "Multiply-add with broadcast scale and bias (Knights Corner)." },
            { "vpmadd23", "Multiply-adds dwords (Knights Corner)." },
            { "vpcmpltd", "Compares dwords for less-than (Knights Corner)." },
            { "vpermf32x4", "Picks 128-bit blocks (Knights Corner)." },
            { "vbcstne", "Loads and broadcasts a 16-bit float, converted to float (AVX-NE-CONVERT)." },
            { "vcvtneebf16", "Converts the even bfloat16 values to floats (AVX-NE-CONVERT)." },
            { "vcvtneobf16", "Converts the odd bfloat16 values to floats (AVX-NE-CONVERT)." },
            { "vcvtneeph", "Converts the even half floats to floats (AVX-NE-CONVERT)." },
            { "vcvtneoph", "Converts the odd half floats to floats (AVX-NE-CONVERT)." },
            { "vp2intersect", "Finds pairs of equal lanes in two registers into two masks." },
            { "vdpbf16", "Dot product of bfloat16 pairs added into floats." },
            { "vcvtne2ps2bf16", "Converts the floats of two sources to bfloat16 values." },
        };

        constexpr std::string_view kVirtualization[] = { "vmcall", "vmclear", "vmfunc", "vmlaunch", "vmload", "vmmcall", "vmptrld", "vmptrst", "vmread", "vmresume",
            "vmrun", "vmsave", "vmwrite", "vmxoff", "vmxon" };

        std::string Contained(std::string_view name)
        {
            static constexpr std::pair<std::string_view, std::string_view> kParts[] = {
                { "broadcast", "Copies one element or block into every lane." }, { "extract", "Copies a 128- or 256-bit block out of a wider register." },
                { "insert", "Inserts a 128- or 256-bit block into a wider register." }, { "shuff", "Picks 128-bit blocks from two sources." },
                { "shufi", "Picks 128-bit blocks from two sources." }, { "compress", "Packs together the lanes selected by a mask." },
                { "expand", "Spreads values into the lanes selected by a mask." },
            };
            for (const auto& [part, text] : kParts)
            {
                if (name.find(part) != std::string_view::npos)
                    return std::string(text);
            }
            return {};
        }
    }

    namespace
    {
        bool Known(const std::string& mnemonic)
        {
            static const std::unordered_set<std::string> known = []
            {
                const std::vector<std::string> names = KnownMnemonics();
                return std::unordered_set<std::string>(names.begin(), names.end());
            }();
            return known.count(mnemonic) > 0;
        }

        std::string Direct(std::string_view name)
        {
            if (const auto found = Plain().find(name); found != Plain().end())
                return std::string(found->second);
            for (std::string (*generator)(std::string_view) : { &Conditional, &StringOperation, &MaskRegister, &FusedMultiply, &FloatLanes, &Conversion, &PackedInteger,
                     &X87Arithmetic })
            {
                if (std::string text = generator(name); !text.empty())
                    return text;
            }
            return {};
        }
    }

    std::string MnemonicSummary(std::string_view mnemonic)
    {
        const std::string name = Lower(Trim(mnemonic));
        if (name.empty())
            return {};
        if (std::string text = Direct(name); !text.empty())
            return text;
        if (name.size() > 1 && name[0] == 'v')
        {
            for (const std::string_view machine : kVirtualization)
            {
                if (name == machine)
                    return "Hardware virtualization (VMX or AMD SVM) instruction, used by hypervisors; privileged.";
            }
            const std::string inner = MnemonicSummary(std::string_view(name).substr(1));
            if (!inner.empty())
                return Known(name.substr(1)) ? "AVX form of `" + name.substr(1) + "`: " + inner : inner;
            for (const Family& family : kWideFamilies)
            {
                if (!family.Text.empty() && name.compare(0, family.Prefix.size(), family.Prefix) == 0)
                    return std::string(family.Text);
            }
        }
        std::size_t digits = name.size();
        while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1])))
            --digits;
        const std::string_view width = std::string_view(name).substr(digits);
        if (digits > 2 && (width == "8" || width == "16" || width == "32" || width == "64"))
        {
            const std::string inner = Direct(std::string_view(name).substr(0, digits));
            if (!inner.empty())
                return "AVX-512 form of `" + name.substr(0, digits) + "` for " + std::string(width) + "-bit elements, with a mask: " + inner;
        }
        for (const Family& family : kFamilies)
        {
            if (name.compare(0, family.Prefix.size(), family.Prefix) == 0)
                return std::string(family.Text);
        }
        if (name[0] == 'v')
        {
            for (const Family& family : kFamilies)
            {
                if (name.compare(1, family.Prefix.size(), family.Prefix) == 0)
                    return std::string(family.Text);
            }
        }
        return Contained(name);
    }
}
