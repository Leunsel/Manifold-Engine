#include "Asm/AsmAssist.h"

#include "Asm/Assembler.h"
#include "Asm/MnemonicDocs.h"
#include "Core/Text.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace Manifold::Asm
{
    namespace
    {
        const std::vector<AsmCommandDoc>& Commands()
        {
            static const std::vector<AsmCommandDoc> commands = {
                { "alloc", { { "name", "label", false, "" }, { "size", "number", false, "Bytes, for example `$1000`." }, { "near", "address", true, "Allocates within 2 GB of this address, so 5-byte jumps reach it." } },
                    "Reserves memory in the target and names it. Freed again by `dealloc` in [DISABLE]." },
                { "globalalloc", { { "name", "label", false, "" }, { "size", "number", false, "" }, { "near", "address", true, "" } },
                    "Like `alloc`, but the memory stays and the name is registered as a symbol; a second script with the same name gets the same memory." },
                { "dealloc", { { "name", "label", false, "" } }, "Frees memory reserved with `alloc`." },
                { "label", { { "name", "label", false, "Several names may be given, separated by commas." } }, "Declares a name that is placed later with `name:`." },
                { "registersymbol", { { "name", "label", false, "" } }, "Makes a name of the script usable everywhere: in the address list, the memory viewer and other scripts." },
                { "unregistersymbol", { { "name", "label", false, "" } }, "Removes a name registered with `registersymbol`." },
                { "define", { { "name", "label", false, "" }, { "value", "text", false, "" } }, "Replaces the name with the value everywhere below in the script." },
                { "aobscan", { { "name", "label", false, "" }, { "bytes", "bytes", false, "Hex bytes, `??` or `*` for any byte." } }, "Searches the whole process for the bytes and names the first match." },
                { "aobscanmodule", { { "name", "label", false, "" }, { "module", "module", false, "For example `game.exe`." }, { "bytes", "bytes", false, "Hex bytes, `??` or `*` for any byte." } },
                    "Searches one module for the bytes and names the first match. Faster and safer than `aobscan`." },
                { "aobscanregion", { { "name", "label", false, "" }, { "start", "address", false, "" }, { "stop", "address", false, "" }, { "bytes", "bytes", false, "" } },
                    "Searches between two addresses for the bytes and names the first match." },
                { "assert", { { "address", "address", false, "" }, { "bytes", "bytes", false, "" } }, "Stops the script unless the bytes at the address are these, so it does not patch the wrong version." },
                { "fullaccess", { { "address", "address", false, "" }, { "size", "number", false, "" } }, "Makes the memory readable, writable and executable." },
                { "createthread", { { "address", "address", false, "" } }, "Starts a thread in the target at the address." },
                { "createthreadandwait", { { "address", "address", false, "" }, { "timeout", "number", true, "Milliseconds." } }, "Starts a thread in the target and waits until it ends." },
                { "loadlibrary", { { "path", "file", false, "" } }, "Loads a DLL into the target." },
                { "readmem", { { "address", "address", false, "" }, { "size", "number", false, "" } }, "Writes a copy of the bytes at the address here." },
                { "reassemble", { { "address", "address", false, "" } }, "Writes the instruction at the address here again, assembled for this place." },
                { "include", { { "file", "file", false, "" } }, "Inserts another script file here." },
                { "luacall", { { "code", "lua", false, "" } }, "Runs one line of Lua while the script is assembled." },
            };
            return commands;
        }

        constexpr std::string_view kData[] = { "db", "dw", "dd", "dq" };
        constexpr std::string_view kSizes[] = { "byte", "word", "dword", "qword", "tword", "xmmword", "ymmword", "ptr", "short", "near", "far" };
        constexpr std::string_view kRegisters64[] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
            "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w", "r8b", "r9b", "r10b", "r11b",
            "r12b", "r13b", "r14b", "r15b", "sil", "dil", "bpl", "spl", "rip", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
            "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15" };
        constexpr std::string_view kRegisters32[] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
            "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4",
            "ymm5", "ymm6", "ymm7", "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7", "cs", "ds", "es", "fs", "gs", "ss" };

        bool WordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '@';
        }

        bool StartsWithNoCase(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() && EqualsNoCase(text.substr(0, prefix.size()), prefix);
        }

        std::string ParamText(const AsmCommandDoc& command)
        {
            std::string text;
            for (const AsmCommandParam& param : command.Params)
                text += (text.empty() ? "" : ", ") + std::string(param.Name) + (param.Optional ? "?" : "");
            return text;
        }

        std::vector<std::string> SplitArguments(std::string_view inside)
        {
            std::vector<std::string> result;
            std::string current;
            for (const char c : inside)
            {
                if (c == ',')
                {
                    result.push_back(Trim(current));
                    current.clear();
                }
                else
                {
                    current += c;
                }
            }
            result.push_back(Trim(current));
            return result;
        }

        bool IsName(std::string_view word)
        {
            if (word.empty() || std::isdigit(static_cast<unsigned char>(word.front())))
                return false;
            return std::all_of(word.begin(), word.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@'; });
        }
    }

    std::span<const AsmCommandDoc> AsmCommands()
    {
        return Commands();
    }

    const AsmCommandDoc* FindAsmCommand(std::string_view name)
    {
        for (const AsmCommandDoc& command : Commands())
        {
            if (EqualsNoCase(command.Name, Trim(name)))
                return &command;
        }
        return nullptr;
    }

    bool InLuaBlock(std::string_view text, std::size_t cursor)
    {
        const std::string lower = Lower(text.substr(0, std::min(cursor, text.size())));
        const std::size_t lua = lower.rfind("{$lua}");
        if (lua == std::string::npos)
            return false;
        const std::size_t back = lower.rfind("{$asm}");
        return back == std::string::npos || back < lua;
    }

    AsmWord AsmWordAt(std::string_view text, std::size_t cursor)
    {
        AsmWord word;
        cursor = std::min(cursor, text.size());
        std::size_t lineStart = cursor;
        while (lineStart > 0 && text[lineStart - 1] != '\n')
            --lineStart;
        std::size_t start = cursor;
        while (start > lineStart && WordChar(text[start - 1]))
            --start;
        word.Start = start;
        word.Prefix = std::string(text.substr(start, cursor - start));
        const std::string_view before = text.substr(lineStart, start - lineStart);
        if (before.find("//") != std::string_view::npos)
            return word;
        const std::size_t open = before.rfind('{');
        if (open != std::string_view::npos && before.find('}', open) == std::string_view::npos)
        {
            if (open + 1 == before.size() && !word.Prefix.empty() && word.Prefix.front() == '$')
                word.Place = AsmPlace::Directive;
            return word;
        }
        std::string_view trimmed = before;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
            trimmed.remove_prefix(1);
        if (TrimView(trimmed) == "[")
        {
            word.Place = AsmPlace::Section;
            return word;
        }
        if (TrimView(trimmed).empty())
        {
            word.Place = AsmPlace::LineStart;
            return word;
        }
        std::size_t tokenEnd = 0;
        while (tokenEnd < trimmed.size() && WordChar(trimmed[tokenEnd]))
            ++tokenEnd;
        if (tokenEnd == 0)
            return word;
        word.Command = Lower(trimmed.substr(0, tokenEnd));
        const std::string_view rest = trimmed.substr(tokenEnd);
        const std::size_t paren = rest.find('(');
        if (paren != std::string_view::npos && TrimView(rest.substr(0, paren)).empty())
        {
            const std::string_view inside = rest.substr(paren + 1);
            if (inside.find(')') != std::string_view::npos)
                return word;
            word.Argument = static_cast<int>(std::count(inside.begin(), inside.end(), ','));
            word.Place = AsmPlace::Argument;
            return word;
        }
        if (rest.empty() || rest.find(':') != std::string_view::npos)
            return word;
        if (rest.front() == ' ' || rest.front() == '\t' || rest.find(',') != std::string_view::npos || rest.find('[') != std::string_view::npos)
            word.Place = AsmPlace::Operand;
        return word;
    }

    std::vector<std::string> ScriptNames(std::string_view text)
    {
        std::set<std::string> seen;
        std::vector<std::string> names;
        const auto add = [&](std::string_view name)
            {
                const std::string clean = Trim(std::string(name));
                if (IsName(clean) && seen.insert(Lower(clean)).second)
                    names.push_back(clean);
            };
        bool lua = false;
        std::size_t at = 0;
        while (at <= text.size())
        {
            std::size_t end = text.find('\n', at);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = TrimView(text.substr(at, end - at));
            if (const std::size_t comment = line.find("//"); comment != std::string_view::npos)
                line = TrimView(line.substr(0, comment));
            const std::string lower = Lower(line);
            if (lower.find("{$lua}") != std::string::npos)
                lua = true;
            if (lower.find("{$asm}") != std::string::npos)
                lua = false;
            if (!lua && !line.empty())
            {
                std::size_t tokenEnd = 0;
                while (tokenEnd < line.size() && WordChar(line[tokenEnd]))
                    ++tokenEnd;
                const std::string command = Lower(line.substr(0, tokenEnd));
                const std::string_view rest = TrimView(line.substr(tokenEnd));
                if (!rest.empty() && rest.front() == '(' && rest.find(')') != std::string_view::npos)
                {
                    const std::vector<std::string> arguments = SplitArguments(rest.substr(1, rest.find(')') - 1));
                    if (command == "label" || command == "registersymbol")
                    {
                        for (const std::string& argument : arguments)
                            add(argument);
                    }
                    else if (command == "alloc" || command == "globalalloc" || command == "define" || command == "aobscan" || command == "aobscanmodule" ||
                             command == "aobscanregion")
                    {
                        add(arguments.front());
                    }
                }
                else if (!rest.empty() && rest.front() == ':' && tokenEnd > 0)
                {
                    add(line.substr(0, tokenEnd));
                }
            }
            if (end >= text.size())
                break;
            at = end + 1;
        }
        return names;
    }

    std::vector<AsmCompletion> CompleteAsm(std::string_view text, std::size_t cursor, const AsmAssistInput& input)
    {
        std::vector<AsmCompletion> items;
        if (InLuaBlock(text, cursor))
            return items;
        const AsmWord word = AsmWordAt(text, cursor);
        std::set<std::string> seen;
        const auto add = [&](std::string name, const char* kind, std::string signature, std::string help)
            {
                if (items.size() < 400 && seen.insert(Lower(name)).second)
                    items.push_back({ std::move(name), kind, std::move(signature), std::move(help) });
            };
        const auto matching = [&](std::string_view name) { return word.Prefix.empty() || StartsWithNoCase(name, word.Prefix.substr(0, 1)); };
        const auto names = [&]()
            {
                for (const std::string& name : ScriptNames(text))
                    add(name, "word", "", "A name of this script.");
            };
        const auto process = [&](bool symbols)
            {
                for (const std::string& module : input.Modules)
                {
                    if (matching(module))
                        add(module, "table", "", "A module of the process.");
                }
                if (!symbols || word.Prefix.size() < 2)
                    return;
                for (const std::string& symbol : input.Symbols)
                {
                    if (StartsWithNoCase(symbol, word.Prefix))
                        add(symbol, "property", "", "A symbol of the process.");
                }
            };

        switch (word.Place)
        {
        case AsmPlace::Directive:
            add("$lua}", "keyword", "", "Lua until `{$asm}`: runs while the script is assembled; what it returns is assembled in its place.");
            add("$asm}", "keyword", "", "Back to Auto Assembler after a `{$lua}` block.");
            break;
        case AsmPlace::Section:
            add("ENABLE]", "keyword", "", "Runs when the script is turned on.");
            add("DISABLE]", "keyword", "", "Runs when the script is turned off; it should undo what [ENABLE] did.");
            break;
        case AsmPlace::LineStart:
            for (const AsmCommandDoc& command : Commands())
                add(command.Name, "function", "(" + ParamText(command) + ")", command.Text);
            for (const std::string_view data : kData)
                add(std::string(data), "keyword", " values", "Writes the values here as bytes, words, dwords or qwords.");
            names();
            if (!word.Prefix.empty())
            {
                for (const std::string& mnemonic : KnownMnemonics())
                {
                    if (StartsWithNoCase(mnemonic, word.Prefix))
                        add(mnemonic, "keyword", "", MnemonicSummary(mnemonic));
                }
            }
            break;
        case AsmPlace::Operand:
            for (const std::string_view reg : kRegisters32)
                add(std::string(reg), "value", "", "");
            if (input.Is64Bit)
            {
                for (const std::string_view reg : kRegisters64)
                    add(std::string(reg), "value", "", "");
            }
            for (const std::string_view size : kSizes)
                add(std::string(size), "keyword", "", "");
            names();
            process(true);
            break;
        case AsmPlace::Argument:
        {
            const AsmCommandDoc* command = FindAsmCommand(word.Command);
            if (!command || word.Argument >= static_cast<int>(command->Params.size()))
                break;
            const std::string_view type = command->Params[static_cast<std::size_t>(word.Argument)].Type;
            const bool declares = word.Argument == 0 && (word.Command == "alloc" || word.Command == "globalalloc" || word.Command == "label" || word.Command == "define" ||
                                                         word.Command == "aobscan" || word.Command == "aobscanmodule" || word.Command == "aobscanregion");
            if (declares)
                break;
            if (type == "module")
                process(false);
            else if (type == "label")
                names();
            else if (type == "address")
            {
                names();
                process(true);
            }
            break;
        }
        case AsmPlace::None:
            break;
        }
        return items;
    }
}
