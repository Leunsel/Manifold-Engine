#include "Text/Highlight.h"

#include "Core/Text.h"

#include <algorithm>
#include <string>

namespace Manifold::Text
{
    namespace
    {
        constexpr std::size_t npos = std::string_view::npos;

        class Spans
        {
        public:
            explicit Spans(std::vector<TokenSpan>& out, std::size_t offset) : m_out(out), m_offset(offset) {}

            void Add(std::size_t start, std::size_t end, Token kind)
            {
                if (kind == Token::Text || end <= start)
                    return;
                start += m_offset;
                end += m_offset;
                if (!m_out.empty() && m_out.back().End == start && m_out.back().Kind == kind)
                {
                    m_out.back().End = end;
                    return;
                }
                m_out.push_back({ start, end, kind });
            }

        private:
            std::vector<TokenSpan>& m_out;
            std::size_t m_offset;
        };

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool IsAlpha(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool IsIdentifier(char c)
        {
            return IsAlpha(c) || IsDigit(c);
        }

        bool IsAaWord(char c)
        {
            return IsIdentifier(c) || c == '.' || c == '$' || c == '#';
        }

        class WordSet
        {
        public:
            template <std::size_t N>
            explicit WordSet(const std::string_view (&words)[N]) : m_words(words, words + N)
            {
                std::sort(m_words.begin(), m_words.end());
            }

            bool Has(std::string_view word) const
            {
                return std::binary_search(m_words.begin(), m_words.end(), word);
            }

        private:
            std::vector<std::string_view> m_words;
        };

        constexpr std::string_view kLuaKeywords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
            "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };

        constexpr std::string_view kAaCommands[] = { "alloc", "aobscan", "aobscanmodule", "aobscanregion", "assert", "byte", "createthread",
            "createthreadandwait", "db", "dd", "dealloc", "define", "dq", "dw", "dword", "far", "fullaccess", "globalalloc", "include", "label", "loadlibrary", "luacall",
            "near", "ptr", "qword", "readmem", "reassemble", "registersymbol", "short", "tword", "unregistersymbol", "word", "xmmword", "ymmword" };

        constexpr std::string_view kRegisters[] = { "ah", "al", "ax", "bh", "bl", "bp", "bpl", "bx", "ch", "cl", "cs", "cx", "dh", "di", "dil", "dl",
            "ds", "dx", "eax", "ebp", "ebx", "ecx", "edi", "edx", "eip", "es", "esi", "esp", "fs", "gs", "r10", "r10b", "r10d", "r10w", "r11", "r11b", "r11d", "r11w",
            "r12", "r12b", "r12d", "r12w", "r13", "r13b", "r13d", "r13w", "r14", "r14b", "r14d", "r14w", "r15", "r15b", "r15d", "r15w", "r8", "r8b", "r8d", "r8w",
            "r9", "r9b", "r9d", "r9w", "rax", "rbp", "rbx", "rcx", "rdi", "rdx", "rip", "rsi", "rsp", "si", "sil", "sp", "spl", "ss", "st0", "st1", "st2", "st3",
            "st4", "st5", "st6", "st7", "xmm0", "xmm1", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
            "xmm8", "xmm9", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7", "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15" };

        bool AaNumber(std::string_view word)
        {
            if (word.empty())
                return false;
            if (word[0] == '#' || word[0] == '$')
                word.remove_prefix(1);
            if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X'))
                word.remove_prefix(2);
            return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) { return IsHexDigit(c) || c == '.'; }) &&
                   (IsDigit(word[0]) || word.size() <= 8);
        }

        bool StartsNoCase(std::string_view line, std::size_t at, std::string_view word)
        {
            return line.size() - std::min(at, line.size()) >= word.size() && EqualsNoCase(line.substr(at, word.size()), word);
        }

        bool LongOpen(std::string_view line, std::size_t at, std::uint8_t& level, std::size_t& after)
        {
            if (at >= line.size() || line[at] != '[')
                return false;
            std::size_t i = at + 1;
            while (i < line.size() && line[i] == '=')
                ++i;
            if (i >= line.size() || line[i] != '[' || i - at - 1 > 255)
                return false;
            level = static_cast<std::uint8_t>(i - at - 1);
            after = i + 1;
            return true;
        }

        std::size_t LongClose(std::string_view line, std::size_t from, std::uint8_t level)
        {
            for (std::size_t at = line.find(']', from); at != npos; at = line.find(']', at + 1))
            {
                std::size_t i = at + 1;
                while (i < line.size() && line[i] == '=' && i - at - 1 < level)
                    ++i;
                if (i - at - 1 == level && i < line.size() && line[i] == ']')
                    return i + 1;
            }
            return npos;
        }

