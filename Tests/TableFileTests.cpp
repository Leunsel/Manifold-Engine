#include "TestKit.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/ImageDecode.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "Table/CheatTable.h"
#include "Table/Structures.h"
#include "Table/TableFiles.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        std::vector<std::uint8_t> Bytes(std::string_view text)
        {
            return std::vector<std::uint8_t>(text.begin(), text.end());
        }

        void Put(Table::AddressList& list, const std::string& name, std::string_view text)
        {
            Table::SetFileData(list, list.AddFile(name), Bytes(text));
        }

        std::vector<std::string> Names(const std::vector<Table::FileRow>& rows)
        {
            std::vector<std::string> names;
            for (const Table::FileRow& row : rows)
                names.push_back(row.Name);
            return names;
        }

        void Kinds()
        {
            using Table::FileKind;
            using namespace std::string_view_literals;
            CHECK(Table::DetectFileKind("module.lua", Bytes("return {}")) == FileKind::Lua);
            CHECK(Table::DetectFileKind("Module.LUA", Bytes("return {}")) == FileKind::Lua);
            CHECK(Table::DetectFileKind("theme.json", Bytes("{\"a\":1}")) == FileKind::Json);
            CHECK(Table::DetectFileKind("theme", Bytes("  {\"a\": [1, 2]}")) == FileKind::Json);
            CHECK(Table::DetectFileKind("form.xml", Bytes("<a/>")) == FileKind::Xml);
            CHECK(Table::DetectFileKind("table", Bytes("<?xml version=\"1.0\"?><CheatTable/>")) == FileKind::Xml);
            CHECK(Table::DetectFileKind("Hook.CEA", Bytes("[ENABLE]\nnop\n[DISABLE]\n")) == FileKind::AutoAssembler);
            CHECK(Table::DetectFileKind("notes.txt", Bytes("hello")) == FileKind::Text);
            CHECK(Table::DetectFileKind("notes", Bytes("hello there")) == FileKind::Text);
            CHECK(Table::DetectFileKind("new.txt", {}) == FileKind::Text);
            CHECK(Table::DetectFileKind("empty.lua", {}) == FileKind::Lua);
            CHECK(Table::DetectFileKind("empty.png", {}) == FileKind::Image);
            CHECK(Table::DetectFileKind("umlaut.txt", Bytes("Gr\xC3\xBC\xC3\x9F" "e")) == FileKind::Text);
            CHECK(Table::DetectFileKind("wrong.txt", Bytes("\x89PNG\r\n\x1A\n...."sv)) == FileKind::Image);
            CHECK(Table::DetectFileKind("photo", Bytes("\xFF\xD8\xFF\xE0" "JFIF"sv)) == FileKind::Image);
            CHECK(Table::DetectFileKind("anim.gif", Bytes("GIF89a....")) == FileKind::Image);
            CHECK(Table::DetectFileKind("sound.dat", Bytes("RIFF\x10\0\0\0WAVEfmt "sv)) == FileKind::Audio);
            CHECK(Table::DetectFileKind("pack.bin", Bytes("PK\x03\x04rest"sv)) == FileKind::Archive);
            CHECK(Table::DetectFileKind("blob", Bytes("\x01\x02\x00\x03"sv)) == FileKind::Binary);
            CHECK(Table::DetectFileKind("blob.wav", Bytes("\x01\x02\x00\x03"sv)) == FileKind::Audio);
            std::vector<std::uint8_t> program(128, 0);
            program[0] = 'M';
            program[1] = 'Z';
            CHECK(Table::DetectFileKind("tool.dat", program) == FileKind::Program);
            CHECK(Table::IsTextKind(FileKind::Lua) && Table::IsTextKind(FileKind::Text) && !Table::IsTextKind(FileKind::Image) && !Table::IsTextKind(FileKind::Binary));
            CHECK(Table::LooksLikeText({}));
            CHECK(!Table::LooksLikeText(Bytes("ab\0cd"sv)));
            CHECK(!Table::LooksLikeText(Bytes("\xC3")));
            std::vector<std::uint8_t> large(200 * 1024, 'a');
            large[70 * 1024] = 0;
            CHECK(Table::LooksLikeText(large));
            CHECK_EQ(Table::FileExtension("a.b.LUA"), std::string("lua"));
            CHECK(Table::FileExtension("noext").empty() && Table::FileExtension("dot.").empty());
            CHECK_EQ(std::string(Table::FileKindName(FileKind::AutoAssembler)), std::string("Auto Assembler"));
        }

        void Arrange()
        {
            using Table::FileColumn;
            using Table::FileKind;
            std::vector<Table::FileRow> rows = {
                { "hook10.CEA", FileKind::AutoAssembler, 300, 120, 0 },
                { "Hook2.cea", FileKind::AutoAssembler, 200, 150, 3 },
                { "theme.json", FileKind::Json, 5000, 900, 1 },
                { "Bootstrap.lua", FileKind::Lua, 80, -1, 2 },
                { "logo.png", FileKind::Image, 5000, 5100, 0 },
            };
            const auto names = [&](std::string_view filter, FileColumn sort, bool descending) { return Names(Table::ArrangeFiles(rows, filter, sort, descending)); };
            CHECK(names("", FileColumn::Order, false) == (std::vector<std::string>{ "hook10.CEA", "Hook2.cea", "theme.json", "Bootstrap.lua", "logo.png" }));
            CHECK(names("", FileColumn::Order, true) == (std::vector<std::string>{ "logo.png", "Bootstrap.lua", "theme.json", "Hook2.cea", "hook10.CEA" }));
            CHECK(names("", FileColumn::Name, false) == (std::vector<std::string>{ "Bootstrap.lua", "Hook2.cea", "hook10.CEA", "logo.png", "theme.json" }));
            CHECK(names("", FileColumn::Name, true) == (std::vector<std::string>{ "theme.json", "logo.png", "hook10.CEA", "Hook2.cea", "Bootstrap.lua" }));
            CHECK(names("", FileColumn::Size, true) == (std::vector<std::string>{ "logo.png", "theme.json", "hook10.CEA", "Hook2.cea", "Bootstrap.lua" }));
            CHECK(names("", FileColumn::Size, false) == (std::vector<std::string>{ "Bootstrap.lua", "Hook2.cea", "hook10.CEA", "logo.png", "theme.json" }));
            CHECK(names("", FileColumn::Stored, false).front() == "Bootstrap.lua");
            CHECK(names("", FileColumn::Uses, true) == (std::vector<std::string>{ "Hook2.cea", "Bootstrap.lua", "theme.json", "hook10.CEA", "logo.png" }));
            CHECK(names("", FileColumn::Kind, false) == (std::vector<std::string>{ "Hook2.cea", "hook10.CEA", "logo.png", "theme.json", "Bootstrap.lua" }));
            CHECK(names("HOOK", FileColumn::Name, false) == (std::vector<std::string>{ "Hook2.cea", "hook10.CEA" }));
            CHECK(names("  json ", FileColumn::Order, false) == (std::vector<std::string>{ "theme.json" }));
            CHECK(names("image", FileColumn::Order, false) == (std::vector<std::string>{ "logo.png" }));
            CHECK(names("hook assembler 10", FileColumn::Order, false) == (std::vector<std::string>{ "hook10.CEA" }));
            CHECK(names("nothing", FileColumn::Order, false).empty());
            CHECK(Table::NaturalLess("file2", "file10") && !Table::NaturalLess("file10", "file2"));
            CHECK(Table::NaturalLess("a", "B") && Table::NaturalLess("A", "b") && !Table::NaturalLess("b", "A"));
            CHECK(Table::NaturalLess("x007", "x8") && Table::NaturalLess("abc", "abcd") && !Table::NaturalLess("same", "same"));
        }

        void NamesAndEdits()
        {
            Table::AddressList list;
            Put(list, "Bootstrap.lua", "return 1");
            Put(list, "theme.json", "{}");
            list.SetClean();
            CHECK(!list.Dirty());

            CHECK(Table::FileNameProblem(list, "new.lua").empty());
            CHECK(!Table::FileNameProblem(list, "").empty() && !Table::FileNameProblem(list, "   ").empty());
            CHECK(!Table::FileNameProblem(list, "BOOTSTRAP.LUA").empty());
            CHECK(Table::FileNameProblem(list, "BOOTSTRAP.LUA", "Bootstrap.lua").empty());
            CHECK(!Table::FileNameProblem(list, "a/b.lua").empty() && !Table::FileNameProblem(list, "a\\b").empty() && !Table::FileNameProblem(list, "a<b>").empty());
            CHECK(!Table::FileNameProblem(list, "line\nbreak").empty());
            CHECK(!Table::FileNameProblem(list, std::string(Table::kMaxFileNameBytes + 1, 'a')).empty());
            CHECK(Table::FileNameProblem(list, "with space (2).txt").empty());

            CHECK_EQ(Table::UniqueFileName(list, "fresh.txt"), std::string("fresh.txt"));
            CHECK_EQ(Table::UniqueFileName(list, "bootstrap.lua"), std::string("bootstrap_2.lua"));
            Put(list, "bootstrap_2.lua", "x");
            CHECK_EQ(Table::UniqueFileName(list, "Bootstrap.lua"), std::string("Bootstrap_3.lua"));
            Put(list, "plain", "x");
            CHECK_EQ(Table::UniqueFileName(list, "plain"), std::string("plain_2"));
            list.SetClean();

            CHECK(!Table::RenameFile(list, "missing.lua", "other.lua") && !list.Dirty());
            CHECK(!Table::RenameFile(list, "Bootstrap.lua", "theme.json") && !list.Dirty());
            CHECK(!Table::RenameFile(list, "Bootstrap.lua", " ") && !list.Dirty());
            CHECK(Table::RenameFile(list, "Bootstrap.lua", "Bootstrap.lua") && !list.Dirty());
            const std::uint64_t generation = list.Generation();
            CHECK(Table::RenameFile(list, "bootstrap.lua", "  Start.lua "));
            CHECK(list.Dirty() && list.Generation() != generation && list.FindFile("start.lua") && !list.FindFile("Bootstrap.lua"));
            CHECK_EQ(list.Files[0].Name, std::string("Start.lua"));
            list.SetClean();
            CHECK(Table::RenameFile(list, "Start.lua", "START.lua") && list.Dirty() && list.Files[0].Name == "START.lua");

            list.SetClean();
            Table::TableFile* file = list.FindFile("theme.json");
            CHECK(file != nullptr);
            if (!file)
                return;
            const std::uint64_t version = *file->Version;
            const auto* data = file->Data.get();
            Table::SetFileData(list, *file, Bytes("{\"dark\":true}"));
            CHECK(list.Dirty() && *file->Version == version + 1 && file->Data.get() == data && file->Data->size() == 13);

            list.SetClean();
            const Table::TableFile* copy = Table::DuplicateFile(list, "theme.json");
            CHECK(copy && copy->Name == "theme_2.json" && list.Dirty());
            CHECK(copy && *copy->Data == Bytes("{\"dark\":true}") && copy->Data.get() != list.FindFile("theme.json")->Data.get());
            CHECK(Names({ { list.Files[0].Name }, { list.Files[1].Name }, { list.Files[2].Name } }) == (std::vector<std::string>{ "START.lua", "theme.json", "theme_2.json" }));
            CHECK(!Table::DuplicateFile(list, "missing"));

            list.SetClean();
            CHECK_EQ(Table::RemoveFiles(list, { "missing" }), 0u);
            CHECK(!list.Dirty());
            const std::size_t before = list.Files.size();
            CHECK_EQ(Table::RemoveFiles(list, { "THEME.JSON", "plain", "missing" }), 2u);
            CHECK(list.Dirty() && list.Files.size() == before - 2 && !list.FindFile("theme.json") && list.FindFile("theme_2.json"));
        }

        void Uses()
        {
            const auto quoted = Table::QuotedNames("local a = findTableFile('One.lua')\n-- don't use \"two.json\" here\nx = \"\" .. 'three'");
            CHECK(quoted.size() >= 3);
            const auto has = [&](std::string_view name, int line)
            {
                for (const auto& [text, at] : quoted)
                {
                    if (text == name && at == line)
                        return true;
                }
                return false;
            };
            CHECK(has("One.lua", 1) && has("two.json", 2) && has("three", 3));

            Table::AddressList list;
            Put(list, "Bootstrap.lua", "local json = findTableFile('Json.lua')\nlocal theme = findTableFile(\"Theme.json\")\nlocal self = 'Bootstrap.lua'");
            Put(list, "Json.lua", "return {}");
            Put(list, "theme.json", "{\"name\":\"Json.lua\"}");
            Put(list, "Unused.bin", std::string_view("\x00\x01'Json.lua'", 12));
            Put(list, "Hook.CEA", "[ENABLE]\n[DISABLE]\n");
            list.TableLuaScript = "-- start\nlocal boot = findTableFile('bootstrap.lua')\nload(boot.Stream.DataString)()\nfindTableFile('bootstrap.lua')";
            Table::MemoryRecord& script = list.AddScript("Enable hooks", "[ENABLE]\n{$lua}\nreturn findTableFile('Hook.CEA').Stream.DataString\n{$asm}\n[DISABLE]\n");
            list.AddValue("Health", "game.exe+10", Scan::ValueType::FourBytes);

            const Table::FileUses uses = Table::FindFileUses(list);
            const auto of = [&](std::string_view name) -> std::vector<Table::FileUse>
            {
                const auto found = uses.find(Table::FileUseKey(name));
                return found == uses.end() ? std::vector<Table::FileUse>() : found->second;
            };
            const auto boot = of("Bootstrap.lua");
            CHECK(boot.size() == 1 && boot[0].Where == Table::FileUse::Place::TableScript && boot[0].Line == 2);
            const auto json = of("json.lua");
            CHECK(json.size() == 2 && json[0].Where == Table::FileUse::Place::File && json[0].Label == "Bootstrap.lua" && json[0].Line == 1 && json[1].Label == "theme.json");
            const auto theme = of("theme.json");
            CHECK(theme.size() == 1 && theme[0].Label == "Bootstrap.lua" && theme[0].Line == 2);
            const auto hook = of("hook.cea");
            CHECK(hook.size() == 1 && hook[0].Where == Table::FileUse::Place::Record && hook[0].RecordId == script.Id && hook[0].Label == "Enable hooks" && hook[0].Line == 3);
            CHECK(of("Unused.bin").empty());
            CHECK(Table::FindFileUses(list, 16).count("json.lua") == 0);

            CHECK_EQ(Table::LuaFindSnippet("Bootstrap.lua"), std::string("findTableFile('Bootstrap.lua')"));
            CHECK_EQ(Table::LuaFindSnippet("it's\\here"), std::string("findTableFile('it\\'s\\\\here')"));
            CHECK(Table::LuaReadSnippet("a.lua").find("findTableFile('a.lua').Stream") != std::string::npos);
        }

        void RoundTrip()
        {
            Table::AddressList list;
            std::vector<std::uint8_t> binary(70000);
            for (std::size_t i = 0; i < binary.size(); ++i)
                binary[i] = static_cast<std::uint8_t>((i * 31) ^ (i >> 7));
            Put(list, "Manifold.Bootstrap.lua", "return 'boot'");
            Put(list, "_InitMainHooks.CEA", "[ENABLE]\n[DISABLE]\n");
            Put(list, "new file (2).txt", "spaces & brackets");
            Put(list, "9lives.bin", std::string_view(reinterpret_cast<const char*>(binary.data()), binary.size()));
            Put(list, "Gr\xC3\xBC\xC3\x9F" "e.txt", "umlaut");
            Put(list, "empty.dat", "");
            Put(list, "File", "plain name that equals the fallback tag");
            CHECK(Table::IsXmlName("Manifold.Bootstrap.lua") && Table::IsXmlName("_InitMainHooks.CEA") && Table::IsXmlName("Dark-Aqua.Min.json") && Table::IsXmlName("File"));
            CHECK(!Table::IsXmlName("new file (2).txt") && !Table::IsXmlName("9lives.bin") && !Table::IsXmlName("") && !Table::IsXmlName("a&b") && !Table::IsXmlName("-x"));

            const Table::StructureStore structures;
            const std::string xml = Table::WriteTable(list, structures, {});
            CHECK(xml.find("<Manifold.Bootstrap.lua Encoding=\"Ascii85\">") != std::string::npos);
            CHECK(xml.find("<File2 Name=\"new file (2).txt\" Encoding=\"Ascii85\">") != std::string::npos);
            CHECK(xml.find("<File Encoding=\"Ascii85\">") != std::string::npos);
            CHECK(xml.find("<new file") == std::string::npos && xml.find("<9lives") == std::string::npos);
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable(xml, loaded).Ok());
            CHECK(loaded.Warnings.empty());
            CHECK_EQ(loaded.Files.size(), list.Files.size());
            for (std::size_t i = 0; i < list.Files.size() && i < loaded.Files.size(); ++i)
            {
                CHECK_MSG(loaded.Files[i].Name == list.Files[i].Name, list.Files[i].Name);
                CHECK_MSG(*loaded.Files[i].Data == *list.Files[i].Data, list.Files[i].Name);
            }
            CHECK_EQ(Table::StoredFileSize(*list.Files[0].Data), Table::EncodeTableFile(*list.Files[0].Data).size());
            CHECK(Table::StoredFileSize(binary) > 0);

            Table::LoadedTable old;
            CHECK(Table::ParseTable("<CheatTable><Files><Old.lua Encoding=\"Ascii85\">" + Table::EncodeTableFile(Bytes("x")) + "</Old.lua></Files></CheatTable>", old).Ok());
            CHECK(old.Files.size() == 1 && old.Files[0].Name == "Old.lua" && *old.Files[0].Data == Bytes("x"));
            CHECK(old.Naming == Table::FileNaming::Tag);

            // Structure fields from a table keep the editor's limits (2 GB per row was allocated every frame).
            Table::LoadedTable huge;
            CHECK(Table::ParseTable("<CheatTable><Structures><Structure Name=\"S\"><Elements>"
                "<Element Offset=\"2147483000\" Vartype=\"String\" Bytesize=\"2000000000\"/>"
                "<Element Offset=\"-99999999999\" Vartype=\"Array of byte\" Bytesize=\"-5\"/>"
                "</Elements></Structure></Structures></CheatTable>", huge).Ok());
            CHECK(huge.Structures.size() == 1 && huge.Structures[0]->Fields.size() == 2);
            if (huge.Structures.size() == 1 && huge.Structures[0]->Fields.size() == 2)
            {
                const Table::StructDefinition& shape = *huge.Structures[0];
                CHECK(shape.Fields[0].Size == 1 && shape.Fields[0].Offset == -Table::kMaxFieldOffset);
                CHECK(shape.Fields[1].Size == Table::kMaxFieldBytes && shape.Fields[1].Offset == Table::kMaxFieldOffset);
                CHECK_EQ(shape.Size(), Table::kMaxFieldOffset + Table::kMaxFieldBytes);
            }
            CHECK(loaded.Naming == Table::FileNaming::Attribute);

            // CE 7.6 spelling: every file as <FileN Name=...>, read back with the names; the tag spelling forced back.
            const std::string modern = Table::WriteTable(list, structures, {}, Table::FileNaming::Attribute);
            CHECK(modern.find("<File0 Name=\"Manifold.Bootstrap.lua\" Encoding=\"Ascii85\">") != std::string::npos);
            CHECK(modern.find("<File6 Name=\"File\" Encoding=\"Ascii85\">") != std::string::npos);
            CHECK(modern.find("<Manifold.Bootstrap.lua") == std::string::npos);
            Table::LoadedTable reread;
            CHECK(Table::ParseTable(modern, reread).Ok());
            CHECK(reread.Naming == Table::FileNaming::Attribute && reread.Files.size() == list.Files.size());
            for (std::size_t i = 0; i < list.Files.size() && i < reread.Files.size(); ++i)
                CHECK_MSG(reread.Files[i].Name == list.Files[i].Name && *reread.Files[i].Data == *list.Files[i].Data, list.Files[i].Name);
            list.LoadedFileNaming = Table::FileNaming::Attribute;
            CHECK(Table::WriteTable(list, structures, {}).find("<File0 Name=\"Manifold.Bootstrap.lua\"") != std::string::npos);
            CHECK(Table::WriteTable(list, structures, {}, Table::FileNaming::Tag).find("<Manifold.Bootstrap.lua Encoding=\"Ascii85\">") != std::string::npos);
        }

        void ExportNames()
        {
            CHECK(Table::SafeExportName("Bootstrap.lua") && Table::SafeExportName("new file (2).txt") && Table::SafeExportName("Gr\xC3\xBC\xC3\x9F" "e.txt"));
            CHECK(Table::SafeExportName(".hidden") && Table::SafeExportName("console.lua") && Table::SafeExportName("COM10") && Table::SafeExportName("nul_and_more.txt"));
            CHECK(!Table::SafeExportName("") && !Table::SafeExportName(".") && !Table::SafeExportName(".."));
            CHECK(!Table::SafeExportName("..\\..\\Startup\\x.bat") && !Table::SafeExportName("../x.lua") && !Table::SafeExportName("C:\\Windows\\x.dll"));
            CHECK(!Table::SafeExportName("\\\\server\\share\\x") && !Table::SafeExportName("a:stream") && !Table::SafeExportName("C:"));
            CHECK(!Table::SafeExportName("tab\there") && !Table::SafeExportName(std::string_view("nul\0byte", 8)) && !Table::SafeExportName("a<b") && !Table::SafeExportName("a?b"));
            CHECK(!Table::SafeExportName("CON") && !Table::SafeExportName("con.txt") && !Table::SafeExportName("Nul.tar.gz") && !Table::SafeExportName("aux"));
            CHECK(!Table::SafeExportName("COM1") && !Table::SafeExportName("lpt9.log") && !Table::SafeExportName("COM0") && !Table::SafeExportName("CONIN$"));
            CHECK(!Table::SafeExportName("COM\xC2\xB9") && !Table::SafeExportName("NUL .txt"));
            CHECK(!Table::SafeExportName("trailing.") && !Table::SafeExportName("trailing "));

            // A loaded name stays (loading never checks it), only a new name is refused.
            Table::AddressList list;
            Put(list, "CON.lua", "x");
            CHECK(!Table::FileNameProblem(list, "nul.txt").empty() && !Table::FileNameProblem(list, "name.").empty() && !Table::FileNameProblem(list, "..").empty());
            CHECK(Table::FileNameProblem(list, "CON.lua", "CON.lua").empty());
            CHECK(!Table::RenameFile(list, "CON.lua", "PRN") && list.FindFile("CON.lua"));
            CHECK(Table::RenameFile(list, "CON.lua", "console.lua") && list.FindFile("console.lua"));
        }

        void DuplicateNames()
        {
            const std::string one = Table::EncodeTableFile(Bytes("one"));
            const std::string two = Table::EncodeTableFile(Bytes("two"));
            const std::string three = Table::EncodeTableFile(Bytes("three"));
            Table::LoadedTable loaded;
            CHECK(Table::ParseTable("<CheatTable><Files>"
                                    "<File0 Name=\"a.lua\" Encoding=\"Ascii85\">" + one + "</File0>"
                                    "<File1 Name=\"A.LUA\" Encoding=\"Ascii85\">" + two + "</File1>"
                                    "<File2 Name=\"A_2.lua\" Encoding=\"Ascii85\">" + three + "</File2>"
                                    "<File3 Name=\"a.lua\" Encoding=\"Ascii85\">" + one + "</File3>"
                                    "<b.txt Encoding=\"Ascii85\">" + one + "</b.txt>"
                                    "</Files></CheatTable>", loaded).Ok());
            CHECK_EQ(loaded.Files.size(), 5u);
            if (loaded.Files.size() != 5)
                return;
            CHECK_EQ(loaded.Files[0].Name, std::string("a.lua"));
            CHECK_EQ(loaded.Files[1].Name, std::string("A_3.LUA"));
            CHECK_EQ(loaded.Files[2].Name, std::string("A_2.lua"));
            CHECK_EQ(loaded.Files[3].Name, std::string("a_4.lua"));
            CHECK_EQ(loaded.Files[4].Name, std::string("b.txt"));
            CHECK(*loaded.Files[1].Data == Bytes("two") && *loaded.Files[2].Data == Bytes("three"));
            CHECK(loaded.Warnings.size() == 1 && loaded.Warnings[0].find("2 table files") != std::string::npos && loaded.Warnings[0].find("A.LUA is now A_3.LUA") != std::string::npos);
            std::set<std::string> keys;
            for (const Table::TableFile& file : loaded.Files)
                keys.insert(Table::FileUseKey(file.Name));
            CHECK_EQ(keys.size(), loaded.Files.size());
        }

        void Content()
        {
            Table::AddressList list;
            Table::StructureStore structures;
            CHECK(!Table::TableHasContent(list, structures, {}));
            list.DisasmNotes["game.exe+10"] = {};
            CHECK(!Table::TableHasContent(list, structures, {}));
            list.DisasmNotes["game.exe+10"].Comment = "hook";
            CHECK(Table::TableHasContent(list, structures, {}));
            list.DisasmNotes.clear();
            list.TableComment = "notes";
            CHECK(Table::TableHasContent(list, structures, {}));
            list.TableComment.clear();
            CHECK(Table::TableHasContent(list, structures, { { "player", "game.exe+20" } }));
            Put(list, "only.txt", "x");
            CHECK(Table::TableHasContent(list, structures, {}));
            list.Files.clear();
            Table::MemoryRecord& record = list.AddValue("Scratch", "game.exe+10", Scan::ValueType::FourBytes);
            record.DontSave = true;
            CHECK(!Table::TableHasContent(list, structures, {}));
            record.DontSave = false;
            CHECK(Table::TableHasContent(list, structures, {}));
        }

        void SafeWrites()
        {
            std::error_code ignored;
            const std::filesystem::path folder = Paths::DataFolder() / L"safe-writes";
            std::filesystem::create_directories(folder, ignored);
            const auto content = [](const std::filesystem::path& file) {
                std::ifstream in(file, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            };
            const std::filesystem::path file = folder / L"table.CT";
            std::filesystem::path temporary = file;
            temporary += L".tmp";
            CHECK(Table::WriteFileSafely(file, "first"));
            CHECK(content(file) == "first" && !std::filesystem::exists(temporary, ignored));
            CHECK(Table::WriteFileSafely(file, "second", true) && content(file) == "second");
            CHECK(!Table::WriteFileSafely(file, "third", false));
            CHECK(content(file) == "second" && !std::filesystem::exists(temporary, ignored));
            CHECK(Table::WriteFileSafely(file, "") && content(file).empty());
            CHECK(!Table::WriteFileSafely(folder / L"missing" / L"x.CT", "lost"));
            CHECK(!std::filesystem::exists(folder / L"missing", ignored));

            Table::AddressList list;
            Put(list, "only.txt", "kept");
            const Table::StructureStore structures;
            CHECK(Table::SaveTable(file, list, structures, {}).Ok());
            Table::LoadedTable loaded;
            CHECK(Table::LoadTable(file, loaded).Ok() && loaded.Files.size() == 1 && *loaded.Files[0].Data == Bytes("kept"));
            std::filesystem::remove_all(folder, ignored);
        }

        std::vector<std::uint8_t> Bitmap(int width, int height)
        {
            const int row = (width * 3 + 3) / 4 * 4;
            const std::uint32_t pixels = static_cast<std::uint32_t>(row) * static_cast<std::uint32_t>(height);
            std::vector<std::uint8_t> bytes(54, 0);
            const auto put = [&](std::size_t at, std::uint32_t value, int size) {
                for (int i = 0; i < size; ++i)
                    bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
            };
            bytes[0] = 'B';
            bytes[1] = 'M';
            put(2, 54 + pixels, 4);
            put(10, 54, 4);
            put(14, 40, 4);
            put(18, static_cast<std::uint32_t>(width), 4);
            put(22, static_cast<std::uint32_t>(height), 4);
            put(26, 1, 2);
            put(28, 24, 2);
            put(34, pixels, 4);
            // ∑ Only the pixels of small bitmaps are stored; a large one is just a header that claims its size.
            if (pixels <= 4096)
                bytes.resize(bytes.size() + pixels, 0x7F);
            return bytes;
        }

        void ImageLimits()
        {
            const auto small = DecodeImage(Bitmap(2, 2));
            CHECK(small && small->Width == 2 && small->Height == 2 && small->Rgba.size() == 16);
            CHECK(!DecodeImage(Bitmap(2, 2), 3));
            CHECK(DecodeImage(Bitmap(2, 2), 4).has_value());
            CHECK(!DecodeImage(Bitmap(kMaxImageSide + 1, 1)));
            CHECK(!DecodeImage(Bytes("not an image")));
        }

        void TableIdentity()
        {
            Settings settings;
            Engine engine(settings);
            Table::AddressList& list = engine.Table();
            const std::uint64_t first = list.Identity();
            Put(list, "a.txt", "x");
            list.AddValue("Health", "game.exe+10", Scan::ValueType::FourBytes);
            CHECK_EQ(list.Identity(), first);
            engine.NewTable();
            CHECK(list.Identity() != first && list.Files.empty());
        }

        void LuaCheck(Engine& engine, const char* name, const std::string& code)
        {
            std::string result;
            const Status status = engine.Lua().RunWithResult(code, name, result);
            CHECK_MSG(status.Ok() && result == "ok", std::string(name) + ": " + (status.Ok() ? result : status.Text));
        }

        void LuaEdits()
        {
            Settings settings;
            Engine engine(settings);
            engine.Lua().Run("__manifoldHeadless = true", "headless");
            Table::AddressList& list = engine.Table();
            list.SetClean();
            LuaCheck(engine, "create", "createTableFile('live.txt') return 'ok'");
            Table::TableFile* file = list.FindFile("live.txt");
            CHECK(file && list.Dirty());
            if (!file)
                return;
            const auto version = file->Version;
            const auto data = file->Data;

            list.SetClean();
            std::uint64_t before = *version;
            LuaCheck(engine, "read", "local s = findTableFile('live.txt').Stream if s.Size ~= 0 then return 'size' end s.Position = 0 return 'ok'");
            CHECK(!list.Dirty() && *version == before);

            LuaCheck(engine, "write", "findTableFile('live.txt').Stream.writeString('hello') return 'ok'");
            CHECK(list.Dirty() && *version > before && data->size() == 5 && list.FindFile("live.txt")->Data == data);

            list.SetClean();
            before = *version;
            LuaCheck(engine, "size", "findTableFile('live.txt').Stream.Size = 2 return 'ok'");
            CHECK(list.Dirty() && *version > before && data->size() == 2);

            list.SetClean();
            before = *version;
            LuaCheck(engine, "clear", "findTableFile('live.txt').Stream.clear() return 'ok'");
            CHECK(list.Dirty() && *version > before && data->empty());

            list.SetClean();
            before = *version;
            LuaCheck(engine, "copy", "local copy = findTableFile('live.txt').getData() copy.writeString('own') return 'ok'");
            CHECK(!list.Dirty() && *version == before && data->empty());

            LuaCheck(engine, "plain stream", "local s = createMemoryStream() s.writeString('abc') s.Size = 1 s.clear() return 'ok'");
            CHECK(!list.Dirty());

            LuaCheck(engine, "delete", "findTableFile('live.txt').delete() return 'ok'");
            CHECK(list.Dirty() && !list.FindFile("live.txt"));
        }
    }

    int RunTableFileTests()
    {
        const int before = Totals().Failures;
        Kinds();
        Arrange();
        NamesAndEdits();
        Uses();
        RoundTrip();
        ExportNames();
        DuplicateNames();
        Content();
        SafeWrites();
        ImageLimits();
        TableIdentity();
        LuaEdits();
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("tablefiles", "Table files", 27, [](const SuiteContext&) { return RunTableFileTests(); });
}
