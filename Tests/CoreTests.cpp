#include "TestKit.h"

#include "Asm/AutoAssembler.h"
#include "Core/Errors.h"
#include "App/Diagnostics.h"
#include "App/Engine.h"
#include "App/Sounds.h"
#include "App/Notifications.h"
#include "App/SettingsStore.h"
#include "Core/BackgroundWork.h"
#include "Core/Credits.h"
#include "Core/Deflate.h"
#include "Core/Handle.h"
#include "Core/Ini.h"
#include "Core/MainThread.h"
#include "Core/Types.h"
#include "Core/Profiler.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/Xml.h"
#include "Debug/BreakAction.h"
#include "Disasm/AssemblySearch.h"
#include "Disasm/Disassembler.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"
#include "Scan/TypeCatalog.h"
#include "Scan/ValueType.h"
#include "Table/CheatTable.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <type_traits>
#include <variant>

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
            CHECK(!Scan::ParseValue(Scan::ValueType::Byte, "300.5", format));
            CHECK(!Scan::ParseValue(Scan::ValueType::TwoBytes, "-40000.5", format));
            const auto roundedByte = Scan::ParseValue(Scan::ValueType::Byte, "254.6", format);
            CHECK(roundedByte && (*roundedByte)[0] == 0xFF);
            const auto negativeByte = Scan::ParseValue(Scan::ValueType::Byte, "-1.2", format);
            CHECK(negativeByte && (*negativeByte)[0] == 0xFF);
            CHECK(!Scan::ParseValue(Scan::ValueType::FourBytes, "1e30", format));
            const auto plus = Scan::ParseValue(Scan::ValueType::Double, "+1.5", format);
            CHECK(plus && Scan::FormatValue(Scan::ValueType::Double, *plus, format) == "1.5");
            CHECK(!Scan::ParseValue(Scan::ValueType::Float, "+-1.5", format));
            CHECK(ParseDouble("+2,5").value_or(0.0) == 2.5 && !ParseDouble("++2") && !ParseDouble("+"));
            CHECK(ParseHex("0000000000000000001F").value_or(0) == 0x1F);
            CHECK(ParseHex("0x00000000000000000000FFFFFFFFFFFFFFFF").value_or(0) == 0xFFFFFFFFFFFFFFFFull);
            CHECK(!ParseHex("1FFFFFFFFFFFFFFFF") && ParseHex("0").value_or(1) == 0 && ParseHex("0000").value_or(1) == 0);
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

        void TypeCategories()
        {
            const auto titles = [](const std::vector<Scan::TypeSection>& sections)
                {
                    std::string text;
                    for (const Scan::TypeSection& section : sections)
                        text += section.Title + ":" + std::to_string(section.Entries.size()) + ";";
                    return text;
                };
            Scan::TypeCatalogOptions scan{ .Types = Scan::kScanTypes, .Several = true };
            scan.Customs = { { "UE FName", 8, false, true }, { "Godot String", 8, false, true }, { "3 Byte", 3 }, { "float big endian", 4, true },
                { "BCD", 4 }, { "Pascal String", 32, false, true }, { "UE", 4 } };
            const std::vector<Scan::TypeSection> sections = Scan::TypeCatalog(scan);
            CHECK_EQ(titles(sections), "Whole Numbers:4;Decimals:2;Text and Bytes:2;Several at Once:2;Custom Numbers:3;Custom Decimals:1;Custom Text:1;Godot:1;Unreal Engine:1;");
            CHECK_EQ(sections[4].Entries[0].Label, "3 Byte");
            CHECK_EQ(sections[4].Entries[1].Label, "BCD");
            CHECK_EQ(sections[4].Entries[2].Label, "UE");
            CHECK_EQ(sections[4].Entries[0].Detail, "3 bytes");
            CHECK_EQ(sections[0].Entries[0].Detail, "1 byte");
            CHECK(sections[3].Entries[0].Choice.Kind == Scan::ScanKind::All && sections[3].Entries[0].Tooltip);
            CHECK(sections[8].Entries[0].Choice.Same({ Scan::ScanKind::Custom, Scan::ValueType::Byte, "ue fname" }));
            CHECK(!sections[0].Entries[0].Choice.Same({ Scan::ScanKind::Single, Scan::ValueType::TwoBytes, {} }));
            CHECK(Scan::TypeEntryMatches(sections[5].Entries[0], " BIG ") && !Scan::TypeEntryMatches(sections[5].Entries[0], "string"));

            const std::vector<Scan::TypeSection> records = Scan::TypeCatalog({ .Types = Scan::kRecordTypes, .PointerSize = 4 });
            CHECK_EQ(titles(records), "Whole Numbers:4;Decimals:2;Text and Bytes:2;Bits and Pointers:2;");
            CHECK_EQ(records[3].Entries[1].Detail, "4 bytes");
            CHECK_EQ(Scan::TypeChoiceLabel({ Scan::ScanKind::Grouped }), "Grouped");
            for (Scan::ValueType type : Scan::kRecordTypes)
                CHECK(std::string_view(Scan::TypeCategory(type)).size() > 0);
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

        void LogEntries()
        {
            const auto before = RuntimeLog::Last();
            RuntimeLog::Write(LogLevel::Warning, "LogTest", __FILE__, __func__, __LINE__, "first line\nsecond line");
            const auto entry = RuntimeLog::Last();
            CHECK(entry.has_value());
            if (!entry)
                return;
            CHECK(entry->Area == "LogTest" && entry->Level == LogLevel::Warning);
            CHECK(entry->Message == "first line second line");
            CHECK(entry->Full == "first line\nsecond line" && entry->Text() == entry->Full);
            CHECK(entry->Milliseconds >= 0 && entry->Milliseconds < 1000 && entry->Time.size() == 8);
            CHECK(entry->Thread == ::GetCurrentThreadId());
            CHECK(entry->Clock > 0.0);
            CHECK(!before || entry->Serial > before->Serial);
            RuntimeLog::Write(LogLevel::Info, "LogTest", __FILE__, __func__, __LINE__, "plain");
            const auto plain = RuntimeLog::Last();
            CHECK(plain && plain->Full.empty() && plain->Text() == "plain" && plain->Serial == entry->Serial + 1 && plain->Clock >= entry->Clock);
            CHECK(RuntimeLog::EntryMatches(*entry, LogLevel::Info, "SECOND LINE"));
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
            else
                Skip("Docs/ERRORS.md not found above " + std::filesystem::current_path().string());
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

            // Numeric entities: only characters XML allows; NUL, surrogates, values past U+10FFFF and bad digits stay as written.
            const auto entities = Xml::Parse("<A>&#65;&#x42;&#x1F600;|&#0;|&#;|&#x;|&#xD800;|&#x110000;|&#-5;|&#12a;|&#x0000000041;</A>");
            CHECK(entities.Root);
            if (entities.Root)
            {
                CHECK_EQ(entities.Root->Text, std::string("AB\xF0\x9F\x98\x80|&#0;|&#;|&#x;|&#xD800;|&#x110000;|&#-5;|&#12a;|&#x0000000041;"));
                CHECK(entities.Root->Text.find('\0') == std::string::npos);
            }

            // Control characters go out as character references and come back;
            // NUL is left out; a tab or line break in an attribute survives.
            Xml::Node node;
            node.Name = "A";
            node.SetAttribute("x", std::string("a\tb\nc\x01", 6));
            node.Text = std::string("line\nnext <b> \x1B[0m\x07|\x00|", 22);
            const std::string written = Xml::Write(node, false);
            CHECK(written.find('\x1B') == std::string::npos && written.find('\x07') == std::string::npos && written.find('\x01') == std::string::npos);
            CHECK(written.find('\0') == std::string::npos);
            CHECK(written.find("CDATA") == std::string::npos);
            CHECK(written.find("x=\"a&#x9;b&#xA;c&#x1;\"") != std::string::npos);
            const auto reread = Xml::Parse(written);
            CHECK(reread.Root);
            if (reread.Root)
            {
                CHECK_EQ(reread.Root->Attribute("x").value_or(""), std::string("a\tb\nc\x01", 6));
                CHECK_EQ(reread.Root->Text, std::string("line\nnext <b> \x1B[0m\x07||", 21));
            }
            CHECK_EQ(EscapeXml("a\tb\r\nc"), "a\tb\r\nc");
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

            const char* oversized = R"(<?xml version="1.0" encoding="utf-8"?>
<CheatTable CheatEngineTableVersion="45">
  <CheatEntries>
    <CheatEntry>
      <ID>1</ID>
      <VariableType>String</VariableType>
      <Length>2147483647</Length>
      <Address>1000</Address>
    </CheatEntry>
    <CheatEntry>
      <ID>2</ID>
      <VariableType>Binary</VariableType>
      <BitStart>200</BitStart>
      <BitLength>-5</BitLength>
      <Address>1000</Address>
    </CheatEntry>
    <CheatEntry>
      <ID>3</ID>
      <VariableType>Binary</VariableType>
      <BitStart>60</BitStart>
      <BitLength>32</BitLength>
      <Address>1000</Address>
    </CheatEntry>
  </CheatEntries>
</CheatTable>
)";
            Table::LoadedTable clamped;
            CHECK(Table::ParseTable(oversized, clamped).Ok() && clamped.Records.size() == 3);
            if (clamped.Records.size() == 3)
            {
                CHECK_EQ(clamped.Records[0]->Length, Table::kMaxRecordLength);
                CHECK(clamped.Records[1]->BitStart == 63 && clamped.Records[1]->BitLength == 1);
                CHECK(clamped.Records[2]->BitStart == 60 && clamped.Records[2]->BitLength == 4);
            }
        }

        // The table as Manifold keeps it after a load (what TableRuntime::LoadTable moves where), written again.
        std::string Resave(Table::LoadedTable& loaded)
        {
            Table::AddressList list;
            list.TableLuaScript = loaded.LuaScript;
            list.TableComment = loaded.Comments;
            list.DisasmNotes = std::move(loaded.DisasmNotes);
            list.Files = std::move(loaded.Files);
            list.ExtraXml = std::move(loaded.ExtraXml);
            list.LoadedVersion = loaded.Version;
            list.LoadedFileNaming = loaded.Naming;
            for (const auto& record : loaded.Records)
                list.NoteId(record->Id);
            for (auto& record : loaded.Records)
                list.Add(std::move(record));
            Table::StructureStore structures;
            for (auto& structure : loaded.Structures)
                structures.Items.push_back(std::move(structure));
            return Table::WriteTable(list, structures, loaded.Symbols);
        }

        // #10: CE's code list (<CheatCodes>) survives a load and a save, and every section the reader takes as its own is written
        // back, so no other known section can vanish the same way.
        void TableSectionsKept()
        {
            {
                // The reporter's table: a code entry, an ordinary record, an unknown extension.
                const std::string input = "<CheatTable>"
                    "<CheatEntries><CheatEntry><ID>1</ID><Description>Control record</Description><GroupHeader>1</GroupHeader></CheatEntry></CheatEntries>"
                    "<CheatCodes><CodeEntry><Description>Saved code</Description><AddressString>example.exe+100</AddressString></CodeEntry></CheatCodes>"
                    "<ExampleExtension><Data>Control extension</Data></ExampleExtension>"
                    "</CheatTable>";
                Table::LoadedTable loaded;
                CHECK(Table::ParseTable(input, loaded).Ok() && loaded.Warnings.empty());
                CHECK_EQ(loaded.CodeListEntries, 1);
                const std::string saved = Resave(loaded);
                for (const char* text : { "Saved code", "example.exe+100", "Control record", "Control extension" })
                    CHECK_MSG(saved.find(text) != std::string::npos, std::string("#10: \"") + text + "\" lost on save");
            }

            // A table with every known section, and a code list as CE writes it: a group header with a colour and an entry with
            // its bytes.
            Table::AddressList list;
            list.AddValue("Health", "game.exe+10", Scan::ValueType::FourBytes);
            list.TableLuaScript = "print('lua')";
            list.TableComment = "table comment";
            list.DisasmNotes["game.exe+20"] = { "note", "" };
            Table::TableFile file;
            file.Name = "data.txt";
            *file.Data = { 'x', 'y' };
            list.Files.push_back(file);
            Table::StructureStore structures;
            structures.Add("Player").Fields.push_back({ 0x10, "Health", Scan::ValueType::FourBytes, 4, Table::FieldDisplay::Signed, false, "" });
            std::string input = Table::WriteTable(list, structures, { { "base", "game.exe+30" } });
            const std::string codes = "<CheatCodes>"
                "<CodeEntry Color=\"000000FF\" GroupHeader=\"1\"><Description>Hooks</Description></CodeEntry>"
                "<CodeEntry><Description>Damage</Description><AddressString>game.exe+1234</AddressString>"
                "<Before><Byte>48</Byte><Byte>8B</Byte></Before><Actual><Byte>29</Byte><Byte>C8</Byte></Actual><After><Byte>89</Byte></After></CodeEntry>"
                "</CheatCodes><Signature>not checked</Signature>";
            const std::size_t end = input.rfind("</CheatTable>");
            CHECK(end != std::string::npos);
            if (end == std::string::npos)
                return;
            input.insert(end, codes);
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(input, loaded).Ok());
            CHECK_EQ(loaded.CodeListEntries, 2);
            const std::string saved = Resave(loaded);
            for (const std::string_view section : Table::KnownTableSections())
            {
                const bool written = saved.find("<" + std::string(section) + ">") != std::string::npos || saved.find("<" + std::string(section) + " ") != std::string::npos;
                // Signature is left out on purpose: CE writes no signature on a plain save either.
                CHECK_MSG(written == (section != "Signature"), "known section " + std::string(section) + (written ? " written" : " lost on save"));
            }
            for (const char* text : { "<CheatCodes>", "GroupHeader=\"1\"", "Color=\"000000FF\"", "<AddressString>game.exe+1234</AddressString>", "<Byte>C8</Byte>", "<After>" })
                CHECK_MSG(saved.find(text) != std::string::npos, std::string("code list: ") + text + " lost on save");
            Table::LoadedTable again;
            CHECK(Table::ParseTable(saved, again).Ok() && again.CodeListEntries == 2);
            CHECK_MSG(Resave(again) == saved, "a second save changes the table");
        }

        void TableDisassemblerComments()
        {
            const std::string input = "<CheatTable><CheatEntries/><DisassemblerComments>"
                "<DisassemblerComment><Address>\"game.exe\"+20</Address><Comment>my note\n</Comment><Header>Combat</Header></DisassemblerComment>"
                "<DisassemblerComment><Address>\"game.exe\"+30</Address><Header>Only header</Header></DisassemblerComment>"
                "</DisassemblerComments></CheatTable>";
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(input, loaded).Ok());
            CHECK_EQ(loaded.DisasmNotes.size(), 2u);
            CHECK(loaded.DisasmNotes.count("\"game.exe\"+20") && loaded.DisasmNotes["\"game.exe\"+20"].Comment == "my note\n" &&
                  loaded.DisasmNotes["\"game.exe\"+20"].Header == "Combat");
            CHECK(loaded.DisasmNotes.count("\"game.exe\"+30") && loaded.DisasmNotes["\"game.exe\"+30"].Comment.empty() &&
                  loaded.DisasmNotes["\"game.exe\"+30"].Header == "Only header");
            const std::string saved = Resave(loaded);
            Xml::ParseResult parsed = Xml::Parse(saved);
            const Xml::Node* section = parsed.Root ? parsed.Root->Child("DisassemblerComments") : nullptr;
            const std::vector<const Xml::Node*> entries = section ? section->ChildrenNamed("DisassemblerComment") : std::vector<const Xml::Node*>();
            CHECK_MSG(entries.size() == 2, "disassembler comments not saved as Cheat Engine reads them: " + saved);
            if (entries.size() == 2)
            {
                CHECK(entries[0]->ChildText("Address") == "\"game.exe\"+20" && entries[0]->ChildText("Comment") == "my note\n" && entries[0]->ChildText("Header") == "Combat");
                CHECK(entries[1]->ChildText("Address") == "\"game.exe\"+30" && !entries[1]->HasChild("Comment") && entries[1]->ChildText("Header") == "Only header");
            }

            Table::LoadedTable older;
            CHECK(Table::ParseTable("<CheatTable><DisassemblerComments><Comment Address=\"game.exe+1234\" Header=\"Old\">old note</Comment></DisassemblerComments></CheatTable>", older).Ok());
            CHECK(older.DisasmNotes.count("game.exe+1234") && older.DisasmNotes["game.exe+1234"].Comment == "old note" && older.DisasmNotes["game.exe+1234"].Header == "Old");
        }

        void TableStructureCustomType()
        {
            Table::StructureStore structures;
            Table::StructDefinition& player = structures.Add("Player");
            player.Fields.push_back({ 0x8, "Purse", Scan::ValueType::Pointer, 8, Table::FieldDisplay::Hex, false, "Purse" });
            player.Fields.push_back({ 0x10, "Gold", Scan::ValueType::ByteArray, 4, Table::FieldDisplay::Unsigned, false, "", "Obscured" });
            structures.Add("Purse").Fields.push_back({ 0x4, "Coins", Scan::ValueType::ByteArray, 4, Table::FieldDisplay::Unsigned, false, "", "Obscured" });
            const auto count = [](const std::string& text, const std::string& wanted)
                {
                    std::size_t found = 0;
                    for (std::size_t at = text.find(wanted); at != std::string::npos; at = text.find(wanted, at + 1))
                        ++found;
                    return found;
                };
            const std::string saved = Table::WriteTable(Table::AddressList(), structures, {});
            CHECK_MSG(count(saved, "Customtype=\"Obscured\"") == 2, "table save without Cheat Engine's Customtype: " + saved);
            const std::string copied = Table::FieldsToXml(structures, player.Fields);
            CHECK_MSG(count(copied, "Customtype=\"Obscured\"") == 2, "copied child structure without Cheat Engine's Customtype: " + copied);
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(saved, loaded).Ok() && loaded.Structures.size() == 2);
            if (loaded.Structures.size() == 2)
                CHECK(loaded.Structures[0]->Fields.size() == 2 && loaded.Structures[0]->Fields[1].CustomType == "Obscured" && loaded.Structures[1]->Fields[0].CustomType == "Obscured");
        }

        void TableStructureElementAttributes()
        {
            const std::string input = "<CheatTable><CheatEntries/><Structures StructVersion=\"2\">"
                "<Structure Name=\"A\"><Elements>"
                "<Element Offset=\"8\" Vartype=\"Pointer\" Bytesize=\"8\" ExpandChangesAddress=\"1\" DisplayMethod=\"unsigned integer\" ChildStructStart=\"16\" BackgroundColor=\"80FF80\" ChildStruct=\"B\"/>"
                "</Elements></Structure>"
                "<Structure Name=\"B\"><Elements><Element Offset=\"16\" Vartype=\"4 Bytes\" Bytesize=\"4\" Description=\"Ammo\" DisplayMethod=\"unsigned integer\"/></Elements></Structure>"
                "</Structures></CheatTable>";
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(input, loaded).Ok());
            const std::string saved = Resave(loaded);
            Xml::ParseResult parsed = Xml::Parse(saved);
            const Xml::Node* structures = parsed.Root ? parsed.Root->Child("Structures") : nullptr;
            const std::vector<const Xml::Node*> list = structures ? structures->ChildrenNamed("Structure") : std::vector<const Xml::Node*>();
            CHECK(list.size() == 2);
            if (list.size() != 2)
                return;
            const Xml::Node* pointer = list[0]->Child("Elements") ? list[0]->Child("Elements")->Child("Element") : nullptr;
            const Xml::Node* ammo = list[1]->Child("Elements") ? list[1]->Child("Elements")->Child("Element") : nullptr;
            CHECK(pointer && ammo);
            if (!pointer || !ammo)
                return;
            CHECK_MSG(pointer->Attribute("ChildStructStart") == std::optional<std::string>("16"), "ChildStructStart lost on save: " + saved);
            CHECK_MSG(pointer->Attribute("BackgroundColor") == std::optional<std::string>("80FF80"), "BackgroundColor lost on save: " + saved);
            CHECK_MSG(pointer->Attribute("ExpandChangesAddress") == std::optional<std::string>("1"), "ExpandChangesAddress lost on save: " + saved);
            CHECK(!ammo->Attribute("ChildStructStart") && !ammo->Attribute("BackgroundColor") && !ammo->Attribute("ExpandChangesAddress"));
        }

        void TableStructureElementRuns()
        {
            const std::string input = "<CheatTable><CheatEntries/><Structures StructVersion=\"2\">"
                "<Structure Name=\"A\"><Elements>"
                "<Element Offset=\"0\" Vartype=\"Pointer\" Bytesize=\"4\" RLECount=\"2\" OffsetHex=\"00000000\" Description=\"Link\" DisplayMethod=\"unsigned integer\"/>"
                "<Element Offset=\"8\" Vartype=\"4 Bytes\" Bytesize=\"4\" OffsetHex=\"00000008\" DisplayMethod=\"unsigned integer\"/>"
                "</Elements></Structure>"
                "<Structure Name=\"Huge\"><Elements>"
                "<Element Offset=\"0\" Vartype=\"Byte\" Bytesize=\"1\" RLECount=\"999999999\" DisplayMethod=\"unsigned integer\"/>"
                "</Elements></Structure>"
                "<Structure Name=\"Edge\"><Elements>"
                "<Element Offset=\"" + std::to_string(Table::kMaxFieldOffset - 4) + "\" Vartype=\"4 Bytes\" Bytesize=\"4\" RLECount=\"5\" DisplayMethod=\"unsigned integer\"/>"
                "</Elements></Structure>"
                "</Structures></CheatTable>";
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(input, loaded).Ok());
            CHECK(loaded.Structures.size() == 3);
            if (loaded.Structures.size() != 3)
                return;
            const std::vector<Table::StructField>& fields = loaded.Structures[0]->Fields;
            CHECK_MSG(fields.size() == 3, "RLECount=2 read as " + std::to_string(fields.size()) + " fields with the element after it, 3 expected");
            if (fields.size() == 3)
            {
                CHECK(fields[0].Offset == 0 && fields[1].Offset == 4 && fields[2].Offset == 8);
                CHECK(fields[1].Type == Scan::ValueType::Pointer && fields[1].Size == 4 && fields[1].Name == "Link");
                CHECK(fields[2].Type == Scan::ValueType::FourBytes && fields[2].Name.empty());
            }
            CHECK_MSG(loaded.Structures[1]->Fields.size() <= 0x10000, "RLECount=999999999 gave " + std::to_string(loaded.Structures[1]->Fields.size()) + " fields");
            const std::vector<Table::StructField>& edge = loaded.Structures[2]->Fields;
            CHECK_MSG(edge.size() == 2, "a run up to kMaxFieldOffset gave " + std::to_string(edge.size()) + " fields, 2 expected");
            for (const Table::StructField& field : edge)
                CHECK(field.Offset <= Table::kMaxFieldOffset);

            const std::string copied = "<Elements>"
                "<Element Offset=\"0\" Vartype=\"Float\" Bytesize=\"4\" RLECount=\"3\" OffsetHex=\"00000000\" Description=\"Axis\" DisplayMethod=\"unsigned integer\"/>"
                "<Element Offset=\"16\" Vartype=\"Pointer\" Bytesize=\"8\" OffsetHex=\"00000010\" DisplayMethod=\"unsigned integer\" ChildStruct=\"Inner\">"
                "<Structure Name=\"Inner\"><Elements>"
                "<Element Offset=\"0\" Vartype=\"4 Bytes\" Bytesize=\"4\" RLECount=\"2\" OffsetHex=\"00000000\" DisplayMethod=\"unsigned integer\"/>"
                "</Elements></Structure></Element>"
                "</Elements>";
            const auto pasted = Table::FieldsFromXml(copied);
            CHECK(pasted.has_value());
            if (!pasted)
                return;
            CHECK_MSG(pasted->Fields.size() == 4, "pasted RLECount=3 gave " + std::to_string(pasted->Fields.size()) + " fields with the element after it, 4 expected");
            if (pasted->Fields.size() == 4)
                CHECK(pasted->Fields[2].Offset == 8 && pasted->Fields[2].Name == "Axis" && pasted->Fields[3].Offset == 16);
            CHECK(pasted->Structures.size() == 1);
            if (pasted->Structures.size() == 1)
                CHECK_MSG(pasted->Structures[0]->Fields.size() == 2, "nested RLECount=2 gave " + std::to_string(pasted->Structures[0]->Fields.size()) + " fields");
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
            int luaRuns = 0;
            host.RunLua = [&luaRuns](std::string_view, bool, std::string& output) -> Status
                {
                    ++luaRuns;
                    output.clear();
                    return Status::Success();
                };
            for (const char* commented : { "// {$lua}\nnop\n", "/* {$lua}\nreturn 1\n{$asm} */\nnop\n", "{ {$lua} }\nnop\n", "db '{$lua}'\n" })
            {
                luaRuns = 0;
                result = Asm::RunAutoAssembler(commented, host, memory, options);
                CHECK_MSG(luaRuns == 0, commented);
            }
            luaRuns = 0;
            result = Asm::RunAutoAssembler("// note\n{$LUA}\nreturn 1\n{$asm}\n/* {$lua} */\n", host, memory, options);
            CHECK_MSG(result.Ok() && luaRuns == 1, result.Error.Text);
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
            result = Asm::RunAutoAssembler("define(,5)\nUseDefine(x)\nnop\n", host, memory, options);
            CHECK(!result.Ok() && result.Error.Code == ErrorCode::AaBadArguments);
            result = Asm::RunAutoAssembler("define( ,5)\nnop\n", host, memory, options);
            CHECK(!result.Ok() && result.Error.Code == ErrorCode::AaBadArguments);
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
            Skip("Assets/Sounds/Success.mp3 not found above " + std::filesystem::current_path().string());
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
        checkWave(Sounds::DecodeToWave(read(file.parent_path() / "Warning.mp3")), "Warning.mp3 decoded");
        checkWave(Sounds::DecodeToWave(read(file.parent_path() / "Error.mp3")), "Error.mp3 decoded");
        CHECK_EQ(std::string(Sounds::FileName(Sounds::Cue::Warning)), "Warning");
        CHECK(Sounds::DecodeToWave({}).empty());
        CHECK(Sounds::DecodeToWave({ 1, 2, 3, 4, 5 }).empty());
    }

    void SerialWork()
    {
        BackgroundWork work;
        const auto post = [&work](BackgroundWork::Job job) { work.Start({ .Title = "Serial job", .Group = "serial", .Cancellable = false, .Quiet = true, .Serial = true }, std::move(job)); };
        int finished = 0;
        std::vector<int> order;
        std::atomic<int> ran{ 0 };
        std::atomic<int> running{ 0 };
        std::atomic<int> overlaps{ 0 };
        std::atomic<std::thread::id> worker{ std::thread::id() };
        for (int i = 0; i < 3; ++i)
        {
            post([&, i](WorkProgress&) -> BackgroundWork::Finish
                {
                    if (++running > 1)
                        ++overlaps;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    worker = std::this_thread::get_id();
                    --running;
                    ++ran;
                    return [&finished, &order, i]()
                        {
                            finished += i + 1;
                            order.push_back(i);
                        };
                });
        }
        for (int wait = 0; wait < 400 && ran < 3; ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK_EQ(ran.load(), 3);
        CHECK_EQ(finished, 0);
        for (int wait = 0; wait < 400 && work.Busy("serial"); ++wait)
        {
            work.Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(!work.Busy("serial"));
        CHECK_EQ(finished, 6);
        CHECK(order == std::vector<int>({ 0, 1, 2 }));
        CHECK_EQ(overlaps.load(), 0);
        CHECK(worker.load() != std::thread::id() && worker.load() != std::this_thread::get_id());
        post([](WorkProgress&) -> BackgroundWork::Finish { throw std::runtime_error("job failed on purpose"); });
        post([&finished](WorkProgress&) -> BackgroundWork::Finish { return [&finished]() { finished = 100; }; });
        for (int wait = 0; wait < 400 && work.Busy("serial"); ++wait)
        {
            work.Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(finished, 100);
        work.Stop();
        post([&finished](WorkProgress&) -> BackgroundWork::Finish { return [&finished]() { finished = -1; }; });
        work.Drain();
        CHECK(!work.Busy("serial"));
        CHECK_EQ(finished, 100);
    }

    std::map<std::string, std::string> SettingLines(const std::string& text)
    {
        std::map<std::string, std::string> lines;
        for (const Ini::Section& section : Ini::Parse(text))
        {
            for (const Ini::Entry& entry : section.Entries)
                lines[Lower(Trim(entry.Key))] = Trim(entry.Value);
        }
        return lines;
    }

    void SettingsTable()
    {
        std::set<std::string> keys;
        for (const SettingField& field : SettingFields())
        {
            const std::string key = Lower(field.Key);
            CHECK_MSG(keys.insert(key).second, std::string("the settings key ") + field.Key + " is used twice");
            CHECK_MSG(key.rfind("color.", 0) != 0 && key.rfind("size.", 0) != 0 && key.rfind("key.", 0) != 0 && key.rfind("recent", 0) == std::string::npos,
                std::string("the settings key ") + field.Key + " collides with a key family");
        }

        const Settings defaults;
        Settings written;
        for (const SettingField& field : SettingFields())
        {
            std::visit([&](auto member)
                {
                    using Member = decltype(member);
                    if constexpr (std::is_same_v<Member, bool Settings::*>)
                        written.*member = !(defaults.*member);
                    else if constexpr (std::is_same_v<Member, int Settings::*> || std::is_same_v<Member, std::uint32_t Settings::*>)
                    {
                        const long long base = static_cast<long long>(defaults.*member);
                        long long next = std::clamp<long long>(base + 7, field.Min, field.Max);
                        if (next == base)
                            next = std::clamp<long long>(base - 7, field.Min, field.Max);
                        written.*member = static_cast<std::remove_reference_t<decltype(written.*member)>>(next);
                    }
                    else if constexpr (std::is_same_v<Member, std::string Settings::*>)
                        written.*member = std::string("probe-") + field.Key;
                    else if constexpr (std::is_same_v<Member, std::vector<std::string> Settings::*>)
                        written.*member = { "first.exe", "second.exe" };
                    else
                        member->Set(written, (member->Get(defaults) + 1) % static_cast<int>(member->Names.size()));
                }, field.Member);
        }
        written.RecentTables = { "C:\\one.CT", "C:\\two.CT" };
        written.LegacyThemeColors["Accent"] = { 0.25f, 0.5f, 0.75f, 1.0f };
        written.LegacyThemeSizes["Rounding"] = 3.5f;
        written.Theme.Custom = "Probe Theme";
        written.Shortcuts["OpenProcess"] = "Ctrl+Shift+P";

        wchar_t temp[MAX_PATH] = L"";
        ::GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path folder = std::filesystem::path(temp) / ("manifold-settings-" + std::to_string(::GetCurrentProcessId()));
        CHECK(written.Save(folder / "written.ini").Ok());
        CHECK(defaults.Save(folder / "defaults.ini").Ok());
        Settings read;
        CHECK(read.Load(folder / "written.ini"));
        CHECK(read.Save(folder / "read.ini").Ok());
        const std::string writtenText = Ini::ReadText(folder / "written.ini").value_or("");
        const std::string readText = Ini::ReadText(folder / "read.ini").value_or("-");
        CHECK_MSG(writtenText == readText, readText);
        const std::map<std::string, std::string> changed = SettingLines(writtenText);
        const std::map<std::string, std::string> plain = SettingLines(Ini::ReadText(folder / "defaults.ini").value_or(""));
        for (const SettingField& field : SettingFields())
        {
            const auto now = changed.find(Lower(field.Key));
            const auto before = plain.find(Lower(field.Key));
            CHECK_MSG(now != changed.end() && (before == plain.end() || before->second != now->second), std::string("the settings key ") + field.Key + " did not change in the file");
        }
        CHECK(read.RecentTables == written.RecentTables && read.LegacyThemeColors == written.LegacyThemeColors && read.LegacyThemeSizes == written.LegacyThemeSizes &&
            read.Theme.Custom == written.Theme.Custom && read.Shortcuts == written.Shortcuts);
        CHECK(plain.count("legacyeditor") == 0 && plain.count("nopsledbest") == 0);
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }

    void Foundations()
    {
        const Result<Address> good(Address(0x1234));
        CHECK(good.Ok() && *good == 0x1234 && good.ToStatus().Ok());
        const Result<Address> bad(Status::Fail(ErrorCode::AllocFailed, "x"));
        CHECK(!bad && bad.ValueOr(7) == 7 && bad.Error().Code == ErrorCode::AllocFailed && bad.ToStatus().Code == ErrorCode::AllocFailed);
        CHECK_EQ(Raw(MakeProcessId(4242)), 4242u);
        CHECK_EQ(ToText(MakeProcessId(17)), std::string("17"));

        UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        CHECK(static_cast<bool>(event));
        UniqueHandle moved(std::move(event));
        CHECK(!event && static_cast<bool>(moved));
        CHECK(!UniqueHandle(INVALID_HANDLE_VALUE));
        moved.Reset();
        CHECK(!moved);

        const std::vector<Ini::Section> sections = Ini::Parse("top=1\r\n; comment\n[One]\nKey = a=b\n# also a comment\n\n[Two]\nx=\n");
        CHECK_EQ(sections.size(), std::size_t(3));
        if (sections.size() == 3)
        {
            CHECK(sections[0].Name.empty() && sections[0].Entries.size() == 1 && sections[0].Entries[0].Value == "1");
            CHECK(sections[1].Name == "One" && sections[1].Entries.size() == 1 && sections[1].Entries[0].Key == "Key " && sections[1].Entries[0].Value == " a=b");
            CHECK(sections[2].Name == "Two" && sections[2].Entries.size() == 1 && sections[2].Entries[0].Value.empty());
        }
        wchar_t folder[MAX_PATH] = L"";
        ::GetTempPathW(MAX_PATH, folder);
        const std::filesystem::path file = std::filesystem::path(folder) / ("manifold-ini-" + std::to_string(::GetCurrentProcessId())) / "test.ini";
        CHECK(Ini::WriteAtomic(file, "[A]\nb=c\n"));
        CHECK(Ini::ReadText(file).value_or("") == "[A]\nb=c\n");
        CHECK(!std::filesystem::exists(std::filesystem::path(file) += L".tmp"));

        Settings settings;
        settings.NopSledBest = 77;
        CHECK(settings.AutoExplorer);
        settings.AutoExplorer = false;
        SettingsStore store(settings, file);
        CHECK(!store.Pending() && !store.Due(100.0));
        store.Changed(10.0);
        CHECK(store.Pending() && !store.Due(10.2) && store.Due(11.0));
        CHECK(store.Flush().Ok());
        CHECK(!store.Pending());
        Settings loaded;
        CHECK(loaded.Load(file) && loaded.NopSledBest == 77 && !loaded.AutoExplorer);
        std::error_code ignored;
        std::filesystem::remove_all(file.parent_path(), ignored);

        NotificationCenter center;
        center.Post(LogLevel::Error, "same", 1.0);
        center.Post(LogLevel::Error, "same", 2.0);
        center.Post(LogLevel::Info, "other", 2.5);
        std::vector<Notice> notices = center.Visible(3.0, 10.0);
        CHECK_EQ(notices.size(), std::size_t(2));
        if (notices.size() == 2)
        {
            CHECK(notices[0].Text == "same" && notices[0].Count == 2 && notices[0].At == 2.0);
            CHECK(notices[1].Text == "other" && notices[1].Count == 1 && notices[1].Id > notices[0].Id);
            center.Expire(notices[1].Id, -100.0);
        }
        CHECK_EQ(center.Visible(3.0, 10.0).size(), std::size_t(1));
        CHECK(center.Visible(30.0, 10.0).empty() && center.Empty());
        for (int i = 0; i < 20; ++i)
            center.Post(LogLevel::Info, "notice " + std::to_string(i), 1.0);
        CHECK(center.Visible(1.0, 10.0).size() <= 6);

        MainThreadQueue queue;
        CHECK(OnMainThread());
        int ran = 0;
        std::thread poster([&queue, &ran]()
            {
                CHECK(!OnMainThread());
                queue.RunAndWait([&ran]() { ran += 1; });
            });
        for (int wait = 0; wait < 400 && ran == 0; ++wait)
        {
            queue.Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        poster.join();
        CHECK_EQ(ran, 1);
        queue.RunAndWait([&ran]() { ran += 10; });
        CHECK_EQ(ran, 11);
        queue.Post([&ran]() { ran += 100; });
        CHECK(!queue.Empty());
        CHECK_EQ(queue.Drain(), std::size_t(1));
        CHECK(queue.Empty() && ran == 111);
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

    std::string NestedElements(int depth)
    {
        std::string text;
        for (int i = 0; i < depth; ++i)
            text += "<a>";
        for (int i = 0; i < depth; ++i)
            text += "</a>";
        return text;
    }

    void HostileXml()
    {
        CHECK(Xml::Parse(NestedElements(Xml::kMaxDepth)).Root != nullptr);
        const Xml::ParseResult tooDeep = Xml::Parse(NestedElements(Xml::kMaxDepth + 1));
        CHECK(!tooDeep.Root && tooDeep.Error.find("nested") != std::string::npos);
        CHECK(!Xml::Parse(NestedElements(200000)).Root);

        const int groups = Table::kMaxRecordDepth + 8;
        std::string nested = "<CheatTable><CheatEntries>";
        for (int i = 0; i < groups; ++i)
            nested += "<CheatEntry><Description>\"g" + std::to_string(i) + "\"</Description><GroupHeader>1</GroupHeader><CheatEntries>";
        for (int i = 0; i < groups; ++i)
            nested += "</CheatEntries></CheatEntry>";
        nested += "</CheatEntries></CheatTable>";
        Table::LoadedTable table;
        CHECK(Table::ParseTable(nested, table).Ok() && table.Records.size() == 1);
        int depth = 0;
        for (const Table::MemoryRecord* at = table.Records.empty() ? nullptr : table.Records[0].get(); at; at = at->Children.empty() ? nullptr : at->Children[0].get())
            ++depth;
        CHECK_EQ(depth, Table::kMaxRecordDepth);
        CHECK_EQ(table.Warnings.size(), 1u);
    }

    void HostileHotkeys()
    {
        CHECK(Table::UsableHotkeyKeys({ VK_F1 }) && Table::UsableHotkeyKeys({ VK_CONTROL, VK_LBUTTON }) && Table::UsableHotkeyKeys({ VK_XBUTTON1 }));
        CHECK(!Table::UsableHotkeyKeys({}) && !Table::UsableHotkeyKeys({ VK_LBUTTON }) && !Table::UsableHotkeyKeys({ VK_RBUTTON, VK_LBUTTON }));
        CHECK(!Table::UsableHotkeyKeys({ 0 }) && !Table::UsableHotkeyKeys({ 255 }) && !Table::UsableHotkeyKeys({ VK_F1, 300 }) && !Table::UsableHotkeyKeys({ -1 }));
        CHECK(!Table::UsableHotkeyKeys({ 16, 17, 18, 65, 66, 67 }));

        // As in Cheat Engine, the longest key set that is down wins: Ctrl+F1
        // does not also fire F1, F1 alone still does, Shift+F1 with nothing
        // longer registered fires with Ctrl held too.
        {
            static bool s_down[256] = {};
            Table::SetKeyDownReader([](int key) { return key >= 0 && key < 256 && s_down[key]; });
            const std::vector<int> f1{ VK_F1 }, ctrlF1{ VK_CONTROL, VK_F1 }, shiftF2{ VK_SHIFT, VK_F2 };
            bool f1Held = false, ctrlF1Held = false, shiftF2Held = false;
            struct Fired { bool F1 = false, CtrlF1 = false, ShiftF2 = false; };
            const auto frame = [&] {
                Table::BeginHotkeyFrame();
                Fired fired;
                fired.F1 = Table::KeysPressed(f1, f1Held);
                fired.CtrlF1 = Table::KeysPressed(ctrlF1, ctrlF1Held);
                fired.ShiftF2 = Table::KeysPressed(shiftF2, shiftF2Held);
                return fired;
            };
            frame(); // every set known
            s_down[VK_CONTROL] = s_down[VK_F1] = true;
            Fired fired = frame();
            CHECK(!fired.F1 && fired.CtrlF1);
            s_down[VK_CONTROL] = false; // F1 still held: no late F1
            fired = frame();
            CHECK(!fired.F1 && !fired.CtrlF1);
            s_down[VK_F1] = false;
            frame();
            s_down[VK_F1] = true;
            fired = frame();
            CHECK(fired.F1 && !fired.CtrlF1);
            s_down[VK_F1] = false;
            frame();
            s_down[VK_CONTROL] = s_down[VK_SHIFT] = s_down[VK_F2] = true;
            fired = frame();
            CHECK(fired.ShiftF2);
            std::fill(std::begin(s_down), std::end(s_down), false);
            // Asked first in a frame, F1 still sees Ctrl+F1 from the frame before.
            frame();
            s_down[VK_CONTROL] = s_down[VK_F1] = true;
            Table::BeginHotkeyFrame();
            CHECK(!Table::KeysPressed(f1, f1Held));
            Table::SetKeyDownReader(nullptr);
        }

        const char* text = R"CT(<CheatTable><CheatEntries>
<CheatEntry><ID>1</ID><Description>"Run"</Description><VariableType>Auto Assembler Script</VariableType><AssemblerScript>[ENABLE]
{$lua}
hostileRan = true
{$asm}
[DISABLE]
</AssemblerScript><Hotkeys>
<Hotkey><Action>Activate</Action><Keys><Key>1</Key></Keys><ID>0</ID></Hotkey>
<Hotkey><Action>Activate</Action><Keys><Key>2</Key><Key>1</Key></Keys><ID>1</ID></Hotkey>
<Hotkey><Action>Activate</Action><Keys><Key>0</Key></Keys><ID>2</ID></Hotkey>
<Hotkey><Action>Activate</Action><Keys><Key>300</Key></Keys><ID>3</ID></Hotkey>
<Hotkey><Action>Activate</Action><Keys><Key>16</Key><Key>17</Key><Key>18</Key><Key>65</Key><Key>66</Key><Key>67</Key></Keys><ID>4</ID></Hotkey>
<Hotkey><Action>Activate</Action><Keys><Key>x</Key><Key>112</Key></Keys><ID>5</ID></Hotkey>
<Hotkey><Action>Toggle Activation</Action><Keys><Key>17</Key><Key>1</Key></Keys><ID>6</ID></Hotkey>
<Hotkey><Action>Toggle Activation</Action><Keys><Key>113</Key></Keys><ID>7</ID></Hotkey>
</Hotkeys></CheatEntry>
<CheatEntry><ID>4000000000</ID><Description>"Health"</Description><VariableType>4 Bytes</VariableType><Address>1000</Address>
<Hotkeys><Hotkey><Action>Set Value</Action><Keys><Key>114</Key></Keys><Value>100</Value><ID>0</ID></Hotkey></Hotkeys></CheatEntry>
<CheatEntry><ID>3</ID><Description>"Group"</Description><GroupHeader>1</GroupHeader><Options moActivateChildrenAsWell="1"/>
<CheatEntries><CheatEntry><ID>5</ID><Description>"Inner"</Description><VariableType>Auto Assembler Script</VariableType><AssemblerScript>[ENABLE]
[DISABLE]
</AssemblerScript></CheatEntry></CheatEntries>
<Hotkeys><Hotkey><Action>Activate</Action><Keys><Key>115</Key></Keys><ID>0</ID></Hotkey></Hotkeys></CheatEntry>
</CheatEntries><LuaScript>hostileTableScript = (hostileTableScript or 0) + 1</LuaScript></CheatTable>
)CT";
        Table::LoadedTable parsed;
        CHECK(Table::ParseTable(text, parsed).Ok() && parsed.Records.size() == 3);
        if (parsed.Records.size() == 3)
        {
            const std::vector<Table::Hotkey>& kept = parsed.Records[0]->Hotkeys;
            CHECK(kept.size() == 2 && kept[0].Id == 6 && kept[1].Id == 7);
            CHECK_EQ(parsed.Warnings.size(), 6u);
            CHECK_EQ(parsed.Records[1]->Id, 0);
            CHECK_EQ(parsed.Records[1]->Hotkeys.size(), 1u);
        }

        const std::filesystem::path path = Paths::DataFolder() / L"hostile-hotkeys.CT";
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }
        Settings settings;
        Engine engine(settings);
        Table::Hotkey toggle;
        toggle.Action = Table::HotkeyAction::Toggle;
        Table::Hotkey activate;
        activate.Action = Table::HotkeyAction::Activate;
        Table::Hotkey setValue;
        setValue.Action = Table::HotkeyAction::SetValue;
        const auto ranTableScript = [&] {
            std::string count;
            engine.Lua().RunWithResult("return tostring(hostileTableScript)", "probe", count);
            return count;
        };
        const auto held = [&](const char* description, const Table::Hotkey& hotkey) {
            const Table::MemoryRecord* record = engine.Table().FindByDescription(description);
            CHECK_MSG(record != nullptr, description);
            return record && engine.Tables().HotkeyHeld(*record, hotkey);
        };

        CHECK(engine.Tables().ScriptsTrusted());
        settings.TableScripts = TableScriptPolicy::Ask;
        CHECK(engine.LoadTable(path, false).Ok());
        CHECK(engine.TableScriptWaiting());
        CHECK(held("Run", toggle) && held("Health", setValue) && held("Health", toggle));
        engine.AnswerTableScript(false);
        CHECK(!engine.TableScriptWaiting() && !engine.Tables().ScriptsTrusted());
        CHECK_EQ(ranTableScript(), "nil");
        CHECK(held("Run", toggle) && held("Run", activate) && held("Group", activate) && held("Inner", toggle));
        CHECK(!held("Health", setValue) && !held("Health", toggle));
        if (const Table::MemoryRecord* health = engine.Table().FindByDescription("Health"))
            engine.Tables().NoteActivationByHand(*health);
        CHECK(held("Run", toggle));
        if (const Table::MemoryRecord* group = engine.Table().FindByDescription("Group"))
            engine.Tables().NoteActivationByHand(*group);
        CHECK(!held("Run", toggle) && !held("Group", activate));

        CHECK(engine.LoadTable(path, false).Ok());
        CHECK(engine.TableScriptWaiting() && held("Run", toggle));
        engine.AnswerTableScript(true);
        CHECK_EQ(ranTableScript(), "1");
        CHECK(engine.Tables().ScriptsTrusted() && !held("Run", toggle));

        settings.TableScripts = TableScriptPolicy::Never;
        CHECK(engine.LoadTable(path, false).Ok());
        CHECK(!engine.TableScriptWaiting() && held("Run", toggle) && !held("Health", setValue));
        CHECK_EQ(ranTableScript(), "1");

        settings.TableScripts = TableScriptPolicy::Always;
        CHECK(engine.LoadTable(path, false).Ok());
        CHECK(!engine.TableScriptWaiting() && !held("Run", toggle));
        CHECK_EQ(ranTableScript(), "2");

        settings.TableScripts = TableScriptPolicy::Never;
        CHECK(engine.LoadTable(path, false).Ok());
        const char* checked = "[ENABLE]\n{$lua}\ncheckRan = (checkRan or 0) + 1\n{$asm}\n[DISABLE]\n";
        const auto checkRan = [&] {
            std::string count;
            engine.Lua().RunWithResult("return tostring(checkRan)", "probe", count);
            return count;
        };
        {
            Asm::AaMemory memory;
            const ScriptCheck held = engine.CheckScript(checked, Asm::AaSection::Enable, memory);
            CHECK_MSG(held.Result.Ok() && held.LuaHeldBack >= 1, held.Result.Error.Text);
            CHECK_EQ(checkRan(), "nil");
            const ScriptCheck broken = engine.CheckScript("[ENABLE]\n{$lua}\nif then\n{$asm}\n[DISABLE]\n", Asm::AaSection::Enable, memory);
            CHECK(!broken.Result.Ok() && broken.LuaHeldBack >= 1);
        }
        engine.NewTable();
        CHECK(engine.Tables().ScriptsTrusted());
        {
            Asm::AaMemory memory;
            const ScriptCheck ran = engine.CheckScript(checked, Asm::AaSection::Enable, memory);
            CHECK_MSG(ran.Result.Ok() && ran.LuaHeldBack == 0, ran.Result.Error.Text);
            CHECK(checkRan() != "nil");
        }

        std::string ran;
        engine.Lua().RunWithResult("return tostring(hostileRan)", "probe", ran);
        CHECK_EQ(ran, "nil");
        std::filesystem::remove(path, ignored);
    }

    void SharedRecordIds()
    {
        const char* text = R"CT(<CheatTable><CheatEntries>
<CheatEntry><ID>3</ID><Description>"Scripts"</Description><GroupHeader>1</GroupHeader><CheatEntries>
  <CheatEntry><ID>0</ID><Description>"Examples"</Description><GroupHeader>1</GroupHeader><CheatEntries>
    <CheatEntry><ID>0</ID><Description>"Example A"</Description><VariableType>4 Bytes</VariableType><Address>1000</Address></CheatEntry>
    <CheatEntry><ID>7</ID><Description>"Example B"</Description><VariableType>4 Bytes</VariableType><Address>1004</Address></CheatEntry>
  </CheatEntries></CheatEntry>
</CheatEntries></CheatEntry>
<CheatEntry><ID>3</ID><Description>"Pointer"</Description><GroupHeader>1</GroupHeader><CheatEntries>
  <CheatEntry><ID>0</ID><Description>"Examples"</Description><GroupHeader>1</GroupHeader><CheatEntries>
    <CheatEntry><ID>7</ID><Description>"Example C"</Description><VariableType>4 Bytes</VariableType><Address>1008</Address></CheatEntry>
  </CheatEntries></CheatEntry>
  <CheatEntry><Description>"No ID"</Description><VariableType>4 Bytes</VariableType><Address>100C</Address></CheatEntry>
</CheatEntries></CheatEntry>
</CheatEntries></CheatTable>)CT";
        Table::LoadedTable loaded;
        CHECK(Table::ParseTable(text, loaded).Ok() && loaded.Records.size() == 2);
        CHECK(std::any_of(loaded.Warnings.begin(), loaded.Warnings.end(), [](const std::string& warning) { return warning.find("the ID of another record") != std::string::npos; }));
        Table::AddressList list;
        for (auto& record : loaded.Records)
            list.Add(std::move(record));
        std::set<int> ids;
        std::size_t records = 0;
        list.ForEach([&](Table::MemoryRecord& record)
            {
                ++records;
                ids.insert(record.Id);
                CHECK_MSG(list.FindById(record.Id) == &record, record.Description + " is not found by its ID " + std::to_string(record.Id));
            });
        CHECK_EQ(records, 8u);
        CHECK_EQ(ids.size(), records);
        CHECK(list.FindById(3) && list.FindById(3)->Description == "Scripts");
        CHECK(list.FindById(7) && list.FindById(7)->Description == "Example B");
        const Table::MemoryRecord* first = list.FindById(0);
        CHECK(first && first->Description == "Examples" && first->Parent && first->Parent->Description == "Scripts");

        Table::MemoryRecord& extra = list.AddGroup("Extra");
        CHECK(extra.Id > 7 && list.FindById(extra.Id) == &extra);
        auto pasted = std::make_unique<Table::MemoryRecord>();
        pasted->Id = 7;
        pasted->Description = "Pasted";
        auto child = std::make_unique<Table::MemoryRecord>();
        child->Id = 3;
        child->Description = "Pasted child";
        pasted->Children.push_back(std::move(child));
        Table::MemoryRecord& added = list.Add(std::move(pasted), &extra);
        CHECK(added.Id != 7 && added.Children.size() == 1 && added.Children[0]->Id != 3);
        CHECK(list.FindById(7)->Description == "Example B" && list.FindById(3)->Description == "Scripts");
    }

    void HostileFiles()
    {
        std::vector<std::uint8_t> plain(3 * 1024 * 1024);
        for (std::size_t i = 0; i < plain.size(); ++i)
            plain[i] = static_cast<std::uint8_t>(i >> 12);
        const std::string encoded = Table::EncodeTableFile(plain);
        CHECK(!encoded.empty() && encoded.size() < plain.size() / 20);
        const auto decoded = Table::DecodeTableFile(encoded);
        CHECK(decoded && *decoded == plain);
        const auto exact = Table::DecodeTableFile(encoded, plain.size());
        CHECK(exact && exact->size() == plain.size());
        CHECK(!Table::DecodeTableFile(encoded, plain.size() - 1));
        CHECK(!Table::DecodeTableFile(encoded, 4096));
        CHECK(!Table::DecodeTableFile(encoded, 0));

        const auto packed = Deflate::Compress(plain);
        CHECK(packed && packed->size() < plain.size() / 20);
        if (packed)
        {
            const auto whole = Deflate::Decompress(*packed);
            CHECK(whole && *whole == plain);
            CHECK(!Deflate::Decompress(*packed, plain.size() - 1));
        }
        const std::uint8_t reservedBlockType[] = { 0x07 };
        CHECK(!Deflate::Decompress(reservedBlockType));
        CHECK(!Deflate::Decompress({}));
        CHECK(!Table::DecodeTableFile(std::string_view(encoded).substr(0, encoded.size() / 2)));

        const std::string small = Table::EncodeTableFile(std::vector<std::uint8_t>{ 1, 2, 3, 4, 5 });
        const std::string twoFiles = "<CheatTable><Files><A Encoding=\"Ascii85\">" + small + "</A><B Encoding=\"Ascii85\">" + encoded + "</B></Files></CheatTable>";
        Table::LoadedTable table;
        CHECK(Table::ParseTable(twoFiles, table).Ok());
        CHECK(table.Files.size() == 2 && table.Files[0].Data->size() == 5 && *table.Files[1].Data == plain);
    }

    void HostileExpressions()
    {
        Settings settings;
        Engine engine(settings);
        const auto wrapped = [](int depth) { return std::string(static_cast<std::size_t>(depth), '(') + "10" + std::string(static_cast<std::size_t>(depth), ')'); };
        CHECK_EQ(engine.Evaluate(wrapped(32)).value_or(0), 0x10u);
        Status error;
        CHECK(!engine.Evaluate(wrapped(200), &error) && error.Code == ErrorCode::ExprSyntax);
        CHECK(!engine.Evaluate(std::string(200000, '(') + "1"));
        CHECK(!engine.Evaluate(std::string(200000, '-') + "1"));
        CHECK(!engine.Evaluate(std::string(200000, '[') + "1"));
        CHECK_EQ(engine.Evaluate("--10").value_or(0), 0x10u);

        Lua::LuaEngine& lua = engine.Lua();
        CHECK(lua.Compile("compileRan = true").Ok());
        CHECK(lua.Compile("local a = 1\nreturn a + 1").Ok());
        const Status escape = lua.Compile("end, (function() compileRan = true end)(), function()");
        CHECK(!escape.Ok() && escape.Code == ErrorCode::LuaSyntax);
        CHECK(!lua.Compile("if then").Ok());
        CHECK(!lua.Compile(std::string_view("\x1bLua\x54\x00", 6)).Ok());
        std::string ran;
        lua.RunWithResult("return tostring(compileRan)", "probe", ran);
        CHECK_EQ(ran, "nil");
    }

    void SessionBackupContent()
    {
        // A table with only table files, a comment or symbols is worth a backup too (it was skipped without records).
        Settings settings;
        settings.BackupFolder = WideToUtf8((Paths::DataFolder() / L"backup-content").wstring());
        Engine engine(settings);
        SessionBackup& backup = engine.Backups();
        Table::AddressList& table = engine.Table();
        bool written = true;
        table.Touch();
        CHECK(table.Dirty() && !Table::TableHasContent(table, engine.Structures(), engine.TableSymbols()));
        CHECK(backup.BackUpNow(&written).Ok() && !written);

        *table.AddFile("notes.txt").Data = { 'h', 'i' };
        CHECK(table.Dirty() && Table::TableHasContent(table, engine.Structures(), engine.TableSymbols()));
        CHECK(backup.BackUpNow(&written).Ok() && written);
        Table::LoadedTable loaded;
        CHECK(Table::LoadTable(backup.LastFile(), loaded).Ok() && loaded.Records.empty() && loaded.Files.size() == 1);
        CHECK(loaded.Files.size() == 1 && loaded.Files[0].Name == "notes.txt" && loaded.Files[0].Data->size() == 2);

        const std::uint64_t identity = table.Identity();
        engine.NewTable();
        CHECK(table.Identity() != identity);
        table.TableComment = "only a comment";
        table.Touch();
        CHECK(backup.BackUpNow(&written).Ok() && written);
        engine.NewTable();
        std::error_code ignored;
        CHECK(!std::filesystem::exists(Backups::MarkerFile(Paths::ConfigFolder(), ::GetCurrentProcessId()), ignored));
    }

    // Ctrl+S on the owner's Isaac table saved every 200-450 ms, each save writing the whole file and two log entries.
    // Merging another table into one with unsaved changes left it marked as saved: no prompt on quit, no session backup.
    void MergeKeepsChanges()
    {
        std::error_code ignored;
        const std::filesystem::path folder = Paths::DataFolder() / L"merge-changes";
        std::filesystem::create_directories(folder, ignored);
        Settings settings;
        Engine engine(settings);
        engine.Table().AddValue("Saved", "game.exe+10", Scan::ValueType::FourBytes);
        CHECK(engine.SaveTable(folder / L"First.CT").Ok());
        engine.NewTable();
        engine.Table().AddValue("Other", "game.exe+20", Scan::ValueType::FourBytes);
        CHECK(engine.SaveTable(folder / L"Second.CT").Ok());
        CHECK(engine.LoadTable(folder / L"First.CT", false).Ok() && !engine.Table().Dirty());
        engine.Table().AddValue("Unsaved", "game.exe+30", Scan::ValueType::FourBytes);
        CHECK(engine.Table().Dirty());
        CHECK(engine.LoadTable(folder / L"Second.CT", true).Ok());
        CHECK_MSG(engine.Table().Dirty(), "the table counts as saved after a merge although neither its edits nor the merged records are in its file");
        CHECK_EQ(engine.Table().Count(), std::size_t{ 3 });
        CHECK(engine.LoadTable(folder / L"Second.CT", false).Ok() && !engine.Table().Dirty());
    }

    void TableSaves()
    {
        std::error_code ignored;
        const std::filesystem::path folder = Paths::DataFolder() / L"table-saves";
        std::filesystem::create_directories(folder, ignored);
        const std::filesystem::path path = folder / L"Saves.CT";
        const std::filesystem::path other = folder / L"Other.CT";
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(other, ignored);
        const auto content = [](const std::filesystem::path& file) {
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const auto entriesSince = [](std::uint64_t serial) {
            std::vector<LogEntry> found;
            for (const LogEntry& entry : RuntimeLog::Entries())
            {
                if (entry.Serial > serial && entry.Message.find("Saves.CT") != std::string::npos)
                    found.push_back(entry);
            }
            return found;
        };
        const auto lastSerial = []() {
            const auto last = RuntimeLog::Last();
            return last ? last->Serial : 0;
        };

        Settings settings;
        Engine engine(settings);
        TableRuntime& tables = engine.Tables();
        // A real write runs in the background (Writing); FinishWrite waits for it like the next frame's Drain would.
        const auto saved = [&](SaveKind kind) {
            tables.FinishWrite();
            return kind == SaveKind::Writing && tables.LastSave() == SaveKind::Written;
        };
        engine.Table().AddValue("Health", "game.exe+10", Scan::ValueType::FourBytes);
        std::vector<std::uint8_t> bytes(64 * 1024);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 5));
        *engine.Table().AddFile("data.bin").Data = bytes;

        // The first save writes, and is one log entry with the time it took.
        std::uint64_t serial = lastSerial();
        CHECK(saved(tables.RequestSave(path, 100.0)));
        CHECK_EQ(tables.TableWrites(), 1u);
        CHECK(!engine.Table().Dirty());
        std::vector<LogEntry> entries = entriesSince(serial);
        CHECK(entries.size() == 1 && entries[0].Area == "Table" && entries[0].Level == LogLevel::Success && entries[0].Message.starts_with("Saved Saves.CT ("));
        const std::string first = content(path);
        CHECK(!first.empty());

        // Presses within the cooldown queue one save; nothing is written or logged until it ends.
        serial = lastSerial();
        CHECK(tables.RequestSave(path, 100.3) == SaveKind::Queued);
        CHECK(tables.RequestSave(path, 100.6) == SaveKind::Queued);
        CHECK(tables.SavePending());
        tables.TickSaves(100.9);
        CHECK(tables.SavePending());
        CHECK(entriesSince(serial).empty());
        // Nothing changed: the trailing save finds the file up to date and does not write it.
        tables.TickSaves(101.0);
        CHECK(!tables.SavePending());
        CHECK(tables.LastSave() == SaveKind::UpToDate);
        CHECK_EQ(tables.TableWrites(), 1u);
        entries = entriesSince(serial);
        CHECK(entries.size() == 1 && entries[0].Level == LogLevel::Info && entries[0].Message.find("up to date") != std::string::npos);

        // After the cooldown an unchanged table is not written either, and a change that was undone counts as unchanged.
        CHECK(tables.RequestSave(path, 105.0) == SaveKind::UpToDate);
        engine.Table().Roots()[0]->Description = "Mana";
        engine.Table().Touch();
        engine.Table().Roots()[0]->Description = "Health";
        CHECK(engine.Table().Dirty());
        CHECK(tables.RequestSave(path, 110.0) == SaveKind::UpToDate);
        CHECK(!engine.Table().Dirty());
        CHECK_EQ(tables.TableWrites(), 1u);
        CHECK_EQ(content(path), first);

        // A real change is always written: right away after the cooldown, at its end within it, with what the table holds then.
        engine.Table().Roots()[0]->Description = "Armor";
        engine.Table().Touch();
        CHECK(saved(tables.RequestSave(path, 120.0)));
        CHECK_EQ(tables.TableWrites(), 2u);
        CHECK(content(path).find("Armor") != std::string::npos);
        engine.Table().AddValue("Ammo", "game.exe+20", Scan::ValueType::FourBytes);
        CHECK(tables.RequestSave(path, 120.2) == SaveKind::Queued);
        engine.Table().AddValue("Gold", "game.exe+30", Scan::ValueType::FourBytes);
        CHECK(engine.Table().Dirty());
        tables.TickSaves(121.0);
        CHECK(tables.SavePending());
        tables.FinishWrite();
        CHECK(tables.LastSave() == SaveKind::Written && !engine.Table().Dirty());
        CHECK_EQ(tables.TableWrites(), 3u);
        CHECK(content(path).find("Ammo") != std::string::npos && content(path).find("Gold") != std::string::npos);

        // A change made while the file is written is not in it, so the table stays dirty; a request meanwhile waits for the write.
        engine.Table().AddValue("Speed", "game.exe+24", Scan::ValueType::FourBytes);
        CHECK(tables.RequestSave(path, 125.0) == SaveKind::Writing);
        engine.Table().AddValue("Luck", "game.exe+28", Scan::ValueType::FourBytes);
        CHECK(tables.RequestSave(path, 126.5) == SaveKind::Queued);
        tables.TickSaves(126.6);
        CHECK(tables.SavePending());
        tables.FinishWrite();
        CHECK(tables.LastSave() == SaveKind::Written && engine.Table().Dirty());
        CHECK(content(path).find("Speed") != std::string::npos && content(path).find("Luck") == std::string::npos);
        tables.TickSaves(126.7);
        tables.FinishWrite();
        CHECK(tables.LastSave() == SaveKind::Written && !engine.Table().Dirty() && !tables.SavePending());
        CHECK(content(path).find("Luck") != std::string::npos);

        // A file changed from outside (another program saved over it) is written again although the table did not change.
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "<CheatTable/>";
        }
        CHECK(saved(tables.RequestSave(path, 130.0)));
        CHECK(content(path).find("Gold") != std::string::npos);
        std::filesystem::remove(path, ignored);
        CHECK(saved(tables.RequestSave(path, 140.0)));
        CHECK(std::filesystem::exists(path, ignored));
        CHECK_EQ(tables.TableWrites(), 7u);

        // A queued save is not lost: quit, a new table and a save to another file run it first.
        engine.Table().AddValue("Keys", "game.exe+40", Scan::ValueType::FourBytes);
        CHECK(saved(engine.RequestSave(path)));
        engine.Table().AddValue("Bombs", "game.exe+50", Scan::ValueType::FourBytes);
        CHECK(engine.RequestSave(path) == SaveKind::Queued);
        engine.FlushPendingSave();
        CHECK(!engine.SavePending() && !engine.Table().Dirty());
        CHECK(content(path).find("Bombs") != std::string::npos);
        engine.Table().AddValue("Coins", "game.exe+60", Scan::ValueType::FourBytes);
        CHECK(engine.RequestSave(path) == SaveKind::Queued);
        CHECK(engine.SaveTable(other).Ok());
        CHECK(!engine.SavePending());
        CHECK(content(path).find("Coins") != std::string::npos && content(other).find("Coins") != std::string::npos);
        CHECK(engine.Table().FilePath == WideToUtf8(other.wstring()));
        engine.Table().AddValue("Hearts", "game.exe+70", Scan::ValueType::FourBytes);
        CHECK(saved(engine.RequestSave(path)));
        engine.Table().AddValue("Souls", "game.exe+80", Scan::ValueType::FourBytes);
        CHECK(engine.RequestSave(path) == SaveKind::Queued);
        engine.NewTable();
        CHECK(!engine.SavePending());
        Table::LoadedTable loaded;
        CHECK(Table::LoadTable(path, loaded).Ok());
        CHECK(std::any_of(loaded.Records.begin(), loaded.Records.end(), [](const auto& record) { return record->Description == "Souls"; }));

        // A save that fails reports it and leaves the table dirty.
        engine.Table().AddValue("Lost", "game.exe+90", Scan::ValueType::FourBytes);
        const Status failed = engine.SaveTable(folder / L"missing folder" / L"No.CT");
        CHECK(!failed.Ok() && failed.Code == ErrorCode::TableNotSaved && engine.Table().Dirty());
        CHECK(tables.LastSave() == SaveKind::Failed);

        // Table files are deflated once while their bytes stay the same, also when Lua writes into them without a version bump.
        Table::ClearEncodeTableFileCache();
        Table::AddressList list;
        list.AddValue("Health", "game.exe+10", Scan::ValueType::FourBytes);
        Table::TableFile& file = list.AddFile("data.bin");
        *file.Data = bytes;
        Table::StructureStore structures;
        const std::string once = Table::WriteTable(list, structures, {});
        const std::string twice = Table::WriteTable(list, structures, {});
        CHECK_EQ(once, twice);
        Table::EncodeCacheStats stats = Table::EncodeTableFileCacheStats();
        CHECK(stats.Misses == 1 && stats.Hits == 1);
        (*file.Data)[100] ^= 0xFF;
        const std::string changed = Table::WriteTable(list, structures, {});
        CHECK(changed != once);
        stats = Table::EncodeTableFileCacheStats();
        CHECK(stats.Misses == 2 && stats.Hits == 1);
        Table::ClearEncodeTableFileCache();
        CHECK_EQ(Table::WriteTable(list, structures, {}), changed);
        Table::LoadedTable reread;
        CHECK(Table::ParseTable(changed, reread).Ok() && reread.Files.size() == 1 && *reread.Files[0].Data == *file.Data);
        std::filesystem::remove_all(folder, ignored);
    }

    void SessionBackups()
    {
        CHECK_EQ(Backups::SafeName("Crab: Champions?"), "Crab_ Champions_");
        CHECK_EQ(Backups::SafeName(".."), "session");
        CHECK_EQ(Backups::SafeName("..\\..\\evil"), ".._.._evil");
        CHECK_EQ(Backups::SafeName("  name. "), "name");
        CHECK_EQ(Backups::SafeName("con"), "_con");
        CHECK_EQ(Backups::SafeName("NUL.txt"), "_NUL.txt");
        CHECK_EQ(Backups::SafeName(""), "session");
        CHECK_EQ(Utf8ToWide(Backups::SafeName(std::string(300, 'a'))).size(), Backups::kMaxNameLength);
        CHECK_EQ(Backups::SessionName("C:\\Tables\\Crab Champions.CT", "game.exe"), "Crab Champions");
        CHECK_EQ(Backups::SessionName("", "CrabChampions-Win64-Shipping.exe"), "CrabChampions-Win64-Shipping");
        CHECK_EQ(Backups::SessionName("", "tool.v2"), "tool.v2");
        CHECK_EQ(Backups::SessionName("", ""), "session");
        CHECK_EQ(Backups::PathKey("C:\\Tables\\Isaac.CT"), Backups::PathKey("c:/tables/./ISAAC.ct"));
        CHECK(Backups::PathKey("C:\\Tables\\Isaac.CT") != Backups::PathKey("D:\\Other\\Isaac.CT"));
        CHECK_EQ(Backups::PathKey("C:\\Tables\\Isaac.CT").size(), 8u);
        CHECK(Backups::FolderName("C:\\Tables\\Isaac.CT", "game.exe") != Backups::FolderName("D:\\Other\\Isaac.CT", "game.exe"));
        CHECK_EQ(Backups::FolderName("C:\\Tables\\Isaac.CT", {}), "Isaac~" + Backups::PathKey("C:\\Tables\\Isaac.CT"));
        CHECK_EQ(Backups::FolderName({}, "isaac-ng.exe"), "isaac-ng");
        CHECK_EQ(Backups::ShownName("My Table~0123abcd"), "My Table");
        CHECK_EQ(Backups::ShownName("My Table"), "My Table");
        CHECK_EQ(Backups::ShownName("a~b"), "a~b");
        CHECK_EQ(Backups::ShownName("x~0123ABCD"), "x~0123ABCD");
        CHECK_EQ(Backups::ShownName("~0123abcd"), "~0123abcd");

        const auto start = std::chrono::system_clock::now();
        CHECK(Backups::IsVersion(Backups::StampName(start) + L".CT"));
        CHECK(Backups::IsVersion(L"2026-11-20_04-15-30.CT") && Backups::IsVersion(L"folder\\2026-11-20_04-15-30_2.ct"));
        CHECK(!Backups::IsVersion(L"2026-11-20_04-15-30.txt") && !Backups::IsVersion(L"2026-11-20 04-15-30.CT") && !Backups::IsVersion(L"table.CT"));
        CHECK(!Backups::IsVersion(L"2026-11-20_04-15-30_.CT") && !Backups::IsVersion(L"2026-11-20_04-15-30_x.CT") && !Backups::IsVersion(L"2026-11-20_04-15-30_12345.CT"));
        CHECK_EQ(Backups::When(L"2026-11-20_04-15-30_2.CT"), "2026-11-20 04:15:30");
        CHECK_EQ(Backups::When(L"table.CT"), "");

        Settings plain;
        CHECK(Backups::Folder(plain) == Paths::BackupFolder());
        plain.BackupFolder = "my backups";
        CHECK(Backups::Folder(plain) == Paths::DataFolder() / L"my backups");

        const auto put = [](const std::filesystem::path& file, std::string_view text) {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        };
        const auto content = [](const std::filesystem::path& file) {
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        std::error_code ignored;

        const std::filesystem::path folder = Paths::DataFolder() / L"backup-versions" / L"session";
        for (int i = 0; i < 6; ++i)
            CHECK(!Backups::Write(folder, "v" + std::to_string(i), start + std::chrono::seconds(i)).empty());
        std::filesystem::path last;
        for (int i = 0; i < 11; ++i)
            last = Backups::Write(folder, "again" + std::to_string(i), start + std::chrono::seconds(5));
        CHECK(last.stem().wstring().ends_with(L"_12"));
        put(folder / L"notes.CT", "mine");
        put(folder / L"2026-11-20_04-15-30.txt", "mine too");
        std::vector<std::filesystem::path> versions = Backups::Versions(folder);
        CHECK_EQ(versions.size(), 17u);
        CHECK(!versions.empty() && versions.front() == last && content(versions.back()) == "v0");
        CHECK(versions.size() == 17 && content(versions[1]) == "again9" && content(versions[11]) == "v5");
        CHECK_EQ(Backups::Prune(folder, 3), 14);
        versions = Backups::Versions(folder);
        CHECK(versions.size() == 3 && content(versions[0]) == "again10" && content(versions[2]) == "again8");
        last = Backups::Write(folder, "after", start + std::chrono::seconds(5));
        CHECK(last.stem().wstring().ends_with(L"_13"));
        const std::filesystem::path early = Backups::Write(folder, "clock set back", start - std::chrono::hours(48));
        CHECK_EQ(Backups::Prune(folder, 2, early), 3);
        versions = Backups::Versions(folder);
        CHECK(versions.size() == 2 && versions[0] == last && versions[1] == early);
        CHECK_EQ(content(folder / L"notes.CT"), "mine");
        CHECK_EQ(content(folder / L"2026-11-20_04-15-30.txt"), "mine too");
        CHECK(Backups::Versions(Paths::DataFolder() / L"backup-missing").empty());
        CHECK_EQ(Backups::Prune(Paths::DataFolder() / L"backup-missing", 3), 0);

        const unsigned long own = ::GetCurrentProcessId();
        const std::filesystem::path marker = Backups::MarkerFile(Paths::ConfigFolder(), own);
        const auto add = [](Engine& engine, const char* name) { engine.Table().AddValue(name, "game.exe+10", Scan::ValueType::FourBytes); };
        {
            Settings settings;
            settings.BackupFolder = WideToUtf8((Paths::DataFolder() / L"backup-engine").wstring());
            Engine engine(settings);
            SessionBackup& backup = engine.Backups();
            bool written = true;
            CHECK(backup.BackUpNow(&written).Ok() && !written);
            CHECK_EQ(backup.Name(), "session");

            add(engine, "Health");
            CHECK(engine.Table().Dirty());
            CHECK(backup.BackUpNow(&written).Ok() && written);
            const std::filesystem::path first = backup.LastFile();
            CHECK(Backups::IsVersion(first) && first.parent_path() == Paths::DataFolder() / L"backup-engine" / L"session");
            Table::LoadedTable loaded;
            CHECK(Table::LoadTable(first, loaded).Ok() && loaded.Records.size() == 1);
            CHECK(std::filesystem::exists(marker, ignored));
            CHECK(backup.BackUpNow(&written).Ok() && !written);
            CHECK(backup.LastFile() == first);

            settings.BackupIntervalMinutes = 1;
            add(engine, "Mana");
            backup.Tick(1000.0);
            backup.Tick(1030.0);
            CHECK(backup.LastFile() == first);
            backup.Tick(1061.0);
            const std::filesystem::path second = backup.LastFile();
            CHECK(second != first);
            backup.Tick(1200.0);
            CHECK(backup.LastFile() == second);
            settings.BackupsEnabled = false;
            add(engine, "Gold");
            backup.Tick(2000.0);
            backup.Tick(3000.0);
            CHECK(backup.LastFile() == second);
            settings.BackupsEnabled = true;
            backup.Tick(3001.0);
            CHECK(backup.LastFile() == second);
            backup.Tick(3062.0);
            CHECK(backup.LastFile() != second);

            const std::filesystem::path saved = Paths::DataFolder() / L"My Table.CT";
            CHECK(engine.SaveTable(saved).Ok());
            CHECK(std::filesystem::exists(marker, ignored));
            backup.Tick(3063.0);
            CHECK(!std::filesystem::exists(marker, ignored));
            CHECK(backup.BackUpNow(&written).Ok() && !written);
            CHECK_EQ(backup.Name(), "My Table");
            add(engine, "Ammo");
            CHECK(backup.BackUpNow(&written).Ok() && written);
            const std::filesystem::path named = backup.LastFile();
            // A saved table's folder carries a hash of its path, so a same-named table elsewhere keeps its own versions.
            const std::string savedPath = WideToUtf8(saved.wstring());
            CHECK(named.parent_path().filename() == Utf8ToWide(Backups::FolderName(savedPath, {})));
            CHECK(named.parent_path() == backup.SessionFolder() && named.parent_path().filename().wstring().starts_with(L"My Table~"));
            CHECK(backup.LegacyFolder() == Paths::DataFolder() / L"backup-engine" / L"My Table");
            {
                const std::filesystem::path legacy = Backups::Write(backup.LegacyFolder(), "<CheatTable/>", start - std::chrono::hours(1));
                const std::vector<std::filesystem::path> listed = backup.Versions();
                CHECK(listed.size() == 2 && listed[0] == named && listed[1] == legacy);
                CHECK_EQ(Backups::Prune(backup.SessionFolder(), 1, named), 0);
                CHECK_MSG(std::filesystem::exists(legacy, ignored), "pruning the new folder touched the old one");
                std::filesystem::remove(legacy, ignored);
            }

            const std::size_t records = engine.Table().Count();
            engine.NewTable();
            CHECK(!std::filesystem::exists(marker, ignored));
            const std::vector<std::string> recent = settings.RecentTables;
            CHECK(engine.RestoreTable(named).Ok());
            CHECK_EQ(engine.Table().Count(), records);
            CHECK(engine.Table().FilePath.empty() && engine.Table().Dirty());
            CHECK(settings.RecentTables == recent);
            CHECK_EQ(backup.RestoredName(), "My Table");
            CHECK_EQ(backup.Name(), "My Table");
            CHECK(backup.BackUpNow(&written).Ok() && !written);

            settings.BackupVersions = 2;
            for (const char* name : { "One", "Two", "Three" })
            {
                add(engine, name);
                CHECK(backup.BackUpNow(&written).Ok() && written);
            }
            CHECK_EQ(Backups::Versions(backup.SessionFolder()).size(), 2u);
            CHECK(std::filesystem::exists(marker, ignored));
            add(engine, "Last");
            backup.Finish();
            CHECK(!std::filesystem::exists(marker, ignored));
            CHECK(Table::LoadTable(backup.LastFile(), loaded).Ok());
            CHECK_EQ(Backups::Versions(backup.SessionFolder()).size(), 2u);

            settings.BackupFolder = WideToUtf8((saved / L"below a file").wstring());
            add(engine, "Nowhere");
            const Status failed = backup.BackUpNow(&written);
            CHECK(!failed.Ok() && failed.Code == ErrorCode::BackupNotSaved && !written);
        }

        const std::filesystem::path markers = Paths::DataFolder() / L"backup-markers";
        std::filesystem::create_directories(markers, ignored);
        const std::filesystem::path lost = Backups::Write(markers / L"Crab", "<CheatTable/>", start);
        const std::filesystem::path older = Backups::Write(markers / L"Isaac", "<CheatTable/>", start - std::chrono::hours(48));
        const unsigned long running = 4000000000ul;
        const HANDLE instance = ::CreateMutexW(nullptr, FALSE, Backups::InstanceMutexName(running).c_str());
        put(Backups::MarkerFile(markers, 4), WideToUtf8(older.wstring()) + "\nIsaac\n");
        put(Backups::MarkerFile(markers, 8), WideToUtf8(lost.wstring()) + "\r\nCrab\r\n");
        put(Backups::MarkerFile(markers, 12), WideToUtf8((markers / L"Crab" / L"2020-01-01_00-00-00.CT").wstring()) + "\nGone\n");
        put(Backups::MarkerFile(markers, 16), WideToUtf8((folder / L"notes.CT").wstring()) + "\nNotes\n");
        put(Backups::MarkerFile(markers, running), WideToUtf8(lost.wstring()) + "\nRunning\n");
        put(Backups::MarkerFile(markers, own), WideToUtf8(lost.wstring()) + "\nOwn\n");
        put(markers / L"backup-x.open", "not a marker");
        std::vector<Backups::Abandoned> abandoned = Backups::TakeAbandoned(markers, own);
        CHECK_EQ(abandoned.size(), 2u);
        CHECK(abandoned.size() == 2 && abandoned[0].File == lost && abandoned[0].Name == "Crab" && abandoned[1].File == older && abandoned[1].Name == "Isaac");
        for (unsigned long gone : { 4ul, 8ul, 12ul, 16ul })
            CHECK(!std::filesystem::exists(Backups::MarkerFile(markers, gone), ignored));
        CHECK(std::filesystem::exists(Backups::MarkerFile(markers, running), ignored) && std::filesystem::exists(Backups::MarkerFile(markers, own), ignored));
        CHECK(std::filesystem::exists(markers / L"backup-x.open", ignored) && std::filesystem::exists(lost, ignored));
        if (instance)
            ::CloseHandle(instance);
        abandoned = Backups::TakeAbandoned(markers, own);
        CHECK(abandoned.size() == 1 && abandoned[0].Name == "Running");
        CHECK(Backups::TakeAbandoned(Paths::DataFolder() / L"backup-missing", own).empty());
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

    // About > Credits: the five first entries in the owner's order, and no entry without a name or a reason
    void CreditsList()
    {
        const std::span<const CreditEntry> credits = Credits();
        const char* expected[][2] = { { "Leunsel", "Author" }, { "cfemen", "SysCalls" }, { "Veydr", "Bug-Hunts" }, { "ocornut", "ImGui" }, { "zyantific", "Zydis" } };
        CHECK(credits.size() >= std::size(expected));
        for (std::size_t i = 0; i < std::size(expected) && i < credits.size(); ++i)
        {
            CHECK_EQ(credits[i].Name, std::string_view(expected[i][0]));
            CHECK_EQ(credits[i].For, std::string_view(expected[i][1]));
        }
        for (const CreditEntry& entry : credits)
            CHECK_MSG(!entry.Name.empty() && !entry.For.empty(), std::string(entry.Name));
    }

    int RunCoreTests()
    {
        const int before = Totals().Failures;
        ValueTypes();
        TypeCategories();
        TextHelpers();
        Errors();
        LogEntries();
        XmlParsing();
        TableRoundTrip();
        TableSectionsKept();
        TableDisassemblerComments();
        TableStructureCustomType();
        TableStructureElementAttributes();
        TableStructureElementRuns();
        AutoAssemblerOffline();
        CodeSearchHelpers();
        ScanKinds();
        Conditions();
        BreakExpressions();
        SerialWork();
        Foundations();
        SettingsTable();
        SoundDecoding();
        FolderMigration();
        RecordIndex();
        ProfilerZones();
        HostileXml();
        SharedRecordIds();
        HostileHotkeys();
        HostileFiles();
        HostileExpressions();
        SessionBackups();
        SessionBackupContent();
        TableSaves();
        MergeKeepsChanges();
        DiagnosticsInfo();
        CreditsList();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("core", "Core", 10, [](const SuiteContext&) { return RunCoreTests(); });
}
