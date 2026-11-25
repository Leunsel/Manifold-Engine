#pragma once

#include "Scan/ValueType.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Manifold::Target
{
    class Process;
}

namespace Manifold::Scan
{
    struct PatternProgress;
}

namespace Manifold::Disasm
{
    class Disassembler;

    enum class SignatureWildcard
    {
        Double,
        Single,
        DoubleStar,
        Star
    };

    enum class MaskMode
    {
        Keep,
        Partial,
        All
    };

    enum class SignatureScope
    {
        Module,
        Process
    };

    enum class SignatureFormat
    {
        Aob,
        CodeMask,
        ByteArray,
        AutoAssembler
    };

    enum class ByteRole : std::uint8_t
    {
        Prefix,
        Opcode,
        ModRm,
        Address,
        Branch,
        Offset,
        Immediate,
        Data
    };

    struct SignatureOptions
    {
        SignatureScope Scope = SignatureScope::Module;
        bool CodeOnly = false;
        bool MaskAddresses = true;
        // Keep, rel32 only (Partial) or every branch distance.
        MaskMode Branches = MaskMode::All;
        // Keep, 32-bit displacements only (Partial) or every displacement.
        MaskMode Offsets = MaskMode::All;
        // Keep, values of at least ImmediateThreshold (Partial) or every immediate.
        MaskMode Immediates = MaskMode::Partial;
        std::uint64_t ImmediateThreshold = 0x10000;
        // Keep, SIB bytes only (Partial: base and index registers of memory
        // operands) or SIB and ModRM bytes whose reg field is a register (All).
        MaskMode Registers = MaskMode::Partial;
        int MaxBytes = 128;
        int MinBytes = 8;
        bool StopAtFunctionEnd = false;
        bool AllowOffset = false;
        int MaxOffset = 64;
        int MinSaving = 4;
        // Searches exactly this range instead of the module or the process.
        std::optional<std::pair<std::uint64_t, std::uint64_t>> Range;
    };

    struct SignatureFormatting
    {
        SignatureWildcard Wildcard = SignatureWildcard::Double;
        bool Spaces = true;
        std::string Label = "INJECT";
    };

    struct SignatureByte
    {
        std::uint8_t Value = 0;
        bool Wild = false;
        ByteRole Role = ByteRole::Opcode;
    };

    struct SignatureLine
    {
        std::uint64_t Address = 0;
        std::uint8_t Length = 0;
        std::string Text;
    };

    struct SignatureCandidate
    {
        std::uint64_t Start = 0;
        std::int64_t Offset = 0;
        std::size_t Length = 0;
        bool Unique = false;
        bool Chosen = false;
        // Not measured to the end: it could not get shorter than the signature
        // without an offset by the minimum saving.
        bool NotShorter = false;
    };

    struct Signature
    {
        std::uint64_t Address = 0;
        std::uint64_t Start = 0;
        // Address - Start: a scan hit plus this is the address.
        std::int64_t Offset = 0;
        std::vector<SignatureByte> Bytes;
        std::vector<SignatureLine> Lines;
        std::string Module;
        std::uint64_t ModuleBase = 0;
        SignatureScope Scope = SignatureScope::Module;
        bool CodeOnly = false;
        bool Code = true;
        std::optional<std::uint64_t> FunctionEnd;
        std::vector<SignatureCandidate> Candidates;
        std::uint64_t SearchedBytes = 0;
        double Seconds = 0.0;
        bool Verified = false;
        // Code only, and the bytes are in data too: aobscan searches all memory and may find that copy first.
        bool UniqueOnlyInCode = false;

        std::size_t Wildcards() const;
        Scan::Pattern Pattern() const;
        std::string ScopeText() const;
    };

    std::optional<Signature> MakeSignature(const Target::Process& process, const Disassembler& disassembler, std::uint64_t address, const SignatureOptions& options,
        std::string& error, Scan::PatternProgress* progress = nullptr);

    const char* WildcardToken(SignatureWildcard wildcard);
    std::string SignatureText(const std::vector<SignatureByte>& bytes, const SignatureFormatting& formatting);
    std::string FormatSignature(const Signature& signature, SignatureFormat format, const SignatureFormatting& formatting);
    const char* SignatureFormatName(SignatureFormat format);
    const char* ByteRoleName(ByteRole role);

    // How long a pattern has to be before nothing but `self` matches it in the
    // given memory: the first length at which every other place differs. More
    // than the pattern's length when another place matches all of it. Exposed
    // for the tests; MakeSignature uses the same search.
    std::size_t UniqueLength(const Target::Process& process, const Scan::Pattern& pattern, std::uint64_t self, std::uint64_t low, std::uint64_t high, bool codeOnly,
        Scan::PatternProgress* progress = nullptr);

    struct ParsedSignature
    {
        Scan::Pattern Pattern;
        std::size_t Fixed = 0;
        std::size_t Wildcards = 0;
        std::size_t Partial = 0;
        std::string Source;
        std::string Module;
        std::string Text;
        std::vector<std::string> Notes;
    };

    // Reads a signature in the shapes tools hand out: "48 8B ?? ??", "48 8B ? ?",
    // "488B????", the C string with its mask ("\x48\x8B\x00", "xx?"), a C array,
    // nibble wildcards ("4?"), an Auto Assembler aobscan line, with comment and
    // header lines around it.
    std::optional<ParsedSignature> ParseSignature(std::string_view text, std::string& error);
}
