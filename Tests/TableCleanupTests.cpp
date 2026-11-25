#include "TestKit.h"

#include "Core/Text.h"
#include "Table/CheatTable.h"
#include "Table/TableCleanup.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Table;

        void CheckNames()
        {
            CHECK(NamedIn("local p = getStructure('Player')", "player"));
            CHECK(!NamedIn("PlayerData", "Player"));
            CHECK(!NamedIn("myPlayer", "Player"));
            CHECK(NamedIn("x = findTableFile(\"Manifold.Core.lua\")", "Manifold.Core.lua"));
            CHECK(NamedIn("require('Manifold.Core')", "Manifold.Core"));
            CHECK(NamedIn("[[game.exe+10]+INJECT_ptr]", "INJECT_ptr"));
            CHECK(!NamedIn("INJECT_ptr2", "INJECT_ptr"));
            CHECK(!NamedIn("", "x") && !NamedIn("abc", ""));
        }

        void CheckStrip()
        {
            const std::string script =
                "{\n  Header : Damage\n  Author : Someone // with a link\n}\n\n[ENABLE]\n// a comment line\n{$strict}\nalloc(n,$100) // trailing\n"
                "label(r)\n\n\n\nn:\n  db 'not // a comment' 0\n  mov eax,1 { inline }\n{$lua}\nreturn 10 // 3 -- Lua's own\n{$asm}\n  jmp r\n[DISABLE]\n";
            const std::string stripped = StripAutoAssemblerComments(script);
            CHECK_MSG(stripped.find("Header") == std::string::npos && stripped.find("a comment line") == std::string::npos && stripped.find("trailing") == std::string::npos,
                stripped);
            CHECK(stripped.find("{$strict}") != std::string::npos && stripped.find("db 'not // a comment' 0") != std::string::npos);
            CHECK(stripped.find("inline") == std::string::npos && stripped.find("  mov eax,1\n") != std::string::npos);
            CHECK_MSG(stripped.find("return 10 // 3 -- Lua's own") != std::string::npos && stripped.find("{$asm}") != std::string::npos, stripped);
            CHECK_MSG(stripped.starts_with("[ENABLE]\n") && stripped.find("\n\n\n") == std::string::npos && stripped.ends_with("[DISABLE]\n"), stripped);
            CHECK(stripped.find("alloc(n,$100)\nlabel(r)") != std::string::npos);
            const std::string crlf = StripAutoAssemblerComments("[ENABLE]\r\n// x\r\nnop\r\n");
            CHECK_MSG(crlf == "[ENABLE]\r\nnop\r\n", crlf);
            CHECK(StripAutoAssemblerComments("nop") == "nop");
            CHECK(StripAutoAssemblerComments("{ never closed\nnop") == "{ never closed\nnop");
            // C code keeps its braces up to {$asm}, as Lua does
            const std::string c = StripAutoAssemblerComments("{$c}\nint add(int a, int b) { return a + b; } // sum\n{$asm}\nnop // x\n");
            CHECK_MSG(c == "{$c}\nint add(int a, int b) { return a + b; } // sum\n{$asm}\nnop\n", c);
            const std::string ccode = StripAutoAssemblerComments("{$ccode x=eax}\nif (x) { x = 0; }\n{$asm}\n");
            CHECK_MSG(ccode.find("if (x) { x = 0; }") != std::string::npos, ccode);
            // a comment over several lines between two instructions leaves them on lines of their own
            const std::string spanned = StripAutoAssemblerComments("mov eax,1 { one\ntwo } mov ebx,2\n");
            CHECK_MSG(spanned == "mov eax,1\nmov ebx,2\n", spanned);
            CHECK_EQ(StripAutoAssemblerComments("{ only\na comment }\nnop\n"), std::string("nop\n"));
        }

        std::string SampleTable()
        {
            AddressList list;
            list.TableLuaScript = "local f = findTableFile('helper.lua')\nlocal g = getAddressList().getMemoryRecordByDescription('Dynamic')\nrequire('Hooks')\n"
                                  "local theme = findTableFile('Theme.Dark-' .. name .. '.Min.json')\n";
            list.TableComment = "Made by someone. Press F1 to start.";
            list.DisasmNotes["game.exe+10"] = { "my note", "" };
            list.DisasmNotes["game.exe+20"] = { "", "Header" };
            MemoryRecord& script = list.AddScript("Damage", "{ credits }\n[ENABLE]\n// make it hurt\nmov eax,[PlayerBase]\nmov ebx,[eax+Player.Health]\n[DISABLE]\n");
            (void)script;
            list.AddValue("Health", "[INJECT_ptr]+10", Scan::ValueType::FourBytes);
            list.AddGroup("Old Stuff");
            list.AddGroup("Dynamic");
            list.AddGroup("");  // a spacer between sections
            MemoryRecord& listHolder = list.AddGroup("listBool");
            listHolder.DropDown.push_back({ "0", "False" });
            listHolder.DropDown.push_back({ "1", "True" });
            MemoryRecord& kept = list.AddGroup("Kept");
            list.AddValue("Inside", "game.exe+4", Scan::ValueType::FourBytes, &kept);
            TableFile& helper = list.AddFile("helper.lua");
            const std::string helperText = "return { player = 'Player2' }";
            helper.Data->assign(helperText.begin(), helperText.end());
            TableFile& hooks = list.AddFile("Hooks.CEA");
            const std::string hooksText = "{\n  generated header\n}\n[ENABLE]\nnop // x\n[DISABLE]\n";
            hooks.Data->assign(hooksText.begin(), hooksText.end());
            TableFile& orphan = list.AddFile("orphan.dat");
            orphan.Data->assign(300, 0x41);
            TableFile& theme = list.AddFile("Theme.Dark-Aqua.Min.json");
            const std::string themeText = "{ \"accent\": 1 }";
            theme.Data->assign(themeText.begin(), themeText.end());
            StructureStore structures;
            for (const char* name : { "Player", "Player2", "Unused" })
            {
                auto structure = std::make_unique<StructDefinition>();
                structure->Name = name;
                StructField field;
                field.Name = "Health";
                structure->Fields.push_back(field);
                structures.Items.push_back(std::move(structure));
            }
            const std::vector<TableSymbol> symbols = { { "PlayerBase", "game.exe+100" }, { "INJECT_ptr", "game.exe+200" }, { "Leftover", "game.exe+300" } };
            return WriteTable(list, structures, symbols, FileNaming::Tag);
        }

        bool HasKey(const CleanupPlan& plan, const std::string& key)
        {
            return std::any_of(plan.Items.begin(), plan.Items.end(), [&](const CleanupItem& item) { return item.Key == key; });
        }

        void CheckPlan()
        {
            const std::string text = SampleTable();
            LoadedTable loaded;
            CHECK(ParseTable(text, loaded).Ok());
            const CleanupPlan plan = PlanCleanup(loaded, text.size());
            std::string keys;
            for (const CleanupItem& item : plan.Items)
                keys += item.Key + " ";
            CHECK_MSG(HasKey(plan, "structure:Unused") && !HasKey(plan, "structure:Player") && !HasKey(plan, "structure:Player2"), keys);
            CHECK_MSG(HasKey(plan, "symbol:Leftover") && !HasKey(plan, "symbol:PlayerBase") && !HasKey(plan, "symbol:INJECT_ptr"), keys);
            CHECK_MSG(HasKey(plan, "file:orphan.dat") && !HasKey(plan, "file:helper.lua") && !HasKey(plan, "file:Hooks.CEA"), keys);
            const auto theme = std::find_if(plan.Items.begin(), plan.Items.end(), [](const CleanupItem& item) { return item.Key == "file:Theme.Dark-Aqua.Min.json"; });
            CHECK_MSG(theme != plan.Items.end() && !theme->Recommended && theme->Reason.find("Theme.Dark-") != std::string::npos,
                theme == plan.Items.end() ? keys : theme->Reason);
            const auto oldStuff = std::find_if(plan.Items.begin(), plan.Items.end(), [](const CleanupItem& item) { return item.Kind == CleanupKind::EmptyGroup; });
            CHECK_MSG(oldStuff != plan.Items.end() && oldStuff->Name == "Old Stuff" &&
                          std::count_if(plan.Items.begin(), plan.Items.end(), [](const CleanupItem& item) { return item.Kind == CleanupKind::EmptyGroup; }) == 1,
                keys);
            CHECK(HasKey(plan, "disasm") && HasKey(plan, "comment") && HasKey(plan, "scriptcomments") && HasKey(plan, "filecomments"));
            for (const CleanupItem& item : plan.Items)
            {
                const bool defaultOff = item.Kind == CleanupKind::TableComment || item.Kind == CleanupKind::ScriptComments || item.Kind == CleanupKind::FileComments ||
                                        item.Kind == CleanupKind::EmptyGroup || item.Key == "file:Theme.Dark-Aqua.Min.json";
                CHECK_MSG(item.Recommended != defaultOff, item.Key);
                CHECK_MSG(item.Bytes > 0, item.Key);
            }

            // Nothing chosen: the copy is the table as Save writes it.
            const Result<std::string> same = CleanTableText(text, {});
            CHECK_MSG(same.Ok() && *same == text, "a cleanup without choices changed the table");

            const Result<std::string> cleaned = CleanTableText(text, plan.Items);
            CHECK(cleaned.Ok() && cleaned->size() < text.size());
            if (!cleaned)
                return;
            LoadedTable after;
            CHECK(ParseTable(*cleaned, after).Ok());
            CHECK(after.Structures.size() == 2 && after.Symbols.size() == 2 && after.Files.size() == 2);
            CHECK(std::any_of(after.Records.begin(), after.Records.end(), [](const auto& record) { return record->Description.empty(); }));
            CHECK(std::any_of(after.Records.begin(), after.Records.end(), [](const auto& record) { return record->Description == "listBool" && record->DropDown.size() == 2; }));
            CHECK(after.DisasmNotes.empty() && after.Comments.empty() && after.LuaScript == loaded.LuaScript);
            std::size_t groups = 0;
            std::string damage;
            for (const auto& record : after.Records)
            {
                if (record->Kind == RecordKind::Group)
                    ++groups;
                if (record->Description == "Damage")
                    damage = record->Script;
            }
            CHECK(groups == 4 && after.Records.size() == loaded.Records.size() - 1);
            CHECK_MSG(damage.find("credits") == std::string::npos && damage.find("make it hurt") == std::string::npos && damage.find("mov ebx,[eax+Player.Health]") != std::string::npos,
                damage);
            const auto hooks = std::find_if(after.Files.begin(), after.Files.end(), [](const TableFile& file) { return file.Name == "Hooks.CEA"; });
            CHECK(hooks != after.Files.end() && std::string(hooks->Data->begin(), hooks->Data->end()) == "[ENABLE]\nnop\n[DISABLE]\n");

            // only what is chosen goes
            std::vector<CleanupItem> one;
            for (const CleanupItem& item : plan.Items)
            {
                if (item.Key == "symbol:Leftover")
                    one.push_back(item);
            }
            const Result<std::string> partial = CleanTableText(text, one);
            LoadedTable partly;
            CHECK(partial.Ok() && ParseTable(*partial, partly).Ok() && partly.Symbols.size() == 2 && partly.Structures.size() == 3 && partly.Files.size() == 4 &&
                  partly.DisasmNotes.size() == 2 && !partly.Comments.empty());
        }

        int RunTableCleanupTests()
        {
            const int before = Totals().Failures;
            CheckNames();
            CheckStrip();
            CheckPlan();
            return Totals().Failures - before;
        }

        // Read only: what the cleanup would offer for a table file, and the size of the copy with the recommended items.
        int RunCleanupPlan(const std::vector<std::string>& arguments)
        {
            if (arguments.size() < 2)
                return 2;
            LoadedTable loaded;
            const std::filesystem::path path = Utf8ToWide(arguments[1]);
            if (Status status = LoadTable(path, loaded); !status)
            {
                std::printf("load failed: %s\n", status.Text.c_str());
                return 3;
            }
            AddressList list;
            StructureStore store;
            std::vector<TableSymbol> symbols = loaded.Symbols;
            // the text Save would write, the way the window starts from it
            const std::string original = [&]
            {
                LoadedTable copy;
                LoadTable(path, copy);
                list.TableLuaScript = copy.LuaScript;
                list.TableComment = copy.Comments;
                list.DisasmNotes = std::move(copy.DisasmNotes);
                list.Files = std::move(copy.Files);
                list.ExtraXml = std::move(copy.ExtraXml);
                list.LoadedVersion = copy.Version;
                list.LoadedFileNaming = copy.Naming;
                for (const auto& record : copy.Records)
                    list.NoteId(record->Id);
                for (auto& record : copy.Records)
                    list.Add(std::move(record));
                for (auto& structure : copy.Structures)
                    store.Items.push_back(std::move(structure));
                return WriteTable(list, store, symbols, copy.Naming);
            }();
            LoadedTable parsed;
            ParseTable(original, parsed);
            const CleanupPlan plan = PlanCleanup(parsed, original.size());
            std::vector<CleanupItem> recommended;
            for (const CleanupItem& item : plan.Items)
            {
                std::printf("%-3s %-22s %-40s %8zu  %s\n", item.Recommended ? "[x]" : "[ ]", CleanupKindName(item.Kind), item.Name.c_str(), item.Bytes, item.Reason.c_str());
                if (item.Recommended)
                    recommended.push_back(item);
            }
            const Result<std::string> cleaned = CleanTableText(original, recommended);
            const Result<std::string> everything = CleanTableText(original, plan.Items);
            std::printf("\n%zu items, %zu recommended; %zu bytes -> %zu recommended, %zu everything\n", plan.Items.size(), recommended.size(), original.size(),
                cleaned ? cleaned->size() : 0, everything ? everything->size() : 0);
            return 0;
        }
    }

    MANIFOLD_MODE("--cleanup-plan", &RunCleanupPlan);

    MANIFOLD_SUITE("tablecleanup", "Table cleanup", 47, [](const SuiteContext&) { return RunTableCleanupTests(); });
}
