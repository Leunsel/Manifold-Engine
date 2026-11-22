#include "Asm/Assembler.h"
#include "Disasm/Disassembler.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma warning(push, 0)
#include "Zydis.h"
#pragma warning(pop)

namespace Manifold::Tests
{
    namespace
    {
        using Asm::AsmError;
        using Asm::AssembleOptions;
        using Asm::AssembleResult;
        using Asm::SymbolState;
        using Asm::SymbolValue;

        struct Report
        {
            int Cases = 0;
            int Failures = 0;
        };

        struct Symbol
        {
            std::string Name;
            SymbolValue Value;
        };

        AssembleOptions Mode64(std::uint64_t address = 0)
        {
            AssembleOptions options;
            options.Is64Bit = true;
            options.Address = address;
            return options;
        }

        AssembleOptions Mode32(std::uint64_t address = 0x401000)
        {
            AssembleOptions options;
            options.Is64Bit = false;
            options.Address = address;
            return options;
        }

        AssembleOptions WithSymbols(AssembleOptions options, std::vector<Symbol> symbols, bool allowPending = false)
        {
            options.AllowPending = allowPending;
            options.Resolve = [symbols](std::string_view name) -> SymbolValue
            {
                for (const Symbol& symbol : symbols)
                {
                    if (symbol.Name == name)
                    {
                        return symbol.Value;
                    }
                }
                return SymbolValue{};
            };
            return options;
        }

        SymbolValue Known(std::uint64_t value)
        {
            return SymbolValue{ SymbolState::Known, value };
        }

        SymbolValue Pending()
        {
            return SymbolValue{ SymbolState::Pending, 0 };
        }

        std::string HexBytes(const std::vector<std::uint8_t>& bytes)
        {
            static constexpr char kDigits[] = "0123456789ABCDEF";
            std::string text;
            for (const std::uint8_t byte : bytes)
            {
                if (!text.empty())
                {
                    text += ' ';
                }
                text += kDigits[byte >> 4];
                text += kDigits[byte & 0x0F];
            }
            return text;
        }

        std::string Normalized(std::string_view text)
        {
            std::string result;
            bool pendingSpace = false;
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const auto c = static_cast<unsigned char>(text[i]);
                if (std::isspace(c))
                {
                    pendingSpace = !result.empty();
                    continue;
                }
                if (pendingSpace)
                {
                    result += ' ';
                    pendingSpace = false;
                }
                result += static_cast<char>(std::tolower(c));
                const bool hexStart = c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X');
                if (hexStart)
                {
                    result += 'x';
                    i += 2;
                    while (i + 1 < text.size() && text[i] == '0' && std::isxdigit(static_cast<unsigned char>(text[i + 1])))
                    {
                        ++i;
                    }
                    --i;
                }
            }
            return result;
        }

        std::string ModeName(const AssembleOptions& options)
        {
            return options.Is64Bit ? "x64" : "x86";
        }

        void Failed(Report& report, const AssembleOptions& options, std::string_view line, std::string_view expected, std::string_view got)
        {
            ++report.Failures;
            std::printf("FAIL [%s] %.*s\n    expected: %.*s\n    got:      %.*s\n", ModeName(options).c_str(), static_cast<int>(line.size()), line.data(), static_cast<int>(expected.size()), expected.data(), static_cast<int>(got.size()), got.data());
        }

        std::string Outcome(const AssembleResult& result)
        {
            if (result.Ok)
            {
                return HexBytes(result.Bytes) + (result.UsedPending ? " (pending)" : "");
            }
            return std::string(Asm::ErrorName(result.Error)) + ": " + result.Message;
        }

        void ExpectBytes(Report& report, std::string_view line, const AssembleOptions& options, std::string_view expected)
        {
            ++report.Cases;
            const AssembleResult result = Asm::Assemble(line, options);
            if (!result.Ok || HexBytes(result.Bytes) != expected || result.UsedPending)
            {
                Failed(report, options, line, expected, Outcome(result));
            }
        }

        void ExpectError(Report& report, std::string_view line, const AssembleOptions& options, AsmError error, std::string_view message = {})
        {
            ++report.Cases;
            const AssembleResult result = Asm::Assemble(line, options);
            const bool messageMatches = message.empty() ? !result.Message.empty() : result.Message == message;
            if (result.Ok || result.Error != error || !messageMatches || !result.Bytes.empty())
            {
                std::string expected = std::string(Asm::ErrorName(error));
                if (!message.empty())
                {
                    expected += ": " + std::string(message);
                }
                Failed(report, options, line, expected, Outcome(result));
            }
        }

