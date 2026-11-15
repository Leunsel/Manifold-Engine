#include "Asm/AutoAssembler.h"

#include "Core/Text.h"

#include <ctime>

namespace Manifold::Asm
{
    namespace
    {
        std::string Today()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            char text[16] = "";
            if (::localtime_s(&local, &now) == 0)
                std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
            return text;
        }

        std::string Indented(const std::vector<std::string>& lines)
        {
            std::string text;
            for (const std::string& line : lines)
                text += "  " + line + "\n";
            return text;
        }

        std::string NopPadding(const TemplateInput& input, int jumpSize)
        {
            const int extra = static_cast<int>(input.OriginalBytes.size()) - jumpSize;
            return extra > 0 ? "  nop " + Hex(static_cast<std::uint64_t>(extra)) + "\n" : "";
        }

        std::string Name(const TemplateInput& input)
        {
            return input.Name.empty() ? "INJECT" : input.Name;
        }
    }

    std::string CheatTableTemplate()
    {
        return "[ENABLE]\n"
               "// code from here to [DISABLE] runs when the script is switched on\n"
               "\n"
               "[DISABLE]\n"
               "// code from here to the end runs when the script is switched off\n";
    }

    std::string CodeInjectionTemplate(const TemplateInput& input)
    {
        std::string text;
        text += "alloc(newmem,$1000," + input.AddressText + ")\n";
        text += "label(returnhere)\nlabel(originalcode)\nlabel(exit)\n\n";
        text += "newmem:\n  // your code here\n\n";
        text += "originalcode:\n" + Indented(input.OriginalLines) + "\n";
        text += "exit:\n  jmp returnhere\n\n";
        text += input.AddressText + ":\n  jmp newmem\n" + NopPadding(input, 5);
        text += "returnhere:\n";
        return text;
    }

    std::string FullInjectionTemplate(const TemplateInput& input)
    {
        const std::string name = Name(input);
        std::string text;
        text += "define(" + name + "," + input.AddressText + ")\n\n";
        text += "[ENABLE]\n";
        text += "assert(" + name + "," + HexBytes(input.OriginalBytes) + ")\n";
        text += "alloc(newmem,$1000," + name + ")\n\n";
        text += "label(code)\nlabel(return)\n\n";
        text += "newmem:\n  // your code here\n\n";
        text += "code:\n" + Indented(input.OriginalLines) + "  jmp return\n\n";
        text += name + ":\n  jmp newmem\n" + NopPadding(input, 5);
        text += "return:\n\n";
        text += "[DISABLE]\n";
        text += name + ":\n  db " + HexBytes(input.OriginalBytes) + "\n\n";
        text += "dealloc(newmem)\n";
        return text;
    }

    std::string AobInjectionTemplate(const TemplateInput& input)
    {
        const std::string name = Name(input);
        std::string text;
        text += "{ Game   : " + input.ModuleName + "\n";
        text += "  Date   : " + Today() + "\n";
        text += "  Author : \n\n";
        text += "  Describe what this script changes.\n}\n\n";
        text += "[ENABLE]\n\n";
        if (!input.ModuleName.empty())
            text += "aobscanmodule(" + name + "," + input.ModuleName + "," + input.Pattern + ") // should be unique\n";
        else
            text += "aobscan(" + name + "," + input.Pattern + ") // should be unique\n";
        const std::string at = input.PatternOffset ? name + "+" + Hex(static_cast<std::uint64_t>(input.PatternOffset)) : name;
        text += "alloc(newmem,$1000," + name + ")\n\n";
        text += "label(code)\nlabel(return)\n\n";
        text += "newmem:\n\n";
        text += "code:\n" + Indented(input.OriginalLines) + "  jmp return\n\n";
        text += at + ":\n  jmp newmem\n" + NopPadding(input, 5);
        text += "return:\nregistersymbol(" + name + ")\n\n";
        text += "[DISABLE]\n\n";
        text += at + ":\n  db " + HexBytes(input.OriginalBytes) + "\n\n";
        text += "unregistersymbol(" + name + ")\n";
        text += "dealloc(newmem)\n";
        if (!input.ContextLines.empty())
        {
            text += "\n{\n// ORIGINAL CODE - INJECTION POINT: " + input.AddressText + "\n\n";
            for (const std::string& line : input.ContextLines)
                text += line + "\n";
            text += "}\n";
        }
        return text;
    }

    std::string LuaCallTemplate()
    {
        return "[ENABLE]\n"
               "{$lua}\n"
               "if syntaxcheck then return end\n"
               "print(\"enabled\")\n"
               "{$asm}\n"
               "\n"
               "[DISABLE]\n"
               "{$lua}\n"
               "if syntaxcheck then return end\n"
               "print(\"disabled\")\n"
               "{$asm}\n";
    }
}
