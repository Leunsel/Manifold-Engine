#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Process;
    class Symbols;
    enum class AddressStyle;
}

namespace Manifold::Disasm
{
    enum class Flow
    {
        None,
        Jump,
        ConditionalJump,
        Call,
        Return,
        Interrupt
    };

    struct MemoryOperand
    {
        bool Present = false;
        bool Read = false;
        bool Written = false;
        int Size = 0;
        int Base = 0;
        int Index = 0;
        int Scale = 0;
        std::int64_t Displacement = 0;
        int Segment = 0;
        bool RipRelative = false;
        std::optional<std::uint64_t> Absolute;
    };

    struct Instruction
    {
        std::uint64_t Address = 0;
        std::uint8_t Length = 0;
        std::array<std::uint8_t, 16> Bytes{};
        bool Valid = false;
        std::string Mnemonic;
        std::string Operands;
        Flow Kind = Flow::None;
        std::optional<std::uint64_t> Target;
        MemoryOperand Memory;
        bool IsNop = false;
        std::uint8_t InlineBytes = 0;

        std::string Text() const { return Operands.empty() ? Mnemonic : Mnemonic + " " + Operands; }
        std::string BytesText() const;
        std::uint64_t Next() const { return Address + (Length ? Length : 1); }
    };

    struct RegisterValues
    {
        std::uint64_t Rax = 0, Rbx = 0, Rcx = 0, Rdx = 0, Rsi = 0, Rdi = 0, Rbp = 0, Rsp = 0;
        std::uint64_t R8 = 0, R9 = 0, R10 = 0, R11 = 0, R12 = 0, R13 = 0, R14 = 0, R15 = 0;
        std::uint64_t Rip = 0;
        std::uint64_t Flags = 0;
        std::uint64_t FsBase = 0, GsBase = 0;
    };

    // The general registers as text lines for a hit or a break, named for the target:
    // RAX ... R15 and RIP in a 64-bit process, EAX ... ESP and EIP with 8 digits in a
    // 32-bit one, which has no R8 to R15 and no 64-bit registers at all.
    std::vector<std::string> RegisterLines(const RegisterValues& registers, bool is64Bit);

    enum class ConditionKind
    {
        None,
        Jump,
        Move,
        Set
    };

    struct ConditionInfo
    {
        ConditionKind Kind = ConditionKind::None;
        bool Taken = false;
        std::uint32_t FlagsRead = 0;
        bool UsesCounter = false;
    };

    struct OperandFact
    {
        std::string Text;
        bool Read = false;
        bool Written = false;
        bool Memory = false;
        bool Immediate = false;
        int Size = 0;
    };

    struct InstructionFacts
    {
        std::string Category;
        std::string IsaSet;
        std::vector<OperandFact> Operands;
        std::uint32_t FlagsTested = 0;
        std::uint32_t FlagsModified = 0;
        std::uint32_t FlagsCleared = 0;
        std::uint32_t FlagsSet = 0;
        std::uint32_t FlagsUndefined = 0;
    };

    enum class FieldKind : std::uint8_t
    {
        Displacement,
        Immediate,
        Relative
    };

    // A number encoded in an instruction: where its bytes sit and what it is.
    struct InstructionField
    {
        std::uint8_t Offset = 0;
        std::uint8_t Size = 0;
        FieldKind Kind = FieldKind::Immediate;
        bool RipRelative = false;
        bool Absolute = false;
        std::int64_t Value = 0;
    };

    // The parts of an encoded instruction: prefixes (legacy, REX, VEX/EVEX/XOP),
    // the opcode bytes, ModRM/SIB and the numbers.
    struct InstructionLayout
    {
        std::uint8_t Length = 0;
        std::uint8_t PrefixEnd = 0;
        std::uint8_t ModRm = 0xFF;
        std::uint8_t Sib = 0xFF;
        // ModRM.reg is an operand register, not an opcode extension.
        bool RegisterInReg = false;
        std::vector<InstructionField> Fields;
    };

