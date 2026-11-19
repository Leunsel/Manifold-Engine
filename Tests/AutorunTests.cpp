#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/DocRegistry.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
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
        engine.CloseProcess();
        CheckQuiet(lua, mark, "process closed");
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("autorun", "Autorun modules", 100, [](const SuiteContext&) { return RunAutorunTests(); });
}
