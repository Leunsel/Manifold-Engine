#include "TestKit.h"

#include "App/Commands.h"
#include "App/Palette.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        PaletteItem Item(PaletteKind kind, std::string key, std::string title, std::string detail = {})
        {
            PaletteItem item;
            item.Kind = kind;
            item.Key = std::move(key);
            item.Title = std::move(title);
            item.Detail = std::move(detail);
            return item;
        }

        std::vector<PaletteItem> Sample()
        {
            std::vector<PaletteItem> items = {
                Item(PaletteKind::Command, "command:OpenProcess", "Open Process", "Command"),
                Item(PaletteKind::Command, "command:OpenTable", "Open Table", "Command"),
                Item(PaletteKind::Command, "command:OpenCodeFilter", "Code Filter", "Command"),
                Item(PaletteKind::Command, "command:OpenStaticPointers", "Find Static Pointers", "Command"),
                Item(PaletteKind::Command, "command:OpenMemoryViewer", "Memory Viewer", "Command"),
                Item(PaletteKind::Command, "command:FirstScan", "First Scan / Next Scan", "Command of the main window"),
                Item(PaletteKind::Window, "window:regions", "Memory Regions", "Window"),
                Item(PaletteKind::Setting, "settings:4", "Settings: Debugger", "Structure Dissect, breakpoints, Unity and the log."),
                Item(PaletteKind::Table, "table:E:\\Tables\\isaac-ng_v5.4.0_Full.CT", "isaac-ng_v5.4.0_Full.CT", "E:\\Tables"),
                Item(PaletteKind::Record, "record:7", "Damage : Multiplier ()->", "Script, off"),
                Item(PaletteKind::Record, "record:8", "Infinite Ammo", "Script, on"),
                Item(PaletteKind::Record, "record:9", "Health", "Value 100"),
            };
            for (std::size_t i = 0; i < items.size(); ++i)
                items[i].Order = static_cast<int>(i);
            return items;
        }

        std::string Top(const std::vector<PaletteItem>& items, std::string_view query, const std::vector<std::string>& recent = {})
        {
            const std::vector<PaletteMatch> matches = RankPalette(items, query, recent, 50);
            return matches.empty() ? std::string() : items[matches.front().Index].Key;
        }

        void CheckFuzzy()
        {
            std::vector<int> positions;
            CHECK(FuzzyScore("cf", "Code Filter", &positions) > 0 && positions == std::vector<int>({ 0, 5 }));
            CHECK(FuzzyScore("fil", "Find Static Pointers ... Code Filter", &positions) > 0 && positions.front() == 30);
            CHECK(FuzzyScore("xyz", "Code Filter") < 0);
            CHECK(FuzzyScore("filtercode", "Code Filter") < 0);
            CHECK(FuzzyScore("OPEN", "open table") > 0);
            // A run of letters at a word start beats the same letters strewn through the text.
            CHECK(FuzzyScore("mem", "Memory Viewer") > FuzzyScore("mem", "Make Empty Map"));
            CHECK(FuzzyScore("ov", "Open Viewer") > FuzzyScore("ov", "Code Overview helper"));
            CHECK(FuzzyScore("Memory Viewer", "Memory Viewer") > FuzzyScore("Memory Viewer", "Memory Viewer Window"));
            // camelCase and digits count as word starts
            CHECK(FuzzyScore("ocf", "OpenCodeFilter", &positions) > 0 && positions == std::vector<int>({ 0, 4, 8 }));
            CHECK(FuzzyScore("54", "isaac-ng_v5.4.0", &positions) > 0 && positions.size() == 2);
        }

        void CheckQuery()
        {
            PaletteQuery query = ParsePaletteQuery("  >  open ");
            CHECK(query.CommandsOnly && !query.Only && query.Text == "open");
            query = ParsePaletteQuery("@ammo");
            CHECK(query.Only == PaletteKind::Record && query.Text == "ammo");
            query = ParsePaletteQuery("#isaac");
            CHECK(query.Only == PaletteKind::Table && query.Text == "isaac");
            query = ParsePaletteQuery("code filter");
            CHECK(!query.Only && !query.CommandsOnly && query.Text == "code filter");
        }

        void CheckRanking()
        {
            const std::vector<PaletteItem> items = Sample();
            CHECK_MSG(Top(items, "cf") == "command:OpenCodeFilter", Top(items, "cf"));
            CHECK(Top(items, "code filter") == "command:OpenCodeFilter");
            CHECK(Top(items, "filter code") == "command:OpenCodeFilter");
            CHECK(Top(items, "mem view") == "command:OpenMemoryViewer");
            CHECK(Top(items, "regions") == "window:regions");
            CHECK(Top(items, "debugger") == "settings:4");
            // a word that only the detail holds still finds the item, below title matches
            CHECK(Top(items, "breakpoints") == "settings:4");
            CHECK(Top(items, "isaac") == "table:E:\\Tables\\isaac-ng_v5.4.0_Full.CT");
            CHECK(Top(items, "ammo") == "record:8");
            CHECK(Top(items, "damage") == "record:7");
            CHECK(RankPalette(items, "zzz", {}, 50).empty());

            // prefixes narrow the kinds
            const std::vector<PaletteMatch> records = RankPalette(items, "@", {}, 50);
            CHECK(records.size() == 3 && std::all_of(records.begin(), records.end(), [&](const PaletteMatch& match) { return items[match.Index].Kind == PaletteKind::Record; }));
            const std::vector<PaletteMatch> commands = RankPalette(items, ">memory", {}, 50);
            CHECK(commands.size() == 2 && std::none_of(commands.begin(), commands.end(), [&](const PaletteMatch& match) { return items[match.Index].Kind == PaletteKind::Record; }));
            CHECK(RankPalette(items, "#", {}, 50).size() == 1);
            CHECK(RankPalette(items, "@open", {}, 50).empty());

            // without a query: no records (they can be thousands), the recent ones first
            const std::vector<std::string> used = { "settings:4", "command:OpenTable" };
            const std::vector<PaletteMatch> empty = RankPalette(items, "", used, 50);
            CHECK(empty.size() == items.size() - 3);
            CHECK(empty.size() >= 2 && items[empty[0].Index].Key == "settings:4" && items[empty[1].Index].Key == "command:OpenTable");
            CHECK(empty.size() >= 3 && items[empty[2].Index].Key == "command:OpenProcess");

            // recent use lifts a match among equals
            CHECK(Top(items, "open") == "command:OpenProcess");
            CHECK(Top(items, "open", { "command:OpenTable" }) == "command:OpenTable");
            CHECK(RankPalette(items, "o", {}, 2).size() == 2);

            // highlight positions point into the title
            const std::vector<PaletteMatch> lit = RankPalette(items, "code fil", {}, 50);
            CHECK(!lit.empty() && lit.front().Highlight == std::vector<int>({ 0, 1, 2, 3, 5, 6, 7 }));
        }

        void CheckRecent()
        {
            std::vector<std::string> recent = SplitPaletteRecent("command:OpenTable|table:C:\\a.CT||record:3");
            CHECK(recent.size() == 3 && recent[1] == "table:C:\\a.CT");
            RememberPaletteKey(recent, "record:3");
            CHECK(recent.front() == "record:3" && recent.size() == 3);
            RememberPaletteKey(recent, "command:OpenLog", 3);
            CHECK(recent.size() == 3 && recent.front() == "command:OpenLog" && recent.back() == "command:OpenTable");
            recent.push_back("bad|key");
            CHECK(JoinPaletteRecent(recent) == "command:OpenLog|record:3|command:OpenTable");
        }

        void CheckCommand()
        {
            const CommandInfo& info = Describe(Command::CommandPalette);
            CHECK(info.Scope == CommandScope::Global && std::string(info.Name) == "Command Palette" && info.Chord == (ImGuiMod_Ctrl | ImGuiKey_K));
            CHECK(ConflictsWith(Command::CommandPalette, info.Chord).empty());
            CHECK(FindCommand("CommandPalette") == Command::CommandPalette);
        }

        int RunPaletteTests()
        {
            const int before = Totals().Failures;
            CheckFuzzy();
            CheckQuery();
            CheckRanking();
            CheckRecent();
            CheckCommand();
            return Totals().Failures - before;
        }
    }

    MANIFOLD_SUITE("palette", "Command palette", 46, [](const SuiteContext&) { return RunPaletteTests(); });
}
