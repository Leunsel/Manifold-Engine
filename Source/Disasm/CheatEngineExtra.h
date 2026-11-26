#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Manifold::Disasm
{
    // What Cheat Engine's guess at the value behind an instruction reads from the target.
    struct CheatEngineMemory
    {
        bool Is64Bit = true;
        // All or nothing, like ReadProcessMemory.
        std::function<bool(std::uint64_t, void*, std::size_t)> Read;
        // As many bytes from the start as could be read (a read that runs into an unreadable page).
        std::function<std::size_t(std::uint64_t, void*, std::size_t)> ReadPrefix;
        // VirtualQueryEx says MEM_COMMIT.
        std::function<bool(std::uint64_t)> Committed;
        // getNameFromAddress: a symbol, module+offset, or plain hex when nothing names the address.
        std::function<std::string(std::uint64_t)> Name;
    };

    struct CheatEngineSplit
    {
        std::string Address;
        std::string Bytes;
        std::string Opcode;
        std::string Extra;
    };

    // Cheat Engine's Lua splitDisassembledString: the three parts of "address - bytes - opcode" and, as
    // the fourth, what CE guesses about the value the instruction refers to (tdisassembler.splitDisassembledString
    // with showvalues on). Without memory the fourth part is empty, as CE's is without a process.
    CheatEngineSplit SplitCheatEngineLine(std::string_view text, const CheatEngineMemory* memory);

    // The fourth part alone. `line` is the whole text the opcode came from.
    std::string CheatEngineExtra(std::string_view line, std::string_view opcode, const CheatEngineMemory& memory);

    // Free Pascal's FloatToStr and Format('%.4f'), the way Cheat Engine 7.6 (x86-64 build) writes them.
    std::string PascalFloatToStr(double value, bool single);
    std::string PascalFixed4(double value);
}
