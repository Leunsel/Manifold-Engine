#include "Debug/BreakAction.h"

#include "Core/Text.h"
#include "Target/Process.h"

#include <cctype>
#include <charconv>
#include <cstring>

namespace Manifold::Debug
{
    namespace
    {
        using namespace Program;

        struct RegisterName
        {
            const char* Name;
            std::uint8_t Index;
            std::uint8_t Part;
            bool Only64;
        };

        constexpr RegisterName kRegisters[] = {
            { "RAX", 0, PartFull, true }, { "RBX", 1, PartFull, true }, { "RCX", 2, PartFull, true }, { "RDX", 3, PartFull, true },
            { "RSI", 4, PartFull, true }, { "RDI", 5, PartFull, true }, { "RBP", 6, PartFull, true }, { "RSP", 7, PartFull, true },
            { "R8", 8, PartFull, true }, { "R9", 9, PartFull, true }, { "R10", 10, PartFull, true }, { "R11", 11, PartFull, true },
            { "R12", 12, PartFull, true }, { "R13", 13, PartFull, true }, { "R14", 14, PartFull, true }, { "R15", 15, PartFull, true },
            { "RIP", kRip, PartFull, true }, { "RFLAGS", kFlags, PartFull, true },
            { "EAX", 0, PartLow32, false }, { "EBX", 1, PartLow32, false }, { "ECX", 2, PartLow32, false }, { "EDX", 3, PartLow32, false },
            { "ESI", 4, PartLow32, false }, { "EDI", 5, PartLow32, false }, { "EBP", 6, PartLow32, false }, { "ESP", 7, PartLow32, false },
            { "EIP", kRip, PartLow32, false }, { "EFLAGS", kFlags, PartLow32, false },
            { "R8D", 8, PartLow32, true }, { "R9D", 9, PartLow32, true }, { "R10D", 10, PartLow32, true }, { "R11D", 11, PartLow32, true },
            { "R12D", 12, PartLow32, true }, { "R13D", 13, PartLow32, true }, { "R14D", 14, PartLow32, true }, { "R15D", 15, PartLow32, true },
            { "AX", 0, PartLow16, false }, { "BX", 1, PartLow16, false }, { "CX", 2, PartLow16, false }, { "DX", 3, PartLow16, false },
            { "SI", 4, PartLow16, false }, { "DI", 5, PartLow16, false }, { "BP", 6, PartLow16, false }, { "SP", 7, PartLow16, false },
            { "R8W", 8, PartLow16, true }, { "R9W", 9, PartLow16, true }, { "R10W", 10, PartLow16, true }, { "R11W", 11, PartLow16, true },
            { "R12W", 12, PartLow16, true }, { "R13W", 13, PartLow16, true }, { "R14W", 14, PartLow16, true }, { "R15W", 15, PartLow16, true },
            { "AL", 0, PartLow8, false }, { "BL", 1, PartLow8, false }, { "CL", 2, PartLow8, false }, { "DL", 3, PartLow8, false },
            { "SIL", 4, PartLow8, true }, { "DIL", 5, PartLow8, true }, { "BPL", 6, PartLow8, true }, { "SPL", 7, PartLow8, true },
            { "R8B", 8, PartLow8, true }, { "R9B", 9, PartLow8, true }, { "R10B", 10, PartLow8, true }, { "R11B", 11, PartLow8, true },
            { "R12B", 12, PartLow8, true }, { "R13B", 13, PartLow8, true }, { "R14B", 14, PartLow8, true }, { "R15B", 15, PartLow8, true },
            { "R8L", 8, PartLow8, true }, { "R9L", 9, PartLow8, true }, { "R10L", 10, PartLow8, true }, { "R11L", 11, PartLow8, true },
            { "R12L", 12, PartLow8, true }, { "R13L", 13, PartLow8, true }, { "R14L", 14, PartLow8, true }, { "R15L", 15, PartLow8, true },
            { "AH", 0, PartHigh8, false }, { "BH", 1, PartHigh8, false }, { "CH", 2, PartHigh8, false }, { "DH", 3, PartHigh8, false },
        };

