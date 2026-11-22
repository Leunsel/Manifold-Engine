#include "TestKit.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/DocRegistry.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaEngine.h"
#include "Core/Deflate.h"
#include "Lua/UiForm.h"
#include "Lua/UiFormImport.h"
#include "Table/CeForms.h"
#include "Table/CheatTable.h"
#include "Lua/UiModel.h"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        volatile std::int32_t g_probeValue = 0x12345678;
        volatile float g_probeFloat = 2.5f;
        volatile std::uint8_t g_probeBytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        volatile char g_probeText[16] = "manifold";
        volatile std::uint8_t g_probeScan[8] = { 0x4D, 0x41, 0x4E, 0x49, 0xF0, 0x1D, 0x5C, 0xA9 };

        void LuaCheck(Engine& engine, const char* name, const std::string& code)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(code, name, result);
            CHECK_MSG(status.Ok() && result == "ok", std::string(name) + ": " + (status.Ok() ? result : status.Text));
        }

        std::optional<Lua::ConsoleEntry> LastEntry(Engine& engine)
        {
            return engine.Lua().Console().Last();
        }

        std::size_t CountEntries(Engine& engine, LogLevel level, std::string_view text)
        {
            std::size_t count = 0;
            for (const Lua::ConsoleEntry& entry : engine.Lua().Console().Entries())
            {
                if (entry.Level == level && entry.PlainText().find(text) != std::string::npos)
                    count += entry.Repeat;
            }
            return count;
        }

        void Schema(Engine& engine)
        {
            LuaCheck(engine, "schema names", R"LUA(
                if manifold.apiVersion ~= 20 or type(manifold.version) ~= "string" then return "constants" end
                local count = 0
                local function lint(path, members, nested)
                  local functions = {}
                  for name, value in pairs(members) do
                    if type(value) == "table" and not nested then
                      if not name:match("^%l+$") then return "sub-area name " .. path .. "." .. name end
                      local problem = lint(path .. "." .. name, value, true)
                      if problem then return problem end
                    else
                      if type(value) ~= "function" then return "non-function " .. path .. "." .. name end
                      if not name:match("^%l%w*$") then return "function name " .. path .. "." .. name end
                      local doc = manifold.doc.entry(value)
                      local returns = doc and doc.signatures and doc.signatures[1] and doc.signatures[1].returns
                      if not doc or doc.origin ~= "manifold" or doc.text == "" or returns == nil then return "undocumented " .. path .. "." .. name end
                      if doc.path ~= "manifold." .. path .. "." .. name then return "documented as " .. doc.path end
                      if (name:match("^is%u") or name:match("^has%u")) and returns ~= "boolean" then return "predicate " .. name .. " returns " .. returns end
                      functions[name] = true
                      count = count + 1
                    end
                  end
                  for name in pairs(functions) do
                    local setting = name:match("^set(%u%w*)$")
                    if setting and not functions["get" .. setting] then return "set without get: " .. path .. "." .. name end
                    local getter = name:match("^get(%u%w*)$")
                    if getter and not functions["set" .. getter] then return "get without set: " .. path .. "." .. name end
                  end
                end
                for areaName, members in pairs(manifold) do
                  if type(members) == "table" then
                    if not areaName:match("^%l+$") then return "area name " .. areaName end
                    local problem = lint(areaName, members, false)
                    if problem then return problem end
                  elseif type(members) == "function" then
                    if areaName ~= "has" then return "top-level function " .. areaName end
                  elseif not areaName:match("^%l%w*$") then
                    return "constant name " .. areaName
                  end
                end
                if count < 25 then return "only " .. count .. " functions" end
                return "ok")LUA");
            LuaCheck(engine, "schema strict", R"LUA(
                local ok, problem = pcall(function() return manifold.log.warn end)
                if ok or not manifold.error.is(problem) then return "unknown name did not raise an error object" end
                if problem.code ~= "ME-7007" then return "code " .. tostring(problem.code) end
                if not problem.message:find("manifold.log.warning", 1, true) then return "no suggestion: " .. problem.message end
                if problem.where ~= "schema strict:2" then return "where " .. tostring(problem.where) end
                ok, problem = pcall(function() return manifold.Log end)
                if ok or not problem.message:find("manifold.log", 1, true) then return "case suggestion" end
                ok = pcall(function() manifold.log.extra = 1 end)
                if ok then return "assignment allowed" end
                ok = pcall(function() manifold.memory = {} end)
                if ok then return "area assignment allowed" end
                if getmetatable(manifold) ~= false then return "metatable visible" end
                if tostring(manifold.process) ~= "manifold.process" then return "tostring " .. tostring(manifold.process) end
                if not manifold.has("log.info") or manifold.has("log.warn") or not manifold.has("memory") then return "has" end
                if not manifold.has("manifold.process.info") or not manifold.has("apiVersion") or not manifold.has("has") then return "has prefix" end
                return "ok")LUA");
            LuaCheck(engine, "ui mode", R"LUA(
                local before = manifold.ui.getMode()
                if type(before.release) ~= "boolean" or type(before.compact) ~= "boolean" then return "getMode" end
                manifold.ui.setMode{ compact = not before.compact }
                local after = manifold.ui.getMode()
                if after.compact == before.compact or after.release ~= before.release then return "setMode compact" end
                manifold.ui.setMode{ release = true }
                if not manifold.ui.getMode().release then return "setMode release" end
                manifold.ui.setMode{ release = before.release, compact = before.compact }
                local back = manifold.ui.getMode()
                if back.release ~= before.release or back.compact ~= before.compact then return "setMode back" end
                local ok = pcall(manifold.ui.setMode, { compact = "yes" })
                if ok then return "a string was taken for a boolean" end
                return "ok")LUA");
            LuaCheck(engine, "schema calls", R"LUA(
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  if not manifold.error.is(problem) or problem.code ~= "ME-7007" then return "not a misuse error: " .. tostring(problem) end
                  if not problem.message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. problem.message end
                end
                local failure = raises(function() manifold.log:info("x") end, "with a dot, not a colon")
                  or raises(function() manifold.process:isOpen() end, "with a dot")
                  or raises(function() manifold.process.isOpen(1) end, "takes 0 arguments, got 1")
                  or raises(function() manifold.log.info() end, "argument 1 (message) is missing")
                  or raises(function() manifold.memory.read(1, 5) end, "argument 2 (type) must be a string, got number")
                  or raises(function() manifold.memory.read(1, "i33") end, 'must be one of "i8"')
                  or raises(function() manifold.memory.read(1, "i32", { cout = 1 }) end, "options.cout is not an option")
                  or raises(function() manifold.memory.read(1, "bytes") end, "needs options.count")
                  or raises(function() manifold.memory.read(true, "i32") end, "must be an address")
                  or raises(function() manifold.log.setLevel("verbose") end, 'must be one of "debug"')
                  or raises(function() manifold.log.inspect(1, { depth = "2" }) end, "options.depth must be an integer")
                  or raises(function() manifold.output.setShowOnPrint(1) end, "must be a boolean")
                local channel = manifold.log.channel("Probe")
                failure = failure or raises(function() channel:info("x") end, "with a dot")
                  or raises(function() return channel.warn end, 'channel("Probe").warning')
                if failure then return failure end
                if manifold.log.channel("Probe") ~= channel or channel.name ~= "Probe" then return "channel identity" end
                if channel.channel("Sub").name ~= "Probe/Sub" then return "sub-channel" end
                return "ok")LUA");
        }

        void Output(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            lua.ClearOutput();
            LuaCheck(engine, "log info", R"LUA(
                manifold.log.info("hello", { b = 2, a = "x", list = { 1, 2 } })
                return "ok")LUA");
            auto entry = LastEntry(engine);
            CHECK(entry && entry->Kind == Lua::EntryKind::Log && entry->Level == LogLevel::Info && entry->TimeMs > 0);
            CHECK_MSG(entry && entry->Text == "hello" && entry->FieldsText == "{a = \"x\", b = 2, list = {1, 2}}", entry ? entry->Text + " | " + entry->FieldsText : "no entry");
            CHECK(entry && entry->Fields && entry->Fields->Nodes.size() == 6 && entry->Fields->Nodes[0].Total == 3 && entry->Fields->Nodes[1].Key == "a");
            CHECK_MSG(entry && entry->Source.Chunk == "log info" && entry->Source.Line == 2, entry ? entry->Source.Chunk + ":" + std::to_string(entry->Source.Line) : "");
            lua.Run("print('Gr\\xC3\\xB6\\xC3\\x9Fe', 7)", "columns");
            entry = LastEntry(engine);
            CHECK(entry && entry->Kind == Lua::EntryKind::Print && entry->Text == "Gr\xC3\xB6\xC3\x9F" "e\t7" && entry->Source.Chunk == "columns" && entry->Source.Line == 1 && entry->Values.empty());
            lua.Run("print('t', { 1, 2 })", "values");
            entry = LastEntry(engine);
            CHECK(entry && entry->Values.size() == 2 && entry->Values[1] && entry->Values[1]->Nodes.front().Total == 2);

            const std::size_t before = lua.Console().Count();
            LuaCheck(engine, "log debug hidden", R"LUA(
                manifold.log.debug("hidden")
                if manifold.log.getLevel() ~= "info" then return "default level" end
                return "ok")LUA");
            CHECK(lua.Console().Count() == before);
            LuaCheck(engine, "log debug shown", R"LUA(
                manifold.log.setLevel("debug")
                manifold.log.debug("shown")
                manifold.log.setLevel("info")
                return "ok")LUA");
            entry = LastEntry(engine);
            CHECK(entry && entry->Level == LogLevel::Debug && entry->Text == "shown");

            LuaCheck(engine, "log channel", R"LUA(
                manifold.log.channel("Teleporter").success("ready")
                return "ok")LUA");
            entry = LastEntry(engine);
            CHECK_MSG(entry && entry->Level == LogLevel::Success && entry->Channel == "Teleporter" && entry->Text == "ready", entry ? entry->PlainText() : "no entry");

            LuaCheck(engine, "log warning", R"LUA(
                manifold.log.channel("Probe").warning("careful")
                return "ok")LUA");
            const auto logged = RuntimeLog::Last();
            CHECK_MSG(logged && logged->Level == LogLevel::Warning && logged->Area == "Lua" && logged->Message == "[Probe] careful", logged ? logged->Message : "nothing logged");
            CHECK_MSG(logged && logged->Trace.find("log warning") != std::string::npos && logged->Trace.ends_with(":2"), logged ? logged->Trace : "");

            LuaCheck(engine, "log error object", R"LUA(
                local ok, problem = manifold.error.try(function() error("boom") end)
                manifold.log.error(problem)
                return "ok")LUA");
            entry = LastEntry(engine);
            CHECK_MSG(entry && entry->Level == LogLevel::Error && entry->Text.find("boom") != std::string::npos && entry->Code == "ME-7002", entry ? entry->PlainText() : "no entry");
            CHECK(entry && entry->Source.Chunk == "log error object" && entry->Source.Line == 2);
            CHECK(entry && entry->Traceback.find("stack traceback:") != std::string::npos);

            LuaCheck(engine, "inspect", R"LUA(
                local cycle = {}
                cycle.self = cycle
                if manifold.log.inspect(cycle) ~= "{self = <cycle>}" then return "cycle " .. manifold.log.inspect(cycle) end
                if manifold.log.inspect({ [1] = "a", ["two words"] = true }) ~= '{"a", ["two words"] = true}' then return "keys" end
                if manifold.log.inspect(255, { hex = true }) ~= "0xFF" then return "hex" end
                if manifold.log.inspect({ { { { 1 } } } }) ~= "{{{{...}}}}" then return "depth " .. manifold.log.inspect({ { { { 1 } } } }) end
                if manifold.log.inspect(manifold.memory) ~= "manifold.memory" then return "namespace" end
                if manifold.log.inspect("a\nb") ~= '"a\\nb"' then return "escape" end
                return "ok")LUA");
        }

        void ShowOnPrint(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            CHECK(engine.Options().LuaShowOnPrint);
            CHECK(lua.ShowOnPrint());
            std::uint64_t signal = lua.PrintSignal();
            lua.Run("print('shown')", "show on print");
            CHECK(lua.PrintSignal() == signal + 1);
            LuaCheck(engine, "session switch", R"LUA(
                manifold.output.setShowOnPrint(false)
                print("quiet")
                manifold.log.info("quiet too")
                if manifold.output.getShowOnPrint() then return "still on" end
                return "ok")LUA");
            CHECK(lua.PrintSignal() == signal + 1);
            CHECK(engine.Options().LuaShowOnPrint);
            lua.RememberShowOnPrint(false);
            CHECK(!engine.Options().LuaShowOnPrint && !lua.ShowOnPrint());
            lua.RememberShowOnPrint(true);
            CHECK(lua.ShowOnPrint());
            LuaCheck(engine, "compat checkbox", R"LUA(
                local box = getLuaEngine().cbShowOnPrint
                if box == nil or box.Checked ~= true then return "checkbox " .. tostring(box and box.Checked) end
                local changes = 0
                box.OnChange = function() changes = changes + 1 end
                getLuaEngine().cbShowOnPrint.Checked = false
                if manifold.output.getShowOnPrint() or box.Checked or box.State ~= 0 then return "not switched off" end
                box.Checked = false
                box.State = 1
                if not manifold.output.getShowOnPrint() or changes ~= 2 then return "state " .. changes end
                return "ok")LUA");
            signal = lua.PrintSignal();
            lua.Run("error('boom')", "show on error");
            CHECK(lua.PrintSignal() > signal);

            wchar_t temp[MAX_PATH] = L"";
            ::GetTempPathW(MAX_PATH, temp);
            const std::filesystem::path path = std::filesystem::path(temp) / L"manifold-show-on-print.ini";
            Settings written;
            written.LuaShowOnPrint = false;
            CHECK(written.Save(path).Ok());
            Settings read;
            CHECK(read.Load(path));
            CHECK(!read.LuaShowOnPrint);
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << "[Scripts]\nAutorun=1\n";
            }
            Settings old;
            CHECK(old.Load(path));
            CHECK(old.LuaShowOnPrint);
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }

        void Errors(Engine& engine)
        {
            LuaCheck(engine, "errors", R"LUA(
                local ok, a, b = manifold.error.try(function(x, y) return x + y, "two" end, 2, 3)
                if not ok or a ~= 5 or b ~= "two" then return "try results" end
                local fine, problem = manifold.error.try(function() local t = nil; return t.field end)
                if fine or not manifold.error.is(problem) then return "try failure" end
                if problem.code ~= "ME-7002" or problem.where ~= "errors:4" or not problem.message:find("index a nil value") then return "parsed " .. tostring(problem) end
                if type(problem.traceback) ~= "string" or not problem.traceback:find("^stack traceback:") then return "traceback" end
                local made = manifold.error.create("custom", { code = "TP-1" })
                if tostring(made) ~= "errors:8: custom (TP-1)" then return "tostring " .. tostring(made) end
                local raised
                fine, raised = manifold.error.try(function() manifold.error.raise(made) end)
                if raised ~= made or raised.traceback == nil then return "raise kept object" end
                fine, raised = manifold.error.try(function() manifold.error.raise("text", { code = "TP-2" }) end)
                if raised.code ~= "TP-2" or raised.where ~= "errors:13" then return "raise text " .. tostring(raised) end
                local value, extra = manifold.error.check(7, "unused")
                if value ~= 7 or extra ~= "unused" then return "check pass" end
                fine, raised = manifold.error.try(function() manifold.error.check(nil, made) end)
                if raised ~= made then return "check raise" end
                fine, raised = manifold.error.try(function() manifold.error.check(manifold.process.info()) end)
                if raised.code ~= "ME-1005" or raised.where ~= "errors:19" then return "check result " .. tostring(raised) end
                if manifold.error.is({}) or manifold.error.is(nil) then return "is" end
                fine, raised = manifold.error.try(function() manifold.log.info:x() end)
                if fine then return "bad call passed" end
                return "ok")LUA");
            const Status status = engine.Lua().Run("manifold.log:info('x')", "uncaught");
            CHECK(!status);
            CHECK_MSG(status.Text.find("uncaught:1: The manifold API was called the wrong way") != std::string::npos, status.Text);
        }

        void WithoutProcess(Engine& engine)
        {
            LuaCheck(engine, "no process", R"LUA(
                if manifold.process.isOpen() then return "open" end
                local info, problem = manifold.process.info()
                if info ~= nil or problem.code ~= "ME-1005" or problem.where ~= "no process:3" then return "info " .. tostring(problem) end
                local value, failure = manifold.memory.read(0x1000, "i32")
                if value ~= nil or failure.code ~= "ME-1005" then return "read" end
                if manifold.memory.isReadable(0x1000) then return "readable" end
                local modules, missing = manifold.process.modules()
                if modules ~= nil or missing.code ~= "ME-1005" then return "modules" end
                local hits, noScan = manifold.memory.scan("90 90")
                if hits ~= nil or noScan.code ~= "ME-1005" then return "scan" end
                local registered, noSymbol = manifold.symbol.register("x", 1)
                if registered ~= nil or noSymbol.code ~= "ME-1005" then return "symbol" end
                local fast, noFast = manifold.memory.read(0x1000, "u64")
                if fast ~= nil or noFast.code ~= "ME-1005" or noFast.where ~= "no process:14" then return "fast read " .. tostring(noFast) end
                local name, noName = manifold.process.name()
                if name ~= nil or noName.code ~= "ME-1005" or process ~= nil then return "name" end
                if #manifold.process.find() < 10 or #manifold.process.find("CSRSS") < 1 then return "find" end
                if #manifold.process.find("no-such-process-name") ~= 0 then return "find filter" end
                local opened, badId = manifold.process.open(0x100000004)
                if opened ~= nil or badId.code ~= "ME-1001" or manifold.process.isOpen() then return "pid range " .. tostring(badId) end
                return "ok")LUA");
        }

        void RecordApi(Engine& engine)
        {
            // manifold.record against this test process: value, float, script, group, errors.
            // The probes are shared with the tests after this one: put them back at the end.
            const std::int32_t keptValue = g_probeValue;
            const float keptFloat = g_probeFloat;
            g_probeValue = 77;
            g_probeFloat = 1.5f;
            LuaCheck(engine, "record api", R"LUA(
                if not manifold.process.isOpen() then
                  local info, problem = manifold.process.open(probePid)
                  if not info then return "open " .. tostring(problem) end
                end
                local R = manifold.record
                local value = R.create({ description = "Probe Value", address = ("%X"):format(probeValue), type = "4bytes" })
                if R.find("Probe Value") ~= value or R.find(value.id) ~= value then return "find returns the same object" end
                local read, text = value.getValue()
                if read ~= 77 or math.type(read) ~= "integer" or text ~= "77" then return "getValue " .. tostring(read) .. " " .. tostring(text) end
                if value.getAddress() ~= probeValue then return "getAddress" end
                local ok, problem = value.setValue(1234)
                if not ok then return "setValue " .. tostring(problem) end
                if manifold.memory.read(probeValue, "i32") ~= 1234 then return "setValue did not write" end
                local info = value.info()
                if info.kind ~= "value" or info.type ~= "4bytes" or info.description ~= "Probe Value" or info.resolved ~= probeValue or info.active then return "info" end
                value.setDescription("Probe Renamed")
                if value.getDescription() ~= "Probe Renamed" or R.find("Probe Renamed") ~= value then return "description" end

                local float = R.create({ description = "Probe Float", address = ("%X"):format(probeFloat), type = "float" })
                if float.getValue() ~= 1.5 then return "float " .. tostring(float.getValue()) end
                float.setValue(2.25)
                if manifold.memory.read(probeFloat, "float") ~= 2.25 then return "float setValue" end
                if float.info().type ~= "float" then return "float type" end

                local group = R.create({ kind = "group", description = "Probe Group" })
                local script = R.create({ kind = "script", description = "Probe Script", script = "[ENABLE]\n[DISABLE]\n", parent = group })
                local child = R.create({ description = "Probe Child", address = ("%X"):format(probeValue), parent = group.id })
                local children = group.children()
                if #children ~= 2 or children[1] ~= script or children[2] ~= child or child.parent() ~= group or value.parent() ~= nil then return "children / parent" end
                if script.isActive() then return "script starts off" end
                local switched, why = script.setActive(true)
                if not switched or not script.isActive() then return "setActive " .. tostring(why) end
                if script.toggle() ~= false or script.isActive() then return "toggle" end
                local none, noValue = script.getValue()
                if none ~= nil or noValue.code ~= "ME-6010" then return "script value " .. tostring(noValue) end
                local missing, notFound = R.find("No Such Record")
                if missing ~= nil or notFound.code ~= "ME-6008" then return "find missing " .. tostring(notFound) end
                local count = 0
                for _, each in ipairs(R.all()) do
                  if each == value or each == float or each == group or each == script or each == child then count = count + 1 end
                end
                if count ~= 5 then return "all " .. count end
                if type(R.selected()) ~= "table" then return "selected" end

                local function raises(fn, needle)
                  local okay, failure = pcall(fn)
                  if okay then return "no error for " .. needle end
                  local message = type(failure) == "table" and failure.message or tostring(failure)
                  if not message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. message end
                end
                local failed = raises(function() R.create({ kind = "folder" }) end, 'options.kind must be "value", "script" or "group"')
                  or raises(function() R.create({ type = "huge" }) end, "unknown record type huge")
                  or raises(function() R.create({ kind = "group", type = "4bytes" }) end, "options.type is for value records")
                  or raises(function() R.find({}) end, "manifold.record.find")
                  or raises(function() value.setValue({}) end, "setValue")
                if failed then return failed end

                if not group.remove() then return "remove" end
                local gone, goneWhy = child.getValue()
                if gone ~= nil or goneWhy.code ~= "ME-6009" then return "gone " .. tostring(goneWhy) end
                if R.find("Probe Child") then return "child still found" end
                value.remove()
                float.remove()
                return "ok")LUA");
            CHECK_EQ(g_probeValue, 1234);
            g_probeValue = keptValue;
            g_probeFloat = keptFloat;
        }

        void OwnProcess(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            lua_State* state = lua.State();
            const auto set = [&](const char* name, const volatile void* address)
                {
                    lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(address)));
                    lua_setglobal(state, name);
                };
            set("probeValue", &g_probeValue);
            set("probeFloat", &g_probeFloat);
            set("probeBytes", &g_probeBytes);
            set("probeText", &g_probeText);
            lua_pushinteger(state, static_cast<lua_Integer>(::GetCurrentProcessId()));
            lua_setglobal(state, "probePid");
            LuaCheck(engine, "own process", R"LUA(
                local info, problem = manifold.process.open(probePid)
                if not info then return "open " .. tostring(problem) end
                if info.id ~= probePid or not info.is64Bit or not info.name:find("Tests") then return "info" end
                if not manifold.process.isOpen() then return "isOpen" end
                local kernel = manifold.process.module("KERNEL32.DLL")
                if not kernel or kernel.base == 0 or kernel.size == 0 then return "module" end
                local missing, why = manifold.process.module("nothing-here.dll")
                if missing ~= nil or why.code ~= "ME-1016" then return "missing module " .. tostring(why) end
                if not manifold.process.mainModule().name:find("Tests") then return "main module" end
                local main = manifold.process.mainModule()
                if manifold.process.name() ~= main.name or manifold.process.name() ~= process then return "name " .. tostring(manifold.process.name()) .. " / " .. tostring(process) end
                local address = manifold.process.address
                if address("$process") ~= main.base or address("$PROCESS+10") ~= main.base + 0x10 or address('"$process"+10') ~= main.base + 0x10 then return "$process" end
                if address("mainmodule") ~= main.base or address("MainModule+1A") ~= main.base + 0x1A or address("[mainmodule+3C]") ~= manifold.memory.read(main.base + 0x3C, "pointer") then return "mainmodule" end
                if address("$10") ~= 0x10 or address("$DEAD+1") ~= 0xDEAE then return "dollar hex" end
                if getAddress("$process") ~= main.base or getAddress("mainmodule+8") ~= main.base + 8 then return "getAddress" end
                if manifold.process.module("$process").base ~= main.base or manifold.process.module("mainmodule").name ~= main.name then return "module alias" end
                local unknown, why = address("$nothing")
                if unknown ~= nil or why.code ~= "ME-8002" then return "unknown dollar " .. tostring(why) end
                if #manifold.process.modules() < 3 then return "modules" end
                local tick = manifold.process.address("kernel32.GetTickCount")
                if not tick or tick == 0 then return "address" end
                local bad, failure = manifold.process.address("kernel32.NoSuchExport")
                if bad ~= nil or failure.code:sub(1, 4) ~= "ME-8" then return "bad address " .. tostring(failure) end
                if not manifold.process.describe(tick):find("GetTickCount") then return "describe " .. manifold.process.describe(tick) end
                if manifold.memory.read(probeValue, "i32") ~= 0x12345678 then return "i32" end
                if manifold.memory.read(probeValue, "u8") ~= 0x78 or manifold.memory.read(probeValue, "i16") ~= 0x5678 then return "small" end
                if manifold.memory.read(probeFloat, "float") ~= 2.5 then return "float" end
                local bytes = manifold.memory.read(probeBytes, "bytes", { count = 4 })
                if #bytes ~= 4 or bytes[1] ~= 1 or bytes[4] ~= 4 then return "bytes" end
                if manifold.memory.read(probeText, "string") ~= "manifold" then return "string" end
                if manifold.memory.read(probeText, "string", { length = 3 }) ~= "man" then return "string length" end
                if manifold.memory.write(probeValue, "i32", -2) ~= true then return "write" end
                if manifold.memory.read(probeValue, "i32") ~= -2 or manifold.memory.read(probeValue, "u32") ~= 0xFFFFFFFE then return "read back" end
                manifold.memory.write(probeBytes, "bytes", { 9, 8 })
                manifold.memory.write(probeText, "string", "ok")
                if manifold.memory.read(probeBytes, "u16") ~= 0x0809 or manifold.memory.read(probeText, "string") ~= "ok" then return "write bytes" end
                local ok, raised = pcall(manifold.memory.write, probeValue, "u8", 300)
                if ok or not raised.message:find("300 does not fit u8") then return "range " .. tostring(raised) end
                if manifold.memory.read(probeValue, "i32") ~= -2 then return "range wrote" end
                ok, raised = pcall(manifold.memory.write, probeValue, "i32", 1.5)
                if ok then return "float into i32" end
                local value, unreadable = manifold.memory.read(0x10, "i32")
                if value ~= nil or unreadable.code ~= "ME-1006" or not unreadable.message:find("10") then return "unreadable " .. tostring(unreadable) end
                if manifold.memory.isReadable(0x10) or not manifold.memory.isReadable(probeValue) then return "isReadable" end
                return "ok")LUA");
        }

        void Version2(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            lua_State* state = lua.State();
            lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(&g_probeScan)));
            lua_setglobal(state, "probeScan");
            LuaCheck(engine, "v2 memory", R"LUA(
                local read = manifold.memory.read
                if read(probeBytes, "raw", { count = 4 }) ~= "\9\8\3\4" then return "raw fast" end
                if string.unpack("<I2", read(probeBytes, "raw", { count = 2 })) ~= 0x0809 then return "unpack" end
                local ok, problem = pcall(read, probeBytes, "raw")
                if ok or not problem.message:find('type "raw" needs options.count', 1, true) then return "raw count " .. tostring(problem) end
                ok, problem = pcall(read, probeBytes, "raw", { count = 2, extra = 1 })
                if ok or not problem.message:find("options.extra is not an option", 1, true) then return "raw option" end
                ok, problem = pcall(read, probeBytes, "u8", nil, 1)
                if ok or not problem.message:find("takes 3 arguments, got 4", 1, true) then return "fast extra argument" end
                ok, problem = pcall(function() manifold.memory:read(probeBytes, "u8") end)
                if ok or not problem.message:find("with a dot", 1, true) then return "fast colon" end
                local value, failure = read(0x10, "u64")
                if value ~= nil or failure.code ~= "ME-1006" or failure.where ~= "v2 memory:13" then return "fast failure " .. tostring(failure) end
                value, failure = read(0x10, "raw", { count = 8 })
                if value ~= nil or failure.code ~= "ME-1006" then return "raw failure" end
                if read(probeValue + 0.0, "i32") ~= -2 then return "float address" end
                ok, problem = pcall(read, probeValue + 0.5, "i32")
                if ok or problem.code ~= "ME-7007" then return "fractional address " .. tostring(problem) end
                if manifold.memory.write(probeBytes, "raw", "\1\2") ~= true or read(probeBytes, "u16") ~= 0x0201 then return "write raw" end
                ok = pcall(manifold.memory.write, probeBytes, "raw", { 1 })
                if ok then return "write raw table" end
                local own = manifold.process.mainModule().name
                local hits = manifold.memory.scan("4D 41 4E 49 F0 1D 5C A9", { module = own })
                if #hits ~= 1 or hits[1] ~= probeScan then return "scan " .. manifold.log.inspect(hits, { hex = true }) end
                if #manifold.memory.scan("4D 41 4E 49 F0 1D 5C A9", { module = own, executable = true }) ~= 0 then return "scan executable" end
                if #manifold.memory.scan("4D 41 4E 49 F0 1D 5C A9", { module = own, writable = true, alignment = 8 }) ~= (probeScan % 8 == 0 and 1 or 0) then return "scan alignment" end
                local bad, badPattern = manifold.memory.scan("4D ZZ")
                if bad ~= nil or badPattern.code ~= "ME-2003" then return "bad pattern " .. tostring(badPattern) end
                local none, noModule = manifold.memory.scan("90", { module = "nothing.dll" })
                if none ~= nil or noModule.code ~= "ME-1016" then return "scan module" end
                ok = pcall(manifold.memory.scan, "90", { alignment = 0 })
                if ok then return "alignment 0" end
                if not manifold.process.moduleAt(probeValue).name:find("Tests") then return "moduleAt" end
                local outside, noModuleAt = manifold.process.moduleAt(0x10)
                if outside ~= nil or noModuleAt.code ~= "ME-1017" then return "moduleAt outside" end
                return "ok")LUA");
            LuaCheck(engine, "v2 symbols", R"LUA(
                if manifold.symbol.register("probeSymbol", probeValue) ~= true then return "register" end
                if manifold.process.address("probeSymbol") ~= probeValue or manifold.memory.read("probeSymbol", "i32") ~= -2 then return "resolve" end
                if manifold.symbol.unregister("probeSymbol") ~= true then return "unregister" end
                local again, missing = manifold.symbol.unregister("probeSymbol")
                if again ~= nil or missing.code ~= "ME-8002" then return "unregister twice" end
                local calls = 0
                local first = manifold.symbol.addResolver(function(name) calls = calls + 1 if name == "dynamicProbe" then return probeValue end end, { key = "probe" })
                local second = manifold.symbol.addResolver(function(name) calls = calls + 100 if name == "dynamicProbe" then return probeValue end end, { key = "probe" })
                if first == second then return "same id" end
                if manifold.process.address("dynamicProbe") ~= probeValue then return "resolver" end
                if calls == 0 or calls % 100 ~= 0 then return "key did not replace: " .. calls end
                if manifold.symbol.removeResolver(first) then return "old id still removable" end
                local broken = manifold.symbol.addResolver(function() error("resolver broke") end)
                manifold.process.address("anotherUnknownName")
                manifold.process.address("yetAnotherUnknownName")
                manifold.symbol.removeResolver(broken)
                if not manifold.symbol.removeResolver(second) then return "remove" end
                if manifold.process.address("dynamicProbe") ~= nil then return "still resolves" end
                return "ok")LUA");
            std::size_t brokenLines = 0;
            for (const Lua::ConsoleEntry& entry : lua.Console().Entries())
            {
                const std::string text = entry.PlainText();
                if (entry.Level == LogLevel::Error && text.find("[manifold.symbol]") != std::string::npos && text.find("resolver broke") != std::string::npos)
                    brokenLines += entry.Repeat;
            }
            CHECK_MSG(brokenLines == 1, "broken resolver logged " + std::to_string(brokenLines) + " times");

            LuaCheck(engine, "v2 events", R"LUA(
                openedWith, closedCount, laterRan = nil, 0, false
                manifold.event.subscribe("processOpened", function(info) openedWith = info and info.id end, { key = "probe" })
                manifold.event.subscribe("processOpened", function(info) openedWith = "replaced:" .. info.id end, { key = "probe" })
                manifold.event.subscribe("processOpened", function() error("handler broke") end, { key = "broken" })
                manifold.event.subscribe("processOpened", function() laterRan = true end, { key = "later" })
                closedId = manifold.event.subscribe("processClosed", function() closedCount = closedCount + 1 end)
                manifold.process.open(probePid)
                if openedWith ~= "replaced:" .. probePid then return "opened " .. tostring(openedWith) end
                if not laterRan then return "error stopped later handlers" end
                return "ok")LUA");
            engine.CloseProcess();
            LuaCheck(engine, "v2 closed", R"LUA(
                if closedCount ~= 1 then return "closed " .. closedCount end
                if manifold.process.isOpen() then return "still open" end
                if not manifold.event.unsubscribe(closedId) or manifold.event.unsubscribe(closedId) then return "unsubscribe" end
                local ok = pcall(manifold.event.subscribe, "processExploded", function() end)
                if ok then return "unknown event" end
                return "ok")LUA");
            engine.CloseProcess();
            CHECK(CountEntries(engine, LogLevel::Error, "[manifold.event]") > 0 && CountEntries(engine, LogLevel::Error, "handler broke") > 0);

            LuaCheck(engine, "v2 structure", R"LUA(
                local id = manifold.structure.addNameResolver(function(address) if address == 0x1234 then return "ProbeClass", 0x1200 end end, { key = "probe" })
                local dissector = manifold.structure.addDissector(function() return false end, { key = "probe" })
                manifold.structure.addNameResolver(function(address) if address == 0x1234 then return "ProbeClass", 0x1200 end end, { key = "probe" })
                if manifold.structure.removeNameResolver(id) then return "key did not replace" end
                if not manifold.structure.removeDissector(dissector) or manifold.structure.removeDissector(dissector) then return "dissector" end
                local name, start = manifold.structure.resolveName(0x1234)
                if name ~= "ProbeClass" or start ~= 0x1200 then return "resolveName " .. tostring(name) end
                if manifold.structure.resolveName(0x9999) ~= nil then return "resolveName miss" end
                local filled = manifold.structure.addDissector(function(target, address) target.filledAt = address return true end)
                local fake = {}
                if manifold.structure.dissect(fake, 0x4000) ~= true or fake.filledAt ~= 0x4000 then return "dissect" end
                manifold.structure.removeDissector(filled)
                if manifold.structure.dissect(fake, 0x4000) ~= false then return "dissect without dissectors" end
                return "ok")LUA");
            const auto looked = lua.LookupStructureName(0x1234);
            CHECK_MSG(looked && looked->Name == "ProbeClass" && looked->Address == 0x1200, looked ? looked->Name : "no lookup");
        }

        void Assist(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            const std::vector<std::string> signatures = lua.CallSignatures("manifold.memory.read", false);
            CHECK_MSG(signatures.size() == 1 && signatures.front() == "read(address, type, options?)", signatures.empty() ? "none" : signatures.front());
            const std::vector<std::string> types = lua.CallArgumentChoices("manifold.memory.read", false, 1);
            CHECK_MSG(types.size() == 14 && types.front() == "i8" && types.back() == "string",
                types.empty() ? "none" : types.front() + ".." + std::to_string(types.size()));
            CHECK(lua.CallArgumentChoices("manifold.memory.read", false, 0).empty());
            CHECK(lua.CallArgumentChoices("manifold.memory.read", false, 2).empty());
            bool found = false;
            for (const Lua::LuaCompletion& item : lua.Complete("manifold.log", "inf", false))
                found = found || (item.Name == "info" && item.Signature == "(message, fields?)" && !item.Text.empty());
            CHECK(found);
            const std::vector<Lua::LuaCompletion> areas = lua.Complete("manifold", "", false);
            CHECK(std::any_of(areas.begin(), areas.end(), [](const Lua::LuaCompletion& item) { return item.Name == "process"; }));
        }

        void DocApi(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            LuaCheck(engine, "doc entries", R"LUA(
                local read = manifold.doc.entry(manifold.memory.read)
                if not read or read.path ~= "manifold.memory.read" or read.origin ~= "manifold" or read.kind ~= "function" then return "entry read" end
                local signature = read.signatures[1]
                if signature.text ~= "read(address, type, options?)" or #signature.params ~= 3 then return "read signature " .. tostring(signature.text) end
                if #signature.params[2].choices ~= 14 or not signature.params[3].optional or signature.params[3].keys.count ~= "integer" then return "read params" end
                if manifold.doc.entry("manifold.memory.read").path ~= "manifold.memory.read" then return "entry by name" end
                local ce = manifold.doc.entry("readInteger")
                if not ce or ce.origin ~= "cheatengine" or ce.signatures[1].text ~= "readInteger(address)" then return "cheat engine entry" end
                local standard = manifold.doc.entry("string.format")
                if not standard or standard.origin ~= "standard" or standard.signatures[1].text ~= "format(format, ...)" then return "standard entry" end
                local channel = manifold.doc.entry(manifold.log.channel("Probe").info)
                if not channel or channel.path ~= "manifold.log.Channel.info" then return "channel entry" end
                local missing, problem = manifold.doc.entry("NoSuch.thing")
                if missing ~= nil or problem.code ~= "ME-7008" then return "missing entry" end
                return "ok")LUA");
            LuaCheck(engine, "doc register", R"LUA(
                local function teleport(x, y) end
                local done = manifold.doc.register(teleport, {
                  path = "DocProbe.teleport",
                  doc = "Moves the player. Then waits.",
                  params = { { name = "x", type = "number", doc = "Across." }, { name = "y", type = "number", optional = true } },
                  returns = "boolean",
                  overloads = { { params = { { name = "target", type = "string" } }, returns = "boolean" } },
                })
                if done ~= true then return "register result" end
                local entry = manifold.doc.entry(teleport)
                if entry.path ~= "DocProbe.teleport" or entry.origin ~= "script" or entry.summary ~= "Moves the player." then return "register entry" end
                if entry.signatures[1].text ~= "teleport(x, y?)" or entry.signatures[1].params[1].text ~= "Across." or entry.signatures[2].text ~= "teleport(target)" then return "register signatures" end
                if manifold.doc.entry("DocProbe.teleport").path ~= "DocProbe.teleport" then return "register by name" end
                local ok, bad = pcall(manifold.doc.register, teleport, { params = { { type = "number" } } })
                if ok or not manifold.error.is(bad) or bad.code ~= "ME-7007" then return "bad spec" end
                ok, bad = pcall(manifold.doc.register, teleport, { colour = "red" })
                if ok or bad.code ~= "ME-7007" then return "unknown spec key" end
                ok, bad = pcall(manifold.doc.register, "manifold.memory.read", { doc = "mine" })
                if ok or bad.code ~= "ME-7007" then return "replaced manifold" end
                if manifold.doc.unregister(teleport) ~= true then return "unregister" end
                local gone, why = manifold.doc.unregister(teleport)
                if gone ~= nil or why.code ~= "ME-7008" then return "unregister twice" end
                ok, bad = pcall(manifold.doc.unregister, "readInteger")
                if ok or bad.code ~= "ME-7007" or manifold.doc.entry("readInteger") == nil then return "unregistered cheat engine" end
                local function original(address, size) end
                local wrapper = function(...) return original(...) end
                if manifold.doc.inherit(wrapper, original) ~= true then return "inherit" end
                local inherited = manifold.doc.entry(wrapper)
                if #inherited.signatures[1].params ~= 2 or inherited.signatures[1].params[1].name ~= "address" then return "inherited params" end
                local named = function(...) end
                manifold.doc.inherit(named, "manifold.memory.read")
                if manifold.doc.entry(named).path ~= "manifold.memory.read" then return "inherit by name" end
                local found = manifold.doc.find("memory.read", { limit = 3 })
                if #found < 1 or found[1].path ~= "manifold.memory.read" then return "find " .. tostring(found[1] and found[1].path) end
                local areas = manifold.doc.find("manifold.", { kind = "table", limit = 100 })
                if #areas < 9 then return "find tables " .. #areas end
                for _, item in ipairs(areas) do
                  if item.kind ~= "table" then return "find kind" end
                end
                local markdown = manifold.doc.markdown("manifold.doc")
                if not markdown:find("## manifold.doc", 1, true) or not markdown:find("`manifold.doc.register(target, spec)`", 1, true) then return "markdown" end
                return "ok")LUA");
            LuaCheck(engine, "doc annotations", R"LUA(
                local chunk = [==[
local M = {}

---Adds two numbers.
---@param a integer The first
---@param b? integer
---@return integer
function M.add(a, b) return a + (b or 0) end

function M.plain(x, ...) end

return M
]==]
                local M = load(chunk)()
                local parsed = manifold.doc.annotations(M.add)
                if not parsed or parsed.summary ~= "Adds two numbers." or parsed.signatures[1].text ~= "add(a, b?)" then return "annotations" end
                if parsed.signatures[1].params[1].type ~= "integer" or parsed.signatures[1].params[1].text ~= "The first" or parsed.signatures[1].returns ~= "integer" then return "annotation params" end
                local none, problem = manifold.doc.annotations(M.plain)
                if none ~= nil or problem.code ~= "ME-7008" then return "plain annotations" end
                local count = manifold.doc.registerModule("DocModule", M, { doc = "A probe module." })
                if count ~= 2 then return "registerModule count " .. tostring(count) end
                local added = manifold.doc.entry("DocModule.add")
                if added.origin ~= "annotation" or added.text ~= "Adds two numbers." then return "module add" end
                local plain = manifold.doc.entry("DocModule.plain")
                if plain.origin ~= "reflection" or plain.signatures[1].text ~= "plain(x, ...)" then return "module plain" end
                if manifold.doc.entry(M.add).path ~= "DocModule.add" then return "module binding" end
                if manifold.doc.entry("DocModule").text ~= "A probe module." then return "module table" end
                DocModuleProbe = M
                return "ok")LUA");
            const std::vector<std::string> added = lua.CallSignatures("DocModuleProbe.add", false);
            CHECK_MSG(added.size() == 1 && added.front() == "add(a, b?)", added.empty() ? "none" : added.front());
            const auto described = lua.Describe("DocModuleProbe.add", false);
            CHECK(described && described->Path == "DocModule.add");
            const std::size_t before = lua.Docs().Size();
            LuaCheck(engine, "doc anchors", "for i = 1, 50 do manifold.doc.register(function() end, { doc = 'temporary' }) end collectgarbage() collectgarbage() return 'ok'");
            CHECK_MSG(lua.Docs().Size() <= before + 2, std::to_string(lua.Docs().Size()) + " after " + std::to_string(before));
        }

        int LuaInteger(Engine& engine, const char* name, const std::string& code)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(code, name, result);
            CHECK_MSG(status.Ok(), std::string(name) + ": " + status.Text);
            return static_cast<int>(ParseDecimal(result).value_or(0));
        }

        void UiOpacity(Engine& engine)
        {
            Lua::UiModel& model = engine.Lua().Ui();
            const int plain = LuaInteger(engine, "ui opacity default", R"LUA(
                return tostring(manifold.ui.window.create("Opaque", { key = "opacity plain" }).id))LUA");
            const Lua::UiNode* node = model.Find(plain);
            CHECK(node && node->Opacity < 0.0f && node->InactiveOpacity < 0.0f);

            const int overlay = LuaInteger(engine, "ui opacity", R"LUA(
                uiOpacityWindow = manifold.ui.window.create("Overlay", { key = "opacity overlay", opacity = 0.8, inactiveOpacity = 0.4, dock = false })
                return tostring(uiOpacityWindow.id))LUA");
            node = model.Find(overlay);
            CHECK(node && std::abs(node->Opacity - 0.8f) < 0.001f && std::abs(node->InactiveOpacity - 0.4f) < 0.001f && !node->Dock);

            LuaCheck(engine, "ui opacity update", R"LUA(
                if not uiOpacityWindow.update({ opacity = 2, inactiveOpacity = 0.01 }) then return "update" end
                local ok, problem = pcall(uiOpacityWindow.update, { opacity = "half" })
                if ok or not manifold.error.is(problem) or problem.code ~= "ME-7007" then return "a string opacity was taken" end
                ok, problem = pcall(manifold.ui.window.create, "Bad", { opacity = true })
                if ok or problem.code ~= "ME-7007" then return "a boolean opacity was taken" end
                return "ok")LUA");
            node = model.Find(overlay);
            CHECK(node && node->Opacity == 1.0f && std::abs(node->InactiveOpacity - 0.1f) < 0.001f);
            LuaCheck(engine, "ui opacity cleanup", R"LUA(
                uiOpacityWindow.remove()
                manifold.ui.window.create("Opaque", { key = "opacity plain" }).remove()
                uiOpacityWindow = nil
                return "ok")LUA");
        }

        void UiApi(Engine& engine)
        {
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "ui widgets", R"LUA(
                local w = manifold.ui.window.create("Probe", { icon = "map-marker-alt", width = 300 })
                if w.kind ~= "window" or type(w.id) ~= "number" or not w.isOpen() then return "window" end
                local row = w.add("row")
                local x = row.add("number", { label = "X", value = 1.5, step = 0.1 })
                local name = row.add("input", { label = "Name", value = "hero" })
                local flag = w.add("checkbox", { label = "God mode", value = true })
                local pick = w.add("combo", { items = { "one", "two", "three" }, value = 2 })
                local grid = w.add("table", { columns = { "Name", { title = "Value", width = 80, align = "right" } }, rows = { { "a", 1 }, { "b", 2 } } })
                local tree = w.add("tree", { items = { { text = "root", id = "r", children = { "leaf" } } } })
                local bar = w.add("progress", { value = 0.25 })
                if x.get() ~= 1.5 or name.get() ~= "hero" or flag.get() ~= true or pick.get() ~= 2 then return "initial values" end
                if grid.get() ~= nil or tree.get() ~= nil or bar.get() ~= 0.25 then return "initial selections" end
                x.set(7); name.set("champ"); flag.set(false); pick.set(3); grid.set(2); tree.set("leaf"); bar.set(2)
                if x.get() ~= 7 or name.get() ~= "champ" or flag.get() ~= false or pick.get() ~= 3 or grid.get() ~= 2 or tree.get() ~= "leaf" or bar.get() ~= 1 then return "set values" end
                if not grid.update({ rows = { { "only" } } }) or grid.get() ~= nil then return "rows reset the selection" end
                local tabs = w.add("tabs")
                local first = tabs.add("tab", { title = "First" })
                first.add("text", { text = "inside", color = "muted", wrap = true })
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  if not manifold.error.is(problem) or problem.code ~= "ME-7007" then return "not a misuse error: " .. tostring(problem) end
                  if not problem.message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. problem.message end
                end
                local failure = raises(function() w:add("text") end, "with a dot")
                  or raises(function() w.add("knob") end, 'must be one of "button"')
                  or raises(function() w.add("text", { colour = "red" }) end, "options.colour is not an option")
                  or raises(function() w.add("text", { color = "red" }) end, "is no theme colour")
                  or raises(function() w.add("button", { icon = "nope-nope" }) end, "is no Font Awesome icon")
                  or raises(function() w.add("button", { style = "loud" }) end, 'options.style must be one of')
                  or raises(function() flag.add("text") end, "holds no widgets")
                  or raises(function() tabs.add("text") end, "tabs hold tab widgets only")
                  or raises(function() w.add("tab") end, "goes into a tabs widget")
                  or raises(function() x.draw({}) end, "only a canvas draws")
                if failure then return failure end
                local gone = w.add("text", { text = "bye" })
                if not gone.remove() or gone.remove() then return "remove twice" end
                local value, problem = gone.get()
                if value ~= nil or problem.code ~= "ME-7010" then return "gone widget" end
                if not w.remove() or w.isOpen() then return "window remove" end
                value, problem = x.set(1)
                if value ~= nil or problem.code ~= "ME-7010" then return "child of a removed window" end
                return "ok")LUA");
            CHECK_EQ(model.Windows().size(), 0u);

            const int button = LuaInteger(engine, "ui events", R"LUA(
                uiProbe = {}
                local w = manifold.ui.window.create("Events", { key = "events probe", onClose = function(window) uiProbe.closed = window.kind end })
                local b = w.add("button", { text = "Go", onClick = function(widget) uiProbe.clicks = (uiProbe.clicks or 0) + 1; uiProbe.kind = widget.kind end })
                uiProbeChange = w.add("number", { value = 1, onChange = function(value) uiProbe.value = value end })
                uiProbeWindow = w
                return tostring(b.id))LUA");
            CHECK(button > 0);
            const Lua::UiNode* number = nullptr;
            for (const int id : model.Windows())
            {
                if (const Lua::UiNode* window = model.Find(id); window && window->Title == "Events" && window->Children.size() == 2)
                    number = model.Find(window->Children[1]);
            }
            CHECK(number && number->Kind == Lua::UiKind::Number && (number->Handlers & Lua::UiHandler::Change));
            model.Post({ button, "click", {} });
            model.Post({ button, "click", {} });
            if (number)
                model.Post({ number->Id, "change", { 42.0 } });
            if (!model.Windows().empty())
                model.Post({ model.Windows().back(), "close", {} });
            engine.Lua().Tick(1000.0);
            LuaCheck(engine, "ui events seen", R"LUA(
                if uiProbe.clicks ~= 2 or uiProbe.kind ~= "button" then return "clicks " .. tostring(uiProbe.clicks) end
                if uiProbe.value ~= 42 then return "change " .. tostring(uiProbe.value) end
                if uiProbe.closed ~= "window" then return "close" end
                local again = manifold.ui.window.create("Events again", { key = "events probe" })
                if uiProbeWindow.isOpen() then return "key did not replace the window" end
                again.remove()
                return "ok")LUA");

            CHECK(engine.Lua().Run(R"LUA(manifold.ui.window.create("Owned one"); manifold.ui.menu.add("Probe/Owned"))LUA", "ui owner").Ok());
            const std::size_t windows = model.Windows().size();
            const std::size_t menus = model.Slots(Lua::UiSlotKind::Menu).size();
            CHECK(windows >= 1 && menus >= 1);
            CHECK(engine.Lua().Run(R"LUA(manifold.ui.window.create("Owned two"))LUA", "ui owner").Ok());
            CHECK_EQ(model.Windows().size(), windows);
            CHECK_EQ(model.Slots(Lua::UiSlotKind::Menu).size(), menus - 1);
            CHECK(engine.Lua().Run("", "ui owner").Ok());
            CHECK_EQ(model.Windows().size(), windows - 1);

            CHECK(engine.Lua().StartTask(R"LUA(manifold.ui.window.create("Task one"))LUA", "ui task", {}) > 0);
            engine.Lua().RunTasks(1.0);
            const std::size_t taskWindows = model.Windows().size();
            CHECK(taskWindows >= 1);
            CHECK(engine.Lua().StartTask(R"LUA(manifold.ui.window.create("Task two"))LUA", "ui task", {}) > 0);
            CHECK_EQ(model.Windows().size(), taskWindows - 1);
            engine.Lua().RunTasks(1.0);
            CHECK_EQ(model.Windows().size(), taskWindows);
            CHECK(engine.Lua().Run("", "ui task").Ok());
            CHECK_EQ(model.Windows().size(), taskWindows - 1);

            const std::size_t fragmentMenus = model.Slots(Lua::UiSlotKind::Menu).size();
            CHECK(engine.Lua().Run(R"LUA(uiFragmentOne = manifold.ui.menu.add("Probe/Fragment one"))LUA", "{$lua}").Ok());
            CHECK(engine.Lua().Run(R"LUA(uiFragmentTwo = manifold.ui.menu.add("Probe/Fragment two"))LUA", "{$lua}").Ok());
            CHECK_EQ(model.Slots(Lua::UiSlotKind::Menu).size(), fragmentMenus + 2);
            LuaCheck(engine, "ui fragments", R"LUA(
                if not uiFragmentOne.remove() or not uiFragmentTwo.remove() then return "remove" end
                return "ok")LUA");
            CHECK_EQ(model.Slots(Lua::UiSlotKind::Menu).size(), fragmentMenus);

            LuaCheck(engine, "ui canvas", R"LUA(
                local w = manifold.ui.window.create("Canvas")
                local c = w.add("canvas", { height = 200, background = "surface", zoom = true, menu = { "Mark", "-", "Clear" } })
                local ok = c.draw({
                  { kind = "line", x = 0, y = 0, x2 = 10, y2 = 10, color = "accent", thickness = 2 },
                  { kind = "rect", x = 1, y = 1, width = 5, height = 5, filled = true, color = "#FF8800" },
                  { kind = "circle", x = 5, y = 5, radius = 3, color = "#11223344" },
                  { kind = "polygon", points = { 0, 0, 4, 0, 2, 3 }, filled = true },
                  { kind = "text", x = 2, y = 2, text = "hi", size = 14 },
                })
                if not ok then return "draw" end
                if not c.draw({ { kind = "line", x = 1, y = 1, x2 = 2, y2 = 2 } }, { append = true }) then return "append" end
                local function raises(fn, needle)
                  local okay, problem = pcall(fn)
                  if okay or not problem.message:find(needle, 1, true) then return "missing error " .. needle end
                end
                local failure = raises(function() c.draw({ { kind = "star" } }) end, "kind must be one of")
                  or raises(function() c.draw({ { kind = "line", x = 1, depth = 2 } }) end, "has no depth")
                  or raises(function() c.draw({ { kind = "line", color = "rainbow" } }) end, "is no theme colour")
                  or raises(function() c.draw({ { kind = "image", x = 0, y = 0 } }) end, "needs an image")
                if failure then return failure end
                local view = c.getView()
                if view.x ~= 0 or view.y ~= 0 or view.scale ~= 1 then return "view" end
                c.setView({ x = 10, y = 20, scale = 2 })
                view = c.getView()
                if view.x ~= 10 or view.y ~= 20 or view.scale ~= 2 then return "set view" end
                if not c.clear() then return "clear" end
                w.remove()
                return "ok")LUA");

            LuaCheck(engine, "ui notice", R"LUA(
                local n = manifold.ui.notice.show("Loading", { progress = 0.2 })
                if not n.isShown() then return "shown" end
                if not n.update({ text = "Loaded", progress = 1, level = "success" }) then return "update" end
                if not n.remove() or n.isShown() then return "remove" end
                local value, problem = n.update({ text = "again" })
                if value ~= nil or problem.code ~= "ME-7010" then return "gone notice" end
                local ok = pcall(manifold.ui.notice.show, "x", { level = "loud" })
                if ok then return "level not checked" end
                return "ok")LUA");

            const int slot = LuaInteger(engine, "ui slots", R"LUA(
                uiSlotProbe = 0
                local entry = manifold.ui.menu.add("Probe/Teleport", { icon = "map-marker-alt", onClick = function(e) uiSlotProbe = uiSlotProbe + e.id end })
                local tool = manifold.ui.toolbar.add("play", { tooltip = "Run", onClick = function() end })
                local state = manifold.ui.status.add("Ready", { icon = "check", color = "success" })
                if not tool.update({ enabled = false }) or not state.update({ text = "Busy" }) then return "0" end
                uiSlotEntries = { entry, tool, state }
                return tostring(entry.id))LUA");
            CHECK(slot > 0);
            CHECK(!model.Slots(Lua::UiSlotKind::Toolbar).empty() && !model.Slots(Lua::UiSlotKind::Status).empty());
            model.Post({ slot, "click", {} });
            engine.Lua().Tick(1001.0);
            LuaCheck(engine, "ui slot click", R"LUA(
                if uiSlotProbe ~= uiSlotEntries[1].id then return "click " .. tostring(uiSlotProbe) end
                for _, entry in ipairs(uiSlotEntries) do
                  if not entry.remove() then return "remove" end
                end
                return "ok")LUA");
            CHECK(model.Slots(Lua::UiSlotKind::Status).empty());

            LuaCheck(engine, "ui dialog", R"LUA(
                uiDialogAnswer = "none"
                uiDialog = manifold.ui.dialog.confirm("Really?", { title = "Probe", onClose = function(answer) uiDialogAnswer = answer end })
                return "ok")LUA");
            CHECK_EQ(model.Dialogs().size(), 1u);
            if (!model.Dialogs().empty())
            {
                const int dialog = model.Dialogs().front().Id;
                model.Post({ dialog, "dialog", { true } });
                model.RemoveDialog(dialog);
            }
            engine.Lua().Tick(1002.0);
            LuaCheck(engine, "ui dialog answer", R"LUA(
                if uiDialogAnswer ~= true then return "answer " .. tostring(uiDialogAnswer) end
                local pick = manifold.ui.dialog.choose("Which?", { "a", "b" })
                if not pick.close() or pick.close() then return "close" end
                return "ok")LUA");
            CHECK(model.Dialogs().empty());

            LuaCheck(engine, "ui theme and images", R"LUA(
                local colors = manifold.ui.theme.colors()
                local found = false
                for _, name in ipairs(colors) do found = found or name == "accent" end
                if not found then return "colors" end
                local icons = manifold.ui.theme.icons("marker")
                found = false
                for _, name in ipairs(icons) do found = found or name == "map-marker-alt" end
                if not found or not manifold.ui.theme.isIcon("play") or manifold.ui.theme.isIcon("nope") then return "icons" end
                local image, problem = manifold.ui.image.load("no such picture.png")
                if image ~= nil or problem.code ~= "ME-7009" then return "missing image" end
                return "ok")LUA");

            LuaCheck(engine, "ui update", R"LUA(
                uiUpdates = 0
                uiUpdateWindow = manifold.ui.window.create("Ticker", { interval = 0.5, onUpdate = function() uiUpdates = uiUpdates + 1 end })
                return "ok")LUA");
            engine.Lua().Tick(2000.0);
            engine.Lua().Tick(2000.1);
            engine.Lua().Tick(2000.6);
            LuaCheck(engine, "ui update ran", R"LUA(
                if uiUpdates ~= 2 then return "updates " .. uiUpdates end
                uiUpdateWindow.hide()
                return "ok")LUA");
            engine.Lua().Tick(2002.0);
            LuaCheck(engine, "ui update stops when hidden", R"LUA(
                if uiUpdates ~= 2 then return "hidden window updated" end
                uiUpdateWindow.remove()
                return "ok")LUA");
        }

        const Lua::UiNode* FindWindow(Lua::UiModel& model, std::string_view title)
        {
            for (const int id : model.Windows())
            {
                if (const Lua::UiNode* node = model.Find(id); node && node->Title == title)
                    return node;
            }
            return nullptr;
        }

        void UiNewWidgets(Engine& engine)
        {
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "ui new widgets", R"LUA(
                local w = manifold.ui.window.create("New widgets", { key = "new widgets" })
                local grid = w.add("grid", { columns = 3 })
                grid.add("text", { text = "Name" })
                local radio = grid.add("radio", { items = { "Low", "High" }, value = 2, horizontal = true })
                local color = grid.add("color", { label = "Tint", value = "#336699", alpha = true })
                w.add("separator", { text = "More" })
                w.add("row", { align = "right" }).add("button", { text = "OK", width = -1 })
                w.add("section", { title = "Folded", collapsible = true, open = false })
                if radio.get() ~= 2 or color.get() ~= "#336699" then return "initial values" end
                radio.set(1); color.set("#FF0000")
                if radio.get() ~= 1 or color.get() ~= "#FF0000" then return "set values" end
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  if not problem.message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. problem.message end
                end
                local failure = raises(function() w.add("row", { align = "top" }) end, "options.align must be one of")
                  or raises(function() w.add("grid", { columns = 0 }) end, "options.columns must be 1 to 16")
                  or raises(function() w.add("color", { value = "purple-ish" }) end, "is no theme colour")
                  or raises(function() w.add("radio", { items = { "a" }, value = "a" }) end, "options.value must be")
                if failure then return failure end
                return "ok")LUA");
            const Lua::UiNode* window = FindWindow(model, "New widgets");
            CHECK(window && window->Children.size() == 4);
            if (window && window->Children.size() == 4)
            {
                const Lua::UiNode* grid = model.Find(window->Children[0]);
                CHECK(grid && grid->Kind == Lua::UiKind::Grid && grid->GridColumns == 3 && grid->Children.size() == 3);
                const Lua::UiNode* separator = model.Find(window->Children[1]);
                CHECK(separator && separator->Text == "More");
                const Lua::UiNode* row = model.Find(window->Children[2]);
                CHECK(row && row->Align == "right");
                const Lua::UiNode* button = row && !row->Children.empty() ? model.Find(row->Children[0]) : nullptr;
                CHECK(button && button->Width == -1.0f);
                const Lua::UiNode* section = model.Find(window->Children[3]);
                CHECK(section && section->Collapsible && !section->Open);
                if (grid && grid->Children.size() == 3)
                {
                    const Lua::UiNode* radio = model.Find(grid->Children[1]);
                    const Lua::UiNode* color = model.Find(grid->Children[2]);
                    CHECK(radio && radio->Horizontal && radio->Items.size() == 2);
                    CHECK(color && color->Alpha && color->Label == "Tint");
                }
            }
            LuaCheck(engine, "ui new widgets cleanup", R"LUA(
                manifold.ui.window.create("x", { key = "new widgets" }).remove()
                return "ok")LUA");
        }

        Lua::FormDocument SampleForm()
        {
            using Lua::UiKind;
            Lua::FormDocument form = Lua::NewForm("Player \"Tools\"");
            form.Key = "player tools";
            Lua::UiNode& window = *form.Model.Find(form.Window);
            window.Icon = "user";
            form.Meta[form.Window].Handlers["onClose"] = "closed";
            const int section = Lua::FormAdd(form, UiKind::Section, form.Window);
            Lua::FormSetProp(*form.Model.Find(section), "title", "Player");
            Lua::FormSetProp(*form.Model.Find(section), "collapsible", "true");
            const int grid = Lua::FormAdd(form, UiKind::Grid, section);
            Lua::FormAdd(form, UiKind::Text, grid);
            const int speed = Lua::FormAdd(form, UiKind::Slider, grid);
            form.Meta[speed].Name = "speed";
            form.Meta[speed].Handlers["onChange"] = "speedChanged";
            Lua::FormSetProp(*form.Model.Find(speed), "label", "");
            Lua::FormSetProp(*form.Model.Find(speed), "max", "5");
            Lua::FormSetProp(*form.Model.Find(speed), "value", "1.5");
            Lua::FormSetProp(*form.Model.Find(speed), "format", "%.1f x");
            const int god = Lua::FormAdd(form, UiKind::Checkbox, section);
            form.Meta[god].Name = "god";
            form.Meta[god].Handlers["onChange"] = "godChanged";
            Lua::FormSetProp(*form.Model.Find(god), "tooltip", "Line one\nline \\two\\");
            const int tabs = Lua::FormAdd(form, UiKind::Tabs, form.Window);
            Lua::FormSetProp(*form.Model.Find(form.Model.Find(tabs)->Children[0]), "title", "Items");
            const int list = Lua::FormAdd(form, UiKind::Table, form.Model.Find(tabs)->Children[0]);
            form.Meta[list].Name = "items";
            Lua::FormSetProp(*form.Model.Find(list), "columns", "Name\nCount | 60 | right");
            Lua::FormSetProp(*form.Model.Find(list), "rows", "Potion | 3\nKey | 1");
            Lua::FormSetProp(*form.Model.Find(list), "menu", "Give\n-\nRemove");
            form.Meta[list].Handlers["onMenu"] = "itemMenu";
            const int tree = Lua::FormAdd(form, UiKind::Tree, form.Model.Find(tabs)->Children[0]);
            Lua::FormSetProp(*form.Model.Find(tree), "items", "World | w | globe | open\n  Town\n  Cave | cave");
            const int row = Lua::FormAdd(form, UiKind::Row, form.Window);
            Lua::FormSetProp(*form.Model.Find(row), "align", "right");
            const int save = Lua::FormAdd(form, UiKind::Button, row);
            form.Meta[save].Name = "save";
            form.Meta[save].Handlers["onClick"] = "saveClicked";
            Lua::FormSetProp(*form.Model.Find(save), "style", "accent");
            Lua::FormSetProp(*form.Model.Find(save), "icon", "save");
            const int pick = Lua::FormAdd(form, UiKind::Radio, row);
            form.Meta[pick].Name = "mode";
            Lua::FormAdd(form, UiKind::Color, row);
            return form;
        }

        void UiForms(Engine& engine)
        {
            using Lua::UiKind;
            Lua::FormDocument form = SampleForm();
            CHECK(Lua::FormProblems(form).empty());
            const std::string saved = Lua::FormSave(form);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> warnings;
            CHECK_MSG(Lua::FormLoad(saved, loaded, error, &warnings), error);
            CHECK(warnings.empty());
            CHECK_MSG(Lua::FormSave(loaded) == saved, "a saved form loads back to the same text");
            CHECK_EQ(loaded.Key, std::string("player tools"));
            CHECK(loaded.Model.Find(loaded.Window) && loaded.Model.Find(loaded.Window)->Title == "Player \"Tools\"");

            const auto fails = [](std::string_view text, std::string_view needle) {
                Lua::FormDocument out;
                std::string why;
                const bool ok = Lua::FormLoad(text, out, why);
                CHECK_MSG(!ok && why.find(needle) != std::string::npos, std::string(needle) + " <- " + why);
            };
            fails("return { title = 'x' }", "no form = 1");
            fails("return { form = 2, title = 'x' }", "newer Manifold");
            fails("return { form = 1, title = 'x', children = { { kind = 'knob' } } }", "unknown widget kind");
            fails("return { form = 1, title = 'x', children = { { kind = 'tab' } } }", "cannot hold a tab");
            fails("return { form = 1, title = 'x', children = { { kind = 'tabs', children = { { kind = 'text' } } } } }", "cannot hold a text");
            fails("return { form = 1, title = 'x', children = { { kind = 'button', name = 'two words' } } }", "must be a Lua name");
            fails("return { form = 1, title = 'x', children = { { kind = 'button', onClick = 12 } } }", "must name a function");
            fails("while true do end", "too much work");
            fails("return os.exit()", "os");
            fails("return 5", "must return a table");
            {
                Lua::FormDocument out;
                std::string why;
                std::vector<std::string> notes;
                CHECK(Lua::FormLoad("return { form = 1, title = 'x', children = { { kind = 'button', colour = 'red' } } }", out, why, &notes));
                CHECK(notes.size() == 1 && notes[0].find("colour") != std::string::npos);
            }

            Lua::FormDocument edits = Lua::NewForm("Edits");
            const int first = Lua::FormAdd(edits, UiKind::Button, edits.Window);
            const int second = Lua::FormAdd(edits, UiKind::Button, edits.Window);
            CHECK(edits.Meta[first].Name == "button1" && edits.Meta[second].Name == "button2");
            CHECK(Lua::FormMove(edits, second, edits.Window, 0));
            CHECK(edits.Model.Find(edits.Window)->Children == std::vector<int>({ second, first }));
            const int column = Lua::FormAdd(edits, UiKind::Column, edits.Window);
            CHECK(Lua::FormMove(edits, first, column, -1));
            CHECK(!Lua::FormMove(edits, column, column, -1));
            const int copy = Lua::FormDuplicate(edits, column);
            CHECK(copy && edits.Model.Find(copy)->Children.size() == 1 && edits.Meta[edits.Model.Find(copy)->Children[0]].Name == "button3");
            edits.Meta[second].Name = "button3";
            CHECK(!Lua::FormProblems(edits).empty());
            CHECK(Lua::FormRemove(edits, copy) && !edits.Model.Find(copy));
            CHECK(Lua::FormAdd(edits, UiKind::Tab, edits.Window) == 0);

            Lua::UiModel& model = engine.Lua().Ui();
            const std::string standalone = Lua::FormScript(form, Lua::FormScriptStyle::Standalone);
            CHECK(standalone.find("local function saveClicked(widget)") != std::string::npos);
            CHECK(standalone.find("local function itemMenu(choice, text, row, widget)") != std::string::npos);
            CHECK(engine.Lua().Run(standalone, "form standalone").Ok());
            const Lua::UiNode* window = FindWindow(model, "Player \"Tools\"");
            CHECK(window && window->Children.size() == 3 && window->Icon == "user" && (window->Handlers & Lua::UiHandler::Close));
            if (window && window->Children.size() == 3)
            {
                const Lua::UiNode* section = model.Find(window->Children[0]);
                CHECK(section && section->Kind == UiKind::Section && section->Title == "Player" && section->Collapsible);
                const Lua::UiNode* grid = section && !section->Children.empty() ? model.Find(section->Children[0]) : nullptr;
                const Lua::UiNode* speed = grid && grid->Children.size() == 2 ? model.Find(grid->Children[1]) : nullptr;
                CHECK(speed && speed->Kind == UiKind::Slider && speed->Number == 1.5 && speed->HasMax && speed->Max == 5.0 && speed->Format == "%.1f x" && (speed->Handlers & Lua::UiHandler::Change));
                const Lua::UiNode* god = section && section->Children.size() == 2 ? model.Find(section->Children[1]) : nullptr;
                CHECK(god && god->Tooltip == "Line one\nline \\two\\");
                const Lua::UiNode* tabs = model.Find(window->Children[1]);
                const Lua::UiNode* tab = tabs && !tabs->Children.empty() ? model.Find(tabs->Children[0]) : nullptr;
                const Lua::UiNode* table = tab && tab->Children.size() == 2 ? model.Find(tab->Children[0]) : nullptr;
                CHECK(table && table->Columns.size() == 2 && table->Columns[1].Width == 60.0f && table->Columns[1].Align == "right" && table->Rows.size() == 2 && table->Menu.size() == 3);
                const Lua::UiNode* tree = tab && tab->Children.size() == 2 ? model.Find(tab->Children[1]) : nullptr;
                CHECK(tree && tree->Tree.size() == 1 && tree->Tree[0].Id == "w" && tree->Tree[0].Open && tree->Tree[0].Children.size() == 2 && tree->Tree[0].Children[1].Id == "cave");
            }
            CHECK(engine.Lua().Run("", "form standalone").Ok());
            CHECK(!FindWindow(model, "Player \"Tools\""));

            const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("manifold-form-test-" + std::to_string(::GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::create_directories(folder / "forms", ignored);
            const std::filesystem::path formFile = folder / "forms" / "Player.form.lua";
            const std::filesystem::path scriptFile = folder / "Player.lua";
            {
                std::ofstream(formFile, std::ios::binary) << saved;
                std::string script = Lua::FormScript(form, Lua::FormScriptStyle::FormFile, "forms/Player.form.lua");
                CHECK(script.find("function handlers.saveClicked(widget)") != std::string::npos);
                CHECK(script.find("ui.form.load(\"forms/Player.form.lua\", handlers)") != std::string::npos);
                script += "\nformProbe = { window = window, save = w.save, speed = w.speed }\n";
                std::ofstream(scriptFile, std::ios::binary) << script;
            }
            const Status ran = engine.Lua().RunFile(scriptFile);
            CHECK_MSG(ran.Ok(), ran.Text);
            CHECK(FindWindow(model, "Player \"Tools\""));
            LuaCheck(engine, "form load", R"LUA(
                if not formProbe or formProbe.save.kind ~= "button" or formProbe.speed.get() ~= 1.5 then return "widgets by name" end
                local value, problem = manifold.ui.form.load("missing.form.lua")
                if value ~= nil or problem.code ~= "ME-7014" then return "missing file" end
                local handlers = { saveClicked = function(widget) formClicks = (formClicks or 0) + 1 end }
                local made, w = manifold.ui.form.build({ form = 1, title = "Built", key = "built form", children = {
                  { kind = "row", children = { { kind = "button", name = "go", text = "Go", onClick = "saveClicked" } } },
                } }, handlers)
                formBuiltGo = w.go.id
                local ok, err = pcall(function() return w.nope end)
                if ok or not err.message:find("does not exist", 1, true) then return "unknown widget name" end
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  if not problem.message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. problem.message end
                end
                local failure = raises(function() manifold.ui.form.build({ title = "x" }) end, "no form = 1")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "button", onClick = "missing" } } }, {}) end, "handlers.missing is missing")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "text", name = "a" }, { kind = "text", name = "a" } } }) end, "two widgets are named")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", colour = "red" }) end, "form.colour is not a window option")
                if failure then return failure end
                local plain = manifold.ui.form.build({ form = 1, title = "No handlers", children = { { kind = "button", onClick = "whatever" } } })
                plain.remove()
                return "ok")LUA");
            CHECK(!FindWindow(model, "Bad"));
            int go = 0;
            {
                std::string result;
                engine.Lua().RunWithResult("return tostring(formBuiltGo)", "form probe", result);
                go = static_cast<int>(ParseDecimal(result).value_or(0));
            }
            CHECK(go > 0);
            model.Post({ go, "click", {} });
            engine.Lua().Tick(1000.0);
            LuaCheck(engine, "form handler", R"LUA(
                if formClicks ~= 1 then return "clicks " .. tostring(formClicks) end
                manifold.ui.window.create("x", { key = "built form" }).remove()
                return "ok")LUA");
            CHECK(engine.Lua().Run("", "@" + WideToUtf8(scriptFile.wstring())).Ok());
            std::filesystem::remove(formFile, ignored);
            std::filesystem::remove(scriptFile, ignored);
            std::filesystem::remove(folder / "forms", ignored);
            std::filesystem::remove(folder, ignored);

            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Examples" / "Form-Demo.lua"); ++i)
                at = at.parent_path();
            const std::filesystem::path demo = at / "Examples" / "Form-Demo.lua";
            if (!std::filesystem::exists(demo))
            {
                Skip("Examples/Form-Demo.lua not found above " + std::filesystem::current_path().string());
                return;
            }
            {
                std::ifstream file(at / "Examples" / "Form-Demo.form.lua", std::ios::binary);
                std::stringstream content;
                content << file.rdbuf();
                Lua::FormDocument example;
                std::string why;
                std::vector<std::string> notes;
                CHECK_MSG(Lua::FormLoad(content.str(), example, why, &notes) && notes.empty(), why);
                CHECK(Lua::FormProblems(example).empty());
            }
            const Status demoRan = engine.Lua().RunFile(demo);
            CHECK_MSG(demoRan.Ok(), demoRan.Text);
            const Lua::UiNode* demoWindow = FindWindow(model, "Player Tools");
            CHECK(demoWindow && demoWindow->Children.size() == 5);
            const Lua::UiNode* section = demoWindow && !demoWindow->Children.empty() ? model.Find(demoWindow->Children[0]) : nullptr;
            const Lua::UiNode* god = section && section->Children.size() == 2 ? model.Find(section->Children[1]) : nullptr;
            const Lua::UiNode* grid = section && !section->Children.empty() ? model.Find(section->Children[0]) : nullptr;
            const int health = grid && grid->Children.size() == 6 ? grid->Children[1] : 0;
            CHECK(god && god->Kind == UiKind::Checkbox && health);
            if (god && health)
            {
                model.Post({ god->Id, "change", { true } });
                engine.Lua().Tick(3000.0);
                const Lua::UiNode* slider = model.Find(health);
                CHECK(slider && !slider->Enabled);
            }
            CHECK(engine.Lua().Run("", "@" + WideToUtf8(demo.wstring())).Ok());
            CHECK(!FindWindow(model, "Player Tools"));
        }

        void UiFormsStage2(Engine& engine)
        {
            using Lua::UiKind;
            Lua::FormDocument form = SampleForm();
            const Lua::UiNode* window = form.Model.Find(form.Window);
            const int tabs = window->Children[1];
            const int tab = form.Model.Find(tabs)->Children[0];

            const std::string whole = Lua::FormFragment(form, { tabs });
            CHECK(whole.find("fragment = true") != std::string::npos);
            Lua::FormDocument other = Lua::NewForm("Other");
            std::string error;
            std::vector<int> made = Lua::FormPaste(other, whole, other.Window, -1, error);
            CHECK_MSG(made.size() == 1 && error.empty(), error);
            made = Lua::FormPaste(other, whole, other.Window, 0, error);
            CHECK(made.size() == 1 && other.Model.Find(other.Window)->Children.front() == made.front());
            std::set<std::string> names;
            for (const auto& [id, meta] : other.Meta)
                if (!meta.Name.empty())
                    CHECK_MSG(names.insert(meta.Name).second, "duplicate name " + meta.Name);
            CHECK(names.count("items") && names.count("items1"));
            CHECK(Lua::FormProblems(other).empty());

            const std::string onlyTab = Lua::FormFragment(form, { tab, form.Model.Find(tab)->Children[0] });
            Lua::FormDocument tabsForm = Lua::NewForm("Tabs");
            const int holder = Lua::FormAdd(tabsForm, UiKind::Tabs, tabsForm.Window);
            error.clear();
            made = Lua::FormPaste(tabsForm, onlyTab, tabsForm.Window, -1, error);
            CHECK(made.empty() && error.find("cannot hold") != std::string::npos);
            error.clear();
            made = Lua::FormPaste(tabsForm, onlyTab, holder, -1, error);
            CHECK_MSG(made.size() == 1 && tabsForm.Model.Find(made[0])->Kind == UiKind::Tab && tabsForm.Model.Find(made[0])->Children.size() == 2, error);
            error.clear();
            CHECK(Lua::FormPaste(tabsForm, "return 1", holder, -1, error).empty() && !error.empty());

            for (const Lua::FormTemplate& entry : Lua::FormTemplates())
            {
                Lua::FormDocument loaded;
                std::vector<std::string> notes;
                const bool ok = Lua::FormLoad(entry.Text, loaded, error, &notes);
                CHECK_MSG(ok && notes.empty() && loaded.Fragment == entry.Snippet, std::string(entry.Id) + ": " + error);
                if (!entry.Snippet)
                    CHECK_MSG(Lua::FormProblems(loaded).empty(), std::string(entry.Id) + " has problems");
                CHECK_MSG(engine.Lua().Run(Lua::FormScript(loaded, Lua::FormScriptStyle::Standalone), "form template").Ok(), std::string(entry.Id) + " script");
            }
            CHECK(engine.Lua().Run("", "form template").Ok());

            const std::string script = Lua::FormScript(form, Lua::FormScriptStyle::FormFile, "Player.form.lua");
            Lua::FormMerge same = Lua::FormMergeScript(script, form);
            CHECK(same.Anchored && same.Added.empty() && same.Text == script);
            std::string trimmed = script;
            const std::string stub = "function handlers.itemMenu(choice, text, row, widget)\nend\n\n";
            CHECK(trimmed.find(stub) != std::string::npos);
            trimmed.erase(trimmed.find(stub), stub.size());
            trimmed += "\nhandlers[\"godChanged\"] = handlers[\"godChanged\"]\n";
            trimmed = "-- my own comment\n" + trimmed;
            Lua::FormMerge merged = Lua::FormMergeScript(trimmed, form);
            CHECK(merged.Anchored && merged.Added == std::vector<std::string>({ "itemMenu" }));
            CHECK(merged.Text.find("-- my own comment") == 0 && merged.Text.find(stub + "window, w = ui.form.load") != std::string::npos);
            CHECK(!Lua::FormMergeScript("print('no form here')", form).Anchored);

            Lua::FormDocument pictured;
            CHECK(Lua::FormLoad("return { form = 1, title = 'Pictures', children = { { kind = 'image', image = 'dot.png', width = 32 }, { kind = 'image', image = 'missing.png' } } }", pictured, error));
            const Lua::UiNode* picture = pictured.Model.Find(pictured.Model.Find(pictured.Window)->Children[0]);
            CHECK(picture && picture->Source == "dot.png" && picture->ImageId == 0);
            const std::string pictureText = Lua::FormSave(pictured);
            CHECK(pictureText.find("image = \"dot.png\"") != std::string::npos);
            CHECK(Lua::FormScript(pictured, Lua::FormScriptStyle::Standalone).find("image = ui.image.load(\"dot.png\")") != std::string::npos);

            static const unsigned char kDot[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x64, 0x60, 0xF8, 0x5F, 0x0F, 0x00, 0x02, 0x87, 0x01, 0x80, 0xEB, 0x47, 0xBA, 0x92, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82 };
            const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("manifold-form-images-" + std::to_string(::GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            const std::filesystem::path dot = folder / "dot.png";
            const std::filesystem::path formFile = folder / "Pictures.form.lua";
            const std::filesystem::path scriptFile = folder / "Pictures.lua";
            std::ofstream(dot, std::ios::binary).write(reinterpret_cast<const char*>(kDot), sizeof(kDot));
            std::ofstream(formFile, std::ios::binary) << pictureText;
            std::ofstream(scriptFile, std::ios::binary) << "local window, w = manifold.ui.form.load('Pictures.form.lua')\n";
            const Status ran = engine.Lua().RunFile(scriptFile);
            CHECK_MSG(ran.Ok(), ran.Text);
            Lua::UiModel& model = engine.Lua().Ui();
            const Lua::UiNode* shown = FindWindow(model, "Pictures");
            CHECK(shown && shown->Children.size() == 2);
            if (shown && shown->Children.size() == 2)
            {
                const Lua::UiNode* first = model.Find(shown->Children[0]);
                const Lua::UiNode* second = model.Find(shown->Children[1]);
                CHECK(first && first->ImageId > 0 && model.FindImage(first->ImageId) && model.FindImage(first->ImageId)->Width == 1);
                CHECK(second && second->ImageId == 0);
            }
            CHECK(engine.Lua().Run("", "@" + WideToUtf8(std::filesystem::absolute(scriptFile).wstring())).Ok());
            CHECK(!FindWindow(model, "Pictures"));
            std::filesystem::remove(dot, ignored);
            std::filesystem::remove(formFile, ignored);
            std::filesystem::remove(scriptFile, ignored);
            std::filesystem::remove(folder, ignored);
        }

        struct LrsWriter
        {
            std::vector<std::uint8_t> Bytes;

            void Byte(int value) { Bytes.push_back(static_cast<std::uint8_t>(value)); }
            void Short(std::string_view text)
            {
                Byte(static_cast<int>(text.size()));
                Bytes.insert(Bytes.end(), text.begin(), text.end());
            }
            void Name(std::string_view text) { Short(text); }
            void Int(std::string_view name, std::int32_t value)
            {
                Name(name);
                Byte(4);
                for (int i = 0; i < 4; ++i)
                    Byte(static_cast<int>((static_cast<std::uint32_t>(value) >> (i * 8)) & 0xFF));
            }
            void Small(std::string_view name, int value)
            {
                Name(name);
                Byte(2);
                Byte(value);
            }
            void Str(std::string_view name, std::string_view value)
            {
                Name(name);
                Byte(6);
                Short(value);
            }
            void Ident(std::string_view name, std::string_view value)
            {
                Name(name);
                Byte(7);
                Short(value);
            }
            void Bool(std::string_view name, bool value)
            {
                Name(name);
                Byte(value ? 9 : 8);
            }
            void Strings(std::string_view name, std::initializer_list<std::string_view> items)
            {
                Name(name);
                Byte(1);
                for (std::string_view item : items)
                {
                    Byte(6);
                    Short(item);
                }
                Byte(0);
            }
            void Set(std::string_view name, std::initializer_list<std::string_view> items)
            {
                Name(name);
                Byte(11);
                for (std::string_view item : items)
                    Short(item);
                Byte(0);
            }
            void Extended(std::string_view name)
            {
                Name(name);
                Byte(5);
                const std::uint8_t onePointFive[10] = { 0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0x3F };
                Bytes.insert(Bytes.end(), onePointFive, onePointFive + 10);
            }
            void Begin(std::string_view type, std::string_view name, bool childPos = false)
            {
                if (childPos)
                {
                    Byte(0xF2);
                    Byte(2);
                    Byte(3);
                }
                Short(type);
                Short(name);
            }
            void End() { Byte(0); }
            void Box(int left, int top, int width, int height)
            {
                Int("Left", left);
                Int("Top", top);
                Int("Width", width);
                Int("Height", height);
            }
        };

        std::vector<std::uint8_t> TrainerStream()
        {
            LrsWriter w;
            w.Bytes = { 'T', 'P', 'F', '0' };
            w.Begin("TCEForm", "UDF1");
            w.Str("Caption", "Trainer");
            w.Int("ClientWidth", 300);
            w.Int("ClientHeight", 260);
            w.Ident("OnClose", "UDF1Close");
            w.Ident("OnShow", "UDF1Show");
            w.Extended("DesignTimePPI");
            w.End();
            w.Begin("TCELabel", "CELabel1");
            w.Box(8, 8, 40, 15);
            w.Str("Caption", "Health");
            w.End();
            w.End();
            w.Begin("TCEEdit", "CEEdit1", true);
            w.Box(60, 6, 100, 23);
            w.Str("Text", "100");
            w.Ident("OnChange", "HealthChange");
            w.End();
            w.End();
            w.Begin("TCELabel", "CELabel2");
            w.Box(8, 40, 40, 15);
            w.Str("Caption", "Speed");
            w.End();
            w.End();
            w.Begin("TCETrackBar", "CETrackBar1");
            w.Box(60, 36, 120, 25);
            w.Small("Max", 10);
            w.Small("Position", 3);
            w.Ident("OnChange", "SpeedChange");
            w.End();
            w.End();
            w.Begin("TCECheckBox", "CECheckBox1");
            w.Box(8, 80, 100, 19);
            w.Str("Caption", "God");
            w.Bool("Checked", true);
            w.Ident("OnClick", "GodClick");
            w.Ident("OnMouseDown", "GodDown");
            w.End();
            w.End();
            w.Begin("TCEButton", "CEButton1");
            w.Box(8, 120, 75, 25);
            w.Str("Caption", "Apply");
            w.Ident("OnClick", "ApplyClick");
            w.End();
            w.End();
            w.Begin("TCEButton", "CEButton2");
            w.Box(100, 120, 75, 25);
            w.Str("Caption", "Cancel");
            w.Bool("Enabled", false);
            w.End();
            w.End();
            w.Begin("TCEListView", "CEListView1");
            w.Box(8, 160, 280, 60);
            w.Name("Columns");
            w.Byte(14);
            w.Byte(1);
            w.Str("Caption", "Name");
            w.Int("Width", 80);
            w.End();
            w.Byte(2);
            w.Byte(1);
            w.Byte(1);
            w.Str("Caption", "Value");
            w.End();
            w.End();
            w.End();
            w.End();
            w.Begin("TCEGroupBox", "CEGroupBox1");
            w.Ident("Align", "alBottom");
            w.Int("Height", 60);
            w.Str("Caption", "More");
            w.End();
            w.Begin("TCELabel", "CELabel3");
            w.Box(8, 4, 60, 15);
            w.Str("Caption", "Title");
            w.Set("Font.Style", { "fsBold" });
            w.End();
            w.End();
            w.Begin("TCEComboBox", "CEComboBox1");
            w.Box(8, 26, 120, 23);
            w.Strings("Items.Strings", { "A", "B" });
            w.Small("ItemIndex", 1);
            w.End();
            w.End();
            w.End();
            w.Begin("TCETimer", "CETimer1");
            w.Int("Interval", 500);
            w.Ident("OnTimer", "Tick");
            w.End();
            w.End();
            w.End();
            return w.Bytes;
        }

        void UiFormImport(Engine& engine)
        {
            using Lua::UiKind;
            const std::vector<std::uint8_t> stream = TrainerStream();
            std::string error;
            std::optional<Table::CeComponent> root = Table::ReadCeComponents(stream, error);
            CHECK_MSG(root.has_value(), error);
            if (!root)
                return;
            CHECK(root->Class == "TCEForm" && root->Name == "UDF1" && root->Children.size() == 10);
            const Table::CeValue* ppi = root->Find("DesignTimePPI");
            CHECK(ppi && ppi->Type == Table::CeValue::Kind::Real && std::abs(ppi->Real - 1.5) < 1e-9);
            std::vector<std::uint8_t> cut(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(stream.size() / 2));
            CHECK(!Table::ReadCeComponents(cut, error) && error.find("ends early") != std::string::npos);
            CHECK(!Table::ReadCeComponents(std::vector<std::uint8_t>{ 'X', 'Y', 'Z', 'W' }, error));

            const std::string packed = Table::EncodeTableFile(stream);
            const std::string frm = "<?xml version=\"1.0\"?>\n<FormData>\n  <UDF1 Encoding=\"Ascii85\" Class=\"TCEForm\">" + packed + "</UDF1>\n</FormData>\n";
            std::vector<Table::CeFormEntry> forms = Table::ReadCeFormFile(frm, error);
            CHECK(forms.size() == 1 && forms[0].Root && forms[0].Name == "UDF1");

            std::vector<std::uint8_t> plain(4 + stream.size());
            for (int i = 0; i < 4; ++i)
                plain[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(stream.size() >> (i * 8));
            std::copy(stream.begin(), stream.end(), plain.begin() + 4);
            const auto deflated = Deflate::Compress(plain);
            std::string hex;
            for (const std::uint8_t byte : *deflated)
            {
                char digits[3];
                std::snprintf(digits, sizeof(digits), "%02X", byte);
                hex += digits;
            }
            const std::string table = "<?xml version=\"1.0\"?>\n<CheatTable CheatEngineTableVersion=\"45\">\n  <Forms>\n    <UDF1 Encoding=\"Ascii85\" Class=\"TCEForm\">" + packed +
                                      "</UDF1>\n    <UDF2 Class=\"TCEForm\">" + hex + "</UDF2>\n    <UDF3 Encoding=\"Ascii85\" Class=\"TCEForm\">!!!</UDF3>\n  </Forms>\n</CheatTable>\n";
            forms = Table::ReadCeFormFile(table, error);
            CHECK(forms.size() == 3 && forms[0].Root && forms[1].Root && !forms[2].Root && !forms[2].Error.empty());
            CHECK(Table::ReadCeFormFile("<CheatTable><CheatEntries/></CheatTable>", error).empty() && error.find("no Cheat Engine forms") != std::string::npos);
            CHECK(Table::ReadCeTableForms({ "<Forms><UDF1 Encoding=\"Ascii85\">" + packed + "</UDF1></Forms>", "<LuaScript/>" }).size() == 1);

            Lua::FormImport imported = Lua::FormFromCe(*root);
            Lua::FormDocument& form = imported.Form;
            const Lua::UiNode* window = form.Model.Find(form.Window);
            CHECK(window && window->Title == "Trainer" && form.Key == "UDF1" && std::abs(window->Interval - 0.5) < 1e-9);
            CHECK(form.Meta[form.Window].Handlers["onClose"] == "UDF1Close" && form.Meta[form.Window].Handlers["onUpdate"] == "Tick");
            CHECK(imported.Converted == 11);
            const auto noted = [&](std::string_view needle) {
                return std::any_of(imported.Notes.begin(), imported.Notes.end(), [&](const std::string& note) { return note.find(needle) != std::string::npos; });
            };
            CHECK(noted("OnMouseDown (GodDown)") && noted("OnShow (UDF1Show)") && noted("became the window's onUpdate") && noted("list view"));
            CHECK(window && window->Children.size() == 5);
            if (window && window->Children.size() == 5)
            {
                const Lua::UiNode* grid = form.Model.Find(window->Children[0]);
                CHECK(grid && grid->Kind == UiKind::Grid && grid->Children.size() == 4);
                if (grid && grid->Children.size() == 4)
                {
                    const Lua::UiNode* health = form.Model.Find(grid->Children[1]);
                    CHECK(form.Model.Find(grid->Children[0])->Text == "Health");
                    CHECK(health && health->Kind == UiKind::Input && health->String == "100" && health->Width == -1.0f);
                    CHECK(form.Meta[grid->Children[1]].Name == "CEEdit1" && form.Meta[grid->Children[1]].Handlers["onChange"] == "HealthChange");
                    const Lua::UiNode* speed = form.Model.Find(grid->Children[3]);
                    CHECK(speed && speed->Kind == UiKind::Slider && speed->Number == 3.0 && speed->Max == 10.0 && speed->Integer);
                }
                const Lua::UiNode* god = form.Model.Find(window->Children[1]);
                CHECK(god && god->Kind == UiKind::Checkbox && god->Bool && god->Label == "God" && form.Meta[god->Id].Handlers["onChange"] == "GodClick");
                const Lua::UiNode* buttons = form.Model.Find(window->Children[2]);
                CHECK(buttons && buttons->Kind == UiKind::Row && buttons->Children.size() == 2);
                if (buttons && buttons->Children.size() == 2)
                    CHECK(!form.Model.Find(buttons->Children[1])->Enabled && form.Meta[buttons->Children[0]].Handlers["onClick"] == "ApplyClick");
                const Lua::UiNode* list = form.Model.Find(window->Children[3]);
                CHECK(list && list->Kind == UiKind::Table && list->Columns.size() == 2 && list->Columns[0].Title == "Name" && list->Columns[0].Width == 80.0f && list->Columns[1].Title == "Value");
                const Lua::UiNode* more = form.Model.Find(window->Children[4]);
                CHECK(more && more->Kind == UiKind::Section && more->Title == "More" && more->Children.size() == 2);
                if (more && more->Children.size() == 2)
                {
                    CHECK(form.Model.Find(more->Children[0])->Kind == UiKind::Heading);
                    const Lua::UiNode* combo = form.Model.Find(more->Children[1]);
                    CHECK(combo && combo->Kind == UiKind::Combo && combo->Items.size() == 2 && combo->Selected == 2);
                }
            }
            CHECK(Lua::FormProblems(form).empty());
            const std::string saved = Lua::FormSave(form);
            Lua::FormDocument again;
            CHECK_MSG(Lua::FormLoad(saved, again, error), error);
            CHECK(engine.Lua().Run(Lua::FormScript(form, Lua::FormScriptStyle::Standalone), "form import").Ok());
            CHECK(FindWindow(engine.Lua().Ui(), "Trainer"));
            CHECK(engine.Lua().Run("", "form import").Ok());
        }

        void UiFormReload(Engine& engine)
        {
            Lua::UiModel& model = engine.Lua().Ui();
            const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("manifold-form-reload-" + std::to_string(::GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            const std::filesystem::path formFile = folder / "Live.form.lua";
            const std::filesystem::path otherFile = folder / "Fixed.form.lua";
            const std::filesystem::path scriptFile = folder / "Live.lua";
            const auto write = [](const std::filesystem::path& file, std::string_view text) { std::ofstream(file, std::ios::binary | std::ios::trunc) << text; };
            write(formFile, R"LUA(return { form = 1, title = "Live", key = "live form", children = {
              { kind = "checkbox", name = "god", label = "God", onChange = "godChanged" },
              { kind = "input", name = "name", value = "Hero" },
            } })LUA");
            write(otherFile, R"LUA(return { form = 1, title = "Fixed", children = { { kind = "text", text = "one" } } })LUA");
            write(scriptFile, R"LUA(
              liveReloads = 0
              local handlers = { godChanged = function() end, saveClicked = function() end }
              liveWindow, liveWidgets = manifold.ui.form.load("Live.form.lua", handlers, { onReload = function(window, widgets) liveReloads = liveReloads + 1; liveSeen = widgets.save.kind end })
              fixedWindow = manifold.ui.form.load("Fixed.form.lua", nil, { reload = false })
              liveWidgets.god.set(true)
              liveFirstId = liveWindow.id
            )LUA");
            const Status ran = engine.Lua().RunFile(scriptFile);
            CHECK_MSG(ran.Ok(), ran.Text);
            const std::string shown = WideToUtf8(std::filesystem::absolute(formFile).wstring());
            CHECK_EQ(model.Watching(shown), 1);
            CHECK_EQ(model.Watching(WideToUtf8(std::filesystem::absolute(otherFile).wstring())), 0);
            const Lua::UiNode* live = FindWindow(model, "Live");
            CHECK(live && live->Children.size() == 2);

            write(formFile, R"LUA(return { form = 1, title = "Live Two", key = "live form", children = {
              { kind = "checkbox", name = "god", label = "God Mode", onChange = "godChanged" },
              { kind = "row", children = { { kind = "button", name = "save", text = "Save", onClick = "saveClicked" } } },
            } })LUA");
            write(otherFile, R"LUA(return { form = 1, title = "Fixed", children = { { kind = "text", text = "one" }, { kind = "text", text = "two" } } })LUA");
            engine.Lua().Tick(5000.0);
            live = FindWindow(model, "Live Two");
            CHECK(live && live->Children.size() == 2);
            if (live && live->Children.size() == 2)
            {
                const Lua::UiNode* god = model.Find(live->Children[0]);
                CHECK(god && god->Kind == Lua::UiKind::Checkbox && god->Label == "God Mode" && god->Bool);
            }
            const Lua::UiNode* fixed = FindWindow(model, "Fixed");
            CHECK(fixed && fixed->Children.size() == 1);
            LuaCheck(engine, "form reload", R"LUA(
                if liveReloads ~= 1 or liveSeen ~= "button" then return "onReload " .. tostring(liveReloads) end
                if liveWindow.id ~= liveFirstId or not liveWindow.isOpen() then return "window changed" end
                if liveWidgets.save.kind ~= "button" or liveWidgets.god.get() ~= true then return "widgets table" end
                local ok = pcall(function() return liveWidgets.name end)
                if ok then return "a removed widget is still there" end
                return "ok")LUA");

            const std::size_t warnings = CountEntries(engine, LogLevel::Warning, "keeps its layout");
            write(formFile, R"LUA(return { form = 1, title = "Broken", children = { { kind = "button", name = "x", onClick = "nope" } } })LUA");
            engine.Lua().Tick(6000.0);
            CHECK(FindWindow(model, "Live Two") && !FindWindow(model, "Broken"));
            CHECK_EQ(CountEntries(engine, LogLevel::Warning, "keeps its layout"), warnings + 1);

            CHECK(engine.Lua().Run("", "@" + WideToUtf8(std::filesystem::absolute(scriptFile).wstring())).Ok());
            CHECK(!FindWindow(model, "Live Two") && !FindWindow(model, "Fixed"));
            CHECK_EQ(model.Watching(shown), 0);
            std::filesystem::remove(formFile, ignored);
            std::filesystem::remove(otherFile, ignored);
            std::filesystem::remove(scriptFile, ignored);
            std::filesystem::remove(folder, ignored);
        }

        void UiWindowOptions(Engine& engine)
        {
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "ui window options", R"LUA(
                local w = manifold.ui.window.create("Options", { key = "options probe", minWidth = 300, minHeight = 200, maxWidth = 900, maxHeight = 700,
                  resizable = false, scrollbar = false, topmost = true, position = "center" })
                local list = w.add("column", { gap = 2, scroll = true, height = 120 })
                list.add("button", { text = "Save", shortcut = "Ctrl+S", confirm = "Save now?", minWidth = 120, maxWidth = 240 })
                list.add("input", { value = "abc", readOnly = true, maxLength = 5, focus = true })
                list.add("text", { text = "long", maxWidth = 80 })
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  if not problem.message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. problem.message end
                end
                local failure = raises(function() manifold.ui.window.create("Bad", { position = "top" }) end, 'options.position must be "center" or "mouse"')
                  or raises(function() list.add("input", { maxLength = 2.5 }) end, "options.maxLength must be")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", position = "left" }) end, "form.position must be")
                if failure then return failure end
                if not w.update({ maxWidth = 1000, topmost = false, resizable = true }) then return "update" end
                return "ok")LUA");
            const Lua::UiNode* window = FindWindow(model, "Options");
            CHECK(window && window->Key == "options probe" && window->MinWidth == 300.0f && window->MinHeight == 200.0f && window->MaxWidth == 1000.0f &&
                  window->MaxHeight == 700.0f && window->Resizable && !window->Scrollbar && !window->Topmost && window->Position == "center");
            CHECK(!FindWindow(model, "Bad"));
            const Lua::UiNode* list = window && !window->Children.empty() ? model.Find(window->Children[0]) : nullptr;
            CHECK(list && list->Gap == 2.0f && list->Scroll && list->Height == 120.0f && list->Children.size() == 3);
            if (list && list->Children.size() == 3)
            {
                const Lua::UiNode* button = model.Find(list->Children[0]);
                CHECK(button && button->Shortcut == "Ctrl+S" && button->Confirm == "Save now?" && button->MinWidth == 120.0f && button->MaxWidth == 240.0f);
                const Lua::UiNode* input = model.Find(list->Children[1]);
                CHECK(input && input->ReadOnly && input->MaxLength == 5 && input->Focus);
                const Lua::UiNode* text = model.Find(list->Children[2]);
                CHECK(text && text->MaxWidth == 80.0f);
            }
            CHECK(ParseChord("Ctrl+S") != 0);
            LuaCheck(engine, "ui window options cleanup", R"LUA(
                manifold.ui.window.create("x", { key = "options probe" }).remove()
                return "ok")LUA");

            const std::string text = R"LUA(return {
  form = 1,
  title = "Options",
  minWidth = 300,
  maxHeight = 600,
  resizable = false,
  topmost = true,
  position = "mouse",
  children = {
    { kind = "column", gap = 4, scroll = true, children = { { kind = "button", text = "Go", shortcut = "F5", confirm = "Sure?" } } },
    { kind = "input", readOnly = true, maxLength = 8, focus = true, minWidth = 100 },
  },
})LUA";
            Lua::FormDocument form;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoad(text, form, error, &notes) && notes.empty(), error);
            const Lua::UiNode* root = form.Model.Find(form.Window);
            CHECK(root && root->MinWidth == 300.0f && root->MaxHeight == 600.0f && !root->Resizable && root->Topmost && root->Position == "mouse");
            const std::string saved = Lua::FormSave(form);
            for (const char* needle : { "minWidth = 300", "maxHeight = 600", "resizable = false", "topmost = true", "position = \"mouse\"", "gap = 4", "scroll = true",
                     "shortcut = \"F5\"", "confirm = \"Sure?\"", "readOnly = true", "maxLength = 8", "focus = true", "minWidth = 100" })
                CHECK_MSG(saved.find(needle) != std::string::npos, std::string("saved form lacks ") + needle);
            Lua::FormDocument again;
            CHECK(Lua::FormLoad(saved, again, error) && Lua::FormSave(again) == saved);
            CHECK(engine.Lua().Run(Lua::FormScript(form, Lua::FormScriptStyle::Standalone), "form options").Ok());
            CHECK(FindWindow(model, "Options"));
            CHECK(engine.Lua().Run("", "form options").Ok());
        }

        void UiFormLinks(Engine& engine)
        {
            Lua::FormDocument form = SampleForm();
            form.Script = "Player.lua";
            const std::string saved = Lua::FormSave(form);
            CHECK(saved.find("script = \"Player.lua\"") != std::string::npos);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoad(saved, loaded, error, &notes) && notes.empty() && loaded.Script == "Player.lua", error);
            LuaCheck(engine, "form script key", R"LUA(
                local made = manifold.ui.form.build({ form = 1, title = "Linked", script = "Linked.lua", children = { { kind = "text", text = "x" } } })
                made.remove()
                return "ok")LUA");

            const std::string standalone = Lua::FormScript(form, Lua::FormScriptStyle::Standalone);
            const std::string stub = "local function itemMenu(choice, text, row, widget)\nend\n\n";
            std::string trimmed = standalone;
            CHECK(trimmed.find(stub) != std::string::npos);
            trimmed.erase(trimmed.find(stub), stub.size());
            CHECK_EQ(Lua::FormFunctionLine(trimmed, "itemMenu"), 0);
            const Lua::FormMerge merged = Lua::FormMergeScript(trimmed, form);
            CHECK(merged.Anchored && merged.Added == std::vector<std::string>({ "itemMenu" }));
            CHECK(merged.Text.find(stub + "window = ui.window.create(") != std::string::npos);
            const int line = Lua::FormFunctionLine(merged.Text, "itemMenu");
            CHECK(line > 0);
            if (line > 0)
            {
                std::size_t at = 0;
                for (int i = 1; i < line && at != std::string::npos; ++i)
                    at = merged.Text.find('\n', at) + 1;
                CHECK(merged.Text.compare(at, std::strlen("local function itemMenu("), "local function itemMenu(") == 0);
            }
            const std::string withFile = Lua::FormScript(form, Lua::FormScriptStyle::FormFile, "Player.form.lua");
            CHECK(Lua::FormFunctionLine(withFile, "saveClicked") > 0);
            CHECK_EQ(Lua::FormFunctionLine("handlers[\"godChanged\"] = function(value) end", "godChanged"), 1);
            CHECK_EQ(Lua::FormFunctionLine("-- nothing\nlocal x = 1\n", "godChanged"), 0);
            CHECK_EQ(Lua::FormFunctionLine("function handlers.saveClickedTwice() end", "saveClicked"), 0);
            CHECK(engine.Lua().Run(merged.Text, "form link").Ok());
            CHECK(engine.Lua().Run("", "form link").Ok());
        }

        void UiFormCanvas(Engine& engine)
        {
            // Function scanner: strings, comments and nested blocks never end a function early.
            const std::string script =
                "local handlers = {}\n"
                "function handlers.saveClicked(widget)\n"
                "    local s = \"end\" .. [[end]] -- end\n"
                "    if widget then\n"
                "        for i = 1, 2 do print(i) end\n"
                "    end\n"
                "    repeat local x = 1 until true\n"
                "    local f = function() return 1 end\n"
                "end\n"
                "--[[ function handlers.ghost() end ]]\n"
                "local function quick(a, b) return a end\n"
                "handlers.later = function(value)\n"
                "end\n"
                "manifold.ui.form.load(\"x.form.lua\", handlers)\n";
            const auto save = Lua::FormFindFunction(script, "saveClicked");
            CHECK(save.has_value());
            if (save)
            {
                CHECK_EQ(save->Params, std::string("widget"));
                CHECK_EQ(script.compare(save->End - 3, 3, "end"), 0);
                CHECK(script.substr(save->End).rfind("\n--[[", 0) == 0);
            }
            CHECK(!Lua::FormFindFunction(script, "ghost").has_value());
            CHECK_EQ(Lua::FormFunctionLine(script, "quick"), 11);
            CHECK_EQ(Lua::FormFunctionLine(script, "later"), 12);
            CHECK_EQ(Lua::FormFunctionBody(script, "quick"), std::string("return a"));
            const auto replaced = Lua::FormReplaceBody(script, "later", "    print(value)");
            CHECK(replaced.has_value());
            if (replaced)
            {
                CHECK(replaced->find("handlers.later = function(value)\n    print(value)\nend\nmanifold.ui.form.load") != std::string::npos);
            }
            CHECK(!Lua::FormReplaceBody(script, "missing", "x").has_value());

            // Editor layout: form place and chip places survive save + load; the runtime ignores them.
            Lua::FormDocument form = SampleForm();
            form.CanvasX = 120.0f;
            form.CanvasY = 64.0f;
            form.Nodes["saveClicked"] = { 400.0f, 80.0f, false };
            form.Nodes["godChanged"] = { 410.0f, 140.0f, true };
            const std::string saved = Lua::FormSave(form);
            CHECK(saved.find("editor = {") != std::string::npos);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoad(saved, loaded, error, &notes) && notes.empty(), error);
            CHECK_EQ(loaded.CanvasX, 120.0f);
            CHECK_EQ(loaded.CanvasY, 64.0f);
            CHECK_EQ(loaded.Nodes.size(), std::size_t(2));
            CHECK(loaded.Nodes.count("godChanged") && loaded.Nodes["godChanged"].Folded && loaded.Nodes["godChanged"].Y == 140.0f);
            CHECK(Lua::FormSave(SampleForm()).find("editor =") == std::string::npos);
            LuaCheck(engine, "form editor key", R"LUA(
                local made = manifold.ui.form.build({ form = 1, title = "Placed", editor = { form = { x = 1, y = 2 }, nodes = { a = { x = 3, y = 4 } } }, children = { { kind = "text", text = "x" } } })
                made.remove()
                return "ok")LUA");
        }

        void UiFormModals(Engine& engine)
        {
            // The document: a modal with a closing button, a button that opens it, save, load, problems, code.
            Lua::FormDocument form = SampleForm();
            const int modal = Lua::FormAddModal(form, "confirm");
            CHECK(modal > 0 && Lua::FormIsModal(form, modal) && Lua::FormModalName(form, modal) == "confirm");
            CHECK_EQ(Lua::FormWindows(form).size(), std::size_t(2));
            int opener = 0;
            for (const auto& [id, meta] : form.Meta)
            {
                if (meta.Name == "save")
                    opener = id;
            }
            CHECK(opener > 0);
            form.Meta[opener].Opens = "confirm";
            form.ModalNodes["confirm"] = { 500.0f, 400.0f, false };
            CHECK(Lua::FormProblems(form).empty());
            const std::string saved = Lua::FormSave(form);
            CHECK(saved.find("modals = {") != std::string::npos);
            CHECK(saved.find("name = \"confirm\"") != std::string::npos);
            CHECK(saved.find("opens = \"confirm\"") != std::string::npos);
            CHECK(saved.find("closes = true") != std::string::npos);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoad(saved, loaded, error, &notes) && notes.empty(), error + (notes.empty() ? std::string() : " / " + notes.front()));
            CHECK_EQ(loaded.Modals.size(), std::size_t(1));
            if (loaded.Modals.size() == 1)
            {
                const int again = loaded.Modals.front();
                CHECK(Lua::FormModalName(loaded, again) == "confirm" && loaded.Model.Find(again)->Modal);
                CHECK(Lua::FormRootWindow(loaded, loaded.Model.Find(again)->Children.back()) == again);
            }
            CHECK(loaded.ModalNodes.count("confirm") && loaded.ModalNodes["confirm"].Y == 400.0f);
            bool opens = false;
            bool closes = false;
            for (const auto& [id, meta] : loaded.Meta)
            {
                opens = opens || meta.Opens == "confirm";
                closes = closes || meta.Closes;
            }
            CHECK(opens && closes);
            CHECK(Lua::FormSave(loaded) == saved);

            Lua::FormDocument renamed = loaded;
            CHECK(Lua::FormRenameModal(renamed, renamed.Modals.front(), "sure"));
            CHECK(Lua::FormSave(renamed).find("opens = \"sure\"") != std::string::npos && renamed.ModalNodes.count("sure"));
            Lua::FormDocument broken = loaded;
            for (auto& [id, meta] : broken.Meta)
            {
                if (meta.Opens == "confirm")
                    meta.Opens = "missing";
            }
            const std::vector<Lua::FormProblem> problems = Lua::FormProblems(broken);
            CHECK(std::any_of(problems.begin(), problems.end(), [](const Lua::FormProblem& p) { return p.Text.find("no modal of that name") != std::string::npos; }));
            Lua::FormDocument removed = loaded;
            CHECK(Lua::FormRemove(removed, removed.Modals.front()) && removed.Modals.empty() && removed.ModalNodes.empty());
            CHECK(Lua::FormSave(removed).find("opens =") == std::string::npos);

            const std::string withFile = Lua::FormScript(loaded, Lua::FormScriptStyle::FormFile, "Player.form.lua");
            CHECK(withFile.find("window, w, modals = ui.form.load(\"Player.form.lua\", handlers)") != std::string::npos);
            const std::string standalone = Lua::FormScript(loaded, Lua::FormScriptStyle::Standalone);
            CHECK(standalone.find("modals.confirm = ui.window.create(\"Dialog\", { modal = true, open = false") != std::string::npos);
            CHECK(standalone.find("modals.confirm.hide() end") != std::string::npos);
            CHECK(standalone.find("modals.confirm.show() saveClicked(widget) end") != std::string::npos);
            CHECK_MSG(engine.Lua().Run(standalone + "\nfor _, made in pairs(modals) do made.remove() end\nwindow.remove()\n", "form modal standalone").Ok(), standalone);

            // The runtime: modal windows wait hidden, opens / closes work, wrong forms raise.
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "form modals", R"LUA(
                modalClicks = 0
                local handlers = { saveClicked = function() modalClicks = modalClicks + 1 end }
                modalWindow, modalWidgets, modalTable = manifold.ui.form.build({ form = 1, title = "Main", children = {
                    { kind = "button", name = "save", text = "Save", onClick = "saveClicked", opens = "confirm" },
                  }, modals = {
                    { name = "confirm", title = "Sure?", width = 300, children = {
                      { kind = "text", text = "Really?" },
                      { kind = "button", name = "yes", text = "Yes", closes = true },
                    } },
                  } }, handlers)
                if not modalTable.confirm or modalTable.confirm.kind ~= "window" then return "no modal table" end
                if modalTable.confirm.isOpen() then return "the modal opened on its own" end
                if modalWidgets.yes.kind ~= "button" then return "modal widgets missing" end
                modalSave, modalYes = modalWidgets.save.id, modalWidgets.yes.id
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  local text = type(problem) == "table" and problem.message or tostring(problem)
                  if not text:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. text end
                end
                local failure = raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "button", opens = "nope" } } }) end, "must name one of the form's modals")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "text", closes = true } } }) end, "only a button opens or closes")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", modals = { { name = "a", title = "A" }, { name = "a", title = "B" } } }) end, "two modal forms are named")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", modal = true }) end, "form.modal is not a window option")
                if failure then return failure end
                return "ok")LUA");
            CHECK(!FindWindow(model, "Bad"));
            const Lua::UiNode* sure = FindWindow(model, "Sure?");
            CHECK(sure && sure->Modal && !sure->Open);
            int save = 0;
            int yes = 0;
            {
                std::string result;
                engine.Lua().RunWithResult("return tostring(modalSave) .. ' ' .. tostring(modalYes)", "form modal ids", result);
                const std::size_t space = result.find(' ');
                if (space != std::string::npos)
                {
                    save = static_cast<int>(ParseDecimal(result.substr(0, space)).value_or(0));
                    yes = static_cast<int>(ParseDecimal(result.substr(space + 1)).value_or(0));
                }
            }
            CHECK(save > 0 && yes > 0);
            model.Post({ save, "click", {} });
            engine.Lua().Tick(7000.0);
            sure = FindWindow(model, "Sure?");
            CHECK(sure && sure->Open);
            model.Post({ yes, "click", {} });
            engine.Lua().Tick(7001.0);
            sure = FindWindow(model, "Sure?");
            CHECK(sure && !sure->Open);
            LuaCheck(engine, "form modals clicked", R"LUA(
                if modalClicks ~= 1 then return "onClick ran " .. tostring(modalClicks) .. " times" end
                modalTable.confirm.remove()
                modalWindow.remove()
                return "ok")LUA");
            CHECK(!FindWindow(model, "Sure?") && !FindWindow(model, "Main"));
        }

        void UiFormOneFile(Engine& engine)
        {
            // The document: the form as a table in its script, the editor's view of it, and back.
            constexpr std::size_t npos = std::string::npos;
            Lua::FormDocument form = SampleForm();
            const std::string view = Lua::FormScript(form, Lua::FormScriptStyle::OneFile);
            CHECK(Lua::FormViewIntact(view));
            CHECK(view.find("window, w = ui.form.build(form, handlers)") != npos);
            CHECK(view.find("function handlers.speedChanged(") != npos);
            const std::optional<std::string> file = Lua::FormComposeScript(view, form);
            CHECK(file.has_value());
            if (!file)
                return;
            CHECK(file->find(std::string(Lua::kFormStandIn)) == npos);
            CHECK(file->find("local form = {\n  form = 1,\n") != npos);
            CHECK(file->find("script =") == npos && file->find("fragment") == npos);
            const std::optional<Lua::FormBlock> block = Lua::FormFindBlock(*file);
            CHECK(block && block->Name == "form" && block->Line == 4);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoadScript(*file, loaded, error, &notes) && notes.empty(), error + (notes.empty() ? std::string() : " / " + notes.front()));
            CHECK(Lua::FormSave(loaded) == Lua::FormSave(form));
            CHECK(Lua::FormScriptView(*file) == view);
            CHECK(Lua::FormComposeScript(Lua::FormScriptView(*file), loaded) == *file);

            // own code around the table is kept; look-alikes are not the form
            const std::string own = "local ui = manifold.ui\nlocal other = { form = \"x}\" }\n--[[ local form = { form = 1 } ]]\n" + *file + "\nprint('done') -- }\n";
            const std::optional<Lua::FormBlock> ownBlock = Lua::FormFindBlock(own);
            CHECK(ownBlock && ownBlock->Line == 7);
            const std::string ownView = Lua::FormScriptView(own);
            CHECK(Lua::FormViewIntact(ownView) && ownView.find("local other = { form = \"x}\" }") != npos && ownView.find("print('done') -- }") != npos);
            CHECK(Lua::FormComposeScript(ownView, loaded) == own);
            {
                // float fields come out as written: 0.4, not 0.4000000059604645
                Lua::FormDocument fraction = Lua::NewForm("Fraction");
                const int bar = Lua::FormAdd(fraction, Lua::UiKind::Progress, fraction.Window);
                Lua::FormSetProp(*fraction.Model.Find(bar), "value", "0.4");
                const std::string text = Lua::FormSave(fraction);
                CHECK_MSG(text.find("value = 0.4") != npos && text.find("0.4000") == npos, text);
            }
            CHECK(!Lua::FormViewIntact(view + "\nlocal again = " + std::string(Lua::kFormStandIn) + "\n"));
            CHECK(!Lua::FormViewIntact("local ui = manifold.ui\n"));
            CHECK(!Lua::FormComposeScript("print(1)\n", form).has_value());
            Lua::FormDocument none;
            CHECK(!Lua::FormLoadScript("local x = { 1, 2 }\n", none, error) && error.find("no form table") != npos);
            CHECK(!Lua::FormLoadScript("local ui = manifold.ui\n\nlocal form = { form = 1, title = = }\n", none, error) && error.find(":3:") != npos);

            // stubs go above form.build in the view
            const Lua::FormMerge merge = Lua::FormMergeScript("local ui = manifold.ui\nlocal form = " + std::string(Lua::kFormStandIn) + "\nlocal window, w\nlocal handlers = {}\n\nwindow, w = ui.form.build(form, handlers)\n", form);
            CHECK(merge.Anchored && !merge.Added.empty() && Lua::FormViewIntact(merge.Text));
            CHECK(merge.Text.find("function handlers.speedChanged(") < merge.Text.find("ui.form.build("));

            // an older script that loads a .form.lua file
            const std::string old = "local ui = manifold.ui\n\nlocal window, w\nlocal handlers = {}\n\nfunction handlers.godChanged(value, widget)\n    print(value)\nend\n\nwindow, w = ui.form.load(\"Player.form.lua\", handlers, { reload = false })\n";
            const std::string converted = Lua::FormConvertScript(old);
            CHECK_MSG(Lua::FormViewIntact(converted), converted);
            CHECK(converted.find("window, w = ui.form.build(form, handlers, { reload = false })") != npos);
            CHECK(converted.find("    print(value)\nend\n") != npos);
            CHECK(converted.rfind("local ui = manifold.ui\n\n-- The form", 0) == 0);
            CHECK(Lua::FormConvertScript("print('no form here')\n").empty());

            // The runtime: the script builds its window from its own table, and saving the file rebuilds it.
            Lua::UiModel& model = engine.Lua().Ui();
            const std::filesystem::path folder = std::filesystem::temp_directory_path() / ("manifold-form-onefile-" + std::to_string(::GetCurrentProcessId()));
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            const std::filesystem::path scriptFile = folder / "Trainer.lua";
            const auto write = [](const std::filesystem::path& path, std::string_view text) { std::ofstream(path, std::ios::binary | std::ios::trunc) << text; };
            Lua::FormDocument trainer = Lua::NewForm("One File");
            trainer.Key = "one file probe";
            const int box = Lua::FormAdd(trainer, Lua::UiKind::Checkbox, trainer.Window);
            trainer.Meta[box].Name = "god";
            trainer.Meta[box].Handlers["onChange"] = "godChanged";
            const int button = Lua::FormAdd(trainer, Lua::UiKind::Button, trainer.Window);
            trainer.Meta[button].Name = "go";
            trainer.Meta[button].Handlers["onClick"] = "goClicked";
            std::string script = Lua::FormComposeScript(Lua::FormScript(trainer, Lua::FormScriptStyle::OneFile), trainer).value_or(std::string());
            script = Lua::FormReplaceBody(script, "goClicked", "    oneFileClicks = (oneFileClicks or 0) + 1\n").value_or(script);
            script += "oneFileWindow, oneFileWidgets = window, w\n"
                      "local extraForm = { form = 1, title = \"One File Extra\", children = {} }\n"
                      "oneFileExtra = ui.form.build(extraForm)\n";
            write(scriptFile, script);
            const Status ran = engine.Lua().RunFile(scriptFile);
            CHECK_MSG(ran.Ok(), ran.Text + "\n" + script);
            const std::string shown = WideToUtf8(scriptFile.wstring());
            CHECK_EQ(model.Watching(shown), 1);
            const Lua::UiNode* probe = FindWindow(model, "One File");
            CHECK(probe && probe->Children.size() == 2 && FindWindow(model, "One File Extra"));

            trainer.Model.Find(trainer.Window)->Title = "One File Two";
            Lua::FormAdd(trainer, Lua::UiKind::Text, trainer.Window);
            const std::string saved = Lua::FormComposeScript(Lua::FormScriptView(script), trainer).value_or(std::string());
            CHECK(saved.find("oneFileClicks = (oneFileClicks or 0) + 1") != npos);
            write(scriptFile, saved);
            engine.Lua().Tick(8000.0);
            probe = FindWindow(model, "One File Two");
            CHECK(probe && probe->Children.size() == 3);
            CHECK(!FindWindow(model, "One File") && FindWindow(model, "One File Extra"));
            int go = 0;
            {
                std::string result;
                engine.Lua().RunWithResult("return tostring(oneFileWidgets.go.id)", "one file ids", result);
                go = static_cast<int>(ParseDecimal(result).value_or(0));
            }
            CHECK(go > 0);
            model.Post({ go, "click", {} });
            engine.Lua().Tick(8001.0);
            LuaCheck(engine, "one file reload", R"LUA(
                if not oneFileWindow.isOpen() then return "window closed" end
                if oneFileWidgets.go.kind ~= "button" then return "widgets table" end
                if oneFileClicks ~= 1 then return "the script's handler ran " .. tostring(oneFileClicks) .. " times" end
                return "ok")LUA");

            CHECK(engine.Lua().Run("", "@" + shown).Ok());
            CHECK(!FindWindow(model, "One File Two") && !FindWindow(model, "One File Extra"));
            CHECK_EQ(model.Watching(shown), 0);
            std::filesystem::remove(scriptFile, ignored);
            std::filesystem::remove(folder, ignored);
        }

        void UiFormRules(Engine& engine)
        {
            // The document: enabledWhen read, written, checked and played; switch style; multi combo.
            const std::optional<Lua::FormRule> plain = Lua::FormParseWhen(" god ");
            CHECK(plain && plain->Source == "god" && !plain->Negate && plain->Values.empty());
            const std::optional<Lua::FormRule> negated = Lua::FormParseWhen("not god");
            CHECK(negated && negated->Negate && Lua::FormWhenText(*negated) == "not god");
            const std::optional<Lua::FormRule> listed = Lua::FormParseWhen("mode = Multiplier ,  No Damage");
            CHECK(listed && listed->Values.size() == 2 && listed->Values[1] == "No Damage" && Lua::FormWhenText(*listed) == "mode = Multiplier, No Damage");
            CHECK(!Lua::FormParseWhen("not mode = A") && !Lua::FormParseWhen("= A") && !Lua::FormParseWhen("mode =") && !Lua::FormParseWhen("2fast"));

            Lua::FormDocument form = Lua::NewForm("Rules");
            const int toggle = Lua::FormAdd(form, Lua::UiKind::Checkbox, form.Window);
            form.Meta[toggle].Name = "god";
            Lua::FormSetProp(*form.Model.Find(toggle), "style", "switch");
            const int mode = Lua::FormAdd(form, Lua::UiKind::Combo, form.Window);
            form.Meta[mode].Name = "mode";
            Lua::FormSetProp(*form.Model.Find(mode), "items", "Default\nNo Damage\nMultiplier");
            Lua::FormSetProp(*form.Model.Find(mode), "value", "1");
            const int slider = Lua::FormAdd(form, Lua::UiKind::Slider, form.Window);
            form.Meta[slider].Name = "factor";
            form.Meta[slider].EnabledWhen = "mode = Multiplier";
            const int other = Lua::FormAdd(form, Lua::UiKind::Slider, form.Window);
            form.Meta[other].EnabledWhen = "god";
            const int keep = Lua::FormAdd(form, Lua::UiKind::Combo, form.Window);
            form.Meta[keep].Name = "keep";
            Lua::FormSetProp(*form.Model.Find(keep), "items", "Ammo\nMoney");
            Lua::FormSetProp(*form.Model.Find(keep), "multi", "true");
            Lua::FormSetProp(*form.Model.Find(keep), "value", "2, 1");
            CHECK(form.Model.Find(keep)->Chosen == std::vector<int>({ 1, 2 }) && Lua::FormPropValue(*form.Model.Find(keep), "value") == "1, 2");
            CHECK_MSG(Lua::FormProblems(form).empty(), Lua::FormProblems(form).empty() ? std::string() : Lua::FormProblems(form).front().Text);

            const std::string saved = Lua::FormSave(form);
            CHECK(saved.find("style = \"switch\"") != std::string::npos);
            CHECK(saved.find("enabledWhen = \"mode = Multiplier\"") != std::string::npos && saved.find("enabledWhen = \"god\"") != std::string::npos);
            CHECK_MSG(saved.find("multi = true") != std::string::npos && saved.find("value = { 1, 2 }") != std::string::npos, saved);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoad(saved, loaded, error, &notes) && notes.empty(), error + (notes.empty() ? std::string() : " / " + notes.front()));
            CHECK(Lua::FormSave(loaded) == saved);

            Lua::FormApplyRules(loaded);
            CHECK(!loaded.Model.Find(Lua::FormFindNamed(loaded, "factor"))->Enabled);
            loaded.Model.Find(Lua::FormFindNamed(loaded, "mode"))->Selected = 3;
            loaded.Model.Find(Lua::FormFindNamed(loaded, "god"))->Bool = true;
            Lua::FormApplyRules(loaded);
            CHECK(loaded.Model.Find(Lua::FormFindNamed(loaded, "factor"))->Enabled);

            Lua::FormDocument renamed = form;
            Lua::FormRenameReferences(renamed, "mode", "damageMode");
            CHECK(renamed.Meta[slider].EnabledWhen == "damageMode = Multiplier" && renamed.Meta[other].EnabledWhen == "god");
            Lua::FormDocument broken = form;
            broken.Meta[slider].EnabledWhen = "mode = Triple";
            broken.Meta[other].EnabledWhen = "nobody";
            const std::vector<Lua::FormProblem> problems = Lua::FormProblems(broken);
            CHECK(std::any_of(problems.begin(), problems.end(), [](const Lua::FormProblem& p) { return p.Text.find("has no item \"Triple\"") != std::string::npos; }));
            CHECK(std::any_of(problems.begin(), problems.end(), [](const Lua::FormProblem& p) { return p.Text.find("no widget has that name") != std::string::npos; }));

            // a snippet pasted twice: the second copy's rule follows its own renamed switch
            {
                const std::span<const Lua::FormTemplate> snippets = Lua::FormTemplates();
                const auto snippet = std::find_if(snippets.begin(), snippets.end(), [](const Lua::FormTemplate& t) { return t.Id == "switchslider"; });
                CHECK(snippet != snippets.end());
                if (snippet != snippets.end())
                {
                    Lua::FormDocument twice = Lua::NewForm("Twice");
                    std::string pasteError;
                    CHECK(Lua::FormPaste(twice, snippet->Text, twice.Window, -1, pasteError).size() == 2 && pasteError.empty());
                    CHECK(Lua::FormPaste(twice, snippet->Text, twice.Window, -1, pasteError).size() == 2 && pasteError.empty());
                    const int second = Lua::FormFindNamed(twice, "featureMultiplier1");
                    CHECK(second > 0 && Lua::FormFindNamed(twice, "feature1") > 0 && twice.Meta[second].EnabledWhen == "feature1");
                    CHECK(twice.Meta[Lua::FormFindNamed(twice, "featureMultiplier")].EnabledWhen == "feature");
                    const Lua::UiNode* grid = second > 0 ? twice.Model.Find(second) : nullptr;
                    CHECK(grid && Lua::FormDuplicate(twice, grid->Parent) > 0);
                    CHECK_MSG(Lua::FormProblems(twice).empty(), Lua::FormProblems(twice).empty() ? std::string() : Lua::FormProblems(twice).front().Text);
                }
            }

            // the Trainer template: loads clean and builds a working window
            const std::span<const Lua::FormTemplate> templates = Lua::FormTemplates();
            const auto trainer = std::find_if(templates.begin(), templates.end(), [](const Lua::FormTemplate& t) { return t.Id == "trainer"; });
            CHECK(trainer != templates.end());
            if (trainer != templates.end())
            {
                Lua::FormDocument made;
                CHECK_MSG(Lua::FormLoad(trainer->Text, made, error, &notes) && notes.empty(), error);
                CHECK_MSG(Lua::FormProblems(made).empty(), Lua::FormProblems(made).empty() ? std::string() : Lua::FormProblems(made).front().Text);
                CHECK_MSG(engine.Lua().Run("local w = manifold.ui.form.build((function() " + std::string(trainer->Text) + " end)())\nw.remove()\n", "trainer template").Ok(), "the trainer template does not build");
            }

            // The runtime: switch style, multi combo, enabledWhen following the user's changes.
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "form rules", R"LUA(
                ruleChanges = {}
                local handlers = { keepChanged = function(value) ruleChanges[#ruleChanges + 1] = value end }
                ruleWindow, ruleWidgets = manifold.ui.form.build({ form = 1, title = "Rules Probe", children = {
                    { kind = "checkbox", name = "god", label = "God", style = "switch" },
                    { kind = "combo", name = "mode", items = { "Default", "No Damage", "Multiplier" }, value = 1 },
                    { kind = "slider", name = "factor", enabledWhen = "mode = Multiplier" },
                    { kind = "slider", name = "guarded", enabledWhen = "not god" },
                    { kind = "combo", name = "keep", items = { "Ammo", "Money" }, multi = true, value = { 2 }, onChange = "keepChanged" },
                    { kind = "number", name = "money", enabledWhen = "keep = Money" },
                  } }, handlers)
                local w = ruleWidgets
                local keep = w.keep.get()
                if type(keep) ~= "table" or #keep ~= 1 or keep[1] ~= 2 then return "multi get" end
                w.keep.set({ 1, 2, 2, 9 })
                keep = w.keep.get()
                if #keep ~= 2 or keep[1] ~= 1 or keep[2] ~= 2 then return "multi set keeps known items once: " .. #keep end
                ruleIds = { god = w.god.id, mode = w.mode.id, factor = w.factor.id, guarded = w.guarded.id, keep = w.keep.id, money = w.money.id }
                local function raises(fn, needle)
                  local ok, problem = pcall(fn)
                  if ok then return "no error for " .. needle end
                  local text = type(problem) == "table" and problem.message or tostring(problem)
                  if not text:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. text end
                end
                local failure = raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "slider", enabledWhen = "nobody" } } }) end, "no widget of the form has that name")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "text", name = "t" }, { kind = "slider", enabledWhen = "t" } } }) end, "it must be a checkbox, combo or radio")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "checkbox", name = "c" }, { kind = "slider", enabledWhen = "c = On" } } }) end, "which is on or off")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "slider", enabledWhen = 3 } } }) end, "must be a string")
                if failure then return failure end
                local styled = manifold.ui.window.create("Bad Style")
                failure = raises(function() styled.add("checkbox", { style = "toggle" }) end, 'must be "checkbox" or "switch"')
                styled.remove()
                if failure then return failure end
                return "ok")LUA");
            CHECK(!FindWindow(model, "Bad"));
            std::map<std::string, int> ids;
            {
                std::string result;
                engine.Lua().RunWithResult("return ('%d %d %d %d %d %d'):format(ruleIds.god, ruleIds.mode, ruleIds.factor, ruleIds.guarded, ruleIds.keep, ruleIds.money)", "rule ids", result);
                std::istringstream read(result);
                for (const char* name : { "god", "mode", "factor", "guarded", "keep", "money" })
                    read >> ids[name];
            }
            const auto enabled = [&](const char* name) { const Lua::UiNode* node = model.Find(ids[name]); return node && node->Enabled; };
            CHECK(model.Find(ids["god"]) && model.Find(ids["god"])->Style == "switch");
            CHECK(!enabled("factor") && enabled("guarded"));
            // the user picks Multiplier and turns the switch on: what UiHost posts
            if (Lua::UiNode* node = model.Find(ids["mode"]))
                node->Selected = 3;
            model.Post({ ids["mode"], "change", { 3.0 } });
            if (Lua::UiNode* node = model.Find(ids["god"]))
                node->Bool = true;
            model.Post({ ids["god"], "change", { true } });
            engine.Lua().Tick(9000.0);
            CHECK(enabled("factor") && !enabled("guarded"));
            // the multi combo sends its list as text; the handler gets a table
            if (Lua::UiNode* node = model.Find(ids["keep"]))
                node->Chosen = { 1 };
            model.Post({ ids["keep"], "change", { std::string("1") } });
            engine.Lua().Tick(9001.0);
            CHECK(!enabled("money"));
            LuaCheck(engine, "form rules changed", R"LUA(
                local last = ruleChanges[#ruleChanges]
                if type(last) ~= "table" or #last ~= 1 or last[1] ~= 1 then return "multi onChange got " .. tostring(last) end
                ruleWindow.remove()
                return "ok")LUA");
            CHECK(!FindWindow(model, "Rules Probe"));
        }

        void RecordBinding(Engine& engine)
        {
            // record = ... in a form: widgets follow and change memory records without code.
            const std::int32_t keptValue = g_probeValue;
            g_probeValue = 77;
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "record binding", R"LUA(
                if not manifold.process.isOpen() then
                  local info, problem = manifold.process.open(probePid)
                  if not info then return "open " .. tostring(problem) end
                end
                local R = manifold.record
                bindSwitch = R.create({ kind = "script", description = "Bind Switch", script = "[ENABLE]\n[DISABLE]\n" })
                bindNumber = R.create({ description = "Bind Number", address = ("%X"):format(probeValue) })
                bindClicks = 0
                bindWindow, bindWidgets = manifold.ui.form.build({ form = 1, title = "Bind Probe", children = {
                    { kind = "checkbox", name = "god", label = "God", style = "switch", record = "Bind Switch" },
                    { kind = "button", name = "flip", text = "Flip", record = bindSwitch.id, onClick = "flipped" },
                    { kind = "number", name = "amount", record = "Bind Number", integer = true },
                    { kind = "text", name = "shown", record = "Bind Number" },
                    { kind = "slider", name = "lost", record = "No Such Record" },
                    { kind = "slider", name = "guarded", record = "Bind Number", enabledWhen = "god" },
                  } }, { flipped = function() bindClicks = bindClicks + 1 end })
                local w = bindWidgets
                if w.god.get() ~= false then return "checkbox starts as the record" end
                if w.amount.get() ~= 77 then return "number shows the value: " .. tostring(w.amount.get()) end
                if w.shown.get() ~= "77" then return "text shows the value: " .. tostring(w.shown.get()) end
                bindIds = { god = w.god.id, flip = w.flip.id, amount = w.amount.id, shown = w.shown.id, lost = w.lost.id, guarded = w.guarded.id }

                local function raises(fn, needle)
                  local ok, failure = pcall(fn)
                  if ok then return "no error for " .. needle end
                  local message = type(failure) == "table" and failure.message or tostring(failure)
                  if not message:find(needle, 1, true) then return "message lacks '" .. needle .. "': " .. message end
                end
                return raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "section", record = "x" } } }) end, "a section cannot show a memory record")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "number", record = {} } } }) end, "record must be a memory record's description or id")
                  or raises(function() manifold.ui.form.build({ form = 1, title = "Bad", children = { { kind = "combo", multi = true, items = { "a" }, record = "x" } } }) end, "multi = true cannot show a memory record")
                  or "ok")LUA");
            CHECK(!FindWindow(model, "Bad"));
            std::map<std::string, int> ids;
            {
                std::string result;
                engine.Lua().RunWithResult("return ('%d %d %d %d %d %d'):format(bindIds.god, bindIds.flip, bindIds.amount, bindIds.shown, bindIds.lost, bindIds.guarded)", "bind ids", result);
                std::istringstream read(result);
                for (const char* name : { "god", "flip", "amount", "shown", "lost", "guarded" })
                    read >> ids[name];
            }
            const auto node = [&](const char* name) { return model.Find(ids[name]); };
            CHECK(node("lost") && !node("lost")->Enabled);
            CHECK(node("guarded") && !node("guarded")->Enabled); // its rule: god is off

            // the user turns the switch on and types a number: the records follow
            if (Lua::UiNode* god = model.Find(ids["god"]))
                god->Bool = true;
            model.Post({ ids["god"], "change", { true } });
            if (Lua::UiNode* amount = model.Find(ids["amount"]))
                amount->Number = 555;
            model.Post({ ids["amount"], "change", { 555.0 } });
            engine.Lua().Tick(10000.0);
            CHECK_EQ(g_probeValue, 555);
            CHECK(node("guarded") && node("guarded")->Enabled);
            LuaCheck(engine, "record binding writes", R"LUA(
                if not bindSwitch.isActive() then return "the switch did not activate the record" end
                -- the record changes elsewhere: a hotkey switches it off
                bindSwitch.setActive(false)
                return "ok")LUA");
            g_probeValue = 42; // the game changes the value
            engine.Lua().Tick(10000.5);
            CHECK(node("god") && !node("god")->Bool);
            CHECK(node("amount") && node("amount")->Number == 42.0);
            CHECK(node("shown") && node("shown")->Text == "42");

            // the button switches the record, then the script's own handler runs
            model.Post({ ids["flip"], "click", {} });
            engine.Lua().Tick(10001.0);
            engine.Lua().Tick(10001.5);
            CHECK(node("god") && node("god")->Bool);
            LuaCheck(engine, "record binding button", R"LUA(
                if not bindSwitch.isActive() then return "the button did not switch the record" end
                if bindClicks ~= 1 then return "the script's onClick ran " .. bindClicks .. " times" end
                bindSwitch.setActive(false)
                bindWindow.remove()
                bindSwitch.remove()
                bindNumber.remove()
                return "ok")LUA");
            CHECK(!FindWindow(model, "Bind Probe"));
            g_probeValue = keptValue;

            // the document: record saved by description or id, read back
            Lua::FormDocument form = Lua::NewForm("Bound");
            const int box = Lua::FormAdd(form, Lua::UiKind::Checkbox, form.Window);
            form.Meta[box].Record = "God Mode";
            const int field = Lua::FormAdd(form, Lua::UiKind::Number, form.Window);
            form.Meta[field].Record = "12";
            form.Meta[field].RecordById = true;
            const std::string saved = Lua::FormSave(form);
            CHECK(saved.find("record = \"God Mode\"") != std::string::npos && saved.find("record = 12") != std::string::npos);
            Lua::FormDocument loaded;
            std::string error;
            std::vector<std::string> notes;
            CHECK_MSG(Lua::FormLoad(saved, loaded, error, &notes) && notes.empty(), error);
            CHECK(Lua::FormSave(loaded) == saved);
            CHECK(Lua::FormCanBind(Lua::UiKind::Slider) && !Lua::FormCanBind(Lua::UiKind::Section));
        }

        void SnakeExample(Engine& engine)
        {
            Lua::UiModel& model = engine.Lua().Ui();
            LuaCheck(engine, "canvas keys", R"LUA(
                uiKeys = {}
                local w = manifold.ui.window.create("Keys", { key = "keys probe" })
                uiKeyCanvas = w.add("canvas", { height = 40, onKey = function(key, widget) uiKeys[#uiKeys + 1] = key .. ":" .. widget.kind end })
                return "ok")LUA");
            const Lua::UiNode* keyed = nullptr;
            for (const int id : model.Windows())
            {
                if (const Lua::UiNode* window = model.Find(id); window && window->Title == "Keys" && !window->Children.empty())
                    keyed = model.Find(window->Children.front());
            }
            CHECK(keyed && (keyed->Handlers & Lua::UiHandler::Key));
            if (keyed)
            {
                model.Post({ keyed->Id, "key", { std::string("LeftArrow") } });
                model.Post({ keyed->Id, "key", { std::string("Space") } });
            }
            engine.Lua().Tick(2000.0);
            LuaCheck(engine, "canvas keys seen", R"LUA(
                if #uiKeys ~= 2 or uiKeys[1] ~= "LeftArrow:canvas" or uiKeys[2] ~= "Space:canvas" then return table.concat(uiKeys, ",") end
                uiKeyCanvas.remove()
                return "ok")LUA");

            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Examples" / "Manifold-Snake.lua"); ++i)
                at = at.parent_path();
            const std::filesystem::path script = at / "Examples" / "Manifold-Snake.lua";
            if (!std::filesystem::exists(script))
            {
                Skip("Examples/Manifold-Snake.lua not found above " + std::filesystem::current_path().string());
                return;
            }
            std::ifstream file(script, std::ios::binary);
            std::stringstream content;
            content << file.rdbuf();
            const Status loaded = engine.Lua().Run(content.str(), "Manifold-Snake.lua");
            CHECK_MSG(loaded.Ok(), loaded.Text);
            const Lua::UiNode* board = nullptr;
            const Lua::UiNode* window = nullptr;
            for (const int id : model.Windows())
            {
                const Lua::UiNode* candidate = model.Find(id);
                if (!candidate || candidate->Title != "Snake")
                    continue;
                window = candidate;
                const std::function<void(const Lua::UiNode&)> search = [&](const Lua::UiNode& node)
                    {
                        for (const int child : node.Children)
                        {
                            const Lua::UiNode* found = model.Find(child);
                            if (!found)
                                continue;
                            if (found->Kind == Lua::UiKind::Canvas)
                                board = found;
                            search(*found);
                        }
                    };
                search(*candidate);
            }
            CHECK(window && window->Updates);
            CHECK(board && (board->Handlers & Lua::UiHandler::Key) && (board->Handlers & Lua::UiHandler::Click));
            if (!board || !window)
                return;
            const int boardId = board->Id;
            const int windowId = window->Id;
            double now = 3000.0;
            for (int frame = 0; frame < 120; ++frame)
            {
                if (frame == 10)
                    model.Post({ boardId, "key", { std::string("UpArrow") } });
                if (frame == 40)
                    model.Post({ boardId, "key", { std::string("P") } });
                if (frame == 60)
                    model.Post({ boardId, "key", { std::string("Space") } });
                if (frame == 80)
                    model.Post({ boardId, "click", { 10.0, 10.0, 1.0 } });
                if (frame == 100)
                    model.Post({ boardId, "key", { std::string("R") } });
                now += 1.0 / 60.0;
                engine.Lua().Tick(now);
            }
            const Lua::UiNode* drawn = model.Find(boardId);
            CHECK(drawn && drawn->Draws.size() >= 6);
            const Lua::UiNode* still = model.Find(windowId);
            CHECK_MSG(still && still->Updates, "the game's onUpdate failed and was switched off");
            CHECK(engine.Lua().Run("", "Manifold-Snake.lua").Ok());
            CHECK(!model.Find(windowId));
        }

        void Documentation(Engine& engine)
        {
            const std::string markdown = Lua::ManifoldApiMarkdown(engine.Lua());
            CHECK(markdown.find("## manifold.memory") != std::string::npos);
            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "Lua" / "Manifold"); ++i)
                at = at.parent_path();
            const std::filesystem::path docs = at / "Docs" / "MANIFOLD-API.md";
            if (std::filesystem::exists(at / "Source" / "Lua" / "Manifold"))
            {
                std::ifstream file(docs, std::ios::binary);
                std::stringstream content;
                content << file.rdbuf();
                std::string written = content.str();
                written.erase(std::remove(written.begin(), written.end(), '\r'), written.end());
                CHECK_MSG(written == markdown, "Docs/MANIFOLD-API.md is out of date: run the tests with --manifold-markdown");
            }
            else
                Skip("Docs/MANIFOLD-API.md not found above " + std::filesystem::current_path().string());
        }
    }

    std::string ManifoldMarkdown()
    {
        Settings settings;
        Engine engine(settings);
        return Lua::ManifoldApiMarkdown(engine.Lua());
    }

    int RunManifoldApiTests()
    {
        const int before = Totals().Failures;
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        Schema(engine);
        Output(engine);
        ShowOnPrint(engine);
        Errors(engine);
        WithoutProcess(engine);
        OwnProcess(engine);
        RecordApi(engine);
        RecordBinding(engine);
        Version2(engine);
        Assist(engine);
        DocApi(engine);
        Documentation(engine);
        UiApi(engine);
        UiOpacity(engine);
        UiNewWidgets(engine);
        UiForms(engine);
        UiFormsStage2(engine);
        UiFormImport(engine);
        UiFormReload(engine);
        UiWindowOptions(engine);
        UiFormLinks(engine);
        UiFormCanvas(engine);
        UiFormModals(engine);
        UiFormOneFile(engine);
        UiFormRules(engine);
        SnakeExample(engine);
        engine.CloseProcess();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("manifold-api", "Manifold Lua API", 90, [](const SuiteContext&) { return RunManifoldApiTests(); });
    MANIFOLD_MODE("--manifold-markdown", [](const std::vector<std::string>&)
        {
            std::fputs(ManifoldMarkdown().c_str(), stdout);
            return 0;
        });
}
