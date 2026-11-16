#include "TestKit.h"

#include "Asm/AutoAssembler.h"
#include "Core/Errors.h"
#include "App/Sounds.h"
#include "Core/JobQueue.h"
#include "Core/Text.h"
#include "Core/Xml.h"
#include "Debug/BreakAction.h"
#include "Disasm/AssemblySearch.h"
#include "Disasm/Disassembler.h"
#include "Scan/Scanner.h"
#include "Scan/ValueType.h"
#include "Table/CheatTable.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace Manifold::Tests
{
    namespace
    {
        void ValueTypes()
        {
            Scan::ValueFormat format;
            const auto four = Scan::ParseValue(Scan::ValueType::FourBytes, "-2", format);
            CHECK(four && four->size() == 4 && (*four)[0] == 0xFE && (*four)[3] == 0xFF);
            CHECK_EQ(Scan::FormatValue(Scan::ValueType::FourBytes, *four, format), "-2");
            format.Signed = false;
            CHECK_EQ(Scan::FormatValue(Scan::ValueType::FourBytes, *four, format), "4294967294");
            format.Hex = true;
            CHECK_EQ(Scan::FormatValue(Scan::ValueType::FourBytes, *four, format), "FFFFFFFE");
            const auto hex = Scan::ParseValue(Scan::ValueType::TwoBytes, "1F4", format);
            CHECK(hex && (*hex)[0] == 0xF4 && (*hex)[1] == 0x01);
            format = {};
            CHECK(!Scan::ParseValue(Scan::ValueType::Byte, "300", format));
            CHECK(Scan::ParseValue(Scan::ValueType::Byte, "-1", format));
            const auto floating = Scan::ParseValue(Scan::ValueType::Float, "1,5", format);
            CHECK(floating && Scan::FormatValue(Scan::ValueType::Float, *floating, format) == "1.5");
            const auto dbl = Scan::ParseValue(Scan::ValueType::Double, "1234.5", format);
            CHECK(dbl && Scan::FormatValue(Scan::ValueType::Double, *dbl, format) == "1234.5");
            const auto aob = Scan::ParseValue(Scan::ValueType::ByteArray, "90 90 CC", format);
            CHECK(aob && aob->size() == 3 && (*aob)[2] == 0xCC);
            format.Unicode = true;
            const auto wide = Scan::ParseValue(Scan::ValueType::String, "Hi", format);
            CHECK(wide && wide->size() == 4 && (*wide)[0] == 'H' && (*wide)[1] == 0);
            CHECK_EQ(Scan::FormatValue(Scan::ValueType::String, *wide, format), "Hi");

            const auto pattern = Scan::ParsePattern("48 8B ?? 1? *");
            CHECK(pattern && pattern->Bytes.size() == 5 && pattern->Mask[2] == 0 && pattern->Mask[3] == 0xF0 && pattern->Mask[4] == 0);
            const auto packed = Scan::ParsePattern("488B??10");
            CHECK(packed && packed->Bytes.size() == 4 && packed->Bytes[3] == 0x10);
            CHECK(!Scan::ParsePattern("48 8G"));
            CHECK(!Scan::ParsePattern(""));
            const std::uint8_t data[] = { 0x48, 0x8B, 0x05, 0x17, 0x99 };
            CHECK(pattern->Matches(data));
            CHECK_EQ(Scan::PatternText(*pattern), "48 8B ?? 10 ??");
        }

        void TextHelpers()
        {
            CHECK_EQ(ParseHex("$10").value_or(0), 0x10u);
            CHECK_EQ(ParseHex("0x7FF6").value_or(0), 0x7FF6u);
            CHECK_EQ(ParseHex("-1").value_or(0), ~0ull);
            CHECK(!ParseHex("12G"));
            CHECK_EQ(Grouped(1234567), "1,234,567");
            CHECK_EQ(ParseDecimal("18446744073709551615").value_or(0), -1);
            CHECK_EQ(Split("a;;b;", ';').size(), 2u);
            CHECK_EQ(ReplaceAll("a.b.c", ".", "::"), "a::b::c");
            CHECK(EqualsNoCase("Game.EXE", "game.exe"));
            CHECK_EQ(HexBytes(std::vector<std::uint8_t>{ 0x0A, 0xFF }), "0A FF");
        }

        void Errors()
        {
            std::set<ErrorCode> seen;
            for (const ErrorInfo& info : ErrorTable())
            {
                CHECK_MSG(seen.insert(info.Code).second, ErrorName(info.Code));
                std::string message(info.Message);
                const std::size_t first = message.find("{}");
                CHECK_MSG(first == std::string::npos || message.find("{}", first + 2) == std::string::npos, ErrorName(info.Code));
                CHECK_MSG(!info.Cause.empty() && !info.Help.empty(), ErrorName(info.Code));
            }
            const std::string text = ErrorMessage(ErrorCode::ReadFailed, "1234");
            CHECK_EQ(text, "Memory at 1234 could not be read. (ME-1006)");
            const ErrorInfo* found = ErrorOf(text);
            CHECK(found && found->Code == ErrorCode::ReadFailed);
            CHECK(FindError("mce-3104") && FindError("mce-3104")->Code == ErrorCode::AaLabelNotPlaced);

            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "Core" / "Errors.h"); ++i)
                at = at.parent_path();
            const std::filesystem::path docs = at / "Docs" / "ERRORS.md";
            if (std::filesystem::exists(docs))
            {
                std::ifstream file(docs, std::ios::binary);
                std::stringstream content;
                content << file.rdbuf();
                std::string written = content.str();
                written.erase(std::remove(written.begin(), written.end(), '\r'), written.end());
                CHECK_MSG(written == ErrorsMarkdown(), "Docs/ERRORS.md is out of date: run the tests with --errors-markdown");
            }
        }

        void XmlParsing()
        {
            const auto parsed = Xml::Parse("<?xml version=\"1.0\"?>\n<!-- c -->\n<A x=\"1 &amp; 2\"><B>t&lt;x</B><C><![CDATA[a<b]]></C><D/></A>");
            CHECK(parsed.Root && parsed.Root->Name == "A");
            CHECK_EQ(parsed.Root->Attribute("x").value_or(""), "1 & 2");
            CHECK_EQ(parsed.Root->ChildText("B"), "t<x");
            CHECK_EQ(parsed.Root->ChildText("C"), "a<b");
            CHECK(parsed.Root->HasChild("D"));
            const auto broken = Xml::Parse("<A><B></A>");
            CHECK(!broken.Root && !broken.Error.empty());
        }

        void TableRoundTrip()
        {
            Table::AddressList list;
            Table::MemoryRecord& group = list.AddGroup("Player");
            group.Options.HideChildren = true;
            group.Options.ActivateChildren = true;
            group.AddressGroupHeader = true;
            group.Address = "game.exe+100";
            group.Offsets = { "20", "D28" };
            group.DropDown = { { "0", "Off" }, { "1", "On" } };
            Table::MemoryRecord& health = list.AddValue("Health", "game.exe+1234", Scan::ValueType::FourBytes, &group);
            health.Offsets = { "10", "8" };
            health.ShowHex = true;
            health.DropDown = { { "0", "Dead" }, { "100", "Full" } };
            Table::Hotkey hotkey;
            hotkey.Keys = { 0x11, 0x70 };
            hotkey.Action = Table::HotkeyAction::SetValue;
            hotkey.Value = "100";
            health.Hotkeys.push_back(hotkey);
            Table::MemoryRecord& name = list.AddValue("Name", "game.exe+2000", Scan::ValueType::String, &group);
            name.Length = 24;
            name.Unicode = true;
            name.DropDownLink = "Player";
            list.AddScript("God mode", "[ENABLE]\nnop\n[DISABLE]\n// <restore & end>\n");
            list.TableLuaScript = "print('hi')";
            list.DisasmNotes["game.exe+1234"] = { "the damage subtraction", "" };
            list.DisasmNotes["game.exe+20A0"] = { "", "Combat" };
            Table::StructureStore structures;
            Table::StructDefinition& definition = structures.Add("Player");
            definition.Fields.push_back({ 0x10, "Health", Scan::ValueType::FourBytes, 4, Table::FieldDisplay::Signed, false, "" });
            definition.Fields.push_back({ 0x14, "Flags", Scan::ValueType::Binary, 2, Table::FieldDisplay::Binary, false, "", "", 5, 6 });
            definition.Fields.push_back({ 0x14, "Alive", Scan::ValueType::Binary, 1, Table::FieldDisplay::Unsigned, false, "", "", 2, 1 });
            const std::string xml = Table::WriteTable(list, structures, { { "base", "game.exe+10" } });

            Table::LoadedTable loaded;
            const Status status = Table::ParseTable(xml, loaded);
            CHECK(status.Ok());
            CHECK_EQ(loaded.Records.size(), 2u);
            if (loaded.Records.size() == 2)
            {
                const Table::MemoryRecord& loadedGroup = *loaded.Records[0];
                CHECK(loadedGroup.Kind == Table::RecordKind::Group);
                CHECK(loadedGroup.Options.HideChildren && loadedGroup.Options.ActivateChildren && !loadedGroup.Options.DeactivateChildren);
                CHECK(loadedGroup.AddressGroupHeader);
                CHECK_EQ(loadedGroup.Address, "game.exe+100");
                CHECK(loadedGroup.Offsets == std::vector<std::string>({ "20", "D28" }));
                CHECK_EQ(loadedGroup.DropDown.size(), 2u);
                CHECK_EQ(loadedGroup.Children.size(), 2u);
                if (loadedGroup.Children.size() == 2)
                {
                    const Table::MemoryRecord& loadedHealth = *loadedGroup.Children[0];
                    CHECK_EQ(loadedHealth.Description, "Health");
                    CHECK_EQ(loadedHealth.Address, "game.exe+1234");
                    CHECK(loadedHealth.Offsets == std::vector<std::string>({ "10", "8" }));
                    CHECK(loadedHealth.ShowHex);
                    CHECK_EQ(loadedHealth.DropDown.size(), 2u);
                    CHECK(loadedHealth.Hotkeys.size() == 1 && loadedHealth.Hotkeys[0].Keys.size() == 2 && loadedHealth.Hotkeys[0].Value == "100" &&
                          loadedHealth.Hotkeys[0].Action == Table::HotkeyAction::SetValue);
                    const Table::MemoryRecord& loadedName = *loadedGroup.Children[1];
                    CHECK(loadedName.Type == Scan::ValueType::String && loadedName.Length == 24 && loadedName.Unicode);
                    CHECK_EQ(loadedName.DropDownLink, "Player");
                }
                const Table::MemoryRecord& script = *loaded.Records[1];
                CHECK(script.Kind == Table::RecordKind::Script);
                CHECK_EQ(script.Script, "[ENABLE]\nnop\n[DISABLE]\n// <restore & end>\n");
            }
            CHECK_EQ(loaded.LuaScript, "print('hi')");
            CHECK(loaded.Symbols.size() == 1 && loaded.Symbols[0].Name == "base");
            CHECK(loaded.Structures.size() == 1 && loaded.Structures[0]->Fields.size() == 3 && loaded.Structures[0]->Fields[0].Display == Table::FieldDisplay::Signed);
            if (loaded.Structures.size() == 1 && loaded.Structures[0]->Fields.size() == 3)
            {
                const Table::StructField& low = loaded.Structures[0]->Fields[1];
                const Table::StructField& high = loaded.Structures[0]->Fields[2];
                CHECK(low.Type == Scan::ValueType::Binary && low.Offset == 0x14 && low.BitStart == 2 && low.BitLength == 1 && low.Size == 1);
                CHECK(high.Type == Scan::ValueType::Binary && high.BitStart == 5 && high.BitLength == 6 && high.Size == 2 && high.Display == Table::FieldDisplay::Binary);
                CHECK(loaded.Structures[0]->Fields[0].Name == "Health" && high.Name == "Flags");
            }
            CHECK_EQ(loaded.DisasmNotes.size(), 2u);
            CHECK(loaded.DisasmNotes.count("game.exe+1234") && loaded.DisasmNotes["game.exe+1234"].Comment == "the damage subtraction");
            CHECK(loaded.DisasmNotes.count("game.exe+20A0") && loaded.DisasmNotes["game.exe+20A0"].Header == "Combat");

            const char* cheatEngine = R"(<?xml version="1.0" encoding="utf-8"?>
<CheatTable CheatEngineTableVersion="45">
  <CheatEntries>
    <CheatEntry>
      <ID>3</ID>
      <Description>"Ammo"</Description>
      <VariableType>4 Bytes</VariableType>
      <Address>"game.exe"+00ABC</Address>
      <Offsets>
        <Offset>1C</Offset>
        <Offset>0</Offset>
      </Offsets>
    </CheatEntry>
  </CheatEntries>
</CheatTable>
)";
            Table::LoadedTable foreign;
            CHECK(Table::ParseTable(cheatEngine, foreign).Ok());
            CHECK(foreign.Records.size() == 1 && foreign.Records[0]->Id == 3 && foreign.Records[0]->Description == "Ammo");
            CHECK(foreign.Records.size() == 1 && foreign.Records[0]->Offsets == std::vector<std::string>({ "0", "1C" }));
        }

        void AutoAssemblerOffline()
        {
            Asm::AaHost host;
            Asm::AaMemory memory;
            Asm::AaOptions options;
            options.CheckOnly = true;
            const char* good = R"(alloc(newmem,$100)
label(back)
label(data)
newmem:
  mov eax,[data]
  cmp eax,#100
  jne back
  inc eax
back:
  ret
data:
  dd #42
  db 'ok',0
)";
            Asm::AaResult result = Asm::RunAutoAssembler(good, host, memory, options);
            CHECK_MSG(result.Ok(), result.Error.Text);

            const char* bad = "alloc(newmem,$100)\nnewmem:\n  mov eax,1\n  movx eax,2\n";
            result = Asm::RunAutoAssembler(bad, host, memory, options);
            CHECK(!result.Ok());
            CHECK_EQ(result.Line, 4);
            CHECK(result.Error.Code == ErrorCode::AsmUnknownInstruction);

            const char* redeclared = "label(a)\nlabel(a)\n";
            result = Asm::RunAutoAssembler(redeclared, host, memory, options);
            CHECK_MSG(result.Ok(), result.Error.Text);

            const char* twice = "alloc(m,$10)\nlabel(a)\nm:\na:\n  nop\na:\n  nop\n";
            result = Asm::RunAutoAssembler(twice, host, memory, options);
            CHECK(!result.Ok() && result.Error.Code == ErrorCode::AaLabelTwice);

            options.Section = Asm::AaSection::Enable;
            const char* sections = "define(VALUE,#5)\n[ENABLE]\nalloc(m,10)\nm:\n  mov eax,VALUE\n[DISABLE]\n  this is not code\n";
            result = Asm::RunAutoAssembler(sections, host, memory, options);
            CHECK_MSG(result.Ok(), result.Error.Text);
            options.Section = Asm::AaSection::Disable;
            result = Asm::RunAutoAssembler(sections, host, memory, options);
            CHECK(!result.Ok());

            options.Section = Asm::AaSection::Whole;
            host.RunLua = [](std::string_view code, bool, std::string& output) -> Status
                {
                    output = code.find("magic") != std::string_view::npos ? "alloc(z,10)\nz:\n  nop\n" : "";
                    return Status::Success();
                };
            result = Asm::RunAutoAssembler("{$lua}\nreturn 'magic'\n{$asm}\n", host, memory, options);
            CHECK_MSG(result.Ok(), result.Error.Text);
            host.RunLua = nullptr;
            const char* ceStyle = "alloc(m,$100)\nlabel(a b)\nm:\n  cmp eax,1\n  jne @f\n  mov [rax],(int)5\n@@:\n  jmp @b\na:\n  dd 0\nb:\n  dd 0\nregistersymbol(a b)\n";
            result = Asm::RunAutoAssembler(ceStyle, host, memory, options);
            CHECK_MSG(result.Ok(), result.Error.Text);
            result = Asm::RunAutoAssembler("alloc(m,$10)\nm:\n  jne @f\n", host, memory, options);
            CHECK(!result.Ok());

            std::string seen;
            host.IsCustomCommand = [](std::string_view name) { return EqualsNoCase(name, "UseDefine"); };
            host.RunCustomCommand = [&seen](std::string_view, std::string_view parameters, bool, std::string& output) -> Status
                {
                    seen = std::string(parameters);
                    output = "define(made,1234)";
                    return Status::Success();
                };
            result = Asm::RunAutoAssembler("define(target,game.exe+10)\nUseDefine(target, 2)\nUseDefine(made)\n", host, memory, options);
            CHECK_MSG(result.Ok(), result.Error.Text);
            CHECK_EQ(seen, "1234");
            result = Asm::RunAutoAssembler("define(target,game.exe+10)\nUseDefine(target, 2)\n", host, memory, options);
            CHECK_EQ(seen, "game.exe+10, 2");
            host.IsCustomCommand = nullptr;
            host.RunCustomCommand = nullptr;

            CHECK(Asm::HasSections("[ENABLE]\n[DISABLE]\n"));
            CHECK_EQ(Asm::SectionText("a\n[ENABLE]\nb\n[DISABLE]\nc\n", Asm::AaSection::Disable), "a\nc\n");
        }

        void CodeSearchHelpers()
        {
            CHECK(Disasm::WildcardMatch("mov [rcx+*],eax", "mov [rcx+10],eax"));
            CHECK(!Disasm::WildcardMatch("mov [rcx+*],eax", "mov [rdx+10],eax"));
            CHECK(Disasm::WildcardMatch("j?", "jz") && !Disasm::WildcardMatch("j?", "jmp"));
            CHECK(Disasm::WildcardMatch("*", "") && Disasm::WildcardMatch("*ret*", "ret") && !Disasm::WildcardMatch("", "nop"));
            CHECK_EQ(Disasm::NormalizeAsm("MOV  dword ptr [RCX + 0x10] , EAX", false), "mov [rcx+10],eax");
            CHECK_EQ(Disasm::NormalizeAsm("mov dword ptr [rcx+10],eax", true), "mov dword ptr [rcx+10],eax");
            CHECK(Disasm::WildcardMatch(Disasm::NormalizeAsm("mov *,eax", false), Disasm::NormalizeAsm("mov dword ptr [rcx],eax", false)));
            const auto pattern = Disasm::ParseAsmPattern("lock xadd [rcx],eax; ret\n\n");
            CHECK(pattern && pattern->Lines.size() == 2 && pattern->Lines[0].Mnemonic == "xadd" && pattern->Lines[1].Mnemonic == "ret");
            const auto wild = Disasm::ParseAsmPattern("mo? rax,*");
            CHECK(wild && wild->Lines.size() == 1 && wild->Lines[0].Mnemonic.empty());
            CHECK(!Disasm::ParseAsmPattern(" ;\n ").has_value());

            std::string text;
            bool wide = false;
            const std::uint8_t ascii[] = "Hello world";
            CHECK(Disasm::ReadStringAt(std::span<const std::uint8_t>(ascii, sizeof(ascii)), text, wide) && text == "Hello world" && !wide);
            const std::uint8_t utf16[] = { 'H', 0, 'e', 0, 'l', 0, 'l', 0, 0xF6, 0, 0, 0 };
            CHECK(Disasm::ReadStringAt(std::span<const std::uint8_t>(utf16, sizeof(utf16)), text, wide) && text == "Hell\xC3\xB6" && wide);
            const std::uint8_t code[] = { 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00 };
            CHECK(!Disasm::ReadStringAt(std::span<const std::uint8_t>(code, sizeof(code)), text, wide));
            const std::uint8_t spaces[] = "    ";
            CHECK(!Disasm::ReadStringAt(std::span<const std::uint8_t>(spaces, sizeof(spaces)), text, wide));
            const std::uint8_t utf8[] = "Gr\xC3\xBC\xC3\x9F" "e";
            CHECK(Disasm::ReadStringAt(std::span<const std::uint8_t>(utf8, sizeof(utf8)), text, wide) && text == "Gr\xC3\xBC\xC3\x9F" "e" && !wide);

            Disasm::DissectResult result;
            result.Refs = { { 0x100, 0x10 }, { 0x200, 0x20 | (static_cast<std::uint64_t>(Disasm::RefKind::Write) << 56) }, { 0x200, 0x30 }, { 0x300, 0x40 } };
            CHECK_EQ(result.To(0x200).size(), 2u);
            CHECK(result.To(0x200)[0].From() == 0x20 && result.To(0x200)[0].Kind() == Disasm::RefKind::Write);
            CHECK_EQ(result.Between(0x100, 0x300).size(), 3u);
            CHECK(result.To(0x250).empty());
        }

        template <class T>
        void Put(std::vector<std::uint8_t>& block, std::size_t offset, T value)
        {
            std::memcpy(block.data() + offset, &value, sizeof(T));
        }

        void Conditions()
        {
            Disasm::RegisterValues registers;
            const auto flags = [&](std::initializer_list<int> bits)
                {
                    registers.Flags = 0x202;
                    for (const int bit : bits)
                        registers.Flags |= 1ull << bit;
                };
            flags({});
            auto info = Disasm::EvaluateCondition("jnz", registers, true);
            CHECK(info.Kind == Disasm::ConditionKind::Jump && info.Taken && info.FlagsRead == (1u << 6));
            CHECK(!Disasm::EvaluateCondition("jz", registers, true).Taken);
            CHECK(!Disasm::EvaluateCondition("je", registers, true).Taken);
            flags({ 6 });
            CHECK(Disasm::EvaluateCondition("JE", registers, true).Taken);
            CHECK(!Disasm::EvaluateCondition("jne", registers, true).Taken);
            CHECK(Disasm::EvaluateCondition("jbe", registers, true).Taken && Disasm::EvaluateCondition("jbe", registers, true).FlagsRead == ((1u << 0) | (1u << 6)));
            CHECK(!Disasm::EvaluateCondition("ja", registers, true).Taken);
            flags({ 7 });
            CHECK(Disasm::EvaluateCondition("jl", registers, true).Taken);
            CHECK(!Disasm::EvaluateCondition("jge", registers, true).Taken);
            flags({ 7, 11 });
            CHECK(!Disasm::EvaluateCondition("jl", registers, true).Taken);
            CHECK(Disasm::EvaluateCondition("jnle", registers, true).Taken && Disasm::EvaluateCondition("jg", registers, true).FlagsRead == ((1u << 6) | (1u << 7) | (1u << 11)));
            flags({ 0 });
            CHECK(Disasm::EvaluateCondition("jb", registers, true).Taken && Disasm::EvaluateCondition("jc", registers, true).Taken && !Disasm::EvaluateCondition("jnb", registers, true).Taken);
            info = Disasm::EvaluateCondition("cmovb", registers, true);
            CHECK(info.Kind == Disasm::ConditionKind::Move && info.Taken);
            info = Disasm::EvaluateCondition("setnc", registers, true);
            CHECK(info.Kind == Disasm::ConditionKind::Set && !info.Taken);
            flags({ 2 });
            CHECK(Disasm::EvaluateCondition("jp", registers, true).Taken && !Disasm::EvaluateCondition("jpo", registers, true).Taken);
            CHECK(Disasm::EvaluateCondition("jmp", registers, true).Kind == Disasm::ConditionKind::None);
            CHECK(Disasm::EvaluateCondition("mov", registers, true).Kind == Disasm::ConditionKind::None);
            CHECK(Disasm::EvaluateCondition("setssbsy", registers, true).Kind == Disasm::ConditionKind::None);
            registers.Rcx = 0x100000000ull;
            info = Disasm::EvaluateCondition("jrcxz", registers, true);
            CHECK(info.UsesCounter && !info.Taken);
            CHECK(Disasm::EvaluateCondition("jecxz", registers, true).Taken);
            registers.Rcx = 1;
            CHECK(!Disasm::EvaluateCondition("loop", registers, true).Taken);
            registers.Rcx = 2;
            flags({ 6 });
            CHECK(Disasm::EvaluateCondition("loope", registers, true).Taken && !Disasm::EvaluateCondition("loopne", registers, true).Taken);
            CHECK(Disasm::EvaluateCondition("bnd jnz", registers, true).Kind == Disasm::ConditionKind::Jump);
        }

        void ScanKinds()
        {
            CHECK_EQ(Scan::AllTypeBit(Scan::ValueType::Byte), 1u);
            CHECK_EQ(Scan::AllTypeBit(Scan::ValueType::Double), 32u);
            CHECK_EQ(Scan::AllTypeBit(Scan::ValueType::String), 0u);
            CHECK(Scan::CompareAvailable(Scan::ScanKind::All, Scan::Compare::IncreasedBy));
            CHECK(!Scan::CompareAvailable(Scan::ScanKind::Grouped, Scan::Compare::Bigger));
            CHECK(Scan::CompareAvailable(Scan::ScanKind::Custom, Scan::Compare::Unchanged));
            CHECK(Scan::CompareUsesPrevious(Scan::Compare::Decreased) && !Scan::CompareUsesPrevious(Scan::Compare::SameAsFirst));

            std::string error;
            const auto spec = Scan::ParseGroup("4:100 f:1.5 skip:4 d:* s:\"a b\"", false, 8, error);
            CHECK_MSG(spec.has_value(), error);
            if (spec)
            {
                CHECK_EQ(spec->Elements.size(), 5u);
                CHECK_EQ(spec->BlockSize, 23);
                CHECK_EQ(spec->Elements[3].Offset, 12);
                CHECK(spec->Elements[2].Skip && spec->Elements[3].Any);
                std::vector<std::uint8_t> block(23);
                Put<std::int32_t>(block, 0, 100);
                Put<float>(block, 4, 1.54f);
                Put<double>(block, 12, -7.25);
                std::memcpy(block.data() + 20, "a b", 3);
                CHECK(spec->Matches(block.data()));
                const std::string text = spec->Describe(block, 8);
                CHECK_MSG(text.find("100") == 0 && text.find("a b") != std::string::npos && text.find("-7.25") != std::string::npos, text);
                Put<float>(block, 4, 1.56f);
                CHECK(!spec->Matches(block.data()));
                Put<float>(block, 4, 1.5f);
                block[20] = 'A';
                CHECK(!spec->Matches(block.data()));
            }

            const auto range = Scan::ParseGroup("2:10..20 1:0x7F 1:-2", false, 8, error);
            CHECK_MSG(range.has_value() && range->BlockSize == 4, error);
            if (range)
            {
                std::vector<std::uint8_t> block = { 15, 0, 0x7F, 0xFE };
                CHECK(range->Matches(block.data()));
                block[0] = 25;
                CHECK(!range->Matches(block.data()));
            }
            const auto hex = Scan::ParseGroup("4:FF p:*", true, 4, error);
            CHECK(hex && hex->Elements[0].Low == 255 && hex->Elements[1].Size == 4 && hex->BlockSize == 8);
            CHECK(!Scan::ParseGroup("1:300", false, 8, error));
            CHECK(!Scan::ParseGroup("x:1", false, 8, error));
            CHECK(!Scan::ParseGroup("4:100 oops", false, 8, error));
            CHECK(!Scan::ParseGroup("4:* skip:2", false, 8, error));
            CHECK(!Scan::ParseGroup("", false, 8, error));
            CHECK(!Scan::ParseGroup("4:1 4:* within:16", false, 8, error));
            CHECK(!Scan::ParseGroup("d:1 within:4", false, 8, error));

            const auto within = Scan::ParseGroup("4:7 f:2.5 2:9 within:32", false, 8, error);
            CHECK_MSG(within.has_value() && within->BlockSize == 32, error);
            if (within)
            {
                std::vector<std::uint8_t> block(32);
                Put<std::int32_t>(block, 0, 7);
                Put<std::int16_t>(block, 6, 9);
                Put<float>(block, 20, 2.5f);
                std::vector<int> offsets;
                CHECK(within->Matches(block.data(), &offsets));
                CHECK(offsets == std::vector<int>({ 0, 20, 6 }));
                CHECK(within->Offsets(block) == offsets);
                Put<std::int32_t>(block, 0, 8);
                CHECK(!within->Matches(block.data()));
            }
        }
    }

    namespace
    {
        struct FakeMemory
        {
            std::uint8_t Bytes[16] = {};
            bool operator()(std::uint64_t address, void* out, std::uint32_t size) const
            {
                if (address < 0x5000 || address + size > 0x5000 + sizeof(Bytes))
                    return false;
                std::memcpy(out, Bytes + (address - 0x5000), size);
                return true;
            }
        };

        void BreakExpressions()
        {
            using namespace Debug;
            const SymbolResolver resolve = [](std::string_view name) -> std::optional<std::uint64_t>
                {
                    if (EqualsNoCase(name, "game.exe+10"))
                        return 0x140000010ull;
                    if (EqualsNoCase(name, "player"))
                        return 0x5000ull;
                    return std::nullopt;
                };
            std::uint64_t registers[Program::kRegisterCount] = {};
            registers[0] = 0x1122334455667788ull;
            registers[1] = 5;
            registers[2] = 0x5000;
            registers[Program::kRip] = 0x140000010ull;
            registers[Program::kFlags] = 0x246;
            FakeMemory memory;
            const std::int32_t health = 100;
            const float speed = 2.5f;
            std::memcpy(memory.Bytes, &health, 4);
            std::memcpy(memory.Bytes + 4, &speed, 4);
            const auto eval = [&](std::string_view text, bool is64Bit = true) -> std::optional<Program::Value>
                {
                    std::vector<std::uint8_t> code;
                    if (!CompileExpression(text, is64Bit, resolve, code))
                        return std::nullopt;
                    Program::Value value;
                    if (!Program::Run(code.data(), static_cast<std::uint32_t>(code.size()), registers, 77, memory, value))
                        return std::nullopt;
                    return value;
                };
            const auto truth = [&](std::string_view text)
                {
                    const auto value = eval(text);
                    return value && Program::Truthy(*value);
                };
            CHECK(truth("EAX == 0x55667788"));
            CHECK(truth("AL == 0x88 and AH == 0x77"));
            CHECK(truth("AX = 30600"));
            CHECK(truth("readInteger(RCX) == 100"));
            CHECK(truth("readFloat(RCX+4) > 2 && readFloat(RCX+4) < 3"));
            CHECK(truth("[RCX] ~= 0"));
            CHECK(truth("ZF and not CF"));
            CHECK(truth("!(SF or OF)"));
            CHECK(truth("THREADID == 77"));
            CHECK(truth("RCX == player"));
            CHECK(truth("RIP == \"game.exe+10\""));
            CHECK(truth("readInteger(RCX, true) - 101 < 0"));
            CHECK(!truth("readInteger(RCX+0x100) == 0"));
            CHECK(!truth("readInteger(RCX) == 101"));
            const auto integer = [&](std::string_view text) { const auto value = eval(text); return value && !value->IsFloat ? value->Int : -999999; };
            CHECK_EQ(integer("7 // 2"), 3);
            CHECK_EQ(integer("-7 // 2"), -4);
            CHECK_EQ(integer("-7 % 3"), 2);
            CHECK_EQ(integer("1 << 4 | 1"), 17);
            CHECK_EQ(integer("0x10 >> 4"), 1);
            CHECK_EQ(integer("$FF"), 255);
            CHECK_EQ(integer("7FF"), 0x7FF);
            CHECK_EQ(integer("10"), 10);
            CHECK_EQ(integer("~0 & 0xF"), 15);
            CHECK_EQ(integer("6 ~ 3"), 5);
            CHECK_EQ(integer("RBX * 2 + 1"), 11);
            CHECK_EQ(integer("0x7FFFFFFFFFFFFFFF + 1 < 0"), 1);
            const auto half = eval("1 / 2");
            CHECK(half && half->IsFloat && half->Float == 0.5);
            CHECK(!eval("EAX =="));
            CHECK(!eval("(EAX"));
            CHECK(!eval("foo == 1"));
            CHECK(!eval("readThing(RCX)"));
            CHECK(!eval("R8 == 1", false));
            CHECK(eval("EAX == 1", false).has_value());
            std::vector<std::uint8_t> code;
            const Status bad = CompileExpression("RAX + ", true, resolve, code);
            CHECK(!bad && bad.Code == ErrorCode::DebugBadExpression && code.empty());

            std::vector<RegisterChange> changes;
            const Status compiled = CompileChanges("RAX = RBX; RBX = RAX\nZF = 0 -- clear it\nAL = 0x12; ECX = -1", true, resolve, changes);
            CHECK_MSG(compiled.Ok(), compiled.Text);
            CHECK_EQ(changes.size(), static_cast<std::size_t>(5));
            Program::Change shared[Program::kMaxChanges] = {};
            for (std::size_t i = 0; i < changes.size() && i < static_cast<std::size_t>(Program::kMaxChanges); ++i)
            {
                shared[i].Target = changes[i].Target;
                shared[i].Part = changes[i].Part;
                shared[i].Length = static_cast<std::uint8_t>(changes[i].Code.size());
                std::memcpy(shared[i].Code, changes[i].Code.data(), changes[i].Code.size());
            }
            const std::uint64_t oldRax = registers[0];
            CHECK_EQ(Program::Apply(shared, static_cast<int>(changes.size()), registers, 77, memory), 5);
            CHECK_EQ(registers[0], 0x12ull);
            CHECK_EQ(registers[1], oldRax);
            CHECK_EQ(registers[2], 0xFFFFFFFFull);
            CHECK_EQ(registers[Program::kFlags] & 0x40, 0ull);
            CHECK(!CompileChanges("RAX == 1", true, resolve, changes));
            CHECK(!CompileChanges("XYZ = 1", true, resolve, changes));
            CHECK(!CompileChanges("R9 = 1", false, resolve, changes));

            BreakAction action;
            action.Condition = "  ";
            action.Changes = "";
            CHECK(CompileAction(action, true, resolve).Ok() && action.Plain());
            action.Condition = "RAX == 1";
            action.Changes = "RBX = 2";
            action.Break = false;
            CHECK(CompileAction(action, true, resolve).Ok() && !action.Plain());
            CHECK_EQ(DescribeAction(action), std::string("if RAX == 1  \u00b7  set RBX = 2  \u00b7  keeps running"));
        }
    }

    void RecordIndex()
    {
        Table::AddressList list;
        Table::MemoryRecord& group = list.AddGroup("Group");
        Table::MemoryRecord& first = list.AddValue("Health", "1000", Scan::ValueType::FourBytes, &group);
        Table::MemoryRecord& second = list.AddValue("Ammo", "1004", Scan::ValueType::FourBytes);
        CHECK(list.FindById(first.Id) == &first && list.FindById(second.Id) == &second && list.FindById(group.Id) == &group);
        CHECK(list.FindById(12345) == nullptr);
        CHECK(list.FindByDescription("Ammo") == &second);
        const int firstId = first.Id;
        list.Move(first, nullptr, 0);
        CHECK(list.FindById(firstId) == &first && first.Parent == nullptr);
        auto copy = std::make_unique<Table::MemoryRecord>();
        copy->Id = second.Id;
        copy->Description = "Copy";
        Table::MemoryRecord& added = list.Add(std::move(copy));
        CHECK(added.Id != second.Id && list.FindById(added.Id) == &added && list.FindById(second.Id) == &second);
        second.Description = "Bullets";
        list.Touch();
        CHECK(list.FindByDescription("Ammo") == nullptr && list.FindByDescription("Bullets") == &second);
        const int groupId = group.Id;
        auto child = std::make_unique<Table::MemoryRecord>();
        child->Description = "Inner";
        Table::MemoryRecord& inner = list.Add(std::move(child), &group);
        const int innerId = inner.Id;
        list.Remove(group);
        CHECK(list.FindById(groupId) == nullptr && list.FindById(innerId) == nullptr && list.FindByDescription("Inner") == nullptr);
        CHECK(list.FindById(firstId) == &first);
        list.Clear();
        CHECK(list.FindById(firstId) == nullptr && list.FindByDescription("Health") == nullptr);
    }

    void SoundDecoding()
    {
        std::filesystem::path at = std::filesystem::current_path();
        for (int i = 0; i < 8 && !std::filesystem::exists(at / "Assets" / "Sounds" / "Success.mp3"); ++i)
            at = at.parent_path();
        const std::filesystem::path file = at / "Assets" / "Sounds" / "Success.mp3";
        if (!std::filesystem::exists(file))
        {
            std::printf("  SKIP Assets/Sounds/Success.mp3 not found\n");
            return;
        }
        const auto read = [](const std::filesystem::path& path)
            {
                std::ifstream input(path, std::ios::binary);
                return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            };
        const auto checkWave = [](const std::vector<std::uint8_t>& wave, const std::string& name)
            {
                CHECK_MSG(wave.size() > 44, name + " is not a sound");
                if (wave.size() <= 44)
                    return;
                CHECK_MSG(std::memcmp(wave.data(), "RIFF", 4) == 0 && std::memcmp(wave.data() + 8, "WAVE", 4) == 0 && std::memcmp(wave.data() + 12, "fmt ", 4) == 0, name);
                std::uint16_t tag = 0;
                std::uint16_t channels = 0;
                std::uint32_t rate = 0;
                std::uint32_t perSecond = 0;
                std::uint32_t data = 0;
                std::memcpy(&tag, wave.data() + 20, 2);
                std::memcpy(&channels, wave.data() + 22, 2);
                std::memcpy(&rate, wave.data() + 24, 4);
                std::memcpy(&perSecond, wave.data() + 28, 4);
                std::memcpy(&data, wave.data() + 40, 4);
                CHECK_MSG(tag == 1 && channels >= 1 && channels <= 2 && rate >= 8000 && rate <= 96000, name);
                CHECK_MSG(static_cast<std::size_t>(data) + 44 == wave.size(), name);
                const double seconds = perSecond ? static_cast<double>(data) / perSecond : 0.0;
                CHECK_MSG(seconds > 0.1 && seconds < 10.0, name + " lasts " + std::to_string(seconds) + " s");
            };
        checkWave(Sounds::DecodeToWave(read(file)), "Success.mp3 decoded");
        checkWave(Sounds::DecodeToWave(read(file.parent_path() / "Error.mp3")), "Error.mp3 decoded");
        CHECK(Sounds::DecodeToWave({}).empty());
        CHECK(Sounds::DecodeToWave({ 1, 2, 3, 4, 5 }).empty());
    }

    void Jobs()
    {
        JobQueue jobs;
        int finished = 0;
        std::thread::id worker;
        for (int i = 0; i < 3; ++i)
        {
            jobs.Post([&worker, &finished, i]() -> JobQueue::Finish
                {
                    worker = std::this_thread::get_id();
                    return [&finished, i]() { finished += i + 1; };
                });
        }
        for (int wait = 0; wait < 200 && jobs.Busy(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(!jobs.Busy());
        CHECK_EQ(finished, 0);
        jobs.Drain();
        CHECK_EQ(finished, 6);
        CHECK(worker != std::thread::id() && worker != std::this_thread::get_id());
        jobs.Post([]() -> JobQueue::Finish { throw std::runtime_error("job failed on purpose"); });
        jobs.Post([&finished]() -> JobQueue::Finish { return [&finished]() { finished = 100; }; });
        for (int wait = 0; wait < 200 && jobs.Busy(); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        jobs.Drain();
        CHECK_EQ(finished, 100);
        jobs.Stop();
        jobs.Post([&finished]() -> JobQueue::Finish { return [&finished]() { finished = -1; }; });
        jobs.Drain();
        CHECK_EQ(finished, 100);
    }

    int RunCoreTests()
    {
        const int before = Totals().Failures;
        ValueTypes();
        TextHelpers();
        Errors();
        XmlParsing();
        TableRoundTrip();
        AutoAssemblerOffline();
        CodeSearchHelpers();
        ScanKinds();
        Conditions();
        BreakExpressions();
        Jobs();
        SoundDecoding();
        RecordIndex();
        return Totals().Failures - before;
    }
}
