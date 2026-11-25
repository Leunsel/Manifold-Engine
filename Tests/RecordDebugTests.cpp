#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Core/Types.h"
#include "Lua/LuaEngine.h"
#include "Table/CheatTable.h"
#include "Table/RecordCheck.h"
#include "Table/RecordPath.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        using Table::IssueLevel;
        using Table::RecordIssue;

        std::vector<const RecordIssue*> Of(const std::vector<RecordIssue>& issues, int id, std::string_view check = {})
        {
            std::vector<const RecordIssue*> found;
            for (const RecordIssue& issue : issues)
            {
                if (issue.RecordId == id && (check.empty() || issue.Check == check))
                    found.push_back(&issue);
            }
            return found;
        }

        bool Has(const std::vector<RecordIssue>& issues, int id, std::string_view check, IssueLevel level, std::string_view part = {})
        {
            for (const RecordIssue* issue : Of(issues, id, check))
            {
                if (issue->Level == level && (part.empty() || issue->Text.find(part) != std::string::npos))
                    return true;
            }
            return false;
        }

        void References()
        {
            const std::string code =
                "local al = getAddressList()\n"
                "local a = al.getMemoryRecordByDescription(\"Health\")\n"
                "local b = AddressList.getMemoryRecordByID(12)\n"
                "local c = manifold.record.find('Ammo')\n"
                "local d = getmemoryrecordbydescription [[Long Name]]\n"
                "-- getMemoryRecordByDescription(\"Commented\")\n"
                "local e = al.getMemoryRecordByDescription(\"Built\" .. suffix)\n"
                "local f = al.getMemoryRecordByDescription(name)\n"
                "local g = mygetMemoryRecordByID(3)\n"
                "local h = al:getMemoryRecordByID(0x10)\n"
                "local i = manifold.record.find(7)\n"
                "local j = al.getMemoryRecordByDescription \"Sugar\"\n"
                "local k = notmanifold.record.find('No')\n";
            const std::vector<Table::RecordReference> found = Table::FindRecordReferencesIn(code);
            std::vector<std::string> targets;
            for (const Table::RecordReference& reference : found)
                targets.push_back(reference.Target + (reference.ById ? "#" : "") + "@" + std::to_string(reference.Line));
            const std::vector<std::string> expected = { "Health@2", "12#@3", "Ammo@4", "Long Name@5", "16#@10", "7#@11", "Sugar@12" };
            CHECK_MSG(targets == expected, Join(targets, ", "));
            CHECK(!found.empty() && found.front().Function == "getMemoryRecordByDescription");
        }

        void StaticChecks()
        {
            Settings settings;
            Engine engine(settings);
            Table::AddressList& list = engine.Table();

            Table::MemoryRecord& modes = list.AddValue("Modes", "00400000", Scan::ValueType::FourBytes);
            modes.DropDown = { { "0", "Off" }, { "1", "On" } };
            Table::MemoryRecord& good = list.AddValue("Good Link", "00400004", Scan::ValueType::FourBytes);
            good.DropDownLink = "\"Modes\"";
            Table::MemoryRecord& missing = list.AddValue("Missing Link", "00400008", Scan::ValueType::FourBytes);
            missing.DropDownLink = "Nowhere";
            Table::MemoryRecord& self = list.AddValue("Self Link", "0040000C", Scan::ValueType::FourBytes);
            self.DropDownLink = "Self Link";
            Table::MemoryRecord& empty = list.AddValue("Empty List", "00400010", Scan::ValueType::FourBytes);
            Table::MemoryRecord& toEmpty = list.AddValue("Links to Empty", "00400014", Scan::ValueType::FourBytes);
            toEmpty.DropDownLink = "Empty List";
            list.AddValue("Twin", "00400018", Scan::ValueType::FourBytes);
            list.AddValue("Twin", "0040001C", Scan::ValueType::FourBytes);
            Table::MemoryRecord& toTwin = list.AddValue("Links to Twin", "00400020", Scan::ValueType::FourBytes);
            toTwin.DropDownLink = "Twin";
            Table::MemoryRecord& chained = list.AddValue("Chained", "00400024", Scan::ValueType::FourBytes);
            chained.DropDownLink = "Good Link";

            Table::MemoryRecord& noSections = list.AddScript("No Sections", "nop\n");
            Table::MemoryRecord& onlyEnable = list.AddScript("Only Enable", "[ENABLE]\nnop\n");
            Table::MemoryRecord& fine = list.AddScript("Fine Script", "[ENABLE]\nnop\n[DISABLE]\nnop\n");
            Table::MemoryRecord& emptyScript = list.AddScript("Empty Script", "  \n");

            Table::MemoryRecord& orphan = list.AddValue("Orphan", "+10", Scan::ValueType::FourBytes);
            Table::MemoryRecord& relativeToScript = list.AddValue("Under Script", "+4", Scan::ValueType::FourBytes, &fine);
            Table::MemoryRecord& header = list.AddGroup("Header");
            header.AddressGroupHeader = true;
            header.Address = "00500000";
            Table::MemoryRecord& relative = list.AddValue("Under Header", "+8", Scan::ValueType::FourBytes, &header);
            Table::MemoryRecord& pointer = list.AddValue("Pointer", "00600000", Scan::ValueType::FourBytes);
            pointer.Offsets = { "10", "" };
            Table::MemoryRecord& noAddress = list.AddValue("No Address", "", Scan::ValueType::FourBytes);
            Table::MemoryRecord& custom = list.AddValue("Custom", "00400028", Scan::ValueType::FourBytes);
            custom.CustomTypeName = "NoSuchType";
            Table::MemoryRecord& leaf = list.AddValue("Leaf", "0040002C", Scan::ValueType::FourBytes);
            leaf.Options.ActivateChildren = true;
            Table::MemoryRecord& unnamed = list.AddValue("", "00400030", Scan::ValueType::FourBytes);

            Table::Hotkey setNothing;
            setNothing.Keys = { VK_F5 };
            setNothing.Action = Table::HotkeyAction::SetValue;
            Table::Hotkey toggle;
            toggle.Keys = { VK_F6 };
            Table::Hotkey mouse;
            mouse.Keys = { VK_LBUTTON };
            Table::Hotkey modifier;
            modifier.Keys = { VK_CONTROL };
            good.Hotkeys = { setNothing, toggle, toggle, mouse };
            leaf.Hotkeys = { modifier };
            modes.Hotkeys = { toggle };

            list.TableLuaScript = "local x = getAddressList().getMemoryRecordByDescription('Gone')\nlocal y = getAddressList().getMemoryRecordByDescription('Twin')\n";
            fine.LuaOnActivate = "manifold.record.find('Modes')";

            Table::CheckHooks hooks;
            hooks.CustomTypeExists = [](std::string_view name) { return name == "Known"; };
            const std::vector<RecordIssue> issues = Table::CheckRecords(list, hooks);

            CHECK(Of(issues, modes.Id, "dropdown-link").empty());
            CHECK(Of(issues, good.Id, "dropdown-link").empty());
            CHECK(Has(issues, missing.Id, "dropdown-link", IssueLevel::Error, "Nowhere"));
            CHECK(Has(issues, self.Id, "dropdown-link", IssueLevel::Warning, "itself"));
            CHECK(Of(issues, empty.Id).empty());
            CHECK(Has(issues, toEmpty.Id, "dropdown-link", IssueLevel::Warning, "no dropdown entries"));
            CHECK(Has(issues, toTwin.Id, "dropdown-link", IssueLevel::Warning, "2 records"));
            CHECK(Has(issues, chained.Id, "dropdown-link", IssueLevel::Warning, "not followed"));

            CHECK(Has(issues, noSections.Id, "script", IssueLevel::Error, "[ENABLE]"));
            CHECK(Has(issues, onlyEnable.Id, "script", IssueLevel::Warning, "[DISABLE]"));
            CHECK(Of(issues, fine.Id, "script").empty());
            CHECK(Has(issues, emptyScript.Id, "script", IssueLevel::Error, "empty"));

            CHECK(Has(issues, orphan.Id, "address", IssueLevel::Error, "no parent"));
            CHECK(Has(issues, relativeToScript.Id, "address", IssueLevel::Error, "none of them has an address"));
            CHECK(Of(issues, relative.Id, "address").empty());
            CHECK(Of(issues, header.Id, "address").empty());
            CHECK(Has(issues, pointer.Id, "address", IssueLevel::Info, "Offset 2"));
            CHECK(Has(issues, noAddress.Id, "address", IssueLevel::Error, "No address"));
            CHECK(Has(issues, custom.Id, "type", IssueLevel::Error, "NoSuchType"));
            CHECK(Has(issues, leaf.Id, "options", IssueLevel::Info, "activate children"));
            CHECK(Has(issues, unnamed.Id, "description", IssueLevel::Info));

            CHECK(Has(issues, good.Id, "hotkey", IssueLevel::Warning, "no value"));
            CHECK(Has(issues, good.Id, "hotkey", IssueLevel::Warning, "Two hotkeys"));
            CHECK(Has(issues, good.Id, "hotkey", IssueLevel::Warning, "can't be used"));
            CHECK(Of(issues, leaf.Id, "hotkey").empty());
            CHECK(Has(issues, good.Id, "hotkey", IssueLevel::Info, "other records"));
            CHECK(Has(issues, modes.Id, "hotkey", IssueLevel::Info, "other records"));

            CHECK(Has(issues, Table::kTableLevel, "lua-reference", IssueLevel::Warning, "\"Gone\""));
            CHECK(Has(issues, Table::kTableLevel, "lua-reference", IssueLevel::Warning, "2 records"));
            CHECK(Of(issues, fine.Id, "lua-reference").empty());
            CHECK_MSG(!issues.empty() && issues.front().RecordId == Table::kTableLevel, "table-level issues come first");

            const std::vector<Table::MemoryRecord*> linking = Table::RecordsLinkingTo(list, modes);
            CHECK(linking.size() == 1 && linking.front() == &good);
            CHECK(Table::CountWithDescription(list, "Twin") == 2);

            std::size_t fineCount = 0;
            for (const Table::RecordReference& reference : Table::FindRecordReferences(list))
            {
                if (reference.FromRecord == fine.Id)
                {
                    ++fineCount;
                    CHECK(reference.TargetId == modes.Id && reference.Where == "On Activate");
                }
            }
            CHECK(fineCount == 1);
        }

        void Renumbered()
        {
            const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<CheatTable CheatEngineTableVersion="45">
  <CheatEntries>
    <CheatEntry><ID>5</ID><Description>"First"</Description><VariableType>4 Bytes</VariableType><Address>00400000</Address></CheatEntry>
    <CheatEntry><ID>5</ID><Description>"Second"</Description><VariableType>4 Bytes</VariableType><Address>00400004</Address></CheatEntry>
  </CheatEntries>
  <LuaScript>local r = getAddressList().getMemoryRecordByID(5)
local s = getAddressList().getMemoryRecordByID(99)
</LuaScript>
</CheatTable>)";
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(xml, loaded).Ok());
            Table::AddressList list;
            for (auto& record : loaded.Records)
                list.Add(std::move(record));
            list.TableLuaScript = loaded.LuaScript;
            Table::MemoryRecord* first = list.FindByDescription("First");
            Table::MemoryRecord* second = list.FindByDescription("Second");
            CHECK(first && second && first->Id == 5 && first->FileId == 5 && second->FileId == 5 && second->Id != 5);
            if (!first || !second)
                return;
            const std::vector<RecordIssue> issues = Table::CheckRecords(list);
            CHECK(Of(issues, first->Id, "id").empty());
            CHECK(Has(issues, second->Id, "id", IssueLevel::Warning, "\"First\""));
            CHECK(Has(issues, Table::kTableLevel, "lua-reference", IssueLevel::Warning, "\"Second\""));
            CHECK(Has(issues, Table::kTableLevel, "lua-reference", IssueLevel::Warning, "ID 99"));
            auto copy = first->Clone([&]() { return list.NextId(); });
            CHECK(copy->FileId == -1);
        }

        void LoadKeepsFileIds()
        {
            const char* xml = R"(<?xml version="1.0" encoding="utf-8"?>
<CheatTable CheatEngineTableVersion="45">
  <CheatEntries>
    <CheatEntry><ID>0</ID><Description>"Player"</Description><GroupHeader>1</GroupHeader>
      <CheatEntries>
        <CheatEntry><ID>1</ID><Description>"Health"</Description><VariableType>4 Bytes</VariableType><Address>00400000</Address></CheatEntry>
        <CheatEntry><ID>2</ID><Description>"Mode"</Description><VariableType>4 Bytes</VariableType><Address>00400004</Address></CheatEntry>
        <CheatEntry><ID>2</ID><Description>"Difficulty"</Description><VariableType>4 Bytes</VariableType><Address>00400008</Address></CheatEntry>
      </CheatEntries>
    </CheatEntry>
    <CheatEntry><ID>5</ID><Description>"Script A"</Description><VariableType>Auto Assembler Script</VariableType><AssemblerScript>[ENABLE]
[DISABLE]
</AssemblerScript></CheatEntry>
    <CheatEntry><ID>6</ID><Description>"Script B"</Description><VariableType>Auto Assembler Script</VariableType><AssemblerScript>[ENABLE]
[DISABLE]
</AssemblerScript></CheatEntry>
  </CheatEntries>
</CheatTable>)";
            const std::filesystem::path file = std::filesystem::temp_directory_path() / ("manifold-ids-" + std::to_string(::GetCurrentProcessId()) + ".CT");
            {
                std::ofstream out(file, std::ios::binary);
                out << xml;
            }
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            const Status loaded = engine.Tables().LoadTable(file, false);
            std::error_code ignored;
            std::filesystem::remove(file, ignored);
            CHECK_MSG(loaded.Ok(), loaded.Text);
            Table::AddressList& list = engine.Table();
            const auto idOf = [&](const char* name) { const Table::MemoryRecord* record = list.FindByDescription(name); return record ? record->Id : -100; };
            CHECK_EQ(idOf("Player"), 0);
            CHECK_EQ(idOf("Health"), 1);
            CHECK_EQ(idOf("Mode"), 2);
            CHECK_EQ(idOf("Script A"), 5);
            CHECK_EQ(idOf("Script B"), 6);
            CHECK_MSG(idOf("Difficulty") == 7, "the duplicate got " + std::to_string(idOf("Difficulty")));
            std::string result;
            const Status found = engine.Lua().RunWithResult("return getAddressList().getMemoryRecordByID(0).Description", "id 0", result);
            CHECK_MSG(found.Ok() && result == "Player", found.Text + result);

            std::size_t renumbered = 0;
            for (const RecordIssue& issue : Table::CheckRecords(list))
                renumbered += issue.Check == "id";
            CHECK_EQ(renumbered, std::size_t(1));

            Table::MemoryRecord& made = list.AddValue("Made in Code", "00400010", Scan::ValueType::FourBytes);
            CHECK(made.Id > 7);

            const std::string copied = Table::RecordsToXml({ list.FindByDescription("Health") });
            std::vector<std::unique_ptr<Table::MemoryRecord>> pasted = Table::RecordsFromXml(copied);
            CHECK(pasted.size() == 1 && pasted.front()->FileId == -1);
        }

        void Paths()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            Table::AddressList& list = engine.Table();

            // The shape of the owner's tables: every script has its own "Multiplier:" and "[Debug]" below it.
            Table::MemoryRecord& scripts = list.AddGroup("[CET : Scripts] ()->");
            Table::MemoryRecord& fire = list.AddScript("Fire Rate : Multiplier ()->", "[ENABLE]\n[DISABLE]\n", &scripts);
            Table::MemoryRecord& fireMultiplier = list.AddValue("Multiplier:", "00400000", Scan::ValueType::Float, &fire);
            Table::MemoryRecord& fireDebug = list.AddGroup("[Debug] ()->", &fire);
            Table::MemoryRecord& fireHealth = list.AddValue("[0020] Health", "00400010", Scan::ValueType::FourBytes, &fireDebug);
            Table::MemoryRecord& reload = list.AddScript("Reload Speed : Multiplier ()->", "[ENABLE]\n[DISABLE]\n", &scripts);
            Table::MemoryRecord& reloadMultiplier = list.AddValue("Multiplier:", "00400020", Scan::ValueType::Float, &reload);
            Table::MemoryRecord& reloadType = list.AddGroup("Type:", &reload);
            Table::MemoryRecord& typeMultiplier = list.AddValue("Multiplier:", "00400030", Scan::ValueType::Float, &reloadType);
            Table::MemoryRecord& lists = list.AddGroup("[CET : Lists] ()->");
            Table::MemoryRecord& twinA = list.AddGroup("listECrabIslandType", &lists);
            Table::MemoryRecord& twinB = list.AddGroup("listECrabIslandType", &lists);
            Table::MemoryRecord& twinChild = list.AddValue("Value", "00400040", Scan::ValueType::FourBytes, &twinA);
            list.AddValue("Value", "00400044", Scan::ValueType::FourBytes, &twinB);

            CHECK((Table::RecordPath(fireHealth) == std::vector<std::string>{ "[CET : Scripts] ()->", "Fire Rate : Multiplier ()->", "[Debug] ()->", "[0020] Health" }));
            CHECK(Table::FindByPath(list, Table::RecordPath(fireMultiplier)) == std::vector<Table::MemoryRecord*>{ &fireMultiplier });
            CHECK(Table::FindByPath(list, Table::RecordPath(twinChild)).size() == 2);
            CHECK(Table::FindByPath(list, { "Multiplier:" }, &reload) == std::vector<Table::MemoryRecord*>{ &reloadMultiplier });
            CHECK(Table::FindByPath(list, { "Nope" }).empty() && Table::FindByPath(list, {}).empty());
            CHECK(Table::FindBelow(reload, "Multiplier:").size() == 2);
            CHECK(Table::FindBelow(fire, "Multiplier:") == std::vector<Table::MemoryRecord*>{ &fireMultiplier });
            CHECK(Table::PathText({ "A", "", "B" }) == "A › (no description) › B");

            const auto lookup = [&](const char* name, const std::string& code, const std::string& expected)
                {
                    std::string result;
                    const Status status = lua.RunWithResult(code, name, result);
                    CHECK_MSG(status.Ok() && result == expected, std::string(name) + ": " + (status.Ok() ? result : status.Text));
                };
            lookup("path", "local r = manifold.record.find({ '[CET : Scripts] ()->', 'Reload Speed : Multiplier ()->', 'Type:', 'Multiplier:' }) return tostring(r.id)",
                std::to_string(typeMultiplier.Id));
            lookup("path missing", "local r, e = manifold.record.find({ '[CET : Scripts] ()->', 'Nope' }) return tostring(r) .. ' ' .. e.code", "nil ME-6008");
            lookup("path ambiguous", "local r, e = manifold.record.find({ '[CET : Lists] ()->', 'listECrabIslandType', 'Value' }) return tostring(r) .. ' ' .. e.code .. ' ' .. tostring(e.message:find('2 records', 1, true) ~= nil)",
                "nil ME-6011 true");
            lookup("below", "local s = manifold.record.find({ '[CET : Scripts] ()->', 'Fire Rate : Multiplier ()->' }) return tostring(s.find('Multiplier:').id) .. ' ' .. tostring(s.find('[0020] Health').id)",
                std::to_string(fireMultiplier.Id) + " " + std::to_string(fireHealth.Id));
            lookup("below ambiguous", "local s = manifold.record.find({ '[CET : Scripts] ()->', 'Reload Speed : Multiplier ()->' }) local r, e = s.find('Multiplier:') return tostring(r) .. ' ' .. e.code",
                "nil ME-6011");
            lookup("below path", "local s = manifold.record.find({ '[CET : Scripts] ()->', 'Reload Speed : Multiplier ()->' }) return tostring(s.find({ 'Multiplier:' }).id) .. ' ' .. tostring(s.find({ 'Type:', 'Multiplier:' }).id)",
                std::to_string(reloadMultiplier.Id) + " " + std::to_string(typeMultiplier.Id));
            lookup("path round trip", "local r = manifold.record.find(" + std::to_string(fireHealth.Id) + ") return tostring(manifold.record.find(r.path()) == r) .. ' ' .. #r.path()", "true 4");
            lookup("empty path raises", "local ok, e = pcall(manifold.record.find, {}) return tostring(ok) .. ' ' .. tostring((type(e) == 'table' and e.message or tostring(e)):find('the path is empty', 1, true) ~= nil)", "false true");
            lookup("bad step raises", "local ok = pcall(manifold.record.find, { 'a', 3 }) return tostring(ok)", "false");
            lookup("api version", "return tostring(manifold.apiVersion >= 1 and manifold.has('record.find'))", "true");

            list.TableLuaScript =
                "local a = manifold.record.find({ '[CET : Scripts] ()->', 'Fire Rate : Multiplier ()->', 'Multiplier:' })\n"
                "local b = manifold.record.find({\n  '[CET : Lists] ()->',\n  'listECrabIslandType',\n  'Value'\n})\n"
                "local c = manifold.record.find({ '[CET : Scripts] ()->', 'Gone' })\n"
                "local d = manifold.record.find({ '[CET : Scripts] ()->', name })\n";
            const std::vector<Table::RecordReference> references = Table::FindRecordReferences(list);
            CHECK_MSG(references.size() == 3, std::to_string(references.size()));
            if (references.size() == 3)
            {
                CHECK(references[0].Path.size() == 3 && references[0].TargetId == fireMultiplier.Id && references[0].Matches == 1);
                CHECK(references[1].Line == 2 && references[1].Matches == 2 && references[1].TargetId == -1);
                CHECK(references[2].Line == 7 && references[2].Matches == 0);
            }
            const std::vector<RecordIssue> issues = Table::CheckRecords(list);
            CHECK(Has(issues, Table::kTableLevel, "lua-reference", IssueLevel::Warning, "ME-6011"));
            CHECK(Has(issues, Table::kTableLevel, "lua-reference", IssueLevel::Warning, "Gone"));
            CHECK(Has(issues, twinA.Id, "path", IssueLevel::Warning, "2 records") && Has(issues, twinB.Id, "path", IssueLevel::Warning));
            CHECK_MSG(Of(issues, twinChild.Id, "path").empty(), "the children of duplicated groups are not reported again");
            CHECK(Of(issues, fireMultiplier.Id, "path").empty() && Of(issues, typeMultiplier.Id, "path").empty());
        }

        void PointerChain()
        {
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            if (!engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok())
            {
                Skip("could not open the test program itself");
                return;
            }
            static std::uint32_t value = 0x1234;
            static std::uint8_t block[0x40] = {};
            static void* toBlock = block;
            *reinterpret_cast<std::uint32_t**>(block + 0x18) = &value;

            Table::AddressList& list = engine.Table();
            Table::MemoryRecord& record = list.AddValue("Chain", Hex(reinterpret_cast<std::uint64_t>(&toBlock)), Scan::ValueType::FourBytes);
            record.Offsets = { "18", "0" };
            const std::vector<AddressStep> steps = engine.Tables().ExplainAddress(record);
            CHECK(steps.size() == 5);
            if (steps.size() == 5)
            {
                CHECK(steps[0].What == AddressStep::Kind::Base && steps[0].Value == reinterpret_cast<std::uint64_t>(&toBlock));
                CHECK(steps[1].What == AddressStep::Kind::Read && steps[1].Value == reinterpret_cast<std::uint64_t>(block));
                CHECK(steps[2].What == AddressStep::Kind::Offset && steps[2].Value == reinterpret_cast<std::uint64_t>(block + 0x18));
                CHECK(steps[3].Value == reinterpret_cast<std::uint64_t>(&value));
                CHECK(steps[4].Ok && steps[4].Value == reinterpret_cast<std::uint64_t>(&value));
            }
            engine.ResolveRecord(record);
            CHECK(record.AddressOk && record.ResolvedAddress == reinterpret_cast<std::uint64_t>(&value));

            Table::MemoryRecord& broken = list.AddValue("Broken", Hex(reinterpret_cast<std::uint64_t>(&toBlock)), Scan::ValueType::FourBytes);
            broken.Offsets = { "20", "0", "0" };
            const std::vector<AddressStep> stops = engine.Tables().ExplainAddress(broken);
            CHECK(stops.size() == 6 && !stops.back().Ok && stops.back().What == AddressStep::Kind::Read && !stops.back().Error.empty());

            Table::MemoryRecord& header = list.AddGroup("Header");
            header.AddressGroupHeader = true;
            header.Address = Hex(reinterpret_cast<std::uint64_t>(block));
            Table::MemoryRecord& child = list.AddValue("Child", "+18", Scan::ValueType::EightBytes, &header);
            const std::vector<AddressStep> relative = engine.Tables().ExplainAddress(child);
            CHECK(relative.size() == 2 && relative[0].What == AddressStep::Kind::Parent && relative[1].Value == reinterpret_cast<std::uint64_t>(block + 0x18));

            // The owner's Isaac table: "[Chapter] ()->" has the address, "[Other]" below it is a plain group, and the "+0004"
            // records below that count from the header, not from the group without an address.
            Table::MemoryRecord& plain = list.AddGroup("[Other]", &header);
            Table::MemoryRecord& innerScript = list.AddScript("Inner Script", "[ENABLE]\n[DISABLE]\n", &plain);
            Table::MemoryRecord& floor = list.AddValue("[0004] Floor Type", "+4", Scan::ValueType::FourBytes, &plain);
            Table::MemoryRecord& deeper = list.AddValue("[0010] Deeper", "+10", Scan::ValueType::FourBytes, &innerScript);
            CHECK(Table::RelativeBase(floor) == &header && Table::RelativeBase(deeper) == &header && Table::RelativeBase(header) == nullptr);
            engine.ResolveRecord(floor);
            engine.ResolveRecord(deeper);
            CHECK_MSG(floor.AddressOk && floor.ResolvedAddress == reinterpret_cast<std::uint64_t>(block + 4), floor.AddressError);
            CHECK_MSG(deeper.AddressOk && deeper.ResolvedAddress == reinterpret_cast<std::uint64_t>(block + 0x10), deeper.AddressError);
            const std::vector<AddressStep> skipped = engine.Tables().ExplainAddress(floor);
            CHECK(skipped.size() == 2 && skipped[0].Text.find("Header") != std::string::npos && skipped[1].Value == reinterpret_cast<std::uint64_t>(block + 4));
            CHECK(engine.Tables().AddressTextOf(floor).Tooltip == "Header +4");
            const std::vector<RecordIssue> fine = Table::CheckRecords(list);
            CHECK(Of(fine, floor.Id, "address").empty() && Of(fine, deeper.Id, "address").empty());

            // A value above counts too, and a value's own relative address chains on.
            Table::MemoryRecord& baseValue = list.AddValue("[0008] Base Value", "+8", Scan::ValueType::EightBytes, &plain);
            Table::MemoryRecord& below = list.AddValue("[0004] Below Value", "+4", Scan::ValueType::FourBytes, &baseValue);
            engine.ResolveRecord(below);
            CHECK(Table::RelativeBase(below) == &baseValue && below.AddressOk && below.ResolvedAddress == reinterpret_cast<std::uint64_t>(block + 0xC));

            // Without any record with an address above, it fails with a message that says so.
            Table::MemoryRecord& loose = list.AddGroup("Loose");
            Table::MemoryRecord& orphan = list.AddValue("Orphan", "+4", Scan::ValueType::FourBytes, &loose);
            engine.ResolveRecord(orphan);
            CHECK(!orphan.AddressOk && orphan.AddressError.find("no record with an address above") != std::string::npos);

            Table::MemoryRecord& script = list.AddScript("Script", "[ENABLE]\n[DISABLE]\n");
            CHECK(engine.Tables().ExplainAddress(script).empty());
        }

        void Trace()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            ActivationRunner& activation = engine.Activation();
            Table::AddressList& list = engine.Table();

            Table::MemoryRecord& group = list.AddGroup("Group");
            group.Options.ActivateChildren = true;
            Table::MemoryRecord& inner = list.AddGroup("Inner", &group);
            CHECK(engine.SetActive(group, true).Ok());
            const auto& trace = activation.Trace();
            CHECK(trace.size() == 2);
            if (trace.size() == 2)
            {
                CHECK(trace[0].RecordId == group.Id && trace[0].Active && trace[0].Result == ActivationTraceEntry::Outcome::Done && trace[0].ViaParent == -1);
                CHECK(trace[1].RecordId == inner.Id && trace[1].ViaParent == group.Id && trace[1].Kind == Table::RecordKind::Group);
                CHECK(trace[0].Serial + 1 == trace[1].Serial && !trace[0].Async);
            }

            Table::MemoryRecord& value = list.AddValue("No Process", "00400000", Scan::ValueType::FourBytes);
            CHECK(!engine.SetActive(value, true).Ok());
            CHECK(!trace.empty() && trace.back().RecordId == value.Id && trace.back().Result == ActivationTraceEntry::Outcome::Failed && !trace.back().Error.empty());

            Table::MemoryRecord& vetoed = list.AddGroup("Vetoed");
            std::string result;
            const Status set = lua.RunWithResult("local r = getAddressList().getMemoryRecordByID(" + std::to_string(vetoed.Id) + ") r.OnActivate = function() return false end return 'ok'", "veto", result);
            CHECK_MSG(set.Ok() && result == "ok", set.Text + result);
            CHECK(engine.SetActive(vetoed, true).Ok() && !vetoed.Active);
            CHECK(!trace.empty() && trace.back().RecordId == vetoed.Id && trace.back().Result == ActivationTraceEntry::Outcome::Vetoed);
            CHECK(!trace.back().FromScript);

            Table::MemoryRecord& byLua = list.AddGroup("By Lua");
            const Status switched = lua.RunWithResult("getAddressList().getMemoryRecordByID(" + std::to_string(byLua.Id) + ").Active = true return 'ok'", "by lua", result);
            CHECK_MSG(switched.Ok() && result == "ok", switched.Text + result);
            CHECK(byLua.Active && !trace.empty() && trace.back().RecordId == byLua.Id && trace.back().FromScript);

            if (!engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok())
            {
                Skip("could not open the test program itself for the script trace");
                return;
            }
            Table::MemoryRecord& script = list.AddScript("Traced Script",
                "[ENABLE]\nalloc(tracemem,64)\nregistersymbol(tracemem)\ntracemem:\ndd 1\ndd 2\ndd 3\n[DISABLE]\nunregistersymbol(tracemem)\ndealloc(tracemem)\n");
            const Status on = engine.SetActive(script, true);
            CHECK_MSG(on.Ok(), on.Text);
            const ActivationTraceEntry& enabled = trace.back();
            CHECK(enabled.RecordId == script.Id && enabled.Result == ActivationTraceEntry::Outcome::Done && enabled.Kind == Table::RecordKind::Script);
            CHECK_MSG(enabled.Allocated.size() == 1 && enabled.Allocated.front().Name == "tracemem", std::to_string(enabled.Allocated.size()));
            CHECK(enabled.Registered.size() == 1 && enabled.Registered.front() == "tracemem");
            CHECK_MSG(enabled.WriteCount >= 1 && enabled.BytesWritten >= 12 && !enabled.Writes.empty(), Format("%zu writes, %zu bytes", enabled.WriteCount, enabled.BytesWritten));
            const Status off = engine.SetActive(script, false);
            CHECK_MSG(off.Ok(), off.Text);
            const ActivationTraceEntry& disabled = trace.back();
            CHECK(disabled.RecordId == script.Id && !disabled.Active && disabled.Result == ActivationTraceEntry::Outcome::Done);
            CHECK(disabled.Released.size() == 1 && disabled.Unregistered.size() == 1);

            Table::MemoryRecord& failing = list.AddScript("Failing", "[ENABLE]\nnot an instruction\n[DISABLE]\n");
            CHECK(!engine.SetActive(failing, true).Ok());
            CHECK(trace.back().RecordId == failing.Id && trace.back().Result == ActivationTraceEntry::Outcome::Failed && trace.back().ErrorLine > 0);

            for (std::size_t i = 0; i < ActivationRunner::kTraceLength + 5; ++i)
            {
                engine.SetActive(group, false);
                engine.SetActive(group, true);
            }
            CHECK(trace.size() == ActivationRunner::kTraceLength);
            activation.ClearTrace();
            CHECK(trace.empty() && activation.TraceSerial() > ActivationRunner::kTraceLength);
        }

        // A child made in code (Id 0, no file ID) gets an id of its own when its tree is added, as a root does; a child the file
        // numbered 0 keeps it. getMemoryRecordByID(0) used to find the code-made child.
        void ClaimTreeIds()
        {
            {
                Table::AddressList list;
                auto parent = std::make_unique<Table::MemoryRecord>();
                parent->Description = "Made Parent";
                parent->Kind = Table::RecordKind::Group;
                auto child = std::make_unique<Table::MemoryRecord>();
                child->Description = "Made Child";
                child->Parent = parent.get();
                parent->Children.push_back(std::move(child));
                Table::MemoryRecord& added = list.Add(std::move(parent));
                const Table::MemoryRecord& made = *added.Children.front();
                CHECK_MSG(made.Id != 0 && made.Id != added.Id, std::to_string(made.Id));
                CHECK(list.FindById(made.Id) == &made && list.FindById(added.Id) == &added);
                CHECK(list.FindById(0) == nullptr);
            }
            {
                Table::AddressList list;
                auto parent = std::make_unique<Table::MemoryRecord>();
                parent->Description = "File Parent";
                parent->Kind = Table::RecordKind::Group;
                parent->Id = 3;
                parent->FileId = 3;
                auto child = std::make_unique<Table::MemoryRecord>();
                child->Description = "File Child";
                child->FileId = 0;
                child->Parent = parent.get();
                parent->Children.push_back(std::move(child));
                Table::MemoryRecord& added = list.Add(std::move(parent));
                const Table::MemoryRecord& filed = *added.Children.front();
                CHECK(added.Id == 3 && filed.Id == 0 && list.FindById(0) == &filed);
            }
        }

        // find(4294967296) used to be cut to id 0, find(1.5) to 0 as well.
        void FindRejectsOddIds()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            auto zero = std::make_unique<Table::MemoryRecord>();
            zero->Description = "Zero";
            zero->FileId = 0;
            Table::MemoryRecord& added = engine.Table().Add(std::move(zero));
            CHECK(added.Id == 0);
            const auto run = [&](const char* name, const std::string& code, const std::string& expected)
                {
                    std::string result;
                    const Status status = lua.RunWithResult(code, name, result);
                    CHECK_MSG(status.Ok() && result == expected, std::string(name) + ": " + (status.Ok() ? result : status.Text));
                };
            run("zero", "return tostring(manifold.record.find(0).id)", "0");
            run("huge", "local r, e = manifold.record.find(4294967296) return tostring(r) .. ' ' .. tostring(e and e.code)", "nil ME-6008");
            run("negative", "local r, e = manifold.record.find(-1) return tostring(r) .. ' ' .. tostring(e and e.code)", "nil ME-6008");
            run("fraction", "local ok, r = pcall(manifold.record.find, 1.5) return tostring(ok and r ~= nil)", "false");
        }

        // The Address List Debug check assembles with Lua held back even in a trusted table; the editor's Check runs it.
        void CheckOnlyRunsNoLua()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            CHECK(engine.Tables().ScriptsTrusted());
            const std::string script = "[ENABLE]\n{$lua}\ncheckOnlyRan = (checkOnlyRan or 0) + 1\n{$asm}\n[DISABLE]\n";
            std::string result;
            Asm::AaMemory memory;
            const ScriptCheck held = engine.CheckScript(script, Asm::AaSection::Enable, memory, true);
            CHECK_MSG(held.LuaHeldBack >= 1, std::to_string(held.LuaHeldBack));
            CHECK(lua.RunWithResult("return tostring(checkOnlyRan)", "held", result).Ok() && result == "nil");
            Asm::AaMemory editor;
            const ScriptCheck ran = engine.CheckScript(script, Asm::AaSection::Enable, editor);
            CHECK(ran.LuaHeldBack == 0);
            CHECK_MSG(lua.RunWithResult("return tostring(checkOnlyRan)", "ran", result).Ok() && result == "1", result);
        }

        // Removing a group switches off the scripts below it first (their [DISABLE] takes the hooks out), fires ondestroy for
        // every removed record, refuses while one of them waits in the activation queue, and setActive reports a veto.
        void RemoveSwitchesOffBelow()
        {
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            Table::AddressList& list = engine.Table();
            std::string result;
            const auto run = [&](const char* name, const std::string& code, const std::string& expected)
                {
                    const Status status = lua.RunWithResult(code, name, result);
                    CHECK_MSG(status.Ok() && result == expected, std::string(name) + ": " + (status.Ok() ? result : status.Text));
                };

            Table::MemoryRecord& vetoed = list.AddGroup("Remove Vetoed");
            run("veto handler", "getAddressList().getMemoryRecordByID(" + std::to_string(vetoed.Id) + ").OnActivate = function() return false end return 'ok'", "ok");
            run("veto reported", "local r, e = manifold.record.find(" + std::to_string(vetoed.Id) + ").setActive(true) return tostring(r) .. ' ' .. tostring(e and e.code)",
                "nil ME-7015");
            CHECK(!vetoed.Active);

            Table::MemoryRecord& queued = list.AddGroup("Remove Queued");
            const int queuedId = queued.Id;
            engine.SetAsync(true);
            engine.RequestActive(queued, true, true);
            CHECK(engine.ActivationPending(queuedId));
            run("refused while queued", "local r, e = manifold.record.find(" + std::to_string(queuedId) + ").remove() return tostring(r) .. ' ' .. tostring(e and e.message:find('being switched', 1, true) ~= nil)",
                "nil true");
            CHECK(list.FindById(queuedId) == &queued);
            engine.CancelActivations();
            engine.SetAsync(false);

            if (!engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok())
            {
                Skip("could not open the test program itself for removing a group with a script on below it");
                return;
            }
            Table::MemoryRecord& group = list.AddGroup("Remove Group");
            Table::MemoryRecord& script = list.AddScript("Remove Child",
                "[ENABLE]\nalloc(removemem,64)\nregistersymbol(removemem)\nremovemem:\ndd 1\n[DISABLE]\nunregistersymbol(removemem)\ndealloc(removemem)\n", &group);
            const int groupId = group.Id;
            const int scriptId = script.Id;
            const Status on = engine.SetActive(script, true);
            CHECK_MSG(on.Ok() && script.Active, on.Text);
            CHECK(engine.Evaluate("removemem").has_value());
            CHECK(!group.Active);
            run("ondestroy", "destroyed = {} for _, id in ipairs({ " + std::to_string(groupId) + ", " + std::to_string(scriptId) +
                " }) do getAddressList().getMemoryRecordByID(id).OnDestroy = function() destroyed[#destroyed + 1] = id end end return 'ok'", "ok");
            run("remove", "return tostring(manifold.record.find(" + std::to_string(groupId) + ").remove())", "true");
            CHECK(list.FindById(groupId) == nullptr && list.FindById(scriptId) == nullptr);
            CHECK_MSG(!engine.Evaluate("removemem").has_value(), "the child's [DISABLE] part did not run");
            const auto& trace = engine.Activation().Trace();
            CHECK(!trace.empty() && trace.back().RecordId == scriptId && !trace.back().Active && trace.back().Released.size() == 1);
            CHECK(!trace.empty() && trace.back().WriteSizes.size() == trace.back().Writes.size());
            run("destroyed", "table.sort(destroyed) return table.concat(destroyed, ',')",
                std::to_string(std::min(groupId, scriptId)) + "," + std::to_string(std::max(groupId, scriptId)));
        }
    }

    int RunRecordDebugTests()
    {
        const int before = Totals().Failures;
        References();
        StaticChecks();
        Renumbered();
        LoadKeepsFileIds();
        Paths();
        PointerChain();
        Trace();
        ClaimTreeIds();
        FindRejectsOddIds();
        CheckOnlyRunsNoLua();
        RemoveSwitchesOffBelow();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("recorddebug", "Address list debug", 28, [](const SuiteContext&) { return RunRecordDebugTests(); });
}
