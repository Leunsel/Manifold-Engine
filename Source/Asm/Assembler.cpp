#include "Asm/Assembler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#pragma warning(push, 0)
#include "Zydis.h"
#pragma warning(pop)

namespace Manifold::Asm
{
    namespace
    {
        constexpr std::uint64_t kMaxNopCount = 0x10000;
        constexpr std::int64_t kBasedDisplacementPlaceholder = 0x12345678;
        constexpr std::int64_t kAbsoluteSentinel64 = -0x7EDCBA98;
        constexpr std::int64_t kAbsoluteSentinel32 = 0x12345678;
        constexpr std::uint64_t kMoffsPlaceholder = 0x123456789ABC;
        constexpr std::uint64_t kNearBranchReach = 0x10000000;
        constexpr std::uint8_t kWaitOpcode = 0x9B;
        constexpr std::uint64_t kFarJumpLength = 14;
        constexpr std::uint64_t kShortJumpLength = 2;
        constexpr std::array<std::uint64_t, 4> kImmediatePlaceholders = { 0x7FFFFFFFFFFFFFFFull, 0x7FFFFFFFull, 0x7FFFull, 0x7Full };
        constexpr std::array<std::uint16_t, 21> kMemorySizeCandidates = { 1, 2, 4, 8, 10, 16, 32, 64, 6, 0, 12, 14, 20, 24, 28, 40, 48, 94, 108, 512, 576 };
        constexpr std::array<std::uint8_t, 6> kFarJumpStub = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        constexpr std::array<std::uint8_t, 8> kFarCallStub = { 0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08 };

        struct Failure
        {
            AsmError Error = AsmError::None;
            std::string Message;
        };

        [[noreturn]] void Fail(AsmError error, std::string message)
        {
            throw Failure{ error, std::move(message) };
        }

        struct Value
        {
            std::uint64_t Number = 0;
            bool Pending = false;
        };

        enum class TokenKind
        {
            Word,
            Number,
            Quoted,
            Punct,
            End
        };

        struct Token
        {
            TokenKind Kind = TokenKind::End;
            std::string Text;
            std::string Lower;
            char Quote = 0;
            std::size_t Begin = 0;
            std::size_t End = 0;
        };

        enum class BranchHint
        {
            None,
            Short,
            Near,
            Far
        };

        enum class OperandKind
        {
            Register,
            Memory,
            Immediate,
            Pointer
        };

        struct Operand
        {
            OperandKind Kind = OperandKind::Immediate;
            ZydisRegister Register = ZYDIS_REGISTER_NONE;
            ZydisRegister Base = ZYDIS_REGISTER_NONE;
            ZydisRegister Index = ZYDIS_REGISTER_NONE;
            std::uint8_t Scale = 0;
            Value Displacement;
            std::uint16_t Size = 0;
            bool SizeWritten = false;
            ZydisRegister Segment = ZYDIS_REGISTER_NONE;
            Value Immediate;
            std::uint64_t PointerSegment = 0;
            std::string Text;
        };

        struct MnemonicInfo
        {
            ZydisMnemonic Mnemonic = ZYDIS_MNEMONIC_INVALID;
            ZydisBranchType BranchType = ZYDIS_BRANCH_TYPE_NONE;
            bool WaitFirst = false;
        };

        struct Instruction
        {
            std::string Text;
            std::string MnemonicText;
            MnemonicInfo Info;
            ZydisInstructionAttributes Prefixes = 0;
            BranchHint Hint = BranchHint::None;
            std::vector<Operand> Operands;
            ZydisRegister Mask = ZYDIS_REGISTER_NONE;
            bool Zeroing = false;
            ZydisBroadcastMode Broadcast = ZYDIS_BROADCAST_MODE_NONE;
            ZydisRoundingMode Rounding = ZYDIS_ROUNDING_MODE_NONE;
            bool Sae = false;
        };

        struct Encoded
        {
            std::array<std::uint8_t, ZYDIS_MAX_INSTRUCTION_LENGTH> Bytes{};
            std::size_t Length = 0;

            bool SameAs(const Encoded& other) const
            {
                return Length == other.Length && std::memcmp(Bytes.data(), other.Bytes.data(), Length) == 0;
            }
        };

        struct SizedEncoding
        {
            std::uint16_t Size = 0;
            Encoded Bytes;
        };

        enum class AbsoluteForm
        {
            None,
            RipRelative,
            Displacement32,
            ZeroExtended32,
            Moffs64
        };

        struct Attempt
        {
            ZydisMnemonic Mnemonic = ZYDIS_MNEMONIC_INVALID;
            const std::vector<Operand>* Operands = nullptr;
            ZydisInstructionAttributes Prefixes = 0;
            std::uint16_t MemorySize = 0;
            bool DefaultMask = false;
            bool SignExtend = false;
            AbsoluteForm Form = AbsoluteForm::None;
            std::size_t Level = 0;
            std::optional<std::size_t> Is4Operand;
        };

        struct MnemonicAlias
        {
            std::string_view Alias;
            std::string_view Target;
            ZydisBranchType BranchType = ZYDIS_BRANCH_TYPE_NONE;
            bool WaitFirst = false;
        };

        constexpr std::array<std::pair<std::string_view, std::string_view>, 14> kConditionAliases = { {
            { "e", "z" },
            { "ne", "nz" },
            { "c", "b" },
            { "nae", "b" },
            { "nc", "nb" },
            { "ae", "nb" },
            { "a", "nbe" },
            { "na", "be" },
            { "g", "nle" },
            { "ge", "nl" },
            { "ng", "le" },
            { "nge", "l" },
            { "pe", "p" },
            { "po", "np" },
        } };

        constexpr std::array<std::string_view, 3> kConditionFamilies = { "j", "cmov", "set" };

        constexpr std::array<MnemonicAlias, 16> kMnemonicAliases = { {
            { "retn", "ret" },
            { "retf", "ret", ZYDIS_BRANCH_TYPE_FAR },
            { "sal", "shl" },
            { "xlatb", "xlat" },
            { "wait", "fwait" },
            { "icebp", "int1" },
            { "movabs", "mov" },
            { "setalc", "salc" },
            { "loopz", "loope" },
            { "loopnz", "loopne" },
            { "fstsw", "fnstsw", ZYDIS_BRANCH_TYPE_NONE, true },
            { "fstcw", "fnstcw", ZYDIS_BRANCH_TYPE_NONE, true },
            { "fstenv", "fnstenv", ZYDIS_BRANCH_TYPE_NONE, true },
            { "fsave", "fnsave", ZYDIS_BRANCH_TYPE_NONE, true },
            { "finit", "fninit", ZYDIS_BRANCH_TYPE_NONE, true },
            { "fclex", "fnclex", ZYDIS_BRANCH_TYPE_NONE, true },
        } };

        std::string Lowered(std::string_view text)
        {
            std::string result(text);
            for (char& c : result)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return result;
        }

        std::string Hex(std::uint64_t value)
        {
            std::array<char, 24> buffer{};
            const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
            std::string result(buffer.data(), converted.ptr);
            for (char& c : result)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return result;
        }

        std::string Quoted(std::string_view text)
        {
            std::string result = "\"";
            result += text;
            result += "\"";
            return result;
        }

        bool IsBlank(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
        }

        bool IsWordCharacter(char c)
        {
            const auto byte = static_cast<unsigned char>(c);
            return std::isalnum(byte) || c == '_' || c == '.' || c == '@' || c == '?' || c == '!' || byte >= 0x80;
        }

