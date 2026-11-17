#include "UI/CodeEditor.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string_view>
#include <vector>

namespace Manifold::UI
{
    namespace
    {
        enum class Block
        {
            None,
            Brace,
            Slash,
            Lua,
            LuaComment
        };

        struct Span
        {
            std::size_t Start;
            std::size_t End;
            ImU32 Color;
        };

        bool IsWord(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$' || c == '#';
        }

        const std::set<std::string>& AaCommands()
        {
            static const std::set<std::string> words = { "alloc", "globalalloc", "dealloc", "label", "registersymbol", "unregistersymbol", "aobscan", "aobscanmodule",
                "aobscanregion", "assert", "fullaccess", "createthread", "createthreadandwait", "loadlibrary", "readmem", "reassemble", "define", "include", "luacall",
                "db", "dw", "dd", "dq", "byte", "word", "dword", "qword", "tword", "xmmword", "ymmword", "ptr", "short", "far", "near" };
            return words;
        }

        const std::set<std::string>& Registers()
        {
            static const std::set<std::string> words = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d", "ax", "bx", "cx", "dx", "si",
                "di", "bp", "sp", "al", "ah", "bl", "bh", "cl", "ch", "dl", "dh", "sil", "dil", "bpl", "spl", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w",
                "r15w", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b", "rip", "eip", "cs", "ds", "es", "fs", "gs", "ss", "xmm0", "xmm1", "xmm2",
                "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "ymm0", "ymm1", "ymm2", "ymm3",
                "ymm4", "ymm5", "ymm6", "ymm7", "st0", "st1", "st2", "st3", "st4", "st5", "st6", "st7" };
            return words;
        }

