#pragma once

#include "Asm/AutoAssembler.h"
#include "Target/CrashWatch.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Manifold
{
    namespace Disasm
    {
        class Disassembler;
    }

    enum class ScriptRangeKind
    {
        Allocation,
        Patch
    };

    struct ScriptRange
    {
        std::uint64_t Begin = 0;
        std::uint64_t End = 0;
        ScriptRangeKind Kind = ScriptRangeKind::Allocation;
        std::string Name;  // the allocation's name; empty for a patch
    };

    // Data inside a script's allocation: a db/dw/dd/dq line, or the part of
    // the allocation nothing was written to (ZeroOnly: data while it is zero).
    struct ScriptData
    {
        std::uint64_t Begin = 0;
        std::uint64_t End = 0;
        bool ZeroOnly = false;
    };

    // What one script put into the game: its allocations (code caves, data)
    // and the bytes it patched outside of them, with its labels for names.
    struct ScriptFootprint
    {
        std::string Label;
        int RecordId = 0;
        std::uint64_t Key = 0;      // the script text's hash: one record's Lua may run many scripts
        std::vector<ScriptRange> Ranges;
        std::map<std::uint64_t, std::string> Labels;
        std::vector<Asm::AaWarning> Warnings;  // what the Auto Assembler found wrong with it, such as an unbalanced stack
        std::vector<ScriptData> Data;          // sorted by address
        std::uint64_t OnTick = 0;   // GetTickCount64 when its [ENABLE] ran
        std::uint64_t OffTick = 0;  // when its [DISABLE] ran; 0 while it is on
    };

    // The scripts' footprints for one process. Every Auto Assembler run that
    // changes the game notes itself here, from any thread.
    class CrashLedger
    {
    public:
        static constexpr std::size_t kMaxFootprints = 256;

        void Reset();
        void Note(std::string label, int recordId, std::uint64_t key, Asm::AaSection section, const Asm::AaResult& result, const Asm::AaMemory& before,
            const Asm::AaMemory& after, std::uint64_t tick);
        std::vector<ScriptFootprint> Footprints() const;
        // The data span of a script that is on at the address, for the disassembler.
        std::optional<ScriptData> DataAt(std::uint64_t address) const;

    private:
        mutable std::mutex m_lock;
        std::vector<ScriptFootprint> m_footprints;
    };

    enum class CrashLink
    {
        InCode,      // the instruction that failed is in the script's code
        InPatch,     // ... in bytes the script patched
        BehindPatch, // ... right behind a patch: a jump back landed in the middle of an instruction
        RanFreed,    // the code ran in an allocation after the script was switched off
        Unbalanced,  // the script leaves the stack unbalanced: the game's code after its hook pops the wrong values
        Accessed,    // the faulting access reached memory of the script
        OnStack,     // the script's code is on the stack: it called what failed
        NearPatch,   // the game's code failed shortly behind the script's patch
        Earlier      // an earlier exception, shortly before, was in the script's code
    };

    const char* CrashLinkText(CrashLink link);

    struct CrashBlame
    {
        std::string Label;
        int RecordId = 0;
        CrashLink Link = CrashLink::InCode;
        std::uint64_t Address = 0;
        std::string Where;
        int Weight = 0;
    };

    struct CrashReport
    {
        bool Crashed = false;
        bool Watched = false;
        std::uint32_t ExitCode = 0;
        std::optional<Target::CrashRecord> Fatal;
        bool Exact = false;                 // the exit code is the noted exception's code
        double SecondsBeforeEnd = 0.0;
        std::vector<CrashBlame> Blames;     // most likely first, one per script
        std::vector<std::string> SwitchedOn;
        std::string LastSwitched;
        double LastSwitchedSeconds = 0.0;
        std::string Process;
        std::string Headline;
        std::string Details;
    };

    struct CrashContext
    {
        std::uint32_t ExitCode = 0;
        std::uint64_t EndTick = 0;
        std::string Process;
        std::vector<ScriptFootprint> Footprints;
        std::function<std::string(std::uint64_t)> Describe;  // module+offset or a symbol; may be empty
        const Disasm::Disassembler* Disassembler = nullptr;
    };

    // The name a run goes by in the crash report: the record's description,
    // or for a script run from Lua the record whose Lua ran it.
    std::string ScriptLabel(std::string_view title, std::string_view owner);

    bool ExceptionExitCode(std::uint32_t code);
    std::string ExceptionName(std::uint32_t code);
    // "access violation reading 0000000000000010" and the like.
    std::string ExceptionText(const Target::CrashRecord& record, bool is64Bit);

    CrashReport AttributeCrash(const Target::CrashCapture& capture, const CrashContext& context);

    // The script whose memory holds the address now (its label and where in
    // it), for a debugger stop.
    std::optional<CrashBlame> ScriptAt(const std::vector<ScriptFootprint>& footprints, std::uint64_t address, std::uint64_t tick,
        const std::function<std::string(std::uint64_t)>& describe);
    // The memory of the scripts that are on, merged, at most `limit` ranges.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ScriptRegions(const std::vector<ScriptFootprint>& footprints, std::size_t limit);
}