        std::string_view Trimmed(std::string_view text)
        {
            while (!text.empty() && IsBlank(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && IsBlank(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        std::string_view WithoutComment(std::string_view line)
        {
            char quote = 0;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (quote != 0)
                {
                    if (c == quote)
                    {
                        quote = 0;
                    }
                }
                else if (c == '"' || c == '\'')
                {
                    quote = c;
                }
                else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                {
                    return line.substr(0, i);
                }
            }
            return line;
        }

        std::vector<Token> Tokenize(std::string_view line)
        {
            std::vector<Token> tokens;
            std::size_t position = 0;
            while (position < line.size())
            {
                const char c = line[position];
                if (IsBlank(c))
                {
                    ++position;
                    continue;
                }
                Token token;
                token.Begin = position;
                if (IsWordCharacter(c))
                {
                    std::size_t end = position;
                    while (end < line.size() && IsWordCharacter(line[end]))
                    {
                        ++end;
                    }
                    token.Kind = TokenKind::Word;
                    token.Text = std::string(line.substr(position, end - position));
                    position = end;
                }
                else if ((c == '$' || c == '#') && position + 1 < line.size() && IsWordCharacter(line[position + 1]))
                {
                    std::size_t end = position + 1;
                    while (end < line.size() && IsWordCharacter(line[end]))
                    {
                        ++end;
                    }
                    token.Kind = TokenKind::Number;
                    token.Text = std::string(line.substr(position, end - position));
                    position = end;
                }
                else if (c == '"' || c == '\'')
                {
                    const std::size_t close = line.find(c, position + 1);
                    if (close == std::string_view::npos)
                    {
                        Fail(AsmError::Syntax, "A quote is not closed.");
                    }
                    token.Kind = TokenKind::Quoted;
                    token.Quote = c;
                    token.Text = std::string(line.substr(position + 1, close - position - 1));
                    position = close + 1;
                }
                else if (std::string_view("[]+-*,:(){}").find(c) != std::string_view::npos)
                {
                    token.Kind = TokenKind::Punct;
                    token.Text = std::string(1, c);
                    ++position;
                }
                else
                {
                    Fail(AsmError::Syntax, "Unexpected character " + Quoted(std::string(1, c)) + ".");
                }
                token.End = position;
                token.Lower = Lowered(token.Text);
                tokens.push_back(std::move(token));
            }
            Token end;
            end.Kind = TokenKind::End;
            end.Begin = line.size();
            end.End = line.size();
            tokens.push_back(std::move(end));
            return tokens;
        }

        bool IsImplicitOnlyRegister(ZydisRegister reg)
        {
            switch (reg)
            {
            case ZYDIS_REGISTER_FLAGS:
            case ZYDIS_REGISTER_EFLAGS:
            case ZYDIS_REGISTER_RFLAGS:
            case ZYDIS_REGISTER_IP:
            case ZYDIS_REGISTER_X87CONTROL:
            case ZYDIS_REGISTER_X87STATUS:
            case ZYDIS_REGISTER_X87TAG:
            case ZYDIS_REGISTER_GDTR:
            case ZYDIS_REGISTER_LDTR:
            case ZYDIS_REGISTER_IDTR:
            case ZYDIS_REGISTER_TR:
            case ZYDIS_REGISTER_MXCSR:
            case ZYDIS_REGISTER_PKRU:
            case ZYDIS_REGISTER_XCR0:
            case ZYDIS_REGISTER_UIF:
            case ZYDIS_REGISTER_IA32_KERNEL_GS_BASE:
            case ZYDIS_REGISTER_BNDCFG:
            case ZYDIS_REGISTER_BNDSTATUS:
                return true;
            default:
                return false;
            }
        }

        const std::unordered_map<std::string, ZydisRegister>& RegisterNames()
        {
            static const std::unordered_map<std::string, ZydisRegister> names = []
            {
                std::unordered_map<std::string, ZydisRegister> table;
                for (int i = ZYDIS_REGISTER_NONE + 1; i <= ZYDIS_REGISTER_MAX_VALUE; ++i)
                {
                    const auto reg = static_cast<ZydisRegister>(i);
                    const char* name = ZydisRegisterGetString(reg);
                    if (name != nullptr && !IsImplicitOnlyRegister(reg))
                    {
                        table.emplace(Lowered(name), reg);
                    }
                }
                table.emplace("st", ZYDIS_REGISTER_ST0);
                return table;
            }();
            return names;
        }

        ZydisRegister FindRegister(const std::string& lower)
        {
            const auto& names = RegisterNames();
            const auto found = names.find(lower);
            return found == names.end() ? ZYDIS_REGISTER_NONE : found->second;
        }

        bool IsSegmentRegister(ZydisRegister reg)
        {
            return ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_SEGMENT;
        }

        bool IsGeneralRegister(ZydisRegister reg)
        {
            switch (ZydisRegisterGetClass(reg))
            {
            case ZYDIS_REGCLASS_GPR8:
            case ZYDIS_REGCLASS_GPR16:
            case ZYDIS_REGCLASS_GPR32:
            case ZYDIS_REGCLASS_GPR64:
                return true;
            default:
                return false;
            }
        }

        bool IsVectorRegister(ZydisRegister reg)
        {
            switch (ZydisRegisterGetClass(reg))
            {
            case ZYDIS_REGCLASS_XMM:
            case ZYDIS_REGCLASS_YMM:
            case ZYDIS_REGCLASS_ZMM:
                return true;
            default:
                return false;
            }
        }

        const std::unordered_map<std::string, MnemonicInfo>& Mnemonics()
        {
            static const std::unordered_map<std::string, MnemonicInfo> table = []
            {
                std::unordered_map<std::string, MnemonicInfo> result;
                for (int i = ZYDIS_MNEMONIC_INVALID + 1; i <= ZYDIS_MNEMONIC_MAX_VALUE; ++i)
                {
                    const auto mnemonic = static_cast<ZydisMnemonic>(i);
                    const char* name = ZydisMnemonicGetString(mnemonic);
                    if (name != nullptr)
                    {
                        result.emplace(Lowered(name), MnemonicInfo{ mnemonic });
                    }
                }
                const auto addAlias = [&result](const std::string& alias, const std::string& target, ZydisBranchType branchType, bool waitFirst)
                {
                    const auto found = result.find(target);
                    if (found != result.end() && result.find(alias) == result.end())
                    {
                        const ZydisMnemonic mnemonic = found->second.Mnemonic;
                        result.emplace(alias, MnemonicInfo{ mnemonic, branchType, waitFirst });
                    }
                };
                for (const std::string_view family : kConditionFamilies)
                {
                    for (const auto& [alias, target] : kConditionAliases)
                    {
                        addAlias(std::string(family) + std::string(alias), std::string(family) + std::string(target), ZYDIS_BRANCH_TYPE_NONE, false);
                    }
                }
                for (const MnemonicAlias& alias : kMnemonicAliases)
                {
                    addAlias(std::string(alias.Alias), std::string(alias.Target), alias.BranchType, alias.WaitFirst);
                }
                return result;
            }();
            return table;
        }

        std::optional<ZydisInstructionAttributes> PrefixWord(std::string_view lower)
        {
            if (lower == "lock")
            {
                return ZYDIS_ATTRIB_HAS_LOCK;
            }
            if (lower == "rep")
            {
                return ZYDIS_ATTRIB_HAS_REP;
            }
            if (lower == "repe" || lower == "repz")
            {
                return ZYDIS_ATTRIB_HAS_REPE;
            }
            if (lower == "repne" || lower == "repnz")
            {
                return ZYDIS_ATTRIB_HAS_REPNE;
            }
            if (lower == "xacquire")
            {
                return ZYDIS_ATTRIB_HAS_XACQUIRE;
            }
            if (lower == "xrelease")
            {
                return ZYDIS_ATTRIB_HAS_XRELEASE;
            }
            if (lower == "bnd")
            {
                return ZYDIS_ATTRIB_HAS_BND;
            }
            if (lower == "notrack")
            {
                return ZYDIS_ATTRIB_HAS_NOTRACK;
            }
            return std::nullopt;
        }

        std::optional<std::uint16_t> SizeWord(std::string_view lower)
        {
            struct SizeWordEntry
            {
                std::string_view Name;
                std::uint16_t Size;
            };
            static constexpr std::array<SizeWordEntry, 13> kSizeWords = { {
                { "byte", 1 },
                { "word", 2 },
                { "dword", 4 },
                { "fword", 6 },
                { "qword", 8 },
                { "mmword", 8 },
                { "tword", 10 },
                { "tbyte", 10 },
                { "xmmword", 16 },
                { "oword", 16 },
                { "dqword", 16 },
                { "ymmword", 32 },
                { "zmmword", 64 },
            } };
            for (const SizeWordEntry& entry : kSizeWords)
            {
                if (lower == entry.Name)
                {
                    return entry.Size;
                }
            }
            return std::nullopt;
        }

        std::string_view SizeName(std::uint64_t size)
        {
            switch (size)
            {
            case 1:
                return "byte";
            case 2:
                return "word";
            case 4:
                return "dword";
            case 6:
                return "fword";
            case 8:
                return "qword";
            case 10:
                return "tword";
            case 16:
                return "xmmword";
            case 32:
                return "ymmword";
            case 64:
                return "zmmword";
            default:
                return {};
            }
        }

        std::optional<std::size_t> DataElementSize(std::string_view lower)
        {
            if (lower == "db")
            {
                return 1;
            }
            if (lower == "dw")
            {
                return 2;
            }
            if (lower == "dd")
            {
                return 4;
            }
            if (lower == "dq")
            {
                return 8;
            }
            return std::nullopt;
        }

        bool IsReservedWord(std::string_view lower)
        {
            return SizeWord(lower).has_value() || PrefixWord(lower).has_value() || lower == "ptr" || lower == "short" || lower == "near" || lower == "far";
        }

        bool FitsSigned32(std::uint64_t value)
        {
            const auto signedValue = static_cast<std::int64_t>(value);
            return signedValue >= std::numeric_limits<std::int32_t>::min() && signedValue <= std::numeric_limits<std::int32_t>::max();
        }

        bool FitsAddress32(std::uint64_t value)
        {
            return value <= 0xFFFFFFFFull || FitsSigned32(value);
        }

        std::int64_t SignExtended32(std::uint64_t value)
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
        }

        std::uint64_t SignExtendedFrom(std::uint64_t value, unsigned bits)
        {
            if (bits == 0 || bits >= 64 || value >= (1ull << bits))
            {
                return value;
            }
            const std::uint64_t sign = 1ull << (bits - 1);
            return (value & sign) != 0 ? (value | ~((1ull << bits) - 1)) : value;
        }

        bool FitsElement(std::uint64_t value, std::size_t size)
        {
            if (size >= 8)
            {
                return true;
            }
            const unsigned bits = static_cast<unsigned>(size * 8);
            const auto signedValue = static_cast<std::int64_t>(value);
            return value < (1ull << bits) || (signedValue < 0 && signedValue >= -static_cast<std::int64_t>(1ull << (bits - 1)));
        }

        void AppendLittleEndian(std::vector<std::uint8_t>& bytes, std::uint64_t value, std::size_t size)
        {
            for (std::size_t i = 0; i < size; ++i)
            {
                bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
            }
        }

        std::vector<std::uint8_t> ToVector(const Encoded& encoded)
        {
            return std::vector<std::uint8_t>(encoded.Bytes.data(), encoded.Bytes.data() + encoded.Length);
        }

        ZydisMachineMode MachineMode(const AssembleOptions& options)
        {
            return options.Is64Bit ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32;
        }

        struct DecodedEncoding
        {
            ZydisDecodedInstruction Instruction;
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> Operands;
        };

        bool Decode(const Encoded& encoded, const AssembleOptions& options, DecodedEncoding& decoded)
        {
            ZydisDecoder decoder;
            if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, MachineMode(options), options.Is64Bit ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32)))
            {
                return false;
            }
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, encoded.Bytes.data(), encoded.Length, &decoded.Instruction, decoded.Operands.data())))
            {
                return false;
            }
            return decoded.Instruction.length == encoded.Length;
        }

        bool MemorySizesMatch(const ZydisEncoderRequest& request, const DecodedEncoding& decoded)
        {
            std::size_t next = 0;
            const std::size_t visible = decoded.Instruction.operand_count_visible;
            for (std::size_t i = 0; i < request.operand_count; ++i)
            {
                const ZydisEncoderOperand& requested = request.operands[i];
                if (requested.type != ZYDIS_OPERAND_TYPE_MEMORY)
                {
                    continue;
                }
                while (next < visible && decoded.Operands[next].type != ZYDIS_OPERAND_TYPE_MEMORY)
                {
                    ++next;
                }
                if (next == visible || decoded.Operands[next].size != requested.mem.size * 8u)
                {
                    return false;
                }
                ++next;
            }
            return true;
        }

        bool PatchAbsoluteDisplacement(Encoded& encoded, const DecodedEncoding& decoded, std::uint32_t displacement)
        {
            const auto& disp = decoded.Instruction.raw.disp;
            if (disp.size != 32)
            {
                return false;
            }
            for (std::size_t i = 0; i < 4; ++i)
            {
                encoded.Bytes[disp.offset + i] = static_cast<std::uint8_t>(displacement >> (8 * i));
            }
            return true;
        }

        bool IsSystemRegister(ZydisRegister reg)
        {
            switch (ZydisRegisterGetClass(reg))
            {
            case ZYDIS_REGCLASS_CONTROL:
            case ZYDIS_REGCLASS_DEBUG:
            case ZYDIS_REGCLASS_TEST:
                return true;
            default:
                return false;
            }
        }

        void PreferConventionalForm(Encoded& encoded, const DecodedEncoding& decoded, bool movesSystemRegister)
        {
            const ZydisDecodedInstruction& instruction = decoded.Instruction;
            const std::size_t modrm = instruction.raw.modrm.offset;
            if (instruction.opcode_map != ZYDIS_OPCODE_MAP_0F || modrm == 0)
            {
                return;
            }
            const bool hintNop = instruction.opcode == 0x0D || (instruction.opcode >= 0x18 && instruction.opcode <= 0x1E);
            if (instruction.mnemonic == ZYDIS_MNEMONIC_NOP && hintNop)
            {
                if (instruction.opcode == 0x18)
                {
                    encoded.Bytes[modrm] = static_cast<std::uint8_t>(encoded.Bytes[modrm] & 0xC7);
                }
                encoded.Bytes[modrm - 1] = 0x1F;
            }
            else if (movesSystemRegister && instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.opcode >= 0x20 && instruction.opcode <= 0x26)
            {
                encoded.Bytes[modrm] = static_cast<std::uint8_t>(encoded.Bytes[modrm] | 0xC0);
            }
        }

        bool IsAbsoluteMemory(const Operand& operand)
        {
            return operand.Kind == OperandKind::Memory && operand.Base == ZYDIS_REGISTER_NONE && operand.Index == ZYDIS_REGISTER_NONE;
        }

        bool UsesAddressOnly(ZydisMnemonic mnemonic)
        {
            switch (mnemonic)
            {
            case ZYDIS_MNEMONIC_LEA:
            case ZYDIS_MNEMONIC_BNDMK:
            case ZYDIS_MNEMONIC_BNDCL:
            case ZYDIS_MNEMONIC_BNDCU:
            case ZYDIS_MNEMONIC_BNDCN:
                return true;
            default:
                return false;
            }
        }

        bool IsShiftOrRotate(ZydisMnemonic mnemonic)
        {
            switch (mnemonic)
            {
            case ZYDIS_MNEMONIC_RCL:
            case ZYDIS_MNEMONIC_RCR:
            case ZYDIS_MNEMONIC_ROL:
            case ZYDIS_MNEMONIC_ROR:
            case ZYDIS_MNEMONIC_SHL:
            case ZYDIS_MNEMONIC_SHR:
            case ZYDIS_MNEMONIC_SAR:
            case ZYDIS_MNEMONIC_SHLD:
            case ZYDIS_MNEMONIC_SHRD:
                return true;
            default:
                return false;
            }
        }

        std::optional<ZydisMnemonic> InvertedCondition(ZydisMnemonic mnemonic)
        {
            static constexpr std::array<std::pair<ZydisMnemonic, ZydisMnemonic>, 8> kPairs = { {
                { ZYDIS_MNEMONIC_JB, ZYDIS_MNEMONIC_JNB },
                { ZYDIS_MNEMONIC_JBE, ZYDIS_MNEMONIC_JNBE },
                { ZYDIS_MNEMONIC_JL, ZYDIS_MNEMONIC_JNL },
                { ZYDIS_MNEMONIC_JLE, ZYDIS_MNEMONIC_JNLE },
                { ZYDIS_MNEMONIC_JO, ZYDIS_MNEMONIC_JNO },
                { ZYDIS_MNEMONIC_JP, ZYDIS_MNEMONIC_JNP },
                { ZYDIS_MNEMONIC_JS, ZYDIS_MNEMONIC_JNS },
                { ZYDIS_MNEMONIC_JZ, ZYDIS_MNEMONIC_JNZ },
            } };
            for (const auto& [first, second] : kPairs)
            {
                if (mnemonic == first)
                {
                    return second;
                }
                if (mnemonic == second)
                {
                    return first;
                }
            }
            return std::nullopt;
        }

        bool IsCounterBranch(ZydisMnemonic mnemonic)
        {
            switch (mnemonic)
            {
            case ZYDIS_MNEMONIC_JCXZ:
            case ZYDIS_MNEMONIC_JECXZ:
            case ZYDIS_MNEMONIC_JRCXZ:
            case ZYDIS_MNEMONIC_LOOP:
            case ZYDIS_MNEMONIC_LOOPE:
            case ZYDIS_MNEMONIC_LOOPNE:
                return true;
            default:
                return false;
            }
        }

        bool IsRelativeBranch(ZydisMnemonic mnemonic)
        {
            return mnemonic == ZYDIS_MNEMONIC_JMP || mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_XBEGIN || InvertedCondition(mnemonic).has_value() || IsCounterBranch(mnemonic);
        }

        bool CanUseFarForm(ZydisMnemonic mnemonic)
        {
            return mnemonic == ZYDIS_MNEMONIC_JMP || mnemonic == ZYDIS_MNEMONIC_CALL || InvertedCondition(mnemonic).has_value();
        }

        std::optional<ZydisMnemonic> PoppingForm(ZydisMnemonic mnemonic)
        {
            switch (mnemonic)
            {
            case ZYDIS_MNEMONIC_FADD:
                return ZYDIS_MNEMONIC_FADDP;
            case ZYDIS_MNEMONIC_FSUB:
                return ZYDIS_MNEMONIC_FSUBP;
            case ZYDIS_MNEMONIC_FSUBR:
                return ZYDIS_MNEMONIC_FSUBRP;
            case ZYDIS_MNEMONIC_FMUL:
                return ZYDIS_MNEMONIC_FMULP;
            case ZYDIS_MNEMONIC_FDIV:
                return ZYDIS_MNEMONIC_FDIVP;
            case ZYDIS_MNEMONIC_FDIVR:
                return ZYDIS_MNEMONIC_FDIVRP;
            default:
                return std::nullopt;
            }
        }

        ZydisInstructionAttributes SegmentPrefix(ZydisRegister segment, bool is64Bit)
        {
            switch (segment)
            {
            case ZYDIS_REGISTER_FS:
                return ZYDIS_ATTRIB_HAS_SEGMENT_FS;
            case ZYDIS_REGISTER_GS:
                return ZYDIS_ATTRIB_HAS_SEGMENT_GS;
            case ZYDIS_REGISTER_ES:
                return is64Bit ? 0 : ZYDIS_ATTRIB_HAS_SEGMENT_ES;
            case ZYDIS_REGISTER_CS:
                return is64Bit ? 0 : ZYDIS_ATTRIB_HAS_SEGMENT_CS;
            case ZYDIS_REGISTER_SS:
                return is64Bit ? 0 : ZYDIS_ATTRIB_HAS_SEGMENT_SS;
            case ZYDIS_REGISTER_DS:
                return is64Bit ? 0 : ZYDIS_ATTRIB_HAS_SEGMENT_DS;
            default:
                return 0;
            }
        }

        std::uint16_t EffectiveMemorySize(const Operand& operand, ZydisMnemonic mnemonic, std::uint16_t candidate)
        {
            return operand.SizeWritten && !UsesAddressOnly(mnemonic) ? operand.Size : candidate;
        }

        unsigned OperationWidth(const std::vector<Operand>& operands, ZydisMnemonic mnemonic, std::uint16_t memorySize, const AssembleOptions& options)
        {
            unsigned width = 0;
            for (const Operand& operand : operands)
            {
                if (operand.Kind == OperandKind::Register && IsGeneralRegister(operand.Register))
                {
                    width = std::max<unsigned>(width, ZydisRegisterGetWidth(MachineMode(options), operand.Register));
                }
                else if (operand.Kind == OperandKind::Memory)
                {
                    width = std::max<unsigned>(width, EffectiveMemorySize(operand, mnemonic, memorySize) * 8u);
                }
            }
            if (width == 0)
            {
                return options.Is64Bit ? 64u : 32u;
            }
            return std::min(width, 64u);
        }

        bool HasSignExtendableImmediate(const std::vector<Operand>& operands, ZydisMnemonic mnemonic, std::uint16_t memorySize, const AssembleOptions& options)
        {
            const unsigned width = OperationWidth(operands, mnemonic, memorySize, options);
            for (const Operand& operand : operands)
            {
                if (operand.Kind == OperandKind::Immediate && !operand.Immediate.Pending && SignExtendedFrom(operand.Immediate.Number, width) != operand.Immediate.Number)
                {
                    return true;
                }
            }
            return false;
        }

        bool HasPendingImmediate(const std::vector<Operand>& operands)
        {
            return std::any_of(operands.begin(), operands.end(), [](const Operand& operand)
            {
                return operand.Kind == OperandKind::Immediate && operand.Immediate.Pending;
            });
        }

        bool MayNeedDefaultMask(const std::vector<Operand>& operands)
        {
            return std::any_of(operands.begin(), operands.end(), [](const Operand& operand)
            {
                return operand.Kind == OperandKind::Register && (IsVectorRegister(operand.Register) || ZydisRegisterGetClass(operand.Register) == ZYDIS_REGCLASS_MASK);
            });
        }

        const Operand* FindAbsoluteMemory(const std::vector<Operand>& operands)
        {
            for (const Operand& operand : operands)
            {
                if (IsAbsoluteMemory(operand))
                {
                    return &operand;
                }
            }
            return nullptr;
        }

        std::vector<AbsoluteForm> AbsoluteForms(const std::vector<Operand>& operands, const AssembleOptions& options)
        {
            const Operand* absolute = FindAbsoluteMemory(operands);
            if (absolute == nullptr)
            {
                return { AbsoluteForm::None };
            }
            const Value& address = absolute->Displacement;
            if (!options.Is64Bit)
            {
                if (address.Pending || FitsAddress32(address.Number))
                {
                    return { AbsoluteForm::Displacement32 };
                }
                return {};
            }
            std::vector<AbsoluteForm> forms;
            if (absolute->Segment != ZYDIS_REGISTER_FS && absolute->Segment != ZYDIS_REGISTER_GS)
            {
                forms.push_back(AbsoluteForm::RipRelative);
            }
            if (address.Pending || FitsSigned32(address.Number))
            {
                forms.push_back(AbsoluteForm::Displacement32);
            }
            if (address.Pending || (!FitsSigned32(address.Number) && address.Number <= 0xFFFFFFFFull))
            {
                forms.push_back(AbsoluteForm::ZeroExtended32);
            }
            if (address.Pending || !FitsSigned32(address.Number))
            {
                forms.push_back(AbsoluteForm::Moffs64);
            }
            return forms;
        }

        std::uint16_t AddressOperandSize(const Operand& operand, AbsoluteForm form, const AssembleOptions& options)
        {
            const ZydisRegister reg = operand.Base != ZYDIS_REGISTER_NONE ? operand.Base : operand.Index;
            if (reg != ZYDIS_REGISTER_NONE)
            {
                return static_cast<std::uint16_t>(ZydisRegisterGetWidth(MachineMode(options), reg) / 8);
            }
            if (form == AbsoluteForm::ZeroExtended32 || !options.Is64Bit)
            {
                return 4;
            }
            return 8;
        }

        struct MemoryEncoding
        {
            bool UseAbsoluteEncoder = false;
            bool AddressSize32 = false;
            std::optional<std::uint32_t> Patch;
        };

        void FillMemoryOperand(ZydisEncoderOperand& target, const Operand& operand, const Attempt& attempt, const AssembleOptions& options, MemoryEncoding& encoding)
        {
            target.type = ZYDIS_OPERAND_TYPE_MEMORY;
            if (UsesAddressOnly(attempt.Mnemonic))
            {
                target.mem.size = AddressOperandSize(operand, attempt.Form, options);
            }
            else
            {
                target.mem.size = EffectiveMemorySize(operand, attempt.Mnemonic, attempt.MemorySize);
            }
            const Value& displacement = operand.Displacement;
            if (!IsAbsoluteMemory(operand))
            {
                target.mem.base = operand.Base;
                target.mem.index = operand.Index;
                const bool boundTable = attempt.Mnemonic == ZYDIS_MNEMONIC_BNDLDX || attempt.Mnemonic == ZYDIS_MNEMONIC_BNDSTX;
                const bool scaled = operand.Index != ZYDIS_REGISTER_NONE && !(boundTable && operand.Scale == 1);
                target.mem.scale = scaled ? operand.Scale : 0;
                if (displacement.Pending)
                {
                    target.mem.displacement = kBasedDisplacementPlaceholder;
                }
                else if (!options.Is64Bit && displacement.Number <= 0xFFFFFFFFull)
                {
                    target.mem.displacement = SignExtended32(displacement.Number);
                }
                else
                {
                    target.mem.displacement = static_cast<std::int64_t>(displacement.Number);
                }
                return;
            }
            switch (attempt.Form)
            {
            case AbsoluteForm::RipRelative:
                target.mem.base = ZYDIS_REGISTER_RIP;
                target.mem.displacement = static_cast<std::int64_t>(displacement.Pending ? options.Address : displacement.Number);
                encoding.UseAbsoluteEncoder = true;
                break;
            case AbsoluteForm::Displacement32:
                target.mem.displacement = options.Is64Bit ? kAbsoluteSentinel64 : kAbsoluteSentinel32;
                encoding.Patch = static_cast<std::uint32_t>(displacement.Pending ? 0 : displacement.Number);
                break;
            case AbsoluteForm::ZeroExtended32:
                target.mem.displacement = kAbsoluteSentinel64;
                encoding.AddressSize32 = true;
                encoding.Patch = static_cast<std::uint32_t>(displacement.Pending ? 0 : displacement.Number);
                break;
            case AbsoluteForm::Moffs64:
                target.mem.displacement = static_cast<std::int64_t>(displacement.Pending ? kMoffsPlaceholder : displacement.Number);
                break;
            case AbsoluteForm::None:
                break;
            }
        }

        std::optional<Encoded> EncodeAttempt(const Instruction& instruction, const Attempt& attempt, const AssembleOptions& options)
        {
            const std::vector<Operand>& operands = *attempt.Operands;
            const ZydisRegister mask = instruction.Mask != ZYDIS_REGISTER_NONE ? instruction.Mask : (attempt.DefaultMask ? ZYDIS_REGISTER_K0 : ZYDIS_REGISTER_NONE);
            const std::size_t total = operands.size() + (mask != ZYDIS_REGISTER_NONE ? 1 : 0);
            if (total > ZYDIS_ENCODER_MAX_OPERANDS || (mask != ZYDIS_REGISTER_NONE && operands.empty()))
            {
                return std::nullopt;
            }
            ZydisEncoderRequest request;
            std::memset(&request, 0, sizeof(request));
            request.machine_mode = MachineMode(options);
            request.mnemonic = attempt.Mnemonic;
            request.prefixes = attempt.Prefixes;
            request.branch_type = instruction.Hint == BranchHint::Far ? ZYDIS_BRANCH_TYPE_FAR : instruction.Info.BranchType;
            request.evex.zeroing_mask = instruction.Zeroing ? ZYAN_TRUE : ZYAN_FALSE;
            request.evex.broadcast = instruction.Broadcast;
            request.evex.rounding = instruction.Rounding;
            request.evex.sae = instruction.Sae ? ZYAN_TRUE : ZYAN_FALSE;
            const unsigned width = attempt.SignExtend ? OperationWidth(operands, attempt.Mnemonic, attempt.MemorySize, options) : 64u;
            MemoryEncoding memory;
            std::size_t slot = 0;
            for (std::size_t index = 0; index < operands.size(); ++index)
            {
                const Operand& operand = operands[index];
                ZydisEncoderOperand& target = request.operands[slot++];
                switch (operand.Kind)
                {
                case OperandKind::Register:
                    target.type = ZYDIS_OPERAND_TYPE_REGISTER;
                    target.reg.value = operand.Register;
                    target.reg.is4 = attempt.Is4Operand == index ? ZYAN_TRUE : ZYAN_FALSE;
                    break;
                case OperandKind::Pointer:
                    target.type = ZYDIS_OPERAND_TYPE_POINTER;
                    target.ptr.segment = static_cast<ZyanU16>(operand.PointerSegment);
                    target.ptr.offset = static_cast<ZyanU32>(operand.Immediate.Pending ? 0 : operand.Immediate.Number);
                    break;
                case OperandKind::Immediate:
                {
                    target.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
                    const std::uint64_t value = operand.Immediate.Pending ? kImmediatePlaceholders[attempt.Level] : operand.Immediate.Number;
                    target.imm.u = attempt.SignExtend ? SignExtendedFrom(value, width) : value;
                    break;
                }
                case OperandKind::Memory:
                    request.prefixes |= SegmentPrefix(operand.Segment, options.Is64Bit);
                    FillMemoryOperand(target, operand, attempt, options, memory);
                    break;
                }
                if (slot == 1 && mask != ZYDIS_REGISTER_NONE)
                {
                    ZydisEncoderOperand& maskOperand = request.operands[slot++];
                    maskOperand.type = ZYDIS_OPERAND_TYPE_REGISTER;
                    maskOperand.reg.value = mask;
                }
            }
            request.operand_count = static_cast<ZyanU8>(slot);
            if (memory.AddressSize32)
            {
                request.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_32;
            }
            Encoded encoded;
            ZyanUSize length = encoded.Bytes.size();
            const ZyanStatus status = memory.UseAbsoluteEncoder
                ? ZydisEncoderEncodeInstructionAbsolute(&request, encoded.Bytes.data(), &length, options.Address)
                : ZydisEncoderEncodeInstruction(&request, encoded.Bytes.data(), &length);
            if (!ZYAN_SUCCESS(status))
            {
                return std::nullopt;
            }
            encoded.Length = length;
            DecodedEncoding decoded;
            if (!Decode(encoded, options, decoded))
            {
                return std::nullopt;
            }
            const bool sizeChecked = !UsesAddressOnly(attempt.Mnemonic) && instruction.Broadcast == ZYDIS_BROADCAST_MODE_NONE;
            if (sizeChecked && !MemorySizesMatch(request, decoded))
            {
                return std::nullopt;
            }
            if (memory.Patch && !PatchAbsoluteDisplacement(encoded, decoded, *memory.Patch))
            {
                return std::nullopt;
            }
            const bool movesSystemRegister = std::any_of(operands.begin(), operands.end(), [](const Operand& operand)
            {
                return operand.Kind == OperandKind::Register && IsSystemRegister(operand.Register);
            });
            PreferConventionalForm(encoded, decoded, movesSystemRegister);
            return encoded;
        }

        std::optional<Encoded> EncodeBestForm(const Instruction& instruction, Attempt attempt, const AssembleOptions& options)
        {
            const std::vector<Operand>& operands = *attempt.Operands;
            const std::vector<AbsoluteForm> forms = AbsoluteForms(operands, options);
            const Operand* absolute = FindAbsoluteMemory(operands);
            const bool pendingAddress = absolute != nullptr && absolute->Displacement.Pending;
            const std::size_t levels = HasPendingImmediate(operands) ? kImmediatePlaceholders.size() : 1;
            std::optional<Encoded> best;
            for (const AbsoluteForm form : forms)
            {
                attempt.Form = form;
                std::optional<Encoded> bestForForm;
                for (std::size_t level = 0; level < levels; ++level)
                {
                    attempt.Level = level;
                    std::optional<Encoded> encoded = EncodeAttempt(instruction, attempt, options);
                    if (encoded && (!bestForForm || encoded->Length > bestForForm->Length))
                    {
                        bestForForm = encoded;
                    }
                }
                if (!bestForForm)
                {
                    continue;
                }
                if (!pendingAddress)
                {
                    return bestForForm;
                }
                if (!best || bestForForm->Length > best->Length)
                {
                    best = bestForForm;
                }
            }
            return best;
        }

        Operand StackRegister(ZydisRegister reg)
        {
            Operand operand;
            operand.Kind = OperandKind::Register;
            operand.Register = reg;
            operand.Text = Lowered(ZydisRegisterGetString(reg));
            return operand;
        }

        bool IsX87Register(const Operand& operand)
        {
            return operand.Kind == OperandKind::Register && ZydisRegisterGetClass(operand.Register) == ZYDIS_REGCLASS_X87;
        }

        std::vector<ZydisMnemonic> MnemonicVariants(const Instruction& instruction)
        {
            const ZydisMnemonic mnemonic = instruction.Info.Mnemonic;
            if (mnemonic == ZYDIS_MNEMONIC_MOVSX)
            {
                return { ZYDIS_MNEMONIC_MOVSX, ZYDIS_MNEMONIC_MOVSXD };
            }
            if (instruction.Operands.empty())
            {
                if (const auto popping = PoppingForm(mnemonic))
                {
                    return { *popping, mnemonic };
                }
            }
            return { mnemonic };
        }

        std::vector<std::vector<Operand>> OperandVariants(const Instruction& instruction)
        {
            const std::vector<Operand>& operands = instruction.Operands;
            std::vector<std::vector<Operand>> variants{ operands };
            const char* name = ZydisMnemonicGetString(instruction.Info.Mnemonic);
            if (name == nullptr || name[0] != 'f' || !std::all_of(operands.begin(), operands.end(), IsX87Register))
            {
                return variants;
            }
            const Operand top = StackRegister(ZYDIS_REGISTER_ST0);
            if (operands.empty())
            {
                const Operand next = StackRegister(ZYDIS_REGISTER_ST1);
                variants.push_back({ next });
                variants.push_back({ next, top });
                variants.push_back({ top, next });
            }
            else if (operands.size() == 1)
            {
                variants.push_back({ top, operands[0] });
                variants.push_back({ operands[0], top });
            }
            else if (operands.size() == 2 && operands[0].Register == ZYDIS_REGISTER_ST0)
            {
                variants.push_back({ operands[1] });
            }
            else if (operands.size() == 2 && operands[1].Register == ZYDIS_REGISTER_ST0)
            {
                variants.push_back({ operands[0] });
            }
            return variants;
        }

        std::vector<ZydisInstructionAttributes> PrefixVariants(ZydisInstructionAttributes prefixes)
        {
            std::vector<ZydisInstructionAttributes> variants{ prefixes };
            if ((prefixes & ZYDIS_ATTRIB_HAS_REP) != 0)
            {
                variants.push_back((prefixes & ~ZYDIS_ATTRIB_HAS_REP) | ZYDIS_ATTRIB_HAS_REPE);
            }
            else if ((prefixes & ZYDIS_ATTRIB_HAS_REPE) != 0)
            {
                variants.push_back((prefixes & ~ZYDIS_ATTRIB_HAS_REPE) | ZYDIS_ATTRIB_HAS_REP);
            }
            return variants;
        }

        std::vector<std::optional<std::size_t>> Is4Choices(const std::vector<Operand>& operands)
        {
            std::vector<std::optional<std::size_t>> choices{ std::nullopt };
            if (operands.size() < 4)
            {
                return choices;
            }
            for (const std::size_t index : { std::size_t{ 3 }, std::size_t{ 2 } })
            {
                if (operands[index].Kind == OperandKind::Register && IsVectorRegister(operands[index].Register))
                {
                    choices.push_back(index);
                }
            }
            return choices;
        }

        bool NeedsSizeSearch(const Instruction& instruction)
        {
            return std::any_of(instruction.Operands.begin(), instruction.Operands.end(), [&instruction](const Operand& operand)
            {
                return operand.Kind == OperandKind::Memory && !operand.SizeWritten && !UsesAddressOnly(instruction.Info.Mnemonic);
            });
        }

        std::vector<SizedEncoding> CollectEncodings(const Instruction& instruction, const AssembleOptions& options)
        {
            const std::vector<ZydisMnemonic> mnemonics = MnemonicVariants(instruction);
            const std::vector<std::vector<Operand>> operandVariants = OperandVariants(instruction);
            const std::vector<ZydisInstructionAttributes> prefixVariants = PrefixVariants(instruction.Prefixes);
            std::vector<std::uint16_t> sizes{ 0 };
            if (NeedsSizeSearch(instruction))
            {
                sizes.assign(kMemorySizeCandidates.begin(), kMemorySizeCandidates.end());
            }
            const auto firstEncoding = [&](std::uint16_t size) -> std::optional<Encoded>
            {
                for (const ZydisMnemonic mnemonic : mnemonics)
                {
                    for (const std::vector<Operand>& operands : operandVariants)
                    {
                        const bool tryDefaultMask = instruction.Mask == ZYDIS_REGISTER_NONE && MayNeedDefaultMask(operands);
                        const bool trySignExtend = HasSignExtendableImmediate(operands, mnemonic, size, options);
                        const std::vector<std::optional<std::size_t>> is4Choices = Is4Choices(operands);
                        for (const ZydisInstructionAttributes prefixes : prefixVariants)
                        {
                            for (const std::optional<std::size_t>& is4 : is4Choices)
                            {
                                for (const bool defaultMask : { false, true })
                                {
                                    if (defaultMask && !tryDefaultMask)
                                    {
                                        continue;
                                    }
                                    for (const bool signExtend : { false, true })
                                    {
                                        if (signExtend && !trySignExtend)
                                        {
                                            continue;
                                        }
                                        Attempt attempt;
                                        attempt.Mnemonic = mnemonic;
                                        attempt.Operands = &operands;
                                        attempt.Prefixes = prefixes;
                                        attempt.MemorySize = size;
                                        attempt.DefaultMask = defaultMask;
                                        attempt.SignExtend = signExtend;
                                        attempt.Is4Operand = is4;
                                        if (std::optional<Encoded> encoded = EncodeBestForm(instruction, attempt, options))
                                        {
                                            return encoded;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                return std::nullopt;
            };
            std::vector<SizedEncoding> results;
            for (const std::uint16_t size : sizes)
            {
                if (std::optional<Encoded> encoded = firstEncoding(size))
                {
                    results.push_back(SizedEncoding{ size, *encoded });
                }
            }
            return results;
        }

        std::optional<std::uint16_t> PreferredMemorySize(const Instruction& instruction, const AssembleOptions& options)
        {
            switch (instruction.Info.Mnemonic)
            {
            case ZYDIS_MNEMONIC_PUSH:
            case ZYDIS_MNEMONIC_POP:
                return static_cast<std::uint16_t>(options.Is64Bit ? 8 : 4);
            case ZYDIS_MNEMONIC_JMP:
            case ZYDIS_MNEMONIC_CALL:
                if (instruction.Hint == BranchHint::Far)
                {
                    return static_cast<std::uint16_t>(6);
                }
                return static_cast<std::uint16_t>(options.Is64Bit ? 8 : 4);
            case ZYDIS_MNEMONIC_NOP:
                return static_cast<std::uint16_t>(4);
            default:
                return std::nullopt;
            }
        }

        std::vector<std::uint16_t> RegisterOperandSizes(const Instruction& instruction)
        {
            std::vector<std::uint16_t> sizes;
            for (const Operand& operand : instruction.Operands)
            {
                if (operand.Kind != OperandKind::Register)
                {
                    continue;
                }
                if (operand.Register == ZYDIS_REGISTER_CL && IsShiftOrRotate(instruction.Info.Mnemonic))
                {
                    continue;
                }
                const ZydisRegisterClass registerClass = ZydisRegisterGetClass(operand.Register);
                if (IsGeneralRegister(operand.Register) || IsVectorRegister(operand.Register) || registerClass == ZYDIS_REGCLASS_MMX)
                {
                    sizes.push_back(static_cast<std::uint16_t>(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, operand.Register) / 8));
                }
            }
            return sizes;
        }

        std::string SizeChoices(const std::vector<SizedEncoding>& results)
        {
            std::vector<std::uint16_t> sizes;
            for (const SizedEncoding& result : results)
            {
                if (!SizeName(result.Size).empty() && std::find(sizes.begin(), sizes.end(), result.Size) == sizes.end())
                {
                    sizes.push_back(result.Size);
                }
            }
            std::sort(sizes.begin(), sizes.end());
            if (sizes.empty())
            {
                return "a size such as dword ptr";
            }
            std::string text;
            for (std::size_t i = 0; i < sizes.size(); ++i)
            {
                if (i > 0)
                {
                    text += i + 1 == sizes.size() ? " or " : ", ";
                }
                text += SizeName(sizes[i]);
                text += " ptr";
            }
            return text;
        }

        Encoded ChooseEncoding(const Instruction& instruction, const std::vector<SizedEncoding>& results, const AssembleOptions& options)
        {
            const Encoded& first = results.front().Bytes;
            const bool allSame = std::all_of(results.begin(), results.end(), [&first](const SizedEncoding& result)
            {
                return result.Bytes.SameAs(first);
            });
            if (allSame)
            {
                return first;
            }
            if (const auto preferred = PreferredMemorySize(instruction, options))
            {
                for (const SizedEncoding& result : results)
                {
                    if (result.Size == *preferred)
                    {
                        return result.Bytes;
                    }
                }
            }
            const bool anyNamed = std::any_of(results.begin(), results.end(), [](const SizedEncoding& result)
            {
                return !SizeName(result.Size).empty();
            });
            if (!anyNamed)
            {
                const auto shortest = std::min_element(results.begin(), results.end(), [](const SizedEncoding& left, const SizedEncoding& right)
                {
                    return left.Bytes.Length < right.Bytes.Length;
                });
                return shortest->Bytes;
            }
            const std::vector<std::uint16_t> registerSizes = RegisterOperandSizes(instruction);
            const Encoded* byRegister = nullptr;
            bool conflicting = false;
            for (const SizedEncoding& result : results)
            {
                if (std::find(registerSizes.begin(), registerSizes.end(), result.Size) == registerSizes.end())
                {
                    continue;
                }
                if (byRegister == nullptr)
                {
                    byRegister = &result.Bytes;
                }
                else if (!byRegister->SameAs(result.Bytes))
                {
                    conflicting = true;
                }
            }
            if (byRegister != nullptr && !conflicting)
            {
                return *byRegister;
            }
            if (options.DefaultToDword)
            {
                for (const SizedEncoding& result : results)
                {
                    if (result.Size == 4)
                    {
                        return result.Bytes;
                    }
                }
            }
            Fail(AsmError::BadOperand, Quoted(instruction.Text) + " needs a size: write " + SizeChoices(results) + ".");
        }

        std::vector<ZydisRegister> SizeSiblings(ZydisRegister reg)
        {
            static constexpr std::array<ZydisRegisterClass, 4> kGeneral = { ZYDIS_REGCLASS_GPR8, ZYDIS_REGCLASS_GPR16, ZYDIS_REGCLASS_GPR32, ZYDIS_REGCLASS_GPR64 };
            static constexpr std::array<ZydisRegisterClass, 3> kVector = { ZYDIS_REGCLASS_XMM, ZYDIS_REGCLASS_YMM, ZYDIS_REGCLASS_ZMM };
            const ZydisRegisterClass registerClass = ZydisRegisterGetClass(reg);
            const ZyanI8 id = ZydisRegisterGetId(reg);
            std::vector<ZydisRegister> siblings;
            const auto collect = [&](const auto& family)
            {
                if (std::find(family.begin(), family.end(), registerClass) == family.end() || id < 0)
                {
                    return;
                }
                for (const ZydisRegisterClass other : family)
                {
                    const ZydisRegister sibling = ZydisRegisterEncode(other, static_cast<ZyanU8>(id));
                    if (other != registerClass && sibling != ZYDIS_REGISTER_NONE)
                    {
                        siblings.push_back(sibling);
                    }
                }
            };
            collect(kGeneral);
            collect(kVector);
            return siblings;
        }

        [[noreturn]] void FailWithDiagnosis(const Instruction& instruction, const AssembleOptions& options)
        {
            const auto encodes = [&options](const Instruction& candidate)
            {
                return !CollectEncodings(candidate, options).empty();
            };
            const std::string text = Quoted(instruction.Text);
            for (std::size_t i = instruction.Operands.size(); i-- > 0;)
            {
                const Operand& operand = instruction.Operands[i];
                Instruction relaxed = instruction;
                Operand& changed = relaxed.Operands[i];
                switch (operand.Kind)
                {
                case OperandKind::Register:
                    for (const ZydisRegister sibling : SizeSiblings(operand.Register))
                    {
                        changed.Register = sibling;
                        if (encodes(relaxed))
                        {
                            Fail(AsmError::BadOperand, Quoted(operand.Text) + " does not fit " + text + ".");
                        }
                    }
                    break;
                case OperandKind::Pointer:
                    break;
                case OperandKind::Immediate:
                    changed.Immediate = Value{};
                    if (encodes(relaxed))
                    {
                        Fail(AsmError::OutOfRange, "The value " + Quoted(operand.Text) + " is too large for " + text + ".");
                    }
                    break;
                case OperandKind::Memory:
                    if (operand.SizeWritten)
                    {
                        changed.SizeWritten = false;
                        if (encodes(relaxed))
                        {
                            Fail(AsmError::BadOperand, Quoted(operand.Text) + " does not fit " + text + ".");
                        }
                        changed.SizeWritten = true;
                    }
                    if (!operand.Displacement.Pending && operand.Displacement.Number != 0)
                    {
                        changed.Displacement = Value{};
                        if (encodes(relaxed))
                        {
                            if (IsAbsoluteMemory(operand))
                            {
                                Fail(AsmError::OutOfRange, "Address " + Hex(operand.Displacement.Number) + " is out of reach for " + text + ".");
                            }
                            Fail(AsmError::OutOfRange, "Displacement " + Hex(operand.Displacement.Number) + " is out of range in " + text + ".");
                        }
                    }
                    break;
                }
            }
            if (instruction.Prefixes != 0)
            {
                Instruction plain = instruction;
                plain.Prefixes = 0;
                if (encodes(plain))
                {
                    Fail(AsmError::BadOperand, "The prefix does not fit " + text + ".");
                }
            }
            AssembleOptions otherMode = options;
            otherMode.Is64Bit = !options.Is64Bit;
            if (!CollectEncodings(instruction, otherMode).empty())
            {
                Fail(AsmError::BadOperand, text + (options.Is64Bit ? " is not available in 64-bit code." : " is only available in 64-bit code."));
            }
            Fail(AsmError::BadOperand, "Invalid operands for " + Quoted(instruction.MnemonicText) + ".");
        }

        std::optional<Encoded> EncodeRelative(ZydisMnemonic mnemonic, ZydisInstructionAttributes prefixes, BranchHint hint, std::uint64_t target, const AssembleOptions& options)
        {
            ZydisEncoderRequest request;
            std::memset(&request, 0, sizeof(request));
            request.machine_mode = MachineMode(options);
            request.mnemonic = mnemonic;
            request.prefixes = prefixes;
            if (hint == BranchHint::Short)
            {
                request.branch_type = ZYDIS_BRANCH_TYPE_SHORT;
                request.branch_width = ZYDIS_BRANCH_WIDTH_8;
            }
            else if (hint == BranchHint::Near)
            {
                request.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
            }
            request.operand_count = 1;
            request.operands[0].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            request.operands[0].imm.u = target;
            Encoded encoded;
            ZyanUSize length = encoded.Bytes.size();
            if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, encoded.Bytes.data(), &length, options.Address)))
            {
                return std::nullopt;
            }
            encoded.Length = length;
            return encoded;
        }

        std::uint64_t WrappedTarget32(std::uint64_t target, std::uint64_t address)
        {
            const auto distance = static_cast<std::uint32_t>(target - address);
            return address + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(distance)));
        }

        std::vector<std::uint8_t> FarBranch(const Instruction& instruction, std::uint64_t target, const AssembleOptions& options)
        {
            const ZydisMnemonic mnemonic = instruction.Info.Mnemonic;
            std::vector<std::uint8_t> bytes;
            if (mnemonic == ZYDIS_MNEMONIC_CALL)
            {
                bytes.assign(kFarCallStub.begin(), kFarCallStub.end());
            }
            else
            {
                if (mnemonic != ZYDIS_MNEMONIC_JMP)
                {
                    const std::optional<Encoded> skip = EncodeRelative(*InvertedCondition(mnemonic), 0, BranchHint::Short, options.Address + kShortJumpLength + kFarJumpLength, options);
                    if (!skip || skip->Length != kShortJumpLength)
                    {
                        Fail(AsmError::EncodeFailed, "Could not encode " + Quoted(instruction.Text) + ".");
                    }
                    bytes = ToVector(*skip);
                }
                bytes.insert(bytes.end(), kFarJumpStub.begin(), kFarJumpStub.end());
            }
            AppendLittleEndian(bytes, target, 8);
            return bytes;
        }

        std::vector<std::uint8_t> EncodeBranch(const Instruction& instruction, const AssembleOptions& options)
        {
            const ZydisMnemonic mnemonic = instruction.Info.Mnemonic;
            const Value& target = instruction.Operands.front().Immediate;
            const bool farCapable = CanUseFarForm(mnemonic);
            const bool farAllowed = options.Is64Bit && options.AllowFarBranches && farCapable && instruction.Hint != BranchHint::Near;
            const BranchHint hint = options.LongBranches && instruction.Hint == BranchHint::None && !IsCounterBranch(mnemonic) ? BranchHint::Near : instruction.Hint;
            if (instruction.Hint == BranchHint::Far)
            {
                if (!options.Is64Bit || !farCapable)
                {
                    Fail(AsmError::BadOperand, "\"far\" with a target needs jmp, call or a conditional jump in 64-bit code.");
                }
                return FarBranch(instruction, target.Number, options);
            }
            std::uint64_t destination = target.Number;
            if (target.Pending)
            {
                if (instruction.Hint == BranchHint::Short || IsCounterBranch(mnemonic))
                {
                    destination = options.Address;
                }
                else if (farAllowed)
                {
                    return FarBranch(instruction, 0, options);
                }
                else
                {
                    destination = options.Address + kNearBranchReach;
                }
            }
            else if (!options.Is64Bit)
            {
                if (!FitsAddress32(destination))
                {
                    Fail(AsmError::OutOfRange, "Target " + Hex(destination) + " is outside 32-bit memory.");
                }
                destination = WrappedTarget32(destination, options.Address);
            }
            if (const std::optional<Encoded> encoded = EncodeRelative(mnemonic, instruction.Prefixes, hint, destination, options))
            {
                return ToVector(*encoded);
            }
            if (!target.Pending && instruction.Hint != BranchHint::Short && farAllowed)
            {
                return FarBranch(instruction, destination, options);
            }
            if (EncodeRelative(mnemonic, instruction.Prefixes, instruction.Hint, options.Address, options))
            {
                if (instruction.Hint == BranchHint::Short)
                {
                    Fail(AsmError::OutOfRange, "Target " + Hex(target.Number) + " is too far for a short jump.");
                }
                Fail(AsmError::OutOfRange, "Target " + Hex(target.Number) + " is out of reach for " + Quoted(instruction.Text) + ".");
            }
            AssembleOptions otherMode = options;
            otherMode.Is64Bit = !options.Is64Bit;
            if (EncodeRelative(mnemonic, instruction.Prefixes, instruction.Hint, options.Address, otherMode))
            {
                Fail(AsmError::BadOperand, Quoted(instruction.Text) + (options.Is64Bit ? " is not available in 64-bit code." : " is only available in 64-bit code."));
            }
            Fail(AsmError::BadOperand, "Invalid operands for " + Quoted(instruction.MnemonicText) + ".");
        }

        std::uint64_t PackedCharacters(const std::string& text)
        {
            if (text.size() > 8)
            {
                Fail(AsmError::OutOfRange, "The text " + Quoted(text) + " is longer than 8 characters.");
            }
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                value |= static_cast<std::uint64_t>(static_cast<unsigned char>(text[i])) << (8 * i);
            }
            return value;
        }

        class LineParser
        {
        public:
            LineParser(std::string_view line, const AssembleOptions& options)
                : Line(line), Tokens(Tokenize(line)), Options(options)
            {
            }

            std::vector<std::uint8_t> AssembleLine()
            {
                Instruction instruction;
                std::string prefixText;
                while (Peek().Kind == TokenKind::Word)
                {
                    const std::optional<ZydisInstructionAttributes> prefix = PrefixWord(Peek().Lower);
                    if (!prefix)
                    {
                        break;
                    }
                    instruction.Prefixes |= *prefix;
                    prefixText += Next().Text + " ";
                }
                if (Peek().Kind != TokenKind::Word)
                {
                    Fail(AsmError::Syntax, "Expected an instruction but found " + Describe(Peek()) + ".");
                }
                const Token& mnemonicToken = Next();
                if (instruction.Prefixes == 0)
                {
                    if (const std::optional<std::size_t> elementSize = DataElementSize(mnemonicToken.Lower))
                    {
                        return AssembleData(*elementSize, mnemonicToken.Text);
                    }
                }
                const auto& mnemonics = Mnemonics();
                const auto found = mnemonics.find(mnemonicToken.Lower);
                if (found == mnemonics.end())
                {
                    Fail(AsmError::UnknownMnemonic, "Unknown instruction " + Quoted(mnemonicToken.Text) + ".");
                }
                instruction.Info = found->second;
                instruction.MnemonicText = mnemonicToken.Text;
                ParseOperands(instruction);
                instruction.Text = prefixText + mnemonicToken.Text;
                for (std::size_t i = 0; i < instruction.Operands.size(); ++i)
                {
                    instruction.Text += i == 0 ? " " : ", ";
                    instruction.Text += instruction.Operands[i].Text;
                }
                return EncodeInstruction(instruction);
            }

            bool UsedPending = false;

        private:
            std::string_view Line;
            std::vector<Token> Tokens;
            const AssembleOptions& Options;
            std::size_t Position = 0;
            std::size_t LastEnd = 0;

            const Token& Peek(std::size_t ahead = 0) const
            {
                return Tokens[std::min(Position + ahead, Tokens.size() - 1)];
            }

            const Token& Next()
            {
                const Token& token = Tokens[Position];
                if (token.Kind != TokenKind::End)
                {
                    ++Position;
                    LastEnd = token.End;
                }
                return token;
            }

            bool AtEnd() const
            {
                return Peek().Kind == TokenKind::End;
            }

            static bool IsPunct(const Token& token, char c)
            {
                return token.Kind == TokenKind::Punct && token.Text[0] == c;
            }

            bool TakePunct(char c)
            {
                if (!IsPunct(Peek(), c))
                {
                    return false;
                }
                Next();
                return true;
            }

            static std::string Describe(const Token& token)
            {
                if (token.Kind == TokenKind::End)
                {
                    return "the end of the line";
                }
                if (token.Kind == TokenKind::Quoted)
                {
                    return token.Quote + token.Text + token.Quote;
                }
                return Quoted(token.Text);
            }

            void ExpectPunct(char c)
            {
                if (!TakePunct(c))
                {
                    Fail(AsmError::Syntax, "Expected " + Quoted(std::string(1, c)) + " but found " + Describe(Peek()) + ".");
                }
            }

            std::string_view Source(std::size_t begin) const
            {
                return Trimmed(Line.substr(begin, LastEnd > begin ? LastEnd - begin : 0));
            }

            Value ResolveSymbol(const std::string& name, bool quoted)
            {
                SymbolValue symbol;
                if (Options.Resolve)
                {
                    symbol = Options.Resolve(name);
                }
                if (symbol.State == SymbolState::Known)
                {
                    return Value{ symbol.Value, false };
                }
                if (symbol.State == SymbolState::Pending)
                {
                    if (!Options.AllowPending)
                    {
                        Fail(AsmError::UnknownSymbol, Quoted(name) + " has no address yet.");
                    }
                    UsedPending = true;
                    return Value{ 0, true };
                }
                if (!quoted)
                {
                    if (const std::optional<std::uint64_t> number = ParseNumber(name))
                    {
                        return Value{ *number, false };
                    }
                    if (std::isdigit(static_cast<unsigned char>(name.front())))
                    {
                        Fail(AsmError::BadNumber, "Bad number " + Quoted(name) + ".");
                    }
                }
                Fail(AsmError::UnknownSymbol, "Unknown symbol " + Quoted(name) + ".");
            }

            bool AtCast() const
            {
                if (!IsPunct(Peek(), '(') || Peek(1).Kind != TokenKind::Word || !IsPunct(Peek(2), ')'))
                {
                    return false;
                }
                return Peek(1).Lower == "float" || Peek(1).Lower == "double" || Peek(1).Lower == "int";
            }

            Value ParseCast()
            {
                Next();
                const std::string kind = Next().Lower;
                const bool isDouble = kind == "double";
                Next();
                const Token& start = Peek();
                if (start.Kind == TokenKind::End)
                {
                    Fail(AsmError::Syntax, "Expected a number after the cast.");
                }
                const char* first = Line.data() + start.Begin;
                const char* last = Line.data() + Line.size();
                if (kind == "int")
                {
                    long long integer = 0;
                    const std::from_chars_result whole = std::from_chars(first, last, integer);
                    if (whole.ec != std::errc{} || whole.ptr == first)
                    {
                        Fail(AsmError::BadNumber, "Bad decimal number after the cast.");
                    }
                    const auto used = static_cast<std::size_t>(whole.ptr - Line.data());
                    while (!AtEnd() && Peek().Begin < used)
                    {
                        if (Peek().End > used)
                        {
                            Fail(AsmError::BadNumber, "Bad decimal number " + Quoted(Peek().Text) + ".");
                        }
                        Next();
                    }
                    return Value{ static_cast<std::uint64_t>(integer), false };
                }
                double number = 0.0;
                const std::from_chars_result parsed = std::from_chars(first, last, number);
                if (parsed.ec != std::errc{} || parsed.ptr == first)
                {
                    Fail(AsmError::BadNumber, "Bad decimal number after the cast.");
                }
                const auto consumed = static_cast<std::size_t>(parsed.ptr - Line.data());
                while (!AtEnd() && Peek().Begin < consumed)
                {
                    if (Peek().End > consumed)
                    {
                        Fail(AsmError::BadNumber, "Bad decimal number " + Quoted(Peek().Text) + ".");
                    }
                    Next();
                }
                if (isDouble)
                {
                    return Value{ std::bit_cast<std::uint64_t>(number), false };
                }
                return Value{ std::bit_cast<std::uint32_t>(static_cast<float>(number)), false };
            }

            Value ParseAtom()
            {
                const Token& token = Peek();
                switch (token.Kind)
                {
                case TokenKind::Number:
                {
                    Next();
                    const std::optional<std::uint64_t> number = ParseNumber(token.Text);
                    if (!number && token.Text.size() > 1 && token.Text.front() == '$' && Options.Resolve)
                    {
                        const SymbolValue symbol = Options.Resolve(token.Text);
                        if (symbol.State == SymbolState::Known)
                            return Value{ symbol.Value, false };
                    }
                    if (!number)
                    {
                        Fail(AsmError::BadNumber, "Bad number " + Quoted(token.Text) + ".");
                    }
                    return Value{ *number, false };
                }
                case TokenKind::Quoted:
                    Next();
                    if (token.Quote == '\'')
                    {
                        return Value{ PackedCharacters(token.Text), false };
                    }
                    return ResolveSymbol(token.Text, true);
                case TokenKind::Word:
                    if (FindRegister(token.Lower) != ZYDIS_REGISTER_NONE)
                    {
                        Fail(AsmError::BadOperand, "The register " + Quoted(token.Text) + " cannot be used in a value.");
                    }
                    if (IsReservedWord(token.Lower))
                    {
                        Fail(AsmError::Syntax, "Unexpected " + Quoted(token.Text) + ".");
                    }
                    Next();
                    if (Options.Resolve && IsPunct(Peek(), '-') && Peek().Begin == token.End)
                    {
                        std::string joined = token.Text;
                        std::string best;
                        std::size_t bestTokens = 0;
                        std::size_t end = token.End;
                        for (std::size_t ahead = 0;; ahead += 2)
                        {
                            const Token& dash = Peek(ahead);
                            const Token& part = Peek(ahead + 1);
                            if (!IsPunct(dash, '-') || dash.Begin != end || (part.Kind != TokenKind::Word && part.Kind != TokenKind::Number) || part.Begin != dash.End)
                                break;
                            joined += "-" + part.Text;
                            end = part.End;
                            if (Options.Resolve(joined).State == SymbolState::Known)
                            {
                                best = joined;
                                bestTokens = ahead + 2;
                            }
                        }
                        if (bestTokens > 0)
                        {
                            for (std::size_t i = 0; i < bestTokens; ++i)
                                Next();
                            return ResolveSymbol(best, false);
                        }
                    }
                    return ResolveSymbol(token.Text, false);
                case TokenKind::Punct:
                case TokenKind::End:
                    break;
                }
                Fail(AsmError::Syntax, "Expected a value but found " + Describe(token) + ".");
            }

            Value ParseFactor()
            {
                if (TakePunct('-'))
                {
                    const Value inner = ParseFactor();
                    return Value{ 0 - inner.Number, inner.Pending };
                }
                if (TakePunct('+'))
                {
                    return ParseFactor();
                }
                if (AtCast())
                {
                    return ParseCast();
                }
                if (TakePunct('('))
                {
                    const Value inner = ParseExpression();
                    ExpectPunct(')');
                    return inner;
                }
                return ParseAtom();
            }

            Value ParseTerm()
            {
                Value result = ParseFactor();
                while (TakePunct('*'))
                {
                    const Value right = ParseFactor();
                    result = Value{ result.Number * right.Number, result.Pending || right.Pending };
                }
                return result;
            }

            Value ParseExpression()
            {
                Value result = ParseTerm();
                while (true)
                {
                    if (TakePunct('+'))
                    {
                        const Value right = ParseTerm();
                        result = Value{ result.Number + right.Number, result.Pending || right.Pending };
                    }
                    else if (TakePunct('-'))
                    {
                        const Value right = ParseTerm();
                        result = Value{ result.Number - right.Number, result.Pending || right.Pending };
                    }
                    else
                    {
                        return result;
                    }
                }
            }

            ZydisRegister PeekRegister() const
            {
                if (Peek().Kind != TokenKind::Word)
                {
                    return ZYDIS_REGISTER_NONE;
                }
                return FindRegister(Peek().Lower);
            }

            ZydisRegister TakeRegister()
            {
                const Token& token = Next();
                if (token.Lower == "st" && IsPunct(Peek(), '('))
                {
                    Next();
                    const Token& index = Next();
                    if (index.Kind != TokenKind::Word || index.Text.size() != 1 || index.Text[0] < '0' || index.Text[0] > '7')
                    {
                        Fail(AsmError::BadOperand, "Expected st(0) to st(7).");
                    }
                    ExpectPunct(')');
                    return static_cast<ZydisRegister>(ZYDIS_REGISTER_ST0 + (index.Text[0] - '0'));
                }
                return FindRegister(token.Lower);
            }

            void AddMemoryRegister(Operand& operand, ZydisRegister reg, std::uint64_t scale)
            {
                const bool instructionPointer = reg == ZYDIS_REGISTER_RIP || reg == ZYDIS_REGISTER_EIP;
                if (instructionPointer)
                {
                    if (scale != 1 || operand.Base != ZYDIS_REGISTER_NONE)
                    {
                        Fail(AsmError::BadOperand, "rip can only be the base of an address.");
                    }
                    operand.Base = reg;
                    return;
                }
                if (scale == 1 && operand.Base == ZYDIS_REGISTER_NONE && !IsVectorRegister(reg))
                {
                    operand.Base = reg;
                    return;
                }
                if (operand.Index != ZYDIS_REGISTER_NONE)
                {
                    Fail(AsmError::BadOperand, "An address can hold at most two registers.");
                }
                if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
                {
                    Fail(AsmError::BadOperand, "The scale must be 1, 2, 4 or 8.");
                }
                operand.Index = reg;
                operand.Scale = static_cast<std::uint8_t>(scale);
            }

            void ParseMemoryTerm(Operand& operand, bool negative)
            {
                ZydisRegister reg = ZYDIS_REGISTER_NONE;
                Value factor{ 1, false };
                do
                {
                    if (PeekRegister() != ZYDIS_REGISTER_NONE)
                    {
                        if (reg != ZYDIS_REGISTER_NONE)
                        {
                            Fail(AsmError::BadOperand, "Registers cannot be multiplied.");
                        }
                        reg = TakeRegister();
                    }
                    else
                    {
                        const Value right = ParseFactor();
                        factor = Value{ factor.Number * right.Number, factor.Pending || right.Pending };
                    }
                }
                while (TakePunct('*'));
                if (reg == ZYDIS_REGISTER_NONE)
                {
                    Value& displacement = operand.Displacement;
                    displacement.Number = negative ? displacement.Number - factor.Number : displacement.Number + factor.Number;
                    displacement.Pending = displacement.Pending || factor.Pending;
                    return;
                }
                if (negative)
                {
                    Fail(AsmError::BadOperand, "A register cannot be subtracted in an address.");
                }
                if (factor.Pending)
                {
                    Fail(AsmError::BadOperand, "The scale must be a number.");
                }
                AddMemoryRegister(operand, reg, factor.Number);
            }

            void ParseMemory(Operand& operand)
            {
                ExpectPunct('[');
                if (IsSegmentRegister(PeekRegister()) && IsPunct(Peek(1), ':'))
                {
                    if (operand.Segment != ZYDIS_REGISTER_NONE)
                    {
                        Fail(AsmError::BadOperand, "An address can have only one segment.");
                    }
                    operand.Segment = TakeRegister();
                    Next();
                }
                bool first = true;
                while (!IsPunct(Peek(), ']'))
                {
                    bool negative = false;
                    if (TakePunct('-'))
                    {
                        negative = true;
                    }
                    else if (!TakePunct('+') && !first)
                    {
                        Fail(AsmError::Syntax, "Expected \"+\", \"-\" or \"]\" but found " + Describe(Peek()) + ".");
                    }
                    if (AtEnd())
                    {
                        break;
                    }
                    ParseMemoryTerm(operand, negative);
                    first = false;
                }
                ExpectPunct(']');
                if (first)
                {
                    Fail(AsmError::Syntax, "An address cannot be empty.");
                }
                if (operand.Base == ZYDIS_REGISTER_NONE && operand.Index != ZYDIS_REGISTER_NONE && operand.Scale == 1 && !IsVectorRegister(operand.Index))
                {
                    operand.Base = operand.Index;
                    operand.Index = ZYDIS_REGISTER_NONE;
                    operand.Scale = 0;
                }
                const bool stackIndex = operand.Index == ZYDIS_REGISTER_RSP || operand.Index == ZYDIS_REGISTER_ESP;
                if (stackIndex && operand.Scale == 1)
                {
                    std::swap(operand.Base, operand.Index);
                }
            }

            void ParseDecorator(Instruction& instruction)
            {
                ExpectPunct('{');
                std::string text;
                while (!AtEnd() && !IsPunct(Peek(), '}'))
                {
                    text += Next().Lower;
                }
                ExpectPunct('}');
                const ZydisRegister reg = FindRegister(text);
                if (ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_MASK)
                {
                    instruction.Mask = reg;
                    return;
                }
                if (text == "z")
                {
                    instruction.Zeroing = true;
                    return;
                }
                if (text == "sae")
                {
                    instruction.Sae = true;
                    return;
                }
                static constexpr std::array<std::pair<std::string_view, ZydisRoundingMode>, 4> kRoundings = { {
                    { "rn-sae", ZYDIS_ROUNDING_MODE_RN },
                    { "rd-sae", ZYDIS_ROUNDING_MODE_RD },
                    { "ru-sae", ZYDIS_ROUNDING_MODE_RU },
                    { "rz-sae", ZYDIS_ROUNDING_MODE_RZ },
                } };
                for (const auto& [name, rounding] : kRoundings)
                {
                    if (text == name)
                    {
                        instruction.Rounding = rounding;
                        instruction.Sae = true;
                        return;
                    }
                }
                static constexpr std::array<std::pair<std::string_view, ZydisBroadcastMode>, 6> kBroadcasts = { {
                    { "1to2", ZYDIS_BROADCAST_MODE_1_TO_2 },
                    { "1to4", ZYDIS_BROADCAST_MODE_1_TO_4 },
                    { "1to8", ZYDIS_BROADCAST_MODE_1_TO_8 },
                    { "1to16", ZYDIS_BROADCAST_MODE_1_TO_16 },
                    { "1to32", ZYDIS_BROADCAST_MODE_1_TO_32 },
                    { "1to64", ZYDIS_BROADCAST_MODE_1_TO_64 },
                } };
                for (const auto& [name, broadcast] : kBroadcasts)
                {
                    if (text == name)
                    {
                        instruction.Broadcast = broadcast;
                        return;
                    }
                }
                Fail(AsmError::Syntax, "Unknown decorator {" + text + "}.");
            }

            Operand ParseOperand(Instruction& instruction)
            {
                Operand operand;
                const std::size_t begin = Peek().Begin;
                while (Peek().Kind == TokenKind::Word)
                {
                    const std::string& lower = Peek().Lower;
                    const std::optional<std::uint16_t> size = SizeWord(lower);
                    if (size && !operand.SizeWritten)
                    {
                        operand.Size = *size;
                        operand.SizeWritten = true;
                        Next();
                        if (Peek().Kind == TokenKind::Word && Peek().Lower == "ptr")
                        {
                            Next();
                        }
                        continue;
                    }
                    if (lower == "short")
                    {
                        instruction.Hint = BranchHint::Short;
                    }
                    else if (lower == "near")
                    {
                        instruction.Hint = BranchHint::Near;
                    }
                    else if (lower == "far")
                    {
                        instruction.Hint = BranchHint::Far;
                    }
                    else
                    {
                        break;
                    }
                    Next();
                }
                if (IsSegmentRegister(PeekRegister()) && IsPunct(Peek(1), ':'))
                {
                    operand.Segment = TakeRegister();
                    Next();
                    if (!IsPunct(Peek(), '['))
                    {
                        Fail(AsmError::Syntax, "Expected \"[\" after the segment but found " + Describe(Peek()) + ".");
                    }
                }
                if (IsPunct(Peek(), '['))
                {
                    operand.Kind = OperandKind::Memory;
                    ParseMemory(operand);
                }
                else if (PeekRegister() != ZYDIS_REGISTER_NONE)
                {
                    operand.Kind = OperandKind::Register;
                    operand.Register = TakeRegister();
                    if (!AtEnd() && !IsPunct(Peek(), ',') && !IsPunct(Peek(), '{'))
                    {
                        Fail(AsmError::BadOperand, "Unexpected " + Describe(Peek()) + " after the register " + Quoted(Source(begin)) + ".");
                    }
                }
                else
                {
                    operand.Kind = OperandKind::Immediate;
                    operand.Immediate = ParseExpression();
                    if (TakePunct(':'))
                    {
                        if (operand.Immediate.Pending || operand.Immediate.Number > 0xFFFF)
                        {
                            Fail(AsmError::OutOfRange, "The segment of a far pointer must be a number up to FFFF.");
                        }
                        operand.Kind = OperandKind::Pointer;
                        operand.PointerSegment = operand.Immediate.Number;
                        operand.Immediate = ParseExpression();
                    }
                }
                while (IsPunct(Peek(), '{'))
                {
                    ParseDecorator(instruction);
                }
                operand.Text = std::string(Source(begin));
                return operand;
            }

            void ParseOperands(Instruction& instruction)
            {
                if (AtEnd())
                {
                    return;
                }
                if (Peek().Kind == TokenKind::Word && Peek().Lower == "far" && (Peek(1).Kind == TokenKind::End || IsPunct(Peek(1), ',')))
                {
                    instruction.Hint = BranchHint::Far;
                    Next();
                    if (AtEnd())
                    {
                        return;
                    }
                    Next();
                }
                while (true)
                {
                    if (IsPunct(Peek(), '{'))
                    {
                        ParseDecorator(instruction);
                    }
                    else if (AtEnd() || IsPunct(Peek(), ','))
                    {
                        Fail(AsmError::Syntax, "Expected an operand but found " + Describe(Peek()) + ".");
                    }
                    else
                    {
                        instruction.Operands.push_back(ParseOperand(instruction));
                    }
                    if (TakePunct(','))
                    {
                        continue;
                    }
                    if (AtEnd())
                    {
                        return;
                    }
                    Fail(AsmError::Syntax, "Unexpected " + Describe(Peek()) + ".");
                }
            }

            bool EndsDataItem(const Token& next, std::size_t elementSize) const
            {
                if (next.Kind == TokenKind::End || IsPunct(next, ','))
                {
                    return true;
                }
                return elementSize == 1 && (next.Kind == TokenKind::Word || next.Kind == TokenKind::Number || next.Kind == TokenKind::Quoted || IsPunct(next, '('));
            }

            std::vector<std::uint8_t> AssembleData(std::size_t elementSize, const std::string& directive)
            {
                if (AtEnd())
                {
                    Fail(AsmError::Syntax, Quoted(directive) + " needs at least one value.");
                }
                std::vector<std::uint8_t> bytes;
                while (true)
                {
                    const Token& token = Peek();
                    if (token.Kind == TokenKind::Quoted && EndsDataItem(Peek(1), elementSize))
                    {
                        Next();
                        bytes.insert(bytes.end(), token.Text.begin(), token.Text.end());
                        while (bytes.size() % elementSize != 0)
                        {
                            bytes.push_back(0);
                        }
                    }
                    else
                    {
                        const Value value = ParseExpression();
                        if (!FitsElement(value.Number, elementSize))
                        {
                            Fail(AsmError::OutOfRange, "The value " + Hex(value.Number) + " does not fit in a " + std::string(SizeName(elementSize)) + ".");
                        }
                        AppendLittleEndian(bytes, value.Number, elementSize);
                    }
                    if (TakePunct(','))
                    {
                        if (AtEnd())
                        {
                            Fail(AsmError::Syntax, "Expected a value after \",\".");
                        }
                        continue;
                    }
                    if (AtEnd())
                    {
                        return bytes;
                    }
                    if (elementSize != 1)
                    {
                        Fail(AsmError::Syntax, "Unexpected " + Describe(Peek()) + ".");
                    }
                }
            }

            std::vector<std::uint8_t> NopFill(const Instruction& instruction)
            {
                const Value& count = instruction.Operands.front().Immediate;
                if (count.Pending)
                {
                    Fail(AsmError::BadOperand, "The length of \"nop\" must be a number.");
                }
                if (count.Number == 0 || count.Number > kMaxNopCount)
                {
                    Fail(AsmError::OutOfRange, "The length of \"nop\" must be between 1 and " + Hex(kMaxNopCount) + ".");
                }
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count.Number));
                if (!ZYAN_SUCCESS(ZydisEncoderNopFill(bytes.data(), bytes.size())))
                {
                    Fail(AsmError::EncodeFailed, "Could not encode " + Quoted(instruction.Text) + ".");
                }
                return bytes;
            }

            std::vector<std::uint8_t> EncodeInstruction(const Instruction& instruction)
            {
                const std::vector<Operand>& operands = instruction.Operands;
                const bool singleImmediate = operands.size() == 1 && operands.front().Kind == OperandKind::Immediate;
                if (instruction.Info.Mnemonic == ZYDIS_MNEMONIC_NOP && singleImmediate)
                {
                    return NopFill(instruction);
                }
                if (IsRelativeBranch(instruction.Info.Mnemonic) && singleImmediate)
                {
                    return EncodeBranch(instruction, Options);
                }
                if (instruction.Hint == BranchHint::Short)
                {
                    Fail(AsmError::BadOperand, "\"short\" needs a jump target.");
                }
                for (const Operand& operand : operands)
                {
                    if (operand.Kind != OperandKind::Pointer)
                    {
                        continue;
                    }
                    if (Options.Is64Bit)
                    {
                        Fail(AsmError::BadOperand, Quoted(instruction.Text) + " uses a far pointer, which only exists in 32-bit code.");
                    }
                    if (instruction.Hint == BranchHint::Near)
                    {
                        Fail(AsmError::BadOperand, "A far pointer cannot be a near target.");
                    }
                    if (!operand.Immediate.Pending && operand.Immediate.Number > 0xFFFFFFFFull)
                    {
                        Fail(AsmError::OutOfRange, "The offset of " + Quoted(operand.Text) + " does not fit in 32 bits.");
                    }
                }
                const std::vector<SizedEncoding> results = CollectEncodings(instruction, Options);
                if (results.empty())
                {
                    FailWithDiagnosis(instruction, Options);
                }
                const Encoded chosen = ChooseEncoding(instruction, results, Options);
                std::vector<std::uint8_t> bytes;
                if (instruction.Info.WaitFirst)
                {
                    bytes.push_back(kWaitOpcode);
                }
                bytes.insert(bytes.end(), chosen.Bytes.data(), chosen.Bytes.data() + chosen.Length);
                return bytes;
            }
        };
    }

    std::optional<std::uint64_t> ParseNumber(std::string_view text)
    {
        bool negative = false;
        if (!text.empty() && text.front() == '-')
        {
            negative = true;
            text.remove_prefix(1);
        }
        std::uint64_t base = 16;
        if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        {
            text.remove_prefix(2);
        }
        else if (!text.empty() && text.front() == '$')
        {
            text.remove_prefix(1);
        }
        else if (!text.empty() && text.front() == '#')
        {
            base = 10;
            text.remove_prefix(1);
        }
        if (text.empty())
        {
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (const char c : text)
        {
            std::uint64_t digit = 0;
            if (c >= '0' && c <= '9')
            {
                digit = static_cast<std::uint64_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                digit = static_cast<std::uint64_t>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                digit = static_cast<std::uint64_t>(c - 'A' + 10);
            }
            else
            {
                return std::nullopt;
            }
            if (digit >= base || value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            {
                return std::nullopt;
            }
            value = value * base + digit;
        }
        return negative ? 0 - value : value;
    }

    std::string_view ErrorName(AsmError error)
    {
        switch (error)
        {
        case AsmError::None:
            return "None";
        case AsmError::Empty:
            return "Empty";
        case AsmError::Syntax:
            return "Syntax";
        case AsmError::BadNumber:
            return "BadNumber";
        case AsmError::UnknownMnemonic:
            return "UnknownMnemonic";
        case AsmError::BadOperand:
            return "BadOperand";
        case AsmError::UnknownSymbol:
            return "UnknownSymbol";
        case AsmError::OutOfRange:
            return "OutOfRange";
        case AsmError::EncodeFailed:
            return "EncodeFailed";
        }
        return "Unknown";
    }

    std::vector<std::string> KnownMnemonics()
    {
        std::vector<std::string> names;
        names.reserve(Mnemonics().size());
        for (const auto& [name, info] : Mnemonics())
            names.push_back(name);
        std::sort(names.begin(), names.end());
        return names;
    }

    AssembleResult Assemble(std::string_view line, const AssembleOptions& options)
    {
        AssembleResult result;
        try
        {
            const std::string_view text = Trimmed(WithoutComment(line));
            if (text.empty())
            {
                result.Error = AsmError::Empty;
                result.Message = "The line is empty.";
                return result;
            }
            LineParser parser(text, options);
            result.Bytes = parser.AssembleLine();
            result.UsedPending = parser.UsedPending;
            result.Ok = true;
        }
        catch (const Failure& failure)
        {
            result = AssembleResult{};
            result.Error = failure.Error;
            result.Message = failure.Message;
        }
        catch (const std::exception& exception)
        {
            result = AssembleResult{};
            result.Error = AsmError::EncodeFailed;
            result.Message = std::string("The assembler failed: ") + exception.what();
        }
        catch (...)
        {
            result = AssembleResult{};
            result.Error = AsmError::EncodeFailed;
            result.Message = "The assembler failed.";
        }
        return result;
    }
}
