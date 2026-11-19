#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Lua/ConsoleHistory.h"
#include "Lua/LuaConsole.h"
#include "Lua/LuaEngine.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    namespace
    {
        Lua::ConsoleEntry Entry(Lua::EntryKind kind, LogLevel level, std::string text, std::string channel = {})
        {
            Lua::ConsoleEntry entry;
            entry.Kind = kind;
            entry.Level = level;
            entry.Text = std::move(text);
            entry.Channel = std::move(channel);
            return entry;
        }

        void Log()
        {
            Lua::ConsoleLog log;
            const std::uint64_t generation = log.Generation();
            const std::uint64_t first = log.Add(Entry(Lua::EntryKind::Log, LogLevel::Info, "ready", "Godot"));
            CHECK(first > 0 && log.Count() == 1 && log.Generation() > generation);
            const std::uint64_t repeat = log.Add(Entry(Lua::EntryKind::Log, LogLevel::Info, "ready", "Godot"));
            CHECK_EQ(repeat, first);
            CHECK(log.Count() == 1 && log.Last() && log.Last()->Repeat == 2);
            log.Add(Entry(Lua::EntryKind::Log, LogLevel::Warning, "careful", "Godot/Resolve"));
            log.Add(Entry(Lua::EntryKind::Error, LogLevel::Error, "broke"));
            const std::uint64_t input = log.Add(Entry(Lua::EntryKind::Input, LogLevel::Info, "1 + 1"));
            log.Add(Entry(Lua::EntryKind::Input, LogLevel::Info, "1 + 1"));
            CHECK_EQ(log.Count(), static_cast<std::size_t>(5));
            const auto entries = log.Entries();
            CHECK(entries.size() == 5 && entries[3].InputId == input && entries[4].Id != input);
            const Lua::ConsoleCounts counts = log.Counts();
            CHECK_EQ(counts.Levels[static_cast<std::size_t>(LogLevel::Info)], static_cast<std::size_t>(4));
            CHECK_EQ(counts.Levels[static_cast<std::size_t>(LogLevel::Warning)], static_cast<std::size_t>(1));
            CHECK(counts.Errors == 1 && counts.Inputs == 2);
            const auto channels = log.Channels();
            CHECK(channels.size() == 2 && channels[0] == "Godot" && channels[1] == "Godot/Resolve");
            std::size_t visited = 0;
            log.Visit(1, 3, [&](std::size_t, const Lua::ConsoleEntry&) { ++visited; });
            CHECK_EQ(visited, static_cast<std::size_t>(2));
            const std::uint64_t epoch = log.Epoch();
            log.Clear();
            CHECK(log.Count() == 0 && log.Epoch() > epoch && log.Counts().Errors == 0 && log.Channels().empty());

            Lua::ConsoleLog shared;
            std::vector<std::thread> writers;
            for (int t = 0; t < 4; ++t)
                writers.emplace_back([&shared, t]()
                    {
                        for (int i = 0; i < 500; ++i)
                            shared.Add(Entry(Lua::EntryKind::Print, LogLevel::Info, std::to_string(t) + ":" + std::to_string(i)));
                    });
            for (std::thread& writer : writers)
                writer.join();
            CHECK_EQ(shared.Count(), static_cast<std::size_t>(2000));

            Lua::ConsoleEntry error = Entry(Lua::EntryKind::Error, LogLevel::Error, "bad");
            error.Channel = "Probe";
            error.Code = "ME-7002";
            error.FieldsText = "{a = 1}";
            error.Traceback = "stack traceback:\n\t[C]: in ?";
            CHECK_EQ(error.PlainText(), std::string("ERR [Probe] bad (ME-7002)  {a = 1}\nstack traceback:\n\t[C]: in ?"));
        }

        void Parsing()
        {
            std::string_view rest;
            Lua::ConsoleSource source = Lua::ParseSource("console:12: attempt to index a nil value", &rest);
            CHECK(source.Chunk == "console" && source.Line == 12 && rest == "attempt to index a nil value");
            source = Lua::ParseSource("F:\\Games\\My Tables\\x.lua:7: boom", &rest);
            CHECK(source.Chunk == "F:\\Games\\My Tables\\x.lua" && source.Line == 7 && rest == "boom");
            source = Lua::ParseSource("[string \"local x = 1...\"]:3: oops", &rest);
            CHECK(source.Chunk == "[string \"local x = 1...\"]" && source.Line == 3 && rest == "oops");
            source = Lua::ParseSource("no position here: really", &rest);
            CHECK(source.Chunk.empty() && source.Line == 0 && rest == "no position here: really");
            std::string text;
            std::string traceback;
            Lua::SplitTraceback("console:1: bad\nstack traceback:\n\t[C]: in ?\n", text, traceback);
            CHECK(text == "console:1: bad" && traceback == "stack traceback:\n\t[C]: in ?");
            Lua::SplitTraceback("plain", text, traceback);
            CHECK(text == "plain" && traceback.empty());
            CHECK_EQ(Lua::ChunkLabel("@F:\\Tables\\Script.lua", "..."), std::string("Script.lua"));
            CHECK_EQ(Lua::ChunkLabel("=console", "console"), std::string("console"));
            CHECK_EQ(Lua::ChunkLabel("local x", "[string \"local x\"]"), std::string("[string \"local x\"]"));
            CHECK_EQ(Lua::QuoteLuaString("a\nb\"c", 100), std::string("\"a\\nb\\\"c\""));
            CHECK(Lua::QuoteLuaString(std::string(400, 'x'), 10).find("(400 bytes)") != std::string::npos);
            CHECK_EQ(Lua::ConsoleTime(0, false).size(), static_cast<std::size_t>(8));
        }

        std::optional<Lua::ConsoleEntry> Find(Lua::LuaEngine& lua, std::uint64_t input, Lua::EntryKind kind)
        {
            for (const Lua::ConsoleEntry& entry : lua.Console().Entries())
            {
                if (entry.InputId == input && entry.Kind == kind)
                    return entry;
            }
            return std::nullopt;
        }

        void Repl(Engine& engine)
        {
            Lua::LuaEngine& lua = engine.Lua();
            lua.ClearOutput();
            Status outcome = Status::Fail(ErrorCode::LuaRuntime);
            const std::uint64_t sum = lua.Evaluate("1 + 1", [&](const Status& status) { outcome = status; });
            CHECK(outcome.Ok());
            auto input = Find(lua, sum, Lua::EntryKind::Input);
            auto result = Find(lua, sum, Lua::EntryKind::Result);
            CHECK(input && input->Text == "1 + 1" && input->Id == sum && input->Source.Chunk == "[1]");
            CHECK(result && result->Values.size() == 1 && result->Values[0]->Nodes[0].Value == "2" && result->Elapsed >= 0.0);

            const std::uint64_t many = lua.Evaluate("1, 'two', nil");
            result = Find(lua, many, Lua::EntryKind::Result);
            CHECK(result && result->Values.size() == 3 && result->Values[1]->Nodes[0].Value == "\"two\"" && result->Values[2]->Nodes[0].Type == "nil");

            const std::uint64_t statement = lua.Evaluate("replValue = 41 + 1");
            CHECK(!Find(lua, statement, Lua::EntryKind::Result) && !Find(lua, statement, Lua::EntryKind::Error));
            const std::uint64_t read = lua.Evaluate("replValue");
            result = Find(lua, read, Lua::EntryKind::Result);
            CHECK(result && result->Values[0]->Nodes[0].IsInteger && result->Values[0]->Nodes[0].Integer == 42);

            const std::uint64_t syntax = lua.Evaluate("if then");
            auto error = Find(lua, syntax, Lua::EntryKind::Error);
            const auto syntaxInput = Find(lua, syntax, Lua::EntryKind::Input);
            CHECK_MSG(error && syntaxInput && error->Code == "ME-7001" && error->Source.Chunk == syntaxInput->Source.Chunk, error ? error->Source.Chunk + " " + error->PlainText() : "no error");

            const std::uint64_t runtime = lua.Evaluate("local t = nil\nreturn t.field");
            error = Find(lua, runtime, Lua::EntryKind::Error);
            CHECK_MSG(error && error->Text.find("attempt to index") != std::string::npos && error->Source.Line == 2 && error->Traceback.find("stack traceback:") != std::string::npos,
                error ? error->PlainText() : "no error");

            const std::uint64_t raised = lua.Evaluate("manifold.error.raise('custom failure', { code = 'MY-1' })");
            error = Find(lua, raised, Lua::EntryKind::Error);
            CHECK_MSG(error && error->Code == "MY-1" && error->Text == "custom failure", error ? error->PlainText() : "no error");

            CHECK(lua.Run("snap = { 10, 'two', nested = { deep = { deeper = { deepest = { x = 1 } } } }, f = print } cyc = {} cyc.self = cyc big = {} for i = 1, 1000 do big[i] = i end", "snapshots").Ok());
            result = Find(lua, lua.Evaluate("snap"), Lua::EntryKind::Result);
            CHECK(result && !result->Values.empty());
            if (result && !result->Values.empty())
            {
                const Lua::ValueSnapshot& snap = *result->Values[0];
                CHECK(snap.Nodes[0].Total == 4 && snap.Nodes[0].Subtree + 1 == snap.Nodes.size());
                CHECK(snap.Nodes[1].Key == "[1]" && snap.Nodes[1].Value == "10" && snap.Nodes[2].Key == "[2]");
                const auto deepest = std::find_if(snap.Nodes.begin(), snap.Nodes.end(), [](const Lua::SnapshotNode& node) { return node.Key == "deepest"; });
                CHECK(deepest != snap.Nodes.end() && deepest->Depth == 4 && deepest->Total == 1 && deepest->Shown == 0);
                const auto function = std::find_if(snap.Nodes.begin(), snap.Nodes.end(), [](const Lua::SnapshotNode& node) { return node.Key == "f"; });
                CHECK(function != snap.Nodes.end() && function->Type == "function" && function->Value == "function (C)");
            }
            result = Find(lua, lua.Evaluate("cyc"), Lua::EntryKind::Result);
            CHECK(result && result->Values[0]->Nodes.size() == 2 && result->Values[0]->Nodes[1].Cycle);
            result = Find(lua, lua.Evaluate("big"), Lua::EntryKind::Result);
            CHECK(result && result->Values[0]->Nodes[0].Total == 1000 && result->Values[0]->Nodes[0].Shown == 256 && result->Values[0]->Nodes.size() == 257);
            result = Find(lua, lua.Evaluate("manifold.memory"), Lua::EntryKind::Result);
            CHECK_MSG(result && result->Values[0]->Nodes[0].Label == "manifold.memory" && result->Values[0]->Nodes[0].Total >= 4,
                result ? result->Values[0]->Nodes[0].Label + " / " + result->Values[0]->Nodes[0].Value + " / " + std::to_string(result->Values[0]->Nodes[0].Total) : "no result");

            CHECK(lua.Run("bomb = setmetatable({}, { __tostring = function() while true do end end })", "slow label").Ok());
            result = Find(lua, lua.Evaluate("bomb"), Lua::EntryKind::Result);
            CHECK(result && result->Values[0]->Nodes[0].Label.empty());

            const std::uint64_t tail = lua.Evaluate("manifold.log.info('tail call')");
            const auto tailInput = Find(lua, tail, Lua::EntryKind::Input);
            const auto tailLog = lua.Console().Last();
            CHECK_MSG(tailInput && tailLog && tailLog->Text == "tail call" && tailLog->Source.Chunk == tailInput->Source.Chunk && tailLog->Source.Line == 1,
                tailLog ? tailLog->Source.Chunk + ":" + std::to_string(tailLog->Source.Line) : "nothing");
            const auto comment = Find(lua, lua.Evaluate("40 + 2 -- the answer"), Lua::EntryKind::Result);
            CHECK(comment && comment->Values.size() == 1 && comment->Values[0]->Nodes[0].Value == "42");
            const std::uint64_t logged = lua.Evaluate("print('from input') manifold.log.warning('careful')");
            const auto loggedInput = Find(lua, logged, Lua::EntryKind::Input);
            std::vector<Lua::ConsoleEntry> after;
            for (const Lua::ConsoleEntry& entry : lua.Console().Entries())
            {
                if (entry.Id > logged)
                    after.push_back(entry);
            }
            CHECK_MSG(after.size() == 2 && loggedInput && after[0].Source.Chunk == loggedInput->Source.Chunk && after[1].Source.Chunk == loggedInput->Source.Chunk && after[1].Source.Line == 1,
                after.size() == 2 ? after[0].Source.Chunk + " / " + after[1].Source.Chunk : std::to_string(after.size()));

            engine.SetAsync(true);
            bool finished = false;
            int task = 0;
            const std::uint64_t background = lua.Evaluate("print('async print') manifold.log.warning('async warning') return 7", [&](const Status&) { finished = true; }, &task);
            const double started = engine.Now();
            while (!finished && engine.Now() - started < 5.0)
                engine.Tick();
            engine.SetAsync(false);
            CHECK(finished && task > 0);
            const auto backgroundInput = Find(lua, background, Lua::EntryKind::Input);
            std::string sources;
            bool sourced = backgroundInput.has_value();
            for (const Lua::ConsoleEntry& entry : lua.Console().Entries())
            {
                if (entry.Id <= background || entry.Kind == Lua::EntryKind::Result)
                    continue;
                sources += entry.Text + "@" + entry.Source.Chunk + ":" + std::to_string(entry.Source.Line) + " ";
                sourced = sourced && entry.Source.Chunk == backgroundInput->Source.Chunk;
            }
            CHECK_MSG(sourced, sources);
            const auto backgroundResult = Find(lua, background, Lua::EntryKind::Result);
            CHECK(backgroundResult && backgroundResult->Values.size() == 1 && backgroundResult->Values[0]->Nodes[0].Value == "7");

            CHECK(Lua::LuaEngine::Incomplete(lua.State(), "if ready then"));
            CHECK(Lua::LuaEngine::Incomplete(lua.State(), "print("));
            CHECK(!Lua::LuaEngine::Incomplete(lua.State(), "1 + 1"));
            CHECK(!Lua::LuaEngine::Incomplete(lua.State(), "x = 1"));
            CHECK(!Lua::LuaEngine::Incomplete(lua.State(), "if then"));
        }

        void History()
        {
            CHECK_EQ(Lua::ConsoleHistory::Unescape(Lua::ConsoleHistory::Escape("a\nb\\c")), std::string("a\nb\\c"));
            CHECK_EQ(Lua::ConsoleHistory::Escape("a\nb\\c"), std::string("a\\nb\\\\c"));
            wchar_t temp[MAX_PATH] = L"";
            ::GetTempPathW(MAX_PATH, temp);
            const std::filesystem::path file = std::filesystem::path(temp) / (L"manifold-history-" + std::to_wstring(::GetCurrentProcessId()) + L".txt");
            Lua::ConsoleHistory history(file, 3);
            history.Add("one");
            history.Add("two\nlines");
            history.Add("three");
            history.Add("two\nlines");
            history.Add("   ");
            CHECK(history.Items() == std::vector<std::string>({ "one", "three", "two\nlines" }));
            history.Add("four");
            CHECK(history.Items() == std::vector<std::string>({ "three", "two\nlines", "four" }));
            CHECK(history.Previous("draft") == std::optional<std::string>("four"));
            CHECK(history.Previous("ignored") == std::optional<std::string>("two\nlines"));
            CHECK(history.Previous("ignored") == std::optional<std::string>("three"));
            CHECK(!history.Previous("ignored"));
            CHECK(history.Next() == std::optional<std::string>("two\nlines"));
            CHECK(history.Next() == std::optional<std::string>("four"));
            CHECK(history.Next() == std::optional<std::string>("draft"));
            CHECK(!history.Next() && !history.Browsing());
            CHECK(history.Save());
            Lua::ConsoleHistory loaded(file, 3);
            loaded.Load();
            CHECK(loaded.Items() == history.Items());
            std::error_code error;
            std::filesystem::remove(file, error);
        }
    }

    int RunConsoleTests()
    {
        const int before = Totals().Failures;
        Log();
        Parsing();
        History();
        Settings settings;
        Engine engine(settings);
        engine.Lua().Run("__manifoldHeadless = true", "headless");
        Repl(engine);
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("console", "Lua console", 70, [](const SuiteContext&) { return RunConsoleTests(); });
}
