#include "TestKit.h"

#include "Core/Text.h"
#include "Core/Xml.h"
#include "Table/CheatTable.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Manifold::Tests
{
    namespace
    {
        using namespace Manifold::Table;

        // ∑ What a table says, element by element: the path of element names from the root, and for each element its
        //   attributes and (for a leaf) its text. A round trip that keeps the table keeps every one of these; one that
        //   loses data shows it as a path or an attribute that occurs fewer times after the save, or a value that changed.
        struct Census
        {
            std::map<std::string, int> Elements;
            std::map<std::string, int> Attributes;
            std::map<std::string, int> Values;
        };

        std::string Normal(std::string text)
        {
            text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
            return std::string(TrimView(text));
        }

        std::string Collapse(std::string path)
        {
            static constexpr std::pair<std::string_view, std::string_view> kRepeats[] = {
                { "/CheatEntries/CheatEntry/CheatEntries/CheatEntry", "/CheatEntries/CheatEntry" },
                { "/Elements/Element/Structure/Elements/Element", "/Elements/Element" },
            };
            for (const auto& [repeat, once] : kRepeats)
            {
                for (std::size_t at; (at = path.find(repeat)) != std::string::npos;)
                    path.replace(at, repeat.size(), once);
            }
            return path;
        }

        void Count(const Xml::Node& node, const std::string& parent, Census& census)
        {
            const std::string path = Collapse(parent.empty() ? node.Name : parent + "/" + node.Name);
            ++census.Elements[path];
            for (const auto& [name, value] : node.Attributes)
            {
                ++census.Attributes[path + "@" + name];
                ++census.Values[path + "@" + name + "=" + Normal(value)];
            }
            if (node.Children.empty())
                ++census.Values[path + "=" + Normal(node.Text)];
            for (const auto& child : node.Children)
                Count(*child, path, census);
        }

        std::optional<Census> CensusOf(const std::string& text)
        {
            const std::string_view body = text.rfind("\xEF\xBB\xBF", 0) == 0 ? std::string_view(text).substr(3) : std::string_view(text);
            const Xml::ParseResult parsed = Xml::Parse(body);
            if (!parsed.Root)
                return std::nullopt;
            Census census;
            Count(*parsed.Root, {}, census);
            return census;
        }

        std::string Resave(LoadedTable& loaded)
        {
            AddressList list;
            list.TableLuaScript = loaded.LuaScript;
            list.TableComment = loaded.Comments;
            list.DisasmNotes = std::move(loaded.DisasmNotes);
            list.Files = std::move(loaded.Files);
            list.ExtraXml = std::move(loaded.ExtraXml);
            list.LoadedVersion = loaded.Version;
            list.LoadedFileNaming = loaded.Naming;
            const std::function<void(const MemoryRecord&)> noteIds = [&](const MemoryRecord& record)
                {
                    list.NoteId(record.Id);
                    for (const auto& child : record.Children)
                        noteIds(*child);
                };
            for (const auto& record : loaded.Records)
                noteIds(*record);
            for (auto& record : loaded.Records)
                list.Add(std::move(record));
            StructureStore structures;
            for (auto& structure : loaded.Structures)
            {
                if (!structures.Find(structure->Name))
                    structures.Items.push_back(std::move(structure));
            }
            return WriteTable(list, structures, loaded.Symbols);
        }

        struct Loss
        {
            int Count = 0;
            int Tables = 0;
            std::string Example;
        };

        void Add(std::map<std::string, Loss>& into, const std::string& key, int count, const std::string& table)
        {
            Loss& loss = into[key];
            loss.Count += count;
            ++loss.Tables;
            if (loss.Example.empty())
                loss.Example = table;
        }

        std::string KeyOf(const std::string& value)
        {
            const std::size_t equals = value.find('=');
            return equals == std::string::npos ? value : value.substr(0, equals);
        }

        void Print(const char* title, const std::map<std::string, Loss>& losses)
        {
            std::printf("\n%s: %zu\n", title, losses.size());
            std::vector<std::pair<std::string, Loss>> sorted(losses.begin(), losses.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.Tables != b.second.Tables ? a.second.Tables > b.second.Tables : a.first < b.first; });
            for (const auto& [key, loss] : sorted)
                std::printf("  %6d in %3d tables  %s   (e.g. %s)\n", loss.Count, loss.Tables, key.c_str(), loss.Example.c_str());
        }

        // Loads each table, writes it the way Save would, and lists what the written text no longer has. The tables are
        // only read; with --out=<folder> the written text of each goes into that folder, never next to the table.
        int RunTableRoundTrip(const std::vector<std::string>& arguments)
        {
            std::vector<std::filesystem::path> tables;
            std::filesystem::path out;
            for (std::size_t i = 1; i < arguments.size(); ++i)
            {
                if (arguments[i].rfind("--out=", 0) == 0)
                {
                    out = Utf8ToWide(arguments[i].substr(6));
                    continue;
                }
                const std::filesystem::path path = Utf8ToWide(arguments[i]);
                std::error_code error;
                if (std::filesystem::is_directory(path, error))
                {
                    for (auto it = std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, error);
                         it != std::filesystem::recursive_directory_iterator(); it.increment(error))
                    {
                        if (error)
                            break;
                        if (it->is_regular_file(error) && Lower(WideToUtf8(it->path().extension().wstring())) == ".ct")
                            tables.push_back(it->path());
                    }
                }
                else
                    tables.push_back(path);
            }
            if (tables.empty())
            {
                std::printf("usage: --table-roundtrip [--out=<folder>] <table.CT or folder> ...\n");
                return 2;
            }
            std::map<std::string, Loss> elements, attributes, values, contents;
            int read = 0, unreadable = 0, clean = 0;
            for (const std::filesystem::path& path : tables)
            {
                const std::string name = WideToUtf8(path.filename().wstring());
                std::ifstream file(path, std::ios::binary);
                std::stringstream content;
                content << file.rdbuf();
                const std::string original = content.str();
                LoadedTable loaded;
                const std::optional<Census> before = CensusOf(original);
                if (!before || !ParseTable(original, loaded))
                {
                    ++unreadable;
                    std::printf("not read: %s\n", name.c_str());
                    continue;
                }
                ++read;
                std::map<std::string, std::vector<std::uint8_t>> files;
                for (const TableFile& file : loaded.Files)
                    files.emplace(file.Name, *file.Data);
                const std::string written = Resave(loaded);
                if (!out.empty())
                {
                    std::error_code ignored;
                    std::filesystem::create_directories(out, ignored);
                    std::ofstream(out / (std::to_wstring(read) + L"-" + path.filename().wstring()), std::ios::binary) << written;
                }
                const std::optional<Census> after = CensusOf(written);
                if (!after)
                {
                    std::printf("written text does not parse: %s\n", name.c_str());
                    continue;
                }
                bool lost = false;
                const auto compare = [&](const std::map<std::string, int>& was, const std::map<std::string, int>& now, std::map<std::string, Loss>& into, bool keyOnly)
                    {
                        std::map<std::string, int> missing;
                        for (const auto& [key, count] : was)
                        {
                            const auto it = now.find(key);
                            const int left = it == now.end() ? 0 : it->second;
                            if (left < count)
                                missing[keyOnly ? KeyOf(key) : key] += count - left;
                        }
                        for (const auto& [key, count] : missing)
                        {
                            Add(into, key, count, name);
                            lost = true;
                        }
                    };
                compare(before->Elements, after->Elements, elements, false);
                compare(before->Attributes, after->Attributes, attributes, false);
                compare(before->Values, after->Values, values, true);
                LoadedTable reloaded;
                if (ParseTable(written, reloaded))
                {
                    for (const TableFile& file : reloaded.Files)
                    {
                        const auto it = files.find(file.Name);
                        if (it == files.end() || it->second != *file.Data)
                            Add(contents, "CheatTable/Files/" + file.Name, 1, name);
                        if (it != files.end())
                            files.erase(it);
                    }
                    for (const auto& [file, data] : files)
                        Add(contents, "CheatTable/Files/" + file + " (missing)", 1, name);
                }
                else
                    Add(contents, "the written table does not load again", 1, name);
                if (!lost)
                    ++clean;
            }
            std::printf("%d tables read, %d not read, %d without any loss\n", read, unreadable, clean);
            Print("Elements that occur fewer times after the save (path, count lost)", elements);
            Print("Attributes that occur fewer times after the save", attributes);
            Print("Values that changed or went missing (path of the value)", values);
            Print("Table files whose decoded bytes differ after loading the written table again", contents);
            return 0;
        }
    }

    MANIFOLD_MODE("--table-roundtrip", &RunTableRoundTrip);
}
