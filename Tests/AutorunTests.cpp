#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/DocRegistry.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        struct EmbedMember
        {
            virtual ~EmbedMember() = default;
            virtual int Value() const { return 7; }
            std::uint64_t Payload = 0x1111;
        };

        struct EmbedOwner
        {
            virtual ~EmbedOwner() = default;
            virtual int Kind() const { return 1; }
            std::uint64_t Before = 0x2222;
            EmbedMember Member;
            std::uint64_t After = 0x3333;
        };

        EmbedOwner g_embedOwner;

        std::filesystem::path AutorunSource()
        {
            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "Lua" / "Manifold"); ++i)
                at = at.parent_path();
            return at / "Autorun";
        }

        std::uint64_t Mark(Lua::LuaEngine& lua)
        {
            const auto last = lua.Console().Last();
            return last ? last->Id : 0;
        }

        std::vector<std::string> ProblemLines(Lua::LuaEngine& lua, std::uint64_t from)
        {
            std::vector<std::string> problems;
            for (const Lua::ConsoleEntry& entry : lua.Console().Entries())
            {
                if (entry.Id <= from)
                    continue;
                const std::string text = entry.PlainText();
                if (entry.Kind == Lua::EntryKind::Error || entry.Level == LogLevel::Error || text.find("ME-7007") != std::string::npos)
                    problems.push_back(text);
            }
            return problems;
        }

        void CheckQuiet(Lua::LuaEngine& lua, std::uint64_t from, const char* step)
        {
            const std::vector<std::string> problems = ProblemLines(lua, from);
            CHECK_MSG(problems.empty(), std::string(step) + ": " + (problems.empty() ? "" : problems.front()));
        }
    }

    int RunAutorunTests()
    {
        const int before = Totals().Failures;
        const std::filesystem::path folder = AutorunSource();
        std::error_code error;
        if (!std::filesystem::is_directory(folder, error))
        {
            Skip("no Autorun folder next to the sources above " + std::filesystem::current_path().string());
            return 0;
        }
        Settings settings;
        Engine engine(settings);
        Lua::LuaEngine& lua = engine.Lua();
        lua.Run("__manifoldHeadless = true", "headless");
        lua.Run("getAutorunPath = function() return [==[" + WideToUtf8(folder.wstring()) + "\\]==] end", "autorun path");

        std::uint64_t mark = Mark(lua);
        CHECK_MSG(lua.RunAutorun(folder).Ok(), "first run");
        CheckQuiet(lua, mark, "first run");
        for (const char* name : { "Manifold-Godot", "Manifold-SigMaker" })
        {
            const Autorun::Module* module = Autorun::Find(lua.Modules(), name);
            CHECK_MSG(module && module->State == Autorun::State::Loaded && module->Origin == Autorun::Origin::Shipped, std::string(name) + (module ? ": " + module->Reason : " is not in the Autorun folder"));
        }
        CHECK_MSG(lua.SetModuleEnabled("Manifold-SigMaker", false).Ok() && lua.SetModuleEnabled("Manifold-Godot", false).Ok(), "switching the shipped modules off");
        for (const char* name : { "Manifold-Godot", "Manifold-SigMaker" })
        {
            const Autorun::Module* module = Autorun::Find(lua.Modules(), name);
            CHECK_MSG(module && module->State == Autorun::State::Disabled && !module->ActiveUntilRestart, std::string(name) + " leaves something behind when it is switched off");
        }
        CHECK_MSG(lua.SetModuleEnabled("Manifold-SigMaker", true).Ok() && lua.SetModuleEnabled("Manifold-Godot", true).Ok(), "switching the shipped modules on again");
        CheckQuiet(lua, mark, "off and on");
        CHECK_MSG(!std::filesystem::exists(folder / "data"), "a shipped module wrote into the repository's Autorun folder");
        for (const auto& [module, least] : { std::pair<const char*, std::size_t>{ "ManifoldRTTI", 8 }, { "ManifoldGodot", 20 }, { "ManifoldSigMaker", 4 } })
        {
            const std::vector<Lua::DocPtr> members = lua.Docs().Members(module);
            const std::size_t annotated = static_cast<std::size_t>(std::count_if(members.begin(), members.end(), [](const Lua::DocPtr& member) { return member->Origin != Lua::DocOrigin::Reflection; }));
            CHECK_MSG(members.size() >= least, std::string(module) + " documents " + std::to_string(members.size()) + " functions");
            CHECK_MSG(lua.Docs().Find(module) && !lua.Docs().Find(module)->Text.empty(), std::string(module) + " has no module text");
            std::printf("  %s: %zu functions documented, %zu with annotations\n", module, members.size(), annotated);
        }

        mark = Mark(lua);
        CHECK_MSG(lua.RunAutorun(folder).Ok(), "second run");
        CheckQuiet(lua, mark, "second run");

        mark = Mark(lua);
        CHECK(engine.OpenProcess(MakeProcessId(::GetCurrentProcessId())).Ok());
        CheckQuiet(lua, mark, "process opened");

        mark = Mark(lua);
        std::string result;
        const Status probed = lua.RunWithResult(R"LUA(
            if manifold.unreal.claims(manifold.process.mainModule().base) then return "the Unreal tool claims the test program" end
            for _, name in ipairs({ "Godot" }) do
              local engine = rawget(_G, "Manifold" .. name)
              if engine ~= nil and type(engine.claims) == "function" then
                local ok, claimed = pcall(engine.claims, manifold.process.mainModule().base)
                if not ok then return name .. ".claims raised: " .. tostring(claimed) end
                if claimed then return name .. " claims the test program" end
              end
            end
            return "ok")LUA", "autorun probe", result);
        CHECK_MSG(probed.Ok() && result == "ok", probed.Ok() ? result : probed.Text);
        CheckQuiet(lua, mark, "claims");

        mark = Mark(lua);
        const std::uint64_t owner = reinterpret_cast<std::uint64_t>(&g_embedOwner);
        const Status embedded = lua.RunWithResult("local address, member, size = 0x" + Hex(owner) + ", " + std::to_string(reinterpret_cast<std::uint64_t>(&g_embedOwner.Member) - owner) + ", " + std::to_string(sizeof(EmbedOwner)) + R"LUA(
            local s = createStructure("EmbedProbe")
            s.autoGuess(address, 0, size)
            ManifoldRTTI.label(s, address)
            local found
            for i = 0, s.Count - 1 do
              local e = s.getElement(i)
              if e.Offset == member then found = e end
              if e.Offset > member and e.Offset < size then return "a field inside the member stayed: " .. string.format("%X", e.Offset) end
            end
            if not found then return "no field at the member" end
            if found.Vartype == vtPointer then return "the member is still a pointer" end
            local child = found.ChildStruct
            if not child then return "no child structure" end
            if not child.Name:find("EmbedMember", 1, true) then return "child is " .. child.Name end
            if found.Bytesize ~= size - member then return "member size " .. found.Bytesize end
            local first = child.getElement(0)
            if not first or first.Name ~= "vftable" then return "child field 0 is " .. tostring(first and first.Name) end
            return "ok")LUA", "embedded member", result);
        CHECK_MSG(embedded.Ok() && result == "ok", embedded.Ok() ? result : embedded.Text);
        CheckQuiet(lua, mark, "embedded member");

        mark = Mark(lua);
        engine.CloseProcess();
        CheckQuiet(lua, mark, "process closed");
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("autorun", "Autorun modules", 100, [](const SuiteContext&) { return RunAutorunTests(); });
}
