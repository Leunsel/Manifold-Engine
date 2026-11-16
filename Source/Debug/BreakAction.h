#pragma once

#include "Core/Errors.h"
#include "Debug/BreakProgram.h"
#include "Disasm/Disassembler.h"

#include <array>
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

namespace Manifold::Debug
{
    using SymbolResolver = std::function<std::optional<std::uint64_t>(std::string_view expression)>;

    struct RegisterChange
    {
        std::uint8_t Target = 0;
        std::uint8_t Part = 0;
        std::vector<std::uint8_t> Code;
    };

    struct BreakAction
    {
        std::string Condition;
        std::string Changes;
        bool Break = true;
        std::uint32_t ThreadId = 0;
        std::vector<std::uint8_t> ConditionCode;
        std::vector<RegisterChange> ChangeCode;

        bool Plain() const { return ConditionCode.empty() && ChangeCode.empty() && Break && ThreadId == 0; }
    };

    Status CompileExpression(std::string_view text, bool is64Bit, const SymbolResolver& resolve, std::vector<std::uint8_t>& code);
    Status CompileChanges(std::string_view text, bool is64Bit, const SymbolResolver& resolve, std::vector<RegisterChange>& changes);
    Status CompileAction(BreakAction& action, bool is64Bit, const SymbolResolver& resolve);

    using RegisterArray = std::array<std::uint64_t, Program::kRegisterCount>;
    RegisterArray ToArray(const Disasm::RegisterValues& registers);
    void FromArray(const RegisterArray& values, Disasm::RegisterValues& registers);

    std::optional<Program::Value> Evaluate(const std::vector<std::uint8_t>& code, const Disasm::RegisterValues& registers, std::uint32_t threadId, const Target::Process& process);
    bool ConditionHolds(const BreakAction& action, const Disasm::RegisterValues& registers, std::uint32_t threadId, const Target::Process& process);
    bool ApplyChanges(const BreakAction& action, Disasm::RegisterValues& registers, std::uint32_t threadId, const Target::Process& process);
    std::string ValueText(const Program::Value& value);
    std::string DescribeAction(const BreakAction& action);
}