        std::string Disassemble(const std::vector<std::uint8_t>& bytes, const AssembleOptions& options)
        {
            ZydisDecoder decoder;
            ZydisDecoderInit(&decoder, options.Is64Bit ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32, options.Is64Bit ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
            ZydisFormatter formatter;
            ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_FORCE_SIZE, ZYAN_TRUE);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_ABSOLUTE, ZYDIS_PADDING_DISABLED);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_ADDR_PADDING_RELATIVE, ZYDIS_PADDING_DISABLED);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_DISP_PADDING, ZYDIS_PADDING_DISABLED);
            ZydisFormatterSetProperty(&formatter, ZYDIS_FORMATTER_PROP_IMM_PADDING, ZYDIS_PADDING_DISABLED);
            ZydisDecodedInstruction instruction;
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes.data(), bytes.size(), &instruction, operands)))
            {
                return "<cannot decode>";
            }
            if (instruction.length != bytes.size())
            {
                return "<decoded " + std::to_string(instruction.length) + " of " + std::to_string(bytes.size()) + " bytes>";
            }
            char buffer[256] = {};
            if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter, &instruction, operands, instruction.operand_count_visible, buffer, sizeof(buffer), options.Address, nullptr)))
            {
                return "<cannot format>";
            }
            return buffer;
        }

        void ExpectText(Report& report, std::string_view line, const AssembleOptions& options, std::string_view expected)
        {
            ++report.Cases;
            const AssembleResult result = Asm::Assemble(line, options);
            if (!result.Ok)
            {
                Failed(report, options, line, expected, Outcome(result));
                return;
            }
            const std::string text = Disassemble(result.Bytes, options);
            if (Normalized(text) != Normalized(expected))
            {
                Failed(report, options, line, expected, text + "  [" + HexBytes(result.Bytes) + "]");
            }
        }

        void ExpectNumber(Report& report, std::string_view text, std::optional<std::uint64_t> expected)
        {
            ++report.Cases;
            const std::optional<std::uint64_t> got = Asm::ParseNumber(text);
            if (got != expected)
            {
                const auto describe = [](std::optional<std::uint64_t> value)
                {
                    if (!value)
                    {
                        return std::string("none");
                    }
                    char buffer[32] = {};
                    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(*value));
                    return std::string(buffer);
                };
                ++report.Failures;
                std::printf("FAIL ParseNumber(\"%.*s\")\n    expected: %s\n    got:      %s\n", static_cast<int>(text.size()), text.data(), describe(expected).c_str(), describe(got).c_str());
            }
        }

        void ExpectPendingBound(Report& report, std::string_view line, const AssembleOptions& base, std::size_t expectedSize, const std::vector<std::uint64_t>& finals)
        {
            ++report.Cases;
            const AssembleResult pending = Asm::Assemble(line, WithSymbols(base, { { "later", Pending() } }, true));
            if (!pending.Ok || !pending.UsedPending || pending.Bytes.size() != expectedSize)
            {
                Failed(report, base, line, std::to_string(expectedSize) + " pending bytes", Outcome(pending));
                return;
            }
            const AssembleResult refused = Asm::Assemble(line, WithSymbols(base, { { "later", Pending() } }, false));
            if (refused.Ok || refused.Error != AsmError::UnknownSymbol)
            {
                Failed(report, base, line, "UnknownSymbol without AllowPending", Outcome(refused));
            }
            std::size_t finished = 0;
            for (const std::uint64_t value : finals)
            {
                const AssembleResult final = Asm::Assemble(line, WithSymbols(base, { { "later", Known(value) } }, true));
                if (!final.Ok)
                {
                    continue;
                }
                ++finished;
                if (final.UsedPending || final.Bytes.size() > pending.Bytes.size())
                {
                    Failed(report, base, line, "final size <= " + std::to_string(pending.Bytes.size()) + " for later=" + std::to_string(value), Outcome(final));
                }
            }
            if (finished == 0)
            {
                Failed(report, base, line, "at least one final encoding", "none");
            }
        }

        void TestNumbers(Report& report)
        {
            ExpectNumber(report, "10", 0x10);
            ExpectNumber(report, "0x10", 0x10);
            ExpectNumber(report, "0X1f", 0x1F);
            ExpectNumber(report, "$FF", 0xFF);
            ExpectNumber(report, "#100", 100);
            ExpectNumber(report, "-1", 0xFFFFFFFFFFFFFFFFull);
            ExpectNumber(report, "-#10", 0xFFFFFFFFFFFFFFF6ull);
            ExpectNumber(report, "-0x10", 0xFFFFFFFFFFFFFFF0ull);
            ExpectNumber(report, "deadBEEF", 0xDEADBEEF);
            ExpectNumber(report, "FFFFFFFFFFFFFFFF", 0xFFFFFFFFFFFFFFFFull);
            ExpectNumber(report, "0000000000000000001", 1);
            ExpectNumber(report, "10000000000000000", std::nullopt);
            ExpectNumber(report, "#18446744073709551615", 0xFFFFFFFFFFFFFFFFull);
            ExpectNumber(report, "#18446744073709551616", std::nullopt);
            ExpectNumber(report, "#12a", std::nullopt);
            ExpectNumber(report, "", std::nullopt);
            ExpectNumber(report, "-", std::nullopt);
            ExpectNumber(report, "0x", std::nullopt);
            ExpectNumber(report, "$", std::nullopt);
            ExpectNumber(report, "#", std::nullopt);
            ExpectNumber(report, "xyz", std::nullopt);
            ExpectNumber(report, "10h", std::nullopt);
            ExpectNumber(report, " 10", std::nullopt);
        }

        void TestBasicEncodings(Report& report)
        {
            const AssembleOptions x64 = Mode64();
            ExpectBytes(report, "mov eax,1", x64, "B8 01 00 00 00");
            ExpectBytes(report, "MOV EAX, 1", x64, "B8 01 00 00 00");
            ExpectBytes(report, "  mov eax,1   // load one", x64, "B8 01 00 00 00");
            ExpectBytes(report, "mov eax, #100", x64, "B8 64 00 00 00");
            ExpectBytes(report, "mov eax, 0x10", x64, "B8 10 00 00 00");
            ExpectBytes(report, "mov eax, $10", x64, "B8 10 00 00 00");
            ExpectBytes(report, "mov eax, -1", x64, "B8 FF FF FF FF");
            ExpectBytes(report, "mov eax, FFFFFFFF", x64, "B8 FF FF FF FF");
            ExpectBytes(report, "mov eax, 1+2*3", x64, "B8 07 00 00 00");
            ExpectBytes(report, "mov eax, (1+2)*3", x64, "B8 09 00 00 00");
            ExpectBytes(report, "mov eax, 'abcd'", x64, "B8 61 62 63 64");
            ExpectBytes(report, "mov eax, (float)1", x64, "B8 00 00 80 3F");
            ExpectBytes(report, "mov al, FF", x64, "B0 FF");
            ExpectBytes(report, "mov ax, FFFF", x64, "66 B8 FF FF");
            ExpectBytes(report, "mov rax, 1122334455667788", x64, "48 B8 88 77 66 55 44 33 22 11");
            ExpectBytes(report, "add eax, FFFFFFFF", x64, "83 C0 FF");
            ExpectBytes(report, "test al, 80", x64, "A8 80");
            ExpectBytes(report, "add rsp,28", x64, "48 83 C4 28");
            ExpectBytes(report, "sub rsp,0x28", x64, "48 83 EC 28");
            ExpectBytes(report, "push rbx", x64, "53");
            ExpectBytes(report, "pop rbp", x64, "5D");
            ExpectBytes(report, "push r12", x64, "41 54");
            ExpectBytes(report, "push 12345678", x64, "68 78 56 34 12");
            ExpectBytes(report, "push 10", x64, "6A 10");
            ExpectBytes(report, "ret", x64, "C3");
            ExpectBytes(report, "retn", x64, "C3");
            ExpectBytes(report, "ret 8", x64, "C2 08 00");
            ExpectBytes(report, "retf", x64, "CB");
            ExpectBytes(report, "int3", x64, "CC");
            ExpectBytes(report, "int 80", x64, "CD 80");
            ExpectBytes(report, "test eax,eax", x64, "85 C0");
            ExpectBytes(report, "imul eax, ecx, 10", x64, "6B C1 10");
            ExpectBytes(report, "syscall", x64, "0F 05");
            ExpectBytes(report, "cpuid", x64, "0F A2");
            ExpectBytes(report, "rdtsc", x64, "0F 31");
            ExpectBytes(report, "ud2", x64, "0F 0B");
            ExpectBytes(report, "hlt", x64, "F4");
            ExpectBytes(report, "pause", x64, "F3 90");
            ExpectBytes(report, "leave", x64, "C9");
            ExpectBytes(report, "bswap eax", x64, "0F C8");
            ExpectBytes(report, "enter 10, 0", x64, "C8 10 00 00");
            ExpectBytes(report, "in al, dx", x64, "EC");
            ExpectBytes(report, "out 80, al", x64, "E6 80");
            ExpectBytes(report, "mov cr3, rax", x64, "0F 22 D8");
            ExpectBytes(report, "mov rax, dr7", x64, "0F 21 F8");
            ExpectBytes(report, "kmovw k1, eax", x64, "C5 F8 92 C8");
        }

        void TestAliases(Report& report)
        {
            const AssembleOptions x64 = Mode64();
            ExpectBytes(report, "pushfq", x64, "9C");
            ExpectBytes(report, "popfq", x64, "9D");
            ExpectBytes(report, "cdqe", x64, "48 98");
            ExpectBytes(report, "cwde", x64, "98");
            ExpectBytes(report, "cqo", x64, "48 99");
            ExpectBytes(report, "cdq", x64, "99");
            ExpectBytes(report, "cbw", x64, "66 98");
            ExpectBytes(report, "xlatb", x64, "D7");
            ExpectBytes(report, "sal eax,1", x64, "D1 E0");
            ExpectBytes(report, "shl eax,4", x64, "C1 E0 04");
            ExpectBytes(report, "shl rax, cl", x64, "48 D3 E0");
            ExpectBytes(report, "cmove eax,ebx", x64, "0F 44 C3");
            ExpectBytes(report, "cmovne eax,ebx", x64, "0F 45 C3");
            ExpectBytes(report, "cmovg rax,rcx", x64, "48 0F 4F C1");
            ExpectBytes(report, "cmovae ecx, edx", x64, "0F 43 CA");
            ExpectBytes(report, "sete al", x64, "0F 94 C0");
            ExpectBytes(report, "setne cl", x64, "0F 95 C1");
            ExpectBytes(report, "setae dl", x64, "0F 93 C2");
            ExpectBytes(report, "seta bl", x64, "0F 97 C3");
            ExpectBytes(report, "setpo al", x64, "0F 9B C0");
            ExpectBytes(report, "fstsw ax", x64, "9B DF E0");
            ExpectBytes(report, "fnstsw ax", x64, "DF E0");
            ExpectBytes(report, "fwait", x64, "9B");
            ExpectBytes(report, "wait", x64, "9B");
            ExpectBytes(report, "finit", x64, "9B DB E3");
            ExpectBytes(report, "movabs rax, 1122334455667788", x64, "48 B8 88 77 66 55 44 33 22 11");

            // fstcw/fstsw are 9B plus the fn form: the rip-relative displacement counts from the end of
            // the fn instruction, one byte later than the line's address (it pointed one byte too far).
            const AssembleOptions image = Mode64(0x140000000);
            ExpectBytes(report, "fnstcw word ptr [140001000]", image, "D9 3D FA 0F 00 00");
            ExpectBytes(report, "fstcw word ptr [140001000]", image, "9B D9 3D F9 0F 00 00");
            ExpectBytes(report, "fstsw [140001000]", image, "9B DD 3D F9 0F 00 00");
            ExpectBytes(report, "fstenv [140001000]", image, "9B D9 35 F9 0F 00 00");

            // a size word on a lone immediate sets the operand size; a 32-bit push does not exist in
            // 64-bit code, and a size word that contradicts a register is an error, not ignored.
            ExpectBytes(report, "push word 1234", x64, "66 68 34 12");
            ExpectBytes(report, "push word 5", Mode32(), "66 6A 05");
            ExpectBytes(report, "push dword 5", Mode32(), "6A 05");
            ExpectBytes(report, "push 5", x64, "6A 05");
            if (const AssembleResult pushed = Asm::Assemble("push dword 5", x64); pushed.Ok)
                Failed(report, x64, "push dword 5", "an error", Outcome(pushed));
            ++report.Cases;
            if (const AssembleResult sized = Asm::Assemble("mov byte ptr eax,5", x64); sized.Ok)
                Failed(report, x64, "mov byte ptr eax,5", "an error", Outcome(sized));
            ++report.Cases;

            // a prefix a branch cannot take is an error, not a far stub that drops it
            for (const char* line : { "lock jmp 140000100", "lock call 140000100", "rep jz 140000100" })
            {
                ++report.Cases;
                if (const AssembleResult prefixed = Asm::Assemble(line, image); prefixed.Ok)
                    Failed(report, image, line, "an error", Outcome(prefixed));
            }
            ExpectBytes(report, "jmp 7FF600000000", image, "FF 25 00 00 00 00 00 00 00 00 F6 7F 00 00");

            // in 32-bit code a rel32 branch reaches every address; the wrap counts from the end
            ExpectBytes(report, "jmp 80001000", Mode32(0x1000), "E9 FB FF FF 7F");
            ExpectBytes(report, "call 80001000", Mode32(0x1000), "E8 FB FF FF 7F");
            ExpectBytes(report, "jz 80001000", Mode32(0x1000), "0F 84 FA FF FF 7F");
            ExpectBytes(report, "jmp 80001004", Mode32(0x1000), "E9 FF FF FF 7F");
            ExpectBytes(report, "jmp 80001005", Mode32(0x1000), "E9 00 00 00 80");
        }

        void TestMemory(Report& report)
        {
            const AssembleOptions x64 = Mode64();
            ExpectBytes(report, "mov rax,[rax+rbx*8+10]", x64, "48 8B 44 D8 10");
            ExpectBytes(report, "mov rax,[rbx*8+rax+10]", x64, "48 8B 44 D8 10");
            ExpectBytes(report, "mov rax,[10+8*rbx+rax]", x64, "48 8B 44 D8 10");
            ExpectBytes(report, "mov rax, qword ptr [rax+rbx*8+10]", x64, "48 8B 44 D8 10");
            ExpectBytes(report, "lea rcx,[rsp+20]", x64, "48 8D 4C 24 20");
            ExpectBytes(report, "lea rcx, qword ptr [rsp+20]", x64, "48 8D 4C 24 20");
            ExpectBytes(report, "lea eax, [rax+rax*2]", x64, "8D 04 40");
            ExpectBytes(report, "mov eax,[rbp-8]", x64, "8B 45 F8");
            ExpectBytes(report, "mov eax,[rbp-#8]", x64, "8B 45 F8");
            ExpectBytes(report, "mov eax,[rbp+-8]", x64, "8B 45 F8");
            ExpectBytes(report, "mov [rax+rcx*4-8], edx", x64, "89 54 88 F8");
            ExpectBytes(report, "mov eax,[rbx+rcx]", x64, "8B 04 0B");
            ExpectBytes(report, "mov eax,[rcx*1]", x64, "8B 01");
            ExpectBytes(report, "mov eax,[rax+rsp]", x64, "8B 04 04");
            ExpectBytes(report, "mov r8, [r9+r10*2+100]", x64, "4F 8B 84 51 00 01 00 00");
            ExpectBytes(report, "mov rax, [rsp+8]", x64, "48 8B 44 24 08");
            ExpectBytes(report, "mov qword ptr [rax],1", x64, "48 C7 00 01 00 00 00");
            ExpectBytes(report, "mov dword ptr [rax],1", x64, "C7 00 01 00 00 00");
            ExpectBytes(report, "mov word ptr [rax],1", x64, "66 C7 00 01 00");
            ExpectBytes(report, "mov byte ptr [rax],1", x64, "C6 00 01");
            ExpectBytes(report, "mov byte [rax],1", x64, "C6 00 01");
            ExpectBytes(report, "mov dword ptr [rax], (float)100", x64, "C7 00 00 00 C8 42");
            ExpectBytes(report, "inc dword ptr [rax]", x64, "FF 00");
            ExpectBytes(report, "inc byte ptr [rax]", x64, "FE 00");
            ExpectBytes(report, "xchg [rax], ecx", x64, "87 08");
            ExpectBytes(report, "lock xadd [rcx],eax", x64, "F0 0F C1 01");
            ExpectBytes(report, "cmpxchg [rcx], edx", x64, "0F B1 11");
            ExpectBytes(report, "lock cmpxchg8b [rdi]", x64, "F0 0F C7 0F");
            ExpectBytes(report, "cmpxchg16b [rdi]", x64, "48 0F C7 0F");
            ExpectBytes(report, "movzx eax, byte ptr [rcx]", x64, "0F B6 01");
            ExpectBytes(report, "movsx rax, word ptr [rcx]", x64, "48 0F BF 01");
            ExpectBytes(report, "movsxd rax, dword ptr [rcx]", x64, "48 63 01");
            ExpectBytes(report, "movsx rax, dword ptr [rcx]", x64, "48 63 01");
            ExpectBytes(report, "call qword ptr [rax]", x64, "FF 10");
            ExpectBytes(report, "call [rax]", x64, "FF 10");
            ExpectBytes(report, "jmp [rax]", x64, "FF 20");
            ExpectBytes(report, "jmp rax", x64, "FF E0");
            ExpectBytes(report, "push [rax]", x64, "FF 30");
            ExpectBytes(report, "push qword ptr [rax]", x64, "FF 30");
            ExpectBytes(report, "pop qword ptr [rax]", x64, "8F 00");
            ExpectBytes(report, "jmp qword ptr [rip+10]", x64, "FF 25 10 00 00 00");
            ExpectBytes(report, "mov [rip+10], eax", x64, "89 05 10 00 00 00");
            ExpectBytes(report, "mov eax, [rip-10]", x64, "8B 05 F0 FF FF FF");
            ExpectBytes(report, "mov rax, gs:[60]", x64, "65 48 8B 04 25 60 00 00 00");
            ExpectBytes(report, "mov rax, qword ptr gs:[60]", x64, "65 48 8B 04 25 60 00 00 00");
            ExpectBytes(report, "mov rax, [gs:60]", x64, "65 48 8B 04 25 60 00 00 00");
            ExpectBytes(report, "mov eax, fs:[rax]", x64, "64 8B 00");
            ExpectBytes(report, "mov eax, ds:[rax]", x64, "8B 00");
            ExpectBytes(report, "mov rax,[7FF000000000]", x64, "48 A1 00 00 00 00 F0 7F 00 00");
            ExpectBytes(report, "mov eax,[7FF000000000]", x64, "A1 00 00 00 00 F0 7F 00 00");
            ExpectBytes(report, "mov [7FF000000000], al", x64, "A2 00 00 00 00 F0 7F 00 00");
            ExpectBytes(report, "nop dword ptr [rax]", x64, "0F 1F 00");
            ExpectBytes(report, "nop [rax]", x64, "0F 1F 00");
            ExpectBytes(report, "nop word ptr [rax+rax+0]", x64, "66 0F 1F 04 00");
            ExpectBytes(report, "nop eax", x64, "0F 1F C0");
            ExpectBytes(report, "fld dword ptr [rax]", x64, "D9 00");
            ExpectBytes(report, "fld qword ptr [rax]", x64, "DD 00");
            ExpectBytes(report, "fld tword ptr [rax]", x64, "DB 28");
            ExpectBytes(report, "fild word ptr [rax]", x64, "DF 00");
            ExpectBytes(report, "fistp qword ptr [rsp]", x64, "DF 3C 24");
            ExpectBytes(report, "fxsave [rax]", x64, "0F AE 00");
            ExpectBytes(report, "prefetcht0 [rax]", x64, "0F 18 08");
            ExpectBytes(report, "clflush [rax]", x64, "0F AE 38");
        }

        void TestAbsoluteAddresses(Report& report)
        {
            const AssembleOptions image = Mode64(0x140001000);
            ExpectBytes(report, "mov rax,[140002000]", image, "48 8B 05 F9 0F 00 00");
            ExpectBytes(report, "mov eax,[140000000]", image, "8B 05 FA EF FF FF");
            ExpectBytes(report, "lea rcx,[140001000]", image, "48 8D 0D F9 FF FF FF");
            ExpectBytes(report, "cmp dword ptr [140002000], 5", image, "83 3D F9 0F 00 00 05");
            ExpectBytes(report, "mov dword ptr [140002000], 12345678", image, "C7 05 F6 0F 00 00 78 56 34 12");
            ExpectBytes(report, "call [140002000]", image, "FF 15 FA 0F 00 00");
            ExpectBytes(report, "mov rax, gs:[140002000]", image, "65 48 A1 00 20 00 40 01 00 00 00");
            const AssembleOptions high = Mode64(0x7FF600000000);
            ExpectBytes(report, "mov rax, [10]", high, "48 8B 04 25 10 00 00 00");
            ExpectBytes(report, "mov eax, [10]", high, "8B 04 25 10 00 00 00");
            ExpectBytes(report, "mov ecx, [FFFFFFFFFFFFFFF0]", high, "8B 0C 25 F0 FF FF FF");
            ExpectBytes(report, "mov rax, [7FF600001000]", high, "48 8B 05 F9 0F 00 00");
            ExpectBytes(report, "mov ecx, [90000000]", high, "67 8B 0C 25 00 00 00 90");
            ExpectBytes(report, "lea rax, [90000000]", high, "67 48 8D 04 25 00 00 00 90");
            ExpectBytes(report, "lea rax, [10]", high, "48 8D 04 25 10 00 00 00");
            ExpectBytes(report, "mov ecx, gs:[90000000]", high, "65 67 8B 0C 25 00 00 00 90");
            ExpectText(report, "mov eax, [90000000]", high, "mov eax, dword ptr [0x90000000]");
            ExpectText(report, "nop dword ptr [rax+rax*1+0x0], eax", high, "nop dword ptr [rax+rax*1], eax");
            ExpectBytes(report, "nop dword ptr [rax+rax*1+0x0], eax", high, "0F 1F 04 00");
            ExpectBytes(report, "nop eax, eax", high, "0F 1F C0");
            ExpectBytes(report, "fldenv [rax]", high, "D9 20");
            ExpectBytes(report, "fnsave [rax]", high, "DD 30");
            ExpectBytes(report, "call far fword ptr [rcx]", high, "FF 19");
            ExpectBytes(report, "vfmaddps xmm0, xmm1, xmm2, xmm3", high, "C4 E3 71 68 C2 30");
        }

        void TestBranches(Report& report)
        {
            const AssembleOptions near = Mode64(0x1000);
            ExpectBytes(report, "jmp 1010", near, "EB 0E");
            ExpectBytes(report, "jmp 1000", near, "EB FE");
            ExpectBytes(report, "jmp 1081", near, "EB 7F");
            ExpectBytes(report, "jmp 1082", near, "E9 7D 00 00 00");
            ExpectBytes(report, "jmp F82", near, "EB 80");
            ExpectBytes(report, "jmp F80", near, "E9 7B FF FF FF");
            ExpectBytes(report, "jmp 2000", near, "E9 FB 0F 00 00");
            ExpectBytes(report, "jmp short 1010", near, "EB 0E");
            ExpectBytes(report, "jmp near 1010", near, "E9 0B 00 00 00");
            AssembleOptions longer = Mode64(0x1000);
            longer.LongBranches = true;
            ExpectBytes(report, "jmp 1010", longer, "E9 0B 00 00 00");
            ExpectBytes(report, "je 1010", longer, "0F 84 0A 00 00 00");
            ExpectBytes(report, "call 1010", longer, "E8 0B 00 00 00");
            ExpectBytes(report, "jmp short 1010", longer, "EB 0E");
            ExpectBytes(report, "loop 1010", longer, "E2 0E");
            AssembleOptions longer32 = Mode32(0x401000);
            longer32.LongBranches = true;
            ExpectBytes(report, "jmp 401010", longer32, "E9 0B 00 00 00");
            ExpectBytes(report, "jne 401010", longer32, "0F 85 0A 00 00 00");
            ExpectBytes(report, "je 1010", near, "74 0E");
            ExpectBytes(report, "jz short 1010", near, "74 0E");
            ExpectBytes(report, "jne 2000", near, "0F 85 FA 0F 00 00");
            ExpectBytes(report, "je F00", near, "0F 84 FA FE FF FF");
            ExpectBytes(report, "ja 1010", near, "77 0E");
            ExpectBytes(report, "jg 1010", near, "7F 0E");
            ExpectBytes(report, "jge 1010", near, "7D 0E");
            ExpectBytes(report, "jae 1010", near, "73 0E");
            ExpectBytes(report, "jc 1010", near, "72 0E");
            ExpectBytes(report, "jnc 1010", near, "73 0E");
            ExpectBytes(report, "jpe 1010", near, "7A 0E");
            ExpectBytes(report, "jpo 1010", near, "7B 0E");
            ExpectBytes(report, "jna 1010", near, "76 0E");
            ExpectBytes(report, "jng 1010", near, "7E 0E");
            ExpectBytes(report, "jnge 1010", near, "7C 0E");
            ExpectBytes(report, "jnae 1010", near, "72 0E");
            ExpectBytes(report, "call 1010", near, "E8 0B 00 00 00");
            ExpectBytes(report, "call F00", near, "E8 FB FE FF FF");
            ExpectBytes(report, "jrcxz 1010", near, "E3 0E");
            ExpectBytes(report, "jecxz 1010", near, "67 E3 0D");
            ExpectBytes(report, "loop 1010", near, "E2 0E");
            ExpectBytes(report, "loope 1010", near, "E1 0E");
            ExpectBytes(report, "loopz 1010", near, "E1 0E");
            ExpectBytes(report, "loopne 1010", near, "E0 0E");
            ExpectBytes(report, "loopnz 1010", near, "E0 0E");
            ExpectBytes(report, "xbegin 1010", near, "C7 F8 0A 00 00 00");
            const AssembleOptions far = Mode64(0x7FF600001000);
            ExpectBytes(report, "jmp 1000", far, "FF 25 00 00 00 00 00 10 00 00 00 00 00 00");
            ExpectBytes(report, "call 1000", far, "FF 15 02 00 00 00 EB 08 00 10 00 00 00 00 00 00");
            ExpectBytes(report, "je 1000", far, "75 0E FF 25 00 00 00 00 00 10 00 00 00 00 00 00");
            ExpectBytes(report, "jne 1000", far, "74 0E FF 25 00 00 00 00 00 10 00 00 00 00 00 00");
            ExpectBytes(report, "ja 1000", far, "76 0E FF 25 00 00 00 00 00 10 00 00 00 00 00 00");
            ExpectBytes(report, "jl 1000", far, "7D 0E FF 25 00 00 00 00 00 10 00 00 00 00 00 00");
            ExpectBytes(report, "jmp 7FF600002000", far, "E9 FB 0F 00 00");
            ExpectBytes(report, "jmp far 1010", near, "FF 25 00 00 00 00 10 10 00 00 00 00 00 00");
            AssembleOptions noFar = far;
            noFar.AllowFarBranches = false;
            ExpectError(report, "jmp 1000", noFar, AsmError::OutOfRange);
            ExpectError(report, "jmp short 2000", near, AsmError::OutOfRange);
            ExpectError(report, "loop 2000", near, AsmError::OutOfRange);
            ExpectError(report, "jrcxz 7FF600002000", far, AsmError::OutOfRange);
            ExpectError(report, "call short 1010", near, AsmError::BadOperand);
            ExpectError(report, "jmp near 1000", far, AsmError::OutOfRange);
            ExpectPendingBound(report, "jmp near later", far, 5, { 0x7FF600001010, 0x7FF600100000 });
            ExpectBytes(report, "ret far", near, "CB");
            ExpectBytes(report, "ret far 8", near, "CA 08 00");
        }

        void TestStringsAndPrefixes(Report& report)
        {
            const AssembleOptions x64 = Mode64();
            ExpectBytes(report, "rep movsb", x64, "F3 A4");
            ExpectBytes(report, "rep stosq", x64, "F3 48 AB");
            ExpectBytes(report, "repe cmpsb", x64, "F3 A6");
            ExpectBytes(report, "repz cmpsb", x64, "F3 A6");
            ExpectBytes(report, "rep cmpsb", x64, "F3 A6");
            ExpectBytes(report, "repne scasb", x64, "F2 AE");
            ExpectBytes(report, "repnz scasb", x64, "F2 AE");
            ExpectBytes(report, "movsb", x64, "A4");
            ExpectBytes(report, "movsw", x64, "66 A5");
            ExpectBytes(report, "movsd", x64, "A5");
            ExpectBytes(report, "movsq", x64, "48 A5");
            ExpectBytes(report, "stosq", x64, "48 AB");
            ExpectBytes(report, "lodsd", x64, "AD");
            ExpectBytes(report, "scasb", x64, "AE");
            ExpectBytes(report, "cmpsb", x64, "A6");
        }

        void TestVectorAndFloat(Report& report)
        {
            const AssembleOptions x64 = Mode64();
            ExpectBytes(report, "movss xmm0,[rax+10]", x64, "F3 0F 10 40 10");
            ExpectBytes(report, "movsd xmm0,xmm1", x64, "F2 0F 10 C1");
            ExpectBytes(report, "movsd xmm0,[rax]", x64, "F2 0F 10 00");
            ExpectBytes(report, "movsd qword ptr [rsp+8], xmm2", x64, "F2 0F 11 54 24 08");
            ExpectBytes(report, "movaps xmm1, xmm2", x64, "0F 28 CA");
            ExpectBytes(report, "pxor xmm0, xmm0", x64, "66 0F EF C0");
            ExpectBytes(report, "movdqu xmm0, [rax]", x64, "F3 0F 6F 00");
            ExpectBytes(report, "cvtsi2sd xmm0, eax", x64, "F2 0F 2A C0");
            ExpectBytes(report, "cvtsi2sd xmm0, qword ptr [rax]", x64, "F2 48 0F 2A 00");
            ExpectBytes(report, "cvttss2si eax, xmm0", x64, "F3 0F 2C C0");
            ExpectBytes(report, "vaddps ymm0,ymm1,ymm2", x64, "C5 F4 58 C2");
            ExpectBytes(report, "vmovups ymm0, [rax]", x64, "C5 FC 10 00");
            ExpectBytes(report, "vxorps xmm0, xmm0, xmm0", x64, "C5 F8 57 C0");
            ExpectBytes(report, "vaddps zmm0,zmm1,zmm2", x64, "62 F1 74 48 58 C2");
            ExpectBytes(report, "vaddps zmm0{k1},zmm1,zmm2", x64, "62 F1 74 49 58 C2");
            ExpectBytes(report, "vaddps zmm0 {k1}{z}, zmm1, zmm2", x64, "62 F1 74 C9 58 C2");
            ExpectBytes(report, "fstp st(1)", x64, "DD D9");
            ExpectBytes(report, "fld st(0)", x64, "D9 C0");
            ExpectBytes(report, "fld st", x64, "D9 C0");
            ExpectBytes(report, "fxch", x64, "D9 C9");
            ExpectBytes(report, "faddp", x64, "DE C1");
            ExpectBytes(report, "fadd", x64, "DE C1");
            ExpectBytes(report, "fadd st(0), st(1)", x64, "D8 C1");
            ExpectBytes(report, "fadd st1", x64, "D8 C1");
            ExpectBytes(report, "fadd st(1), st(0)", x64, "DC C1");
            ExpectBytes(report, "faddp st1", x64, "DE C1");
            ExpectBytes(report, "fcomi st1", x64, "DB F1");
            ExpectBytes(report, "fcom st0, st1", x64, "D8 D1");
        }

        void TestData(Report& report)
        {
            const AssembleOptions x64 = Mode64();
            ExpectBytes(report, "db 'abc',0", x64, "61 62 63 00");
            ExpectBytes(report, "db \"hi\",0A", x64, "68 69 0A");
            ExpectBytes(report, "db 90 90 90", x64, "90 90 90");
            ExpectBytes(report, "db 1,2, 3", x64, "01 02 03");
            ExpectBytes(report, "db 'a' 'b' 0", x64, "61 62 00");
            ExpectBytes(report, "db '//',0 // comment", x64, "2F 2F 00");
            ExpectBytes(report, "db -1", x64, "FF");
            ExpectBytes(report, "db #200", x64, "C8");
            ExpectBytes(report, "dw 1234", x64, "34 12");
            ExpectBytes(report, "dw 1,2", x64, "01 00 02 00");
            ExpectBytes(report, "dw -1", x64, "FF FF");
            ExpectBytes(report, "dd 12345678", x64, "78 56 34 12");
            ExpectBytes(report, "dd 'ab'", x64, "61 62 00 00");
            ExpectBytes(report, "dd 1+2*3", x64, "07 00 00 00");
            ExpectBytes(report, "dd #10, (float)100", x64, "0A 00 00 00 00 00 C8 42");
            ExpectBytes(report, "dd (float)1", x64, "00 00 80 3F");
            ExpectBytes(report, "dd (float)1.5", x64, "00 00 C0 3F");
            ExpectBytes(report, "dd (float)-1", x64, "00 00 80 BF");
            ExpectBytes(report, "dd (float)1e3", x64, "00 00 7A 44");
            ExpectBytes(report, "dq 1", x64, "01 00 00 00 00 00 00 00");
            ExpectBytes(report, "dq -2", x64, "FE FF FF FF FF FF FF FF");
            ExpectBytes(report, "dq (double)1", x64, "00 00 00 00 00 00 F0 3F");
            ExpectBytes(report, "dq (double)2.5", x64, "00 00 00 00 00 00 04 40");
            ExpectBytes(report, "nop", x64, "90");
            ExpectBytes(report, "nop 1", x64, "90");
            ExpectBytes(report, "nop 2", x64, "66 90");
            ExpectBytes(report, "nop 5", x64, "0F 1F 44 00 00");
            ExpectBytes(report, "nop A", x64, "66 0F 1F 84 00 00 00 00 00 90");
            ExpectBytes(report, "nop #3", x64, "0F 1F 00");
        }

        void TestLegacyMode(Report& report)
        {
            const AssembleOptions x86 = Mode32();
            ExpectBytes(report, "mov eax,[ebp+8]", x86, "8B 45 08");
            ExpectBytes(report, "push ebp", x86, "55");
            ExpectBytes(report, "pushad", x86, "60");
            ExpectBytes(report, "popad", x86, "61");
            ExpectBytes(report, "pushfd", x86, "9C");
            ExpectBytes(report, "popfd", x86, "9D");
            ExpectBytes(report, "inc eax", x86, "40");
            ExpectBytes(report, "dec ecx", x86, "49");
            ExpectBytes(report, "call 401000", x86, "E8 FB FF FF FF");
            ExpectBytes(report, "jmp 400000", x86, "E9 FB EF FF FF");
            ExpectBytes(report, "jmp 401010", x86, "EB 0E");
            ExpectBytes(report, "je 401100", x86, "0F 84 FA 00 00 00");
            ExpectBytes(report, "jmp FFFFFFF0", Mode32(0x10), "EB DE");
            ExpectBytes(report, "jmp 10", Mode32(0xFFFFFFF0), "EB 1E");
            ExpectBytes(report, "mov eax,[401000]", x86, "A1 00 10 40 00");
            ExpectBytes(report, "mov ecx,[401000]", x86, "8B 0D 00 10 40 00");
            ExpectBytes(report, "mov ecx,[FFFFFFF0]", x86, "8B 0D F0 FF FF FF");
            ExpectBytes(report, "mov eax, fs:[30]", x86, "64 A1 30 00 00 00");
            ExpectBytes(report, "mov eax, dword ptr fs:[18]", x86, "64 A1 18 00 00 00");
            ExpectBytes(report, "mov ecx, fs:[0]", x86, "64 8B 0D 00 00 00 00");
            ExpectBytes(report, "mov eax, [eax+80000000]", x86, "8B 80 00 00 00 80");
            ExpectBytes(report, "push 12345678", x86, "68 78 56 34 12");
            ExpectBytes(report, "push FFFFFFFF", x86, "6A FF");
            ExpectBytes(report, "push 80000000", x86, "68 00 00 00 80");
            ExpectBytes(report, "fld dword ptr [eax]", x86, "D9 00");
            ExpectBytes(report, "jecxz 401010", x86, "E3 0E");
            ExpectBytes(report, "loop 401010", x86, "E2 0E");
            ExpectBytes(report, "lea eax, [ecx+edx*4+10]", x86, "8D 44 91 10");
            ExpectBytes(report, "mov ax, word ptr [esi]", x86, "66 8B 06");
            ExpectBytes(report, "rep movsd", x86, "F3 A5");
            ExpectBytes(report, "call dword ptr [eax]", x86, "FF 10");
            ExpectBytes(report, "jmp [eax]", x86, "FF 20");
            ExpectBytes(report, "mov eax, es:[eax]", x86, "26 8B 00");
            ExpectBytes(report, "jmp far 33:401000", x86, "EA 00 10 40 00 33 00");
            ExpectBytes(report, "jmp 0x33:0x401000", x86, "EA 00 10 40 00 33 00");
            ExpectBytes(report, "call far 23:401000", x86, "9A 00 10 40 00 23 00");
            ExpectBytes(report, "jmp far fword ptr [eax]", x86, "FF 28");
            ExpectBytes(report, "bndldx bnd0, [ebp+edx+39]", x86, "0F 1A 44 15 39");
        }

        void TestCheatEngineHabits(Report& report)
        {
            const std::vector<Symbol> symbols = {
                { "My-Game-Win64.exe", Known(0x400000) },
                { "a", Known(5) },
                { "b", Known(3) },
            };
            const AssembleOptions image = WithSymbols(Mode64(0x140001000), symbols);
            ExpectBytes(report, "mov eax,My-Game-Win64.exe+10", image, "B8 10 00 40 00");
            ExpectBytes(report, "mov eax,a-b", image, "B8 02 00 00 00");
            ExpectBytes(report, "mov eax,(int)10", Mode64(), "B8 0A 00 00 00");
            ExpectBytes(report, "mov eax,(int)-1", Mode64(), "B8 FF FF FF FF");
            AssembleOptions dword = Mode64();
            dword.DefaultToDword = true;
            ExpectBytes(report, "mov [rax],(int)0", dword, "C7 00 00 00 00 00");
            ExpectBytes(report, "mov [rbx+FC],1", dword, "C7 83 FC 00 00 00 01 00 00 00");
            ExpectBytes(report, "mov byte ptr [rax],1", dword, "C6 00 01");
        }

        void TestSymbols(Report& report)
        {
            const std::vector<Symbol> symbols = {
                { "target", Known(0x140001010) },
                { "beef", Known(0x140002000) },
                { "dead", Pending() },
                { "game.exe", Known(0x140000000) },
                { "kernel32.Sleep", Known(0x7FF812340000) },
                { "data_1", Known(0x140003000) },
            };
            const AssembleOptions image = WithSymbols(Mode64(0x140001000), symbols, true);
            ExpectBytes(report, "jmp target", image, "EB 0E");
            ExpectBytes(report, "jmp target+10", image, "EB 1E");
            ExpectBytes(report, "jmp beef", image, "E9 FB 0F 00 00");
            ExpectBytes(report, "mov eax, cafe", image, "B8 FE CA 00 00");
            ExpectBytes(report, "mov eax, [\"game.exe\"+1234]", image, "8B 05 2E 02 00 00");
            ExpectBytes(report, "call \"game.exe\"+2000", image, "E8 FB 0F 00 00");
            ExpectBytes(report, "call kernel32.Sleep", image, "FF 15 02 00 00 00 EB 08 00 00 34 12 F8 7F 00 00");
            ExpectBytes(report, "mov rax, [data_1]", image, "48 8B 05 F9 1F 00 00");
            ExpectBytes(report, "mov rax, data_1", image, "48 B8 00 30 00 40 01 00 00 00");
            ExpectBytes(report, "lea rcx, [data_1+8]", image, "48 8D 0D 01 20 00 00");
            ExpectBytes(report, "mov eax, [rbx+beef-140001FF0]", image, "8B 43 10");
            ExpectBytes(report, "dq kernel32.Sleep", image, "00 00 34 12 F8 7F 00 00");
            const AssembleResult pending = Asm::Assemble("mov eax, dead", image);
            ++report.Cases;
            if (!pending.Ok || !pending.UsedPending || pending.Bytes.size() != 5)
            {
                Failed(report, image, "mov eax, dead", "5 pending bytes", Outcome(pending));
            }
            ExpectError(report, "jmp foo", image, AsmError::UnknownSymbol, "Unknown symbol \"foo\".");
            ExpectError(report, "mov eax, [foo+10]", image, AsmError::UnknownSymbol, "Unknown symbol \"foo\".");
            ExpectError(report, "mov eax, \"nothere.dll\"", image, AsmError::UnknownSymbol, "Unknown symbol \"nothere.dll\".");
            ExpectError(report, "mov eax, dead", WithSymbols(Mode64(0x140001000), symbols, false), AsmError::UnknownSymbol);
            ExpectBytes(report, "mov eax, 10", AssembleOptions{}, "B8 10 00 00 00");

            const AssembleOptions x64 = Mode64(0x140001000);
            const std::vector<std::uint64_t> finals64 = { 0x140001010, 0x140001080, 0x140100000, 0x140001000, 0x10, 0x90000000, 0x7FF812340000, 0xFFFFFFFFFFFFFFF0ull };
            ExpectPendingBound(report, "jmp later", x64, 14, finals64);
            ExpectPendingBound(report, "call later", x64, 16, finals64);
            ExpectPendingBound(report, "je later", x64, 16, finals64);
            ExpectPendingBound(report, "jmp short later", x64, 2, finals64);
            ExpectPendingBound(report, "loop later", x64, 2, finals64);
            ExpectPendingBound(report, "xbegin later", x64, 6, finals64);
            ExpectPendingBound(report, "mov rax, later", x64, 10, finals64);
            ExpectPendingBound(report, "mov eax, later", x64, 5, finals64);
            ExpectPendingBound(report, "push later", x64, 5, finals64);
            ExpectPendingBound(report, "add rax, later", x64, 6, finals64);
            ExpectPendingBound(report, "cmp byte ptr [rax], later", x64, 3, finals64);
            ExpectPendingBound(report, "mov eax, [later]", x64, 9, finals64);
            ExpectPendingBound(report, "mov rcx, [later]", x64, 9, finals64);
            ExpectPendingBound(report, "lea rax, [later]", x64, 9, finals64);
            ExpectPendingBound(report, "lea eax, [later]", x64, 8, finals64);
            ExpectPendingBound(report, "mov [rax+later], ecx", x64, 6, finals64);
            ExpectPendingBound(report, "mov qword ptr [later], later", x64, 13, finals64);
            ExpectPendingBound(report, "mov al, [later]", x64, 9, finals64);
            ExpectPendingBound(report, "mov ecx, gs:[later]", x64, 9, finals64);
            ExpectPendingBound(report, "dq later", x64, 8, finals64);
            ExpectPendingBound(report, "db later", x64, 1, { 0x10, 0xFF });
            AssembleOptions noFar = x64;
            noFar.AllowFarBranches = false;
            ExpectPendingBound(report, "jmp later", noFar, 5, finals64);
            ExpectPendingBound(report, "je later", noFar, 6, finals64);
            const AssembleOptions x86 = Mode32(0x401000);
            const std::vector<std::uint64_t> finals32 = { 0x401010, 0x401080, 0x500000, 0x10, 0xFFFFFFF0 };
            ExpectPendingBound(report, "jmp later", x86, 5, finals32);
            ExpectPendingBound(report, "je later", x86, 6, finals32);
            ExpectPendingBound(report, "call later", x86, 5, finals32);
            ExpectPendingBound(report, "mov eax, [later]", x86, 5, finals32);
            ExpectPendingBound(report, "mov ecx, [later]", x86, 6, finals32);
            ExpectPendingBound(report, "push later", x86, 5, finals32);
        }

        void TestErrors(Report& report)
        {
            const AssembleOptions x64 = Mode64(0x1000);
            ExpectError(report, "", x64, AsmError::Empty);
            ExpectError(report, "   \t  ", x64, AsmError::Empty);
            ExpectError(report, "   // only a comment", x64, AsmError::Empty);
            ExpectError(report, "movx eax, 1", x64, AsmError::UnknownMnemonic, "Unknown instruction \"movx\".");
            ExpectError(report, "mov al, rbx", x64, AsmError::BadOperand, "\"rbx\" does not fit \"mov al, rbx\".");
            ExpectError(report, "mov al,rbx", x64, AsmError::BadOperand, "\"rbx\" does not fit \"mov al, rbx\".");
            ExpectError(report, "mov byte ptr [rax], ebx", x64, AsmError::BadOperand, "\"ebx\" does not fit \"mov byte ptr [rax], ebx\".");
            ExpectError(report, "mov al, 1234", x64, AsmError::OutOfRange);
            ExpectError(report, "add rax, FFFFFFFF", x64, AsmError::OutOfRange);
            ExpectError(report, "push FFFFFFFF", x64, AsmError::OutOfRange);
            ExpectError(report, "mov [rax], 1", x64, AsmError::BadOperand, "\"mov [rax], 1\" needs a size: write byte ptr, word ptr, dword ptr or qword ptr.");
            ExpectError(report, "inc [rax]", x64, AsmError::BadOperand);
            ExpectError(report, "movzx eax, [rcx]", x64, AsmError::BadOperand);
            ExpectError(report, "fld [rax]", x64, AsmError::BadOperand, "\"fld [rax]\" needs a size: write dword ptr, qword ptr or tword ptr.");
            ExpectError(report, "cvtsi2sd xmm0, [rax]", x64, AsmError::BadOperand);
            ExpectError(report, "shl [rax], cl", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax, [rax", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, [rax]]", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, []", x64, AsmError::Syntax);
            ExpectError(report, "mov eax,", x64, AsmError::Syntax);
            ExpectError(report, "mov ,eax", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, 'abc", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, 1 2", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, ~1", x64, AsmError::Syntax);
            ExpectError(report, "lock", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, $xyz", x64, AsmError::BadNumber);
            ExpectError(report, "mov eax, #12a", x64, AsmError::BadNumber);
            ExpectError(report, "mov eax, 12xz", x64, AsmError::BadNumber);
            ExpectError(report, "mov eax, ebx+1", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax, [rax+rbx*3]", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax, [rax+rbx+rcx]", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax, [rax-rbx]", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax, [rax*rbx]", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax, [rax+80000000]", x64, AsmError::OutOfRange);
            ExpectError(report, "mov rbx, [7FF000000000]", x64, AsmError::OutOfRange);
            ExpectError(report, "mov rbx, gs:[7FF000000000]", x64, AsmError::OutOfRange);
            ExpectError(report, "pushad", x64, AsmError::BadOperand, "\"pushad\" is not available in 64-bit code.");
            ExpectError(report, "push eax", x64, AsmError::BadOperand);
            ExpectError(report, "mov eax", x64, AsmError::BadOperand);
            ExpectError(report, "mov rax, [eax+rbx]", x64, AsmError::BadOperand);
            ExpectError(report, "db 100", x64, AsmError::OutOfRange);
            ExpectError(report, "dw 10000", x64, AsmError::OutOfRange);
            ExpectError(report, "dd (double)1", x64, AsmError::OutOfRange);
            ExpectError(report, "db", x64, AsmError::Syntax);
            ExpectError(report, "db 1,", x64, AsmError::Syntax);
            ExpectError(report, "dd 1 2", x64, AsmError::Syntax);
            ExpectError(report, "dd (float)abc", x64, AsmError::BadNumber);
            ExpectError(report, "nop 0", x64, AsmError::OutOfRange);
            ExpectError(report, "nop 100000", x64, AsmError::OutOfRange);
            ExpectError(report, "mov eax, fs:30", x64, AsmError::Syntax);
            ExpectError(report, "fld st(8)", x64, AsmError::BadOperand);
            ExpectError(report, "vaddps zmm0 {k9}, zmm1, zmm2", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, ptr", x64, AsmError::Syntax);
            ExpectError(report, "mov eax, 1 % 2", x64, AsmError::Syntax);
            ExpectError(report, "rep", x64, AsmError::Syntax);
            ExpectError(report, "jcxz 1010", x64, AsmError::BadOperand, "\"jcxz 1010\" is not available in 64-bit code.");
            ExpectError(report, "jrcxz 401010", Mode32(), AsmError::BadOperand, "\"jrcxz 401010\" is only available in 64-bit code.");
            ExpectError(report, "lock mov eax, ebx", x64, AsmError::BadOperand, "The prefix does not fit \"lock mov eax, ebx\".");
            const AssembleOptions x86 = Mode32();
            ExpectError(report, "mov rax, 1", x86, AsmError::BadOperand);
            ExpectError(report, "jmp 7FF000000000", x86, AsmError::OutOfRange);
            ExpectError(report, "mov eax, [7FF000000000]", x86, AsmError::OutOfRange);
            ExpectError(report, "jmp far 1000", x86, AsmError::BadOperand);
            ExpectError(report, "jmp near 33:401000", x86, AsmError::BadOperand);
            ExpectError(report, "jmp 10000:401000", x86, AsmError::OutOfRange);
            ExpectError(report, "jmp far 33:401000", x64, AsmError::BadOperand, "\"jmp far 33:401000\" uses a far pointer, which only exists in 32-bit code.");
        }

        void TestRoundTrip(Report& report)
        {
            const AssembleOptions x64 = Mode64(0x140001000);
            ExpectText(report, "mov rax, [rax+rbx*8+10]", x64, "mov rax, qword ptr [rax+rbx*8+0x10]");
            ExpectText(report, "lea rcx, [rsp+20]", x64, "lea rcx, qword ptr [rsp+0x20]");
            ExpectText(report, "mov eax, [rbp-8]", x64, "mov eax, dword ptr [rbp-0x08]");
            ExpectText(report, "mov r15d, [r14+r13*2-10]", x64, "mov r15d, dword ptr [r14+r13*2-0x10]");
            ExpectText(report, "xor eax, eax", x64, "xor eax, eax");
            ExpectText(report, "push rbx", x64, "push rbx");
            ExpectText(report, "sub rsp, 28", x64, "sub rsp, 0x28");
            ExpectText(report, "call qword ptr [rax]", x64, "call qword ptr [rax]");
            ExpectText(report, "mov rax, [140002000]", x64, "mov rax, qword ptr [0x140002000]");
            ExpectText(report, "lea rax, [rip+100]", x64, "lea rax, qword ptr [0x140001107]");
            ExpectText(report, "mov eax, [rip]", x64, "mov eax, dword ptr [0x140001006]");
            ExpectText(report, "jmp 140001010", x64, "jmp 0x140001010");
            ExpectText(report, "je 140002000", x64, "jz 0x140002000");
            ExpectText(report, "call 140000F00", x64, "call 0x140000F00");
            ExpectText(report, "jrcxz 140001010", x64, "jrcxz 0x140001010");
            ExpectText(report, "loopne 140001010", x64, "loopne 0x140001010");
            ExpectText(report, "xbegin 140001100", x64, "xbegin 0x140001100");
            ExpectText(report, "lock xadd [rcx], eax", x64, "lock xadd dword ptr [rcx], eax");
            ExpectText(report, "rep movsb", x64, "rep movsb");
            ExpectText(report, "movss xmm0, [rax+10]", x64, "movss xmm0, dword ptr [rax+0x10]");
            ExpectText(report, "movsd xmm0, xmm1", x64, "movsd xmm0, xmm1");
            ExpectText(report, "movsd", x64, "movsd");
            ExpectText(report, "vaddps ymm0, ymm1, ymm2", x64, "vaddps ymm0, ymm1, ymm2");
            ExpectText(report, "cmovg rax, rcx", x64, "cmovnle rax, rcx");
            ExpectText(report, "setae al", x64, "setnb al");
            ExpectText(report, "mov rax, gs:[60]", x64, "mov rax, qword ptr gs:[0x60]");
            ExpectText(report, "fld tword ptr [rax]", x64, "fld tbyte ptr [rax]");
            ExpectText(report, "movzx eax, word ptr [rcx]", x64, "movzx eax, word ptr [rcx]");
            ExpectText(report, "imul rax, [rbx], 7F", x64, "imul rax, qword ptr [rbx], 0x7F");
            ExpectText(report, "shl rax, cl", x64, "shl rax, cl");
            ExpectText(report, "test byte ptr [rax+rcx*2+80], 1", x64, "test byte ptr [rax+rcx*2+0x80], 0x01");
            ExpectText(report, "pshufd xmm0, xmm1, 1B", x64, "pshufd xmm0, xmm1, 0x1B");
            ExpectText(report, "vmovdqu ymm1, [rsp+40]", x64, "vmovdqu ymm1, ymmword ptr [rsp+0x40]");
            ExpectText(report, "vpaddd zmm0 {k1}{z}, zmm1, zmm2", x64, "vpaddd zmm0 {k1} {z}, zmm1, zmm2");
            ExpectText(report, "vaddps zmm0, zmm1, [rax]{1to16}", x64, "vaddps zmm0, zmm1, dword ptr [rax] {1to16}");
            ExpectText(report, "vaddps zmm0, zmm1, zmm2, {rn-sae}", x64, "vaddps zmm0, zmm1, zmm2 {rn-sae}");
            ExpectText(report, "vmovups [rax]{k2}, zmm3", x64, "vmovups zmmword ptr [rax] {k2}, zmm3");
            ExpectText(report, "vpcmpeqd k1, zmm0, zmm1", x64, "vpcmpeqd k1, zmm0, zmm1");
            ExpectText(report, "vfmadd231ps ymm0, ymm1, [rax]", x64, "vfmadd231ps ymm0, ymm1, ymmword ptr [rax]");
            ExpectText(report, "vpbroadcastd zmm0, eax", x64, "vpbroadcastd zmm0, eax");
            ExpectText(report, "vgatherdps ymm0, [rax+ymm1*4], ymm2", x64, "vgatherdps ymm0, dword ptr [rax+ymm1*4], ymm2");
            ExpectText(report, "kmovw k1, eax", x64, "kmovw k1, eax");
            ExpectText(report, "mov cr3, rax", x64, "mov cr3, rax");
            ExpectText(report, "mov rax, dr7", x64, "mov rax, dr7");
            ExpectText(report, "mov ax, ds", x64, "mov ax, ds");
            ExpectText(report, "push 12345678", x64, "push 0x12345678");
            ExpectText(report, "cmpxchg16b [rdi]", x64, "cmpxchg16b xmmword ptr [rdi]");
            ExpectText(report, "prefetcht0 [rax]", x64, "prefetcht0 byte ptr [rax]");
            ExpectText(report, "xchg [rax], ecx", x64, "xchg dword ptr [rax], ecx");
            ExpectText(report, "bt dword ptr [rax], 3", x64, "bt dword ptr [rax], 0x03");
            ExpectText(report, "enter 10, 0", x64, "enter 0x10, 0x00");
            ExpectText(report, "in al, dx", x64, "in al, dx");
            ExpectText(report, "out 80, al", x64, "out 0x80, al");
            ExpectText(report, "int 3", x64, "int 0x03");
            ExpectText(report, "mov word ptr [rax], 1234", x64, "mov word ptr [rax], 0x1234");
            ExpectText(report, "and qword ptr [rbp+10], FFFFFFFFFFFFFFF0", x64, "and qword ptr [rbp+0x10], 0xFFFFFFFFFFFFFFF0");
            ExpectText(report, "movq xmm0, rax", x64, "movq xmm0, rax");
            ExpectText(report, "cvtsi2ss xmm1, dword ptr [rcx]", x64, "cvtsi2ss xmm1, dword ptr [rcx]");
            ExpectText(report, "fstp st(1)", x64, "fstp st1");
            ExpectText(report, "fmul st(0), st(3)", x64, "fmul st0, st3");
            ExpectText(report, "fistp qword ptr [rsp]", x64, "fistp qword ptr [rsp]");
            ExpectText(report, "movsxd rcx, dword ptr [rax]", x64, "movsxd rcx, dword ptr [rax]");
            ExpectText(report, "crc32 eax, byte ptr [rcx]", x64, "crc32 eax, byte ptr [rcx]");
            ExpectText(report, "crc32 eax, [rcx]", x64, "crc32 eax, dword ptr [rcx]");
            ExpectText(report, "popcnt rax, rcx", x64, "popcnt rax, rcx");
            ExpectText(report, "tzcnt eax, ecx", x64, "tzcnt eax, ecx");
            ExpectText(report, "rdtscp", x64, "rdtscp");
            ExpectText(report, "xgetbv", x64, "xgetbv");
            ExpectText(report, "vzeroupper", x64, "vzeroupper");
            ExpectText(report, "mov al, [rax]", x64, "mov al, byte ptr [rax]");
            ExpectText(report, "mov [rdx+8], r9w", x64, "mov word ptr [rdx+0x08], r9w");
            ExpectText(report, "movups xmm9, [r8+r9*8+12345678]", x64, "movups xmm9, xmmword ptr [r8+r9*8+0x12345678]");
            ExpectText(report, "andps xmm0, [140003000]", x64, "andps xmm0, xmmword ptr [0x140003000]");
            ExpectText(report, "sub dword ptr [rsp+rbp*4+#16], -#5", x64, "sub dword ptr [rsp+rbp*4+0x10], 0xFFFFFFFB");
            ExpectText(report, "mov rax, 7FFFFFFF", x64, "mov rax, 0x7FFFFFFF");
            ExpectText(report, "mov rax, -1", x64, "mov rax, 0xFFFFFFFFFFFFFFFF");
            ExpectText(report, "bnd jmp rax", x64, "bnd jmp rax");
            ExpectText(report, "notrack call rax", x64, "notrack call rax");
            ExpectText(report, "xacquire lock add dword ptr [rax], 1", x64, "xacquire lock add dword ptr [rax], 0x01");
            ExpectText(report, "jmp far [rax]", x64, "jmp far fword ptr [rax]");
            ExpectText(report, "bndldx bnd0, [rbp+rdx+39]", x64, "bndldx bnd0, [rbp+rdx+0x39]");
            ExpectText(report, "bndstx [rdx+rdi+20], bnd3", x64, "bndstx [rdx+rdi+0x20], bnd3");
            ExpectText(report, "vfmaddps xmm0, xmm1, xmm2, [rax]", x64, "vfmaddps xmm0, xmm1, xmm2, xmmword ptr [rax]");
            ExpectText(report, "vfmaddps xmm0, xmm1, [rax], xmm3", x64, "vfmaddps xmm0, xmm1, xmmword ptr [rax], xmm3");
            const AssembleOptions x86 = Mode32(0x401000);
            ExpectText(report, "mov eax, [ebp+8]", x86, "mov eax, dword ptr [ebp+0x08]");
            ExpectText(report, "push ebp", x86, "push ebp");
            ExpectText(report, "call 401234", x86, "call 0x401234");
            ExpectText(report, "jmp 400000", x86, "jmp 0x400000");
            ExpectText(report, "mov eax, fs:[30]", x86, "mov eax, dword ptr fs:[0x30]");
            ExpectText(report, "fld dword ptr [eax]", x86, "fld dword ptr [eax]");
            ExpectText(report, "pushad", x86, "pushad");
            ExpectText(report, "mov eax, [401000]", x86, "mov eax, dword ptr [0x401000]");
            ExpectText(report, "lea eax, [ecx+edx*4+10]", x86, "lea eax, dword ptr [ecx+edx*4+0x10]");
            ExpectText(report, "jecxz 401010", x86, "jecxz 0x401010");
            ExpectText(report, "inc eax", x86, "inc eax");
            ExpectText(report, "mov ax, word ptr [esi]", x86, "mov ax, word ptr [esi]");
            ExpectText(report, "push FFFFFFFF", x86, "push 0xFFFFFFFF");
            ExpectText(report, "mov eax, [FFFFFFF0]", x86, "mov eax, dword ptr [0xFFFFFFF0]");
            ExpectText(report, "mov dword ptr [esp+4], 0", x86, "mov dword ptr [esp+0x04], 0x00");
            ExpectText(report, "movd xmm0, [ecx]", x86, "movd xmm0, dword ptr [ecx]");
            ExpectText(report, "jmp far [eax]", x86, "jmp far fword ptr [eax]");
        }

        void TestDisassemblyAssemblesBack(Report& report)
        {
            const Disasm::Disassembler disassembler(true);
            const std::uint64_t address = 0x140001000;
            struct Case
            {
                std::vector<std::uint8_t> Bytes;
                const char* Text;
            };
            const Case cases[] = {
                { { 0xFF, 0x8F, 0xF8, 0x02, 0x00, 0x00 }, "dec dword ptr [rdi+2F8]" },
                { { 0xFF, 0x00 }, "inc dword ptr [rax]" },
                { { 0x48, 0xFF, 0x08 }, "dec qword ptr [rax]" },
                { { 0x66, 0xFF, 0x00 }, "inc word ptr [rax]" },
                { { 0xFE, 0x00 }, "inc byte ptr [rax]" },
                { { 0xF7, 0x18 }, "neg dword ptr [rax]" },
                { { 0xF7, 0x60, 0x08 }, "mul dword ptr [rax+8]" },
                { { 0xD9, 0x00 }, "fld dword ptr [rax]" },
                { { 0xDD, 0x00 }, "fld qword ptr [rax]" },
                { { 0xFF, 0x10 }, "call qword ptr [rax]" },
                { { 0xFF, 0x30 }, "push qword ptr [rax]" },
                { { 0x0F, 0x18, 0x00 }, "prefetchnta [rax]" },
                { { 0xF0, 0xFF, 0x00 }, "lock inc dword ptr [rax]" },
                { { 0x80, 0xB8, 0x5C, 0x01, 0x00, 0x00, 0x00 }, "cmp byte ptr [rax+15C],0" },
                { { 0x48, 0x8B, 0x8F, 0x40, 0x02, 0x00, 0x00 }, "mov rcx,[rdi+240]" },
            };
            for (const Case& item : cases)
            {
                ++report.Cases;
                const Disasm::Instruction decoded = disassembler.Decode(address, item.Bytes.data(), item.Bytes.size());
                const AssembleResult again = Asm::Assemble(decoded.Text(), Mode64(address));
                if (decoded.Text() != item.Text || !again.Ok || again.Bytes != item.Bytes)
                {
                    ++report.Failures;
                    std::printf("FAIL disassembly of %s: shown \"%s\", assembles to %s\n", HexBytes(item.Bytes).c_str(), decoded.Text().c_str(),
                        again.Ok ? HexBytes(again.Bytes).c_str() : again.Message.c_str());
                }
            }

            ++report.Cases;
            const std::vector<std::uint8_t> compare = { 0x80, 0xB8, 0x5C, 0x01, 0x00, 0x00, 0x00 };
            const Disasm::Instruction split = disassembler.Decode(address, compare.data(), compare.size());
            const std::vector<std::uint8_t> locked = { 0xF0, 0xFF, 0x00 };
            const Disasm::Instruction prefixed = disassembler.Decode(address, locked.data(), locked.size());
            if (split.Mnemonic != "cmp" || split.Operands != "byte ptr [rax+15C],0" || prefixed.Mnemonic != "lock inc" || prefixed.Operands != "dword ptr [rax]")
            {
                ++report.Failures;
                std::printf("FAIL mnemonic split: \"%s\" | \"%s\" and \"%s\" | \"%s\"\n", split.Mnemonic.c_str(), split.Operands.c_str(), prefixed.Mnemonic.c_str(), prefixed.Operands.c_str());
            }

            ++report.Cases;
            AssembleOptions loose = Mode64(address);
            loose.DefaultToDword = true;
            const AssembleResult unsized = Asm::Assemble("dec [rdi+2F8]", loose);
            if (!unsized.Ok || unsized.Bytes != std::vector<std::uint8_t>{ 0xFF, 0x8F, 0xF8, 0x02, 0x00, 0x00 })
            {
                ++report.Failures;
                std::printf("FAIL unsized dec as dword: %s\n", unsized.Ok ? HexBytes(unsized.Bytes).c_str() : unsized.Message.c_str());
            }
        }
    }

    int RunAssemblerTests()
    {
        Report report;
        TestNumbers(report);
        TestBasicEncodings(report);
        TestAliases(report);
        TestMemory(report);
        TestAbsoluteAddresses(report);
        TestBranches(report);
        TestStringsAndPrefixes(report);
        TestVectorAndFloat(report);
        TestData(report);
        TestLegacyMode(report);
        TestSymbols(report);
        TestCheatEngineHabits(report);
        TestErrors(report);
        TestRoundTrip(report);
        TestDisassemblyAssemblesBack(report);
        ++report.Cases;
        if (Asm::ErrorName(AsmError::OutOfRange) != "OutOfRange" || Asm::ErrorName(AsmError::None) != "None")
        {
            ++report.Failures;
            std::printf("FAIL ErrorName\n");
        }
        std::printf("Assembler: %d cases, %d failed\n", report.Cases, report.Failures);
        return report.Failures;
    }
}
