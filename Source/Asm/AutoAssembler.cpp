#include "Asm/AutoAssembler.h"

#include "Asm/Assembler.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

#include <windows.h>

namespace Manifold::Asm
{
    namespace
    {
        constexpr int kMaxLayoutPasses = 12;
        constexpr std::uint64_t kCheckOnlyBase = 0x0000000010000000ull;
        constexpr std::uint64_t kForwardGuess = 0x10000;
        constexpr std::uint64_t kMaxAllocSize = 0x7FFFFFFF;

        struct SourceLine
        {
            std::string Text;
            int Line = 0;
        };

        struct Statement
        {
            enum class Kind
            {
                Place,
                Label,
                Code,
                Bytes,
                Align
            };
            Kind Type = Kind::Code;
            std::string Text;
            std::vector<std::uint8_t> Data;
            int Line = 0;
            std::uint64_t Alignment = 0;
        };

        struct PendingRead
        {
            std::size_t Index = 0;
            std::string Address;
            int Line = 0;
            std::string Text;
        };

        struct PendingAlloc
        {
            std::string Name;
            std::uint64_t Size = 0;
            std::string Near;
            bool Global = false;
            int Line = 0;
        };

        struct Command
        {
            std::string Name;
            std::vector<std::string> Arguments;
        };

        ErrorCode CodeFor(AsmError error)
        {
            switch (error)
            {
            case AsmError::UnknownMnemonic: return ErrorCode::AsmUnknownInstruction;
            case AsmError::BadOperand: return ErrorCode::AsmBadOperand;
            case AsmError::UnknownSymbol: return ErrorCode::AsmUnknownSymbol;
            case AsmError::OutOfRange: return ErrorCode::AsmOutOfRange;
            case AsmError::EncodeFailed: return ErrorCode::AsmEncodeFailed;
            default: return ErrorCode::AsmSyntax;
            }
        }

        std::string LineDetail(int line, std::string_view message, std::string_view text = {})
        {
            std::string detail = std::to_string(line) + ": " + std::string(message);
            if (!text.empty())
                detail += " (" + std::string(TrimView(text)) + ")";
            return detail;
        }

        bool IsWordChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '@';
        }

