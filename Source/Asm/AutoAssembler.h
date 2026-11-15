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
        std::string Owner;
        std::filesystem::path IncludeFolder;
    };

    struct AaResult
    {
        Status Error;
        int Line = 0;
        std::vector<AaWrite> Writes;
        std::vector<std::string> Notes;
        std::map<std::string, std::uint64_t> Symbols;

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

    AaResult RunAutoAssembler(std::string_view script, AaHost& host, AaMemory& memory, const AaOptions& options);

    bool HasSections(std::string_view script);
    std::string SectionText(std::string_view script, AaSection section);

    struct TemplateInput
    {
        std::uint64_t Address = 0;
        std::string AddressText;
        std::string ModuleName;
        std::uint64_t ModuleBase = 0;
        bool Is64Bit = true;
        std::string Name;
        std::vector<std::string> OriginalLines;
        std::vector<std::uint8_t> OriginalBytes;
        std::string Pattern;
        int PatternOffset = 0;
        std::vector<std::string> ContextLines;
    };

    std::string CheatTableTemplate();
    std::string CodeInjectionTemplate(const TemplateInput& input);
    std::string AobInjectionTemplate(const TemplateInput& input);
    std::string FullInjectionTemplate(const TemplateInput& input);
    std::string LuaCallTemplate();
}
