#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Disasm/Disassembler.h"
#include "Disasm/Signature.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"

#include <windows.h>
#include <process.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        struct Block
        {
            std::uint8_t* Data = nullptr;
            std::size_t Size = 0;

            Block(std::size_t size, DWORD protect) : Size(size)
            {
                Data = static_cast<std::uint8_t*>(::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, protect));
            }
            ~Block()
            {
                if (Data)
                    ::VirtualFree(Data, 0, MEM_RELEASE);
            }
            Block(const Block&) = delete;
            Block& operator=(const Block&) = delete;

            std::uint64_t At(std::size_t offset) const { return reinterpret_cast<std::uint64_t>(Data) + offset; }
            void Put(std::size_t offset, const std::vector<std::uint8_t>& bytes) { std::memcpy(Data + offset, bytes.data(), bytes.size()); }
        };

        std::size_t Oracle(const Block& block, const Scan::Pattern& pattern, std::uint64_t self)
        {
            std::size_t need = 1;
            const std::uint64_t base = block.At(0);
            for (std::size_t p = 0; p < block.Size; ++p)
            {
                if (base + p == self)
                    continue;
                std::size_t i = 0;
                while (i < pattern.Bytes.size() && p + i < block.Size && ((block.Data[p + i] ^ pattern.Bytes[i]) & pattern.Mask[i]) == 0)
                    ++i;
                need = std::max(need, i + 1);
            }
            return need;
        }

        Disasm::SignatureOptions InBlock(const Block& block)
        {
            Disasm::SignatureOptions options;
            options.Range = std::make_pair(block.At(0), block.At(block.Size));
            return options;
        }

        std::string Text(const Disasm::Signature& signature)
        {
            return Disasm::SignatureText(signature.Bytes, {});
        }

        void Layouts(const Disasm::Disassembler& disassembler)
        {
            const auto layout = [&](std::vector<std::uint8_t> bytes)
                {
                    const Disasm::Instruction line = disassembler.Decode(0x140001000, bytes.data(), bytes.size());
                    return disassembler.Layout(line);
                };
            {
                const auto rip = layout({ 0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40 });
                CHECK(rip && rip->PrefixEnd == 1 && rip->ModRm == 2 && rip->Fields.size() == 1);
                CHECK(rip && rip->Fields[0].Offset == 3 && rip->Fields[0].Size == 4 && rip->Fields[0].RipRelative && rip->Fields[0].Kind == Disasm::FieldKind::Displacement);
            }
            {
                const auto call = layout({ 0xE8, 0x00, 0x01, 0x00, 0x00 });
                CHECK(call && call->PrefixEnd == 0 && call->Fields.size() == 1 && call->Fields[0].Kind == Disasm::FieldKind::Relative && call->Fields[0].Size == 4);
            }
            {
                const auto store = layout({ 0xC7, 0x45, 0xF8, 0x01, 0x00, 0x00, 0x00 });
                CHECK(store && store->Fields.size() == 2);
                CHECK(store && store->Fields[0].Kind == Disasm::FieldKind::Displacement && store->Fields[0].Size == 1 && store->Fields[0].Value == -8 && !store->Fields[0].RipRelative);
                CHECK(store && store->Fields[1].Kind == Disasm::FieldKind::Immediate && store->Fields[1].Offset == 3 && store->Fields[1].Value == 1);
            }
            {
                const auto vex = layout({ 0xC4, 0xE2, 0x79, 0x18, 0x05, 0x00, 0x10, 0x00, 0x00 });
                CHECK(vex && vex->PrefixEnd == 3 && vex->Fields.size() == 1 && vex->Fields[0].RipRelative);
            }
            {
                const auto sib = layout({ 0x8B, 0x04, 0x85, 0x00, 0x10, 0x40, 0x00 });
                CHECK(sib && sib->Sib == 2 && sib->Fields.size() == 1 && sib->Fields[0].Absolute);
            }
        }

        void AgainstOracle(const Target::Process& process)
        {
            Block block(5 * 1024 * 1024 + 4096, PAGE_READWRITE);
            CHECK(block.Data != nullptr);
            if (!block.Data)
                return;
            std::mt19937 random(1234);
            for (std::size_t i = 0; i < block.Size; ++i)
            {
                const std::uint32_t pick = random() % 100;
                block.Data[i] = static_cast<std::uint8_t>(pick < 30 ? 0x48 : pick < 45 ? 0x8B : pick < 55 ? 0x00 : random() & 0xFF);
            }
            const auto plant = [&](std::size_t from, std::size_t to, std::size_t length, std::size_t differ)
                {
                    std::memcpy(block.Data + to, block.Data + from, length);
                    block.Data[to + differ] ^= 0x5A;
                };
            const std::size_t straddle = 2 * 1024 * 1024 - 7;
            plant(1000, straddle, 48, 40);
            plant(1000, 3 * 1024 * 1024, 48, 21);
            plant(9000, block.Size - 30, 30, 29);
            plant(9000, 4 * 1024 * 1024, 64, 63);

            int checked = 0;
            int matched = 0;
            const auto compare = [&](std::size_t at, std::size_t length, bool wild)
                {
                    Scan::Pattern pattern;
                    pattern.Bytes.assign(block.Data + at, block.Data + at + length);
                    pattern.Mask.assign(length, 0xFF);
                    if (wild)
                    {
                        for (std::size_t i = 1; i < length; i += 5)
                            pattern.Mask[i] = 0;
                        pattern.Mask[3] = 0xF0;
                    }
                    const std::size_t engine = Disasm::UniqueLength(process, pattern, block.At(at), block.At(0), block.At(block.Size), false);
                    const std::size_t oracle = Oracle(block, pattern, block.At(at));
                    ++checked;
                    matched += engine == oracle ? 1 : 0;
                    if (engine != oracle)
                        std::printf("    pattern at +%zX (%zu bytes%s): engine %zu, oracle %zu\n", at, length, wild ? ", wildcards" : "", engine, oracle);
                };
            compare(1000, 48, false);
            compare(1000, 48, true);
            compare(9000, 64, false);
            compare(9000, 30, false);
            compare(straddle, 48, false);
            for (int round = 0; round < 12; ++round)
                compare(static_cast<std::size_t>(random() % (block.Size - 80)), 24 + static_cast<std::size_t>(random() % 40), round % 2 == 1);
            CHECK_MSG(matched == checked, std::to_string(matched) + " of " + std::to_string(checked) + " lengths agree with the brute-force search");
        }

        void Masking(const Target::Process& process, const Disasm::Disassembler& disassembler)
        {
            Block block(64 * 1024, PAGE_EXECUTE_READWRITE);
            CHECK(block.Data != nullptr);
            if (!block.Data)
                return;
            std::memset(block.Data, 0xCC, block.Size);
            const std::vector<std::uint8_t> code = {
                0x48, 0x89, 0x5C, 0x24, 0x08,
                0x48, 0x83, 0xEC, 0x20,
                0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x04,
                0x8B, 0x88, 0xA8, 0x01, 0x00, 0x00,
                0xE8, 0x55, 0x66, 0x77, 0x08,
                0xC3 };
            block.Put(0x100, code);
            std::vector<std::uint8_t> similar = code;
            similar[12] = 0x99;
            similar[17] = 0x89;
            block.Put(0x400, similar);

            std::string error;
            Disasm::SignatureOptions options = InBlock(block);
            const auto made = Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error);
            CHECK_MSG(made.has_value(), error);
            if (made)
            {
                CHECK_EQ(Text(*made), std::string("48 89 5C 24 ?? 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 8B 88"));
                CHECK(made->Verified && made->Offset == 0 && made->Start == block.At(0x100));
                CHECK(made->Bytes[0].Role == Disasm::ByteRole::Prefix && made->Bytes[1].Role == Disasm::ByteRole::Opcode && made->Bytes[2].Role == Disasm::ByteRole::ModRm);
                CHECK(made->Bytes[4].Role == Disasm::ByteRole::Offset && made->Bytes[8].Role == Disasm::ByteRole::Immediate && made->Bytes[12].Role == Disasm::ByteRole::Address);
                CHECK_EQ(made->Lines.size(), std::size_t(4));
                CHECK(!made->FunctionEnd);
            }
            options.MaskAddresses = false;
            const auto kept = Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error);
            CHECK(kept && Text(*kept) == "48 89 5C 24 ?? 48 83 EC 20 48 8B 05 11");

            options = InBlock(block);
            options.MinBytes = 26;
            const auto longer = Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error);
            CHECK(longer && Text(*longer) == "48 89 5C 24 ?? 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 8B 88 ?? ?? ?? ?? E8");
            options.Offsets = Disasm::MaskMode::Keep;
            options.Branches = Disasm::MaskMode::Keep;
            options.MinBytes = 28;
            const auto literal = Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error);
            CHECK(literal && Text(*literal) == "48 89 5C 24 08 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 8B 88 A8 01 00 00 E8 55 66 77 08 C3");

            block.Put(0x800, code);
            options = InBlock(block);
            options.MaxBytes = 28;
            CHECK(!Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error));
            CHECK_MSG(error.find("No unique signature within 28 bytes") != std::string::npos, error);

            options.MaxBytes = 128;
            options.StopAtFunctionEnd = true;
            CHECK(!Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error));
            block.Data[0x100 + code.size()] = 0x90;
            options.StopAtFunctionEnd = false;
            const auto past = Disasm::MakeSignature(process, disassembler, block.At(0x100), options, error);
            CHECK_MSG(past && past->Bytes.size() == code.size() + 1 && past->FunctionEnd == block.At(0x100 + code.size() - 1), error);
        }

        void Offsets(const Target::Process& process, const Disasm::Disassembler& disassembler)
        {
            Block block(64 * 1024, PAGE_EXECUTE_READWRITE);
            CHECK(block.Data != nullptr);
            if (!block.Data)
                return;
            std::memset(block.Data, 0xCC, block.Size);
            const std::vector<std::uint8_t> body = { 0x89, 0x41, 0x10, 0x89, 0x51, 0x14, 0xC3 };
            for (std::size_t copy = 0; copy < 8; ++copy)
                block.Put(0x1000 + copy * 0x40, body);
            block.Put(0x2000, { 0xB9, 0x37, 0x13, 0x00, 0x00 });
            block.Put(0x2005, body);
            const std::uint64_t target = block.At(0x2005);

            std::string error;
            Disasm::SignatureOptions options = InBlock(block);
            options.MaxBytes = 32;
            CHECK(!Disasm::MakeSignature(process, disassembler, target, options, error));

            options.AllowOffset = true;
            options.MinBytes = 6;
            const auto shifted = Disasm::MakeSignature(process, disassembler, target, options, error);
            CHECK_MSG(shifted.has_value(), error);
            if (shifted)
            {
                CHECK_EQ(shifted->Offset, std::int64_t(5));
                CHECK_EQ(Text(*shifted), std::string("B9 37 13 00 00 89"));
                CHECK(shifted->Verified && shifted->Start == block.At(0x2000));
                CHECK(std::any_of(shifted->Candidates.begin(), shifted->Candidates.end(), [&](const Disasm::SignatureCandidate& c) { return c.Start == target && !c.Unique; }));
                Disasm::SignatureFormatting formatting;
                formatting.Label = "player";
                CHECK_EQ(Disasm::FormatSignature(*shifted, Disasm::SignatureFormat::AutoAssembler, formatting), std::string("aobscan(player,B9 37 13 00 00 89) // player+05"));
            }

            block.Data[0x2005 + body.size()] = 0x90;
            options.MinSaving = 4;
            const auto direct = Disasm::MakeSignature(process, disassembler, target, options, error);
            CHECK(direct && direct->Offset == 0 && direct->Bytes.size() == 8);
            options.MinSaving = 2;
            const auto saved = Disasm::MakeSignature(process, disassembler, target, options, error);
            CHECK(saved && saved->Offset == 5 && saved->Bytes.size() == 6);
        }

        void Formats()
        {
            Disasm::Signature signature;
            signature.Module = "game.exe";
            signature.Bytes = { { 0x48, false }, { 0x8B, false }, { 0x05, false }, { 0x00, true }, { 0x00, true }, { 0xC3, false } };
            Disasm::SignatureFormatting formatting;
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::Aob, formatting), std::string("48 8B 05 ?? ?? C3"));
            formatting.Wildcard = Disasm::SignatureWildcard::Single;
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::Aob, formatting), std::string("48 8B 05 ? ? C3"));
            formatting.Spaces = false;
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::Aob, formatting), std::string("488B05????C3"));
            formatting.Spaces = true;
            formatting.Wildcard = Disasm::SignatureWildcard::Star;
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::Aob, formatting), std::string("48 8B 05 * * C3"));
            formatting.Wildcard = Disasm::SignatureWildcard::DoubleStar;
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::AutoAssembler, formatting), std::string("aobscanmodule(INJECT,game.exe,48 8B 05 ** ** C3)"));
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::CodeMask, formatting), std::string("\"\\x48\\x8B\\x05\\x00\\x00\\xC3\", \"xxx??x\""));
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::ByteArray, formatting), std::string("{ 0x48, 0x8B, 0x05, 0x00, 0x00, 0xC3 }, \"xxx??x\""));
            signature.Offset = -8;
            CHECK_EQ(Disasm::FormatSignature(signature, Disasm::SignatureFormat::AutoAssembler, {}), std::string("aobscanmodule(INJECT,game.exe,48 8B 05 ?? ?? C3) // INJECT-08"));
            for (const Disasm::SignatureWildcard wildcard : { Disasm::SignatureWildcard::Double, Disasm::SignatureWildcard::Single, Disasm::SignatureWildcard::DoubleStar, Disasm::SignatureWildcard::Star })
            {
                for (const bool spaces : { true, false })
                {
                    Disasm::SignatureFormatting each;
                    each.Wildcard = wildcard;
                    each.Spaces = spaces;
                    const auto parsed = Scan::ParsePattern(Disasm::SignatureText(signature.Bytes, each));
                    CHECK_MSG(parsed && parsed->Bytes.size() == 6 && parsed->Mask[3] == 0 && parsed->Mask[5] == 0xFF, Disasm::SignatureText(signature.Bytes, each));
                }
            }
        }

        void Parsing()
        {
            const auto text = [](const std::string& input) -> std::string
                {
                    std::string error;
                    const auto parsed = Disasm::ParseSignature(input, error);
                    return parsed ? parsed->Text : "error: " + error;
                };
            CHECK_EQ(text("48 8B ? ? 66"), std::string("48 8B ?? ?? 66"));
            CHECK_EQ(text("48 8B ** * 66"), std::string("48 8B ?? ?? 66"));
            CHECK_EQ(text("488B????66"), std::string("48 8B ?? ?? 66"));
            CHECK_EQ(text("\"48 8B ?? ?? 66\""), std::string("48 8B ?? ?? 66"));
            CHECK_EQ(text("\"\\x48\\x8B\\x00\\x00\\x66\", \"xx??x\""), std::string("48 8B ?? ?? 66"));
            CHECK_EQ(text("{ 0x48, 0x8B, 0x05 }"), std::string("48 8B 05"));
            CHECK_EQ(text("4? 8B ?5"), std::string("4? 8B ?5"));
            CHECK_EQ(text("Address of signature = game.exe + 0x00001234\n\"\\x48\\x8B\", \"xx\"\n\"48 8B\""), std::string("48 8B"));
            CHECK_EQ(text("// player hook\n48 8B 05 ?? ?? ?? ??"), std::string("48 8B 05 ?? ?? ?? ??"));
            std::string error;
            const auto aa = Disasm::ParseSignature("aobscanmodule(INJECT,game.exe,48 8B 05 ** ** ** **) // INJECT+0C", error);
            CHECK(aa && aa->Module == "game.exe" && aa->Text == "48 8B 05 ?? ?? ?? ??" && aa->Source == "aobscanmodule");
            const auto bare = Disasm::ParseSignature("\\x48\\x00\\x8B", error);
            CHECK(bare && bare->Text == "48 00 8B" && bare->Notes.size() == 1);
            CHECK(text("zz 48").starts_with("error"));
            CHECK(text("?? ??").starts_with("error"));
            CHECK(text("").starts_with("error"));
            CHECK(text("\"\\x48\\x8B\", \"x\"").starts_with("error"));
            const auto lead = Disasm::ParseSignature("?? 48 8B", error);
            CHECK(lead && lead->Wildcards == 1 && lead->Fixed == 2 && !lead->Notes.empty());
        }

        void LuaApi()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            // In this very process the scan buffers hold copies of what they read, so all memory is never unique here.
            engine.Options().SignatureCodeOnly = true;
            CHECK(engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok());
            Block block(64 * 1024, PAGE_EXECUTE_READWRITE);
            if (!block.Data)
                return;
            std::memset(block.Data, 0xCC, block.Size);
            block.Put(0x100, { 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x04, 0x89, 0x41, 0x10, 0xC3 });
            const std::string at = std::to_string(block.At(0x100));
            const std::string script =
                "local made = assert(manifold.signature.make(" + at + ", { scope = 'process', codeOnly = true, wildcard = '?' }))\n"
                "assert(made.pattern:sub(1, 8) == '48 8B 05', made.pattern)\n"
                "assert(made.address == " + at + " and made.start == made.address and made.offset == 0)\n"
                "assert(made.scope == 'process' and made.verified and #made.bytes == made.length)\n"
                "assert(made.bytes[4].wild and made.bytes[4].role == 'address' and made.bytes[1].role == 'prefix')\n"
                "assert(made.mask:sub(1, 7) == 'xxx' .. string.rep('?', 4))\n"
                "local parsed = assert(manifold.signature.parse('aobscanmodule(INJECT,game.exe,48 8B ** **)'))\n"
                "assert(parsed.pattern == '48 8B ' .. string.rep('?', 2) .. ' ' .. string.rep('?', 2) and parsed.module == 'game.exe' and parsed.fixed == 2 and parsed.wildcards == 2)\n"
                "local hits = assert(manifold.signature.find(made.pattern, { executable = true }))\n"
                "assert(#hits == 1 and hits[1] == made.address, #hits)\n"
                "local none, problem = manifold.signature.parse('zz')\n"
                "assert(none == nil and problem.code == 'ME-2003', tostring(problem and problem.code))\n"
                "local facade, why = ManifoldSigMaker:Pattern(" + at + ")\n"
                "assert(facade, why)\n"
                "assert(facade == manifold.signature.make(" + at + ").pattern)\n"
                "assert(manifold.has('signature.make') and manifold.apiVersion >= 28)\n";
            const Status status = lua.Run(script, "signature api");
            CHECK_MSG(status.Ok(), status.Text);
        }

        void Speed(const Target::Process& process, const Disasm::Disassembler& disassembler)
        {
            const auto self = process.ModuleAt(reinterpret_cast<std::uint64_t>(&Oracle));
            CHECK(self.has_value());
            if (!self)
                return;
            std::string error;
            Disasm::SignatureOptions options;
            const auto started = std::chrono::steady_clock::now();
            const auto made = Disasm::MakeSignature(process, disassembler, reinterpret_cast<std::uint64_t>(&Oracle), options, error);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            CHECK_MSG(made && made->Verified, error);
            if (made)
                std::printf("  signature for Oracle in %s: %s (%zu bytes, %s searched, %.1f ms)\n", self->Name.c_str(), Text(*made).c_str(), made->Bytes.size(),
                    Grouped(made->SearchedBytes).c_str(), seconds * 1000.0);
            options.AllowOffset = true;
            const auto shifted = Disasm::MakeSignature(process, disassembler, reinterpret_cast<std::uint64_t>(&Oracle), options, error);
            CHECK_MSG(shifted && shifted->Verified, error);
            if (shifted)
                std::printf("  with an AoB offset: %s (offset %lld, %zu candidates, %.1f ms)\n", Text(*shifted).c_str(), static_cast<long long>(shifted->Offset),
                    shifted->Candidates.size(), shifted->Seconds * 1000.0);
        }
    }

    int RunSignatureTests()
    {
        const int before = Totals().Failures;
        auto opened = Target::Process::Open(MakeProcessId(static_cast<std::uint32_t>(_getpid())));
        CHECK(opened.Ok());
        if (!opened.Ok())
            return Totals().Failures - before;
        const std::shared_ptr<Target::Process> process = *opened;
        Disasm::Disassembler disassembler(true);
        Layouts(disassembler);
        AgainstOracle(*process);
        Masking(*process, disassembler);
        Offsets(*process, disassembler);
        Formats();
        Parsing();
        Speed(*process, disassembler);
        LuaApi();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("signatures", "Signature maker", 32, [](const SuiteContext&) { return RunSignatureTests(); });
}
