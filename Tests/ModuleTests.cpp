#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Text.h"
#include "Lua/AutorunModules.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        using Autorun::Module;
        using Autorun::State;

        struct Tree
        {
            std::filesystem::path Root;

            explicit Tree(const char* name)
            {
                Root = std::filesystem::temp_directory_path() / ("manifold-modules-" + std::string(name) + "-" + std::to_string(::GetCurrentProcessId()));
                std::error_code ignored;
                std::filesystem::remove_all(Root, ignored);
                std::filesystem::create_directories(Root / "autorun", ignored);
            }

            ~Tree()
            {
                std::error_code ignored;
                std::filesystem::remove_all(Root, ignored);
            }

            std::filesystem::path Autorun() const { return Root / "autorun"; }
            std::filesystem::path StateFile() const { return Root / "modules.ini"; }

            void Put(const char* relative, std::string_view text, bool append = false) const
            {
                const std::filesystem::path path = Autorun() / relative;
                std::error_code ignored;
                std::filesystem::create_directories(path.parent_path(), ignored);
                std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
                file.write(text.data(), static_cast<std::streamsize>(text.size()));
            }

            std::string LuaPath(const char* relative) const
            {
                return "[==[" + WideToUtf8((Autorun() / relative).wstring()) + "]==]";
            }
        };

        std::string Names(const std::vector<Module>& modules)
        {
            std::string text;
            for (const Module& module : modules)
                text += (text.empty() ? "" : " ") + module.Name;
            return text;
        }

        std::string OrderNames(std::vector<Module>& modules, int api = 10)
        {
            std::string text;
            for (const std::size_t index : Autorun::Order(modules, api))
            {
                if (modules[index].State == State::Pending)
                    text += (text.empty() ? "" : " ") + modules[index].Name;
            }
            return text;
        }

        const Module& Get(const std::vector<Module>& modules, const char* name)
        {
            static const Module missing;
            const Module* found = Autorun::Find(modules, name);
            CHECK_MSG(found != nullptr, std::string("no module ") + name);
            return found ? *found : missing;
        }

        std::string Eval(Lua::LuaEngine& lua, const std::string& code)
        {
            std::string result;
            const Status status = lua.RunWithResult(code, "probe", result);
            return status.Ok() ? result : "ERROR " + status.Text;
        }

        void HeaderChecks()
        {
            const Autorun::Header owner = Autorun::ParseHeader(
                "--[[\r\n    Manifold-Thing.lua\r\n    ----------------\r\n\r\n    NAME        : Thing\r\n    AUTHOR  : Leunsel\r\n    VERSION : 2.1.0\r\n    DESCRIPTION : Does a thing: well.\r\n"
                "    REQUIRES : Json >= 1.2, Other Lib\r\n    API : 9\r\n    LICENSE : MIT\r\n\r\n    MIT License:\r\n        Copyright (c) 2026\r\n    CHANGELOG:\r\n        v1\r\n    VERSION : 9.9\r\n]]\r\nlocal x = 1\r\n");
            CHECK_EQ(owner.Name, "Thing");
            CHECK_EQ(owner.Version, "2.1.0");
            CHECK_EQ(owner.Author, "Leunsel");
            CHECK_EQ(owner.Description, "Does a thing: well.");
            CHECK(owner.Requires.size() == 2 && owner.Requires[0].Name == "Json" && owner.Requires[0].MinVersion == "1.2" && owner.Requires[1].Name == "Other Lib" && owner.Requires[1].MinVersion.empty());
            CHECK(owner.Api == 9 && owner.Problem.empty());
            CHECK(owner.Other.size() == 1 && owner.Other[0].first == "LICENSE" && owner.Other[0].second == "MIT");

            const Autorun::Header none = Autorun::ParseHeader("print('hello')\n-- NAME : NotAHeader\n");
            CHECK(none.Name.empty() && none.Version.empty() && none.Requires.empty() && none.Problem.empty());

            const Autorun::Header lines = Autorun::ParseHeader("\xEF\xBB\xBF-- NAME : Lines\n-- VERSION : 1.0\nlocal a = 1\n-- AUTHOR : Late\n");
            CHECK(lines.Name == "Lines" && lines.Version == "1.0" && lines.Author.empty());

            const Autorun::Header level = Autorun::ParseHeader("--[==[\nNAME : Level\n]==]\n");
            CHECK_EQ(level.Name, "Level");

            for (const char* bad : { "--[[\nREQUIRES : Json > 1\n]]", "--[[\nREQUIRES : Json >=\n]]", "--[[\nREQUIRES : Json,, Other\n]]", "--[[\nREQUIRES : >= 2\n]]", "--[[\nAPI : new\n]]" })
            {
                const Autorun::Header header = Autorun::ParseHeader(bad);
                CHECK_MSG(!header.Problem.empty() && header.Requires.empty(), bad);
            }
            CHECK(Autorun::ParseHeader("--[[\nNAME : a\\b\n]]").Name.empty());

            CHECK(Autorun::CompareVersions("1.2", "1.2.0") == 0 && Autorun::CompareVersions("1.10", "1.9") > 0 && Autorun::CompareVersions("2.0.0", "2.0.1") < 0 && Autorun::CompareVersions("3", "2.9.9") > 0);
            CHECK(Autorun::CompareVersions("1.99999999999999999999999", "1.99999999999999999999998") > 0 && Autorun::CompareVersions("100000000000000000000000", "99999999999999999999999") > 0);
            CHECK(Autorun::CompareVersions("1.000000000000000000000002", "1.2") == 0 && Autorun::CompareVersions("0.0", "") == 0 && Autorun::CompareVersions("9999999999999999999999.1", "9999999999999999999999.10") < 0);

            for (const char* bad : { "..", ".", "...", ".hidden", "trailing.", "a\x01" "b", "tab\tinside" })
                CHECK_MSG(Autorun::ParseHeader("--[[\nNAME : " + std::string(bad) + "\n]]").Name.empty(), bad);
            CHECK(!Autorun::ValidName("") && !Autorun::ValidName(" lead") && !Autorun::ValidName("trail ") && Autorun::ValidName("a.b") && Autorun::ValidName("Manifold-SigMaker"));
            CHECK_EQ(Autorun::Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        }

        void ScanChecks()
        {
            Tree tree("scan");
            tree.Put("b.lua", "B = true");
            tree.Put("A.lua", "A = true");
            tree.Put("notes.txt", "no");
            tree.Put("custom/c.lua", "C = true");
            tree.Put("modules/Zeta.lua", "-- NAME : Alpha\n");
            tree.Put("modules/Cat/Beta/init.lua", "--[[\nVERSION : 1.0\n]]");
            tree.Put("modules/Cat/Beta/lib.lua", "return 1");
            tree.Put("modules/Cat/Beta/sub/init.lua", "return 2");
            tree.Put("modules/Cat/Loose.lua", "");
            tree.Put("modules/Cat/Deep/Deeper/init.lua", "");
            tree.Put("modules/Cat/Deep/Skipped.lua", "");
            tree.Put("lib/json.lua", "return {}");
            tree.Put("data/Beta/config.lua", "return {}");
            tree.Put("Resource/thing.lua", "RESOURCE = true");
            tree.Put("Resource/init.lua", "RESOURCE = true");
            const std::vector<Module> modules = Autorun::Scan(tree.Autorun());
            CHECK_EQ(Names(modules), "A b c Alpha Beta Loose");
            const Module& beta = Get(modules, "beta");
            CHECK(beta.Shape == Autorun::Shape::Folder && beta.Category == "Cat" && beta.Header.Version == "1.0" && beta.Where == "modules\\Cat\\Beta");
            CHECK(Get(modules, "Alpha").Shape == Autorun::Shape::File && Get(modules, "Alpha").Category.empty());
            CHECK(Get(modules, "c").Shape == Autorun::Shape::Loose && Get(modules, "c").Category == "custom");

            const std::string before = beta.Hash;
            tree.Put("modules/Cat/Beta/icon.png", "png");
            Module again = beta;
            Autorun::Refresh(again);
            CHECK_EQ(again.Hash, before);
            tree.Put("modules/Cat/Beta/sub/init.lua", "return 3");
            Autorun::Refresh(again);
            CHECK(again.Hash != before && again.Hash.size() == 64);

            tree.Put("Alpha.lua", "LOOSE_ALPHA = true");
            tree.Put("modules/Cat/Second.lua", "-- NAME : beta\n");
            tree.Put("Manifold-RTTI.lua", "local K = \"__MANIFOLD_RTTI_RUNTIME__\"");
            const std::vector<Module> clash = Autorun::Scan(tree.Autorun());
            int superseded = 0;
            int duplicates = 0;
            int builtIn = 0;
            for (const Module& module : clash)
            {
                superseded += module.State == State::Superseded ? 1 : 0;
                duplicates += module.Duplicate ? 1 : 0;
                builtIn += module.State == State::BuiltIn ? 1 : 0;
            }
            CHECK(superseded == 1 && duplicates == 1 && builtIn == 1);
            CHECK(Get(clash, "Alpha").Shape == Autorun::Shape::File && Get(clash, "Beta").Shape == Autorun::Shape::Folder);
        }

        void OrderChecks()
        {
            {
                Tree tree("order");
                tree.Put("zz.lua", "");
                tree.Put("aa.lua", "");
                tree.Put("custom/mm.lua", "");
                tree.Put("modules/Beta.lua", "");
                tree.Put("modules/Cat/alpha.lua", "");
                std::vector<Module> modules = Autorun::Scan(tree.Autorun());
                CHECK_EQ(OrderNames(modules), "aa zz mm alpha Beta");
            }
            {
                Tree tree("requires");
                tree.Put("aa.lua", "-- REQUIRES : Zed\n");
                tree.Put("modules/App.lua", "-- REQUIRES : Json >= 1.2, Zed\n");
                tree.Put("modules/Json.lua", "-- VERSION : 1.2.1\n");
                tree.Put("modules/Zed.lua", "");
                tree.Put("modules/Old.lua", "-- REQUIRES : Json >= 2\n");
                tree.Put("modules/NoVersion.lua", "-- REQUIRES : Zed >= 1\n");
                tree.Put("modules/Missing.lua", "-- REQUIRES : Nowhere\n");
                tree.Put("modules/Future.lua", "-- API : 99\n");
                tree.Put("modules/Broken.lua", "-- REQUIRES : Json < 3\n");
                tree.Put("modules/LoopA.lua", "-- REQUIRES : LoopB\n");
                tree.Put("modules/LoopB.lua", "-- REQUIRES : LoopA\n");
                tree.Put("modules/OnLoop.lua", "-- REQUIRES : LoopA\n");
                tree.Put("modules/Off.lua", "");
                tree.Put("modules/NeedsOff.lua", "-- REQUIRES : Off\n");
                std::vector<Module> modules = Autorun::Scan(tree.Autorun());
                Autorun::Find(modules, "Off")->State = State::Disabled;
                CHECK_EQ(OrderNames(modules), "Zed aa Json App");
                for (const auto& [name, part] : { std::pair{ "Old", "found 1.2.1" }, { "NoVersion", "names no version" }, { "Missing", "not in the autorun folder" }, { "Future", "manifold API 99" },
                         { "Broken", "only >=" }, { "LoopA", "circle" }, { "LoopB", "LoopA -> LoopB -> LoopA" }, { "OnLoop", "is blocked" }, { "NeedsOff", "is disabled" } })
                {
                    const Module& module = Get(modules, name);
                    CHECK_MSG(module.State == State::Blocked && module.Reason.find(part) != std::string::npos, std::string(name) + ": " + module.Reason);
                }
            }
        }

        void StoreChecks()
        {
            Tree tree("store");
            tree.Put("modules/One.lua", "ONE = 1");
            tree.Put("modules/Shipped.lua", "SHIPPED = 1");
            std::vector<Module> modules = Autorun::Scan(tree.Autorun());
            const std::string shippedHash = Get(modules, "Shipped").Hash;
            const Autorun::ShippedModule shipped[] = { { "shipped", shippedHash } };
            const Autorun::ApplyOptions hold{ true, shipped };

            Autorun::StateStore first;
            CHECK(!first.Load(tree.StateFile()) && !first.Existed());
            Autorun::ApplyState(modules, first, hold);
            CHECK(Get(modules, "One").State == State::Pending && !Get(modules, "One").New);
            CHECK(Get(modules, "Shipped").Origin == Autorun::Origin::Shipped && Get(modules, "Shipped").Unmodified && Get(modules, "One").Origin == Autorun::Origin::User);
            CHECK(first.Save() && std::filesystem::exists(tree.StateFile()));

            tree.Put("modules/Two.lua", "TWO = 1");
            tree.Put("modules/One.lua", "ONE = 2");
            modules = Autorun::Scan(tree.Autorun());
            Autorun::StateStore second;
            CHECK(second.Load(tree.StateFile()) && second.Existed());
            Autorun::ApplyState(modules, second, hold);
            CHECK(Get(modules, "Two").State == State::Held && Get(modules, "Two").New);
            CHECK(Get(modules, "One").State == State::Pending && Get(modules, "One").Changed);
            CHECK(second.Find("two") == nullptr);
            second.Find("one")->Enabled = false;
            second.Touch();
            CHECK(second.Save());

            std::error_code ignored;
            std::filesystem::remove(tree.Autorun() / "modules" / "Shipped.lua", ignored);
            tree.Put("modules/Shipped2.lua", "SHIPPED = 1");
            tree.Put("modules/Shipped3.lua", "-- NAME : Shipped\nSHIPPED = 2");
            modules = Autorun::Scan(tree.Autorun());
            Autorun::StateStore third;
            CHECK(third.Load(tree.StateFile()));
            Autorun::ApplyState(modules, third, hold);
            CHECK(Get(modules, "One").State == State::Disabled && !Get(modules, "One").Enabled && !Get(modules, "One").Changed);
            CHECK(Get(modules, "Shipped2").State == State::Held && Get(modules, "Shipped2").Origin == Autorun::Origin::User);
            CHECK(Get(modules, "Shipped").Origin == Autorun::Origin::Shipped && !Get(modules, "Shipped").Unmodified && Get(modules, "Shipped").Changed);

            Autorun::StateStore relaxed;
            relaxed.Load(tree.StateFile());
            modules = Autorun::Scan(tree.Autorun());
            Autorun::ApplyState(modules, relaxed, { false, shipped });
            CHECK(Get(modules, "Two").State == State::Pending && Get(modules, "Shipped2").State == State::Pending);
        }

        void BuildTree(const Tree& tree)
        {
            tree.Put("zz.lua", "ORDER = (ORDER or '') .. 'zz '");
            tree.Put("Json.lua", "OLD_JSON_RAN = true");
            tree.Put("lib/shared.lua", "return { name = 'shared' }");
            tree.Put("modules/Json.lua", "--[[\nNAME : Json\nVERSION : 1.2\n]]\nORDER = (ORDER or '') .. 'Json '\nreturn { encode = function() return 'encoded' end }");
            tree.Put("modules/Tools/App/init.lua",
                "--[[\nNAME : App\nVERSION : 0.3\nDESCRIPTION : The app.\nREQUIRES : Json >= 1.0\n]]\n"
                "ORDER = (ORDER or '') .. 'App '\n"
                "local util = require('util')\nlocal nested = require('parts.piece')\nlocal json = require('Json')\nlocal shared = require('shared')\n"
                "APP = { util = util.value, again = require('util') == util, nested = nested, json = json.encode(), shared = shared.name, runs = (APP and APP.runs or 0) + 1 }\n"
                "APP_EVENTS = APP_EVENTS or 0\n"
                "manifold.event.subscribe('processClosed', function() APP_EVENTS = APP_EVENTS + 1 end)\n"
                "manifold.module.onUnload(function() APP_UNLOADS = (APP_UNLOADS or 0) + 1 end)\n"
                "local me = manifold.module.current()\nAPP_ME = me.name .. '|' .. me.version .. '|' .. me.kind .. '|' .. me.category .. '|' .. me.state .. '|' .. me.requires[1].name\n"
                "APP_PATH = me.path\nAPP_DATA = me.dataPath\n"
                "return 'app value'");
            tree.Put("modules/Tools/App/util.lua", "manifold.event.subscribe('processClosed', function() APP_EVENTS = APP_EVENTS + 100 end)\nreturn { value = 'app util' }");
            tree.Put("modules/Tools/App/parts/piece.lua", "return 'piece'");
            tree.Put("modules/Other/init.lua", "ORDER = (ORDER or '') .. 'Other '\nOTHER = require('util').value .. '|' .. tostring(pcall(require, 'parts.piece'))");
            tree.Put("modules/Other/util.lua", "return { value = 'other util' }");
            tree.Put("modules/Bad.lua", "ORDER = (ORDER or '') .. 'Bad '\nerror('bad on purpose')");
            tree.Put("modules/NeedsBad.lua", "-- REQUIRES : Bad\nNEEDS_BAD_RAN = true");
            tree.Put("modules/Later.lua", "ORDER = (ORDER or '') .. 'Later '\nLATER = (LATER or 0) + 1");
            tree.Put("modules/Sneaky.lua", "ORDER = (ORDER or '') .. 'Sneaky '\nSNEAKY = require('App')");
            tree.Put("modules/Timer.lua", "ORDER = (ORDER or '') .. 'Timer '\nlocal t = createTimer(nil, false)");
        }

        void LoaderChecks()
        {
            Tree tree("loader");
            BuildTree(tree);
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            const Status first = lua.RunAutorun(tree.Autorun());
            CHECK_MSG(!first.Ok() && first.Text.find("Bad") != std::string::npos, first.Text);
            CHECK_EQ(Eval(lua, "return ORDER"), "zz Json App Bad Later Other Sneaky Timer ");
            CHECK_EQ(Eval(lua, "return tostring(OLD_JSON_RAN) .. tostring(NEEDS_BAD_RAN)"), "nilnil");
            CHECK_EQ(Eval(lua, "return APP.util .. '|' .. tostring(APP.again) .. '|' .. APP.nested .. '|' .. APP.json .. '|' .. APP.shared"), "app util|true|piece|encoded|shared");
            CHECK_EQ(Eval(lua, "return OTHER"), "other util|false");
            CHECK_EQ(Eval(lua, "return APP_ME"), "App|0.3|folder|Tools|pending|Json");
            CHECK_EQ(Eval(lua, "return SNEAKY"), "app value");
            CHECK_EQ(Eval(lua, "return tostring(package.loaded.util) .. tostring(package.loaded['parts.piece'])"), "nilnil");
            CHECK(Eval(lua, "return APP_PATH") == WideToUtf8((tree.Autorun() / "modules" / "Tools" / "App").wstring()) + "\\");
            CHECK(Eval(lua, "return APP_DATA") == WideToUtf8((tree.Autorun() / "data" / "App").wstring()) + "\\");
            CHECK(!std::filesystem::exists(tree.Autorun() / "data"));

            const std::vector<Module>& modules = lua.Modules();
            CHECK(Get(modules, "App").State == State::Loaded && Get(modules, "Json").State == State::Loaded && Get(modules, "Later").State == State::Loaded && Get(modules, "zz").State == State::Loaded);
            CHECK(Get(modules, "Bad").State == State::Failed && Get(modules, "Bad").Reason.find("bad on purpose") != std::string::npos && !Get(modules, "Bad").Traceback.empty());
            CHECK(Get(modules, "NeedsBad").State == State::Blocked && Get(modules, "NeedsBad").Reason.find("Bad, which failed") != std::string::npos);
            bool superseded = false;
            for (const Module& module : modules)
                superseded = superseded || (module.Shape == Autorun::Shape::Loose && module.Key == "json" && module.State == State::Superseded);
            CHECK(superseded);
            bool warned = false;
            for (const Lua::ConsoleEntry& entry : lua.Console().Entries())
                warned = warned || entry.PlainText().find("does not name it in REQUIRES") != std::string::npos;
            CHECK(warned);

            CHECK_EQ(Eval(lua, "local l = manifold.module.list() local i = manifold.module.info('app') local c, e = manifold.module.current() local m, e2 = manifold.module.info('nope') "
                               "return #l .. '|' .. i.state .. '|' .. tostring(c) .. '|' .. e.code .. '|' .. tostring(m) .. '|' .. e2.code"),
                std::to_string(modules.size()) + "|loaded|nil|ME-7011|nil|ME-7011");
            CHECK_EQ(Eval(lua, "local ok, e = manifold.module.reload('Bad') return tostring(ok) .. '|' .. e.code"), "nil|ME-7003");
            CHECK_EQ(Eval(lua, "local ok, e = pcall(require, 'NeedsBad') return tostring(ok) .. '|' .. tostring(e):match('not loaded')"), "false|not loaded");

            lua.DispatchEvent("processClosed");
            CHECK_EQ(Eval(lua, "return tostring(APP_EVENTS)"), "101");
            tree.Put("modules/Tools/App/util.lua", "return { value = 'edited util' }");
            CHECK(lua.ReloadModule("App").Ok());
            CHECK_EQ(Eval(lua, "return APP.util .. '|' .. APP.runs .. '|' .. tostring(APP_UNLOADS) .. '|' .. SNEAKY"), "edited util|2|1|app value");
            lua.DispatchEvent("processClosed");
            CHECK_EQ(Eval(lua, "return tostring(APP_EVENTS)"), "102");

            CHECK(lua.SetModuleEnabled("Json", false).Ok());
            CHECK(Get(modules, "Json").State == State::Disabled && !Get(modules, "Json").ActiveUntilRestart);
            CHECK(Get(modules, "App").State == State::Blocked && Get(modules, "App").Reason.find("is disabled") != std::string::npos);
            lua.DispatchEvent("processClosed");
            CHECK_EQ(Eval(lua, "return APP_EVENTS .. '|' .. APP_UNLOADS"), "102|2");
            CHECK(lua.SetModuleEnabled("Json", true).Ok());
            CHECK(Get(modules, "Json").State == State::Loaded && Get(modules, "App").State == State::Loaded);
            CHECK_EQ(Eval(lua, "return tostring(APP.runs)"), "3");

            CHECK(lua.SetModuleEnabled("Timer", false).Ok());
            CHECK(Get(modules, "Timer").State == State::Disabled && Get(modules, "Timer").ActiveUntilRestart);
            CHECK(!lua.SetModuleEnabled("Nope", false).Ok() && !lua.ReloadModule("Timer").Ok());

            tree.Put("modules/Bad.lua", "BAD_FIXED = true");
            CHECK(lua.ReloadModule("Bad").Ok());
            CHECK(Get(modules, "Bad").State == State::Loaded && Get(modules, "NeedsBad").State == State::Loaded);
            CHECK_EQ(Eval(lua, "return tostring(NEEDS_BAD_RAN)"), "true");

            const Status again = lua.RunAutorun(tree.Autorun());
            CHECK_MSG(again.Ok(), again.Text);
            CHECK_EQ(Eval(lua, "return LATER .. '|' .. APP.runs .. '|' .. APP_UNLOADS"), "2|4|3");
            lua.DispatchEvent("processClosed");
            CHECK_EQ(Eval(lua, "return tostring(APP_EVENTS)"), "103");
            CHECK(Get(lua.Modules(), "Timer").State == State::Loaded);
            CHECK_EQ(Eval(lua, "return manifold.module.ensureData == nil and 'x' or tostring(select(2, manifold.module.ensureData()).code)"), "ME-7011");
        }

        void TrustChecks()
        {
            Tree tree("trust");
            tree.Put("modules/Json.lua", "-- VERSION : 1.0\nJSON = (JSON or 0) + 1");
            tree.Put("modules/App.lua", "-- REQUIRES : Json\nAPP_RUNS = (APP_RUNS or 0) + 1\nAPP_DATA = manifold.module.ensureData()");
            tree.Put("loose.lua", "LOOSE = (LOOSE or 0) + 1");
            const Lua::AutorunOptions options{ tree.StateFile(), true };
            {
                Settings settings;
                Engine engine(settings);
                Lua::LuaEngine& lua = engine.Lua();
                lua.Run("__manifoldHeadless = true", "headless");
                CHECK(lua.RunAutorun(tree.Autorun(), options).Ok());
                CHECK_EQ(Eval(lua, "return JSON .. '|' .. APP_RUNS .. '|' .. LOOSE"), "1|1|1");
                CHECK(std::filesystem::exists(tree.StateFile()) && std::filesystem::is_directory(tree.Autorun() / "data" / "App"));

                tree.Put("modules/Planted.lua", "PLANTED = true");
                tree.Put("planted-loose.lua", "PLANTED_LOOSE = true");
                CHECK(lua.RunAutorun(tree.Autorun(), options).Ok());
                CHECK(Get(lua.Modules(), "Planted").State == State::Held && Get(lua.Modules(), "Planted").New && Get(lua.Modules(), "planted-loose").State == State::Held);
                CHECK_EQ(Eval(lua, "return tostring(PLANTED) .. tostring(PLANTED_LOOSE) .. JSON"), "nilnil2");
                CHECK(lua.ModulesNeedingAttention() == 2);
                CHECK_EQ(Eval(lua, "local ok, e = manifold.module.reload('Planted') return tostring(ok) .. '|' .. e.code"), "nil|ME-7013");
                CHECK(lua.AllowModule("Planted").Ok());
                CHECK_EQ(Eval(lua, "return tostring(PLANTED) .. tostring(PLANTED_LOOSE)"), "truenil");
                CHECK(lua.SetModuleEnabled("planted-loose", false).Ok());
            }
            {
                Settings settings;
                Engine engine(settings);
                Lua::LuaEngine& lua = engine.Lua();
                lua.Run("__manifoldHeadless = true", "headless");
                CHECK(lua.RunAutorun(tree.Autorun(), options).Ok());
                CHECK(Get(lua.Modules(), "Planted").State == State::Loaded && !Get(lua.Modules(), "Planted").Changed);
                CHECK(Get(lua.Modules(), "planted-loose").State == State::Disabled);
                CHECK(lua.SetModuleEnabled("planted-loose", true).Text.find("ME-7013") != std::string::npos || Get(lua.Modules(), "planted-loose").State == State::Held);
                CHECK_EQ(Eval(lua, "return tostring(PLANTED_LOOSE)"), "nil");

                lua.Run("local f = io.open(" + tree.LuaPath("data/App/settings.lua") + ", 'w') f:write('return {}') f:close()", "table");
                lua.Run("local f = io.open(" + tree.LuaPath("modules/Json.lua") + ", 'r') f:close()", "table");
                CHECK(lua.ModulesNeedingAttention() == 1 && !Get(lua.Modules(), "Json").HeldNext);
                lua.Run("local f = io.open(" + tree.LuaPath("modules/Json.lua") + ", 'a') f:write('\\nJSON_TAMPERED = true\\n') f:close()", "table");
                CHECK(Get(lua.Modules(), "Json").HeldNext && Get(lua.Modules(), "Json").State == State::Loaded);
                lua.Run("os.remove(" + tree.LuaPath("loose.lua") + ".. '.missing')", "table");

                const Status held = lua.RunAutorun(tree.Autorun(), options);
                CHECK(!held.Ok());
                CHECK(Get(lua.Modules(), "Json").State == State::Held && !Get(lua.Modules(), "Json").New && Get(lua.Modules(), "App").State == State::Blocked);
                CHECK_EQ(Eval(lua, "return tostring(JSON_TAMPERED) .. '|' .. APP_RUNS"), "nil|1");
                CHECK(lua.AllowModule("Json").Ok());
                CHECK(Get(lua.Modules(), "Json").State == State::Loaded && Get(lua.Modules(), "App").State == State::Loaded);
                CHECK_EQ(Eval(lua, "return tostring(JSON_TAMPERED) .. '|' .. APP_RUNS"), "true|2");

                lua.Run("local f = io.open(" + tree.LuaPath("modules/App.lua") + ", 'a') f:write('\\nAPP_PLANTED = true\\n') f:close()", "table");
                CHECK(Get(lua.Modules(), "App").HeldNext && Get(lua.Modules(), "App").State == State::Loaded);
                CHECK_EQ(Eval(lua, "return tostring(manifold.module.reload('Json'))"), "true");
                CHECK(Get(lua.Modules(), "Json").State == State::Loaded && Get(lua.Modules(), "App").State == State::Held && !Get(lua.Modules(), "App").HeldNext);
                CHECK_EQ(Eval(lua, "return tostring(APP_PLANTED) .. '|' .. APP_RUNS .. '|' .. JSON"), "nil|2|3");
                CHECK(lua.ModulesNeedingAttention() == 2);
                CHECK(lua.AllowModule("App").Ok());
                CHECK(Get(lua.Modules(), "App").State == State::Loaded);
                CHECK_EQ(Eval(lua, "return tostring(APP_PLANTED) .. '|' .. APP_RUNS"), "true|3");

                lua.Run("local f = io.open(" + tree.LuaPath("modules/App.lua") + ", 'a') f:write('\\nAPP_PLANTED_AGAIN = true\\n') f:close()", "table");
                CHECK(lua.SetModuleEnabled("Json", false).Ok());
                CHECK(Get(lua.Modules(), "App").State == State::Blocked);
                CHECK(lua.SetModuleEnabled("Json", true).Ok());
                CHECK(Get(lua.Modules(), "Json").State == State::Loaded && Get(lua.Modules(), "App").State == State::Held);
                CHECK_EQ(Eval(lua, "return tostring(APP_PLANTED_AGAIN) .. '|' .. APP_RUNS .. '|' .. JSON"), "nil|3|4");
                CHECK(lua.AllowModule("App").Ok());
                CHECK_EQ(Eval(lua, "return tostring(APP_PLANTED_AGAIN) .. '|' .. APP_RUNS"), "true|4");

                tree.Put("loose.lua", "\nLOOSE_EDITED = true", true);
                CHECK(lua.RunAutorun(tree.Autorun(), options).Ok());
                CHECK(Get(lua.Modules(), "loose").State == State::Loaded && Get(lua.Modules(), "loose").Changed && !Get(lua.Modules(), "Json").Changed);
                CHECK_EQ(Eval(lua, "return tostring(LOOSE_EDITED)"), "true");
                CHECK(lua.RunAutorun(tree.Autorun(), options).Ok());
                CHECK(!Get(lua.Modules(), "loose").Changed);
            }
        }

        void GuardChecks()
        {
            Tree tree("guard");
            tree.Put("modules/Dots.lua", "-- NAME : ..\nDOTS_DATA = manifold.module.ensureData()");
            tree.Put("modules/Loop.lua",
                "LOOP_RUNS = (LOOP_RUNS or 0) + 1\n"
                "manifold.module.onUnload(function()\n"
                "  local ok, e = manifold.module.reload('Loop')\n  local ok2, e2 = manifold.module.disable('Dots')\n"
                "  LOOP_REFUSED = tostring(ok) .. '|' .. e.code .. '|' .. tostring(ok2) .. '|' .. e2.code\n"
                "end)");
            Settings settings;
            Engine engine(settings);
            Lua::LuaEngine& lua = engine.Lua();
            lua.Run("__manifoldHeadless = true", "headless");
            CHECK(lua.RunAutorun(tree.Autorun()).Ok());
            CHECK(Get(lua.Modules(), "Dots").State == State::Loaded && Get(lua.Modules(), "Loop").State == State::Loaded);
            CHECK(Eval(lua, "return DOTS_DATA") == WideToUtf8((tree.Autorun() / "data" / "Dots").wstring()) + "\\");
            Module dots;
            dots.Name = "..";
            CHECK(lua.ModuleDataFolder(dots) == tree.Autorun() / "data" / "__");

            CHECK(lua.ReloadModule("Loop").Ok());
            CHECK_EQ(Eval(lua, "return LOOP_REFUSED .. '|' .. LOOP_RUNS"), "nil|ME-7012|nil|ME-7012|2");
            CHECK(Get(lua.Modules(), "Dots").State == State::Loaded && Get(lua.Modules(), "Dots").Enabled);
            CHECK(lua.RunAutorun(tree.Autorun()).Ok());
            CHECK_EQ(Eval(lua, "return tostring(LOOP_RUNS)"), "3");
            CHECK(Get(lua.Modules(), "Loop").State == State::Loaded && Get(lua.Modules(), "Dots").State == State::Loaded);

            std::error_code ignored;
            std::filesystem::remove_all(tree.Autorun(), ignored);
            CHECK(lua.RunAutorun(tree.Autorun()).Ok());
            CHECK(lua.Modules().empty() && std::filesystem::is_directory(tree.Autorun()));
            CHECK_EQ(Eval(lua, "return tostring(LOOP_RUNS) .. '|' .. tostring(#manifold.module.list())"), "3|0");
        }

        void ShippedChecks()
        {
            std::filesystem::path at = std::filesystem::current_path();
            for (int i = 0; i < 8 && !std::filesystem::exists(at / "Source" / "Lua" / "Manifold"); ++i)
                at = at.parent_path();
            std::error_code error;
            if (!std::filesystem::is_directory(at / "Autorun", error))
            {
                Skip("no Autorun folder next to the sources above " + std::filesystem::current_path().string());
                return;
            }
            std::vector<Module> modules = Autorun::Scan(at / "Autorun");
            CHECK(!modules.empty());
            for (Module& module : modules)
            {
                Autorun::ApplyShipped(module, Autorun::ShippedModules());
                CHECK_MSG(module.Origin == Autorun::Origin::Shipped && module.Unmodified, module.Name + " in Autorun does not match Source/Generated/ShippedModules.cpp: build again, or run Tools/embed_modules.py");
                CHECK_MSG(!module.Header.Version.empty() && !module.Header.Description.empty() && module.Header.Problem.empty(), module.Name + " has no VERSION or DESCRIPTION line");
            }
            CHECK(Get(modules, "Manifold-Godot").Category == "Engines" && Get(modules, "Manifold-SigMaker").Category == "Tools" && Get(modules, "Manifold-SigMaker").Shape == Autorun::Shape::Folder);
        }
    }

    int RunModuleTests()
    {
        const int before = Totals().Failures;
        HeaderChecks();
        ScanChecks();
        OrderChecks();
        StoreChecks();
        LoaderChecks();
        TrustChecks();
        GuardChecks();
        ShippedChecks();
        return Totals().Failures - before;
    }

    MANIFOLD_SUITE("modules", "Autorun module loader", 99, [](const SuiteContext&) { return RunModuleTests(); });
}
