#pragma once

#include "Core/Errors.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    class Process;
    class Symbols;
}

namespace Manifold::Asm
{
    struct AaAllocation
    {
        std::string Name;
        std::uint64_t Address = 0;
        std::uint64_t Size = 0;
        std::uint64_t Block = 0;
        bool Global = false;
    };

    struct AaMemory
    {
        std::vector<AaAllocation> Allocations;
        std::vector<std::string> Registered;
        std::map<std::string, std::uint64_t> Values;
    };

    enum class AaSection
    {
        Enable,
        Disable,
        Whole
    };

    struct AaWrite
    {
        std::uint64_t Address = 0;
        std::vector<std::uint8_t> Bytes;
        int Line = 0;
    };

    struct AaOptions
    {
        AaSection Section = AaSection::Whole;
        bool CheckOnly = false;
        bool ScanInCheck = false;
        bool Log = false;           // every command writes what it did to the log ({$log} in the script does the same)
        std::string LogTitle;       // what the log lines call the script, such as the record's description
        std::string Owner;
        std::filesystem::path IncludeFolder;
    };

    struct AaLogLine
    {
        int Line = 0;
        std::string Command;  // the command as written, such as aobscanmodule(INJECT) or a label line
        std::string Text;     // what it did
    };

    // Something that assembles but will likely go wrong in the game, such as
    // a hook that leaves the stack unbalanced.
    struct AaWarning
    {
        int Line = 0;
        std::string Text;
    };

    struct AaResult
    {
        Status Error;
        int Line = 0;
        bool Logged = false;
        std::vector<AaLogLine> Log;  // the steps when the run logged, at most kMaxLogLines of them
        std::vector<AaWrite> Writes;
        std::vector<std::string> Notes;
        std::vector<AaWarning> Warnings;
        std::map<std::string, std::uint64_t> Symbols;
        std::map<std::string, std::uint64_t> SymbolSizes;
        std::map<std::string, std::uint64_t> Labels;  // every label the run placed, by its name as written, once the code is in the game
        // What db/dw/dd/dq lines wrote into allocated memory, one [begin, end) per line: data, not code (readmem stays code).
        std::vector<std::pair<std::uint64_t, std::uint64_t>> DataRanges;

        bool Ok() const { return Error.Ok(); }
    };

    struct AaHost
    {
        Target::Process* Process = nullptr;
        Target::Symbols* Symbols = nullptr;
        std::function<Status(std::string_view code, bool syntaxCheck, std::string& output)> RunLua;
        std::function<bool(std::string_view name)> IsCustomCommand;
        std::function<Status(std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output)> RunCustomCommand;
    };

    inline constexpr std::size_t kMaxLogLines = 400;

    bool WantsLog(std::string_view script);
    // The steps as an aligned table (line, command, result), one row per step.
    std::string FormatLog(const std::vector<AaLogLine>& steps);
    AaResult RunAutoAssembler(std::string_view script, AaHost& host, AaMemory& memory, const AaOptions& options);

    bool HasSections(std::string_view script);
    std::string SectionText(std::string_view script, AaSection section);

    std::string CheatTableTemplate();
}