    std::string FlagNames(std::uint32_t mask);
    std::vector<std::string> ExplainFacts(const InstructionFacts& facts);

    ConditionInfo EvaluateCondition(std::string_view mnemonic, const RegisterValues& registers, bool is64Bit);
    std::string CopyText(std::span<const Instruction> lines, const Target::Symbols& symbols, Target::AddressStyle style);

    // Instruction starts known from outside the bytes: where functions begin and
    // end (a runtime's method table, a module's unwind data) and where an
    // allocation begins. Decoding never lets an instruction run across one, and
    // backing up sweeps forward from one instead of guessing, so padding or data
    // in front of a function cannot swallow its first instructions.
    class CodeHints
    {
    public:
        virtual ~CodeHints() = default;
        // The lowest known instruction start strictly between from and to.
        virtual std::optional<std::uint64_t> BoundaryIn(std::uint64_t from, std::uint64_t to) const = 0;
        // The closest known instruction start below an address, at most reach bytes away.
        virtual std::optional<std::uint64_t> StartBelow(std::uint64_t address, std::uint64_t reach) const = 0;
    };

    class Disassembler
    {
    public:
        explicit Disassembler(bool is64Bit);
        ~Disassembler();

        Disassembler(const Disassembler&) = delete;
        Disassembler& operator=(const Disassembler&) = delete;

        bool Is64Bit() const { return m_is64Bit; }

        Instruction Decode(std::uint64_t address, const std::uint8_t* bytes, std::size_t available, const Target::Symbols* symbols = nullptr) const;
        Instruction Read(const Target::Process& process, std::uint64_t address, const Target::Symbols* symbols = nullptr) const;
        std::vector<Instruction> ReadMany(const Target::Process& process, std::uint64_t address, int count, const Target::Symbols* symbols = nullptr,
            const CodeHints* hints = nullptr) const;
        std::vector<Instruction> DecodeMany(const std::vector<std::uint8_t>& buffer, const std::vector<bool>& readable, std::uint64_t address, int count,
            const Target::Symbols* symbols = nullptr, const CodeHints* hints = nullptr) const;

        std::optional<std::uint64_t> EffectiveAddress(const Instruction& instruction, const RegisterValues& registers) const;
        std::optional<InstructionFacts> Facts(const Instruction& instruction) const;
        std::uint16_t AddressBytes(const Instruction& instruction, const std::function<bool(std::uint64_t)>& isAddress = {}) const;
        std::optional<InstructionLayout> Layout(const Instruction& instruction) const;
        std::optional<std::uint64_t> PreviousStart(const Target::Process& process, std::uint64_t address) const;
        std::optional<std::uint64_t> PreviousStart(const std::uint8_t* buffer, std::size_t size, std::uint64_t base, std::uint64_t address) const;
        // The address the given number of lines above an address. Stops only
        // where the memory above is unreadable, so a listing can be scrolled from
        // one allocation into the readable one in front of it.
        std::uint64_t BackUp(const Target::Process& process, std::uint64_t address, int lines, const CodeHints* hints = nullptr) const;
        // Where the instructions between from and to start when decoding begins
        // at from, cut at the hints the same way DecodeMany cuts them.
        std::vector<std::uint64_t> Starts(const Target::Process& process, std::uint64_t from, std::uint64_t to, const CodeHints* hints = nullptr) const;
        std::optional<std::uint64_t> InlineHolder(const Target::Process& process, std::uint64_t address) const;

        static std::string RegisterName(int zydisRegister);

    private:
        std::size_t Window(const std::vector<bool>& readable, std::size_t offset, std::uint64_t at, const CodeHints* hints) const;
        Instruction DecodeWindow(std::uint64_t address, const std::uint8_t* bytes, std::size_t available, std::size_t raw, const Target::Symbols* symbols) const;
        std::size_t StepLength(const std::uint8_t* bytes, std::size_t window, std::size_t raw) const;

        struct State;
        State* m_state = nullptr;
        bool m_is64Bit = true;
    };
}