        std::size_t NumberEnd(std::string_view line, std::size_t i)
        {
            const bool hex = line[i] == '0' && i + 1 < line.size() && (line[i + 1] == 'x' || line[i + 1] == 'X');
            std::size_t end = hex ? i + 2 : i;
            while (end < line.size() && (IsIdentifier(line[end]) || line[end] == '.'))
            {
                const char c = line[end++];
                const bool exponent = hex ? (c == 'p' || c == 'P') : (c == 'e' || c == 'E');
                if (exponent && end < line.size() && (line[end] == '+' || line[end] == '-'))
                    ++end;
            }
            return end;
        }

        std::size_t LuaTokens(std::string_view line, std::size_t from, LineState& state, Spans& out, bool insideAa)
        {
            std::size_t i = from;
            while (i < line.size())
            {
                if (state.Kind == Block::LuaComment || state.Kind == Block::LuaString)
                {
                    const std::size_t close = LongClose(line, i, state.Level);
                    const std::size_t stop = close == npos ? line.size() : close;
                    out.Add(i, stop, state.Kind == Block::LuaComment ? Token::Comment : Token::String);
                    if (close != npos)
                        state = { Block::Lua, 0 };
                    i = stop;
                    continue;
                }
                const char c = line[i];
                if (c == '{' && StartsNoCase(line, i, "{$asm}"))
                {
                    out.Add(i, i + 6, Token::Section);
                    i += 6;
                    if (insideAa)
                    {
                        state = { Block::None, 0 };
                        return i;
                    }
                    continue;
                }
                if (c == '-' && i + 1 < line.size() && line[i + 1] == '-')
                {
                    std::uint8_t level = 0;
                    std::size_t after = 0;
                    if (LongOpen(line, i + 2, level, after))
                    {
                        state = { Block::LuaComment, level };
                        out.Add(i, after, Token::Comment);
                        i = after;
                        continue;
                    }
                    out.Add(i, line.size(), Token::Comment);
                    return line.size();
                }
                if (c == '[')
                {
                    std::uint8_t level = 0;
                    std::size_t after = 0;
                    if (LongOpen(line, i, level, after))
                    {
                        state = { Block::LuaString, level };
                        out.Add(i, after, Token::String);
                        i = after;
                        continue;
                    }
                }
                if (c == '"' || c == '\'')
                {
                    std::size_t end = i + 1;
                    while (end < line.size() && line[end] != c)
                        end += line[end] == '\\' ? 2 : 1;
                    end = std::min(end + 1, line.size());
                    out.Add(i, end, Token::String);
                    i = end;
                    continue;
                }
                if (IsDigit(c) || (c == '.' && i + 1 < line.size() && IsDigit(line[i + 1]) && (i == 0 || line[i - 1] != '.')))
                {
                    const std::size_t end = NumberEnd(line, i);
                    out.Add(i, end, Token::Number);
                    i = end;
                    continue;
                }
                if (IsAlpha(c))
                {
                    std::size_t end = i;
                    while (end < line.size() && IsIdentifier(line[end]))
                        ++end;
                    const std::string_view word = line.substr(i, end - i);
                    if (IsLuaKeyword(word))
                        out.Add(i, end, Token::Keyword);
                    else if (end < line.size() && line[end] == '(')
                        out.Add(i, end, Token::Call);
                    i = end;
                    continue;
                }
                ++i;
            }
            return line.size();
        }

        void AaTokens(std::string_view line, LineState& state, Spans& out)
        {
            const std::string_view trimmed = TrimView(line);
            if (state.Kind == Block::None && !trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']')
            {
                out.Add(0, line.size(), Token::Section);
                return;
            }
            bool firstWord = true;
            std::size_t i = 0;
            while (i < line.size())
            {
                if (state.Kind == Block::Lua || state.Kind == Block::LuaComment || state.Kind == Block::LuaString)
                {
                    i = LuaTokens(line, i, state, out, true);
                    continue;
                }
                if (state.Kind == Block::Brace || state.Kind == Block::Slash)
                {
                    const bool brace = state.Kind == Block::Brace;
                    const std::size_t end = brace ? line.find('}', i) : line.find("*/", i);
                    const std::size_t stop = end == npos ? line.size() : end + (brace ? 1 : 2);
                    out.Add(i, stop, Token::Comment);
                    if (end != npos)
                        state = { Block::None, 0 };
                    i = stop;
                    continue;
                }
                const char c = line[i];
                if (c == '{' && i + 1 < line.size() && line[i + 1] == '$')
                {
                    const std::size_t end = line.find('}', i);
                    const std::size_t stop = end == npos ? line.size() : end + 1;
                    out.Add(i, stop, Token::Section);
                    if (EqualsNoCase(line.substr(i, stop - i), "{$lua}"))
                        state = { Block::Lua, 0 };
                    i = stop;
                    continue;
                }
                if (c == '{')
                {
                    state = { Block::Brace, 0 };
                    continue;
                }
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                {
                    out.Add(i, line.size(), Token::Comment);
                    return;
                }
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
                {
                    state = { Block::Slash, 0 };
                    continue;
                }
                if (c == '"' || c == '\'')
                {
                    std::size_t end = line.find(c, i + 1);
                    end = end == npos ? line.size() : end + 1;
                    out.Add(i, end, Token::String);
                    i = end;
                    continue;
                }
                if (IsAaWord(c))
                {
                    std::size_t end = i;
                    while (end < line.size() && IsAaWord(line[end]))
                        ++end;
                    const std::string word = Lower(line.substr(i, end - i));
                    Token kind = Token::Text;
                    if (end < line.size() && line[end] == ':' && TrimView(line.substr(end + 1)).empty())
                        kind = Token::Label;
                    else if (IsAaCommand(word))
                        kind = Token::Keyword;
                    else if (IsRegister(word))
                        kind = Token::Register;
                    else if (firstWord)
                        kind = Token::Mnemonic;
                    else if (AaNumber(word))
                        kind = Token::Number;
                    out.Add(i, end, kind);
                    firstWord = false;
                    i = end;
                    continue;
                }
                ++i;
            }
        }
    }

