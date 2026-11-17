#include "TestKit.h"

#include "Asm/AutoAssembler.h"
#include "Core/Errors.h"
#include "App/Diagnostics.h"
#include "App/Engine.h"
#include "App/Sounds.h"
#include "Core/JobQueue.h"
#include "Core/Profiler.h"
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

#include <windows.h>

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

    void Spin(double milliseconds)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < milliseconds)
        {
        }
    }

    const Profiler::ZoneView* FindZone(const std::vector<Profiler::ZoneView>& tree, std::string_view name)
    {
        for (const Profiler::ZoneView& zone : tree)
        {
            if (name == zone.Name)
                return &zone;
        }
        return nullptr;
    }

    void ProfilerZones()
    {
        Profiler::SetEnabled(false);
        Profiler::Reset();
        {
            PROFILE_ZONE("Test while off");
        }
        Profiler::SetEnabled(true);
        Profiler::BeginFrame();
        for (int frame = 0; frame < 5; ++frame)
        {
            {
                PROFILE_ZONE("Test outer");
                Spin(2.0);
                for (int call = 0; call < 2; ++call)
                {
                    PROFILE_ZONE("Test inner");
                    Spin(1.0);
                }
            }
            {
                PROFILE_WAIT("Test wait");
                Spin(3.0);
            }
            std::thread([] { PROFILE_ZONE("Test other thread"); }).join();
            Profiler::Count("Test counter", frame);
            Profiler::BeginFrame();
        }
        CHECK_EQ(Profiler::Frames(), 5);
        CHECK_EQ(Profiler::FrameCount(), 5u);
        const std::vector<Profiler::ZoneView> tree = Profiler::Tree();
        const Profiler::ZoneView* outer = FindZone(tree, "Test outer");
        const Profiler::ZoneView* inner = FindZone(tree, "Test inner");
        const Profiler::ZoneView* wait = FindZone(tree, "Test wait");
        CHECK(outer && inner && wait);
        CHECK(!FindZone(tree, "Test while off"));
        CHECK(!FindZone(tree, "Test other thread"));
        if (outer && inner && wait)
        {
            CHECK_EQ(outer->Depth, 1);
            CHECK_EQ(inner->Depth, 2);
            CHECK_EQ(inner->Parent, outer->Node);
            CHECK(wait->Wait && !outer->Wait);
            CHECK(std::abs(inner->CallsPerFrame - 2.0) < 1e-9);
            CHECK_MSG(outer->AverageMs >= 3.8, std::to_string(outer->AverageMs));
            CHECK_MSG(outer->SelfMs >= 1.8 && outer->SelfMs < outer->AverageMs - 1.5, std::to_string(outer->SelfMs));
            CHECK(outer->MaxMs >= outer->AverageMs);
            CHECK(Profiler::ZoneMs(inner->Node, 0) >= 1.9);
            CHECK(std::abs(Profiler::ZoneCalls(inner->Node, 0) - 2.0) < 1e-9);
        }
        CHECK_MSG(Profiler::FrameMs(0) >= Profiler::WorkMs(0) + 2.8, std::to_string(Profiler::FrameMs(0)) + " vs " + std::to_string(Profiler::WorkMs(0)));
        CHECK(Profiler::WorkMs(0) >= 3.8);
        const Profiler::FrameSummary summary = Profiler::Summary();
        CHECK_EQ(summary.Frames, 5);
        CHECK(summary.PeriodMs > summary.WorkMs && summary.WorkP50 <= summary.WorkP99 && summary.WorkP99 <= summary.WorkMax);
        CHECK(summary.FramesPerSecond > 0.0 && summary.FramesPerSecond < 1000.0 / 6.5);
        bool counted = false;
        for (const Profiler::CounterView& counter : Profiler::Counters())
            counted = counted || (std::string_view(counter.Name) == "Test counter" && counter.Last == 4.0 && counter.Max == 4.0);
        CHECK(counted);
        std::vector<float> history;
        Profiler::WorkHistory(history);
        CHECK_EQ(history.size(), 5u);
        Profiler::SetPaused(true);
        Profiler::BeginFrame();
        Profiler::BeginFrame();
        CHECK_EQ(Profiler::Frames(), 5);
        Profiler::SetPaused(false);
        Profiler::Reset();
        CHECK_EQ(Profiler::Frames(), 0);
        CHECK_EQ(Profiler::FrameCount(), 0u);
        CHECK_EQ(Profiler::FrameMs(0), 0.0);
        Profiler::SetEnabled(false);
    }

    void FolderMigration()
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / ("Manifold-Migration-Tests-" + std::to_string(::GetCurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        const auto write = [](const std::filesystem::path& path, const std::string& text)
            {
                std::error_code error;
                std::filesystem::create_directories(path.parent_path(), error);
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << text;
            };
        const auto read = [](const std::filesystem::path& path)
            {
                std::ifstream file(path, std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            };
        const auto nameOnDisk = [](const std::filesystem::path& path)
            {
                WIN32_FIND_DATAW data{};
                const HANDLE find = ::FindFirstFileW(path.c_str(), &data);
                if (find == INVALID_HANDLE_VALUE)
                    return std::wstring();
                ::FindClose(find);
                return std::wstring(data.cFileName);
            };
        const auto exists = [](const std::filesystem::path& path) { return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; };
        const auto first = [](const std::vector<std::string>& lines) { return lines.empty() ? std::string() : lines.front(); };

        const std::filesystem::path current = root / "current";
        write(current / "Manifold-Engine.ini", "[Interface]\nCompact=1\n");
        write(current / "Manifold-Engine.layout.ini", "[Window][Main]\n");
        write(current / "Manifold-Engine.scripts.ini", "[Test]\r\nKey=Value\r\n");
        write(current / "Manifold-CheatEngine.ini", "[Interface]\nCompact=0\n");
        write(current / "Manifold-CheatEngine.layout.ini", "earlier layout");
        write(current / "Tables" / "Game.CT", "<CheatTable/>");
        write(current / "Sounds" / "Success.wav", "RIFF");
        write(current / "Manifold-Engine.log", "last run");
        write(current / "Manifold-Engine.previous.log", "run before");
        const Paths::Migration moved = Paths::AdoptEarlierFiles(current);
        CHECK_MSG(moved.Problems.empty(), first(moved.Problems));
        CHECK_EQ(moved.Moved.size(), static_cast<std::size_t>(7));
        CHECK_EQ(read(current / "logs" / "Manifold-Engine.log"), "last run");
        CHECK_EQ(read(current / "logs" / "Manifold-Engine.previous.log"), "run before");
        CHECK(!exists(current / "Manifold-Engine.log") && !exists(current / "Manifold-Engine.previous.log"));
        CHECK_EQ(read(current / "configs" / "Manifold-Engine.ini"), "[Interface]\nCompact=1\n");
        CHECK_EQ(read(current / "configs" / "Manifold-Engine.layout.ini"), "[Window][Main]\n");
        CHECK_EQ(read(current / "configs" / "Manifold-Engine.scripts.ini"), "[Test]\r\nKey=Value\r\n");
        CHECK(!exists(current / "Manifold-Engine.ini") && !exists(current / "Manifold-Engine.layout.ini") && !exists(current / "Manifold-Engine.scripts.ini"));
        CHECK_EQ(read(current / "Manifold-CheatEngine.ini"), "[Interface]\nCompact=0\n");
        CHECK_EQ(read(current / "Manifold-CheatEngine.layout.ini"), "earlier layout");
        CHECK(nameOnDisk(current / "tables") == L"tables");
        CHECK(nameOnDisk(current / "sounds") == L"sounds");
        CHECK_EQ(read(current / "tables" / "Game.CT"), "<CheatTable/>");
        CHECK_EQ(read(current / "sounds" / "Success.wav"), "RIFF");
        CHECK(!exists(current / "tables.renaming") && !exists(current / "sounds.renaming"));
        Settings loaded;
        CHECK(loaded.Load(current / "configs" / "Manifold-Engine.ini") && loaded.CompactMode);

        const Paths::Migration again = Paths::AdoptEarlierFiles(current);
        CHECK_MSG(again.Moved.empty() && again.Problems.empty(), first(again.Moved) + first(again.Problems));
        CHECK(Paths::LeftoverFiles(current).empty());

        write(current / "Manifold-Engine.ini", "[Interface]\nCompact=0\n");
        write(current / "Manifold-Engine.log", "older program");
        const Paths::Migration collision = Paths::AdoptEarlierFiles(current);
        CHECK(collision.Moved.empty());
        CHECK_EQ(read(current / "configs" / "Manifold-Engine.ini"), "[Interface]\nCompact=1\n");
        CHECK_EQ(read(current / "Manifold-Engine.ini"), "[Interface]\nCompact=0\n");
        CHECK_EQ(read(current / "logs" / "Manifold-Engine.log"), "last run");
        CHECK_EQ(read(current / "Manifold-Engine.log"), "older program");
        CHECK(Paths::LeftoverFiles(current) == std::vector<std::string>{ "Manifold-Engine.ini" });

        const std::filesystem::path earlier = root / "earlier";
        write(earlier / "Manifold-CheatEngine.ini", "[Interface]\nCompact=1\n");
        write(earlier / "Manifold-CheatEngine.layout.ini", "[Window][Main]\n");
        write(earlier / "Manifold-CheatEngine.log", "earlier run");
        write(earlier / "Manifold-CheatEngine.previous.log", "earlier run before");
        const Paths::Migration copied = Paths::AdoptEarlierFiles(earlier);
        CHECK_MSG(copied.Problems.empty(), first(copied.Problems));
        CHECK_EQ(read(earlier / "logs" / "Manifold-CheatEngine.log"), "earlier run");
        CHECK_EQ(read(earlier / "logs" / "Manifold-CheatEngine.previous.log"), "earlier run before");
        CHECK(!exists(earlier / "Manifold-CheatEngine.log") && !exists(earlier / "Manifold-CheatEngine.previous.log"));
        CHECK_EQ(read(earlier / "configs" / "Manifold-Engine.ini"), "[Interface]\nCompact=1\n");
        CHECK_EQ(read(earlier / "configs" / "Manifold-Engine.layout.ini"), "[Window][Main]\n");
        CHECK_EQ(read(earlier / "Manifold-CheatEngine.ini"), "[Interface]\nCompact=1\n");
        CHECK(!exists(earlier / "Manifold-Engine.ini") && !exists(earlier / "tables") && !exists(earlier / "sounds"));

        const std::filesystem::path odd = root / "odd";
        write(odd / "Tables", "not a folder");
        write(odd / "Sounds" / "Error.wav", "RIFF");
        write(odd / "sounds.renaming" / "keep.txt", "keep");
        const Paths::Migration file = Paths::AdoptEarlierFiles(odd);
        CHECK_EQ(file.Problems.size(), static_cast<std::size_t>(1));
        CHECK(nameOnDisk(odd / "Tables") == L"Tables");
        CHECK_EQ(read(odd / "Tables"), "not a folder");
        CHECK(nameOnDisk(odd / "sounds") == L"sounds");
        CHECK_EQ(read(odd / "sounds" / "Error.wav"), "RIFF");
        CHECK_EQ(read(odd / "sounds.renaming" / "keep.txt"), "keep");
        CHECK(!exists(odd / "sounds.renaming1"));

        const std::filesystem::path locked = root / "locked";
        write(locked / "Tables" / "Game.CT", "<CheatTable/>");
        const HANDLE holder = ::CreateFileW((locked / "Tables").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        CHECK(holder != INVALID_HANDLE_VALUE);
        const Paths::Migration busy = Paths::AdoptEarlierFiles(locked);
        if (holder != INVALID_HANDLE_VALUE)
            ::CloseHandle(holder);
        CHECK_EQ(busy.Problems.size(), static_cast<std::size_t>(1));
        CHECK(nameOnDisk(locked / "Tables") == L"Tables");
        CHECK(!exists(locked / "tables.renaming"));
        const Paths::Migration later = Paths::AdoptEarlierFiles(locked);
        CHECK_MSG(later.Problems.empty(), first(later.Problems));
        CHECK(nameOnDisk(locked / "tables") == L"tables");
        CHECK_EQ(read(locked / "tables" / "Game.CT"), "<CheatTable/>");

        const std::filesystem::path program = L"C:\\Games\\Manifold";
        CHECK_EQ(Paths::CurrentTablePath("C:\\Games\\Manifold\\Tables\\Game.CT", program), "C:\\Games\\Manifold\\tables\\Game.CT");
        CHECK_EQ(Paths::CurrentTablePath("c:\\games\\manifold\\TABLES\\Sub\\Game.CT", program), "C:\\Games\\Manifold\\tables\\Sub\\Game.CT");
        CHECK_EQ(Paths::CurrentTablePath("C:\\Games\\Manifold\\TablesOld\\Game.CT", program), "C:\\Games\\Manifold\\TablesOld\\Game.CT");
        CHECK_EQ(Paths::CurrentTablePath("D:\\Tables\\Game.CT", program), "D:\\Tables\\Game.CT");
        CHECK_EQ(Paths::CurrentTablePath("C:\\Games\\Manifold\\Tables\\", program), "C:\\Games\\Manifold\\Tables\\");

        std::filesystem::remove_all(root, ignored);
    }

    void DiagnosticsInfo()
    {
        CHECK_EQ(Diagnostics::Milliseconds(12.345), "12.3 ms");
        CHECK_EQ(Diagnostics::Milliseconds(0.25), "0.25 ms");
        CHECK_EQ(Diagnostics::Milliseconds(0.05), "50 us");
        CHECK_EQ(Diagnostics::Bytes(3u << 20), "3.0 MB");
        CHECK_EQ(Diagnostics::Bytes(512), "512 bytes");

        Settings settings;
        Engine engine(settings);
        Diagnostics::Sample(true);
        Spin(20.0);
        Diagnostics::Sample(true);
        const Diagnostics::ProcessSample& sample = Diagnostics::LastSample();
        CHECK(sample.Valid);
        CHECK(sample.PrivateBytes > 0 && sample.WorkingSet > 0 && sample.Handles > 0 && sample.Threads >= 1);
        const std::vector<Diagnostics::ThreadSample>& threads = Diagnostics::LastThreads();
        CHECK(!threads.empty());
        CHECK(std::any_of(threads.begin(), threads.end(), [](const Diagnostics::ThreadSample& thread) { return thread.Main && !thread.Start.empty(); }));

        const std::vector<Diagnostics::InfoGroup> groups = Diagnostics::CollectInfo(engine);
        const auto group = [&](std::string_view title) {
            return std::find_if(groups.begin(), groups.end(), [&](const Diagnostics::InfoGroup& entry) { return entry.Title == title; }) != groups.end();
        };
        CHECK(group("Build") && group("System") && group("Graphics") && group("This program") && group("Engine") && group("Lua") && group("Settings"));
        if (::GetSystemMetrics(SM_CMONITORS) > 0)
        {
            CHECK(Diagnostics::DisplayAdapterLuid(nullptr) != 0);
            const auto screens = std::find_if(groups.begin(), groups.end(), [](const Diagnostics::InfoGroup& entry) { return entry.Title == "Displays"; });
            CHECK(screens != groups.end() && std::any_of(screens->Rows.begin(), screens->Rows.end(), [](const Diagnostics::InfoRow& row) { return row.Key.rfind("\\\\.\\DISPLAY", 0) == 0 && row.Value.find(" Hz, on ") != std::string::npos; }));
        }
        const std::string preference = Diagnostics::WindowsGraphicsPreference();
        CHECK(preference.empty() || preference == "Power saving" || preference == "High performance" || preference == "a chosen graphics card");

        const std::vector<Diagnostics::Check> checks = Diagnostics::RunChecks(engine);
        CHECK(checks.size() >= 4);
        bool sorted = true;
        for (std::size_t i = 1; i < checks.size(); ++i)
            sorted = sorted && static_cast<int>(checks[i - 1].Severity) >= static_cast<int>(checks[i].Severity);
        CHECK(sorted);
        CHECK(std::any_of(checks.begin(), checks.end(), [](const Diagnostics::Check& check) { return check.Area == "Files" && check.Title.find("program folder") != std::string::npos; }));
        CHECK(std::any_of(checks.begin(), checks.end(), [](const Diagnostics::Check& check) { return check.Act == Diagnostics::Action::TurnOnProfiler; }));

        const std::string report = Diagnostics::Report(engine, { { "Extra group", { { "Extra key", "Extra value" } } } });
        CHECK(report.rfind("# Manifold Engine diagnostics", 0) == 0);
        CHECK(report.find("## Checks") != std::string::npos);
        CHECK(report.find("## Build") != std::string::npos);
        CHECK(report.find("## Extra group") != std::string::npos && report.find("- Extra key: Extra value") != std::string::npos);
        CHECK(report.find("## Threads") != std::string::npos);

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency >= 30)
        {
            const auto twoRefreshes = std::chrono::microseconds(static_cast<long long>(2.2e6 / mode.dmDisplayFrequency));
            Profiler::SetEnabled(false);
            Profiler::Reset();
            Profiler::SetEnabled(true);
            Profiler::BeginFrame();
            for (int frame = 0; frame < 70; ++frame)
            {
                if (frame < 10)
                {
                    PROFILE_WAIT("Waiting for input");
                    std::this_thread::sleep_for(twoRefreshes);
                }
                {
                    PROFILE_WAIT("Test present");
                    std::this_thread::sleep_for(twoRefreshes);
                }
                Profiler::BeginFrame();
            }
            const std::vector<Diagnostics::Check> paced = Diagnostics::RunChecks(engine);
            const auto missed = std::find_if(paced.begin(), paced.end(), [](const Diagnostics::Check& check) { return check.Title == "Frames miss the display's refresh"; });
            CHECK(missed != paced.end());
            if (missed != paced.end())
            {
                CHECK(missed->Severity == Diagnostics::Level::Warning);
                CHECK_MSG(missed->Detail.find("60 of the last 60 frames") == 0, missed->Detail);
                CHECK_MSG(missed->Detail.find("waiting until the graphics card") != std::string::npos, missed->Detail);
            }
            Profiler::SetEnabled(false);
            Profiler::Reset();
        }
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
        FolderMigration();
        RecordIndex();
        ProfilerZones();
        DiagnosticsInfo();
        return Totals().Failures - before;
    }
}
