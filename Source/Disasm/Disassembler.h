#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;
    class Symbols;
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
        std::vector<Instruction> ReadMany(const Target::Process& process, std::uint64_t address, int count, const Target::Symbols* symbols = nullptr) const;
        std::vector<Instruction> DecodeMany(const std::vector<std::uint8_t>& buffer, const std::vector<bool>& readable, std::uint64_t address, int count,
            const Target::Symbols* symbols = nullptr) const;

        std::optional<std::uint64_t> EffectiveAddress(const Instruction& instruction, const RegisterValues& registers) const;
        std::optional<std::uint64_t> PreviousStart(const Target::Process& process, std::uint64_t address) const;
        std::optional<std::uint64_t> PreviousStart(const std::uint8_t* buffer, std::size_t size, std::uint64_t base, std::uint64_t address) const;
        std::uint64_t BackUp(const Target::Process& process, std::uint64_t address, int lines) const;

        static std::string RegisterName(int zydisRegister);

    private:
        struct State;
        State* m_state = nullptr;
        bool m_is64Bit = true;
    };
}
