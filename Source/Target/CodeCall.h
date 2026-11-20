#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Manifold::Target
{
    class Process;

    enum class CodeCallKind
    {
        Integer,
        Float,
        Double,
        Text,
        WideText
    };

    struct CodeCallArgument
    {
        CodeCallKind Kind = CodeCallKind::Integer;
        std::uint64_t Value = 0;
        std::string Text;
        std::wstring Wide;
    };

    struct CodeCallRequest
    {
        std::uint64_t Function = 0;
        bool Cdecl = false;
        std::vector<CodeCallArgument> Arguments;
        bool HasInstance = false;
        std::uint64_t Instance = 0;
        int InstanceRegister = -1;
        bool Wait = true;
        std::uint32_t TimeoutMilliseconds = 0xFFFFFFFFu;
    };

    inline constexpr std::size_t kMaxCodeCallArguments = 32;

    Result<std::vector<std::uint8_t>> CodeCallStub(bool is64Bit, std::uint64_t address, std::uint64_t resultAddress, const CodeCallRequest& request,
        const std::vector<std::uint64_t>& values);
    Result<std::uint64_t> CallCode(const Process& process, const CodeCallRequest& request);
}
