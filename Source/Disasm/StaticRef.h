#pragma once

#include "Core/Types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Disasm
{
    // Where an instruction names a fixed address.
    enum class StaticKind
    {
        Auto,
        Memory,     // a memory operand: [rip+disp], [disp32], moffs, [disp32+index*scale]; lea too
        Branch,     // the target of a relative call, jmp, jcc or loop
        Immediate   // a number that is an address: mov rax,imm64, push imm32, mov [esi+8],imm32
    };

    enum class StaticEncoding
    {
        Auto,
        Relative,   // counted from the next instruction: rip-relative operands and branch targets
        Absolute    // written out in full: x86 [disp32], moffs, immediates
    };

    enum class StaticOutput
    {
        Address,    // the static address itself
        Pointer     // the pointer stored at the static address
    };

    struct StaticOptions
    {
        StaticKind Kind = StaticKind::Auto;
        StaticEncoding Encoding = StaticEncoding::Auto;
        StaticOutput Output = StaticOutput::Address;
        // Assertions from the older table-file command: the byte offset of the number
        // inside the instruction and the instruction's length. They select and check,
        // they never replace the decoded instruction.
        std::optional<std::uint64_t> FieldOffset;
        std::optional<std::uint64_t> Length;
        // AA only: take the first hit of an aobscan the target names even when the
        // pattern matches elsewhere too.
        bool FirstHit = false;
    };

    struct StaticRef
    {
        std::uint64_t Instruction = 0;
        std::uint8_t Length = 0;
        std::string Text;
        StaticKind Kind = StaticKind::Memory;
        bool Relative = false;
        bool Indexed = false;
        std::uint8_t FieldOffset = 0;
        std::uint8_t FieldSize = 0;
        std::int64_t FieldValue = 0;
        int OperandSize = 0;
        std::uint64_t Address = 0;
        std::optional<std::uint64_t> Pointer;
        std::uint64_t Value = 0;  // Address or Pointer, as the options asked
        std::vector<std::string> Notes;
    };

    std::string_view StaticKindName(StaticKind kind);

    // Reads one option word of resolvestatic / ManifoldResolveStatic: a kind (memory,
    // branch, immediate, auto), an encoding (rip/relative, absolute/abs32), an output
    // (address/slot, pointer/deref), "first", or a number (field offset, then length).
    // Returns false with a reason for a word it does not know or a second one of a kind.
    bool ParseStaticOption(std::string_view word, StaticOptions& options, std::string& error);

    // Decodes the instruction at the address in the process (32-bit for WOW64 targets)
    // and returns the fixed address it names. Errors are the ME-2019..2025 codes.
    Result<StaticRef> ResolveStatic(const Target::Process& process, std::uint64_t address, const StaticOptions& options);
    // The same over bytes already read, for tests; mapped says whether an address is
    // committed memory, image whether it lies in a module.
    Result<StaticRef> ResolveStaticBytes(bool is64Bit, std::uint64_t address, const std::uint8_t* bytes, std::size_t size, const StaticOptions& options,
        const std::function<bool(std::uint64_t)>& mapped, const std::function<bool(std::uint64_t)>& image,
        const std::function<std::optional<std::uint64_t>(std::uint64_t)>& readPointer);
}