        const std::set<std::string>& LuaKeywords()
        {
            static const std::set<std::string> words = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil",
                "not", "or", "repeat", "return", "then", "true", "until", "while" };
            return words;
        }

        bool IsNumber(std::string_view word)
        {
            if (word.empty())
                return false;
            if (word[0] == '#' || word[0] == '$')
                word.remove_prefix(1);
            if (word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X'))
                word.remove_prefix(2);
            return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) { return IsHexDigit(c) || c == '.'; }) &&
                   (word[0] >= '0' && word[0] <= '9' ? true : word.size() <= 8);
        }

        void LuaLine(std::string_view line, std::size_t offset, Block& block, std::vector<Span>& spans)
        {
            const ImU32 comment = ImGui::GetColorU32(ManifoldTheme::CodeComment);
            const ImU32 keyword = ImGui::GetColorU32(ManifoldTheme::CodeKeyword);
            const ImU32 number = ImGui::GetColorU32(ManifoldTheme::CodeNumber);
            const ImU32 string = ImGui::GetColorU32(ManifoldTheme::CodeString);
            const ImU32 text = ImGui::GetColorU32(ManifoldTheme::Text);
            std::size_t i = 0;
            while (i < line.size())
            {
                if (block == Block::LuaComment)
                {
                    const std::size_t end = line.find("]]", i);
                    const std::size_t stop = end == std::string_view::npos ? line.size() : end + 2;
                    spans.push_back({ offset + i, offset + stop, comment });
                    if (end != std::string_view::npos)
                        block = Block::Lua;
                    i = stop;
                    continue;
                }
                if (line.compare(i, 6, "{$asm}") == 0 || line.compare(i, 6, "{$ASM}") == 0)
                {
                    spans.push_back({ offset + i, offset + i + 6, ImGui::GetColorU32(ManifoldTheme::CodeSection) });
                    block = Block::None;
                    return;
                }
                const char c = line[i];
                if (c == '-' && i + 1 < line.size() && line[i + 1] == '-')
                {
                    if (line.compare(i, 4, "--[[") == 0)
                    {
                        block = Block::LuaComment;
                        spans.push_back({ offset + i, offset + i + 4, comment });
                        i += 4;
                        continue;
                    }
                    spans.push_back({ offset + i, offset + line.size(), comment });
                    return;
                }
                if (c == '"' || c == '\'')
                {
                    std::size_t end = i + 1;
                    while (end < line.size() && line[end] != c)
                        end += line[end] == '\\' ? 2 : 1;
                    end = std::min(end + 1, line.size());
                    spans.push_back({ offset + i, offset + end, string });
                    i = end;
                    continue;
                }
                if (IsWord(c))
                {
                    std::size_t end = i;
                    while (end < line.size() && (IsWord(line[end]) && line[end] != '.'))
                        ++end;
                    if (end == i)
                        end = i + 1;
                    const std::string word(line.substr(i, end - i));
                    ImU32 color = text;
                    if (LuaKeywords().count(word))
                        color = keyword;
                    else if (word[0] >= '0' && word[0] <= '9')
                        color = number;
                    else if (end < line.size() && line[end] == '(')
                        color = ImGui::GetColorU32(ManifoldTheme::CodeRegister);
                    spans.push_back({ offset + i, offset + end, color });
                    i = end;
                    continue;
                }
                spans.push_back({ offset + i, offset + i + 1, text });
                ++i;
            }
        }

        void AaLine(std::string_view line, std::size_t offset, Block& block, std::vector<Span>& spans)
        {
            const ImU32 comment = ImGui::GetColorU32(ManifoldTheme::CodeComment);
            const ImU32 text = ImGui::GetColorU32(ManifoldTheme::Text);
            const ImU32 keyword = ImGui::GetColorU32(ManifoldTheme::CodeKeyword);
            const ImU32 reg = ImGui::GetColorU32(ManifoldTheme::CodeRegister);
            const ImU32 number = ImGui::GetColorU32(ManifoldTheme::CodeNumber);
            const ImU32 string = ImGui::GetColorU32(ManifoldTheme::CodeString);
            const ImU32 section = ImGui::GetColorU32(ManifoldTheme::CodeSection);
            const ImU32 label = ImGui::GetColorU32(ManifoldTheme::CodeLabel);
            const ImU32 mnemonic = ImGui::GetColorU32(ManifoldTheme::Label);

            const std::string_view trimmed = TrimView(line);
            if (block == Block::None && !trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']')
            {
                spans.push_back({ offset, offset + line.size(), section });
                return;
            }
            bool firstWord = true;
            std::size_t i = 0;
            while (i < line.size())
            {
                if (block == Block::Lua || block == Block::LuaComment)
                {
                    LuaLine(line.substr(i), offset + i, block, spans);
                    if (block != Block::None)
                        return;
                    i = line.size();
                    for (std::size_t at = 0; at + 6 <= line.size(); ++at)
                    {
                        if (EqualsNoCase(line.substr(at, 6), "{$asm}"))
                        {
                            i = at + 6;
                            break;
                        }
                    }
                    continue;
                }
                if (block == Block::Brace)
                {
                    const std::size_t end = line.find('}', i);
                    const std::size_t stop = end == std::string_view::npos ? line.size() : end + 1;
                    spans.push_back({ offset + i, offset + stop, comment });
                    if (end != std::string_view::npos)
                        block = Block::None;
                    i = stop;
                    continue;
                }
                if (block == Block::Slash)
                {
                    const std::size_t end = line.find("*/", i);
                    const std::size_t stop = end == std::string_view::npos ? line.size() : end + 2;
                    spans.push_back({ offset + i, offset + stop, comment });
                    if (end != std::string_view::npos)
                        block = Block::None;
                    i = stop;
                    continue;
                }
                const char c = line[i];
                if (c == '{' && i + 1 < line.size() && line[i + 1] == '$')
                {
                    const std::size_t end = line.find('}', i);
                    const std::size_t stop = end == std::string_view::npos ? line.size() : end + 1;
                    const std::string_view directive = line.substr(i, stop - i);
                    spans.push_back({ offset + i, offset + stop, section });
                    if (EqualsNoCase(directive, "{$lua}"))
                        block = Block::Lua;
                    i = stop;
                    continue;
                }
                if (c == '{')
                {
                    block = Block::Brace;
                    continue;
                }
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                {
                    spans.push_back({ offset + i, offset + line.size(), comment });
                    return;
                }
                if (c == '/' && i + 1 < line.size() && line[i + 1] == '*')
                {
                    block = Block::Slash;
                    continue;
                }
                if (c == '"' || c == '\'')
                {
                    std::size_t end = line.find(c, i + 1);
                    end = end == std::string_view::npos ? line.size() : end + 1;
                    spans.push_back({ offset + i, offset + end, string });
                    i = end;
                    continue;
                }
                if (IsWord(c))
                {
                    std::size_t end = i;
                    while (end < line.size() && IsWord(line[end]))
                        ++end;
                    const std::string word = Lower(line.substr(i, end - i));
                    ImU32 color = text;
                    const bool labelPlace = end < line.size() && line[end] == ':' && TrimView(line.substr(end + 1)).empty();
                    if (labelPlace)
                        color = label;
                    else if (AaCommands().count(word))
                        color = keyword;
                    else if (Registers().count(word))
                        color = reg;
                    else if (firstWord)
                        color = mnemonic;
                    else if (IsNumber(word))
                        color = number;
                    spans.push_back({ offset + i, offset + end, color });
                    firstWord = false;
                    i = end;
                    continue;
                }
                spans.push_back({ offset + i, offset + i + 1, text });
                ++i;
            }
        }

        int TabCallback(ImGuiInputTextCallbackData* data)
        {
            auto* state = static_cast<CodeEditorState*>(data->UserData);
            if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways)
            {
                if (state->KeepCursor)
                {
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = std::clamp(state->FocusCursor, 0, data->BufTextLen);
                    state->KeepCursor = false;
                }
                CodeAssistState& assist = state->Assisting;
                const bool replace = assist.Replace;
                if (replace)
                {
                    const int start = static_cast<int>(assist.ReplaceStart);
                    const int end = static_cast<int>(assist.ReplaceEnd);
                    if (assist.RemoveTyped && end < data->BufTextLen && (data->Buf[end] == '\t' || data->Buf[end] == '\n'))
                        data->DeleteChars(end, 1);
                    if (start <= end && end <= data->BufTextLen)
                    {
                        data->DeleteChars(start, end - start);
                        data->InsertChars(start, assist.Replacement.c_str());
                        data->CursorPos = data->SelectionStart = data->SelectionEnd = start + static_cast<int>(assist.Replacement.size());
                    }
                    assist.Replace = false;
                    assist.RemoveTyped = false;
                }
                for (int i = 0; i < data->BufTextLen; ++i)
                {
                    if (data->Buf[i] == '\t')
                    {
                        data->DeleteChars(i, 1);
                        data->InsertChars(i, "    ");
                        if (data->CursorPos > i)
                            data->CursorPos += 3;
                        data->SelectionStart = data->SelectionEnd = data->CursorPos;
                    }
                }
                if (!replace && assist.RestoreCursor >= 0)
                {
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = std::min(assist.RestoreCursor, data->BufTextLen);
                    assist.RestoreCursor = -1;
                }
                if (!state->Insert.empty())
                {
                    const int at = data->CursorPos;
                    data->InsertChars(at, state->Insert.c_str());
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = at + static_cast<int>(state->Insert.size());
                    state->Insert.clear();
                }
                int line = 1;
                int column = 1;
                for (int i = 0; i < data->CursorPos && i < data->BufTextLen; ++i)
                {
                    if (data->Buf[i] == '\n')
                    {
                        ++line;
                        column = 1;
                    }
                    else
                    {
                        ++column;
                    }
                }
                state->CursorLine = line;
                state->CursorColumn = column;
                if (state->GotoLine > 0)
                {
                    int target = 0;
                    int current = 1;
                    for (int i = 0; i < data->BufTextLen && current < state->GotoLine; ++i)
                    {
                        if (data->Buf[i] == '\n')
                        {
                            ++current;
                            target = i + 1;
                        }
                    }
                    data->CursorPos = data->SelectionStart = data->SelectionEnd = target;
                    state->GotoLine = 0;
                }
                state->CursorOffset = data->CursorPos;
            }
            return 0;
        }

        bool IsIdentifier(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool InCode(const std::string& text, std::size_t cursor)
        {
            std::size_t lineStart = cursor;
            while (lineStart > 0 && text[lineStart - 1] != '\n')
                --lineStart;
            char quote = 0;
            for (std::size_t i = lineStart; i < cursor; ++i)
            {
                const char c = text[i];
                if (quote)
                {
                    if (c == '\\')
                        ++i;
                    else if (c == quote)
                        quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'')
                    quote = c;
                else if (c == '-' && i + 1 < cursor && text[i + 1] == '-')
                    return false;
            }
            return quote == 0;
        }

        std::size_t ChainStart(const std::string& text, std::size_t end)
        {
            std::size_t position = end;
            while (true)
            {
                const std::size_t identifierEnd = position;
                while (position > 0 && IsIdentifier(text[position - 1]))
                    --position;
                if (position == identifierEnd)
                    return std::string::npos;
                if (position > 1 && text[position - 1] == '.' && text[position - 2] != '.')
                {
                    --position;
                    continue;
                }
                return position;
            }
        }

        struct WordContext
        {
            bool Valid = false;
            std::size_t Start = 0;
            std::string Prefix;
            std::string Chain;
            bool Method = false;
            bool Member = false;
        };

        WordContext WordAt(const std::string& text, std::size_t cursor)
        {
            WordContext context;
            cursor = std::min(cursor, text.size());
            if (!InCode(text, cursor))
                return context;
            std::size_t start = cursor;
            while (start > 0 && IsIdentifier(text[start - 1]))
                --start;
            if (start < cursor && IsDigit(text[start]))
                return context;
            context.Start = start;
            context.Prefix = text.substr(start, cursor - start);
            if (start > 0 && (text[start - 1] == '.' || text[start - 1] == ':') && !(start > 1 && text[start - 2] == '.'))
            {
                const std::size_t chainStart = ChainStart(text, start - 1);
                if (chainStart == std::string::npos || IsDigit(text[chainStart]))
                    return WordContext{};
                context.Chain = text.substr(chainStart, start - 1 - chainStart);
                context.Method = text[start - 1] == ':';
                context.Member = true;
            }
            context.Valid = true;
            return context;
        }

        struct CallContext
        {
            bool Valid = false;
            std::string Chain;
            bool Method = false;
            int Argument = 0;
            std::size_t Paren = 0;
        };

        CallContext CallAt(const std::string& text, std::size_t cursor)
        {
            CallContext context;
            cursor = std::min(cursor, text.size());
            std::size_t from = cursor > 2000 ? cursor - 2000 : 0;
            while (from > 0 && text[from - 1] != '\n')
                --from;
            struct Open
            {
                std::size_t At;
                char Kind;
                int Commas;
            };
            std::vector<Open> stack;
            char quote = 0;
            for (std::size_t i = from; i < cursor; ++i)
            {
                const char c = text[i];
                if (quote)
                {
                    if (c == '\\')
                        ++i;
                    else if (c == quote || c == '\n')
                        quote = 0;
                    continue;
                }
                if (c == '-' && i + 1 < cursor && text[i + 1] == '-')
                {
                    while (i < cursor && text[i] != '\n')
                        ++i;
                    continue;
                }
                if (c == '"' || c == '\'')
                    quote = c;
                else if (c == '(' || c == '{' || c == '[')
                    stack.push_back({ i, c, 0 });
                else if ((c == ')' || c == '}' || c == ']') && !stack.empty())
                    stack.pop_back();
                else if (c == ',' && !stack.empty())
                    ++stack.back().Commas;
            }
            if (quote || stack.empty() || stack.back().Kind != '(')
                return context;
            std::size_t end = stack.back().At;
            while (end > 0 && text[end - 1] == ' ')
                --end;
            std::size_t start = end;
            while (start > 0 && IsIdentifier(text[start - 1]))
                --start;
            if (start == end || IsDigit(text[start]))
                return context;
            std::string chain = text.substr(start, end - start);
            if (start > 1 && (text[start - 1] == '.' || text[start - 1] == ':') && text[start - 2] != '.')
            {
                const std::size_t chainStart = ChainStart(text, start - 1);
                if (chainStart == std::string::npos)
                    return context;
                context.Method = text[start - 1] == ':';
                chain = text.substr(chainStart, start - 1 - chainStart) + "." + chain;
                start = chainStart;
            }
            std::size_t before = start;
            while (before > 0 && (text[before - 1] == ' ' || text[before - 1] == '\t'))
                --before;
            if (before >= 8 && text.compare(before - 8, 8, "function") == 0 && (before == 8 || !IsIdentifier(text[before - 9])))
                return context;
            if (LuaKeywords().count(chain))
                return context;
            context.Valid = true;
            context.Chain = std::move(chain);
            context.Argument = stack.back().Commas;
            context.Paren = stack.back().At;
            return context;
        }

        std::vector<std::pair<std::string, std::string>> BufferFunctions(const std::string& text)
        {
            std::vector<std::pair<std::string, std::string>> result;
            for (std::size_t at = text.find("function"); at != std::string::npos; at = text.find("function", at + 8))
            {
                if ((at > 0 && IsIdentifier(text[at - 1])) || (at + 8 < text.size() && IsIdentifier(text[at + 8])))
                    continue;
                std::size_t i = at + 8;
                while (i < text.size() && text[i] == ' ')
                    ++i;
                std::string name;
                if (i < text.size() && text[i] == '(')
                {
                    std::size_t back = at;
                    while (back > 0 && text[back - 1] == ' ')
                        --back;
                    if (back > 0 && text[back - 1] == '=')
                    {
                        --back;
                        while (back > 0 && text[back - 1] == ' ')
                            --back;
                        std::size_t nameStart = back;
                        while (nameStart > 0 && (IsIdentifier(text[nameStart - 1]) || text[nameStart - 1] == '.'))
                            --nameStart;
                        name = text.substr(nameStart, back - nameStart);
                    }
                }
                else
                {
                    const std::size_t nameStart = i;
                    while (i < text.size() && (IsIdentifier(text[i]) || text[i] == '.' || text[i] == ':'))
                        ++i;
                    name = text.substr(nameStart, i - nameStart);
                    while (i < text.size() && text[i] == ' ')
                        ++i;
                }
                if (name.empty() || i >= text.size() || text[i] != '(')
                    continue;
                const std::size_t close = text.find(')', i);
                if (close == std::string::npos)
                    continue;
                const std::size_t separator = name.find_last_of(".:");
                std::string params = Trim(text.substr(i + 1, close - i - 1));
                if (separator != std::string::npos && name[separator] == ':')
                    params = params.empty() ? "self" : "self, " + params;
                result.emplace_back(std::move(name), std::move(params));
            }
            return result;
        }

        void AddBufferWords(const std::string& text, const WordContext& word, std::vector<CodeSuggestion>& items)
        {
            std::set<std::string> known;
            for (const CodeSuggestion& item : items)
                known.insert(Lower(item.Name));
            const auto functions = BufferFunctions(text);
            const std::string prefix = Lower(word.Prefix);
            const std::string member = word.Member ? word.Chain : std::string();
            std::size_t i = 0;
            while (i < text.size() && items.size() < 400)
            {
                if (!IsIdentifier(text[i]) || IsDigit(text[i]))
                {
                    ++i;
                    continue;
                }
                const std::size_t start = i;
                while (i < text.size() && IsIdentifier(text[i]))
                    ++i;
                if (start == word.Start)
                    continue;
                const bool afterSeparator = start > 0 && (text[start - 1] == '.' || text[start - 1] == ':');
                if (word.Member)
                {
                    if (!afterSeparator || start < member.size() + 1 || text.compare(start - 1 - member.size(), member.size(), member) != 0)
                        continue;
                    if (start - 1 > member.size() && (IsIdentifier(text[start - 2 - member.size()]) || text[start - 2 - member.size()] == '.'))
                        continue;
                }
                else if (afterSeparator)
                {
                    continue;
                }
                const std::string name = text.substr(start, i - start);
                const std::string lower = Lower(name);
                if (name.size() < 3 || known.count(lower) || LuaKeywords().count(name) || lower.compare(0, prefix.size(), prefix) != 0 || lower == prefix)
                    continue;
                known.insert(lower);
                CodeSuggestion suggestion{ name, "word", "", "" };
                for (const auto& [function, params] : functions)
                {
                    const std::size_t separator = function.find_last_of(".:");
                    const std::string last = separator == std::string::npos ? function : function.substr(separator + 1);
                    if (last == name)
                    {
                        suggestion.Kind = "function";
                        suggestion.Signature = "(" + params + ")";
                        break;
                    }
                }
                items.push_back(std::move(suggestion));
            }
        }

        void Refresh(CodeAssistState& assist, const CodeAssist& provider, const std::string& text, const WordContext& word)
        {
            std::vector<CodeSuggestion> items = provider.Complete ? provider.Complete(word.Chain, word.Prefix, word.Method) : std::vector<CodeSuggestion>{};
            AddBufferWords(text, word, items);
            std::stable_sort(items.begin(), items.end(), [&](const CodeSuggestion& a, const CodeSuggestion& b)
                {
                    const bool left = a.Name.compare(0, word.Prefix.size(), word.Prefix) == 0;
                    const bool right = b.Name.compare(0, word.Prefix.size(), word.Prefix) == 0;
                    if (left != right)
                        return left;
                    return Lower(a.Name) < Lower(b.Name);
                });
            if (items.empty() || (items.size() == 1 && items.front().Name == word.Prefix))
            {
                assist.Open = false;
                return;
            }
            const std::string previous = assist.Open && assist.Selected < static_cast<int>(assist.Items.size()) ? assist.Items[static_cast<std::size_t>(assist.Selected)].Name : std::string();
            assist.Items = std::move(items);
            assist.Selected = 0;
            for (std::size_t i = 0; i < assist.Items.size(); ++i)
            {
                if (!previous.empty() && assist.Items[i].Name == previous)
                    assist.Selected = static_cast<int>(i);
            }
            assist.Open = true;
            assist.WordStart = word.Start;
            assist.Chain = word.Chain;
            assist.Method = word.Method;
        }

        std::vector<std::string> SplitParameters(std::string_view params)
        {
            std::vector<std::string> parts;
            int depth = 0;
            std::size_t start = 0;
            for (std::size_t i = 0; i < params.size(); ++i)
            {
                const char c = params[i];
                if (c == '(' || c == '{' || c == '[')
                    ++depth;
                else if (c == ')' || c == '}' || c == ']')
                    --depth;
                else if (c == ',' && depth == 0)
                {
                    parts.push_back(Trim(params.substr(start, i - start)));
                    start = i + 1;
                }
            }
            if (!TrimView(params.substr(start)).empty())
                parts.push_back(Trim(params.substr(start)));
            return parts;
        }

        const char* KindIcon(const std::string& kind)
        {
            if (kind == "function" || kind == "method")
                return ICON_FA_CUBE;
            if (kind == "table")
                return ICON_FA_FOLDER;
            if (kind == "object")
                return ICON_FA_CUBES;
            if (kind == "property")
                return ICON_FA_TAG;
            if (kind == "event")
                return ICON_FA_BOLT;
            if (kind == "keyword")
                return ICON_FA_KEY;
            if (kind == "word")
                return ICON_FA_FONT;
            return ICON_FA_CIRCLE;
        }

        ImVec4 KindColor(const std::string& kind)
        {
            if (kind == "function" || kind == "method")
                return ManifoldTheme::CodeRegister;
            if (kind == "table")
                return ManifoldTheme::CodeLabel;
            if (kind == "object" || kind == "property")
                return ManifoldTheme::AddressText;
            if (kind == "event")
                return ManifoldTheme::Warning;
            if (kind == "keyword")
                return ManifoldTheme::CodeKeyword;
            if (kind == "value")
                return ManifoldTheme::CodeNumber;
            return ManifoldTheme::Muted;
        }

        constexpr ImGuiWindowFlags kOverlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;

        void DrawSuggestions(const char* id, CodeAssistState& assist, ImVec2 caret, float lineHeight, ImGuiID viewport)
        {
            constexpr int kVisible = 10;
            const int count = static_cast<int>(assist.Items.size());
            assist.Selected = std::clamp(assist.Selected, 0, count - 1);
            if (assist.Selected < assist.First)
                assist.First = assist.Selected;
            if (assist.Selected >= assist.First + kVisible)
                assist.First = assist.Selected - kVisible + 1;
            assist.First = std::clamp(assist.First, 0, std::max(0, count - kVisible));

            ImGui::SetNextWindowPos(ImVec2(caret.x - Px(6.0f), caret.y + lineHeight + Px(3.0f)));
            ImGui::SetNextWindowViewport(viewport);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(6.0f, 6.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(5.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::Border);
            const std::string name = std::string("##suggestions") + id;
            if (ImGui::Begin(name.c_str(), nullptr, kOverlayFlags))
            {
                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
                const float width = std::round(Px(470.0f));
                ImDrawList* draw = ImGui::GetWindowDrawList();
                ImFont* font = Fonts::Mono();
                ImGui::PushFont(font, 0.0f);
                const float fontSize = ImGui::GetFontSize();
                const float rowHeight = std::round(ImGui::GetTextLineHeight() + Px(5.0f));
                const float iconWidth = std::round(Px(22.0f));
                for (int i = assist.First; i < std::min(count, assist.First + kVisible); ++i)
                {
                    const CodeSuggestion& item = assist.Items[static_cast<std::size_t>(i)];
                    const ImVec2 row = ImGui::GetCursorScreenPos();
                    if (i == assist.Selected)
                        draw->AddRectFilled(row, ImVec2(row.x + width, row.y + rowHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.20f)), Px(3.0f));
                    const float textY = row.y + std::round((rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
                    draw->AddText(font, fontSize, ImVec2(row.x + Px(5.0f), textY), ImGui::GetColorU32(KindColor(item.Kind)), KindIcon(item.Kind));
                    const ImVec4 clip(row.x, row.y, row.x + width - Px(4.0f), row.y + rowHeight);
                    const float nameX = row.x + iconWidth;
                    draw->AddText(font, fontSize, ImVec2(nameX, textY), ImGui::GetColorU32(ManifoldTheme::Text), item.Name.c_str(), nullptr, 0.0f, &clip);
                    if (!item.Signature.empty())
                    {
                        const float signatureX = nameX + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, item.Name.c_str()).x;
                        draw->AddText(font, fontSize, ImVec2(signatureX, textY), ImGui::GetColorU32(ManifoldTheme::Muted), item.Signature.c_str(), nullptr, 0.0f, &clip);
                    }
                    ImGui::Dummy(ImVec2(width, rowHeight));
                }
                ImGui::PopFont();
                const CodeSuggestion& current = assist.Items[static_cast<std::size_t>(assist.Selected)];
                if (!current.Text.empty() || count > kVisible)
                {
                    ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
                    ImGui::PushStyleColor(ImGuiCol_Separator, ManifoldTheme::Border);
                    ImGui::Separator();
                    ImGui::PopStyleColor();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                    if (!current.Text.empty())
                        ImGui::TextColored(ManifoldTheme::Label, "%s", current.Text.c_str());
                    if (count > kVisible)
                        ImGui::TextColored(ManifoldTheme::Muted, "%d of %d  " ICON_FA_LONG_ARROW_ALT_UP ICON_FA_LONG_ARROW_ALT_DOWN "  Tab or Enter to insert", assist.Selected + 1, count);
                    ImGui::PopTextWrapPos();
                }
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }

        void DrawHints(const char* id, const CodeAssistState& assist, ImVec2 caret, ImGuiID viewport)
        {
            ImGui::SetNextWindowPos(ImVec2(caret.x - Px(6.0f), caret.y - Px(3.0f)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowViewport(viewport);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(8.0f, 5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(5.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, PxWhole(0.0f, 3.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::Border);
            const std::string name = std::string("##hints") + id;
            if (ImGui::Begin(name.c_str(), nullptr, kOverlayFlags))
            {
                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
                ImGui::PushFont(Fonts::Mono(), 0.0f);
                const std::size_t shown = std::min<std::size_t>(assist.Hints.size(), 4);
                for (std::size_t h = 0; h < shown; ++h)
                {
                    const std::string& hint = assist.Hints[h];
                    const std::size_t open = hint.find('(');
                    const std::size_t close = hint.rfind(')');
                    const std::string head = open == std::string::npos ? hint : hint.substr(0, open);
                    const std::vector<std::string> params = open == std::string::npos || close == std::string::npos || close < open ? std::vector<std::string>{} :
                        SplitParameters(std::string_view(hint).substr(open + 1, close - open - 1));
                    int current = assist.HintArgument;
                    if (!params.empty() && current >= static_cast<int>(params.size()) && params.back().find("...") != std::string::npos)
                        current = static_cast<int>(params.size()) - 1;
                    ImGui::TextColored(ManifoldTheme::CodeRegister, "%s", head.c_str());
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ManifoldTheme::Muted, "(");
                    for (std::size_t p = 0; p < params.size(); ++p)
                    {
                        ImGui::SameLine(0.0f, 0.0f);
                        if (p > 0)
                        {
                            ImGui::TextColored(ManifoldTheme::Muted, ", ");
                            ImGui::SameLine(0.0f, 0.0f);
                        }
                        const bool active = static_cast<int>(p) == current;
                        ImGui::TextColored(active ? ManifoldTheme::Accent : ManifoldTheme::Text, "%s", params[p].c_str());
                        if (active)
                        {
                            const ImVec2 min = ImGui::GetItemRectMin();
                            const ImVec2 max = ImGui::GetItemRectMax();
                            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), ImGui::GetColorU32(ManifoldTheme::Accent), Px(1.0f));
                        }
                    }
                    ImGui::SameLine(0.0f, 0.0f);
                    ImGui::TextColored(ManifoldTheme::Muted, ")");
                }
                if (assist.Hints.size() > shown)
                    ImGui::TextColored(ManifoldTheme::Muted, "+%zu more", assist.Hints.size() - shown);
                if (!assist.HintChoices.empty())
                {
                    std::string choices;
                    for (const std::string& value : assist.HintChoices)
                        choices += (choices.empty() ? "\"" : "  \"") + value + "\"";
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Px(360.0f));
                    ImGui::TextColored(ManifoldTheme::CodeString, "%s", choices.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::PopFont();
            }
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }
    }

    bool CodeEditor(const char* id, std::string& text, Language language, ImVec2 size, CodeEditorState& state)
    {
        if (text.find('\t') != std::string::npos)
            text = ReplaceAll(text, "\t", "    ");

        ImGui::PushID(id);
        ImGui::PushFont(Fonts::Mono(), 0.0f);
        const float lineHeight = ImGui::GetTextLineHeight();
        const int lines = static_cast<int>(std::count(text.begin(), text.end(), '\n')) + 1;
        state.LineCount = lines;
        const int digits = std::max(3, static_cast<int>(std::to_string(lines).size()));
        const float gutter = ImGui::CalcTextSize("0").x * static_cast<float>(digits) + Px(14.0f);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (size.x <= 0.0f)
            size.x = available.x + size.x;
        if (size.y <= 0.0f)
            size.y = available.y + size.y;
        size.x = std::max(size.x, gutter + Px(60.0f));
        size.y = std::max(size.y, lineHeight * 3.0f);

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList* parentList = ImGui::GetWindowDrawList();
        parentList->AddRectFilled(origin, ImVec2(origin.x + gutter, origin.y + size.y), ImGui::GetColorU32(ManifoldTheme::Sidebar), Px(4.0f), ImDrawFlags_RoundCornersLeft);

        CodeAssistState& assist = state.Assisting;
        const bool assistEnabled = language == Language::Lua && state.Assist && !state.ReadOnly;
        if (state.Active)
        {
            const ImGuiID owner = ImGui::GetID("##assist");
            const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive;
            if (ImGui::Shortcut(ImGuiKey_Escape, route, owner))
            {
                if (!assist.Open && !assist.Hints.empty())
                    assist.DismissedParen = assist.HintParen;
                assist.Open = false;
            }
            if (assistEnabled && assist.Open && !assist.Items.empty())
            {
                const ImGuiIO& io = ImGui::GetIO();
                const bool plain = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
                const bool tab = plain && ImGui::IsKeyPressed(ImGuiKey_Tab, false);
                const bool enter = plain && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
                const bool keypad = plain && ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
                const int count = static_cast<int>(assist.Items.size());
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                {
                    assist.Selected = (assist.Selected + 1) % count;
                    assist.RestoreCursor = state.CursorOffset;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                {
                    assist.Selected = (assist.Selected + count - 1) % count;
                    assist.RestoreCursor = state.CursorOffset;
                }
                if (tab || enter || keypad)
                {
                    assist.Replace = true;
                    assist.RemoveTyped = true;
                    assist.ReplaceStart = assist.WordStart;
                    assist.ReplaceEnd = static_cast<std::size_t>(std::max(0, state.CursorOffset));
                    assist.Replacement = assist.Items[static_cast<std::size_t>(std::clamp(assist.Selected, 0, count - 1))].Name;
                    assist.Open = false;
                    assist.JustAccepted = true;
                }
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x + gutter, origin.y));
        if (state.Focus)
        {
            ImGui::SetKeyboardFocusHere();
            if (!state.KeepCursor)
            {
                state.KeepCursor = true;
                state.FocusCursor = state.CursorOffset;
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ManifoldTheme::WithAlpha(ManifoldTheme::Sidebar, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ManifoldTheme::Text);
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways;
        if (state.ReadOnly)
            flags |= ImGuiInputTextFlags_ReadOnly;
        const bool changed = InputMultiline("##code", text, ImVec2(size.x - gutter, size.y), flags, &TabCallback, &state);
        ImGui::PopStyleColor(3);
        state.Active = ImGui::IsItemActive();
        if (state.Focus && (state.Active || ++state.FocusAttempts > 30))
        {
            if (!state.Active)
                state.KeepCursor = false;
            state.Focus = false;
            state.FocusAttempts = 0;
        }

        const std::size_t cursor = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, state.CursorOffset)), text.size());
        if (assistEnabled)
        {
            if (!state.Active)
            {
                assist.Open = false;
                assist.Hints.clear();
                assist.HintChain.clear();
                assist.HintChoices.clear();
                assist.HintChoiceChain.clear();
                assist.HintChoiceArgument = -1;
            }
            else
            {
                const WordContext word = WordAt(text, cursor);
                const bool force = ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);
                if (assist.JustAccepted)
                {
                    assist.JustAccepted = false;
                }
                else if (force || changed)
                {
                    const char typed = cursor > 0 ? text[cursor - 1] : '\0';
                    const bool want = word.Valid && (force || (word.Member && (typed == '.' || typed == ':' || IsIdentifier(typed))) ||
                        (!word.Member && IsIdentifier(typed) && (word.Prefix.size() >= 2 || assist.Open)));
                    if (want)
                        Refresh(assist, *state.Assist, text, word);
                    else
                        assist.Open = false;
                }
                else if (assist.Open && (!word.Valid || word.Start != assist.WordStart || word.Chain != assist.Chain))
                {
                    assist.Open = false;
                }

                const CallContext call = CallAt(text, cursor);
                if (call.Valid && call.Paren != assist.DismissedParen)
                {
                    if (call.Chain != assist.HintChain || call.Method != assist.HintMethod || call.Paren != assist.HintParen)
                    {
                        assist.HintChain = call.Chain;
                        assist.HintMethod = call.Method;
                        assist.HintParen = call.Paren;
                        assist.Hints = state.Assist->Signatures ? state.Assist->Signatures(call.Chain, call.Method) : std::vector<std::string>{};
                        if (assist.Hints.empty())
                        {
                            for (const auto& [function, params] : BufferFunctions(text))
                            {
                                std::string dotted = function;
                                std::replace(dotted.begin(), dotted.end(), ':', '.');
                                if (dotted == call.Chain || (call.Chain.find('.') == std::string::npos && dotted.substr(dotted.find_last_of('.') + 1) == call.Chain))
                                {
                                    const std::string last = dotted.substr(dotted.find_last_of('.') + 1);
                                    std::string shown = params;
                                    if (call.Method && (shown == "self" || shown.rfind("self,", 0) == 0))
                                        shown = Trim(shown.substr(std::min<std::size_t>(shown.size(), 5)));
                                    assist.Hints.push_back(last + "(" + shown + ")");
                                    break;
                                }
                            }
                        }
                    }
                    assist.HintArgument = call.Argument;
                    if (call.Chain != assist.HintChoiceChain || call.Argument != assist.HintChoiceArgument)
                    {
                        assist.HintChoiceChain = call.Chain;
                        assist.HintChoiceArgument = call.Argument;
                        assist.HintChoices = state.Assist->Choices ? state.Assist->Choices(call.Chain, call.Method, call.Argument) : std::vector<std::string>{};
                    }
                }
                else
                {
                    assist.Hints.clear();
                    assist.HintChain.clear();
                    assist.HintChoices.clear();
                    assist.HintChoiceChain.clear();
                    assist.HintChoiceArgument = -1;
                    if (!call.Valid)
                        assist.DismissedParen = static_cast<std::size_t>(-1);
                }
            }
        }
        bool caretKnown = false;
        ImVec2 caret(0.0f, 0.0f);

        ImGuiWindow* parent = ImGui::GetCurrentWindow();
        ImGuiWindow* child = parent->DC.ChildWindows.empty() ? nullptr : parent->DC.ChildWindows.back();
        if (child)
        {
            const ImVec2 padding = ImGui::GetStyle().FramePadding;
            const ImVec2 textOrigin(child->Pos.x + padding.x - child->Scroll.x, child->Pos.y + padding.y - child->Scroll.y);
            const int firstVisible = std::max(0, static_cast<int>((child->Scroll.y - padding.y) / lineHeight));
            const int lastVisible = firstVisible + static_cast<int>(child->Size.y / lineHeight) + 2;

            ImDrawList* drawList = child->DrawList;
            drawList->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
            ImFont* font = Fonts::Mono();
            const float fontSize = ImGui::GetFontSize();
            {
                std::size_t lineBegin = cursor;
                while (lineBegin > 0 && text[lineBegin - 1] != '\n')
                    --lineBegin;
                caret.x = textOrigin.x + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.data() + lineBegin, text.data() + cursor).x;
                caret.y = textOrigin.y + static_cast<float>(state.CursorLine - 1) * lineHeight;
                caretKnown = true;
            }

            Block block = Block::None;
            if (language == Language::Lua)
                block = Block::Lua;
            std::vector<Span> spans;
            std::size_t lineStart = 0;
            for (int line = 0; lineStart <= text.size(); ++line)
            {
                std::size_t lineEnd = text.find('\n', lineStart);
                if (lineEnd == std::string::npos)
                    lineEnd = text.size();
                const std::string_view content(text.data() + lineStart, lineEnd - lineStart);
                const bool visible = line >= firstVisible && line <= lastVisible;
                spans.clear();
                if (language == Language::Plain)
                {
                    if (visible)
                        spans.push_back({ lineStart, lineEnd, ImGui::GetColorU32(ManifoldTheme::Text) });
                }
                else if (language == Language::Lua)
                    LuaLine(content, lineStart, block, spans);
                else
                    AaLine(content, lineStart, block, spans);

                if (visible)
                {
                    const float y = textOrigin.y + static_cast<float>(line) * lineHeight;
                    if (state.ErrorLine == line + 1)
                        drawList->AddRectFilled(ImVec2(child->Pos.x, y), ImVec2(child->Pos.x + child->Size.x, y + lineHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.20f)));
                    float x = textOrigin.x;
                    std::size_t drawn = lineStart;
                    for (const Span& span : spans)
                    {
                        if (span.Start > drawn)
                        {
                            const char* begin = text.data() + drawn;
                            const char* end = text.data() + span.Start;
                            drawList->AddText(font, fontSize, ImVec2(x, y), ImGui::GetColorU32(ManifoldTheme::Text), begin, end);
                            x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, end).x;
                        }
                        const char* begin = text.data() + span.Start;
                        const char* end = text.data() + span.End;
                        drawList->AddText(font, fontSize, ImVec2(x, y), span.Color, begin, end);
                        x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, begin, end).x;
                        drawn = span.End;
                    }
                    if (drawn < lineEnd)
                        drawList->AddText(font, fontSize, ImVec2(x, y), ImGui::GetColorU32(ManifoldTheme::Text), text.data() + drawn, text.data() + lineEnd);
                }
                if (line > lastVisible && block == Block::None && language != Language::Lua)
                    break;
                lineStart = lineEnd + 1;
            }
            drawList->PopClipRect();

            parentList->PushClipRect(origin, ImVec2(origin.x + gutter, origin.y + size.y), true);
            for (int line = firstVisible; line <= std::min(lastVisible, lines - 1); ++line)
            {
                const float y = textOrigin.y + static_cast<float>(line) * lineHeight;
                const std::string number = std::to_string(line + 1);
                const float width = ImGui::CalcTextSize(number.c_str()).x;
                const bool current = line + 1 == state.CursorLine;
                const ImVec4 color = state.ErrorLine == line + 1 ? ManifoldTheme::Error : current ? ManifoldTheme::Label : ManifoldTheme::Muted;
                parentList->AddText(font, fontSize, ImVec2(origin.x + gutter - width - Px(7.0f), y), ImGui::GetColorU32(color), number.c_str());
            }
            parentList->PopClipRect();
        }
        ImGui::PopFont();
        if (assistEnabled && state.Active && caretKnown)
        {
            const ImGuiID viewport = ImGui::GetWindowViewport()->ID;
            if (assist.Open && !assist.Items.empty())
                DrawSuggestions(id, assist, caret, lineHeight, viewport);
            else if (!assist.Hints.empty())
                DrawHints(id, assist, caret, viewport);
        }
        ImGui::PopID();
        return changed;
    }
}
