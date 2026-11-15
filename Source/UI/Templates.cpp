#include "UI/Templates.h"

#include "Asm/AutoAssembler.h"
#include "Core/Text.h"
#include "Scan/Scanner.h"

namespace Manifold::UI
{
    namespace
    {
        constexpr int kJumpSize = 5;
        constexpr int kMaxPattern = 64;
        constexpr int kContextBefore = 8;
        constexpr int kContextAfter = 8;
    }

    std::optional<std::string> MakeTemplate(Engine& engine, TemplateKind kind, std::uint64_t address, const std::string& name, std::string& error)
    {
        if (kind == TemplateKind::CheatTable)
            return Asm::CheatTableTemplate();
        if (kind == TemplateKind::LuaCall)
            return Asm::LuaCallTemplate();
        if (!engine.HasProcess())
        {
            error = ErrorMessage(ErrorCode::NoProcess);
            return std::nullopt;
        }
        const Target::Process& process = *engine.Process();
        const Disasm::Disassembler& disassembler = engine.Disassembler();

        Asm::TemplateInput input;
        input.Address = address;
        input.AddressText = engine.Symbols().ModuleOffset(address);
        input.Is64Bit = process.Is64Bit();
        input.Name = name.empty() ? "INJECT" : name;
        if (const auto module = process.ModuleAt(address))
        {
            input.ModuleName = module->Name;
            input.ModuleBase = module->Base;
        }

        std::uint64_t at = address;
        while (input.OriginalBytes.size() < static_cast<std::size_t>(kJumpSize))
        {
            const Disasm::Instruction instruction = disassembler.Read(process, at, &engine.Symbols());
            if (!instruction.Valid)
            {
                error = "The code at " + Hex(at) + " could not be disassembled.";
                return std::nullopt;
            }
            input.OriginalLines.push_back(instruction.Text());
            input.OriginalBytes.insert(input.OriginalBytes.end(), instruction.Bytes.begin(), instruction.Bytes.begin() + instruction.Length);
            at += instruction.Length;
        }

        if (kind == TemplateKind::AobInjection)
        {
            std::uint64_t start = input.ModuleBase ? input.ModuleBase : 0;
            std::uint64_t stop = input.ModuleBase ? input.ModuleBase + process.ModuleAt(address)->Size : process.HighestAddress();
            std::vector<std::uint8_t> bytes(kMaxPattern);
            const std::size_t got = process.Read(address, bytes.data(), bytes.size());
            bytes.resize(got);
            std::size_t length = input.OriginalBytes.size();
            bool unique = false;
            while (length <= bytes.size())
            {
                Scan::Pattern pattern;
                pattern.Bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
                pattern.Mask.assign(length, 0xFF);
                const auto hits = Scan::FindPattern(process, pattern, start, stop, false, 2);
                if (hits.size() == 1)
                {
                    unique = true;
                    break;
                }
                length += 2;
            }
            length = std::min(length, bytes.size());
            input.Pattern = HexBytes(std::span<const std::uint8_t>(bytes.data(), length));
            if (!unique)
                input.Pattern += " // not unique within 64 bytes";
            if (input.ModuleName.empty())
                input.ModuleName.clear();
        }

        std::uint64_t contextStart = disassembler.BackUp(process, address, kContextBefore);
        const auto before = disassembler.ReadMany(process, contextStart, kContextBefore + kContextAfter + static_cast<int>(input.OriginalLines.size()), &engine.Symbols());
        for (const Disasm::Instruction& instruction : before)
        {
            std::string line = engine.Symbols().ModuleOffset(instruction.Address) + ": " + instruction.BytesText();
            line.resize(std::max<std::size_t>(line.size(), 44), ' ');
            line += "- " + instruction.Text();
            if (instruction.Address == address)
                line = "// ---------- INJECTING HERE ----------\n" + line;
            if (instruction.Address == at)
                line = "// ---------- DONE INJECTING  ----------\n" + line;
            input.ContextLines.push_back(line);
        }

        switch (kind)
        {
        case TemplateKind::AobInjection: return Asm::AobInjectionTemplate(input);
        case TemplateKind::CodeInjection: return Asm::CodeInjectionTemplate(input);
        case TemplateKind::FullInjection: return Asm::FullInjectionTemplate(input);
        default: break;
        }
        return std::nullopt;
    }
}
