#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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
                createListView(odd)
                odd.Caption = "Odd"
                oddForm = odd
                return "ok")LUA");
            Lua::LuaEngine& lua = engine.Lua();
            CHECK(lua.FormsShown());
            const auto forms = lua.ShownForms();
            CHECK_EQ(forms.size(), static_cast<std::size_t>(2));
            const auto hits = std::find_if(forms.begin(), forms.end(), [](const Lua::ScriptForm& form) { return form.Caption == "Hits"; });
            const auto odd = std::find_if(forms.begin(), forms.end(), [](const Lua::ScriptForm& form) { return form.Caption == "Odd"; });
            CHECK(odd != forms.end() && odd->Unsupported == "TListView");
            CHECK(hits != forms.end());
            if (hits == forms.end())
                return;
            CHECK(hits->Unsupported.empty() && hits->Width == 300 && hits->Height == 120);
            CHECK(hits->Controls.size() == 1);
            if (hits->Controls.size() != 1)
                return;
            const Lua::ScriptControl& list = hits->Controls[0];
            CHECK(list.Kind == "listbox" && list.Align == 5 && list.Items.size() == 2 && list.ItemIndex == -1);
            lua.SetFormValue(list.Id, "itemindex", "1");
            lua.FireFormEvent(list.Id, "select");
            lua.SetFormBounds(hits->Id, 10, 20, 320, 140);
            const auto after = lua.ShownForms();
            const auto again = std::find_if(after.begin(), after.end(), [](const Lua::ScriptForm& form) { return form.Caption == "Hits"; });
            CHECK(again != after.end() && !again->Controls.empty() && again->Controls[0].ItemIndex == 1);
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

        void ScriptSettings(Engine& engine)
        {
            const std::filesystem::path path = Paths::ScriptSettingsFile();
            std::error_code error;
            const bool existed = std::filesystem::exists(path, error);
            std::string original;
            if (existed)
            {
                std::ifstream file(path, std::ios::binary);
                original.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            LuaCheck(engine, "getSettings", R"LUA(
                local settings = getSettings("Manifold Tests\\Compat")
                if settings.Value["missing"] ~= "" then return "missing" end
                settings.Value["Scope"] = "process"
                settings.Value.Lines = "a\nb"
                local again = getSettings("manifold tests\\compat")
                if again.Value["scope"] ~= "process" then return "read " .. tostring(again.Value["scope"]) end
                if again.Value.Lines ~= "a\nb" then return "lines" end
                return "ok")LUA");
            if (existed)
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(original.data(), static_cast<std::streamsize>(original.size()));
            }
            else
            {
                std::filesystem::remove(path, error);
            }
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
            CHECK_EQ(lua.OutputCount(), static_cast<std::size_t>(60000));
            CHECK_EQ(lua.OutputDropped(), static_cast<std::uint64_t>(0));
            std::string first;
            std::string last;
            lua.VisitOutput(0, 1, [&](std::size_t, const Lua::OutputLine& line) { first = line.Text; });
            lua.VisitOutput(lua.OutputCount() - 1, lua.OutputCount(), [&](std::size_t, const Lua::OutputLine& line) { last = line.Text; });
            CHECK_EQ(first, std::string("entry 1"));
            CHECK_EQ(last, std::string("third line"));
            CHECK(lua.Run("for i = 1, 50000 do print('more ' .. i) end", "overflow").Ok());
            CHECK_EQ(lua.OutputCount(), static_cast<std::size_t>(100000));
            CHECK_EQ(lua.OutputDropped(), static_cast<std::uint64_t>(10000));
            lua.VisitOutput(2, 3, [&](std::size_t, const Lua::OutputLine& line) { first = line.Text; });
            CHECK_EQ(first, std::string("entry 3335"));
            lua.ClearOutput();
            CHECK_EQ(lua.OutputCount(), static_cast<std::size_t>(0));
            CHECK_EQ(lua.OutputDropped(), static_cast<std::uint64_t>(0));
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
        CeHabits(engine);
        Assist(engine);
        Extensions(engine);
        Records(engine);
        Streams(engine);
        Forms(engine);
        CustomTypes(engine);
        StructureCallbacks(engine);
        SymbolCallbacks(engine);
        Commands(engine);
        WithoutProcess(engine);
        SettingsRoundTrip();
        ScriptMenus(engine);
        ScriptForms(engine);
        ScriptSettings(engine);
        return Totals().Failures - before;
    }
}