        struct FlagName
        {
            const char* Name;
            std::uint8_t Bit;
        };

        constexpr FlagName kFlagNames[] = { { "CF", 0 }, { "PF", 2 }, { "AF", 4 }, { "ZF", 6 }, { "SF", 7 }, { "TF", 8 }, { "IF", 9 }, { "DF", 10 }, { "OF", 11 } };

        struct ReadFunction
        {
            const char* Name;
            std::uint8_t Unsigned;
            std::uint8_t Signed;
        };

        constexpr ReadFunction kReads[] = {
            { "readByte", ReadU8, ReadS8 },
            { "readSmallInteger", ReadU16, ReadS16 },
            { "readShortInteger", ReadU16, ReadS16 },
            { "readWord", ReadU16, ReadS16 },
            { "readInteger", ReadU32, ReadS32 },
            { "readDword", ReadU32, ReadS32 },
            { "readQword", ReadU64, ReadU64 },
            { "readFloat", ReadF32, ReadF32 },
            { "readDouble", ReadF64, ReadF64 },
        };

        enum class TokenKind
        {
            End,
            Number,
            Name,
            String,
            Symbol
        };

        struct Token
        {
            TokenKind Kind = TokenKind::End;
            std::string Text;
            Value Number;
            std::size_t Column = 0;
        };

        const RegisterName* FindRegister(std::string_view name)
        {
            for (const RegisterName& entry : kRegisters)
            {
                if (EqualsNoCase(entry.Name, name))
                    return &entry;
            }
            return nullptr;
        }

        const FlagName* FindFlag(std::string_view name)
        {
            for (const FlagName& entry : kFlagNames)
            {
                if (EqualsNoCase(entry.Name, name))
                    return &entry;
            }
            return nullptr;
        }

        const ReadFunction* FindRead(std::string_view name)
        {
            for (const ReadFunction& entry : kReads)
            {
                if (EqualsNoCase(entry.Name, name))
                    return &entry;
            }
            return nullptr;
        }

        class Compiler
        {
        public:
            Compiler(std::string_view text, bool is64Bit, const SymbolResolver& resolve, std::vector<std::uint8_t>& code)
                : m_text(text), m_is64(is64Bit), m_resolve(resolve), m_code(code)
            {
            }

            Status Expression()
            {
                if (Status lexed = Lex(); !lexed)
                    return lexed;
                if (m_tokens.size() == 1)
                    return Fail("it is empty");
                if (!Or())
                    return m_error;
                if (Peek().Kind != TokenKind::End)
                    return Fail("\"" + Peek().Text + "\" does not belong there" + Where(Peek()));
                if (m_code.size() > static_cast<std::size_t>(kMaxCode))
                    return Fail("it is too long (" + std::to_string(m_code.size()) + " bytes of code, at most " + std::to_string(kMaxCode) + ")");
                if (m_deepest > kMaxStack)
                    return Fail("it is nested too deeply (" + std::to_string(m_deepest) + " values at once, at most " + std::to_string(kMaxStack) + ")");
                return Status::Success();
            }

        private:
            Status Fail(const std::string& detail)
            {
                m_error = Status::Fail(ErrorCode::DebugBadExpression, detail);
                return m_error;
            }

            bool Error(const std::string& detail)
            {
                Fail(detail);
                return false;
            }

            std::string Where(const Token& token) const
            {
                if (token.Kind == TokenKind::End)
                    return " at the end";
                return " at column " + std::to_string(token.Column + 1);
            }

