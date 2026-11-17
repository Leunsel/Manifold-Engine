#include "Lua/DocRegistry.h"

#include "Core/Text.h"
#include "Lua/DocText.h"
#include "Lua/LuaDocs.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace Manifold::Lua
{
    namespace
    {
        std::atomic<std::uint32_t> g_nextId{ 1 };

        bool Prefer(const DocPtr& candidate, const DocPtr& current)
        {
            if (!current)
                return true;
            if (!candidate)
                return false;
            return candidate->Origin >= current->Origin;
        }

        int Score(const DocRecord& record, std::string_view query)
        {
            const std::string name = Lower(record.Name);
            const std::string path = Lower(record.Path);
            if (name == query || path == query)
                return 0;
            if (name.compare(0, query.size(), query) == 0)
                return 1;
            if (name.find(query) != std::string::npos)
                return 2;
            if (path.find(query) != std::string::npos)
                return 3;
            if (Lower(record.Summary).find(query) != std::string::npos)
                return 4;
            return -1;
        }

        std::string CleanParent(std::string_view parent)
        {
            std::string text = Trim(parent);
            for (const std::string_view prefix : { std::string_view("Inheritence:"), std::string_view("Inheritance:") })
            {
                if (StartsWithNoCase(text, prefix))
                    text = Trim(std::string_view(text).substr(prefix.size()));
            }
            return text;
        }
    }

    std::string DocKey(std::string_view path)
    {
        std::string key = Lower(TrimView(path));
        std::replace(key.begin(), key.end(), ':', '.');
        return key;
    }

    std::string ExactKey(std::string_view path)
    {
        std::string key(TrimView(path));
        std::replace(key.begin(), key.end(), ':', '.');
        return key;
    }

    DocRegistry::DocRegistry(std::shared_ptr<const DocRegistry> base) : m_base(std::move(base)) {}

    DocPtr DocRegistry::Add(DocRecord record)
    {
        if (record.Name.empty() && !record.Path.empty())
            SplitPath(record.Path, record.Owner, record.Name);
        if (record.Path.empty())
            record.Path = record.Owner.empty() ? record.Name : record.Owner + "." + record.Name;
        if (record.Summary.empty())
            record.Summary = FirstSentence(record.Text);
        const std::string key = ExactKey(record.Path);
        std::unique_lock lock(m_mutex);
        const auto existing = m_byPath.find(key);
        if (existing != m_byPath.end())
        {
            if (existing->second->Origin > record.Origin)
                return existing->second;
            record.Order = existing->second->Order;
            m_byId.erase(existing->second->Id);
            if (DocKey(existing->second->Owner) != DocKey(record.Owner))
            {
                auto& list = m_byOwner[DocKey(existing->second->Owner)];
                list.erase(std::remove(list.begin(), list.end(), key), list.end());
                m_byOwner[DocKey(record.Owner)].push_back(key);
            }
        }
        else
        {
            record.Order = ++m_order;
            m_byOwner[DocKey(record.Owner)].push_back(key);
            m_byLower[DocKey(key)].push_back(key);
        }
        record.Id = g_nextId.fetch_add(1);
        auto stored = std::make_shared<const DocRecord>(std::move(record));
        m_byPath[key] = stored;
        m_byId[stored->Id] = stored;
        m_generation.fetch_add(1);
        return stored;
    }

    bool DocRegistry::RemoveKey(const std::string& key)
    {
        const auto found = m_byPath.find(key);
        if (found == m_byPath.end())
            return false;
        auto& list = m_byOwner[DocKey(found->second->Owner)];
        list.erase(std::remove(list.begin(), list.end(), key), list.end());
        auto& lower = m_byLower[DocKey(key)];
        lower.erase(std::remove(lower.begin(), lower.end(), key), lower.end());
        m_byId.erase(found->second->Id);
        m_byPath.erase(found);
        m_generation.fetch_add(1);
        return true;
    }

    bool DocRegistry::Remove(std::string_view path)
    {
        std::unique_lock lock(m_mutex);
        if (RemoveKey(ExactKey(path)))
            return true;
        const auto loose = m_byLower.find(DocKey(path));
        if (loose == m_byLower.end() || loose->second.size() != 1)
            return false;
        return RemoveKey(std::string(loose->second.front()));
    }

    bool DocRegistry::RemoveId(std::uint32_t id)
    {
        std::unique_lock lock(m_mutex);
        const auto found = m_byId.find(id);
        if (found == m_byId.end())
            return false;
        return RemoveKey(ExactKey(found->second->Path));
    }

    void DocRegistry::AddClassAlias(std::string_view alias, std::string_view target)
    {
        std::unique_lock lock(m_mutex);
        m_aliases[DocKey(alias)] = std::string(target);
        m_generation.fetch_add(1);
    }

    std::string DocRegistry::AliasOf(const std::string& key) const
    {
        {
            std::shared_lock lock(m_mutex);
            if (const auto found = m_aliases.find(key); found != m_aliases.end())
                return found->second;
        }
        return m_base ? m_base->AliasOf(key) : std::string();
    }

    DocPtr DocRegistry::Get(std::uint32_t id) const
    {
        {
            std::shared_lock lock(m_mutex);
            if (const auto found = m_byId.find(id); found != m_byId.end())
                return found->second;
        }
        return m_base ? m_base->Get(id) : nullptr;
    }

    DocPtr DocRegistry::FindIn(std::string_view path, bool exact) const
    {
        DocPtr local;
        {
            std::shared_lock lock(m_mutex);
            if (exact)
            {
                if (const auto found = m_byPath.find(ExactKey(path)); found != m_byPath.end())
                    local = found->second;
            }
            else if (const auto loose = m_byLower.find(DocKey(path)); loose != m_byLower.end() && !loose->second.empty())
            {
                if (const auto found = m_byPath.find(loose->second.front()); found != m_byPath.end())
                    local = found->second;
            }
        }
        const DocPtr base = m_base ? m_base->FindIn(path, exact) : nullptr;
        return Prefer(local, base) ? local : base;
    }

    DocPtr DocRegistry::Find(std::string_view path) const
    {
        if (DocPtr exact = FindIn(path, true))
            return exact;
        return FindIn(path, false);
    }

    std::vector<DocPtr> DocRegistry::Members(std::string_view owner) const
    {
        std::vector<DocPtr> result = m_base ? m_base->Members(owner) : std::vector<DocPtr>{};
        std::unordered_map<std::string, std::size_t> position;
        for (std::size_t i = 0; i < result.size(); ++i)
            position[result[i]->Name] = i;
        std::shared_lock lock(m_mutex);
        const auto found = m_byOwner.find(DocKey(owner));
        if (found == m_byOwner.end())
            return result;
        for (const std::string& key : found->second)
        {
            const auto record = m_byPath.find(key);
            if (record == m_byPath.end())
                continue;
            const std::string& name = record->second->Name;
            if (const auto at = position.find(name); at != position.end())
            {
                if (Prefer(record->second, result[at->second]))
                    result[at->second] = record->second;
                continue;
            }
            position[name] = result.size();
            result.push_back(record->second);
        }
        return result;
    }

    std::vector<DocPtr> DocRegistry::ClassMembers(std::string_view className) const
    {
        std::vector<DocPtr> result;
        std::set<std::string> seen;
        std::string current = ResolveClass(className);
        for (int guard = 0; !current.empty() && guard < 16; ++guard)
        {
            for (DocPtr& member : Members(current))
            {
                if (seen.insert(DocKey(member->Name)).second)
                    result.push_back(std::move(member));
            }
            current = ParentOf(current);
        }
        return result;
    }

    std::string DocRegistry::ResolveClass(std::string_view name) const
    {
        const auto classAt = [&](std::string_view candidate) -> std::string
        {
            const DocPtr record = Find(candidate);
            return record && record->Kind == DocItemKind::Class ? record->Path : std::string();
        };
        if (name.empty())
            return {};
        if (std::string found = classAt(name); !found.empty())
            return found;
        if (name.size() > 1 && (name[0] == 'T' || name[0] == 't'))
        {
            if (std::string found = classAt(name.substr(1)); !found.empty())
                return found;
        }
        const std::string alias = AliasOf(DocKey(name));
        return alias.empty() ? std::string() : classAt(alias);
    }

    std::string DocRegistry::ParentOf(std::string_view className) const
    {
        const DocPtr record = Find(className);
        if (!record || record->Kind != DocItemKind::Class || record->Parent.empty())
            return {};
        return ResolveClass(record->Parent);
    }

    std::vector<DocPtr> DocRegistry::All() const
    {
        std::vector<DocPtr> result = m_base ? m_base->All() : std::vector<DocPtr>{};
        std::unordered_map<std::string, std::size_t> position;
        for (std::size_t i = 0; i < result.size(); ++i)
            position[ExactKey(result[i]->Path)] = i;
        std::vector<DocPtr> local;
        {
            std::shared_lock lock(m_mutex);
            local.reserve(m_byPath.size());
            for (const auto& [key, record] : m_byPath)
                local.push_back(record);
        }
        std::sort(local.begin(), local.end(), [](const DocPtr& a, const DocPtr& b) { return a->Order < b->Order; });
        for (DocPtr& record : local)
        {
            const std::string key = ExactKey(record->Path);
            if (const auto at = position.find(key); at != position.end())
            {
                if (Prefer(record, result[at->second]))
                    result[at->second] = std::move(record);
                continue;
            }
            position[key] = result.size();
            result.push_back(std::move(record));
        }
        return result;
    }

    std::vector<DocPtr> DocRegistry::Search(std::string_view query, std::size_t limit) const
    {
        const std::string lowered = Lower(TrimView(query));
        std::vector<std::pair<int, DocPtr>> scored;
        for (DocPtr& record : All())
        {
            if (record->Hidden)
                continue;
            const int score = lowered.empty() ? 5 : Score(*record, lowered);
            if (score >= 0)
                scored.emplace_back(score, std::move(record));
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b)
            {
                if (a.first != b.first)
                    return a.first < b.first;
                if (a.second->Origin != b.second->Origin)
                    return a.second->Origin > b.second->Origin;
                return a.second->Path < b.second->Path;
            });
        std::vector<DocPtr> result;
        for (auto& [score, record] : scored)
        {
            if (result.size() >= limit)
                break;
            result.push_back(std::move(record));
        }
        return result;
    }

    std::uint64_t DocRegistry::Generation() const
    {
        return m_generation.load() + (m_base ? m_base->Generation() : 0);
    }

    std::size_t DocRegistry::Size() const
    {
        std::shared_lock lock(m_mutex);
        return m_byPath.size();
    }

    std::string MarkdownCell(std::string_view text)
    {
        std::string cell;
        bool space = false;
        for (const char c : text)
        {
            if (c == '\n' || c == '\r' || c == '\t')
            {
                space = !cell.empty();
                continue;
            }
            if (space && c != ' ')
                cell += ' ';
            space = false;
            if (c == '|')
                cell += '\\';
            cell += c;
        }
        while (!cell.empty() && cell.back() == ' ')
            cell.pop_back();
        return cell;
    }

    namespace
    {
        void CallRows(std::vector<std::string>& out, const DocRecord& record, const std::string& shown)
        {
            if (record.Signatures.empty())
            {
                out.push_back("| `" + shown + "()` | | " + MarkdownCell(record.Text) + " |");
                return;
            }
            for (const DocSignature& signature : record.Signatures)
                out.push_back("| `" + shown + "(" + FormatParams(signature) + ")` | " + MarkdownCell(signature.Returns) + " | " + MarkdownCell(record.Text) + " |");
        }

        void ValueRow(std::vector<std::string>& out, const DocRecord& record, const std::string& value, const std::string& shown)
        {
            out.push_back("| `" + shown + "` | " + MarkdownCell(value) + " | " + MarkdownCell(record.Text) + " |");
        }

        std::string InstanceName(const DocRecord& type)
        {
            std::string name = type.Name;
            if (!name.empty() && name[0] >= 'A' && name[0] <= 'Z')
                name[0] = static_cast<char>(name[0] - 'A' + 'a');
            return name;
        }

        void Section(std::vector<std::string>& out, const DocRegistry& registry, const DocRecord& table, int depth)
        {
            if (depth > 8)
                return;
            if (!out.empty())
                out.emplace_back();
            out.push_back("## " + table.Path);
            out.emplace_back();
            if (!table.Text.empty())
            {
                out.push_back(table.Text);
                out.emplace_back();
            }
            std::vector<DocPtr> constants;
            std::vector<DocPtr> calls;
            std::vector<DocPtr> values;
            std::vector<DocPtr> tables;
            std::vector<DocPtr> classes;
            for (DocPtr& member : registry.Members(table.Path))
            {
                if (member->Hidden)
                    continue;
                switch (member->Kind)
                {
                case DocItemKind::Constant: constants.push_back(std::move(member)); break;
                case DocItemKind::Function:
                case DocItemKind::Method: calls.push_back(std::move(member)); break;
                case DocItemKind::Table: tables.push_back(std::move(member)); break;
                case DocItemKind::Class: classes.push_back(std::move(member)); break;
                default: values.push_back(std::move(member)); break;
                }
            }
            if (!constants.empty() || !values.empty())
            {
                out.push_back("| Name | Value | |");
                out.push_back("| --- | --- | --- |");
                for (const DocPtr& record : constants)
                    ValueRow(out, *record, record->Value, record->Path);
                for (const DocPtr& record : values)
                    ValueRow(out, *record, record->Type, record->Path);
                for (const DocPtr& record : calls)
                    CallRows(out, *record, record->Path);
            }
            else if (!calls.empty())
            {
                out.push_back("| Function | Returns | |");
                out.push_back("| --- | --- | --- |");
                for (const DocPtr& record : calls)
                    CallRows(out, *record, record->Path);
            }
            for (const DocPtr& type : classes)
            {
                out.emplace_back();
                out.push_back("### " + (type->Title.empty() ? type->Name : type->Title));
                out.emplace_back();
                if (!type->Text.empty())
                {
                    out.push_back(type->Text);
                    out.emplace_back();
                }
                out.push_back("| Name | Returns | |");
                out.push_back("| --- | --- | --- |");
                const std::string instance = InstanceName(*type);
                for (const DocPtr& member : registry.Members(type->Path))
                {
                    if (member->Hidden)
                        continue;
                    if (member->Callable())
                        CallRows(out, *member, instance + "." + member->Name);
                    else
                        ValueRow(out, *member, member->Type, instance + "." + member->Name);
                }
            }
            for (const DocPtr& child : tables)
                Section(out, registry, *child, depth + 1);
        }
    }

    std::string RenderMarkdown(const DocRegistry& registry, std::string_view prefix)
    {
        const DocPtr root = registry.Find(prefix);
        if (!root)
            return {};
        std::vector<std::string> out;
        Section(out, registry, *root, 0);
        std::string text;
        for (const std::string& line : out)
            text += line + "\n";
        return text;
    }

    void AddCheatEngineDocs(DocRegistry& registry)
    {
        for (const DocClass& entry : DocClasses())
        {
            DocRecord record;
            record.Path = entry.Name;
            record.Name = entry.Name;
            record.Kind = DocItemKind::Class;
            record.Origin = DocOrigin::CheatEngine;
            record.Parent = CleanParent(entry.Parent);
            registry.Add(std::move(record));
        }
        std::map<std::string, DocRecord> pending;
        std::vector<std::string> order;
        for (const DocEntry& entry : DocEntries())
        {
            std::string name = entry.Name;
            bool indexed = false;
            if (name.size() > 2 && name.compare(name.size() - 2, 2, "[]") == 0)
            {
                indexed = true;
                name.resize(name.size() - 2);
            }
            const std::string owner = entry.Owner;
            const std::string path = owner.empty() ? name : owner + "." + name;
            const std::string key = DocKey(path);
            auto found = pending.find(key);
            if (found == pending.end())
            {
                DocRecord record;
                record.Path = path;
                record.Owner = owner;
                record.Name = name;
                record.OwnerIsClass = !owner.empty();
                record.Origin = DocOrigin::CheatEngine;
                record.Indexed = indexed;
                if (entry.Kind == DocKind::Property)
                {
                    CeProperty property = ParseCeProperty(entry.Text);
                    record.Kind = name.rfind("On", 0) == 0 && property.Type.rfind("function", 0) == 0 ? DocItemKind::Event : DocItemKind::Property;
                    record.Type = std::move(property.Type);
                    record.Text = std::move(property.Text);
                    record.Choices = std::move(property.Choices);
                }
                else
                {
                    record.Kind = entry.Kind == DocKind::Method ? DocItemKind::Method : DocItemKind::Function;
                    record.Text = entry.Text;
                }
                found = pending.emplace(key, std::move(record)).first;
                order.push_back(key);
            }
            DocRecord& record = found->second;
            if (record.Callable())
            {
                DocSignature signature = ParseLooseParams(entry.Params);
                const std::string params = FormatParams(signature);
                const bool known = std::any_of(record.Signatures.begin(), record.Signatures.end(), [&](const DocSignature& other) { return FormatParams(other) == params; });
                if (!known)
                    record.Signatures.push_back(std::move(signature));
                if (record.Text.empty() && entry.Text[0] != '\0' && std::string_view(entry.Text) != "^ but adds a custom title")
                    record.Text = entry.Text;
            }
        }
        for (const std::string& key : order)
            registry.Add(std::move(pending[key]));
        constexpr std::pair<const char*, const char*> kAliases[] = { { "TMainForm", "Form" }, { "TfrmLuaEngine", "Form" }, { "TMemoryBrowser", "Form" },
            { "TCustomForm", "Form" }, { "TPortableNetworkGraphic", "Graphic" }, { "TJpegImage", "Graphic" } };
        for (const auto& [alias, target] : kAliases)
            registry.AddClassAlias(alias, target);
    }
}