        bool AnonymousNameChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '@' || c == '.';
        }

        std::string AnonymousLabel(std::size_t index)
        {
            return "__anonymous_" + std::to_string(index);
        }

        // ∑ The same rule as Collect's place statements: a line with a quote in it is data
        //   (db 'a:'), except a quoted module name at the start ("game.exe"+10:).
        bool IsPlaceLine(const std::string& text)
        {
            return text.size() > 1 && text.back() == ':' && text.find('\'') == std::string::npos && (text.find('"') == std::string::npos || text.front() == '"');
        }

        // ∑ Quoted text is one token (as in ReplaceWord), so db '@f' stays the two characters.
        std::string ReplaceAnonymous(const std::string& text, bool forward, const std::string& label)
        {
            const char lower = forward ? 'f' : 'b';
            const char upper = forward ? 'F' : 'B';
            std::string result;
            for (std::size_t at = 0; at < text.size();)
            {
                if (text[at] == '\'' || text[at] == '"')
                {
                    const std::size_t close = text.find(text[at], at + 1);
                    const std::size_t end = close == std::string::npos ? text.size() : close + 1;
                    result.append(text, at, end - at);
                    at = end;
                    continue;
                }
                const bool start = at == 0 || !AnonymousNameChar(text[at - 1]);
                if (start && at + 1 < text.size() && text[at] == '@' && (text[at + 1] == lower || text[at + 1] == upper) &&
                    (at + 2 >= text.size() || !AnonymousNameChar(text[at + 2])))
                {
                    result += label;
                    at += 2;
                    continue;
                }
                result += text[at++];
            }
            return result;
        }

        // ∑ CE's unlabeledlabels (autoassembler.pas 800-897): every line that ends in ':' is a place,
        //   not only @@: (newmem:, a named label, an address line). @f is the next place after the
        //   line, @b the last place before it. @@: lines become declared labels with their own names.
        bool ResolveAnonymousLabels(std::vector<SourceLine>& lines, std::string& problem, int& problemLine)
        {
            bool used = false;
            for (const SourceLine& line : lines)
                used = used || line.Text == "@@:" || ReplaceAnonymous(line.Text, true, "") != line.Text || ReplaceAnonymous(line.Text, false, "") != line.Text;
            if (!used)
                return true;
            std::vector<std::string> places;
            std::vector<SourceLine> declarations;
            for (SourceLine& line : lines)
            {
                if (line.Text == "@@:")
                {
                    const std::string name = AnonymousLabel(declarations.size());
                    declarations.push_back({ "label(" + name + ")", line.Line });
                    line.Text = name + ":";
                }
                if (IsPlaceLine(line.Text))
                    places.push_back(line.Text.substr(0, line.Text.size() - 1));
            }
            std::ptrdiff_t lastSeen = -1;
            for (SourceLine& line : lines)
            {
                if (IsPlaceLine(line.Text))
                {
                    ++lastSeen;
                    continue;
                }
                if (ReplaceAnonymous(line.Text, true, "") != line.Text)
                {
                    if (lastSeen + 1 >= static_cast<std::ptrdiff_t>(places.size()))
                    {
                        problem = "@f has no label line (name:) after it";
                        problemLine = line.Line;
                        return false;
                    }
                    line.Text = ReplaceAnonymous(line.Text, true, places[static_cast<std::size_t>(lastSeen + 1)]);
                }
                else if (ReplaceAnonymous(line.Text, false, "") != line.Text)
                {
                    if (lastSeen < 0)
                    {
                        problem = "@b has no label line (name:) before it";
                        problemLine = line.Line;
                        return false;
                    }
                    line.Text = ReplaceAnonymous(line.Text, false, places[static_cast<std::size_t>(lastSeen)]);
                }
            }
            lines.insert(lines.begin(), declarations.begin(), declarations.end());
            return true;
        }

        // ∑ CE reads the sizes of alloc, globalalloc, readmem and fullaccess with StrToInt
        //   (autoassembler.pas 2455, 2646, 2822): decimal, hex only with $ or 0x. Reading them as
        //   hex made readmem(x,14) copy 20 bytes, so the cave ran 6 bytes of the next instruction.
        std::optional<std::uint64_t> ParseSize(std::string_view text)
        {
            const std::string trimmed = Trim(std::string(text));
            if (trimmed.empty())
                return std::nullopt;
            std::string_view digits = trimmed;
            int base = 10;
            if (digits.front() == '$')
            {
                digits.remove_prefix(1);
                base = 16;
            }
            else if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
            {
                digits.remove_prefix(2);
                base = 16;
            }
            else if (digits.front() == '#')
            {
                digits.remove_prefix(1);
            }
            if (digits.empty() || digits.size() > 16)
                return std::nullopt;
            std::uint64_t value = 0;
            for (const char c : digits)
            {
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (base == 16 && c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (base == 16 && c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                if (digit < 0)
                    return std::nullopt;
                if (value > (~0ull - static_cast<std::uint64_t>(digit)) / static_cast<std::uint64_t>(base))
                    return std::nullopt;
                value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
            }
            return value;
        }

        std::vector<std::string> NameList(const std::vector<std::string>& arguments)
        {
            std::vector<std::string> names;
            for (const std::string& argument : arguments)
            {
                std::string current;
                for (const char c : argument)
                {
                    if (c == ' ' || c == '\t')
                    {
                        if (!current.empty())
                            names.push_back(std::move(current));
                        current.clear();
                    }
                    else
                    {
                        current += c;
                    }
                }
                if (!current.empty())
                    names.push_back(std::move(current));
            }
            return names;
        }

        // ∑ CE's replacetoken works on tokens, and a quoted string is one token with its quotes, so a
        //   define never changes text inside 'db' or "module" strings.
        std::string ReplaceWord(const std::string& text, const std::string& word, const std::string& replacement)
        {
            if (word.empty())
                return text;
            std::string out;
            std::size_t at = 0;
            while (at < text.size())
            {
                const char c = text[at];
                if (c == '\'' || c == '"')
                {
                    const std::size_t close = text.find(c, at + 1);
                    const std::size_t end = close == std::string::npos ? text.size() : close + 1;
                    out.append(text, at, end - at);
                    at = end;
                    continue;
                }
                if (text.compare(at, word.size(), word) == 0)
                {
                    const bool leftOk = at == 0 || !IsWordChar(text[at - 1]);
                    const bool rightOk = at + word.size() >= text.size() || !IsWordChar(text[at + word.size()]);
                    if (leftOk && rightOk)
                    {
                        out += replacement;
                        at += word.size();
                        continue;
                    }
                }
                out += c;
                ++at;
            }
            return out;
        }

        std::string StripComments(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            bool inBlock = false;
            bool inBraceComment = false;
            char quote = 0;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                if (inBlock)
                {
                    if (c == '*' && i + 1 < text.size() && text[i + 1] == '/')
                    {
                        inBlock = false;
                        ++i;
                    }
                    else if (c == '\n')
                        out += '\n';
                    continue;
                }
                if (inBraceComment)
                {
                    if (c == '}')
                        inBraceComment = false;
                    else if (c == '\n')
                        out += '\n';
                    continue;
                }
                if (quote)
                {
                    out += c;
                    if (c == quote || c == '\n')
                        quote = 0;
                    continue;
                }
                if (c == '\'' || c == '"')
                {
                    quote = c;
                    out += c;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                        ++i;
                    if (i < text.size())
                        out += '\n';
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
                {
                    inBlock = true;
                    ++i;
                    continue;
                }
                if (c == '{')
                {
                    if (i + 1 < text.size() && text[i + 1] == '$')
                    {
                        const std::size_t close = text.find('}', i);
                        i = close == std::string_view::npos ? text.size() : close;
                        continue;
                    }
                    inBraceComment = true;
                    continue;
                }
                out += c;
            }
            return out;
        }

        // ∑ The first {$lua} at or after from that is code: one inside //, /* */, { } or a string is commented out and must not run.
        //   Walks the same states as StripComments; from is outside a comment (the start, or just after {$asm}).
        std::size_t FindLuaBlock(std::string_view lower, std::size_t from)
        {
            bool inBlock = false;
            bool inBraceComment = false;
            char quote = 0;
            for (std::size_t i = from; i < lower.size(); ++i)
            {
                const char c = lower[i];
                const char next = i + 1 < lower.size() ? lower[i + 1] : '\0';
                if (inBlock)
                {
                    if (c == '*' && next == '/')
                    {
                        inBlock = false;
                        ++i;
                    }
                    continue;
                }
                if (inBraceComment)
                {
                    if (c == '}')
                        inBraceComment = false;
                    continue;
                }
                if (quote)
                {
                    if (c == quote || c == '\n')
                        quote = 0;
                    continue;
                }
                if (c == '\'' || c == '"')
                    quote = c;
                else if (c == '/' && next == '/')
                {
                    while (i + 1 < lower.size() && lower[i + 1] != '\n')
                        ++i;
                }
                else if (c == '/' && next == '*')
                {
                    inBlock = true;
                    ++i;
                }
                else if (c == '{')
                {
                    if (lower.substr(i, 6) == "{$lua}")
                        return i;
                    if (next == '$')
                    {
                        const std::size_t close = lower.find('}', i);
                        i = close == std::string_view::npos ? lower.size() : close;
                    }
                    else
                        inBraceComment = true;
                }
            }
            return std::string_view::npos;
        }

        // ∑ The {$...} directives in an assembler part (outside comments and strings). {$strict}
        //   turns off CE's implicit labels; {$try}/{$except} and the C blocks change what the code
        //   means, so stripping them like comments would run the script without its exception
        //   handler. Those are reported instead of being dropped.
        struct Directives
        {
            bool Strict = false;
            std::string Unsupported;
        };

        Directives ScanDirectives(std::string_view lower)
        {
            Directives found;
            bool inBlock = false;
            bool inBraceComment = false;
            char quote = 0;
            for (std::size_t i = 0; i < lower.size(); ++i)
            {
                const char c = lower[i];
                const char next = i + 1 < lower.size() ? lower[i + 1] : '\0';
                if (inBlock)
                {
                    if (c == '*' && next == '/')
                    {
                        inBlock = false;
                        ++i;
                    }
                    continue;
                }
                if (inBraceComment)
                {
                    if (c == '}')
                        inBraceComment = false;
                    continue;
                }
                if (quote)
                {
                    if (c == quote || c == '\n')
                        quote = 0;
                    continue;
                }
                if (c == '\'' || c == '"')
                    quote = c;
                else if (c == '/' && next == '/')
                {
                    while (i + 1 < lower.size() && lower[i + 1] != '\n')
                        ++i;
                }
                else if (c == '/' && next == '*')
                {
                    inBlock = true;
                    ++i;
                }
                else if (c == '{' && next == '$')
                {
                    const std::size_t close = lower.find('}', i);
                    const std::string name = Trim(std::string(lower.substr(i + 2, close == std::string_view::npos ? std::string_view::npos : close - i - 2)));
                    if (name == "strict")
                        found.Strict = true;
                    else if ((name == "try" || name == "except" || name == "c" || name == "ccode" || name == "luacode") && found.Unsupported.empty())
                        found.Unsupported = "{$" + name + "}";
                    i = close == std::string_view::npos ? lower.size() : close;
                }
                else if (c == '{')
                    inBraceComment = true;
            }
            return found;
        }

        std::optional<Command> ParseCommand(const std::string& line)
        {
            const std::size_t open = line.find('(');
            if (open == std::string::npos || open == 0 || line.back() != ')')
                return std::nullopt;
            const std::string name = Lower(Trim(line.substr(0, open)));
            if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
                return std::nullopt;
            Command command;
            command.Name = name;
            const std::string inside = line.substr(open + 1, line.size() - open - 2);
            std::string current;
            int depth = 0;
            char quote = 0;
            for (char c : inside)
            {
                if (quote)
                {
                    current += c;
                    if (c == quote)
                        quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'')
                {
                    quote = c;
                    current += c;
                    continue;
                }
                if (c == '(' || c == '[')
                    ++depth;
                else if (c == ')' || c == ']')
                    --depth;
                if (c == ',' && depth == 0)
                {
                    command.Arguments.push_back(Trim(current));
                    current.clear();
                    continue;
                }
                current += c;
            }
            if (!Trim(current).empty() || !command.Arguments.empty())
                command.Arguments.push_back(Trim(current));
            return command;
        }

        bool IsKnownCommand(const std::string& name)
        {
            static const std::set<std::string> kCommands = { "alloc", "globalalloc", "dealloc", "label", "registersymbol", "unregistersymbol", "aobscan",
                "aobscanmodule", "aobscanregion", "assert", "fullaccess", "createthread", "createthreadandwait", "loadlibrary", "readmem", "reassemble", "define",
                "include", "luacall" };
            return kCommands.count(name) > 0;
        }

        std::string Unquote(std::string text)
        {
            text = Trim(text);
            if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
                return text.substr(1, text.size() - 2);
            return text;
        }

        class Run
        {
        public:
            Run(AaHost& host, AaMemory& memory, const AaOptions& options) : m_host(host), m_memory(memory), m_options(options)
            {
                m_is64 = host.Process ? host.Process->Is64Bit() : true;
            }

            AaResult Execute(std::string_view script)
            {
                std::vector<SourceLine> lines;
                if (!Preprocess(script, lines))
                    return m_result;
                if (!Collect(lines))
                    return Abort();
                if (!PrepareLibraries() || !RunScans() || !Allocate() || !ReadMemory())
                    return Abort();
                if (!Layout())
                    return Abort();
                MeasureLabels();
                if (!CheckAsserts() || !CheckThreads())
                    return Abort();
                if (m_options.CheckOnly)
                {
                    ReleaseAllocations();
                    return m_result;
                }
                if (!WriteCode())
                    return Abort();
                Finish();
                return m_result;
            }

        private:
            bool Fail(ErrorCode code, int line, std::string_view message, std::string_view text = {})
            {
                if (m_result.Error.Ok())
                {
                    const bool lineFormat = (code >= ErrorCode::AsmSyntax && code <= ErrorCode::AsmEncodeFailed) || (code >= ErrorCode::AaUnknownCommand && code <= ErrorCode::AaLabelTwice);
                    m_result.Error = Status::Fail(code, lineFormat ? LineDetail(line, message, text) : std::string(message));
                    m_result.Line = line;
                }
                return false;
            }

            AaResult Abort()
            {
                ReleaseAllocations();
                return m_result;
            }

            bool Preprocess(std::string_view script, std::vector<SourceLine>& out)
            {
                std::string sectioned;
                bool inEnable = false;
                bool inDisable = false;
                bool sawSection = false;
                for (const std::string& raw : SplitLines(script))
                {
                    const std::string trimmed = Trim(raw);
                    if (EqualsNoCase(trimmed, "[enable]"))
                    {
                        inEnable = true;
                        inDisable = false;
                        sawSection = true;
                        sectioned += '\n';
                        continue;
                    }
                    if (EqualsNoCase(trimmed, "[disable]"))
                    {
                        inEnable = false;
                        inDisable = true;
                        sawSection = true;
                        sectioned += '\n';
                        continue;
                    }
                    const bool header = !inEnable && !inDisable;
                    const bool wanted = header || m_options.Section == AaSection::Whole || (m_options.Section == AaSection::Enable && inEnable) ||
                                        (m_options.Section == AaSection::Disable && inDisable);
                    if (wanted)
                        sectioned += raw;
                    sectioned += '\n';
                }
                if (!sawSection && m_options.Section != AaSection::Whole)
                    return Fail(ErrorCode::AaNoSections, 0, "");

                std::string text;
                if (!ProcessText(sectioned, 0, true, "", text))
                    return false;

                std::vector<SourceLine> selected;
                const std::vector<std::string> lines = SplitLines(text);
                for (std::size_t i = 0; i < lines.size(); ++i)
                {
                    std::string line = Trim(lines[i]);
                    if (!line.empty())
                        selected.push_back({ std::move(line), static_cast<int>(i) + 1 });
                }

                std::vector<SourceLine> included;
                if (!ExpandIncludes(selected, included, 0))
                    return false;
                for (const SourceLine& line : included)
                {
                    if (EqualsNoCase(line.Text, "[32-bit]") || EqualsNoCase(line.Text, "[64-bit]") || EqualsNoCase(line.Text, "[/32-bit]") || EqualsNoCase(line.Text, "[/64-bit]"))
                        return Fail(ErrorCode::AaUnknownCommand, line.Line, "[32-bit]/[64-bit] blocks are not supported", line.Text);
                }
                // ∑ @f/@b before the custom commands, on purpose: CE's autoassemble2 runs
                //   removecomments and unlabeledlabels on the whole script before its first pass
                //   expands registered commands line by line, so a place line a custom command
                //   produces is not an @f target there either.
                std::string problem;
                int problemLine = 0;
                if (!ResolveAnonymousLabels(included, problem, problemLine))
                    return Fail(ErrorCode::AsmSyntax, problemLine, problem, "");
                std::vector<SourceLine> expanded;
                std::vector<std::pair<std::string, std::string>> seen;
                if (!ExpandCustomCommands(included, expanded, seen, 0))
                    return false;

                // ∑ CE applies defines in script order (autoassembler.pas 2004-2050): a define counts
                //   from its own line on, its value already has the earlier defines applied (so
                //   define(b,a+4) works), and a second define of the same name replaces the value.
                //   "The same name" ignores case (CE compares uppercase(defines[j].name)), and the
                //   entry keeps its first spelling; the substitution itself is case-sensitive like
                //   CE's replacetoken. So define(Foo,1) and define(FOO,2) are one define, and
                //   m_defines (keyed by the lower-case name) has one entry for it instead of two
                //   names fighting over one key.
                std::vector<std::pair<std::string, std::string>> defines;
                for (SourceLine line : expanded)
                {
                    const auto command = ParseCommand(line.Text);
                    if (command && command->Name == "define")
                    {
                        if (command->Arguments.size() < 2)
                            return Fail(ErrorCode::AaBadArguments, line.Line, "define(name, text) needs two arguments", line.Text);
                        if (command->Arguments[0].empty())
                            return Fail(ErrorCode::AaBadArguments, line.Line, "define(name, text) needs a name", line.Text);
                        std::string value = command->Arguments[1];
                        for (std::size_t extra = 2; extra < command->Arguments.size(); ++extra)
                            value += "," + command->Arguments[extra];
                        for (const auto& [name, earlier] : defines)
                            value = ReplaceWord(value, name, earlier);
                        const auto same = std::find_if(defines.begin(), defines.end(), [&](const auto& define) { return EqualsNoCase(define.first, command->Arguments[0]); });
                        if (same != defines.end())
                            same->second = value;
                        else
                            defines.emplace_back(command->Arguments[0], value);
                        continue;
                    }
                    if (!command || (command->Name != "registersymbol" && command->Name != "unregistersymbol"))
                    {
                        for (const auto& [name, value] : defines)
                            line.Text = ReplaceWord(line.Text, name, value);
                    }
                    out.push_back(std::move(line));
                }
                for (const auto& [name, value] : defines)
                    m_defines[Lower(name)] = value;
                return true;
            }

            // ∑ Runs the {$lua} blocks of one text, scans its {$...} directives and strips its
            //   comments, appending the result to text. Included files, custom-command output and
            //   {$lua} output go through the same scan as the script itself: before, the comment
            //   stripper dropped every {$...} in them, so an include with {$try} ran without its
            //   handler and an included {$strict} was ignored. CE runs Lua only in the script
            //   (luacode comes before includes and custom commands are expanded, and an include
            //   only gets removecomments and unlabeledlabels), so {$lua} anywhere else is refused
            //   instead of being run or dropped. fixedLine is the script line every error in a
            //   text that is not the script itself is reported at (0: count lines in source).
            bool ProcessText(std::string_view source, int fixedLine, bool allowLua, const std::string& origin, std::string& text)
            {
                const std::string lower = Lower(source);
                const auto lineAt = [&](std::size_t offset) {
                    if (fixedLine)
                        return fixedLine;
                    return static_cast<int>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(std::min(offset, source.size())), '\n')) + 1;
                };
                const std::string in = origin.empty() ? std::string() : " in " + origin;
                std::size_t at = 0;
                while (at < source.size())
                {
                    const std::size_t luaStart = FindLuaBlock(lower, at);
                    const std::size_t asmEnd = luaStart == std::string::npos ? source.size() : luaStart;
                    const Directives directives = ScanDirectives(std::string_view(lower).substr(at, asmEnd - at));
                    m_strict = m_strict || directives.Strict;
                    if (!directives.Unsupported.empty())
                    {
                        const std::size_t where = lower.find(directives.Unsupported, at);
                        return Fail(ErrorCode::AaUnknownCommand, lineAt(where == std::string::npos ? at : where),
                            directives.Unsupported + in + " is not supported yet; the script would run without it", directives.Unsupported);
                    }
                    if (luaStart == std::string::npos)
                    {
                        text += StripComments(source.substr(at));
                        break;
                    }
                    if (!allowLua)
                        return Fail(ErrorCode::AaUnknownCommand, lineAt(luaStart), "{$lua}" + in + " is not supported; Cheat Engine runs {$lua} blocks only in the script itself", "{$lua}");
                    text += StripComments(source.substr(at, luaStart - at));
                    const std::size_t codeStart = luaStart + 6;
                    std::size_t luaEnd = lower.find("{$asm}", codeStart);
                    if (luaEnd == std::string::npos)
                        luaEnd = source.size();
                    const std::string_view code = source.substr(codeStart, luaEnd - codeStart);
                    const int lineCount = static_cast<int>(std::count(code.begin(), code.end(), '\n'));
                    const int startLine = lineAt(luaStart);
                    if (!m_host.RunLua)
                        return Fail(ErrorCode::AaLuaFailed, startLine, "Lua is not available");
                    std::string output;
                    const Status status = m_host.RunLua(code, m_options.CheckOnly, output);
                    if (!status)
                    {
                        m_result.Error = Status::Fail(ErrorCode::AaLuaFailed, status.Text);
                        m_result.Line = startLine;
                        return false;
                    }
                    for (char& c : output)
                    {
                        if (c == '\r')
                            c = ' ';
                    }
                    const int outputLines = static_cast<int>(std::count(output.begin(), output.end(), '\n'));
                    if (!ProcessText(output, startLine, false, "the output of a {$lua} block", text))
                        return false;
                    for (int i = outputLines; i < lineCount; ++i)
                        text += '\n';
                    at = luaEnd == source.size() ? luaEnd : luaEnd + 6;
                }
                return true;
            }

            bool ExpandCustomCommands(const std::vector<SourceLine>& lines, std::vector<SourceLine>& out, std::vector<std::pair<std::string, std::string>>& seen, int depth)
            {
                for (const SourceLine& line : lines)
                {
                    const std::size_t open = line.Text.find('(');
                    const std::string name = open == std::string::npos ? "" : Trim(line.Text.substr(0, open));
                    if (EqualsNoCase(name, "define"))
                    {
                        if (const auto command = ParseCommand(line.Text); command && command->Arguments.size() >= 2 && !command->Arguments[0].empty())
                        {
                            std::string value = command->Arguments[1];
                            for (std::size_t extra = 2; extra < command->Arguments.size(); ++extra)
                                value += "," + command->Arguments[extra];
                            for (const auto& [defined, earlier] : seen)
                                value = ReplaceWord(value, defined, earlier);
                            const auto same = std::find_if(seen.begin(), seen.end(), [&](const auto& define) { return EqualsNoCase(define.first, command->Arguments[0]); });
                            if (same != seen.end())
                                same->second = value;
                            else
                                seen.emplace_back(command->Arguments[0], value);
                        }
                    }
                    if (name.empty() || !m_host.IsCustomCommand || !m_host.IsCustomCommand(name) || line.Text.back() != ')')
                    {
                        out.push_back(line);
                        continue;
                    }
                    if (depth > 6)
                        return Fail(ErrorCode::AaBadArguments, line.Line, "custom commands nest too deep", line.Text);
                    std::string parameters = line.Text.substr(open + 1, line.Text.size() - open - 2);
                    for (const auto& [defined, value] : seen)
                        parameters = ReplaceWord(parameters, defined, value);
                    std::string replacement;
                    const Status status = m_host.RunCustomCommand(name, parameters, m_options.CheckOnly, replacement);
                    if (!status)
                    {
                        m_result.Error = Status::Fail(ErrorCode::AaLuaFailed, name + ": " + status.Text);
                        m_result.Line = line.Line;
                        return false;
                    }
                    std::string stripped;
                    if (!ProcessText(replacement, line.Line, false, "the output of " + name, stripped))
                        return false;
                    std::vector<SourceLine> produced;
                    for (const std::string& text : SplitLines(stripped))
                    {
                        const std::string trimmed = Trim(text);
                        if (!trimmed.empty())
                            produced.push_back({ trimmed, line.Line });
                    }
                    if (!ExpandCustomCommands(produced, out, seen, depth + 1))
                        return false;
                }
                return true;
            }

            bool ExpandIncludes(const std::vector<SourceLine>& lines, std::vector<SourceLine>& out, int depth)
            {
                for (const SourceLine& line : lines)
                {
                    const auto command = ParseCommand(line.Text);
                    if (!command || command->Name != "include")
                    {
                        out.push_back(line);
                        continue;
                    }
                    if (depth > 8 || command->Arguments.empty())
                        return Fail(ErrorCode::AaBadArguments, line.Line, "include(file) needs a file", line.Text);
                    std::filesystem::path path = Utf8ToWide(Unquote(command->Arguments[0]));
                    if (path.is_relative() && !m_options.IncludeFolder.empty())
                        path = m_options.IncludeFolder / path;
                    std::ifstream file(path, std::ios::binary);
                    if (!file)
                    {
                        m_result.Error = Status::Fail(ErrorCode::AaIncludeMissing, WideToUtf8(path.wstring()));
                        m_result.Line = line.Line;
                        return false;
                    }
                    std::stringstream content;
                    content << file.rdbuf();
                    std::string stripped;
                    if (!ProcessText(content.str(), line.Line, false, "the included file " + WideToUtf8(path.filename().wstring()), stripped))
                        return false;
                    std::vector<SourceLine> included;
                    for (const std::string& text : SplitLines(stripped))
                    {
                        const std::string trimmed = Trim(text);
                        if (!trimmed.empty())
                            included.push_back({ trimmed, line.Line });
                    }
                    if (!ExpandIncludes(included, out, depth + 1))
                        return false;
                }
                return true;
            }

            bool Collect(const std::vector<SourceLine>& lines)
            {
                for (const SourceLine& line : lines)
                {
                    const std::string& text = line.Text;
                    if (IsPlaceLine(text))
                    {
                        Statement statement;
                        statement.Type = Statement::Kind::Place;
                        statement.Text = Trim(text.substr(0, text.size() - 1));
                        statement.Line = line.Line;
                        m_statements.push_back(std::move(statement));
                        continue;
                    }

                    const auto command = ParseCommand(text);
                    if (command && IsKnownCommand(command->Name))
                    {
                        if (!HandleCommand(*command, line))
                            return false;
                        continue;
                    }
                    std::string spaced = text;
                    std::replace(spaced.begin(), spaced.end(), ',', ' ');
                    const std::vector<std::string> words = NameList({ spaced });
                    if (!words.empty() && EqualsNoCase(words.front(), "align"))
                    {
                        const auto alignment = words.size() >= 2 ? ParseNumber(words[1]) : std::nullopt;
                        const auto fill = words.size() >= 3 ? ParseNumber(words[2]) : std::optional<std::uint64_t>(0);
                        if (words.size() > 3 || !alignment || *alignment == 0 || *alignment > 0x10000 || !fill || *fill > 0xFF)
                            return Fail(ErrorCode::AaBadArguments, line.Line, "align needs a size and an optional fill byte, like align 10 or align 10 CC", text);
                        Statement statement;
                        statement.Type = Statement::Kind::Align;
                        statement.Text = text;
                        statement.Line = line.Line;
                        statement.Alignment = *alignment;
                        statement.Data.assign(1, static_cast<std::uint8_t>(*fill));
                        m_statements.push_back(std::move(statement));
                        continue;
                    }
                    Statement statement;
                    statement.Type = Statement::Kind::Code;
                    statement.Text = text;
                    statement.Line = line.Line;
                    m_statements.push_back(std::move(statement));
                }
                PromotePotentialLabels();
                for (const auto& [name, line] : m_register)
                {
                    const std::string key = Lower(name);
                    const bool declared = m_declared.count(key) || m_defines.count(key);
                    if (!declared)
                        return Fail(ErrorCode::AaBadArguments, line, "registersymbol(" + name + "): " + name + " is not declared here with alloc, label, define or an aobscan");
                }
                return true;
            }

            // ∑ CE's getPotentialLabels (autoassembler.pas 708-731, used at 2975 and 3027): without
            //   {$strict}, a "name:" line whose name is no symbol, no number and has no + or . is a
            //   label even without label(name). In a syntax check that also covers a [DISABLE] part
            //   whose names the [ENABLE] part registers; the real pass still needs an address for it.
            void PromotePotentialLabels()
            {
                if (m_strict)
                    return;
                for (const Statement& statement : m_statements)
                {
                    if (statement.Type != Statement::Kind::Place)
                        continue;
                    const std::string& name = statement.Text;
                    const std::string key = Lower(name);
                    if (name.empty() || m_declared.count(key) || !std::all_of(name.begin(), name.end(), [](char c) { return IsWordChar(c) && c != '.'; }))
                        continue;
                    if (std::all_of(name.begin(), name.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
                        continue;
                    // ∑ Only what this run knows decides that a name is an address: CE resolves labels
                    //   anew on every run. The values that earlier runs left in m_memory.Values (every
                    //   placed label of the Auto Assemble window's runs, the [ENABLE] part's labels in
                    //   its [DISABLE] part) made "skip:" an address line on the second run, so the code
                    //   after it went into the old, live block.
                    //   The same goes for a symbol an earlier run of this memory registered and this
                    //   run registers again (registersymbol(flag) with flag:): it is this run's label,
                    //   and Finish moves the symbol to it.
                    if (KnownName(name, false))
                        continue;
                    const auto matches = [&](const std::string& other) { return EqualsNoCase(other, name); };
                    const bool registeredHere = std::any_of(m_register.begin(), m_register.end(), [&](const auto& registered) { return matches(registered.first); });
                    const bool reRegistered = registeredHere && std::any_of(m_memory.Registered.begin(), m_memory.Registered.end(), matches);
                    if (!reRegistered && m_host.Symbols && m_host.Symbols->Evaluate(name, Extra(false)).Ok())
                        continue;
                    // ∑ Stricter than CE on the real pass: the name must also be used by the code (a
                    //   jump to it) or registered by registersymbol, as the syntax check accepts it.
                    //   A bare "Hook:" whose symbol is missing, say in a [DISABLE] part after a failed
                    //   enable, would otherwise become a label at the current address and write the
                    //   original bytes to the wrong place.
                    if (!m_options.CheckOnly)
                    {
                        const bool used = std::any_of(m_statements.begin(), m_statements.end(), [&](const Statement& other) {
                            return other.Type == Statement::Kind::Code && ReplaceWord(Lower(other.Text), key, "\x02") != Lower(other.Text);
                        }) || registeredHere;
                        if (!used)
                            continue;
                    }
                    m_labels[key] = name;
                    m_declared.insert(key);
                    m_potential.insert(key);
                }
            }

            bool NeedArguments(const Command& command, const SourceLine& line, std::size_t least, const char* usage)
            {
                if (command.Arguments.size() >= least)
                    return true;
                return Fail(ErrorCode::AaBadArguments, line.Line, usage, line.Text);
            }

            bool DeclareName(const std::string& name, int line, const std::string& text)
            {
                const std::string key = Lower(name);
                if (m_declared.count(key))
                    return Fail(ErrorCode::AaLabelTwice, line, "\"" + name + "\" is declared twice", text);
                m_declared.insert(key);
                return true;
            }

            bool HandleCommand(const Command& command, const SourceLine& line)
            {
                const std::string& name = command.Name;
                if (name == "alloc" || name == "globalalloc")
                {
                    if (!NeedArguments(command, line, 2, "alloc(name, size[, near address])"))
                        return false;
                    const auto size = ParseSize(command.Arguments[1]);
                    if (!size || *size == 0)
                        return Fail(ErrorCode::AaBadArguments, line.Line, "the size of alloc is not a number", line.Text);
                    // ∑ CE reads the size with StrToInt, a 32-bit Integer, so nothing above 7FFFFFFF
                    //   gets through there. Here a bigger one wrapped (size+15)&~15 to a few bytes, and
                    //   the next alloc of the group was placed inside it.
                    if (*size > kMaxAllocSize)
                        return Fail(ErrorCode::AaBadArguments, line.Line, "the size of alloc is too large (at most 7FFFFFFF bytes)", line.Text);
                    if (!DeclareName(command.Arguments[0], line.Line, line.Text))
                        return false;
                    PendingAlloc alloc;
                    alloc.Name = command.Arguments[0];
                    alloc.Size = *size;
                    alloc.Near = command.Arguments.size() > 2 ? command.Arguments[2] : "";
                    alloc.Global = name == "globalalloc";
                    alloc.Line = line.Line;
                    m_allocs.push_back(std::move(alloc));
                    return true;
                }
                if (name == "dealloc")
                {
                    if (!NeedArguments(command, line, 1, "dealloc(name)"))
                        return false;
                    for (const std::string& allocation : NameList(command.Arguments))
                        m_deallocs.push_back(allocation);
                    return true;
                }
                if (name == "label")
                {
                    if (!NeedArguments(command, line, 1, "label(name)"))
                        return false;
                    for (const std::string& label : NameList(command.Arguments))
                    {
                        if (m_labels.count(Lower(label)))
                            continue;
                        if (!DeclareName(label, line.Line, line.Text))
                            return false;
                        m_labels[Lower(label)] = label;
                    }
                    return true;
                }
                if (name == "registersymbol")
                {
                    if (!NeedArguments(command, line, 1, "registersymbol(name)"))
                        return false;
                    for (const std::string& symbol : NameList(command.Arguments))
                        m_register.push_back({ symbol, line.Line });
                    return true;
                }
                if (name == "unregistersymbol")
                {
                    if (!NeedArguments(command, line, 1, "unregistersymbol(name)"))
                        return false;
                    for (const std::string& symbol : NameList(command.Arguments))
                        m_unregister.push_back(symbol);
                    return true;
                }
                if (name == "aobscan" || name == "aobscanmodule" || name == "aobscanregion")
                {
                    const std::size_t least = name == "aobscan" ? 2 : name == "aobscanmodule" ? 3 : 4;
                    if (!NeedArguments(command, line, least, name == "aobscan" ? "aobscan(name, bytes)" : name == "aobscanmodule" ? "aobscanmodule(name, module, bytes)" : "aobscanregion(name, start, stop, bytes)"))
                        return false;
                    if (!DeclareName(command.Arguments[0], line.Line, line.Text))
                        return false;
                    m_scans.push_back({ name, command.Arguments, line.Line });
                    return true;
                }
                if (name == "assert")
                {
                    if (!NeedArguments(command, line, 2, "assert(address, bytes)"))
                        return false;
                    m_asserts.push_back({ command.Arguments[0], command.Arguments[1], line.Line });
                    return true;
                }
                if (name == "fullaccess")
                {
                    if (!NeedArguments(command, line, 2, "fullaccess(address, size)"))
                        return false;
                    m_fullAccess.push_back({ command.Arguments[0], command.Arguments[1], line.Line });
                    return true;
                }
                if (name == "createthread" || name == "createthreadandwait")
                {
                    if (!NeedArguments(command, line, 1, "createthread(address)"))
                        return false;
                    m_threads.push_back({ command.Arguments[0], name == "createthreadandwait" ? "wait" : "", line.Line });
                    return true;
                }
                if (name == "loadlibrary")
                {
                    if (!NeedArguments(command, line, 1, "loadlibrary(path)"))
                        return false;
                    m_libraries.push_back({ Unquote(command.Arguments[0]), line.Line });
                    return true;
                }
                if (name == "readmem")
                {
                    if (!NeedArguments(command, line, 2, "readmem(address, size)"))
                        return false;
                    if (!m_host.Process || !m_host.Symbols)
                        return Fail(ErrorCode::NoProcess, line.Line, "");
                    const auto size = ParseSize(command.Arguments[1]);
                    if (!size || *size == 0 || *size > 0x100000)
                        return Fail(ErrorCode::AaBadArguments, line.Line, "readmem(address, size) got an invalid address or size", line.Text);
                    Statement statement;
                    statement.Type = Statement::Kind::Bytes;
                    statement.Data.resize(static_cast<std::size_t>(*size));
                    statement.Line = line.Line;
                    statement.Text = line.Text;
                    m_reads.push_back({ m_statements.size(), command.Arguments[0], line.Line, line.Text });
                    m_statements.push_back(std::move(statement));
                    return true;
                }
                if (name == "reassemble")
                {
                    if (!NeedArguments(command, line, 1, "reassemble(address)"))
                        return false;
                    Statement statement;
                    statement.Type = Statement::Kind::Code;
                    statement.Text = "\x01reassemble " + command.Arguments[0];
                    statement.Line = line.Line;
                    m_statements.push_back(std::move(statement));
                    return true;
                }
                if (name == "luacall")
                {
                    if (!m_host.RunLua)
                        return Fail(ErrorCode::AaLuaFailed, line.Line, "Lua is not available");
                    std::string code = line.Text.substr(line.Text.find('(') + 1);
                    code.pop_back();
                    if (!m_options.CheckOnly)
                    {
                        std::string ignored;
                        const Status status = m_host.RunLua(code, false, ignored);
                        if (!status)
                        {
                            m_result.Error = Status::Fail(ErrorCode::AaLuaFailed, status.Text);
                            m_result.Line = line.Line;
                            return false;
                        }
                    }
                    return true;
                }
                return Fail(ErrorCode::AaUnknownCommand, line.Line, "unknown command " + name, line.Text);
            }

            // earlierValues: also the labels and scan results earlier runs left in m_memory.Values
            // (the allocations of earlier runs always count; dealloc and the [DISABLE] part need them).
            std::optional<std::uint64_t> KnownName(std::string_view name, bool earlierValues = true) const
            {
                const std::string key = Lower(name);
                if (const auto found = m_values.find(key); found != m_values.end())
                    return found->second;
                if (const auto found = m_placed.find(key); found != m_placed.end())
                    return found->second;
                for (const AaAllocation& allocation : m_memory.Allocations)
                {
                    if (EqualsNoCase(allocation.Name, name))
                        return allocation.Address;
                }
                if (!earlierValues)
                    return std::nullopt;
                if (const auto found = m_memory.Values.find(key); found != m_memory.Values.end() && !m_labels.count(key))
                    return found->second;
                return std::nullopt;
            }

            Target::ExtraResolver Extra(bool earlierValues = true) const
            {
                return [this, earlierValues](std::string_view name) -> std::optional<std::uint64_t> { return KnownName(name, earlierValues); };
            }

            bool PrepareLibraries()
            {
                if (m_options.CheckOnly)
                    return true;
                for (const auto& [path, line] : m_libraries)
                {
                    if (!m_host.Process)
                        return Fail(ErrorCode::NoProcess, line, "");
                    std::filesystem::path file = Utf8ToWide(path);
                    if (file.is_relative() && !m_options.IncludeFolder.empty() && std::filesystem::exists(m_options.IncludeFolder / file))
                        file = m_options.IncludeFolder / file;
                    LOG_WARN("Script", "loadlibrary: " + WideToUtf8(file.wstring()));
                    const Status status = m_host.Process->InjectLibrary(file.wstring());
                    if (!status)
                    {
                        m_result.Error = status;
                        m_result.Line = line;
                        return false;
                    }
                }
                return true;
            }

            bool RunScans()
            {
                for (const ScanCommand& scan : m_scans)
                {
                    const std::string& patternText = scan.Arguments.back();
                    const auto pattern = Scan::ParsePattern(patternText);
                    if (!pattern)
                    {
                        m_result.Error = Status::Fail(ErrorCode::ScanBadPattern, patternText);
                        m_result.Line = scan.Line;
                        return false;
                    }
                    // ∑ CE turns every aobscan into DEFINE(name, 00000000) during a syntax check and
                    //   scans only on the real pass. A check that scanned would fail on a module that is
                    //   not loaded or a pattern that is gone, where CE's check passes, and every
                    //   activation that checks first would scan twice. The program's own checks
                    //   (Engine::CheckScript: the script editor, the record checks) still scan, since
                    //   telling the user that a pattern is gone is what they are for.
                    if (m_options.CheckOnly && !m_options.ScanInCheck)
                    {
                        m_values[Lower(scan.Arguments[0])] = 0;
                        continue;
                    }
                    if (!m_host.Process || !m_host.Symbols)
                        return Fail(ErrorCode::NoProcess, scan.Line, "");
                    std::uint64_t start = 0;
                    std::uint64_t stop = m_host.Process->HighestAddress();
                    if (scan.Kind == "aobscanmodule")
                    {
                        const auto module = m_host.Process->FindModule(Unquote(scan.Arguments[1]));
                        if (!module)
                        {
                            m_result.Error = Status::Fail(ErrorCode::ExprUnknownSymbol, Unquote(scan.Arguments[1]));
                            m_result.Line = scan.Line;
                            return false;
                        }
                        start = module->Base;
                        stop = module->Base + module->Size;
                    }
                    else if (scan.Kind == "aobscanregion")
                    {
                        const auto from = m_host.Symbols->Evaluate(scan.Arguments[1], Extra());
                        const auto to = m_host.Symbols->Evaluate(scan.Arguments[2], Extra());
                        if (!from.Ok() || !to.Ok())
                            return Fail(ErrorCode::AaBadArguments, scan.Line, "the region of aobscanregion is not valid");
                        start = *from.Value;
                        stop = *to.Value;
                    }
                    const auto hits = Scan::FindPattern(*m_host.Process, *pattern, start, stop, false, 1);
                    if (hits.empty())
                    {
                        m_result.Error = Status::Fail(ErrorCode::AaPatternNotFound, scan.Arguments[0]);
                        m_result.Line = scan.Line;
                        return false;
                    }
                    m_values[Lower(scan.Arguments[0])] = hits.front();
                    m_result.Notes.push_back(scan.Arguments[0] + " = " + Hex(hits.front()));
                }
                return true;
            }

            bool Allocate()
            {
                // ∑ CE groups allocations in script order (autoassembler.pas 3359-3445): one without a
                //   near address joins the running group, and a different near address starts a new
                //   one. So alloc(data,8) next to alloc(newmem,$1000,INJECT) lands beside newmem, in
                //   reach of rip-relative code, instead of anywhere in the 64-bit address space.
                struct Group
                {
                    std::vector<PendingAlloc*> Members;
                    std::optional<std::uint64_t> Near;
                };
                std::vector<Group> groups;
                std::optional<std::size_t> running;
                for (PendingAlloc& alloc : m_allocs)
                {
                    if (alloc.Global && m_host.Symbols)
                    {
                        if (const auto existing = m_host.Symbols->FindRegistered(alloc.Name))
                        {
                            m_values[Lower(alloc.Name)] = *existing;
                            continue;
                        }
                    }
                    std::optional<std::uint64_t> nearValue;
                    if (!alloc.Near.empty())
                    {
                        if (!m_host.Symbols)
                            return Fail(ErrorCode::NoProcess, alloc.Line, "");
                        const auto value = m_host.Symbols->Evaluate(alloc.Near, Extra());
                        if (!value.Ok())
                        {
                            m_result.Error = value.Error;
                            m_result.Line = alloc.Line;
                            return false;
                        }
                        nearValue = *value.Value;
                    }
                    if (alloc.Global)
                    {
                        groups.push_back({ { &alloc }, nearValue });
                        continue;
                    }
                    if (running && nearValue && groups[*running].Near && *groups[*running].Near != *nearValue)
                        running.reset();
                    if (!running)
                    {
                        groups.push_back({});
                        running = groups.size() - 1;
                    }
                    Group& group = groups[*running];
                    group.Members.push_back(&alloc);
                    if (nearValue && !group.Near)
                        group.Near = nearValue;
                }

                std::uint64_t fakeBase = kCheckOnlyBase;
                for (const Group& group : groups)
                {
                    const auto& members = group.Members;
                    const PendingAlloc& first = *members.front();
                    // ∑ Each size is at most 7FFFFFFF (HandleCommand), but a group adds them up: the
                    //   sum must not wrap, and it must fit the address space of the target (4 GB for
                    //   a WOW64 one).
                    const std::uint64_t limit = m_is64 ? (~0ull >> 1) : 0xFFFFFFFFull;
                    std::uint64_t total = 0;
                    for (PendingAlloc* alloc : members)
                    {
                        const std::uint64_t rounded = (alloc->Size + 15) & ~15ull;
                        if (rounded > limit - total)
                            return Fail(ErrorCode::AaBadArguments, alloc->Line, "the allocations of this group are too large together");
                        total += rounded;
                    }
                    const std::uint64_t nearAddress = group.Near.value_or(0);
                    std::uint64_t block = 0;
                    if (!m_options.CheckOnly && !m_host.Process)
                        return Fail(ErrorCode::NoProcess, first.Line, "");
                    if (m_options.CheckOnly)
                    {
                        block = nearAddress ? (nearAddress & ~0xFFFFull) + 0x100000 : fakeBase;
                        fakeBase += (total + 0xFFFF) & ~0xFFFFull;
                        m_fakeBlocks.emplace_back(block, block + total);
                    }
                    else
                    {
                        const Result<Address> allocated = m_host.Process->Allocate(static_cast<std::size_t>(total), PAGE_EXECUTE_READWRITE, m_is64 ? nearAddress : 0);
                        if (!allocated)
                        {
                            m_result.Error = Status::Fail(ErrorCode::AaAllocFailed, first.Name + " (" + allocated.Error().Text + ")");
                            m_result.Line = first.Line;
                            return false;
                        }
                        block = *allocated;
                        m_allocatedBlocks.push_back(block);
                        LOG_DEBUG("Auto Assembler", "Allocated " + Hex(total) + " bytes at " + Hex(block));
                    }
                    std::uint64_t offset = 0;
                    for (PendingAlloc* alloc : members)
                    {
                        const std::uint64_t address = block + offset;
                        m_values[Lower(alloc->Name)] = address;
                        m_newAllocations.push_back({ alloc->Name, address, alloc->Size, block, alloc->Global });
                        offset += (alloc->Size + 15) & ~15ull;
                    }
                }
                return true;
            }

            bool ReadMemory()
            {
                for (const PendingRead& read : m_reads)
                {
                    Statement& statement = m_statements[read.Index];
                    const auto address = m_host.Symbols->Evaluate(read.Address, Extra());
                    // ∑ CE reads nothing for readmem during a syntax check, and an address it cannot
                    //   resolve there becomes 0. Custom commands rely on that: they answer a check with
                    //   define(Name, 0) and run their scan only on the real pass, so a check that read
                    //   (or resolved) the source would reject scripts that CE accepts. Here a check still
                    //   copies the bytes when they can be read, and keeps zeros when they cannot.
                    if (m_options.CheckOnly)
                    {
                        const auto fake = [&](std::uint64_t at) {
                            return std::any_of(m_fakeBlocks.begin(), m_fakeBlocks.end(), [at](const auto& block) { return at >= block.first && at < block.second; });
                        };
                        if (address.Ok() && !fake(*address.Value) && !m_host.Process->ReadExact(*address.Value, statement.Data.data(), statement.Data.size()))
                            std::fill(statement.Data.begin(), statement.Data.end(), std::uint8_t(0));
                        continue;
                    }
                    if (!address.Ok())
                        return Fail(ErrorCode::AaBadArguments, read.Line, "readmem(address, size) got an invalid address or size", read.Text);
                    if (!m_host.Process->ReadExact(*address.Value, statement.Data.data(), statement.Data.size()))
                    {
                        m_result.Error = Status::Fail(ErrorCode::AaReadMemFailed, Hex(*address.Value));
                        m_result.Line = read.Line;
                        return false;
                    }
                }
                return true;
            }

            void ReleaseAllocations()
            {
                if (!m_host.Process || m_options.CheckOnly)
                    return;
                for (std::uint64_t block : m_allocatedBlocks)
                    m_host.Process->Free(block);
                m_allocatedBlocks.clear();
            }

            std::optional<std::uint64_t> PlaceAddress(const Statement& statement, std::uint64_t current, bool& isLabel)
            {
                const std::string key = Lower(statement.Text);
                isLabel = m_labels.count(key) > 0;
                if (isLabel)
                    return current;
                if (const auto known = KnownName(statement.Text))
                    return known;
                if (!m_host.Symbols)
                    return std::nullopt;
                const auto value = m_host.Symbols->Evaluate(statement.Text, Extra());
                if (!value.Ok())
                    return std::nullopt;
                return value.Value;
            }

            bool Layout()
            {
                std::vector<std::size_t> reserved(m_statements.size(), 0);
                std::map<std::string, std::uint64_t> previousLabels;
                for (int pass = 0; pass < kMaxLayoutPasses; ++pass)
                {
                    m_placed.clear();
                    m_output.clear();
                    m_unplacedUses.clear();
                    std::optional<std::uint64_t> current;
                    bool changed = false;
                    for (std::size_t i = 0; i < m_statements.size(); ++i)
                    {
                        const Statement& statement = m_statements[i];
                        if (statement.Type == Statement::Kind::Place)
                        {
                            bool isLabel = false;
                            const auto address = PlaceAddress(statement, current.value_or(0), isLabel);
                            if (isLabel)
                            {
                                if (!current && m_options.CheckOnly)
                                    current = 0;
                                if (!current && m_potential.count(Lower(statement.Text)))
                                {
                                    m_result.Error = Status::Fail(ErrorCode::ExprUnknownSymbol, statement.Text);
                                    m_result.Line = statement.Line;
                                    return false;
                                }
                                if (!current)
                                    return Fail(ErrorCode::AaLabelNotPlaced, statement.Line, statement.Text);
                                const std::string key = Lower(statement.Text);
                                if (m_placed.count(key))
                                    return Fail(ErrorCode::AaLabelTwice, statement.Line, "label placed twice", statement.Text);
                                m_placed[key] = *current;
                                continue;
                            }
                            if (!address)
                            {
                                const auto value = m_host.Symbols ? m_host.Symbols->Evaluate(statement.Text, Extra()) : Target::Evaluation{};
                                m_result.Error = !value.Error.Ok() ? value.Error : Status::Fail(ErrorCode::ExprSyntax, statement.Text);
                                m_result.Line = statement.Line;
                                return false;
                            }
                            current = *address;
                            continue;
                        }
                        if (!current && m_options.CheckOnly)
                            current = 0;
                        if (!current)
                            return Fail(ErrorCode::AsmSyntax, statement.Line, "no address is set for this line; start the block with an address line like newmem:", statement.Text);

                        if (statement.Type == Statement::Kind::Align)
                        {
                            // ∑ CE's align (Assemblerunit.pas 4938): pad with the fill byte up to the next
                            //   multiple. The pad depends on the address, so it never keeps a reserve
                            //   from an earlier pass; moved labels make the layout run again.
                            const std::uint64_t pad = (statement.Alignment - (*current % statement.Alignment)) % statement.Alignment;
                            m_output.push_back({ *current, std::vector<std::uint8_t>(static_cast<std::size_t>(pad), statement.Data.front()), statement.Line });
                            *current += pad;
                            continue;
                        }
                        std::vector<std::uint8_t> bytes;
                        if (statement.Type == Statement::Kind::Bytes)
                        {
                            bytes = statement.Data;
                        }
                        else
                        {
                            AssembleResult assembled = AssembleAt(statement, *current, previousLabels, true, false);
                            if (!assembled.Ok)
                                assembled = AssembleAt(statement, *current, previousLabels, false, false);
                            if (!assembled.Ok)
                                return Fail(CodeFor(assembled.Error), statement.Line, assembled.Message, statement.Text.front() == '\x01' ? statement.Text.substr(1) : statement.Text);
                            if (assembled.Bytes.size() < reserved[i])
                            {
                                AssembleResult longer = AssembleAt(statement, *current, previousLabels, true, true);
                                if (longer.Ok && longer.Bytes.size() > assembled.Bytes.size() && longer.Bytes.size() <= reserved[i])
                                    assembled = std::move(longer);
                            }
                            bytes = std::move(assembled.Bytes);
                        }
                        if (bytes.size() > reserved[i])
                        {
                            reserved[i] = bytes.size();
                            changed = true;
                        }
                        while (bytes.size() < reserved[i])
                            bytes.push_back(0x90);
                        m_output.push_back({ *current, bytes, statement.Line });
                        *current += bytes.size();
                    }
                    if (!changed && m_placed == previousLabels)
                    {
                        // ∑ A label the code uses but never places was assembled against a guess
                        //   (the previous pass, or a few bytes ahead), so label(returnhere) with a
                        //   typo in its "returnhere:" line wrote a jump to a made-up address into the
                        //   game. Every label the last pass asked for that is still not placed fails,
                        //   like a registered one. CE goes further and refuses any label() whose
                        //   "name:" line is missing (rsLabelIsNotDefinedInTheScript); an unused one
                        //   stays allowed here, which shared headers and the program's own checks
                        //   rely on.
                        for (const auto& [key, label] : m_labels)
                        {
                            if (m_placed.count(key))
                                continue;
                            if (const auto use = m_unplacedUses.find(key); use != m_unplacedUses.end())
                                return Fail(ErrorCode::AaLabelNotPlaced, use->second, label);
                            bool used = false;
                            for (const auto& [symbol, line] : m_register)
                                used = used || EqualsNoCase(symbol, label);
                            if (used)
                                return Fail(ErrorCode::AaLabelNotPlaced, 0, label);
                        }
                        return true;
                    }
                    previousLabels = m_placed;
                }
                return Fail(ErrorCode::AaLayoutUnstable, 0, "");
            }

            static bool IsData(const Statement& statement)
            {
                if (statement.Type == Statement::Kind::Bytes)
                    return true;
                if (statement.Type != Statement::Kind::Code)
                    return false;
                const std::string text = Lower(Trim(statement.Text));
                const std::string word = text.substr(0, text.find_first_of(" \t"));
                return word == "db" || word == "dw" || word == "dd" || word == "dq";
            }

            void MeasureLabels()
            {
                std::size_t written = 0;
                for (std::size_t i = 0; i < m_statements.size(); ++i)
                {
                    const Statement& statement = m_statements[i];
                    if (statement.Type != Statement::Kind::Place)
                    {
                        ++written;
                        continue;
                    }
                    const std::string key = Lower(statement.Text);
                    if (!m_labels.count(key))
                        continue;
                    std::uint64_t size = 0;
                    for (std::size_t next = i + 1, output = written; next < m_statements.size() && IsData(m_statements[next]) && output < m_output.size(); ++next, ++output)
                        size += m_output[output].Bytes.size();
                    if (size)
                        m_sizes[key] = size;
                }
            }

            AssembleResult AssembleAt(const Statement& statement, std::uint64_t address, const std::map<std::string, std::uint64_t>& previousLabels, bool farGuess, bool longBranches)
            {
                AssembleOptions options;
                options.Is64Bit = m_is64;
                options.Address = address;
                options.AllowFarBranches = true;
                options.DefaultToDword = true;
                options.LongBranches = longBranches;
                options.Resolve = [&](std::string_view name) -> SymbolValue
                    {
                        const std::string key = Lower(name);
                        if (const auto placed = m_placed.find(key); placed != m_placed.end())
                            return { SymbolState::Known, placed->second };
                        if (m_labels.count(key))
                        {
                            // A forward reference is placed later in the pass; Layout checks the rest.
                            m_unplacedUses.emplace(key, statement.Line);
                            if (const auto before = previousLabels.find(key); before != previousLabels.end())
                                return { SymbolState::Known, before->second };
                            return { SymbolState::Known, farGuess ? address + kForwardGuess : address + 0x10 };
                        }
                        if (const auto known = KnownName(name))
                            return { SymbolState::Known, *known };
                        if (m_host.Symbols)
                        {
                            if (const auto value = m_host.Symbols->Resolve(name))
                                return { SymbolState::Known, *value };
                        }
                        return { SymbolState::Unknown, 0 };
                    };
                std::string text = statement.Text;
                if (!text.empty() && text.front() == '\x01')
                {
                    const std::string target = Trim(text.substr(std::string("\x01reassemble ").size()));
                    if ((!m_host.Process || !m_host.Symbols) && m_options.CheckOnly)
                    {
                        AssembleResult placeholder;
                        placeholder.Ok = true;
                        placeholder.Bytes.assign(5, 0x90);
                        return placeholder;
                    }
                    if (!m_host.Process || !m_host.Symbols)
                    {
                        AssembleResult failed;
                        failed.Error = AsmError::Syntax;
                        failed.Message = "reassemble needs an open process";
                        return failed;
                    }
                    const auto source = m_host.Symbols->Evaluate(target, Extra());
                    // ∑ A syntax check must not depend on the game's memory: custom commands answer the
                    //   check with define(Name, 0), so reassemble(Name+4) points at nothing. CE's own
                    //   placeholder for this case is nop; any source that does not resolve, read or
                    //   assemble becomes that during a check, and only the real pass reports it.
                    const auto checkPlaceholder = [&]() {
                        AssembleResult placeholder;
                        placeholder.Ok = true;
                        placeholder.Bytes.assign(1, 0x90);
                        return placeholder;
                    };
                    if (!source.Ok())
                    {
                        if (m_options.CheckOnly)
                            return checkPlaceholder();
                        AssembleResult failed;
                        failed.Error = AsmError::UnknownSymbol;
                        failed.Message = "reassemble: " + source.Error.Text;
                        return failed;
                    }
                    Disasm::Disassembler disassembler(m_is64);
                    const Disasm::Instruction instruction = disassembler.Read(*m_host.Process, *source.Value, nullptr);
                    if (m_options.CheckOnly)
                    {
                        if (!instruction.Valid)
                            return checkPlaceholder();
                        AssembleResult checked = Assemble(instruction.Text(), options);
                        return checked.Ok ? checked : checkPlaceholder();
                    }
                    text = instruction.Text();
                }
                return Assemble(text, options);
            }

            bool CheckAsserts()
            {
                // ∑ CE skips assert during a syntax check: the address may come from a custom command's
                //   define(Name, 0) answer, and the bytes only mean something on the real pass.
                if (m_options.CheckOnly)
                {
                    for (const AssertCommand& check : m_asserts)
                    {
                        if (!Scan::ParsePattern(check.Bytes))
                        {
                            m_result.Error = Status::Fail(ErrorCode::ScanBadPattern, check.Bytes);
                            m_result.Line = check.Line;
                            return false;
                        }
                    }
                    return true;
                }
                for (const AssertCommand& check : m_asserts)
                {
                    if (!m_host.Process || !m_host.Symbols)
                        return Fail(ErrorCode::NoProcess, check.Line, "");
                    const auto address = m_host.Symbols->Evaluate(check.Address, Extra());
                    if (!address.Ok())
                    {
                        m_result.Error = address.Error;
                        m_result.Line = check.Line;
                        return false;
                    }
                    const auto pattern = Scan::ParsePattern(check.Bytes);
                    if (!pattern)
                    {
                        m_result.Error = Status::Fail(ErrorCode::ScanBadPattern, check.Bytes);
                        m_result.Line = check.Line;
                        return false;
                    }
                    std::vector<std::uint8_t> actual(pattern->Bytes.size());
                    if (!m_host.Process->ReadExact(*address.Value, actual.data(), actual.size()) || !pattern->Matches(actual.data()))
                    {
                        m_result.Error = Status::Fail(ErrorCode::AaAssertFailed, check.Address + " (found " + HexBytes(actual) + ")");
                        m_result.Line = check.Line;
                        return false;
                    }
                }
                return true;
            }

            bool WriteCode()
            {
                if (!m_host.Process)
                {
                    if (m_output.empty() && m_fullAccess.empty())
                        return true;
                    return Fail(ErrorCode::NoProcess, m_output.empty() ? 0 : m_output.front().Line, "");
                }
                for (const FullAccessCommand& access : m_fullAccess)
                {
                    const auto address = m_host.Symbols->Evaluate(access.Address, Extra());
                    const auto size = ParseSize(access.Size);
                    if (!address.Ok() || !size)
                        return Fail(ErrorCode::AaBadArguments, access.Line, "fullaccess(address, size) got an invalid address or size");
                    // ∑ CE calls virtualprotectex for fullaccess and ignores the result, so a failure
                    //   does not stop the script here either; it is no longer silent, though. The old
                    //   protection is not put back on a later failure: CE never restores it.
                    if (!m_host.Process->Protect(*address.Value, static_cast<std::size_t>(*size), PAGE_EXECUTE_READWRITE))
                    {
                        const std::string message = "fullaccess(" + access.Address + ", " + access.Size + "): could not make " + Hex(*address.Value) + " writable and executable";
                        LOG_WARN("Auto Assembler", message);
                        m_result.Notes.push_back(message);
                    }
                }

                std::vector<AaWrite> merged;
                for (const AaWrite& write : m_output)
                {
                    if (!merged.empty() && merged.back().Address + merged.back().Bytes.size() == write.Address)
                        merged.back().Bytes.insert(merged.back().Bytes.end(), write.Bytes.begin(), write.Bytes.end());
                    else
                        merged.push_back(write);
                }
                merged.erase(std::remove_if(merged.begin(), merged.end(), [](const AaWrite& write) { return write.Bytes.empty(); }), merged.end());
                // ∑ A failed write must not leave the earlier ones behind: Abort frees this run's
                //   allocations, and a hook already written into the game would then jump into freed
                //   memory. The bytes each write replaces are kept, and on a failure every finished
                //   write is put back (newest first) before the error is returned. The original is
                //   read before each write: a range whose bytes cannot be read could not be put
                //   back, so it fails before anything is written to it, and the failing write
                //   itself is undone too (Process::Write can copy part of the range before it
                //   fails, and that part stayed in the game).
                std::vector<AaWrite> undo;
                const auto restore = [&]() {
                    for (auto done = undo.rbegin(); done != undo.rend(); ++done)
                    {
                        if (!m_host.Process->Write(done->Address, done->Bytes))
                            LOG_WARN("Auto Assembler", "Could not restore the bytes at " + Hex(done->Address) + " after a failed write.");
                    }
                };
                for (const AaWrite& write : merged)
                {
                    AaWrite original{ write.Address, std::vector<std::uint8_t>(write.Bytes.size()), write.Line };
                    if (!m_host.Process->ReadExact(write.Address, original.Bytes.data(), original.Bytes.size()))
                    {
                        restore();
                        m_result.Error = Status::Fail(ErrorCode::AaWriteFailed, Hex(write.Address) + " (the bytes there cannot be read, so the write could not be undone)");
                        m_result.Line = write.Line;
                        return false;
                    }
                    undo.push_back(std::move(original));
                    const Status status = m_host.Process->Write(write.Address, write.Bytes);
                    if (!status)
                    {
                        restore();
                        m_result.Error = Status::Fail(ErrorCode::AaWriteFailed, Hex(write.Address));
                        m_result.Line = write.Line;
                        return false;
                    }
                }
                m_result.Writes = std::move(merged);
                return true;
            }

            // ∑ CE checks createthread targets before it writes anything (autoassembler.pas 3116-3165);
            //   a target that does not resolve used to be a note after the code was already in place.
            bool CheckThreads()
            {
                for (const ThreadCommand& thread : m_threads)
                {
                    if (!FinalValue(thread.Address))
                        return Fail(ErrorCode::AaBadArguments, thread.Line, "createthread(" + thread.Address + "): the address does not resolve");
                }
                return true;
            }

            std::optional<std::uint64_t> FinalValue(std::string_view name)
            {
                if (const auto known = KnownName(name))
                    return known;
                if (m_host.Symbols)
                {
                    const auto value = m_host.Symbols->Evaluate(name, Extra());
                    if (value.Ok())
                        return value.Value;
                }
                return std::nullopt;
            }

            void Finish()
            {
                // ∑ CE order (autoassembler.pas 2718-2758, 4234-4282): dealloc only frees what an earlier
                //   run allocated (never this run's code, which a new thread may be running), and the
                //   symbols to unregister go before the ones to register.
                std::vector<std::string> deallocs;
                for (const std::string& name : m_deallocs)
                {
                    if (name == "*")
                    {
                        for (const AaAllocation& allocation : m_memory.Allocations)
                            deallocs.push_back(allocation.Name);
                    }
                    else
                    {
                        deallocs.push_back(name);
                    }
                }
                for (const std::string& name : deallocs)
                {
                    auto found = std::find_if(m_memory.Allocations.begin(), m_memory.Allocations.end(), [&](const AaAllocation& allocation) { return EqualsNoCase(allocation.Name, name); });
                    if (found == m_memory.Allocations.end())
                        continue;
                    if (found->Global)
                    {
                        m_memory.Allocations.erase(found);
                        continue;
                    }
                    const std::uint64_t block = found->Block;
                    m_memory.Allocations.erase(found);
                    const bool stillUsed = std::any_of(m_memory.Allocations.begin(), m_memory.Allocations.end(), [&](const AaAllocation& allocation) { return allocation.Block == block; });
                    if (!stillUsed && m_host.Process)
                    {
                        const Status status = m_host.Process->Free(block);
                        if (!status)
                            m_result.Notes.push_back(status.Text);
                    }
                }
                for (const AaAllocation& allocation : m_newAllocations)
                    m_memory.Allocations.push_back(allocation);
                m_allocatedBlocks.clear();

                for (const auto& [key, value] : m_values)
                    m_memory.Values[key] = value;
                for (const auto& [key, value] : m_placed)
                    m_memory.Values[key] = value;

                std::vector<std::string> unregister;
                for (const std::string& name : m_unregister)
                {
                    if (name == "*")
                        unregister.insert(unregister.end(), m_memory.Registered.begin(), m_memory.Registered.end());
                    else
                        unregister.push_back(name);
                }
                for (const std::string& name : unregister)
                {
                    if (m_host.Symbols)
                        m_host.Symbols->Unregister(name);
                    m_memory.Registered.erase(std::remove_if(m_memory.Registered.begin(), m_memory.Registered.end(), [&](const std::string& known) { return EqualsNoCase(known, name); }),
                        m_memory.Registered.end());
                }

                for (const auto& [name, line] : m_register)
                {
                    const auto defined = m_defines.find(Lower(name));
                    const auto value = defined != m_defines.end() ? FinalValue(defined->second) : FinalValue(name);
                    if (!value)
                    {
                        m_result.Notes.push_back("registersymbol: " + name + " has no address");
                        continue;
                    }
                    const auto size = defined == m_defines.end() ? m_sizes.find(Lower(name)) : m_sizes.end();
                    if (m_host.Symbols)
                    {
                        m_host.Symbols->Register(name, *value, m_options.Owner);
                        if (size != m_sizes.end())
                            m_host.Symbols->SetSize(name, size->second);
                    }
                    m_result.Symbols[name] = *value;
                    if (size != m_sizes.end())
                        m_result.SymbolSizes[name] = size->second;
                    if (std::find_if(m_memory.Registered.begin(), m_memory.Registered.end(), [&](const std::string& known) { return EqualsNoCase(known, name); }) ==
                        m_memory.Registered.end())
                        m_memory.Registered.push_back(name);
                }
                for (const ThreadCommand& thread : m_threads)
                {
                    const auto address = FinalValue(thread.Address);
                    if (!address)
                    {
                        m_result.Notes.push_back("createthread: " + thread.Address + " has no address");
                        continue;
                    }
                    if (!m_host.Process)
                    {
                        m_result.Notes.push_back("createthread: no process is open");
                        continue;
                    }
                    const Status status = m_host.Process->CreateThread(*address, 0, !thread.Mode.empty());
                    if (!status)
                        m_result.Notes.push_back(status.Text);
                }

                for (const PendingAlloc& alloc : m_allocs)
                {
                    if (alloc.Global && m_host.Symbols && !m_host.Symbols->FindRegistered(alloc.Name))
                    {
                        if (const auto value = KnownName(alloc.Name))
                            m_host.Symbols->Register(alloc.Name, *value, "");
                    }
                }
            }

            struct ScanCommand
            {
                std::string Kind;
                std::vector<std::string> Arguments;
                int Line = 0;
            };

            struct AssertCommand
            {
                std::string Address;
                std::string Bytes;
                int Line = 0;
            };

            struct FullAccessCommand
            {
                std::string Address;
                std::string Size;
                int Line = 0;
            };

            struct ThreadCommand
            {
                std::string Address;
                std::string Mode;
                int Line = 0;
            };

            AaHost& m_host;
            AaMemory& m_memory;
            const AaOptions& m_options;
            bool m_is64 = true;
            AaResult m_result;

            std::vector<Statement> m_statements;
            std::vector<PendingRead> m_reads;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> m_fakeBlocks;
            std::vector<PendingAlloc> m_allocs;
            std::vector<std::string> m_deallocs;
            std::map<std::string, std::string> m_labels;
            std::set<std::string> m_declared;
            std::set<std::string> m_potential;
            bool m_strict = false;
            std::vector<std::pair<std::string, int>> m_register;
            std::map<std::string, std::string> m_defines;
            std::vector<std::string> m_unregister;
            std::vector<ScanCommand> m_scans;
            std::vector<AssertCommand> m_asserts;
            std::vector<FullAccessCommand> m_fullAccess;
            std::vector<ThreadCommand> m_threads;
            std::vector<std::pair<std::string, int>> m_libraries;

            std::map<std::string, std::uint64_t> m_values;
            std::map<std::string, std::uint64_t> m_placed;
            std::map<std::string, int> m_unplacedUses;  // labels the current layout pass used before (or without) placing them, with the first line
            std::map<std::string, std::uint64_t> m_sizes;
            std::vector<AaAllocation> m_newAllocations;
            std::vector<std::uint64_t> m_allocatedBlocks;
            std::vector<AaWrite> m_output;
        };
    }

    AaResult RunAutoAssembler(std::string_view script, AaHost& host, AaMemory& memory, const AaOptions& options)
    {
        Run run(host, memory, options);
        AaResult result = run.Execute(script);
        if (!result.Ok())
            LOG_WARN("Auto Assembler", result.Error.Text);
        return result;
    }

    bool HasSections(std::string_view script)
    {
        const std::string lower = Lower(script);
        return lower.find("[enable]") != std::string::npos && lower.find("[disable]") != std::string::npos;
    }

    std::string SectionText(std::string_view script, AaSection section)
    {
        std::string out;
        bool inEnable = false;
        bool inDisable = false;
        std::vector<std::string> lines = SplitLines(script);
        if (!lines.empty() && lines.back().empty())
            lines.pop_back();
        for (const std::string& line : lines)
        {
            const std::string trimmed = Trim(line);
            if (EqualsNoCase(trimmed, "[enable]"))
            {
                inEnable = true;
                inDisable = false;
                continue;
            }
            if (EqualsNoCase(trimmed, "[disable]"))
            {
                inEnable = false;
                inDisable = true;
                continue;
            }
            const bool header = !inEnable && !inDisable;
            if (header || section == AaSection::Whole || (section == AaSection::Enable && inEnable) || (section == AaSection::Disable && inDisable))
                out += line + "\n";
        }
        return out;
    }
}