            Status Lex()
            {
                std::size_t at = 0;
                const std::string_view text = m_text;
                while (at < text.size())
                {
                    const char c = text[at];
                    if (std::isspace(static_cast<unsigned char>(c)))
                    {
                        ++at;
                        continue;
                    }
                    if (c == '-' && at + 1 < text.size() && text[at + 1] == '-')
                        break;
                    Token token;
                    token.Column = at;
                    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '$' && at + 1 < text.size() && IsHexDigit(text[at + 1])) ||
                        (c == '.' && at + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[at + 1]))))
                    {
                        std::size_t end = at + 1;
                        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' || text[end] == '.' ||
                                                     ((text[end] == '+' || text[end] == '-') && (text[end - 1] == 'e' || text[end - 1] == 'E') && IsDecimalFloat(text.substr(at, end - at - 1)))))
                            ++end;
                        token.Kind = TokenKind::Number;
                        token.Text = std::string(text.substr(at, end - at));
                        if (!ParseNumber(token.Text, token.Number))
                            return Fail("\"" + token.Text + "\" is not a number" + Where(token));
                        at = end;
                    }
                    else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                    {
                        std::size_t end = at + 1;
                        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' || text[end] == '.'))
                            ++end;
                        token.Kind = TokenKind::Name;
                        token.Text = std::string(text.substr(at, end - at));
                        at = end;
                    }
                    else if (c == '"' || c == '\'')
                    {
                        const std::size_t close = text.find(c, at + 1);
                        if (close == std::string_view::npos)
                            return Fail("a quote is not closed" + Where(token));
                        token.Kind = TokenKind::String;
                        token.Text = std::string(text.substr(at + 1, close - at - 1));
                        at = close + 1;
                    }
                    else
                    {
                        static constexpr const char* kLong[] = { "==", "~=", "!=", "<=", ">=", "<<", ">>", "//", "&&", "||" };
                        token.Kind = TokenKind::Symbol;
                        for (const char* op : kLong)
                        {
                            if (text.substr(at, 2) == op)
                            {
                                token.Text = op;
                                break;
                            }
                        }
                        if (token.Text.empty())
                        {
                            if (std::strchr("+-*/%&|~^<>()[],!=", c) == nullptr)
                                return Fail(std::string("\"") + c + "\" is not allowed here" + Where(token));
                            token.Text = std::string(1, c);
                        }
                        at += token.Text.size();
                    }
                    m_tokens.push_back(std::move(token));
                }
                Token end;
                end.Column = text.size();
                m_tokens.push_back(end);
                return Status::Success();
            }

            static bool IsDecimalFloat(std::string_view text)
            {
                bool digit = false;
                for (char c : text)
                {
                    if (std::isdigit(static_cast<unsigned char>(c)))
                        digit = true;
                    else if (c != '.')
                        return false;
                }
                return digit;
            }

            static bool ParseNumber(const std::string& text, Value& out)
            {
                std::string_view body = text;
                bool hex = false;
                if (body.front() == '$')
                {
                    body.remove_prefix(1);
                    hex = true;
                }
                else if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
                {
                    body.remove_prefix(2);
                    hex = true;
                }
                if (body.empty())
                    return false;
                bool digits = true;
                bool hexDigits = true;
                bool dot = false;
                bool exponent = false;
                for (std::size_t i = 0; i < body.size(); ++i)
                {
                    const char c = body[i];
                    if (c == '.')
                        dot = true;
                    else if ((c == 'e' || c == 'E') && !hex)
                        exponent = exponent || (i > 0 && i + 1 < body.size());
                    if (!std::isdigit(static_cast<unsigned char>(c)))
                        digits = false;
                    if (!IsHexDigit(c))
                        hexDigits = false;
                }
                if (!hex && digits)
                {
                    std::uint64_t value = 0;
                    const auto result = std::from_chars(body.data(), body.data() + body.size(), value, 10);
                    if (result.ec != std::errc() || result.ptr != body.data() + body.size())
                        return false;
                    out = MakeInt(Wrap(value));
                    return true;
                }
                if (!hex && (dot || exponent) && !hexDigits)
                {
                    const auto value = ParseDouble(body);
                    if (!value)
                        return false;
                    out = MakeFloat(*value);
                    return true;
                }
                if (!hexDigits || body.size() > 16)
                    return false;
                std::uint64_t value = 0;
                const auto result = std::from_chars(body.data(), body.data() + body.size(), value, 16);
                if (result.ec != std::errc() || result.ptr != body.data() + body.size())
                    return false;
                out = MakeInt(Wrap(value));
                return true;
            }

            const Token& Peek() const { return m_tokens[m_at]; }
            const Token& Take() { return m_tokens[m_at < m_tokens.size() - 1 ? m_at++ : m_at]; }

            bool IsSymbol(std::string_view text) const
            {
                return Peek().Kind == TokenKind::Symbol && Peek().Text == text;
            }

            bool IsWord(std::string_view text) const
            {
                return Peek().Kind == TokenKind::Name && EqualsNoCase(Peek().Text, text);
            }

            void Emit(std::uint8_t op) { m_code.push_back(op); }

            void Push(std::uint8_t op)
            {
                Emit(op);
                if (++m_depth > m_deepest)
                    m_deepest = m_depth;
            }

            void EmitConstant(std::int64_t value)
            {
                if (value >= INT32_MIN && value <= INT32_MAX)
                {
                    Push(OpInt32);
                    const std::int32_t small = static_cast<std::int32_t>(value);
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&small);
                    m_code.insert(m_code.end(), bytes, bytes + 4);
                    return;
                }
                Push(OpInt);
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
                m_code.insert(m_code.end(), bytes, bytes + 8);
            }

            void EmitConstant(const Value& value)
            {
                if (!value.IsFloat)
                {
                    EmitConstant(value.Int);
                    return;
                }
                Push(OpFloat);
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value.Float);
                m_code.insert(m_code.end(), bytes, bytes + 8);
            }

            bool Binary(bool (Compiler::*next)(), std::initializer_list<std::pair<const char*, std::uint8_t>> ops)
            {
                if (!(this->*next)())
                    return false;
                for (;;)
                {
                    std::uint8_t found = 0;
                    std::string text;
                    for (const auto& [name, op] : ops)
                    {
                        const bool word = std::isalpha(static_cast<unsigned char>(name[0])) != 0;
                        if ((word && IsWord(name)) || (!word && IsSymbol(name)))
                        {
                            found = op;
                            text = name;
                            break;
                        }
                    }
                    if (!found)
                        return true;
                    Take();
                    if (Peek().Kind == TokenKind::End)
                        return Error("something is missing after \"" + text + "\"");
                    if (!(this->*next)())
                        return false;
                    Emit(found);
                    --m_depth;
                }
            }

            bool Or() { return Binary(&Compiler::And, { { "or", OpOr }, { "||", OpOr } }); }
            bool And() { return Binary(&Compiler::Compare, { { "and", OpAnd }, { "&&", OpAnd } }); }
            bool Compare()
            {
                return Binary(&Compiler::BitOr, { { "==", OpEq }, { "=", OpEq }, { "~=", OpNe }, { "!=", OpNe }, { "<=", OpLe }, { ">=", OpGe }, { "<", OpLt }, { ">", OpGt } });
            }
            bool BitOr() { return Binary(&Compiler::BitXor, { { "|", OpBitOr } }); }
            bool BitXor() { return Binary(&Compiler::BitAnd, { { "~", OpBitXor } }); }
            bool BitAnd() { return Binary(&Compiler::Shift, { { "&", OpBitAnd } }); }
            bool Shift() { return Binary(&Compiler::Sum, { { "<<", OpShl }, { ">>", OpShr } }); }
            bool Sum() { return Binary(&Compiler::Product, { { "+", OpAdd }, { "-", OpSub } }); }
            bool Product() { return Binary(&Compiler::Unary, { { "*", OpMul }, { "//", OpIntDiv }, { "/", OpDiv }, { "%", OpMod } }); }

            bool Unary()
            {
                if (IsWord("not") || IsSymbol("!"))
                {
                    Take();
                    if (!Unary())
                        return false;
                    Emit(OpNot);
                    return true;
                }
                if (IsSymbol("-"))
                {
                    Take();
                    if (Peek().Kind == TokenKind::Number)
                    {
                        Value value = Take().Number;
                        if (value.IsFloat)
                            value.Float = -value.Float;
                        else
                            value.Int = Wrap(0ull - static_cast<std::uint64_t>(value.Int));
                        EmitConstant(value);
                        return true;
                    }
                    if (!Unary())
                        return false;
                    Emit(OpNeg);
                    return true;
                }
                if (IsSymbol("~"))
                {
                    Take();
                    if (!Unary())
                        return false;
                    Emit(OpBitNot);
                    return true;
                }
                if (IsSymbol("^"))
                    return Error("\"^\" (power) is not supported" + Where(Peek()));
                return Primary();
            }

            bool Resolve(const std::string& name, const Token& token)
            {
                std::optional<std::uint64_t> address;
                if (m_resolve)
                    address = m_resolve(name);
                if (!address)
                    return Error("\"" + name + "\" is not a register, function or known symbol" + Where(token));
                EmitConstant(Wrap(*address));
                return true;
            }

            bool Primary()
            {
                const Token token = Take();
                switch (token.Kind)
                {
                case TokenKind::End:
                    return Error("a value is missing at the end");
                case TokenKind::Number:
                    EmitConstant(token.Number);
                    return true;
                case TokenKind::String:
                    return Resolve(token.Text, token);
                case TokenKind::Symbol:
                    if (token.Text == "(")
                    {
                        if (!Or())
                            return false;
                        if (!IsSymbol(")"))
                            return Error("\")\" is missing" + Where(Peek()));
                        Take();
                        return true;
                    }
                    if (token.Text == "[")
                    {
                        if (!Or())
                            return false;
                        if (!IsSymbol("]"))
                            return Error("\"]\" is missing" + Where(Peek()));
                        Take();
                        Emit(OpRead);
                        Emit(m_is64 ? ReadU64 : ReadU32);
                        return true;
                    }
                    return Error("\"" + token.Text + "\" does not belong there" + Where(token));
                case TokenKind::Name:
                    break;
                }
                if (const RegisterName* reg = FindRegister(token.Text))
                {
                    if (reg->Only64 && !m_is64)
                        return Error(token.Text + " does not exist in a 32-bit process" + Where(token));
                    Push(OpRegister);
                    Emit(reg->Index);
                    Emit(m_is64 ? reg->Part : (reg->Part == PartLow32 ? PartFull : reg->Part));
                    return true;
                }
                if (const FlagName* flag = FindFlag(token.Text))
                {
                    Push(OpFlag);
                    Emit(flag->Bit);
                    return true;
                }
                if (EqualsNoCase(token.Text, "THREADID"))
                {
                    Push(OpThread);
                    return true;
                }
                if (EqualsNoCase(token.Text, "true") || EqualsNoCase(token.Text, "false") || EqualsNoCase(token.Text, "nil"))
                {
                    EmitConstant(static_cast<std::int64_t>(EqualsNoCase(token.Text, "true") ? 1 : 0));
                    return true;
                }
                if (!IsSymbol("("))
                    return Resolve(token.Text, token);
                Take();
                if (EqualsNoCase(token.Text, "getAddress") || EqualsNoCase(token.Text, "getAddressSafe"))
                {
                    if (Peek().Kind != TokenKind::String)
                        return Error(token.Text + " needs a name in quotes" + Where(Peek()));
                    const Token name = Take();
                    if (!IsSymbol(")"))
                        return Error("\")\" is missing" + Where(Peek()));
                    Take();
                    return Resolve(name.Text, name);
                }
                const ReadFunction* read = FindRead(token.Text);
                const bool pointer = EqualsNoCase(token.Text, "readPointer");
                if (!read && !pointer)
                    return Error(token.Text + " is not a function conditions know; use readByte, readSmallInteger, readInteger, readQword, readPointer, readFloat or readDouble" + Where(token));
                if (!Or())
                    return false;
                bool isSigned = false;
                if (IsSymbol(","))
                {
                    Take();
                    if (IsWord("true"))
                        isSigned = true;
                    else if (!IsWord("false"))
                        return Error("the second value of " + token.Text + " is true (signed) or false" + Where(Peek()));
                    Take();
                }
                if (!IsSymbol(")"))
                    return Error("\")\" is missing" + Where(Peek()));
                Take();
                Emit(OpRead);
                if (pointer)
                    Emit(m_is64 ? ReadU64 : ReadU32);
                else
                    Emit(isSigned ? read->Signed : read->Unsigned);
                return true;
            }

            std::string m_text;
            bool m_is64 = true;
            const SymbolResolver& m_resolve;
            std::vector<std::uint8_t>& m_code;
            std::vector<Token> m_tokens;
            std::size_t m_at = 0;
            int m_depth = 0;
            int m_deepest = 0;
            Status m_error;
        };

        std::vector<std::string> Statements(std::string_view text)
        {
            std::vector<std::string> out;
            std::string current;
            char quote = 0;
            bool comment = false;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                if (comment)
                {
                    if (c == '\n')
                        comment = false;
                    else
                        continue;
                }
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
                if (c == '-' && i + 1 < text.size() && text[i + 1] == '-')
                {
                    comment = true;
                    continue;
                }
                if (c == ';' || c == '\n' || c == '\r')
                {
                    if (!Trim(current).empty())
                        out.push_back(Trim(current));
                    current.clear();
                    continue;
                }
                current += c;
            }
            if (!Trim(current).empty())
                out.push_back(Trim(current));
            return out;
        }

        struct ProcessReader
        {
            const Target::Process& Process;
            bool operator()(std::uint64_t address, void* out, std::uint32_t size) const { return Process.ReadExact(address, out, size); }
        };
    }

    Status CompileExpression(std::string_view text, bool is64Bit, const SymbolResolver& resolve, std::vector<std::uint8_t>& code)
    {
        code.clear();
        Compiler compiler(text, is64Bit, resolve, code);
        Status status = compiler.Expression();
        if (!status)
            code.clear();
        return status;
    }

    Status CompileChanges(std::string_view text, bool is64Bit, const SymbolResolver& resolve, std::vector<RegisterChange>& changes)
    {
        changes.clear();
        for (const std::string& statement : Statements(text))
        {
            std::size_t equals = std::string::npos;
            for (std::size_t i = 0; i < statement.size(); ++i)
            {
                if (statement[i] != '=')
                    continue;
                const bool joined = (i + 1 < statement.size() && statement[i + 1] == '=') || (i > 0 && std::strchr("=<>~!", statement[i - 1]) != nullptr);
                if (!joined)
                {
                    equals = i;
                    break;
                }
            }
            if (equals == std::string::npos)
                return Status::Fail(ErrorCode::DebugBadExpression, "\"" + statement + "\" is not a change like RAX = 0x10");
            const std::string target = Trim(statement.substr(0, equals));
            RegisterChange change;
            if (const RegisterName* reg = FindRegister(target))
            {
                if (reg->Only64 && !is64Bit)
                    return Status::Fail(ErrorCode::DebugBadExpression, target + " does not exist in a 32-bit process");
                change.Target = reg->Index;
                change.Part = is64Bit ? reg->Part : (reg->Part == PartLow32 ? PartFull : reg->Part);
            }
            else if (const FlagName* flag = FindFlag(target))
            {
                change.Target = static_cast<std::uint8_t>(kFlagTarget + flag->Bit);
            }
            else
            {
                return Status::Fail(ErrorCode::DebugBadExpression, "\"" + target + "\" is not a register or flag that can be changed");
            }
            if (Status compiled = CompileExpression(statement.substr(equals + 1), is64Bit, resolve, change.Code); !compiled)
                return Status::Fail(ErrorCode::DebugBadExpression, target + ": " + compiled.Text);
            changes.push_back(std::move(change));
            if (changes.size() > static_cast<std::size_t>(kMaxChanges))
                return Status::Fail(ErrorCode::DebugBadExpression, "at most " + std::to_string(kMaxChanges) + " registers can be changed at one place");
        }
        return Status::Success();
    }

    Status CompileAction(BreakAction& action, bool is64Bit, const SymbolResolver& resolve)
    {
        action.ConditionCode.clear();
        action.ChangeCode.clear();
        if (!Trim(action.Condition).empty())
        {
            if (Status compiled = CompileExpression(action.Condition, is64Bit, resolve, action.ConditionCode); !compiled)
                return compiled;
        }
        return CompileChanges(action.Changes, is64Bit, resolve, action.ChangeCode);
    }

    RegisterArray ToArray(const Disasm::RegisterValues& r)
    {
        return { r.Rax, r.Rbx, r.Rcx, r.Rdx, r.Rsi, r.Rdi, r.Rbp, r.Rsp, r.R8, r.R9, r.R10, r.R11, r.R12, r.R13, r.R14, r.R15, r.Rip, r.Flags };
    }

    void FromArray(const RegisterArray& v, Disasm::RegisterValues& r)
    {
        r.Rax = v[0]; r.Rbx = v[1]; r.Rcx = v[2]; r.Rdx = v[3]; r.Rsi = v[4]; r.Rdi = v[5]; r.Rbp = v[6]; r.Rsp = v[7];
        r.R8 = v[8]; r.R9 = v[9]; r.R10 = v[10]; r.R11 = v[11]; r.R12 = v[12]; r.R13 = v[13]; r.R14 = v[14]; r.R15 = v[15];
        r.Rip = v[kRip];
        r.Flags = v[kFlags];
    }

    std::optional<Value> Evaluate(const std::vector<std::uint8_t>& code, const Disasm::RegisterValues& registers, std::uint32_t threadId, const Target::Process& process)
    {
        const RegisterArray values = ToArray(registers);
        ProcessReader reader{ process };
        Value result;
        if (!Run(code.data(), static_cast<std::uint32_t>(code.size()), values.data(), threadId, reader, result))
            return std::nullopt;
        return result;
    }

    bool ConditionHolds(const BreakAction& action, const Disasm::RegisterValues& registers, std::uint32_t threadId, const Target::Process& process)
    {
        if (action.ThreadId != 0 && action.ThreadId != threadId)
            return false;
        if (action.ConditionCode.empty())
            return true;
        const auto value = Evaluate(action.ConditionCode, registers, threadId, process);
        return value && Truthy(*value);
    }

    bool ApplyChanges(const BreakAction& action, Disasm::RegisterValues& registers, std::uint32_t threadId, const Target::Process& process)
    {
        if (action.ChangeCode.empty())
            return false;
        RegisterArray values = ToArray(registers);
        const RegisterArray before = values;
        ProcessReader reader{ process };
        bool changed = false;
        std::vector<std::optional<Value>> results;
        for (const RegisterChange& change : action.ChangeCode)
        {
            Value value;
            results.push_back(Run(change.Code.data(), static_cast<std::uint32_t>(change.Code.size()), before.data(), threadId, reader, value) ? std::optional<Value>(value) : std::nullopt);
        }
        for (std::size_t i = 0; i < action.ChangeCode.size(); ++i)
        {
            if (!results[i])
                continue;
            Store(values.data(), action.ChangeCode[i].Target, action.ChangeCode[i].Part, *results[i]);
            changed = true;
        }
        if (changed)
        {
            const std::uint64_t fs = registers.FsBase;
            const std::uint64_t gs = registers.GsBase;
            FromArray(values, registers);
            registers.FsBase = fs;
            registers.GsBase = gs;
        }
        return changed;
    }

    std::string ValueText(const Value& value)
    {
        if (value.IsFloat)
            return Format("%g", value.Float);
        const std::uint64_t raw = static_cast<std::uint64_t>(value.Int);
        if (value.Int >= -9 && value.Int <= 9)
            return std::to_string(value.Int);
        return std::to_string(value.Int) + " (0x" + Hex(raw) + ")";
    }

    std::string DescribeAction(const BreakAction& action)
    {
        std::vector<std::string> parts;
        if (!Trim(action.Condition).empty())
            parts.push_back("if " + Trim(action.Condition));
        if (!Trim(action.Changes).empty())
            parts.push_back("set " + Join(Statements(action.Changes), ", "));
        if (action.ThreadId)
            parts.push_back("thread " + std::to_string(action.ThreadId));
        if (!action.Break)
            parts.push_back("keeps running");
        return Join(parts, "  ·  ");
    }
}
