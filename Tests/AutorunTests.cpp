#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

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

        std::vector<std::string> ProblemLines(Lua::LuaEngine& lua, std::size_t from)
        {
            std::vector<std::string> problems;
            const std::vector<Lua::OutputLine> output = lua.Output();
            for (std::size_t i = from; i < output.size(); ++i)
            {
                const Lua::OutputLine& line = output[i];
                if (line.Kind == Lua::OutputKind::Error || line.Text.find("ME-7007") != std::string::npos)
                    problems.push_back(line.Text);
            }
            return problems;
        }

        void CheckQuiet(Lua::LuaEngine& lua, std::size_t from, const char* step)
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
            std::printf("  no Autorun folder next to the sources, skipped\n");
            return 0;
        }
        Settings settings;
        Engine engine(settings);
        Lua::LuaEngine& lua = engine.Lua();
        lua.Run("__manifoldHeadless = true", "headless");
        lua.Run("getAutorunPath = function() return [==[" + WideToUtf8(folder.wstring()) + "\\]==] end", "autorun path");

        std::size_t mark = lua.OutputCount();
        CHECK_MSG(lua.RunAutorun(folder).Ok(), "first run");
        CheckQuiet(lua, mark, "first run");

        mark = lua.OutputCount();
        CHECK_MSG(lua.RunAutorun(folder).Ok(), "second run");
        CheckQuiet(lua, mark, "second run");

        mark = lua.OutputCount();
        CHECK(engine.OpenProcess(::GetCurrentProcessId()).Ok());
        CheckQuiet(lua, mark, "process opened");

        mark = lua.OutputCount();
        std::string result;
        const Status probed = lua.RunWithResult(R"LUA(
            for _, name in ipairs({ "UE", "Godot" }) do
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

        mark = lua.OutputCount();
        engine.CloseProcess();
        CheckQuiet(lua, mark, "process closed");
        return Totals().Failures - before;
    }
}
