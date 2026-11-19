#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/DocRegistry.h"
#include "Lua/LuaCompatGaps.h"
#include "Lua/LuaDocs.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        void LuaCheck(Engine& engine, const char* name, const std::string& code)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(code, name, result);
            CHECK_MSG(status.Ok() && result == "ok", std::string(name) + ": " + (status.Ok() ? result : status.Text));
        }

        void Extensions(Engine& engine)
        {
            LuaCheck(engine, "split", R"LUA(
                local a, b, c = ("x, y, z"):split(", ")
                if a ~= "x" or b ~= "y" or c ~= "z" then return "split gave " .. tostring(a) .. tostring(b) .. tostring(c) end
                if select("#", string.split("a b", " ")) ~= 2 then return "split count" end
                if not ("table.lua"):endsWith(".lua") or not ("table.lua"):startsWith("table") then return "starts or ends" end
                return "ok")LUA");
            LuaCheck(engine, "defines", R"LUA(
                if vtDword ~= 2 or vtAutoAssembler ~= 11 or soExactValue ~= 1 or mrhToggleActivation ~= 0 or mrhDecreaseValue ~= 7 then return "constants" end
                if clDefault ~= 0x20000000 or rtTruncated ~= 2 or fsmAligned ~= 1 then return "more constants" end
                if bptExecute ~= 0 or bptAccess ~= 1 or bptWrite ~= 2 then return "breakpoint triggers " .. tostring(bptAccess) .. "/" .. tostring(bptWrite) end
                if mrOK ~= 1 or mrOk ~= 1 or mrYes ~= 6 or mbOK ~= 2 or co_runtill ~= 3 or VK_F1 ~= 0x70 or PAGE_EXECUTE_READWRITE ~= 0x40 then return "dialog and key constants" end
                return "ok")LUA");
            LuaCheck(engine, "no process", R"LUA(
                local modules = enumModules()
                if type(modules) ~= "table" or #modules ~= 0 then return "enumModules" end
                if readInteger(0x10000) ~= nil then return "readInteger" end
                if writeInteger(0x10000, 1) ~= false then return "writeInteger" end
                return "ok")LUA");
            LuaCheck(engine, "local", R"LUA(
                if getAddressSafe("kernel32.GetTickCount", true) == nil then return "local export" end
                if lfs.attributes(".", "mode") ~= "directory" then return "lfs" end
                if extractFileNameWithoutExt([[C:\a\b.CT]]) ~= "b" or extractFilePath([[C:\a\b.CT]]) ~= [[C:\a\]] then return "paths" end
                if stringToMD5String("abc") ~= "900150983cd24fb0d6963f7d28e17f72" then return "md5" end
                return "ok")LUA");
        }

        void Records(Engine& engine)
        {
            LuaCheck(engine, "records", R"LUA(
                local mr = getAddressList().createMemoryRecord()
                mr.description = "lower"
                if mr.Description ~= "lower" then return "case" end
                mr.setDescription("dot")
                if mr.Description ~= "dot" then return "dot call" end
                mr:setDescription("colon")
                if mr.DESCRIPTION ~= "colon" then return "colon call" end
                if not rawequal(AddressList.getMemoryRecordByID(mr.ID), mr) then return "identity" end
                mr.MyField = 5
                if mr.myfield ~= 5 then return "extra field" end
                mr.DropDownList.Text = "0:Off\n1:On"
                if mr.DropDownCount ~= 2 or mr.DropDownDescription[1] ~= "On" or mr.DropDownValue[0] ~= "0" then return "drop-down" end
                local hotkey = mr.createHotkey({ VK_F1 }, mrhSetValue, "5", "set five")
                if mr.HotkeyCount ~= 1 or hotkey.Description ~= "set five" or mr.Hotkey[0].ID ~= hotkey.ID or hotkey.Action ~= mrhSetValue then return "hotkey" end
                mr.Options = "[moHideChildren,moActivateChildrenAsWell]"
                if not mr.Options:find("moHideChildren") or not mr.Options:find("moActivateChildrenAsWell") then return "options" end
                if memoryrecord_getDescription(mr) ~= "colon" then return "legacy global" end
                mr.Type = vtSingle
                if mr.VarType ~= vtSingle then return "type" end
                return "ok")LUA");
            LuaCheck(engine, "activation hook", R"LUA(
                local group = getAddressList().createMemoryRecord()
                group.IsGroupHeader = true
                local calls = 0
                group.OnActivate = function(record, before, current)
                    calls = calls + 1
                    return false
                end
                group.Active = true
                if group.Active then return "activation should have been cancelled" end
                if calls ~= 1 then return "OnActivate calls " .. calls end
                group.OnActivate = function(record, before, current) return true end
                group.Active = true
                if not group.Active then return "activation should have passed" end
                return "ok")LUA");
        }

        void Streams(Engine& engine)
        {
            LuaCheck(engine, "streams", R"LUA(
                local stream = createStringStream("abc")
                if stream.Size ~= 3 or readStringLocal(stream.Memory, stream.Size) ~= "abc" then return "string stream" end
                stream.Position = 0
                if stream.readByte() ~= 97 then return "readByte" end
                local file = createTableFile("compat-test.txt")
                file.Stream.writeString("hello")
                local found = findTableFile("compat-test.txt")
                if not found or found.Stream.Size ~= 5 then return "table file" end
                found.delete()
                if findTableFile("compat-test.txt") ~= nil then return "delete" end
                return "ok")LUA");
        }

        void Forms(Engine& engine)
        {
            LuaCheck(engine, "forms", R"LUA(
                local form = createForm(false)
                local label = createLabel(form)
                label.Caption = "Hi"
                if label.Parent ~= form or form.ControlCount ~= 1 or form.Control[0] ~= label then return "parent" end
                label.Name = "lblHello"
                if form.lblHello ~= label or form.findComponentByName("lblHello") ~= label then return "names" end
                local list = createStringlist()
                list.add("a")
                list.add("b")
                if list.Count ~= 2 or list[1] ~= "b" or list.Text ~= "a\r\nb\r\n" then return "strings" end
                local memo = createMemo(form)
                memo.Lines.add("x")
                if memo.Text ~= "x" then return "memo " .. memo.Text end
                if not MainForm.findComponentByName("miTable") then return "miTable" end
                if MainForm.Menu.Items.Count < 5 then return "menu" end
                local item = createMenuItem(MainForm.Menu)
                item.Caption = "Mine"
                MainForm.Menu.Items.add(item)
                if MainForm.Menu.Items.find("Mine") ~= item then return "menu find" end
                if getLuaEngine().mOutput == nil or getLuaEngine().mScript == nil then return "lua engine" end
                if type(form.Font.Size) ~= "number" or form.Canvas.getTextWidth("abc") <= 0 then return "font" end
                if component_getComponent(AddressList, 1) == nil then return "address list header" end
                form.destroy()
                return "ok")LUA");
        }

        void CustomTypes(Engine& engine)
        {
            LuaCheck(engine, "custom types", R"LUA(
                registerCustomTypeLua("Twice", 4,
                    function(a, b, c, d) return (a | (b << 8) | (c << 16) | (d << 24)) * 2 end,
                    function(value) value = value // 2 return value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF end,
                    false)
                local twice = getCustomType("twice")
                if not twice or twice.byteSize ~= 4 then return "lookup" end
                if twice.byteTableToValue({ 1, 0, 0, 0 }) ~= 2 then return "to value" end
                if twice.valueToByteTable(10)[1] ~= 5 then return "to bytes" end
                return "ok")LUA");
            LuaCheck(engine, "string custom type", R"LUA(
                registerCustomTypeLua("Letters", 2,
                    function(a, b) return string.char(a, b) end,
                    function(text) return text:byte(1), text:byte(2) end,
                    false, true)
                local letters = getCustomType("Letters")
                if letters.byteTableToValue({ 72, 105 }) ~= "Hi" then return "to text" end
                if letters.valueToByteTable("Yo")[2] ~= 111 then return "to bytes" end
                local s = createStructure("letters")
                local e = s.addElement()
                e.Vartype = vtCustom
                e.CustomType = letters
                if e.Vartype ~= vtCustom or e.CustomTypeName ~= "Letters" or e.Bytesize ~= 2 or e.CustomType.Name ~= "Letters" then return "element custom type" end
                e.Vartype = vtDword
                if e.CustomTypeName ~= "" or e.CustomType ~= nil then return "custom type kept" end
                local bits = s.addElement()
                bits.Offset = 0x14
                bits.BitStart = 6
                bits.BitLength = 4
                bits.Vartype = vtBinary
                bits.DisplayMethod = "dtBinary"
                if bits.Vartype ~= vtBinary or bits.Bytesize ~= 2 or bits.BitStart ~= 6 or bits.BitLength ~= 4 or bits.DisplayMethod ~= "dtBinary" then return "binary element" end
                bits.BitLength = 1
                if bits.Bytesize ~= 1 then return "binary size" end
                s.destroy()
                return "ok")LUA");
        }

        void StructureCallbacks(Engine& engine)
        {
            LuaCheck(engine, "structure callbacks", R"LUA(
                structureLookupId = registerStructureNameLookup(function(address)
                    if address == 0x1234 then return "Thing", 0x1200 end
                end)
                structureOverrideId = registerStructureDissectOverride(function(structure, base)
                    if base ~= 0x1200 then return nil end
                    local second = structure.addElement()
                    second.Offset, second.Name, second.Vartype = 8, "Second", vtDword
                    local first = structure.addElement()
                    first.Offset, first.Name, first.Vartype = 0, "First", vtPointer
                    return true
                end)
                return "ok")LUA");
            const auto lookup = engine.Lua().LookupStructureName(0x1234);
            CHECK_MSG(lookup && lookup->Name == "Thing" && lookup->Address == 0x1200, "structure name lookup");
            CHECK_MSG(!engine.Lua().LookupStructureName(0x5678), "structure name lookup without a name");
            Table::StructDefinition& filled = engine.Structures().Add("Callback test");
            CHECK_MSG(engine.Lua().DissectStructure(filled, 0x1200) && filled.Fields.size() == 2 && filled.Fields[0].Name == "First" && filled.Fields[1].Offset == 8,
                "dissect override fills and sorts");
            Table::StructDefinition& declined = engine.Structures().Add("Callback test 2");
            CHECK_MSG(!engine.Lua().DissectStructure(declined, 0x9999) && declined.Fields.empty(), "dissect override declines");
            engine.Structures().Remove(filled);
            engine.Structures().Remove(declined);
            LuaCheck(engine, "unregister structure callbacks", R"LUA(
                if not unregisterStructureNameLookup(structureLookupId) or not unregisterStructureDissectOverride(structureOverrideId) then return "unregister" end
                if unregisterStructureNameLookup(structureLookupId) then return "unregistered twice" end
                return "ok")LUA");
            CHECK_MSG(!engine.Lua().LookupStructureName(0x1234), "structure name lookup after unregister");
        }

        void LaterCallbacks(Engine& engine)
        {
            LuaCheck(engine, "slow structure callbacks", R"LUA(
                local function busy(milliseconds)
                    local stop = getTickCount() + milliseconds
                    while getTickCount() < stop do end
                end
                laterLookupId = registerStructureNameLookup(function(address)
                    if address ~= 0x4321 then return nil end
                    busy(150)
                    return "Slow", 0x4300
                end)
                laterOverrideId = registerStructureDissectOverride(function(structure, base)
                    if base ~= 0x4300 then return nil end
                    busy(150)
                    local field = structure.addElement()
                    field.Offset, field.Name, field.Vartype = 4, "Late", vtDword
                    return true
                end)
                return "ok")LUA");
            engine.SetAsync(true);
            std::optional<Lua::StructureLookup> named;
            bool nameDone = false;
            engine.Lua().LookupStructureNameLater(0x4321, [&](std::optional<Lua::StructureLookup> found) { named = found; nameDone = true; });
            Table::StructDefinition& later = engine.Structures().Add("Later test");
            bool handled = false;
            bool dissectDone = false;
            engine.Lua().DissectStructureLater(&later, 0x4300, [&](bool result) { handled = result; dissectDone = true; });
            double longest = 0.0;
            const double started = engine.Now();
            while ((!nameDone || !dissectDone) && engine.Now() - started < 5.0)
            {
                const double before = engine.Now();
                engine.Tick();
                longest = std::max(longest, engine.Now() - before);
            }
            CHECK_MSG(nameDone && named && named->Name == "Slow" && named->Address == 0x4300, "a slow name lookup finishes in the background");
            CHECK_MSG(dissectDone && handled && later.Fields.size() == 1 && later.Fields[0].Name == "Late", "a slow dissect override finishes in the background");
            CHECK_MSG(longest < 0.1, "one frame took " + std::to_string(longest) + " s while the lookups ran");
            engine.Structures().Remove(later);
            engine.SetAsync(false);
            LuaCheck(engine, "unregister slow structure callbacks", R"LUA(
                unregisterStructureNameLookup(laterLookupId)
                unregisterStructureDissectOverride(laterOverrideId)
                return "ok")LUA");

            bool none = false;
            engine.Lua().LookupStructureNameLater(0x4321, [&](std::optional<Lua::StructureLookup> found) { none = !found; });
            CHECK_MSG(none, "without lookups the answer comes at once");
        }

        void VisibleRefresh(Engine& engine)
        {
            Table::AddressList& list = engine.Table();
            Table::MemoryRecord& shown = list.AddValue("Shown", "1000", Scan::ValueType::FourBytes);
            Table::MemoryRecord& hidden = list.AddValue("Hidden", "2000", Scan::ValueType::FourBytes);
            const int shownId = shown.Id;
            const int hiddenId = hidden.Id;
            engine.SetVisibleRecords({ shownId });
            engine.RefreshValues(true);
            CHECK_MSG(shown.RefreshedAt > 0.0 && shown.ValueText == "??", "a visible record is refreshed");
            CHECK_MSG(hidden.RefreshedAt < 0.0, "a record that is not shown is left alone");
            CHECK(engine.NeedsValue(hidden) && !engine.NeedsValue(shown));
            engine.RefreshRecord(hiddenId);
            CHECK(!engine.NeedsValue(hidden) && hidden.ValueText == "??");
            list.Remove(*list.FindById(shownId));
            list.Remove(*list.FindById(hiddenId));
        }

        void SymbolIndex(Engine& engine)
        {
            Target::Symbols& symbols = engine.Symbols();
            CHECK(symbols.Register("Beta", 0x7000, "test").Ok());
            CHECK(symbols.Register("Alpha", 0x7000, "test").Ok());
            CHECK_EQ(symbols.Describe(0x7000), std::string("Alpha"));
            CHECK(symbols.ExactName(0x7000) == std::optional<std::string>("Alpha"));
            const auto inside = symbols.Containing(0x7010, 0x100);
            CHECK(inside && inside->first == 0x7000 && inside->second == "Alpha");
            CHECK(!symbols.Containing(0x7200, 0x100));
            CHECK(!symbols.Containing(0x6FF0, 0x100));
            CHECK(symbols.Unregister("alpha"));
            CHECK_EQ(symbols.Describe(0x7000), std::string("Beta"));
            CHECK(symbols.Register("Beta", 0x7100, "test", true).Ok());
            CHECK_EQ(symbols.Describe(0x7100), std::string("Beta"));
            CHECK(symbols.Describe(0x7000) != "Beta");
            CHECK(symbols.Register("Gamma", 0x7200, "other").Ok());
            symbols.UnregisterOwner("test");
            CHECK(symbols.Describe(0x7100) != "Beta");
            CHECK_EQ(symbols.Describe(0x7200), std::string("Gamma"));
            CHECK(symbols.Unregister("Gamma"));
            CHECK(!symbols.ExactName(0x7200));
        }

        void CallbackLimit(Engine& engine)
        {
            const int saved = engine.Options().ScriptCallbackLimitMs;
            engine.Options().ScriptCallbackLimitMs = 250;
            LuaCheck(engine, "endless timer", R"LUA(
                endlessTimer = createTimer(nil, false)
                endlessTimer.Interval = 1
                endlessTimer.OnTimer = function()
                    while true do
                        pcall(function() while true do end end)
                    end
                end
                endlessTimer.Enabled = true
                return "ok")LUA");
            const double started = engine.Now();
            for (int i = 0; i < 6; ++i)
            {
                engine.Lua().Tick(engine.Now());
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            const double spent = engine.Now() - started;
            CHECK_MSG(spent < 2.0, "the endless timer held the frame for " + std::to_string(spent) + " s");
            LuaCheck(engine, "endless timer is off", R"LUA(
                if endlessTimer.Enabled then return "still enabled" end
                endlessTimer.destroy()
                local value = 0
                for i = 1, 1000000 do value = value + i end
                if value ~= 500000500000 then return "ordinary code after the limit" end
                return "ok")LUA");
            engine.Options().ScriptCallbackLimitMs = saved;
        }

        void SymbolCallbacks(Engine& engine)
        {
            LuaCheck(engine, "symbol lookup callbacks", R"LUA(
                symbolFailureId = registerSymbolLookupCallback(function(name)
                    if name == "Game::Thing::vftable" then return 0x4000 end
                    if name == "Recursive" then return getAddressSafe("Recursive") end
                end, slFailure)
                symbolStartId = registerSymbolLookupCallback(function(name)
                    if name == "Early" then return 0x5000 end
                end, slStart)
                if getAddressSafe("Game::Thing::vftable") ~= 0x4000 then return "failure callback" end
                if getAddressSafe("Game::Thing::vftable+10") ~= 0x4010 then return "inside an expression" end
                if getAddressSafe("Early") ~= 0x5000 then return "start callback" end
                if getAddressSafe("Unknown::vftable") ~= nil then return "unknown name" end
                if getAddressSafe("Recursive") ~= nil then return "recursion" end
                registerSymbol("Early", 0x6000, true)
                if getAddressSafe("Early") ~= 0x5000 then return "start callback before registered symbols" end
                unregisterSymbol("Early")
                return "ok")LUA");
            CHECK_EQ(engine.Evaluate("Game::Thing::vftable+8").value_or(0), 0x4008u);
            std::optional<std::uint64_t> fromThread = 1;
            std::thread([&] { fromThread = engine.Symbols().Resolve("Game::Thing::vftable"); }).join();
            CHECK_MSG(!fromThread, "symbol callbacks only run on the Lua thread");
            LuaCheck(engine, "unregister symbol lookup callbacks", R"LUA(
                if not unregisterSymbolLookupCallback(symbolFailureId) or not unregisterSymbolLookupCallback(symbolStartId) then return "unregister" end
                if unregisterSymbolLookupCallback(symbolFailureId) then return "unregistered twice" end
                if getAddressSafe("Game::Thing::vftable") ~= nil then return "still resolved" end
                return "ok")LUA");
        }

        void WithoutProcess(Engine& engine)
        {
            LuaCheck(engine, "lua script without a process", R"LUA(
                local mr = getAddressList().createMemoryRecord()
                mr.Script = "{$lua}\nif syntaxcheck then return end\nif memrec.Active then startedByScript = false else startedByScript = true end\n[ENABLE]\n[DISABLE]\n"
                mr.Active = true
                if not mr.Active then return "not active: " .. tostring(mr.LastError) end
                if startedByScript ~= true then return "the Lua part did not run" end
                mr.Active = false
                if mr.Active then return "still active" end
                local hook = getAddressList().createMemoryRecord()
                hook.Script = "[ENABLE]\nalloc(newmem,$100)\nnewmem:\n  ret\n[DISABLE]\ndealloc(newmem)\n"
                hook.Active = true
                if hook.Active then return "a script that allocates ran without a process" end
                if not tostring(hook.LastError):find("ME-1005", 1, true) then return "error was " .. tostring(hook.LastError) end
                return "ok")LUA");
            const Asm::AaResult result = engine.AutoAssemble("[ENABLE]\n[DISABLE]\n", false);
            CHECK_MSG(result.Ok(), result.Error.Text);
            CHECK(engine.ActiveScripts().empty());

            LuaCheck(engine, "turn everything off", R"LUA(
                local group = getAddressList().createMemoryRecord()
                group.IsGroupHeader = true
                group.Active = true
                local child = getAddressList().createMemoryRecord()
                child.IsGroupHeader = true
                child.appendToEntry(group)
                child.Active = true
                if not group.Active or not child.Active then return "setup" end
                return "ok")LUA");
            CHECK(engine.DeactivateAll().empty());
            bool anyActive = false;
            engine.Table().ForEach([&](Table::MemoryRecord& record) { anyActive = anyActive || record.Active; });
            CHECK(!anyActive);
        }

        void SettingsRoundTrip()
        {
            wchar_t temp[MAX_PATH] = L"";
            ::GetTempPathW(MAX_PATH, temp);
            const std::filesystem::path path = std::filesystem::path(temp) / L"manifold-settings-test.ini";
            Settings written;
            written.ActiveScriptsOnQuit = QuitPolicy::Disable;
            written.CompactMode = true;
            written.ReleaseMode = true;
            CHECK(written.Save(path).Ok());
            Settings read;
            CHECK(read.Load(path));
            CHECK(read.ActiveScriptsOnQuit == QuitPolicy::Disable);
            CHECK(read.CompactMode);
            CHECK(read.ReleaseMode);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }

        void ScriptMenus(Engine& engine)
        {
            LuaCheck(engine, "script menus", R"LUA(
                local view = getMemoryViewForm()
                local popup = view.debuggerpopup
                if popup == nil then return "no popup" end
                menuClicks, menuOpened = 0, 0
                popup.OnPopup = function(sender) menuOpened = menuOpened + 1 end
                local item = createMenuItem(popup)
                item.Caption = "&Copy signature"
                item.OnClick = function(sender) menuClicks = menuClicks + 10 end
                popup.Items.add(item)
                local hidden = createMenuItem(popup)
                hidden.Caption = "Hidden"
                hidden.Visible = false
                popup.Items.add(hidden)
                local root = createMenuItem(view.Menu)
                root.Caption = "Manifold"
                view.Menu.Items.add(root)
                local child = createMenuItem(view.Menu)
                child.Caption = "Find"
                child.Shortcut = "Ctrl+Shift+F"
                child.OnClick = function() menuClicks = menuClicks + 1 end
                root.add(child)
                menuItemId = 0
                return "ok")LUA");
            Lua::LuaEngine& lua = engine.Lua();
            const std::uint64_t generation = lua.MenuGeneration();
            const auto popup = lua.MenuItems(Lua::ScriptMenu::MemoryViewPopup);
            CHECK_MSG(popup.size() == 1 && popup[0].Caption == "Copy signature", std::to_string(popup.size()));
            const auto menu = lua.MenuItems(Lua::ScriptMenu::MemoryViewMenu);
            CHECK_MSG(menu.size() == 1 && menu[0].Caption == "Manifold" && menu[0].Children.size() == 1, std::to_string(menu.size()));
            if (menu.size() == 1 && menu[0].Children.size() == 1)
                CHECK_EQ(menu[0].Children[0].Shortcut, std::string("Ctrl+Shift+F"));
            lua.MenuOpening(Lua::ScriptMenu::MemoryViewPopup);
            if (!popup.empty())
                lua.ClickMenuItem(popup[0].Id);
            if (menu.size() == 1 && menu[0].Children.size() == 1)
                lua.ClickMenuItem(menu[0].Children[0].Id);
            LuaCheck(engine, "script menu clicks", R"LUA(
                if menuOpened ~= 1 then return "popup " .. menuOpened end
                if menuClicks ~= 11 then return "clicks " .. menuClicks end
                local view = getMemoryViewForm()
                view.Menu.Items[view.Menu.Items.Count - 1].Caption = "Renamed"
                return "ok")LUA");
            CHECK(lua.MenuGeneration() != generation);
            const auto renamed = lua.MenuItems(Lua::ScriptMenu::MemoryViewMenu);
            CHECK(renamed.size() == 1 && renamed[0].Caption == "Renamed");

            const std::size_t before = lua.MenuItems(Lua::ScriptMenu::MainMenu).size();
            LuaCheck(engine, "main menu", R"LUA(
                local menu = MainForm.Menu
                mainOpened, mainClicks = 0, 0
                local root = createMenuItem(menu)
                root.Caption = "Unreal &Engine"
                root.OnClick = function() mainOpened = mainOpened + 1 end
                menu.Items.add(root)
                local entry = createMenuItem(menu)
                entry.Caption = "Inspector..."
                entry.Shortcut = "Ctrl+Alt+U"
                entry.OnClick = function() mainClicks = mainClicks + 1 end
                root.add(entry)
                local extra = createMenuItem(menu)
                extra.Caption = "Script help"
                MainForm.miHelp.add(extra)
                return "ok")LUA");
            const auto main = lua.MenuItems(Lua::ScriptMenu::MainMenu);
            CHECK_MSG(main.size() == before + 2, std::to_string(main.size()));
            const auto help = std::find_if(main.begin(), main.end(), [](const Lua::ScriptMenuItem& item) { return item.Merge == "help"; });
            CHECK(help != main.end() && help->Children.size() == 1 && help->Children[0].Caption == "Script help");
            const auto unreal = std::find_if(main.begin(), main.end(), [](const Lua::ScriptMenuItem& item) { return item.Caption == "Unreal Engine"; });
            CHECK(unreal != main.end() && unreal->Merge.empty() && unreal->Children.size() == 1);
            CHECK(std::none_of(main.begin(), main.end(), [](const Lua::ScriptMenuItem& item) { return item.Caption == "File" || item.Caption == "Table"; }));
            if (unreal != main.end() && unreal->Children.size() == 1)
            {
                CHECK_EQ(unreal->Children[0].Shortcut, std::string("Ctrl+Alt+U"));
                lua.ClickMenuItem(unreal->Id);
                lua.ClickMenuItem(unreal->Children[0].Id);
            }
            LuaCheck(engine, "main menu clicks", R"LUA(
                if mainOpened ~= 1 then return "opened " .. mainOpened end
                if mainClicks ~= 1 then return "clicks " .. mainClicks end
                return "ok")LUA");

            LuaCheck(engine, "list view", R"LUA(
                local form = createForm(false)
                form.Caption = "List view test"
                listForm = form
                local view = createListView(form)
                view.Align, view.ViewStyle, view.RowSelect, view.Checkboxes = alClient, "vsReport", true, true
                for _, column in ipairs({ { "Offset", 70 }, { "Name", 200 }, { "Type", 120 } }) do
                    local c = view.Columns.add()
                    c.Caption, c.Width = column[1], column[2]
                end
                view.Columns[2].Alignment = taRightJustify or 1
                for i = 1, 3 do
                    local item = view.Items.add()
                    item.Caption = string.format("%04X", i * 8)
                    item.SubItems.add("field" .. i)
                    item.SubItems.add("int32")
                end
                listView = view
                listEvents = {}
                view.OnSelectItem = function(sender, item, selected) listEvents[#listEvents + 1] = item.Caption .. (selected and "+" or "-") end
                view.OnColumnClick = function(sender, column) listEvents[#listEvents + 1] = "column " .. column.Caption end
                view.OnItemChecked = function(sender, item) listEvents[#listEvents + 1] = "checked " .. item.Caption .. tostring(item.Checked) end
                form.show()
                return "ok")LUA");
            const auto forms = lua.ShownForms();
            const auto form = std::find_if(forms.begin(), forms.end(), [](const Lua::ScriptForm& item) { return item.Caption == "List view test"; });
            CHECK(form != forms.end());
            if (form != forms.end())
            {
                CHECK(form->Unsupported.empty());
                CHECK_EQ(form->Controls.size(), 1u);
                if (form->Controls.size() == 1)
                {
                    const Lua::ScriptControl& view = form->Controls[0];
                    CHECK_EQ(view.Kind, std::string("listview"));
                    CHECK(view.Unsupported.empty() && view.ViewStyle == 3 && view.CheckBoxes);
                    CHECK(view.Columns.size() == 3 && view.Columns[1].Caption == "Name" && view.Columns[1].Width == 200 && view.Columns[2].Alignment == 1);
                    CHECK(view.Rows.size() == 3 && view.Rows[1].Text == "0010" && view.Rows[1].Sub == std::vector<std::string>({ "field2", "int32" }));
                    CHECK_EQ(view.ItemIndex, -1);
                    if (view.Rows.size() == 3)
                    {
                        lua.SetFormValue(view.Id, "itemindex", "0");
                        lua.FireFormEvent(view.Id, "itemselect", { -1 });
                        lua.SetFormValue(view.Id, "itemindex", "2");
                        lua.FireFormEvent(view.Id, "itemselect", { 0 });
                        lua.FireFormEvent(view.Id, "columnclick", { 1 });
                        lua.SetFormValue(view.Rows[1].Id, "checked", "1");
                        lua.FireFormEvent(view.Id, "itemcheck", { view.Rows[1].Id });
                    }
                }
            }
            LuaCheck(engine, "list view events", R"LUA(
                local text = table.concat(listEvents, ",")
                if text ~= "0008+,0008-,0018+,column Name,checked 0010true" then return text end
                if listView.Selected == nil or listView.Selected.Caption ~= "0018" or listView.ItemIndex ~= 2 then return "selected" end
                listView.Items.clear()
                listForm.destroy()
                return "ok")LUA");

            engine.TakeRequests();
            LuaCheck(engine, "memory view bridge", R"LUA(
                local view = getMemoryViewForm()
                view.DisassemblerView.SelectedAddress = 0x140001000
                if view.DisassemblerView.SelectedAddress ~= 0x140001000 then return "selected" end
                local hex = view.HexadecimalView
                hex.Address = 0x140002000
                hex.SelectionStart = 0x140002000
                hex.SelectionStop = 0x140002007
                if hex.SelectionStart ~= 0x140002000 or hex.SelectionStop ~= 0x140002007 then return "hex selection" end
                if hex.Address ~= 0x140002000 then return "hex address " .. tostring(hex.Address) end
                view.show()
                return "ok")LUA");
            const std::vector<UiRequest> requests = engine.TakeRequests();
            const auto has = [&](UiRequestKind kind, std::uint64_t address, std::uint64_t start, std::uint64_t stop)
                {
                    return std::any_of(requests.begin(), requests.end(), [&](const UiRequest& request)
                        { return request.Kind == kind && request.Address == address && request.Start == start && request.Stop == stop; });
                };
            CHECK(has(UiRequestKind::MemoryViewerSelect, 0x140001000, 0, 0));
            CHECK(has(UiRequestKind::MemoryViewerHex, 0x140002000, 0, 0));
            CHECK(has(UiRequestKind::MemoryViewerHex, 0, 0x140002000, 0x140002007));
            CHECK(has(UiRequestKind::MemoryViewerRaise, 0, 0, 0));
            CHECK(engine.SelectedCodeAddress == 0x140001000);
        }

        void ScriptForms(Engine& engine)
        {
            LuaCheck(engine, "script form", R"LUA(
                formPicks, formClosed = {}, false
                local form = createForm(false)
                form.Caption = "Hits"
                form.ClientWidth, form.ClientHeight = 300, 120
                local list = createListBox(form)
                list.Align = "alClient"
                list.Items.add("first")
                list.Items.add("second")
                list.OnClick = function(sender) formPicks[#formPicks + 1] = sender.ItemIndex end
                form.OnClose = function() formClosed = true return caFree end
                form.show()
                scriptForm, scriptList = form, list
                local odd = createForm(true)
                createTrackBar(odd)
                odd.Caption = "Odd"
                oddForm = odd
                return "ok")LUA");
            Lua::LuaEngine& lua = engine.Lua();
            CHECK(lua.FormsShown());
            const auto forms = lua.ShownForms();
            CHECK_EQ(forms.size(), static_cast<std::size_t>(2));
            const auto hits = std::find_if(forms.begin(), forms.end(), [](const Lua::ScriptForm& form) { return form.Caption == "Hits"; });
            const auto odd = std::find_if(forms.begin(), forms.end(), [](const Lua::ScriptForm& form) { return form.Caption == "Odd"; });
            CHECK(odd != forms.end() && odd->Unsupported == "TTrackBar");
            CHECK(hits != forms.end());
            if (hits == forms.end())
                return;
            CHECK(hits->Unsupported.empty() && hits->ClientWidth == 300 && hits->ClientHeight == 120);
            CHECK(hits->Controls.size() == 1);
            if (hits->Controls.size() != 1)
                return;
            const Lua::ScriptControl& list = hits->Controls[0];
            CHECK(list.Kind == "listbox" && list.X == 0 && list.Y == 0 && list.W == 300 && list.H == 120 && list.Items.size() == 2 && list.ItemIndex == -1);
            lua.SetFormValue(list.Id, "itemindex", "1");
            lua.FireFormEvent(list.Id, "select");
            lua.SetFormBounds(hits->Id, 10, 20, 340, 180, 320, 140);
            const auto after = lua.ShownForms();
            const auto again = std::find_if(after.begin(), after.end(), [](const Lua::ScriptForm& form) { return form.Caption == "Hits"; });
            CHECK(again != after.end() && !again->Controls.empty() && again->Controls[0].ItemIndex == 1);
            if (again != after.end() && !again->Controls.empty())
                CHECK(again->Controls[0].W == 320 && again->Controls[0].H == 140);
            lua.CloseForm(hits->Id);
            LuaCheck(engine, "script form events", R"LUA(
                if #formPicks ~= 1 or formPicks[1] ~= 1 then return "picks " .. #formPicks end
                if not formClosed then return "closed" end
                if scriptList.ItemIndex ~= 1 then return "index" end
                oddForm.close()
                return "ok")LUA");
            CHECK(lua.ShownForms().empty());
            CHECK(!lua.FormsShown());
        }

        void ScriptLayout(Engine& engine)
        {
            LuaCheck(engine, "lazarus layout", R"LUA(
                local form = createForm(false)
                form.ClientWidth, form.ClientHeight = 200, 200
                local function panel(parent, align, size)
                  local p = createPanel(parent)
                  p.Align = align
                  if align == "alTop" or align == "alBottom" then p.Height = size else p.Width = size end
                  p.BevelOuter = "bvNone"
                  return p
                end
                local a, b, c = panel(form, "alTop", 10), panel(form, "alTop", 20), panel(form, "alTop", 30)
                if c.Top ~= 0 or b.Top ~= 30 or a.Top ~= 50 then return string.format("alTop order %d %d %d", c.Top, b.Top, a.Top) end
                if a.Width ~= 200 then return "alTop width " .. a.Width end
                local body = panel(form, "alClient", 0)
                if body.Top ~= 60 or body.Height ~= 140 then return string.format("alClient %d %d", body.Top, body.Height) end
                local editor = panel(body, "alClient", 0)
                local splitter = createSplitter(body)
                splitter.Align = "alLeft"
                splitter.Width = 6
                local tree = panel(body, "alLeft", 80)
                if tree.Left ~= 0 or splitter.Left ~= 80 or editor.Left ~= 86 or editor.Width ~= 114 then
                  return string.format("splitter %d %d %d %d", tree.Left, splitter.Left, editor.Left, editor.Width)
                end
                local spaced = createPanel(form)
                spaced.BevelOuter = "bvNone"
                spaced.Align = "alBottom"
                spaced.Height = 20
                spaced.BorderSpacing.Around = 6
                if spaced.Left ~= 6 or spaced.Width ~= 188 or spaced.Top ~= 174 then return string.format("spacing %d %d %d", spaced.Left, spaced.Width, spaced.Top) end
                local framed = createPanel(form)
                framed.Align = "alNone"
                framed.Width, framed.Height = 50, 50
                local inner = createPanel(framed)
                inner.Align = "alClient"
                if inner.Left ~= 1 or inner.Width ~= 48 then return string.format("bevel inset %d %d", inner.Left, inner.Width) end
                local label = createLabel(spaced)
                label.Align = "alLeft"
                label.Caption = "Status text"
                if label.Width <= 20 then return "autosize " .. label.Width end
                local view = createTreeView(form)
                local root = view.Items:add("Root")
                local child = root:add("Child")
                local other = view.Items.add("Other")
                if root.Text ~= "Root" or other.Text ~= "Other" or child.Text ~= "Child" then return "node text" end
                if root.Count ~= 1 or child.Parent ~= root or view.Items.Count ~= 3 then return "node tree" end
                local one, two = createPanel(form), createPanel(form)
                if tostring(one) == tostring(two) or not tostring(one):match("^TPanel: %x+$") then return "tostring " .. tostring(one) end
                form.destroy()
                return "ok")LUA");
        }

        void PaintBoxCanvas(Engine& engine)
        {
            LuaCheck(engine, "paint box canvas", R"LUA(
                local form = createForm(false)
                form.ClientWidth, form.ClientHeight = 200, 100
                local box = createPaintBox(form)
                box.Align = "alClient"
                local bitmap = createBitmap(300, 200)
                local c = bitmap.Canvas
                c.Brush.Color = 0x101010
                c.fillRect(0, 0, 200, 100)
                c.Pen.Color = 0xFFFFFF
                c.line(0, 0, 10, 10)
                c.textOut(5, 5, "hi")
                c.ellipse(1, 1, 9, 9)
                local painted = 0
                box.OnPaint = function(sender)
                  painted = painted + 1
                  sender.Canvas.draw(0, 0, bitmap)
                end
                local moves = {}
                box.OnMouseMove = function(sender, x, y) moves[#moves + 1] = x .. "," .. y end
                form.show()
                local function surface()
                  for _, f in ipairs(__manifoldFormSnapshot()) do
                    for _, control in ipairs(f.controls) do
                      if control.kind == "paintbox" then return control end
                    end
                  end
                end
                local entry = surface()
                if not entry or entry.w ~= 200 or entry.h ~= 100 or not entry.mouse then return "entry" end
                if painted ~= 1 then return "painted " .. painted end
                local count, draws = __manifoldCanvasCount(entry.canvas)
                if count ~= 1 or #draws ~= 1 then return "paint box list " .. count end
                local inner = __manifoldCanvasCount(draws[1])
                if inner ~= 4 then return "bitmap list " .. inner end
                c.fillRect(0, 0, 200, 100)
                if __manifoldCanvasCount(draws[1]) ~= 1 then return "frame reset " .. __manifoldCanvasCount(draws[1]) end
                surface()
                if painted ~= 1 then return "painted without a repaint" end
                box.repaint()
                surface()
                if painted ~= 2 then return "repaint " .. painted end
                __manifoldFormFire(entry.id, "mousemove", 12, 34)
                if moves[1] ~= "12,34" then return "mouse move " .. tostring(moves[1]) end
                form.destroy()
                bitmap.destroy()
                return "ok")LUA");
        }

        void ScriptSettings(Engine& engine)
        {
            const std::filesystem::path path = Paths::ScriptSettingsFile();
            CHECK_MSG(Paths::DataFolder() != Paths::ProgramFolder() && path.parent_path().parent_path() == Paths::DataFolder(),
                "the tests would write the real " + WideToUtf8(path.wstring()));
            LuaCheck(engine, "getSettings", R"LUA(
                local settings = getSettings("Manifold Tests\\Compat")
                if settings.Value["missing"] ~= "" then return "missing" end
                settings.Value["Scope"] = "process"
                settings.Value.Lines = "a\nb"
                local again = getSettings("manifold tests\\compat")
                if again.Value["scope"] ~= "process" then return "read " .. tostring(again.Value["scope"]) end
                if again.Value.Lines ~= "a\nb" then return "lines" end
                return "ok")LUA");
        }

        void Commands(Engine& engine)
        {
            LuaCheck(engine, "register command", R"LUA(
                registerAutoAssemblerCommand("CompatAlloc", function(parameters, syntaxcheck)
                    return "alloc(" .. parameters .. ",$10)"
                end)
                return "ok")LUA");
            const Asm::AaResult result = engine.AutoAssemble("CompatAlloc(block)\nblock:\n  nop\n", true);
            CHECK_MSG(result.Ok(), result.Error.Text);
            LuaCheck(engine, "unregister command", R"LUA(
                unregisterAutoAssemblerCommand("CompatAlloc")
                return "ok")LUA");
            const Asm::AaResult missing = engine.AutoAssemble("CompatAlloc(block)\n", true);
            CHECK(!missing.Ok());
        }
    }

    namespace
    {
        void OutputBuffer(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            lua.ClearOutput();
            CHECK(lua.Run("for i = 1, 20000 do print('entry ' .. i .. '\\nsecond line\\nthird line') end", "output").Ok());
            CHECK_EQ(lua.Console().Count(), static_cast<std::size_t>(20000));
            CHECK_EQ(lua.Console().Dropped(), static_cast<std::uint64_t>(0));
            std::string first;
            std::string last;
            lua.Console().Visit(0, 1, [&](std::size_t, const Lua::ConsoleEntry& entry) { first = entry.Text; });
            lua.Console().Visit(lua.Console().Count() - 1, lua.Console().Count(), [&](std::size_t, const Lua::ConsoleEntry& entry) { last = entry.Text; });
            CHECK_EQ(first, std::string("entry 1\nsecond line\nthird line"));
            CHECK_EQ(last, std::string("entry 20000\nsecond line\nthird line"));
            CHECK(lua.Run("for i = 1, 90000 do print('more ' .. i) end", "overflow").Ok());
            CHECK_EQ(lua.Console().Count(), static_cast<std::size_t>(100000));
            CHECK_EQ(lua.Console().Dropped(), static_cast<std::uint64_t>(10000));
            lua.Console().Visit(2, 3, [&](std::size_t, const Lua::ConsoleEntry& entry) { first = entry.Text; });
            CHECK_EQ(first, std::string("entry 10003\nsecond line\nthird line"));
            lua.ClearOutput();
            CHECK_EQ(lua.Console().Count(), static_cast<std::size_t>(0));
            CHECK_EQ(lua.Console().Dropped(), static_cast<std::uint64_t>(0));
        }

        bool IsMethodName(std::string_view name)
        {
            for (const char* method : Lua::CheatEngineMethodNames())
            {
                if (name == method)
                    return true;
            }
            return false;
        }

        std::size_t Warnings(Engine& engine, const std::string& text)
        {
            std::size_t count = 0;
            for (const Lua::ConsoleEntry& entry : engine.Lua().Console().Entries())
            {
                if (entry.Level == LogLevel::Warning && entry.Text.find(text) != std::string::npos)
                    ++count;
            }
            return count;
        }

        void CheatEngineCoverage(Engine& engine)
        {
            lua_State* state = engine.Lua().State();
            std::set<std::string> documented;
            std::string missing;
            for (const Lua::DocEntry& entry : Lua::DocEntries())
            {
                const std::string name = entry.Name;
                if (entry.Kind != Lua::DocKind::Function || entry.Owner[0] != '\0' || name.find_first_of("[]. ") != std::string::npos || IsMethodName(name))
                    continue;
                if (!documented.insert(name).second)
                    continue;
                if (lua_getglobal(state, name.c_str()) != LUA_TFUNCTION)
                    missing += (missing.empty() ? "" : ", ") + name;
                lua_pop(state, 1);
            }
            CHECK_MSG(missing.empty(), "documented Cheat Engine functions that are nil: " + missing);
            CHECK(documented.size() > 450);

            std::set<std::string> seen;
            for (const Lua::CompatGap& gap : Lua::CompatGaps())
            {
                const std::string name = gap.Name;
                CHECK_MSG(seen.insert(name).second, "listed twice: " + name);
                CHECK_MSG(documented.count(name) == 1, "not a documented Cheat Engine function: " + name);
                CHECK_MSG(gap.Reason && gap.Reason->Title && gap.Reason->Text, "no reason for " + name);
                lua_pushglobaltable(state);
                lua_pushstring(state, gap.Name);
                const int raw = lua_rawget(state, -2);
                lua_pop(state, 2);
                if (gap.Kind == Lua::GapKind::Unavailable)
                    CHECK_MSG(raw == LUA_TNIL, name + " is listed as not available but is a real global now: remove it from the gap table");
                else
                    CHECK_MSG(raw == LUA_TFUNCTION, name + " is listed as having no effect but is not defined");
                const Lua::DocPtr record = engine.Lua().Docs().Find(name);
                CHECK_MSG(record && record->Support == (gap.Kind == Lua::GapKind::Unavailable ? Lua::DocSupport::Missing : Lua::DocSupport::NoEffect), "documentation does not mark " + name);
            }
            const Lua::DocPtr implemented = engine.Lua().Docs().Find("setMousePos");
            CHECK(implemented && implemented->Support == Lua::DocSupport::Full);
            for (const char* method : Lua::CheatEngineMethodNames())
                CHECK_MSG(documented.count(method) == 0, std::string("method name filtered wrongly: ") + method);

            const std::size_t before = Warnings(engine, "dbk_initialize is not available");
            LuaCheck(engine, "unavailable functions", R"LUA(
                if rawget(_G, "dbk_initialize") ~= nil then return "stub stored as a global" end
                if dbk_initialize() ~= false or Dbk_initialize() ~= false then return "dbk_initialize result" end
                if dbk_getCR3() ~= nil then return "nil result" end
                if select("#", dbk_writeMSR(1, 2)) ~= 0 then return "no result" end
                local id = registerAddressLookupCallback(function() end)
                if math.type(id) ~= "integer" then return "register id" end
                unregisterAddressLookupCallback(id)
                if type(getHandleList()) ~= "table" then return "empty table result" end
                if debug.getinfo(dbk_initialize).what ~= "C" then return "stub is not a C function" end
                if not rawequal(dbk_initialize, dbk_initialize) then return "stub not cached" end
                if noSuchCheatEngineFunction ~= nil then return "unknown names resolve" end
                local kind, reason = __manifoldCompatGap("compile")
                if kind ~= "unavailable" or not reason:find("compiler") then return "gap info" end
                return "ok")LUA");
            CHECK_EQ(Warnings(engine, "dbk_initialize is not available") - before, std::size_t(1));
            CHECK_EQ(Warnings(engine, "unregisterAddressLookupCallback"), std::size_t(0));
            CHECK_EQ(Warnings(engine, "registerAddressLookupCallback is not available"), std::size_t(1));

            LuaCheck(engine, "functions without effect", R"LUA(
                if waitForExports() ~= true or waitforsymbols(true) ~= true then return "wait functions" end
                setProgressState(tbpsNormal)
                hideAllCEWindows()
                unhideMainCEwindow()
                if debug_isStepping() ~= false then return "stepping" end
                local form = getSettingsForm()
                if form ~= getSettingsForm() or form.ClassName ~= "TForm" then return "settings form" end
                local _ = form.SomeCheckBox.Checked
                if getFreezeTimer().Interval <= 0 or getUpdateTimer().Interval <= 0 then return "timers" end
                local owner = createForm(false)
                if createCalendar(owner).ClassName ~= "TCalendar" or createDiagram(owner).Parent ~= owner then return "placeholders" end
                createColorBox(owner)
                createVirtualStringTree(owner)
                createRegion()
                owner.destroy()
                return "ok")LUA");

            LuaCheck(engine, "cheat engine system functions", R"LUA(
                if signExtend(0xFF, 7) ~= -1 or signExtend(0x7F, 7) ~= 0x7F or signExtend(0x8000, 15) ~= -0x8000 then return "signExtend" end
                local one = extendedToByteTable(1.0)
                if #one ~= 10 or one[8] ~= 0x80 or one[9] ~= 0xFF or one[10] ~= 0x3F then return "extendedToByteTable 1.0" end
                for _, value in ipairs({ 0.0, -1234.5, 3.25e-12, 1e300, 0.1 }) do
                  if byteTableToExtended(extendedToByteTable(value)) ~= value then return "extended round trip " .. value end
                end
                if utf8ToAnsi("abc") ~= "abc" or ansiToUtf8("abc") ~= "abc" or ansiToUtf8(utf8ToAnsi("Gr\u{00FC}\u{00DF}e")) ~= "Gr\u{00FC}\u{00DF}e" then return "ansi" end
                if invertColor(0) ~= 0xFFFFFF or invertColor(0x00FF00) ~= 0xFF00FF then return "invertColor" end
                if getOperatingSystem() ~= 0 or not targetIsX86() or targetIsArm() or targetIsAndroid() then return "platform" end
                if getPointerSize() ~= 8 or getOpenedProcessHandle() ~= 0 then return "no process" end
                local raw, version = getCheatEngineFileVersion()
                if version.major ~= 7 or version.minor ~= 5 or raw >> 48 ~= 7 then return "file version" end
                if cpuid(0).EAX < 1 or cpuid(0).eax ~= cpuid(0).EAX then return "cpuid" end
                if getCPUCount() < 1 then return "cpu count" end
                if getSystemMetrics(0) ~= getScreenWidth() then return "system metrics" end
                if getUserRegistryEnvironmentVariable("ManifoldEngineNoSuchVariable") ~= nil then return "environment" end
                if isConnectedToCEServer() ~= false or speedhack_getSpeed() ~= 1 or debug_canBreak() ~= false or debug_getCurrentDebuggerInterface() ~= nil then return "fixed answers" end
                if debug_getMaxLastBranchRecord() ~= -1 then return "branch records" end
                if readShortInteger(0x10000) ~= nil or getRTTIClassName(0x10000) ~= nil then return "reads without process" end
                if translateID("id", "text") ~= "text" or translateID("id") ~= "id" then return "translateID" end

                local path = getTempFolder() .. "manifold-compat-" .. getCheatEngineProcessID() .. ".txt"
                local file = io.open(path, "wb")
                file:write("x")
                file:close()
                if not fileExists(path) then return "fileExists" end
                if deleteFile(path) ~= true or fileExists(path) or deleteFile(path) ~= false then return "deleteFile" end

                local ref = createRef("a", nil, 3)
                local a, b, c = getRef(ref)
                if a ~= "a" or b ~= nil or c ~= 3 then return "refs" end
                destroyRef(ref)
                if getRef(ref) ~= nil then return "destroyRef" end
                setGlobalVariable("ManifoldCompatGlobal", 5)
                if getGlobalVariable("ManifoldCompatGlobal") ~= 5 or ManifoldCompatGlobal ~= 5 then return "global variables" end
                ManifoldCompatGlobal = nil

                if getNextAllocNumber("alloc(newmem,2048)\nalloc(newmem2,$1000)") ~= 3 or getNextAllocNumber("") ~= 0 then return "alloc number" end
                local script = createStringlist()
                script.Text = "ALLOC(newmem5,8)"
                if getNextAllocNumber(script) ~= 6 then return "alloc number from strings" end

                if convertKeyComboToString(VK_CONTROL, VK_F1) ~= "Ctrl+F1" or convertKeyComboToString({ VK_SHIFT, VK_F2 }) ~= "Shift+F2" then return "key combo" end
                if textToShortCut("Ctrl+Shift+F5") ~= (0x4000 | 0x2000 | 0x74) or textToShortCut("alt+a") ~= (0x8000 | 0x41) then return "textToShortCut" end
                if shortCutToText(0x4000 | 0x41) ~= "Ctrl+A" or shortCutToText(textToShortCut("Shift+Del")) ~= "Shift+Del" then return "shortCutToText" end

                local nop = assemble("nop")
                if type(nop) ~= "table" or #nop ~= 1 or nop[1] ~= 0x90 then return "assemble nop" end
                if #assemble("mov eax,1") ~= 5 or assemble("not an instruction") ~= nil then return "assemble" end
                setAssemblerMode(0)
                local push = assemble("push ebp")
                setAssemblerMode(1)
                if #push ~= 1 or push[1] ~= 0x55 then return "32-bit assembler mode" end

                local source = allocateSharedMemoryLocal("ManifoldCompatShared" .. getCheatEngineProcessID(), 64)
                if not source then return "shared memory" end
                writeBytesLocal(source, 1, 2, 3, 4)
                local copy = copyMemory(source, 4, nil, 3)
                local bytes = readBytesLocal(copy, 4, true)
                if bytes[4] ~= 4 or compareMemory(source, copy, 4, 2) ~= true then return "copyMemory" end
                writeBytesLocal(copy + 2, 9)
                local same, index = compareMemory(source, copy, 4, 2)
                if same ~= false or index ~= 2 then return "compareMemory" end

                setComment(0x1000, "comment")
                setHeader(0x1000, "header")
                if getComment(0x1000) ~= "comment" or getHeader(0x1000) ~= "header" then return "comments" end
                setComment(0x1000, "")
                setHeader(0x1000, "")
                if getComment(0x1000) ~= "" then return "clear comment" end

                if errorOnLookupFailure(false) ~= true or getAddress("no.such.symbol+1") ~= 0 then return "lookup without errors" end
                if errorOnLookupFailure(true) ~= false or pcall(getAddress, "no.such.symbol+1") then return "lookup errors" end
                local _, lookupError = pcall(getAddress, "no.such.symbol+1")
                if lookupError ~= "Failure determining what no.such.symbol+1 means" then return "lookup error text: " .. tostring(lookupError) end
                if getAddressSafe("no.such.symbol") ~= nil then return "getAddressSafe" end

                registerSymbol("ManifoldCompatSymbol", 0x1234)
                local found = false
                for _, symbol in ipairs(enumRegisteredSymbols()) do
                  found = found or (symbol.symbolname == "ManifoldCompatSymbol" and symbol.address == 0x1234)
                end
                local info = getSymbolInfo("ManifoldCompatSymbol")
                unregisterSymbol("ManifoldCompatSymbol")
                if not found or not info or info.address == nil or info.address ~= 0x1234 or info.searchkey ~= "ManifoldCompatSymbol" then return "symbols" end

                if #getThreadlist() ~= 0 then return "thread list without process" end
                if type(getWindowlist()) ~= "table" then return "window list" end
                local processes = createStringlist()
                getProcesslist(processes)
                if processes.Count < 1 or not processes[0]:match("^%x%x%x%x%x%x%x%x%-") then return "process list strings" end
                local output, code = runCommand("cmd.exe", { "/c", "echo", "manifold" })
                if code ~= 0 or not tostring(output):find("manifold") then return "runCommand" end
                return "ok")LUA");

            LuaCheck(engine, "cheat engine objects", R"LUA(
                local section = createCriticalSection()
                section.enter()
                if not section.tryEnter() then return "reentrant critical section" end
                section.leave()
                section:leave()
                local event = createEvent(false, false)
                if event.waitFor(0) ~= wrTimeout then return "unsignalled event" end
                event.setEvent()
                if event.waitFor(0) ~= wrSignaled or event.waitFor(0) ~= wrTimeout then return "auto reset event" end
                local manual = createEvent(true, true)
                if manual.waitFor(0) ~= wrSignaled or manual.waitFor(0) ~= wrSignaled then return "manual reset event" end
                local semaphore = createSemaphore(1)
                semaphore.acquire()
                semaphore.release()
                local sync = createMultiReadExclusiveWriteSynchronizer()
                sync.beginRead()
                sync.endRead()
                if sync.beginWrite() ~= true then return "beginWrite" end
                sync.endWrite()
                local disassembler = createDisassembler()
                if type(disassembler.disassemble) ~= "function" or getVisibleDisassembler() ~= getVisibleDisassembler() then return "disassembler" end
                if type(disassembler.getLastDisassembleData()) ~= "table" then return "last disassemble data" end
                local list = createClass("TStringList")
                if list.ClassName ~= "TStringList" then return "createClass" end
                local form = createForm(false)
                local button = createComponentClass("TButton", form)
                if button.ClassName ~= "TButton" or button.Owner ~= form or button.Parent ~= form then return "createComponentClass" end
                form.destroy()
                if createClass("TNoSuchClass") ~= nil then return "unknown class" end
                local properties = getPropertyList("TLabel")
                if properties.Count < 5 or properties.IndexOf("caption") < 0 then return "property list" end
                local attach = getAutoAttachList()
                if attach ~= getAutoAttachList() then return "auto attach list" end
                attach.add("manifold-no-such-process.exe")
                attach.clear()

                local ran, args = false, nil
                local thread = createThreadSuspended(function(self, a, b) ran = true args = (a or 0) + (b or 0) end, 2, 3)
                local started = getTickCount()
                repeat processMessages() until getTickCount() - started > 60
                if ran then return "suspended thread ran before resume" end
                thread.resume()
                started = getTickCount()
                repeat processMessages() until ran or getTickCount() - started > 3000
                if not ran or args ~= 5 then return "suspended thread after resume" end
                local fresh = false
                ManifoldCompatFlag = nil
                createThreadNewState("ManifoldCompatFlag = 7")
                started = getTickCount()
                repeat processMessages() until ManifoldCompatFlag or getTickCount() - started > 3000
                if ManifoldCompatFlag ~= 7 then return "new state thread" end
                ManifoldCompatFlag = nil
                local event2 = createEvent(false, false)
                local waited
                createThread(function() waited = event2.waitFor(2000) end)
                createTimer(30, function() event2.setEvent() end)
                started = getTickCount()
                repeat processMessages() until waited ~= nil or getTickCount() - started > 3000
                if waited ~= wrSignaled then return "thread waits for an event: " .. tostring(waited) end
                return "ok")LUA");

            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "Lua" / "LuaCompatGaps.cpp"); ++i)
                at = at.parent_path();
            const std::filesystem::path docs = at / "Docs" / "CE-COMPAT.md";
            if (std::filesystem::exists(docs))
            {
                std::ifstream file(docs, std::ios::binary);
                std::stringstream content;
                content << file.rdbuf();
                std::string written = content.str();
                written.erase(std::remove(written.begin(), written.end(), '\r'), written.end());
                CHECK_MSG(written == Lua::CompatGapsMarkdown(), "Docs/CE-COMPAT.md is out of date: run the tests with --ce-compat-markdown");
            }
            else
                Skip("Docs/CE-COMPAT.md not found above " + std::filesystem::current_path().string());
        }

        void WaitingLoops(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            std::string result;
            const Status pumped = lua.RunWithResult(
                "local done = false\n"
                "local t = createTimer(nil, false)\n"
                "t.Interval = 1\n"
                "t.OnTimer = function(sender) done = true sender.destroy() end\n"
                "t.Enabled = true\n"
                "local started = getTickCount()\n"
                "repeat processMessages() until done or getTickCount() - started > 3000\n"
                "local viaApplication = false\n"
                "local u = createTimer(nil, false)\n"
                "u.Interval = 1\n"
                "u.OnTimer = function(sender) viaApplication = true sender.destroy() end\n"
                "u.Enabled = true\n"
                "started = getTickCount()\n"
                "repeat getApplication().processMessages() until viaApplication or getTickCount() - started > 3000\n"
                "started = getTickCount()\n"
                "sleep(60)\n"
                "return tostring(done) .. ' ' .. tostring(viaApplication) .. ' ' .. tostring(getTickCount() - started >= 45)",
                "waiting", result);
            CHECK_MSG(pumped.Ok(), pumped.Text);
            CHECK_EQ(result, std::string("true true true"));
        }

        bool HasCompletion(const std::vector<Lua::LuaCompletion>& list, const std::string& name, const std::string& signature = {})
        {
            return std::any_of(list.begin(), list.end(), [&](const Lua::LuaCompletion& item) { return item.Name == name && (signature.empty() || item.Signature == signature); });
        }

        void Assist(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            CHECK(HasCompletion(lua.Complete("", "readInt", false), "readInteger", "(address)"));
            const auto capital = lua.Complete("", "AutoAss", false);
            CHECK(HasCompletion(capital, "AutoAssemble") && !HasCompletion(capital, "autoAssemble"));
            const auto lower = lua.Complete("", "autoAss", false);
            CHECK(HasCompletion(lower, "autoAssemble") && !HasCompletion(lower, "AutoAssemble"));
            CHECK(HasCompletion(lua.Complete("", "whi", false), "while"));
            CHECK(lua.Complete("", "__manifold", false).empty());
            CHECK(HasCompletion(lua.Complete("string", "for", false), "format", "(format, ...)"));
            CHECK(lua.Run("AssistProbe = { Count = 3, Nested = { Deep = true }, Run = function(self, first, second) end }", "assist").Ok());
            CHECK(HasCompletion(lua.Complete("AssistProbe", "", false), "Count"));
            CHECK(HasCompletion(lua.Complete("AssistProbe", "R", false), "Run", "(self, first, second)"));
            CHECK(HasCompletion(lua.Complete("AssistProbe", "", true), "Run", "(first, second)"));
            CHECK(!HasCompletion(lua.Complete("AssistProbe", "", true), "Count"));
            CHECK(HasCompletion(lua.Complete("AssistProbe.Nested", "", false), "Deep"));
            CHECK(HasCompletion(lua.Complete("AddressList", "getMemoryRecordBy", true), "getMemoryRecordByDescription"));
            CHECK(HasCompletion(lua.Complete("MainForm", "Capt", false), "Caption"));
            CHECK_EQ(lua.CallSignatures("messageDialog", false).size(), static_cast<std::size_t>(3));
            const auto run = lua.CallSignatures("AssistProbe.Run", true);
            CHECK(run.size() == 1 && run.front() == "Run(first, second)");
            const auto format = lua.CallSignatures("string.format", false);
            CHECK(format.size() == 1 && format.front() == "format(format, ...)");
            CHECK(lua.CallSignatures("NoSuchThing.Run", false).empty());
            CHECK(!Lua::LuaEngine::DocumentedSignatures("readInteger").empty());
        }

        void CeHabits(Engine& engine)
        {
            LuaCheck(engine, "capital aliases", R"LUA(
                local function native(f) return debug.getinfo(f).what == "C" end
                if AutoAssemble ~= autoAssemble or RegisterSymbol ~= registerSymbol or GetAddressSafe ~= getAddressSafe then return "same function" end
                if not native(AutoAssemble) or not native(RegisterAutoAssemblerCommand) then return "native" end
                if ShowMessage ~= showMessage or GetMainForm ~= getMainForm or CreatePanel ~= createPanel then return "compat functions" end
                if Pairs ~= nil or Type ~= nil or Tostring ~= nil then return "standard library aliased" end
                local ok = pcall(debug.getinfo, nil)
                if ok then return "getinfo(nil) must still fail" end
                return "ok")LUA");
            LuaCheck(engine, "custom type object", R"LUA(
                local ct = registerCustomTypeLua("CompatSized", 0, function(...) return "x" end, function(text, address) return { 0 } end)
                ct.scriptUsesString = true
                if not ct.scriptUsesString or ct.byteSize ~= 0 then return "flags or zero size" end
                local object = readQwordLocal(tostring(ct):match "0%x*")
                if not object or object == 0 then return "object pointer" end
                writeIntegerLocal(object + 0x54, 16)
                if ct.byteSize ~= 16 or getCustomType("CompatSized").byteSize ~= 16 then return "size through the object: " .. tostring(ct.byteSize) end
                local again = registerCustomTypeLua("CompatSized", 4, function(...) return "y" end, function(text, address) return { 0 } end)
                if readQwordLocal(tostring(again):match "0%x*") ~= object then return "object kept on re-register" end
                if ct.byteSize ~= 4 then return "size after re-register" end
                if pcall(function() ct.unknownProperty = 1 end) then return "unknown property accepted" end
                return "ok")LUA");
            LuaCheck(engine, "window and process helpers", R"LUA(
                if type(getCheatEngineProcessID()) ~= "number" or getCheatEngineProcessID() == 0 then return "own pid" end
                if findWindow("Manifold.NoSuchWindowClass") ~= 0 then return "findWindow" end
                if type(getForegroundWindow()) ~= "number" then return "foreground" end
                if getWindowCaption(0) ~= "" or getWindowProcessID(0) ~= 0 or getWindow(0, GW_OWNER) ~= 0 then return "null window" end
                if detachIfPossible() ~= false then return "detach without debugger" end
                local captured
                local original = print
                print = function(text) captured = text end
                printf("%d-%s", 5, "x")
                print = original
                if captured ~= "5-x" then return "printf gave " .. tostring(captured) end
                if type(MainForm.OnProcessOpened) ~= "function" then return "OnProcessOpened default" end
                return "ok")LUA");
            LuaCheck(engine, "local calls by name", R"LUA(
                if executeCodeLocalEx("kernel32.GetCurrentProcessId") ~= getCheatEngineProcessID() then return "module.export" end
                if executeCodeLocalEx("kernel32!GetCurrentProcessId") ~= getCheatEngineProcessID() then return "module!export" end
                if executeCodeLocalEx("kernel32.MultiByteToWideChar", 65001, 0, "Hallo", -1, 0, 0) ~= 6 then return "string argument" end
                if executeCodeLocalEx("kernel32.MultiByteToWideChar", 65001, 0, "", -1, 0, 0) ~= 1 then return "empty string is a pointer" end
                local buffer = executeCodeLocalEx("kernel32.LocalAlloc", 0, 16)
                if not buffer or buffer == 0 then return "LocalAlloc" end
                if executeCodeLocalEx("kernel32.LocalFree", buffer) ~= 0 then return "LocalFree" end
                if pcall(executeCodeLocalEx, "kernel32.NoSuchFunction") then return "unknown export accepted" end
                if getProcessIDFromProcessName("Manifold-Engine.Tests") == nil then return "process name prefix" end
                return "ok")LUA");
        }
    }

    int RunCompatTests()
    {
        const int before = Totals().Failures;
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        OutputBuffer(engine);
        WaitingLoops(engine);
        CeHabits(engine);
        Assist(engine);
        Extensions(engine);
        Records(engine);
        Streams(engine);
        Forms(engine);
        CustomTypes(engine);
        StructureCallbacks(engine);
        LaterCallbacks(engine);
        CallbackLimit(engine);
        VisibleRefresh(engine);
        SymbolIndex(engine);
        SymbolCallbacks(engine);
        Commands(engine);
        WithoutProcess(engine);
        SettingsRoundTrip();
        ScriptMenus(engine);
        ScriptForms(engine);
        ScriptLayout(engine);
        PaintBoxCanvas(engine);
        ScriptSettings(engine);
        CheatEngineCoverage(engine);
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("compat", "Compatibility", 80, [](const SuiteContext&) { return RunCompatTests(); });

    MANIFOLD_MODE("--ce-compat-markdown", [](const std::vector<std::string>&)
        {
            std::fputs(Lua::CompatGapsMarkdown().c_str(), stdout);
            return 0;
        });
}
