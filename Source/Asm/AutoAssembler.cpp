#include "Asm/AutoAssembler.h"

#include "Asm/Assembler.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Disasm/StaticRef.h"
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
        // A reassemble statement is kept as text behind this marker; a script line that
        // merely starts with the control character (a fuzzed table did) is no such statement.
        constexpr std::string_view kReassembleMarker = "\x01reassemble ";

        bool IsReassemble(std::string_view text)
        {
            return text.starts_with(kReassembleMarker);
        }
        constexpr std::uint64_t kCheckOnlyBase = 0x0000000010000000ull;
        constexpr std::uint64_t kForwardGuess = 0x10000;
        constexpr std::uint64_t kMaxAllocSize = 0x7FFFFFFF;

        struct SourceLine
        {
            std::string Text;
            int Line = 0;
            bool Generated = false;  // written by a custom command, whose own log step shows it
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
            bool Log = false;
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
                    else if (name == "log")
                        found.Log = true;
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
                "include", "luacall", "resolvestatic" };
            return kCommands.count(name) > 0;
        }

        std::string Unquote(std::string text)
        {
            text = Trim(text);
            if (text.size() >= 2 && ((text.front() == '"' && text.back() == '"') || (text.front() == '\'' && text.back() == '\'')))
                return text.substr(1, text.size() - 2);
            return text;
        }

        // ∑ Log lines stay one line each and readable: long patterns, Lua code and command output
        //   are cut, line breaks become " | ".
        std::string Short(std::string_view text, std::size_t limit = 96)
        {
            std::string out;
            for (const char c : TrimView(text))
            {
                if (c == '\r')
                    continue;
                if (c == '\n')
                {
                    while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
                        out.pop_back();
                    if (!out.empty() && !out.ends_with(" | "))
                        out += " | ";
                    continue;
                }
                out += c == '\t' ? ' ' : c;
            }
            while (out.ends_with(" | "))
                out.resize(out.size() - 3);
            if (out.size() > limit)
            {
                out.resize(limit);
                out += "...";
            }
            return out;
        }

        std::string BytesPreview(std::span<const std::uint8_t> bytes, std::size_t limit = 16)
        {
            std::string text = HexBytes(bytes.first(std::min(bytes.size(), limit)));
            if (bytes.size() > limit)
                text += " ...";
            return text;
        }

        std::string ByteCount(std::uint64_t count)
        {
            return std::to_string(count) + (count == 1 ? " byte" : " bytes");
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
                m_log = m_options.Log || WantsLog(script);
                m_result.Logged = m_log;
                Steps(script);
                if (m_log)
                    LogEnd();
                return m_result;
            }

        private:
            void Steps(std::string_view script)
            {
                std::vector<SourceLine> lines;
                if (!Preprocess(script, lines))
                    return;
                if (!Collect(lines))
                    return Abort();
                if (!PrepareLibraries() || !RunScans() || !ResolveStatics() || !Allocate() || !ReadMemory())
                    return Abort();
                if (!Layout())
                    return Abort();
                MeasureLabels();
                CheckStackBalance();
                if (m_log)
                    LogLayout();
                if (!CheckAsserts() || !CheckThreads())
                    return Abort();
                if (m_options.CheckOnly)
                {
                    ReleaseAllocations();
                    return;
                }
                if (!WriteCode())
                    return Abort();
                Finish();
            }

            // ---------------------------------------------------------------- log

            // ∑ The steps are collected and written as ONE log entry when the run ends: one line per
            //   step made the Log window unreadable (a table switching ten hooks wrote hundreds of
            //   lines with the same prefix, and the check before every run doubled them). The list
            //   shows the summary, the details pane the steps as a table.
            void Step(int line, std::string command, std::string text)
            {
                if (!m_log)
                    return;
                if (m_result.Log.size() < kMaxLogLines)
                    m_result.Log.push_back({ line, std::move(command), std::move(text) });
                else
                    ++m_droppedLog;
            }

            std::string Where(std::uint64_t address) const
            {
                const std::string hex = Hex(address);
                if (!m_host.Symbols || m_options.CheckOnly)
                    return hex;
                const std::string name = m_host.Symbols->Describe(address);
                return name.empty() || name == hex ? hex : name + " (" + hex + ")";
            }

            std::string RunName() const
            {
                std::vector<std::string> parts;
                if (!m_options.LogTitle.empty())
                    parts.push_back(m_options.LogTitle);
                if (m_options.CheckOnly)
                    parts.push_back("check");
                if (m_options.Section != AaSection::Whole)
                    parts.push_back(m_options.Section == AaSection::Enable ? "[ENABLE]" : "[DISABLE]");
                return parts.empty() ? std::string("Script") : Join(parts, " ");
            }

            void LogEnd()
            {
                std::string summary = RunName();
                LogLevel level = LogLevel::Info;
                if (!m_result.Ok())
                {
                    std::string text = "failed";
                    if (m_result.Line > 0)
                        text += " at line " + std::to_string(m_result.Line);
                    text += ": " + m_result.Error.Text;
                    m_result.Log.push_back({ m_result.Line, "", text });
                    summary += " " + text;
                    level = LogLevel::Warning;
                }
                else if (m_options.CheckOnly)
                {
                    // ∑ A check that passes changes nothing; tables check every script before they run
                    //   it, so logging passed checks only buried the runs.
                    return;
                }
                else
                {
                    std::size_t bytes = 0;
                    for (const AaWrite& write : m_result.Writes)
                        bytes += write.Bytes.size();
                    std::vector<std::string> facts;
                    if (m_result.Writes.empty())
                        facts.push_back("nothing written");
                    else
                        facts.push_back(std::to_string(m_result.Writes.size()) + (m_result.Writes.size() == 1 ? " write (" : " writes (") + ByteCount(bytes) + ")");
                    if (!m_newAllocations.empty())
                        facts.push_back(std::to_string(m_newAllocations.size()) + (m_newAllocations.size() == 1 ? " allocation" : " allocations"));
                    if (m_freedBlocks)
                        facts.push_back(std::to_string(m_freedBlocks) + (m_freedBlocks == 1 ? " block freed" : " blocks freed"));
                    if (!m_result.Symbols.empty())
                    {
                        std::vector<std::string> names;
                        for (const auto& [name, value] : m_result.Symbols)
                        {
                            if (names.size() == 3)
                            {
                                names.push_back("+" + std::to_string(m_result.Symbols.size() - 3));
                                break;
                            }
                            names.push_back(name);
                        }
                        facts.push_back("registered " + Join(names, ", "));
                    }
                    if (m_unregistered)
                        facts.push_back(std::to_string(m_unregistered) + (m_unregistered == 1 ? " symbol removed" : " symbols removed"));
                    summary += ": " + Join(facts, " · ");
                }
                std::string details = summary + "\n";
                details += m_host.Process ? m_host.Process->Name() + (m_is64 ? " (64-bit)" : " (32-bit)") : std::string("no process open");
                if (m_options.CheckOnly)
                    details += m_options.ScanInCheck ? "; a check allocates and writes nothing, but it scans" : "; a check allocates, scans and writes nothing";
                details += "\n\n" + FormatLog(m_result.Log);
                if (m_droppedLog)
                    details += "\n" + std::to_string(m_droppedLog) + " more steps were left out";
                RuntimeLog::WriteDetailed(level, "Auto Assembler", __FILE__, "RunAutoAssembler", __LINE__, summary + " · " + std::to_string(m_result.Log.size()) + " steps", details);
            }

            void LogLayout()
            {
                std::size_t output = 0;
                for (std::size_t i = 0; i < m_statements.size(); ++i)
                {
                    const Statement& statement = m_statements[i];
                    if (statement.Type == Statement::Kind::Place)
                    {
                        const std::string key = Lower(statement.Text);
                        if (const auto placed = m_placed.find(key); placed != m_placed.end() && m_labels.count(key))
                            Step(statement.Line, statement.Text + ":", "label at " + Where(placed->second));
                        else if (m_placeAt.size() > i && m_placeAt[i])
                            Step(statement.Line, statement.Text + ":", "code goes to " + Where(*m_placeAt[i]));
                        continue;
                    }
                    const AaWrite* written = output < m_output.size() ? &m_output[output] : nullptr;
                    ++output;
                    if (!IsReassemble(statement.Text) || !written)
                        continue;
                    const std::string source = Trim(statement.Text.substr(kReassembleMarker.size()));
                    const auto found = m_reassembled.find(&statement);
                    std::string text;
                    if (found != m_reassembled.end())
                        text = found->second.second + " from " + Where(found->second.first);
                    else if (m_options.CheckOnly)
                        text = "nop (stands in during a check)";
                    text += " → " + Where(written->Address) + " as " + BytesPreview(written->Bytes);
                    Step(statement.Line, "reassemble(" + source + ")", text);
                }
            }

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

            void Abort()
            {
                ReleaseAllocations();
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
                        if (!line.Generated)
                            Step(line.Line, "define(" + command->Arguments[0] + ")", Short(value) + (same != defines.end() ? " (was " + Short(same->second) + ")" : ""));
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
                    if (m_log)
                        Step(startLine, "{$lua}", std::string(Trim(output).empty() ? "ran, returned no script text" : "ran, returned " + Short(output)) +
                                                      (m_options.CheckOnly ? " (syntaxcheck was true)" : ""));
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
                    if (m_log)
                    {
                        const auto arguments = ParseCommand(line.Text);
                        const std::string first = arguments && !arguments->Arguments.empty() ? arguments->Arguments.front() : std::string();
                        const bool more = arguments && arguments->Arguments.size() > 1;
                        Step(line.Line, name + "(" + first + (more ? ", ...)" : ")"), Trim(replacement).empty() ? "returned no script text" : "returned " + Short(replacement, 160));
                    }
                    std::string stripped;
                    if (!ProcessText(replacement, line.Line, false, "the output of " + name, stripped))
                        return false;
                    std::vector<SourceLine> produced;
                    for (const std::string& text : SplitLines(stripped))
                    {
                        const std::string trimmed = Trim(text);
                        if (!trimmed.empty())
                            produced.push_back({ trimmed, line.Line, true });
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
                    Step(line.Line, "include(" + Unquote(command->Arguments[0]) + ")", "read " + WideToUtf8(path.wstring()) + ", " + std::to_string(included.size()) +
                                                                                   (included.size() == 1 ? " line" : " lines"));
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
                        m_deallocs.push_back({ allocation, line.Line });
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
                        m_unregister.push_back({ symbol, line.Line });
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
                if (name == "resolvestatic")
                {
                    if (!NeedArguments(command, line, 2, "resolvestatic(name, address[, options])"))
                        return false;
                    const std::string& symbol = command.Arguments[0];
                    const bool word = !symbol.empty() && (std::isalpha(static_cast<unsigned char>(symbol.front())) || symbol.front() == '_') &&
                                      std::all_of(symbol.begin(), symbol.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
                    if (!word)
                        return Fail(ErrorCode::AaBadArguments, line.Line, "resolvestatic(name, address): \"" + symbol + "\" is no name (letters, digits and _, not starting with a digit)", line.Text);
                    // ∑ A name that reads as hex (BEEF, Add) would be taken as the number in every
                    //   line that uses it, so the static would silently never be used.
                    if (std::all_of(symbol.begin(), symbol.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
                        return Fail(ErrorCode::AaBadArguments, line.Line, "resolvestatic(name, address): \"" + symbol + "\" reads as a hex number; pick a name with other letters", line.Text);
                    if (Trim(command.Arguments[1]).empty())
                        return Fail(ErrorCode::AaBadArguments, line.Line, "resolvestatic(name, address): the address is empty", line.Text);
                    Disasm::StaticOptions options;
                    for (std::size_t i = 2; i < command.Arguments.size(); ++i)
                    {
                        std::string problem;
                        if (!Disasm::ParseStaticOption(command.Arguments[i], options, problem))
                            return Fail(ErrorCode::AaBadArguments, line.Line, "resolvestatic: " + problem, line.Text);
                    }
                    if (!DeclareName(symbol, line.Line, line.Text))
                        return false;
                    m_statics.push_back({ symbol, command.Arguments[1], options, line.Line, line.Text });
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
                    statement.Text = std::string(kReassembleMarker) + command.Arguments[0];
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
                    Step(line.Line, "luacall(" + Short(code, 30) + ")", m_options.CheckOnly ? "skipped in a check" : "ran");
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
                {
                    for (const auto& [path, line] : m_libraries)
                        Step(line, "loadlibrary(" + path + ")", "skipped in a check");
                    return true;
                }
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
                    Step(line, "loadlibrary(" + path + ")", "loaded " + WideToUtf8(file.wstring()));
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
                    const std::string scanText = scan.Kind + "(" + scan.Arguments[0] + ")";
                    if (m_options.CheckOnly && !m_options.ScanInCheck)
                    {
                        m_values[Lower(scan.Arguments[0])] = 0;
                        Step(scan.Line, scanText, "not scanned in a check, 0 until the script runs");
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
                    // ∑ A scan that a resolvestatic reads from looks for a second match too: on the
                    //   first of two the static may be another one, and nothing would show it.
                    const bool unique = std::any_of(m_statics.begin(), m_statics.end(), [&](const StaticCommand& resolve) {
                        return !resolve.Options.FirstHit && UsesWord(resolve.Address, scan.Arguments[0]);
                    });
                    const auto hits = Scan::FindPattern(*m_host.Process, *pattern, start, stop, false, unique ? 2 : 1);
                    if (hits.size() > 1)
                        m_secondHits[Lower(scan.Arguments[0])] = hits[1];
                    if (hits.empty())
                    {
                        if (m_log)
                            Step(scan.Line, scanText, "no " + Short(patternText, 64) +
                                                          (scan.Kind == "aobscanmodule" ? " in " + Unquote(scan.Arguments[1]) + " (" + Hex(start) + "-" + Hex(stop) + ")"
                                                           : scan.Kind == "aobscanregion" ? " between " + Hex(start) + " and " + Hex(stop)
                                                                                          : " in the whole process"));
                        m_result.Error = Status::Fail(ErrorCode::AaPatternNotFound, scan.Arguments[0]);
                        m_result.Line = scan.Line;
                        return false;
                    }
                    m_values[Lower(scan.Arguments[0])] = hits.front();
                    m_result.Notes.push_back(scan.Arguments[0] + " = " + Hex(hits.front()));
                    if (m_log)
                        Step(scan.Line, scanText, "found at " + Where(hits.front()) + (hits.size() > 1 ? ", and at " + Where(hits[1]) : ""));
                }
                return true;
            }

            static bool UsesWord(const std::string& text, const std::string& word)
            {
                const std::string lower = Lower(text);
                return ReplaceWord(lower, Lower(word), "\x02") != lower;
            }

            // ∑ After the scans, so the address may be an aobscan of the same script, and before
            //   alloc, so an alloc may be near the static. In script order: a resolvestatic may
            //   use the name an earlier one resolved. A check reads nothing (like CE's aobscan
            //   there): the name is 0 until the script runs.
            bool ResolveStatics()
            {
                for (const StaticCommand& resolve : m_statics)
                {
                    const std::string key = Lower(resolve.Name);
                    const std::string command = "resolvestatic(" + resolve.Name + ")";
                    if (m_options.CheckOnly)
                    {
                        m_values[key] = 0;
                        Step(resolve.Line, command, "not resolved in a check, 0 until the script runs");
                        continue;
                    }
                    if (!m_host.Process || !m_host.Symbols)
                        return Fail(ErrorCode::NoProcess, resolve.Line, "");
                    for (const ScanCommand& scan : m_scans)
                    {
                        const auto second = m_secondHits.find(Lower(scan.Arguments[0]));
                        if (second == m_secondHits.end() || resolve.Options.FirstHit || !UsesWord(resolve.Address, scan.Arguments[0]))
                            continue;
                        const auto first = m_values.find(Lower(scan.Arguments[0]));
                        m_result.Error = Status::Fail(ErrorCode::AaStaticNotUnique, scan.Arguments[0] + " at " + Where(first == m_values.end() ? 0 : first->second) +
                                                                                         " and " + Where(second->second));
                        m_result.Line = resolve.Line;
                        return false;
                    }
                    const auto address = m_host.Symbols->Evaluate(resolve.Address, Extra());
                    if (!address.Ok())
                    {
                        m_result.Error = address.Error;
                        m_result.Line = resolve.Line;
                        return false;
                    }
                    const auto ref = Disasm::ResolveStatic(*m_host.Process, *address.Value, resolve.Options);
                    if (!ref)
                    {
                        if (m_log)
                            Step(resolve.Line, command, "failed at " + Where(*address.Value));
                        m_result.Error = ref.Error();
                        m_result.Line = resolve.Line;
                        return false;
                    }
                    m_values[key] = ref->Value;
                    m_result.Notes.push_back(resolve.Name + " = " + Hex(ref->Value));
                    if (m_log)
                    {
                        std::string text = ref->Text + " at " + Where(ref->Instruction) + " → " +
                                           (ref->Kind == Disasm::StaticKind::Branch ? "branch target " : ref->Kind == Disasm::StaticKind::Immediate ? "number " : ref->Relative ? "rip-relative " : "absolute ") +
                                           Where(ref->Address);
                        if (resolve.Options.Output == Disasm::StaticOutput::Pointer)
                            text += ", pointer there " + Where(ref->Value);
                        for (const std::string& note : ref->Notes)
                            text += "; " + note;
                        Step(resolve.Line, command, text);
                    }
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
                            if (m_log)
                                Step(alloc.Line, "globalalloc(" + alloc.Name + ")", "reuses " + Where(*existing) + ", registered by an earlier run");
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
                        if (m_log)
                        {
                            std::string text = Hex(address) + ", " + ByteCount(alloc->Size);
                            if (m_options.CheckOnly)
                                text += " (a stand-in address for the check)";
                            if (members.size() > 1)
                                text += ", shares the block at " + Hex(block) + " with " + std::to_string(members.size() - 1) + (members.size() == 2 ? " other" : " others");
                            if (group.Near && !m_options.CheckOnly)
                            {
                                const std::uint64_t distance = address > *group.Near ? address - *group.Near : *group.Near - address;
                                text += ", " + Hex(distance) + " from " + Where(*group.Near);
                                if (m_is64)
                                    text += distance < 0x7FFF0000ull ? " (a 5-byte jump reaches)" : " (beyond 2 GB: 14-byte jumps)";
                            }
                            Step(alloc->Line, std::string(alloc->Global ? "globalalloc(" : "alloc(") + alloc->Name + ")", text);
                        }
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
                        bool copied = address.Ok() && !fake(*address.Value);
                        if (copied && !m_host.Process->ReadExact(*address.Value, statement.Data.data(), statement.Data.size()))
                        {
                            std::fill(statement.Data.begin(), statement.Data.end(), std::uint8_t(0));
                            copied = false;
                        }
                        Step(read.Line, "readmem(" + read.Address + ", " + std::to_string(statement.Data.size()) + ")",
                            copied ? BytesPreview(statement.Data) + " (copied for the check)" : "zeros in the check (nothing readable there yet)");
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
                    if (m_log)
                        Step(read.Line, "readmem(" + read.Address + ", " + std::to_string(statement.Data.size()) + ")", BytesPreview(statement.Data) + " from " + Where(*address.Value));
                }
                return true;
            }

            void ReleaseAllocations()
            {
                if (!m_host.Process || m_options.CheckOnly)
                    return;
                for (std::uint64_t block : m_allocatedBlocks)
                {
                    m_host.Process->Free(block);
                    Step(0, "", "freed " + Hex(block) + " again, since the run failed");
                }
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
                    m_outputData.clear();
                    m_unplacedUses.clear();
                    m_placeAt.assign(m_statements.size(), std::nullopt);
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
                            m_placeAt[i] = *address;
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
                            m_outputData.push_back(false);
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
                                return Fail(CodeFor(assembled.Error), statement.Line, assembled.Message, IsReassemble(statement.Text) ? statement.Text.substr(1) : statement.Text);
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
                        m_outputData.push_back(IsData(statement) && !ReadIndex(i));
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

            // A readmem line is a Bytes statement too, but what it copies is code (the original instructions of a hook).
            bool ReadIndex(std::size_t index) const
            {
                return std::any_of(m_reads.begin(), m_reads.end(), [index](const PendingRead& read) { return read.Index == index; });
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

            // ---------------------------------------------------------------- stack balance

            struct StackPush
            {
                int Line = 0;
                std::string Text;
                std::int64_t Size = 0;
            };

            struct StackPath
            {
                std::uint64_t At = 0;
                std::int64_t Delta = 0;
                std::vector<StackPush> Pushes;
                std::uint64_t Origin = 0;   // the patch the hook was entered from
                bool Called = false;        // entered by a call: its ret goes back to the patch
            };

            // How an instruction moves the stack pointer: negative for what it puts
            // on the stack. Unknown (nullopt) for anything that sets it another way.
            std::optional<std::int64_t> StackChange(const Disasm::Instruction& instruction) const
            {
                const std::int64_t word = m_is64 ? 8 : 4;
                const std::string sp = m_is64 ? "rsp" : "esp";
                const std::string mnemonic = Lower(instruction.Mnemonic);
                const std::string operands = Lower(instruction.Operands);
                const std::size_t comma = operands.find(',');
                const std::string destination = operands.substr(0, comma);
                const std::string source = comma == std::string::npos ? std::string() : operands.substr(comma + 1);
                if (mnemonic == "push")
                    return -(operands.starts_with("word") ? 2 : word);
                if (mnemonic == "pop")
                    return operands.starts_with("word") ? 2 : word;
                if (mnemonic == "pushad" || mnemonic == "pusha")
                    return mnemonic == "pushad" ? -32 : -16;
                if (mnemonic == "popad" || mnemonic == "popa")
                    return mnemonic == "popad" ? 32 : 16;
                if (mnemonic.starts_with("pushf"))
                    return -word;
                if (mnemonic.starts_with("popf"))
                    return word;
                if ((mnemonic == "sub" || mnemonic == "add") && destination == sp)
                {
                    const auto amount = StackOperand(source);
                    if (!amount)
                        return std::nullopt;
                    return mnemonic == "sub" ? -*amount : *amount;
                }
                if (mnemonic == "lea" && destination == sp && source.starts_with("[" + sp) && !source.empty() && source.back() == ']')
                {
                    const std::string inside = source.substr(sp.size() + 1, source.size() - sp.size() - 2);
                    return inside.empty() ? std::optional<std::int64_t>(0) : StackOperand(inside);
                }
                if (destination == sp || mnemonic == "leave" || mnemonic == "enter" || (mnemonic == "xchg" && source == sp))
                    return std::nullopt;
                return 0;
            }

            // ∑ The bytes a patch replaced belong to the game, and the cave runs
            //   them again: a patch over push ebp makes the cave leave 4 bytes on the
            //   stack, rightly. So the cave's change is compared with what the
            //   original instructions did, from the patch up to where the cave jumps
            //   back (or up to their ret). Without the original bytes (no process, an
            //   address a check could not resolve) there is no verdict.
            std::optional<std::int64_t> OriginalChange(std::uint64_t origin, std::optional<std::uint64_t> until, bool toRet) const
            {
                constexpr std::size_t kReach = 64;
                if (!m_host.Process || origin == 0)
                    return std::nullopt;
                std::uint8_t bytes[kReach] = {};
                const std::size_t got = m_host.Process->Read(origin, bytes, sizeof(bytes));
                if (got == 0)
                    return std::nullopt;
                const Disasm::Disassembler disassembler(m_is64);
                std::int64_t change = 0;
                std::size_t offset = 0;
                while (offset < got)
                {
                    const std::uint64_t at = origin + offset;
                    if (!toRet && until && at == *until)
                        return change;
                    const Disasm::Instruction instruction = disassembler.Decode(at, bytes + offset, got - offset);
                    if (!instruction.Valid || instruction.Length == 0)
                        return std::nullopt;
                    if (toRet && instruction.Kind == Disasm::Flow::Return)
                        return change;
                    if (instruction.Kind == Disasm::Flow::Jump || instruction.Kind == Disasm::Flow::Interrupt)
                        return std::nullopt;
                    const auto step = StackChange(instruction);
                    if (!step)
                        return std::nullopt;
                    change += *step;
                    offset += instruction.Length;
                    if (!toRet && until && at + instruction.Length > *until)
                        return std::nullopt;
                }
                return std::nullopt;
            }

            const AaWrite* WriteAt(const std::vector<const AaWrite*>& writes, std::uint64_t address) const
            {
                auto it = std::upper_bound(writes.begin(), writes.end(), address, [](std::uint64_t at, const AaWrite* write) { return at < write->Address; });
                if (it == writes.begin())
                    return nullptr;
                --it;
                return address < (*it)->Address + (*it)->Bytes.size() ? *it : nullptr;
            }

            bool InAllocation(std::uint64_t address) const
            {
                const auto holds = [address](const AaAllocation& allocation) { return address >= allocation.Address && address < allocation.Address + std::max<std::uint64_t>(allocation.Size, 1); };
                return std::any_of(m_newAllocations.begin(), m_newAllocations.end(), holds) || std::any_of(m_memory.Allocations.begin(), m_memory.Allocations.end(), holds);
            }

            // The target of a jump or call, also of the far forms the assembler writes
            // (jmp [rip+0] / call [rip+2]; jmp +8 with the address after them).
            std::optional<std::uint64_t> BranchTarget(const Disasm::Instruction& instruction, const AaWrite& write) const
            {
                if (instruction.Target)
                    return instruction.Target;
                const std::size_t offset = static_cast<std::size_t>(instruction.Address - write.Address);
                const std::vector<std::uint8_t>& bytes = write.Bytes;
                const auto qword = [&](std::size_t at) -> std::optional<std::uint64_t>
                {
                    if (at + 8 > bytes.size())
                        return std::nullopt;
                    std::uint64_t value = 0;
                    std::memcpy(&value, bytes.data() + at, 8);
                    return value;
                };
                if (offset + 6 <= bytes.size() && bytes[offset] == 0xFF && bytes[offset + 1] == 0x25 && bytes[offset + 2] == 0 && bytes[offset + 3] == 0 && bytes[offset + 4] == 0 && bytes[offset + 5] == 0)
                    return m_is64 ? qword(offset + 6) : std::nullopt;
                if (m_is64 && offset + 8 <= bytes.size() && bytes[offset] == 0xFF && bytes[offset + 1] == 0x15 && bytes[offset + 2] == 2 && bytes[offset + 3] == 0 && bytes[offset + 4] == 0 &&
                    bytes[offset + 5] == 0 && bytes[offset + 6] == 0xEB && bytes[offset + 7] == 8)
                    return qword(offset + 8);
                return std::nullopt;
            }

            static std::optional<std::int64_t> StackOperand(std::string_view text)
            {
                bool negative = false;
                if (!text.empty() && (text.front() == '-' || text.front() == '+'))
                {
                    negative = text.front() == '-';
                    text.remove_prefix(1);
                }
                if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
                    text.remove_prefix(2);
                if (!text.empty() && (text.back() == 'h' || text.back() == 'H'))
                    text.remove_suffix(1);
                if (text.empty() || text.size() > 16)
                    return std::nullopt;
                std::uint64_t value = 0;
                for (const char c : text)
                {
                    const int digit = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (digit < 0)
                        return std::nullopt;
                    value = (value << 4) | static_cast<std::uint64_t>(digit);
                }
                if (value > 0x7FFFFFFFull)
                    return std::nullopt;
                return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
            }

            // ∑ Each hook is followed from the jump (or call) of its patch into the
            //   cave until it leaves the code this run wrote, or returns: there the
            //   stack has to be where it was on the way in, or the game's own code
            //   pops the wrong values (a push eax without its pop turns the game's
            //   next pop esi into garbage and crashes it later, far from the script).
            //   Calls count as balanced; an instruction that sets the stack pointer
            //   some other way (mov, and, leave) ends that path without a verdict.
            void CheckStackBalance()
            {
                std::vector<const AaWrite*> writes;
                for (const AaWrite& write : m_output)
                {
                    if (!write.Bytes.empty())
                        writes.push_back(&write);
                }
                if (writes.empty())
                    return;
                std::sort(writes.begin(), writes.end(), [](const AaWrite* a, const AaWrite* b) { return a->Address < b->Address; });
                const Disasm::Disassembler disassembler(m_is64);
                const auto decode = [&](std::uint64_t at, const AaWrite*& write) -> std::optional<Disasm::Instruction>
                {
                    write = WriteAt(writes, at);
                    if (!write)
                        return std::nullopt;
                    const std::size_t offset = static_cast<std::size_t>(at - write->Address);
                    Disasm::Instruction instruction = disassembler.Decode(at, write->Bytes.data() + offset, write->Bytes.size() - offset);
                    if (!instruction.Valid)
                        return std::nullopt;
                    return instruction;
                };

                std::vector<StackPath> todo;
                for (const AaWrite* write : writes)
                {
                    if (InAllocation(write->Address))
                        continue;
                    const AaWrite* holder = nullptr;
                    const auto first = decode(write->Address, holder);
                    if (!first || (first->Kind != Disasm::Flow::Jump && first->Kind != Disasm::Flow::Call))
                        continue;
                    const auto target = BranchTarget(*first, *holder);
                    if (target && WriteAt(writes, *target) && InAllocation(*target))
                        todo.push_back({ *target, 0, {}, write->Address, first->Kind == Disasm::Flow::Call });
                }

                std::set<std::pair<std::uint64_t, std::int64_t>> seen;
                std::set<std::pair<int, std::int64_t>> warned;
                const auto report = [&](const StackPath& path, int line, const char* how, std::optional<std::uint64_t> target, bool ret)
                {
                    // A call into the cave returns to the patch: its ret must find the stack as the call left it.
                    const std::optional<std::int64_t> expected = path.Called && ret ? std::optional<std::int64_t>(0) : OriginalChange(path.Origin, target, ret);
                    if (!expected)
                        return;
                    const std::int64_t delta = path.Delta - *expected;
                    if (delta == 0 || !warned.insert({ line, delta }).second || m_result.Warnings.size() >= 20)
                        return;
                    const std::int64_t off = delta < 0 ? -delta : delta;
                    AaWarning warning;
                    warning.Line = line;
                    std::string text = Format("the stack is %lld bytes off %s in line %d", static_cast<long long>(off), how, line);
                    if (delta < 0 && !path.Pushes.empty())
                    {
                        const StackPush& push = path.Pushes.back();
                        warning.Line = push.Line;
                        text += Format(": %s in line %d has no matching pop", push.Text.c_str(), push.Line);
                        if (path.Pushes.size() > 1)
                            text += Format(" (%zu pushes are left open)", path.Pushes.size());
                    }
                    else if (delta > 0)
                    {
                        text += ": the code takes more off the stack than it put on";
                    }
                    warning.Text = text + ". The game's code after it will read the wrong values and likely crash.";
                    m_result.Warnings.push_back(std::move(warning));
                    if (m_log)
                        Step(line, "stack check", m_result.Warnings.back().Text);
                };

                int steps = 0;
                while (!todo.empty() && steps < 20000)
                {
                    StackPath path = std::move(todo.back());
                    todo.pop_back();
                    while (++steps < 20000)
                    {
                        if (!seen.insert({ path.At, path.Delta }).second)
                            break;
                        const AaWrite* write = nullptr;
                        const auto instruction = decode(path.At, write);
                        if (!instruction)
                            break;
                        const int line = write->Line;
                        const auto change = StackChange(*instruction);
                        if (!change)
                            break;
                        if (*change < 0)
                        {
                            path.Delta += *change;
                            path.Pushes.push_back({ line, instruction->Text(), -*change });
                        }
                        else if (*change > 0)
                        {
                            path.Delta += *change;
                            if (!path.Pushes.empty())
                                path.Pushes.pop_back();
                        }
                        if (instruction->Kind == Disasm::Flow::Return)
                        {
                            report(path, line, "at the ret", std::nullopt, true);
                            break;
                        }
                        if (instruction->Kind == Disasm::Flow::Interrupt)
                            break;
                        if (instruction->Kind == Disasm::Flow::Jump)
                        {
                            const auto target = BranchTarget(*instruction, *write);
                            if (target && WriteAt(writes, *target) && InAllocation(*target))
                            {
                                path.At = *target;
                                continue;
                            }
                            report(path, line, "at the jump back", target, false);
                            break;
                        }
                        if (instruction->Kind == Disasm::Flow::ConditionalJump)
                        {
                            const auto target = BranchTarget(*instruction, *write);
                            if (target && WriteAt(writes, *target) && InAllocation(*target))
                                todo.push_back({ *target, path.Delta, path.Pushes, path.Origin, path.Called });
                            else
                                report(path, line, "at the jump out", target, false);
                        }
                        path.At = instruction->Next();
                        if (!WriteAt(writes, path.At))
                            break;
                    }
                }
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
                if (IsReassemble(text))
                {
                    const std::string target = Trim(text.substr(kReassembleMarker.size()));
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
                        if (checked.Ok && m_log)
                            m_reassembled[&statement] = { *source.Value, instruction.Text() };
                        return checked.Ok ? checked : checkPlaceholder();
                    }
                    text = instruction.Text();
                    if (m_log)
                        m_reassembled[&statement] = { *source.Value, text };
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
                        Step(check.Line, "assert(" + check.Address + ")", "skipped in a check");
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
                    const bool readable = m_host.Process->ReadExact(*address.Value, actual.data(), actual.size());
                    if (!readable || !pattern->Matches(actual.data()))
                    {
                        if (m_log)
                            Step(check.Line, "assert(" + check.Address + ")", "expected " + Short(check.Bytes, 64) + " at " + Where(*address.Value) +
                                                                              (readable ? ", found " + HexBytes(actual) : ", which cannot be read"));
                        m_result.Error = Status::Fail(ErrorCode::AaAssertFailed, check.Address + " (found " + HexBytes(actual) + ")");
                        m_result.Line = check.Line;
                        return false;
                    }
                    if (m_log)
                        Step(check.Line, "assert(" + check.Address + ")", "matches " + HexBytes(actual) + " at " + Where(*address.Value));
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
                    else if (m_log)
                        Step(access.Line, "fullaccess(" + access.Address + ", " + access.Size + ")", Where(*address.Value) + " is now readable, writable and executable");
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
                        else
                            Step(done->Line, "write", "put back " + ByteCount(done->Bytes.size()) + " at " + Hex(done->Address) + ", since a later write failed");
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
                    if (m_log)
                        Step(write.Line, "write", ByteCount(write.Bytes.size()) + " at " + Where(write.Address) + ": " + BytesPreview(write.Bytes) + "  (was " +
                                                      BytesPreview(undo.back().Bytes) + ")");
                }
                m_result.Writes = std::move(merged);
                for (std::size_t i = 0; i < m_output.size() && i < m_outputData.size(); ++i)
                {
                    const AaWrite& write = m_output[i];
                    if (m_outputData[i] && !write.Bytes.empty() && InAllocation(write.Address))
                        m_result.DataRanges.emplace_back(write.Address, write.Address + write.Bytes.size());
                }
                for (const auto& [key, address] : m_placed)
                {
                    const auto name = m_labels.find(key);
                    m_result.Labels[name != m_labels.end() ? name->second : key] = address;
                }
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
                std::vector<std::pair<std::string, int>> deallocs;
                for (const auto& [name, line] : m_deallocs)
                {
                    if (name == "*")
                    {
                        if (m_memory.Allocations.empty())
                            Step(line, "dealloc(*)", "nothing that an earlier run allocated");
                        for (const AaAllocation& allocation : m_memory.Allocations)
                            deallocs.push_back({ allocation.Name, line });
                    }
                    else
                    {
                        deallocs.push_back({ name, line });
                    }
                }
                for (const auto& [name, line] : deallocs)
                {
                    auto found = std::find_if(m_memory.Allocations.begin(), m_memory.Allocations.end(), [&](const AaAllocation& allocation) { return EqualsNoCase(allocation.Name, name); });
                    if (found == m_memory.Allocations.end())
                    {
                        Step(line, "dealloc(" + name + ")", "nothing allocated under this name; nothing freed");
                        continue;
                    }
                    if (found->Global)
                    {
                        Step(line, "dealloc(" + name + ")", "globalalloc memory at " + Hex(found->Address) + " stays, as it is shared");
                        m_memory.Allocations.erase(found);
                        continue;
                    }
                    const std::uint64_t block = found->Block;
                    const std::uint64_t address = found->Address;
                    m_memory.Allocations.erase(found);
                    const bool stillUsed = std::any_of(m_memory.Allocations.begin(), m_memory.Allocations.end(), [&](const AaAllocation& allocation) { return allocation.Block == block; });
                    if (!stillUsed && m_host.Process)
                    {
                        const Status status = m_host.Process->Free(block);
                        if (!status)
                            m_result.Notes.push_back(status.Text);
                        if (status)
                            ++m_freedBlocks;
                        Step(line, "dealloc(" + name + ")", status ? "freed the block at " + Hex(block) : "could not free the block at " + Hex(block) + ": " + status.Text);
                    }
                    else if (stillUsed)
                        Step(line, "dealloc(" + name + ")", "released " + Hex(address) + "; the block at " + Hex(block) + " stays for the others in it");
                }
                for (const AaAllocation& allocation : m_newAllocations)
                    m_memory.Allocations.push_back(allocation);
                m_allocatedBlocks.clear();

                for (const auto& [key, value] : m_values)
                    m_memory.Values[key] = value;
                for (const auto& [key, value] : m_placed)
                    m_memory.Values[key] = value;

                std::vector<std::pair<std::string, int>> unregister;
                for (const auto& [name, line] : m_unregister)
                {
                    if (name == "*")
                    {
                        if (m_memory.Registered.empty())
                            Step(line, "unregistersymbol(*)", "nothing that an earlier run registered");
                        for (const std::string& known : m_memory.Registered)
                            unregister.push_back({ known, line });
                    }
                    else
                        unregister.push_back({ name, line });
                }
                for (const auto& [name, line] : unregister)
                {
                    const bool removed = m_host.Symbols && m_host.Symbols->Unregister(name);
                    if (removed)
                        ++m_unregistered;
                    Step(line, "unregistersymbol(" + name + ")", removed ? "removed" : "was not registered");
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
                        Step(line, "registersymbol(" + name + ")", "has no address, nothing registered");
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
                    if (m_log)
                        Step(line, "registersymbol(" + name + ")", Hex(*value) + (size != m_sizes.end() ? ", " + ByteCount(size->second) + " of data" : ""));
                    if (std::find_if(m_memory.Registered.begin(), m_memory.Registered.end(), [&](const std::string& known) { return EqualsNoCase(known, name); }) ==
                        m_memory.Registered.end())
                        m_memory.Registered.push_back(name);
                }
                for (const ThreadCommand& thread : m_threads)
                {
                    const auto address = FinalValue(thread.Address);
                    const std::string command = std::string(thread.Mode.empty() ? "createthread(" : "createthreadandwait(") + thread.Address + ")";
                    if (!address)
                    {
                        m_result.Notes.push_back("createthread: " + thread.Address + " has no address");
                        Step(thread.Line, command, "has no address, no thread started");
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
                    if (m_log)
                        Step(thread.Line, command, status ? (thread.Mode.empty() ? "started a thread at " : "ran a thread to its end at ") + Where(*address)
                                                          : "could not start a thread at " + Hex(*address) + ": " + status.Text);
                }

                for (const PendingAlloc& alloc : m_allocs)
                {
                    if (alloc.Global && m_host.Symbols && !m_host.Symbols->FindRegistered(alloc.Name))
                    {
                        if (const auto value = KnownName(alloc.Name))
                        {
                            m_host.Symbols->Register(alloc.Name, *value, "");
                            Step(alloc.Line, "globalalloc(" + alloc.Name + ")", "registered at " + Hex(*value));
                        }
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

            struct StaticCommand
            {
                std::string Name;
                std::string Address;
                Disasm::StaticOptions Options;
                int Line = 0;
                std::string Text;
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
            std::vector<std::pair<std::string, int>> m_deallocs;
            std::map<std::string, std::string> m_labels;
            std::set<std::string> m_declared;
            std::set<std::string> m_potential;
            bool m_strict = false;
            std::vector<std::pair<std::string, int>> m_register;
            std::map<std::string, std::string> m_defines;
            std::vector<std::pair<std::string, int>> m_unregister;
            std::vector<ScanCommand> m_scans;
            std::vector<AssertCommand> m_asserts;
            std::vector<StaticCommand> m_statics;
            std::map<std::string, std::uint64_t> m_secondHits;  // a second match of a scan a resolvestatic reads from
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
            std::vector<bool> m_outputData;  // per m_output entry: a db/dw/dd/dq line

            bool m_log = false;
            std::size_t m_droppedLog = 0;
            std::size_t m_freedBlocks = 0;
            std::size_t m_unregistered = 0;
            std::vector<std::optional<std::uint64_t>> m_placeAt;     // where each address line of the last layout pass pointed
            std::map<const Statement*, std::pair<std::uint64_t, std::string>> m_reassembled;  // where each reassemble() read and what, for the log
        };
    }

    bool WantsLog(std::string_view script)
    {
        return ScanDirectives(Lower(script)).Log;
    }

    std::string FormatLog(const std::vector<AaLogLine>& steps)
    {
        constexpr std::size_t kCommandWidth = 36;
        std::size_t lineWidth = 4;
        std::size_t commandWidth = 7;
        for (const AaLogLine& step : steps)
        {
            lineWidth = std::max(lineWidth, std::to_string(step.Line).size());
            commandWidth = std::max(commandWidth, std::min(step.Command.size(), kCommandWidth));
        }
        const auto row = [&](std::string line, std::string command, const std::string& text) {
            if (command.size() > kCommandWidth)
                command = command.substr(0, kCommandWidth - 3) + "...";
            std::string out(lineWidth - std::min(lineWidth, line.size()), ' ');
            out += line + "  " + command + std::string(commandWidth - command.size(), ' ') + "  " + text;
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out + "\n";
        };
        std::string table = row("line", "command", "result");
        for (const AaLogLine& step : steps)
            table += row(step.Line > 0 ? std::to_string(step.Line) : std::string(), step.Command, step.Text);
        if (!table.empty())
            table.pop_back();
        return table;
    }

    AaResult RunAutoAssembler(std::string_view script, AaHost& host, AaMemory& memory, const AaOptions& options)
    {
        Run run(host, memory, options);
        AaResult result;
        // ∑ A slip in the script's handling (a fuzzed table found one) must fail this
        //   script, not end the program; what was written before the throw stays
        //   (the same as a crash of the program would leave it).
        try
        {
            result = run.Execute(script);
        }
        catch (const std::exception& error)
        {
            result = AaResult{};
            result.Error = Status::Fail(ErrorCode::AaBadArguments, std::string("The script could not be processed: ") + error.what());
        }
        if (!result.Ok() && !result.Logged)
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
