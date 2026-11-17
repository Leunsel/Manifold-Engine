#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

        void LuaCheck(Engine& engine, const char* name, const std::string& code)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(code, name, result);
            CHECK_MSG(status.Ok() && result == "ok", std::string(name) + ": " + (status.Ok() ? result : status.Text));
        }

        const Lua::OutputLine* LastLine(Engine& engine, std::vector<Lua::OutputLine>& keep)
        {
            keep = engine.Lua().Output();
            return keep.empty() ? nullptr : &keep.back();
        }

        void Schema(Engine& engine)
        {
            LuaCheck(engine, "schema names", R"LUA(
                if manifold.apiVersion ~= 1 or type(manifold.version) ~= "string" then return "constants" end
                local docs = debug.getregistry()["Manifold.Docs"]
                if type(docs) ~= "table" then return "no docs table" end
                local count = 0
                for areaName, members in pairs(manifold) do
                  if type(members) == "table" then
                    if not areaName:match("^%l+$") then return "area name " .. areaName end
                    local functions = {}
                    for name, value in pairs(members) do
                      if type(value) ~= "function" then return "non-function " .. areaName .. "." .. name end
                      if not name:match("^%l%w*$") then return "function name " .. areaName .. "." .. name end
                      local doc = docs[value]
                      if not doc or doc.text == nil or doc.text == "" or doc.returns == nil then return "undocumented " .. areaName .. "." .. name end
                      if (name:match("^is%u") or name:match("^has%u")) and doc.returns ~= "boolean" then return "predicate " .. name .. " returns " .. doc.returns end
                      functions[name] = true
                      count = count + 1
                    end
                    for name in pairs(functions) do
                      local setting = name:match("^set(%u%w*)$")
                      if setting and not functions["get" .. setting] then return "set without get: " .. name end
                      local getter = name:match("^get(%u%w*)$")
                      if getter and not functions["set" .. getter] then return "get without set: " .. name end
                    end
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
            std::vector<Lua::OutputLine> keep;
            lua.ClearOutput();
            LuaCheck(engine, "log info", R"LUA(
                manifold.log.info("hello", { b = 2, a = "x", list = { 1, 2 } })
                return "ok")LUA");
            const Lua::OutputLine* line = LastLine(engine, keep);
            CHECK(line && line->Kind == Lua::OutputKind::Info);
            CHECK(line && line->HeadEnd == line->TagEnd && line->TagEnd > line->TimeEnd && line->TimeEnd == 9);
            CHECK_MSG(line && line->Text.find("INFO") == 9 && line->Text.substr(line->HeadEnd) == "hello  {a = \"x\", b = 2, list = {1, 2}}", line ? line->Text : "no line");

            const std::size_t before = lua.OutputCount();
            LuaCheck(engine, "log debug hidden", R"LUA(
                manifold.log.debug("hidden")
                if manifold.log.getLevel() ~= "info" then return "default level" end
                return "ok")LUA");
            CHECK(lua.OutputCount() == before);
            LuaCheck(engine, "log debug shown", R"LUA(
                manifold.log.setLevel("debug")
                manifold.log.debug("shown")
                manifold.log.setLevel("info")
                return "ok")LUA");
            line = LastLine(engine, keep);
            CHECK(line && line->Kind == Lua::OutputKind::Debug && line->Text.ends_with("shown"));

            LuaCheck(engine, "log channel", R"LUA(
                manifold.log.channel("Teleporter").success("ready")
                return "ok")LUA");
            line = LastLine(engine, keep);
            CHECK_MSG(line && line->Kind == Lua::OutputKind::Success && line->Text.substr(line->TagEnd) == "[Teleporter] ready", line ? line->Text : "no line");

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
            keep = lua.Output();
            bool sawMessage = false;
            bool sawTrace = false;
            for (const Lua::OutputLine& entry : keep)
            {
                if (entry.Kind != Lua::OutputKind::Error)
                    continue;
                sawMessage = sawMessage || entry.Text.find("log error object:2: boom (ME-7002)") != std::string::npos;
                sawTrace = sawTrace || (entry.HeadEnd == 0 && entry.Text.find("stack traceback:") != std::string::npos && entry.Text.starts_with("    "));
            }
            CHECK(sawMessage);
            CHECK(sawTrace);

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
                if #manifold.process.find() < 10 or #manifold.process.find("CSRSS") < 1 then return "find" end
                if #manifold.process.find("no-such-process-name") ~= 0 then return "find filter" end
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

        void Assist(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            const std::vector<std::string> signatures = lua.CallSignatures("manifold.memory.read", false);
            CHECK_MSG(signatures.size() == 1 && signatures.front() == "read(address, type, options?)", signatures.empty() ? "none" : signatures.front());
            bool found = false;
            for (const Lua::LuaCompletion& item : lua.Complete("manifold.log", "inf", false))
                found = found || (item.Name == "info" && item.Signature == "(message, fields?)" && !item.Text.empty());
            CHECK(found);
            const std::vector<Lua::LuaCompletion> areas = lua.Complete("manifold", "", false);
            CHECK(std::any_of(areas.begin(), areas.end(), [](const Lua::LuaCompletion& item) { return item.Name == "process"; }));
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
        Assist(engine);
        Documentation(engine);
        engine.CloseProcess();
        return Totals().Failures - before;
    }
}
