#pragma once

#include "Core/Errors.h"
#include "Templates/TemplateLibrary.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Manifold
{
    class Engine;
}

namespace Manifold::Templates
{
    struct Prompts
    {
        std::function<std::optional<std::string>(const std::string& title, const std::string& message, const std::string& value)> Ask;
    };

    struct GenerationOptions
    {
        bool AskForHookName = true;
        bool AskForInjectionAddress = false;
        std::string AllocationSize = "$1000";
        bool AllocationNear = true;
        std::string DefaultHookName = "Injection";
        std::string AppendToHookName = "Hook";
        int InjectionInfoLines = 3;
        bool InjectionInfoRemoveSpaces = true;
        bool InjectionInfoAddTabs = true;
    };

    struct GenerationRequest
    {
        std::uint64_t Address = 0;
        bool Use14ByteJump = false;
        std::map<std::string, Value> Inputs;
        GenerationOptions Options;
        Prompts Ask;
        std::optional<std::string> Script;
        bool SampleValues = false;
    };

    struct GenerationFailure
    {
        ErrorCode Code = ErrorCode::None;
        std::string Message;
        std::string Source;
        std::string Variable;
        std::string Hint;
        int Line = 0;
        bool Cancelled = false;
    };

    struct GenerationResult
    {
        std::optional<std::string> Text;
        GenerationFailure Failure;
        std::vector<std::string> Warnings;

        bool Ok() const { return Text.has_value(); }
    };

    struct VariableInfo
    {
        const char* Name;
        const char* Provider;
        const char* Description;
    };

    GenerationResult Generate(Engine& engine, const Library& library, const TemplateInfo& info, const GenerationRequest& request);
    std::string Describe(const GenerationFailure& failure, const std::string& caption);
    std::vector<VariableInfo> Variables();
    bool IsVariable(std::string_view name);
    std::optional<Value> ParseInput(const InputDefinition& input, const std::string& text, std::string& error);
    std::string InputText(const Value& value);
}
