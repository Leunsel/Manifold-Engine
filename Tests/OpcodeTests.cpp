#include "TestKit.h"

#include "Core/Text.h"
#include "Disasm/OpcodeConvert.h"

#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using Bytes = std::vector<std::uint8_t>;

        Bytes Parsed(std::string_view text)
        {
            return Disasm::ParseByteText(text).value_or(Bytes{ 0xDE, 0xAD });
        }

        void ByteText()
        {
            CHECK(Parsed("48 8B 05 10 00 00 00") == (Bytes{ 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 }));
            CHECK(Parsed("488b05") == (Bytes{ 0x48, 0x8B, 0x05 }));
            CHECK(Parsed("0x48, 0x8B") == (Bytes{ 0x48, 0x8B }));
            CHECK(Parsed("\\x48\\x8B\\x05") == (Bytes{ 0x48, 0x8B, 0x05 }));
            CHECK(Parsed("{ 0x90, 0xC3 };") == (Bytes{ 0x90, 0xC3 }));
            CHECK(Parsed("F 0") == (Bytes{ 0x0F, 0x00 }));
            CHECK(Parsed("48h 90h") == (Bytes{ 0x48, 0x90 }));
            CHECK(Parsed("48 8B\n05\t10") == (Bytes{ 0x48, 0x8B, 0x05, 0x10 }));
            CHECK(Parsed("").empty());
            std::string error;
            CHECK(!Disasm::ParseByteText("48 8G", &error).has_value());
            CHECK(error.find("8G") != std::string::npos);
            CHECK(!Disasm::ParseByteText("123", &error).has_value());
            CHECK(error.find("odd") != std::string::npos);
            CHECK(Disasm::ParseByteText("90", &error).has_value() && error.empty());

            const Bytes sample{ 0x48, 0x8B, 0x0A };
            CHECK_EQ(Disasm::FormatBytes(sample, Disasm::ByteStyle::Spaced), std::string("48 8B 0A"));
            CHECK_EQ(Disasm::FormatBytes(sample, Disasm::ByteStyle::Compact), std::string("488B0A"));
            CHECK_EQ(Disasm::FormatBytes(sample, Disasm::ByteStyle::Escaped), std::string("\\x48\\x8B\\x0A"));
            CHECK_EQ(Disasm::FormatBytes(sample, Disasm::ByteStyle::CArray), std::string("0x48, 0x8B, 0x0A"));
            CHECK(Disasm::FormatBytes({}, Disasm::ByteStyle::CArray).empty());
        }

        void Decoding()
        {
            // mov rax,[rip+10] at 140001000 reads 140001017; then a ret.
            const Bytes rip{ 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00, 0xC3 };
            const auto lines = Disasm::DecodeBytes(rip, 0x140001000, true);
            CHECK_EQ(lines.size(), static_cast<std::size_t>(2));
            if (lines.size() == 2)
            {
                CHECK(lines[0].Error.empty() && lines[0].Bytes.size() == 7 && lines[0].Address == 0x140001000);
                CHECK_MSG(ContainsNoCase(lines[0].Text, "mov") && ContainsNoCase(lines[0].Text, "140001017"), lines[0].Text);
                CHECK(lines[1].Address == 0x140001007 && ContainsNoCase(lines[1].Text, "ret"));
            }
            // call rel32 in 32-bit code at 401000 goes to 402000.
            const auto call = Disasm::DecodeBytes(Bytes{ 0xE8, 0xFB, 0x0F, 0x00, 0x00 }, 0x401000, false);
            CHECK(call.size() == 1 && call[0].Error.empty() && call[0].Bytes.size() == 5);
            if (!call.empty())
                CHECK_MSG(ContainsNoCase(call[0].Text, "call") && ContainsNoCase(call[0].Text, "402000"), call[0].Text);
            // 06 (push es) is no instruction in 64-bit code but one in 32-bit code.
            const auto invalid = Disasm::DecodeBytes(Bytes{ 0x06, 0x90 }, 0x1000, true);
            CHECK(invalid.size() == 2 && !invalid[0].Error.empty() && invalid[0].Text == "db 06" && invalid[1].Error.empty());
            const auto legacy = Disasm::DecodeBytes(Bytes{ 0x06 }, 0x1000, false);
            CHECK(legacy.size() == 1 && legacy[0].Error.empty());
            // A cut-off instruction gives one db line per byte.
            const auto cut = Disasm::DecodeBytes(Bytes{ 0x48, 0x8B }, 0x1000, true);
            CHECK(cut.size() == 2 && !cut[0].Error.empty() && cut[1].Address == 0x1001);
        }

        void Assembling()
        {
            const auto lines = Disasm::AssembleText("mov rax,[rcx+10]\n// a comment\n\nnop // trailing\njmp 140001000\nmov rax,[\nret", 0x140001000, true);
            CHECK_EQ(lines.size(), static_cast<std::size_t>(5));
            if (lines.size() == 5)
            {
                CHECK(lines[0].Bytes == (Bytes{ 0x48, 0x8B, 0x41, 0x10 }) && lines[0].SourceLine == 1 && lines[0].Address == 0x140001000);
                CHECK(lines[1].Bytes == (Bytes{ 0x90 }) && lines[1].SourceLine == 4 && lines[1].Address == 0x140001004);
                CHECK_MSG(lines[2].Bytes == (Bytes{ 0xEB, 0xF9 }), Disasm::FormatBytes(lines[2].Bytes, Disasm::ByteStyle::Spaced));
                CHECK(lines[2].Address == 0x140001005 && lines[2].Error.empty());
                CHECK(!lines[3].Error.empty() && lines[3].Bytes.empty() && lines[3].SourceLine == 6 && lines[3].Text == "mov rax,[");
                // The failed line takes no room.
                CHECK(lines[4].Address == 0x140001007 && lines[4].Bytes == (Bytes{ 0xC3 }));
            }
            const auto x86 = Disasm::AssembleText("push ebp\nmov eax,[ebp+8]", 0x401000, false);
            CHECK(x86.size() == 2 && x86[0].Bytes == (Bytes{ 0x55 }) && x86[1].Bytes == (Bytes{ 0x8B, 0x45, 0x08 }) && x86[1].Address == 0x401001);
            CHECK(Disasm::AssembleText("", 0, true).empty());
            CHECK(Disasm::AssembleText("\n  \n// only a comment", 0, true).empty());

            // Round trip: what the bytes decode to assembles to the same bytes.
            const Bytes code{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x01, 0x00, 0x00, 0xC3 };
            const auto decoded = Disasm::DecodeBytes(code, 0x7FF600001000, true);
            std::string text;
            for (const auto& line : decoded)
                text += line.Text + "\n";
            const auto again = Disasm::AssembleText(text, 0x7FF600001000, true);
            Bytes joined;
            for (const auto& line : again)
            {
                CHECK_MSG(line.Error.empty(), line.Text + ": " + line.Error);
                joined.insert(joined.end(), line.Bytes.begin(), line.Bytes.end());
            }
            CHECK_MSG(joined == code, Disasm::FormatBytes(joined, Disasm::ByteStyle::Spaced) + " from " + text);
        }
    }

    int RunOpcodeTests()
    {
        const int before = Totals().Failures;
        ByteText();
        Decoding();
        Assembling();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("opcodes", "Opcode converter", 31, [](const SuiteContext&) { return RunOpcodeTests(); });
}
