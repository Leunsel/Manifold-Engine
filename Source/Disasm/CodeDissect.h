#pragma once

#include "Target/Process.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Manifold::Disasm
{
    enum class RefKind : std::uint8_t
    {
        Call,
        Jump,
        Conditional,
        Read,
        Write,
        Address
    };

    struct CodeRef
    {
        std::uint64_t Target = 0;
        std::uint64_t Packed = 0;

        std::uint64_t From() const { return Packed & 0x00FFFFFFFFFFFFFFull; }
        RefKind Kind() const { return static_cast<RefKind>(Packed >> 56); }
    };

    struct ReferencedString
    {
        std::uint64_t Address = 0;
        std::string Text;
        bool Wide = false;
        std::uint32_t References = 0;
    };

    struct ReferencedFunction
    {
        std::uint64_t Address = 0;
        std::uint32_t Calls = 0;
    };

    struct DissectStats
    {
        std::uint64_t CodeBytes = 0;
        std::uint64_t Instructions = 0;
        std::uint64_t Invalid = 0;
        std::uint64_t Calls = 0;
        std::uint64_t Jumps = 0;
        std::uint64_t Conditionals = 0;
        std::uint64_t Reads = 0;
        std::uint64_t Writes = 0;
        std::uint64_t Addresses = 0;
        double Seconds = 0.0;
    };

    struct DissectProgress
    {
        std::atomic<std::uint64_t> Done{ 0 };
        std::atomic<std::uint64_t> Total{ 0 };
        std::atomic<int> Phase{ 0 };
        std::atomic<bool> Cancel{ false };
    };

    struct CodeSpan
    {
        std::uint64_t Start = 0;
        std::uint64_t End = 0;
    };

    class DissectResult
    {
    public:
        std::vector<Target::ModuleInfo> Modules;
        std::vector<CodeSpan> Spans;
        std::vector<CodeRef> Refs;
        std::vector<ReferencedString> Strings;
        std::vector<ReferencedFunction> Functions;
        DissectStats Stats;
        bool Is64Bit = true;
        bool Cancelled = false;

        std::span<const CodeRef> To(std::uint64_t target) const;
        std::span<const CodeRef> Between(std::uint64_t start, std::uint64_t end) const;
        bool Covers(std::uint64_t address) const;
    };

    std::vector<CodeSpan> ExecutableSpans(const Target::Process& process, const std::vector<Target::ModuleInfo>& modules);
    std::shared_ptr<DissectResult> DissectCode(const Target::Process& process, const std::vector<Target::ModuleInfo>& modules, DissectProgress* progress = nullptr);
    const char* RefKindName(RefKind kind);
    bool ReadStringAt(std::span<const std::uint8_t> bytes, std::string& text, bool& wide);
}
