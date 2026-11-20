#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/DocRegistry.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaEngine.h"
#include "Lua/UiModel.h"

#include "lua.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
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
                if manifold.apiVersion ~= 9 or type(manifold.version) ~= "string" then return "constants" end
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
        Version2(engine);
        Assist(engine);
        DocApi(engine);
        Documentation(engine);
        UiApi(engine);
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
