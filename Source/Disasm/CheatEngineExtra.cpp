#include "Disasm/CheatEngineExtra.h"

#include "Core/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Manifold::Disasm
{
    namespace
    {
        // ∑ Everything here follows Cheat Engine 7.6's source, not what looks sensible: tables compare the
        //   fourth value of splitDisassembledString with what CE printed for them. The names in the comments
        //   are CE's (disassembler.pas, byteinterpreter.pas, Assemblerunit.pas) and Free Pascal 3.2's (sysstr.inc,
        //   flt_core.inc), so a difference can be looked up there.

        enum class DataType
        {
            Byte,
            Word,
            Dword,
            Single,
            Double,
            String,
            Unicode,
            Pointer
        };

        enum ValueType
        {
            kByte = 0,
            kWord = 1,
            kDword = 2,
            kSingle = 3,
            kDouble = 4,
            kString = 5,
            kUnicode = 6
        };

        bool StartsWith(std::string_view text, std::string_view head)
        {
            return text.size() >= head.size() && text.substr(0, head.size()) == head;
        }

        bool Contains(std::string_view text, std::string_view part)
        {
            return text.find(part) != std::string_view::npos;
        }

        // Pascal's val('$' + text, value, error) without an error: hex digits only, and the value fits.
        bool PlainHex(std::string_view text, int maxDigits)
        {
            if (text.empty())
                return false;
            std::size_t first = 0;
            while (first + 1 < text.size() && text[first] == '0')
                ++first;
            for (const char c : text)
            {
                if (!IsHexDigit(c))
                    return false;
            }
            return static_cast<int>(text.size() - first) <= maxDigits;
        }

        std::uint64_t HexValue64(std::string_view text)
        {
            std::uint64_t value = 0;
            for (const char c : text)
                value = value * 16 + static_cast<std::uint64_t>(HexValue(c));
            return value;
        }

        // getNameFromAddress(value, ..., hexcharsize 8): the name, or the value as at least 8 hex digits.
        std::string NameOrHex(const CheatEngineMemory& memory, std::uint64_t value)
        {
            std::string name = memory.Name ? memory.Name(value) : std::string();
            if (PlainHex(name, 16) || name.empty())
                return Hex(value, 8);
            return name;
        }

        bool Named(const CheatEngineMemory& memory, std::uint64_t value)
        {
            const std::string name = memory.Name ? memory.Name(value) : std::string();
            return !name.empty() && !PlainHex(name, 16);
        }

        bool Committed(const CheatEngineMemory& memory, std::uint64_t address)
        {
            return address != 0 && memory.Committed && memory.Committed(address);
        }

        bool Read(const CheatEngineMemory& memory, std::uint64_t address, void* buffer, std::size_t size)
        {
            return memory.Read && memory.Read(address, buffer, size);
        }

        // has4ByteHexString: the longest run of hex characters, the rightmost of equal ones, if it has 8 or more.
        bool LongestHexRun(std::string_view text, std::string_view& run)
        {
            int count = 0;
            int best = 0;
            std::size_t bestAt = 0;
            for (std::size_t i = text.size(); i-- > 0;)
            {
                if (!IsHexDigit(text[i]))
                {
                    count = 0;
                    continue;
                }
                ++count;
                if (count > best)
                {
                    best = count;
                    bestAt = i;
                }
            }
            if (best < 8)
                return false;
            run = text.substr(bestAt, static_cast<std::size_t>(best));
            return true;
        }

        enum class Found
        {
            No,
            Yes,
            Fault
        };

        // hasAddress without a context: only the part after the first comma counts, and an offset ("+") rules
        // the operand out, so "mov [x],eax" and "cmp dword ptr [x],05" have none.
        Found HasAddress(std::string_view opcode, std::uint64_t& address, const CheatEngineMemory& memory)
        {
            if (opcode.empty() || opcode.find(' ') == std::string_view::npos)
                return Found::No;
            std::string_view part = opcode;
            if (const std::size_t comma = part.find(','); comma != std::string_view::npos)
                part = part.substr(comma + 1);
            if (Contains(part, "+"))
                return Found::No;
            std::string_view run;
            if (!LongestHexRun(part, run))
                return Found::No;
            if (!PlainHex(run, 16))
                return Found::Fault;
            address = HexValue64(run);
            return Committed(memory, address) ? Found::Yes : Found::No;
        }

        // opcodeToValueType: x87 instructions read floats unless they are integer or control ones.
        int OpcodeValueType(std::string_view opcode)
        {
            if (opcode.empty() || opcode[0] != 'f')
                return kDword;
            if (opcode.size() < 2)
                return kSingle;
            switch (opcode[1])
            {
            case 'i': return kDword;
            case 'n': return opcode.size() >= 3 && opcode[2] == 's' ? kDword : kSingle;
            case 's':
                if (opcode.size() >= 3 && opcode[2] == 't')
                {
                    if (opcode.size() >= 4 && (opcode[3] == 'c' || opcode[3] == 'e' || opcode[3] == 's'))
                        return kDword;
                }
                return kSingle;
            default: return kSingle;
            }
        }

        // TokenToRegisterbit, reduced to what splitDisassembledString makes of it: 8-bit 0, 16-bit 1, else 2.
        // spl/bpl/sil/dil are CE's "8 bit with prefix" and so count as 2.
        int RegisterValueType(std::string_view token, bool is64Bit)
        {
            std::string name(token);
            for (char& c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (name.size() < 2)
                return kDword;
            static constexpr const char* kByteNames[] = { "AL", "CL", "DL", "BL", "AH", "CH", "DH", "BH" };
            static constexpr const char* kWordNames[] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
            for (const char* entry : kByteNames)
            {
                if (name == entry)
                    return kByte;
            }
            for (const char* entry : kWordNames)
            {
                if (name == entry)
                    return kWord;
            }
            if (is64Bit && name.size() >= 3 && name[0] == 'R')
            {
                const char last = name.back();
                const std::string number = name.substr(1, name.size() - 2);
                if (number == "8" || number == "9" || number == "10" || number == "11" || number == "12" || number == "13" ||
                    number == "14" || number == "15")
                {
                    if (last == 'L' || last == 'B')
                        return kByte;
                    if (last == 'W')
                        return kWord;
                }
            }
            return kDword;
        }

        std::uint32_t Dword(const std::uint8_t* bytes)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        std::uint64_t Qword(const std::uint8_t* bytes)
        {
            std::uint64_t value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        float Single(const std::uint8_t* bytes)
        {
            float value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        double Double(const std::uint8_t* bytes)
        {
            double value = 0;
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }

        bool Printable(std::uint8_t c)
        {
            return c >= 32 && c <= 127;
        }

        // FindTypeOfData without custom types.
        DataType FindTypeOfData(std::uint64_t address, const std::uint8_t* buf, int size, const CheatEngineMemory& memory)
        {
            DataType result = DataType::Dword;

            bool isString = true;
            bool couldBeCounter = true;
            for (int i = 0; i < 4; ++i)
            {
                if (i < size)
                {
                    if (!Printable(buf[i]))
                    {
                        isString = false;
                        if (i > 0)
                            couldBeCounter = false;
                        if (!couldBeCounter)
                            break;
                    }
                }
                else
                {
                    isString = false;
                    couldBeCounter = false;
                }
            }
            if (isString)
                return DataType::String;
            // ∑ CE's test of the fifth byte is "(buf[4]>=32) or (buf[4]<=127)", always true.
            if (couldBeCounter && size > 4)
                return DataType::Byte;

            if (size >= 8)
            {
                bool unicode = true;
                for (int i = 0; i < 8; i += 2)
                {
                    if (!Printable(buf[i]) || buf[i + 1] != 0)
                    {
                        unicode = false;
                        break;
                    }
                }
                if (unicode)
                    return DataType::Unicode;
            }

            const int misaligned = (size == 4 || size == 8) ? 0 : static_cast<int>(address % 4);
            if (misaligned == 1)
                return DataType::Byte;
            if (misaligned >= 2)
            {
                const std::uint16_t word = static_cast<std::uint16_t>(buf[0] | (buf[1] << 8));
                return (word < 255 || word % 10 > 0) ? DataType::Byte : DataType::Word;
            }

            const int pointerSize = memory.Is64Bit ? 8 : 4;
            if (size >= pointerSize)
            {
                bool named = false;
                if (memory.Is64Bit)
                {
                    if (address % 8 == 0)
                        named = Named(memory, Qword(buf));
                }
                else
                {
                    named = Named(memory, Dword(buf));
                }
                if (named)
                    return DataType::Pointer;
            }

            if (size >= 2 && size < 4)
                return DataType::Word;
            if (size == 1)
                return DataType::Byte;

            const float single = Single(buf);
            if (single != 0)
            {
                const std::string text = PascalFloatToStr(single, true);
                if (!Contains(text, "E") && single >= -100000.0f && single <= 100000.0f)
                {
                    result = DataType::Single;
                    if (text.size() <= 4 || !Contains(text, "."))
                        return result;
                }
            }

            if (size >= 8)
            {
                const double value = Double(buf);
                if (value != 0)
                {
                    const std::string text = PascalFloatToStr(value, false);
                    if (!Contains(text, "E") && value < 100000 && value > -100000)
                    {
                        if (result == DataType::Single && value > static_cast<double>(single))
                            return result;
                        result = DataType::Double;
                        if (Dword(buf) != 0 && !Contains(PascalFloatToStr(single, true), "E"))
                        {
                            if (FindTypeOfData(address + 4, buf + 4, size - 4, memory) == DataType::Single)
                                result = DataType::Single;
                        }
                        return result;
                    }
                }
            }

            if (memory.Is64Bit)
            {
                if (address % 8 == 0 && Committed(memory, Qword(buf)))
                    return DataType::Pointer;
            }
            else if (Committed(memory, Dword(buf)))
            {
                return DataType::Pointer;
            }
            return result;
        }

        std::string AsciiText(const CheatEngineMemory& memory, std::uint64_t address, const std::uint8_t* head)
        {
            std::uint8_t buffer[128]{};
            std::memcpy(buffer, head, 16);
            const std::size_t got = memory.ReadPrefix ? memory.ReadPrefix(address, buffer, sizeof(buffer)) : 0;
            buffer[127] = 0;
            buffer[126] = '.';
            buffer[125] = '.';
            buffer[124] = '.';
            if (got > 0)
                buffer[got - 1] = 0;
            return "\"" + std::string(reinterpret_cast<const char*>(buffer)) + "\"";
        }

        std::string UnicodeText(const CheatEngineMemory& memory, std::uint64_t address, const std::uint8_t* head)
        {
            std::uint8_t buffer[128]{};
            std::memcpy(buffer, head, 16);
            const std::size_t got = memory.ReadPrefix ? memory.ReadPrefix(address, buffer, sizeof(buffer)) : 0;
            buffer[127] = 0;
            buffer[126] = 0;
            buffer[125] = 0;
            buffer[124] = '.';
            buffer[123] = 0;
            buffer[122] = '.';
            buffer[121] = 0;
            buffer[120] = '.';
            if (got > 1)
            {
                buffer[got - 1] = 0;
                buffer[got - 2] = 0;
            }
            std::wstring text;
            for (std::size_t i = 0; i + 1 < sizeof(buffer); i += 2)
            {
                const wchar_t c = static_cast<wchar_t>(buffer[i] | (buffer[i + 1] << 8));
                if (c == 0)
                    break;
                text += c;
            }
            // ∑ Pascal's '""' is the two characters "", so CE puts two quotes on each side of a UTF-16 text.
            return "\"\"" + WideToUtf8(text) + "\"\"";
        }

        // ------------------------------------------------------------------------------------------------
        // Free Pascal's number text (Grisu digits, then rounding the way flt_core.inc's round_digits does).

        struct Digits
        {
            std::vector<int> Values;
            int Exponent = 0;   // the power of ten of the first digit
        };

        // The correctly rounded leading digits of |value|, without trailing zeros (Grisu stops at an exact end).
        Digits LeadingDigits(double value, int count)
        {
            char text[64];
            std::snprintf(text, sizeof(text), "%.*e", count - 1, std::fabs(value));
            Digits digits;
            const char* at = text;
            for (; *at && *at != 'e'; ++at)
            {
                if (*at >= '0' && *at <= '9')
                    digits.Values.push_back(*at - '0');
            }
            if (*at == 'e')
                digits.Exponent = std::atoi(at + 1);
            while (!digits.Values.empty() && digits.Values.back() == 0)
                digits.Values.pop_back();
            return digits;
        }

        // round_digits(buf, n, keep, half_round_to_even = false) with GRISU1_F2A_AGRESSIVE_ROUNDUP.
        void RoundDigits(Digits& digits, int keep)
        {
            std::vector<int>& buf = digits.Values;
            const int n = static_cast<int>(buf.size());
            if (keep >= n)
                return;
            int round = buf[static_cast<std::size_t>(keep)];
            if (round == 4 && keep < n - 3 && buf[static_cast<std::size_t>(n - 2)] >= 8)
            {
                int i = n - 2;
                do
                    --i;
                while (i != keep && buf[static_cast<std::size_t>(i)] == 9);
                if (i == keep)
                    round = 9;
            }
            buf.resize(static_cast<std::size_t>(keep));
            if (round < 5)
                return;
            for (int i = keep - 1; i >= 0; --i)
            {
                if (++buf[static_cast<std::size_t>(i)] < 10)
                {
                    buf.resize(static_cast<std::size_t>(i) + 1);
                    return;
                }
            }
            buf.assign(1, 1);
            ++digits.Exponent;
        }

        std::string DigitText(const std::vector<int>& values, std::size_t from, std::size_t to)
        {
            std::string text;
            for (std::size_t i = from; i < to && i < values.size(); ++i)
                text += static_cast<char>('0' + values[i]);
            return text;
        }
    }

    std::string PascalFloatToStr(double value, bool single)
    {
        if (std::isnan(value))
            return "Nan";
        if (std::isinf(value))
            return value > 0 ? "+Inf" : "-Inf";
        if (value == 0)
            return "0";
        // ∑ FloatToStr is Str(x:21) for a single (10 digits of the type) and Str(x:22) for a double (17 digits,
        //   printed as 15). The exponent goes when it is above -6 and below the number of printed digits.
        const int printed = single ? 10 : 15;
        Digits digits = LeadingDigits(value, single ? 10 : 17);
        if (!single)
            RoundDigits(digits, printed);
        const std::string sign = std::signbit(value) ? "-" : "";
        const int e = digits.Exponent;
        if (e >= printed || e <= -6)
            return sign + DigitText(digits.Values, 0, 1) + "." + DigitText(digits.Values, 1, digits.Values.size()) + "E" + std::to_string(e);
        std::string whole;
        std::string fraction;
        if (e >= 0)
        {
            whole = DigitText(digits.Values, 0, static_cast<std::size_t>(e) + 1);
            while (static_cast<int>(whole.size()) < e + 1)
                whole += '0';
            fraction = DigitText(digits.Values, static_cast<std::size_t>(e) + 1, digits.Values.size());
        }
        else
        {
            whole = "0";
            fraction = std::string(static_cast<std::size_t>(-e - 1), '0') + DigitText(digits.Values, 0, digits.Values.size());
        }
        while (!fraction.empty() && fraction.back() == '0')
            fraction.pop_back();
        return sign + whole + (fraction.empty() ? "" : "." + fraction);
    }

    std::string PascalFixed4(double value)
    {
        if (std::isnan(value))
            return "Nan";
        if (std::isinf(value))
            return value > 0 ? "+Inf" : "-Inf";
        constexpr int kFraction = 4;
        const bool minus = std::signbit(value);
        std::string text;
        if (value != 0)
        {
            Digits digits = LeadingDigits(value, 17);
            int dot = digits.Exponent + 1;
            const int cut = dot + kFraction;
            if (cut < 0)
                digits.Values.clear();
            else if (cut < static_cast<int>(digits.Values.size()))
            {
                const int before = digits.Exponent;
                RoundDigits(digits, cut);
                dot += digits.Exponent - before;
            }
            const int have = static_cast<int>(digits.Values.size());
            // ∑ Str() gives up on fixed notation past 255 characters and writes "d.dE+XXX" instead.
            if (dot + 1 + kFraction + (minus ? 1 : 0) > 255)
            {
                Digits shortDigits = LeadingDigits(value, 17);
                RoundDigits(shortDigits, 2);
                while (shortDigits.Values.size() < 2)
                    shortDigits.Values.push_back(0);
                char exponent[16];
                std::snprintf(exponent, sizeof(exponent), "%03d", shortDigits.Exponent);
                return std::string(minus ? "-" : "") + DigitText(shortDigits.Values, 0, 1) + "." + DigitText(shortDigits.Values, 1, 2) + "E+" + exponent;
            }
            std::string whole;
            if (dot <= 0 || have == 0)
                whole = "0";
            else
            {
                whole = DigitText(digits.Values, 0, static_cast<std::size_t>(std::min(dot, have)));
                if (dot > have)
                    whole += std::string(static_cast<std::size_t>(dot - have), '0');
            }
            std::string fraction;
            if (dot < 0)
                fraction = std::string(static_cast<std::size_t>(std::min(-dot, kFraction)), '0');
            if (have > 0 && dot < have)
                fraction += DigitText(digits.Values, static_cast<std::size_t>(std::max(dot, 0)), static_cast<std::size_t>(have));
            fraction.resize(kFraction, '0');
            text = whole + "." + fraction;
        }
        else
        {
            text = "0.0000";
        }
        // RemoveLeadingNegativeSign: "-0.0000" loses its sign.
        if (minus && text.find_first_not_of("0.") != std::string::npos)
            return "-" + text;
        return text;
    }

    std::string CheatEngineExtra(std::string_view line, std::string_view opcode, const CheatEngineMemory& memory)
    {
        const bool lea = opcode.size() > 3 && StartsWith(opcode, "lea");
        std::uint64_t target = 0;
        const Found found = HasAddress(opcode, target, memory);
        if (found == Found::Fault)
            return {};
        // ∑ For a lea without an address CE goes on with an unset variable; that ends empty in practice.
        if (found == Found::No)
            return {};

        if (lea)
        {
            const std::size_t open = opcode.find('[');
            const std::size_t close = opcode.find(']');
            const std::string_view inside = open != std::string_view::npos && close != std::string_view::npos && close > open
                ? opcode.substr(open + 1, close - open - 1)
                : std::string_view();
            if (!PlainHex(inside, 16))
                return {};
            target = HexValue64(inside);
        }

        const bool jumper = StartsWith(opcode, "j") || StartsWith(opcode, "loo") || StartsWith(opcode, "ca");
        int valueType = OpcodeValueType(opcode);
        if (Contains(line, "["))
        {
            if (Contains(opcode, "qword ptr"))
                valueType = kDouble;
            else if (Contains(opcode, "dword ptr"))
                valueType = kDword;
            else if (Contains(opcode, "word ptr"))
                valueType = kWord;
            else if (Contains(opcode, "byte ptr"))
                valueType = kByte;
            else if (const std::size_t before = opcode.find(",["); before != std::string_view::npos)
            {
                const std::size_t space = opcode.find(' ');
                const std::string_view token = space != std::string_view::npos && space < before ? opcode.substr(space + 1, before - space - 1) : std::string_view();
                valueType = RegisterValueType(token, memory.Is64Bit);
            }
            else if (const std::size_t after = opcode.find("],"); after != std::string_view::npos)
            {
                valueType = RegisterValueType(opcode.substr(after + 2), memory.Is64Bit);
            }
        }

        std::uint8_t head[16]{};
        if (valueType == kDword && Read(memory, target, head, sizeof(head)))
        {
            switch (FindTypeOfData(target, head, sizeof(head), memory))
            {
            case DataType::Single: valueType = kSingle; break;
            case DataType::Double: valueType = kDouble; break;
            case DataType::String: valueType = kString; break;
            case DataType::Unicode: valueType = kUnicode; break;
            default: break;
            }
        }
        if (jumper)
            valueType = kDword;

        std::string text;
        switch (valueType)
        {
        case kByte:
        {
            std::uint8_t value = 0;
            if (Read(memory, target, &value, 1))
                text = Hex(value, 2);
            break;
        }
        case kWord:
        {
            std::uint16_t value = 0;
            if (Read(memory, target, &value, 2))
                text = Hex(value, 4);
            break;
        }
        case kDword:
        {
            std::uint32_t low = 0;
            if (!Read(memory, target, &low, 4))
                break;
            std::uint64_t value = low;
            if (jumper && (value & 0xFFFF) == 0x25FF)
            {
                // ∑ A branch to "jmp [pointer]": CE names where the pointer goes, "->module.export".
                std::uint32_t displacement = 0;
                if (Read(memory, target + 2, &displacement, 4))
                {
                    value = displacement;
                    if (memory.Is64Bit)
                        value = target + 6 + value;
                    std::uint64_t pointer = 0;
                    if (Read(memory, value, &pointer, memory.Is64Bit ? 8 : 4))
                        text = "->" + NameOrHex(memory, pointer);
                }
            }
            else
            {
                text = NameOrHex(memory, value);
            }
            // A branch keeps only a name, never a number.
            if (jumper && PlainHex(text, 8))
                text.clear();
            break;
        }
        case kSingle:
        {
            float value = 0;
            if (Read(memory, target, &value, 4))
                text = "(float)" + PascalFixed4(value);
            break;
        }
        case kDouble:
        {
            double value = 0;
            if (Read(memory, target, &value, 8))
                text = "(double)" + PascalFixed4(value);
            break;
        }
        case kString: text = AsciiText(memory, target, head); break;
        case kUnicode: text = UnicodeText(memory, target, head); break;
        default: break;
        }
        return text.empty() ? text : "[" + text + "]";
    }

    CheatEngineSplit SplitCheatEngineLine(std::string_view text, const CheatEngineMemory* memory)
    {
        // ∑ The parts are cut the way CE's Pascal does it: the address up to the first " - " (upper case),
        //   the bytes up to the next one, the opcode up to " : ". The fourth part is never the text after
        //   " : "; CE always works it out from memory.
        CheatEngineSplit split;
        const std::size_t first = text.find(" - ");
        if (first != std::string_view::npos)
        {
            split.Address = std::string(text.substr(0, first));
            for (char& c : split.Address)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        const std::size_t bytesAt = first == std::string_view::npos ? 2 : first + 3;
        std::size_t second = bytesAt <= text.size() ? text.find(" - ", bytesAt) : std::string_view::npos;
        if (second == std::string_view::npos)
            second = text.size();
        if (bytesAt < second)
            split.Bytes = std::string(text.substr(bytesAt, second - bytesAt));
        const std::size_t opcodeAt = second + 3;
        std::size_t colon = opcodeAt <= text.size() ? text.find(" : ", opcodeAt) : std::string_view::npos;
        if (colon == std::string_view::npos)
            colon = text.size();
        if (opcodeAt < colon)
            split.Opcode = std::string(text.substr(opcodeAt, colon - opcodeAt));
        if (memory)
            split.Extra = CheatEngineExtra(text, split.Opcode, *memory);
        return split;
    }
}
