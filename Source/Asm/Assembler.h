#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Asm
{
    enum class AsmError
    {
        None,
        Empty,
        Syntax,
        BadNumber,
        UnknownMnemonic,
        BadOperand,
        UnknownSymbol,
        OutOfRange,
        EncodeFailed
    };

    enum class SymbolState
    {
        Unknown,
        Pending,
        Known
    };

    struct SymbolValue
    {
        SymbolState State = SymbolState::Unknown;
        std::uint64_t Value = 0;
    };

    using SymbolResolver = std::function<SymbolValue(std::string_view name)>;

    struct AssembleOptions
    {
        bool Is64Bit = true;
        std::uint64_t Address = 0;
        SymbolResolver Resolve;
        bool AllowPending = false;
        bool AllowFarBranches = true;
        bool DefaultToDword = false;
        bool LongBranches = false;
    };

    struct AssembleResult
    {
        bool Ok = false;
        std::vector<std::uint8_t> Bytes;
        AsmError Error = AsmError::None;
        std::string Message;
        bool UsedPending = false;
    };

    AssembleResult Assemble(std::string_view line, const AssembleOptions& options);

    std::optional<std::uint64_t> ParseNumber(std::string_view text);

    std::string_view ErrorName(AsmError error);
}
