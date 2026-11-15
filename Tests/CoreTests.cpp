#include "TestKit.h"

#include "Asm/AutoAssembler.h"
#include "Core/Errors.h"
#include "Core/Text.h"
#include "Core/Xml.h"
#include "Scan/ValueType.h"
#include "Table/CheatTable.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

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
        return Totals().Failures - before;
    }
}