    namespace
    {
        bool IsJsonNumber(char c)
        {
            return IsDigit(c) || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
        }

        void JsonTokens(std::string_view line, LineState& state, Spans& out)
        {
            std::size_t i = 0;
            while (i < line.size())
            {
                if (state.Kind == Block::Slash)
                {
                    const std::size_t end = line.find("*/", i);
                    const std::size_t stop = end == npos ? line.size() : end + 2;
                    out.Add(i, stop, Token::Comment);
                    if (end != npos)
                        state = { Block::None, 0 };
                    i = stop;
                    continue;
                }
                const char c = line[i];
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                {
                    out.Add(i, line.size(), Token::Comment);
                    return;
                }
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
                {
                    out.Add(i, i + 2, Token::Comment);
                    state = { Block::Slash, 0 };
                    i += 2;
                    continue;
                }
                if (c == '"')
                {
                    std::size_t end = i + 1;
                    while (end < line.size() && line[end] != '"')
                        end += line[end] == '\\' ? 2 : 1;
                    end = std::min(end + 1, line.size());
                    std::size_t after = end;
                    while (after < line.size() && (line[after] == ' ' || line[after] == '\t'))
                        ++after;
                    out.Add(i, end, after < line.size() && line[after] == ':' ? Token::Label : Token::String);
                    i = end;
                    continue;
                }
                if (IsDigit(c) || (c == '-' && i + 1 < line.size() && IsDigit(line[i + 1])))
                {
                    std::size_t end = i + 1;
                    while (end < line.size() && IsJsonNumber(line[end]))
                        ++end;
                    out.Add(i, end, Token::Number);
                    i = end;
                    continue;
                }
                if (IsAlpha(c))
                {
                    std::size_t end = i;
                    while (end < line.size() && IsIdentifier(line[end]))
                        ++end;
                    const std::string_view word = line.substr(i, end - i);
                    if (word == "true" || word == "false" || word == "null")
                        out.Add(i, end, Token::Keyword);
                    i = end;
                    continue;
                }
                ++i;
            }
        }
    }

    bool IsLuaKeyword(std::string_view word)
    {
        static const WordSet words(kLuaKeywords);
        return words.Has(word);
    }

    bool IsAaCommand(std::string_view lowerWord)
    {
        static const WordSet words(kAaCommands);
        return words.Has(lowerWord);
    }

    bool IsRegister(std::string_view lowerWord)
    {
        static const WordSet words(kRegisters);
        return words.Has(lowerWord);
    }

    LineState InitialState(Language language)
    {
        return { language == Language::Lua ? Block::Lua : Block::None, 0 };
    }

    void HighlightLine(Language language, std::string_view line, std::size_t offset, LineState& state, std::vector<TokenSpan>& spans)
    {
        Spans out(spans, offset);
        if (language == Language::Lua)
        {
            if (state.Kind != Block::LuaComment && state.Kind != Block::LuaString)
                state = { Block::Lua, 0 };
            LuaTokens(line, 0, state, out, false);
        }
        else if (language == Language::AutoAssembler)
        {
            AaTokens(line, state, out);
        }
        else if (language == Language::Json)
        {
            JsonTokens(line, state, out);
        }
    }

    std::vector<TokenSpan> Highlight(Language language, std::string_view text)
    {
        std::vector<TokenSpan> spans;
        LineState state = InitialState(language);
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == npos)
                end = text.size();
            HighlightLine(language, text.substr(start, end - start), start, state, spans);
            start = end + 1;
        }
        return spans;
    }
}
