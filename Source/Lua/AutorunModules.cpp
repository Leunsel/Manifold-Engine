#include "Lua/AutorunModules.h"

#include "Core/Ini.h"
#include "Core/Text.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#include <windows.h>

#include <bcrypt.h>

namespace Manifold::Autorun
{
    namespace
    {
        constexpr std::size_t kHeadBytes = 16384;

        std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            std::ostringstream content;
            content << file.rdbuf();
            return content.str();
        }

        bool IsLua(const std::filesystem::path& path)
        {
            return EqualsNoCase(WideToUtf8(path.extension().wstring()), ".lua");
        }

        std::string_view HeaderBody(std::string_view text, std::string& joined)
        {
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
                text.remove_prefix(3);
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            text.remove_prefix(first);
            if (text.rfind("--[", 0) == 0)
            {
                std::size_t level = 3;
                while (level < text.size() && text[level] == '=')
                    ++level;
                if (level < text.size() && text[level] == '[')
                {
                    const std::string close = "]" + std::string(level - 3, '=') + "]";
                    const std::size_t end = text.find(close, level + 1);
                    return text.substr(level + 1, end == std::string_view::npos ? std::string_view::npos : end - level - 1);
                }
            }
            std::size_t start = 0;
            while (start < text.size())
            {
                std::size_t end = text.find('\n', start);
                if (end == std::string_view::npos)
                    end = text.size();
                const std::string_view line = TrimView(text.substr(start, end - start));
                if (line.rfind("--", 0) != 0)
                    break;
                joined += std::string(line.substr(line.find_first_not_of('-') == std::string_view::npos ? line.size() : line.find_first_not_of('-')));
                joined += '\n';
                start = end + 1;
            }
            return joined;
        }

        bool HeaderKey(std::string_view key)
        {
            if (key.size() < 2 || key[0] < 'A' || key[0] > 'Z')
                return false;
            return std::all_of(key.begin(), key.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; });
        }

        bool ParseRequires(std::string_view value, std::vector<Requirement>& out, std::string& problem)
        {
            std::size_t start = 0;
            while (start <= value.size())
            {
                std::size_t end = value.find(',', start);
                if (end == std::string_view::npos)
                    end = value.size();
                const std::string_view item = TrimView(value.substr(start, end - start));
                start = end + 1;
                if (item.empty())
                {
                    if (end == value.size() && out.empty() && TrimView(value).empty())
                        return true;
                    problem = "REQUIRES has an empty entry";
                    return false;
                }
                Requirement requirement;
                const std::size_t op = item.find_first_of("<>=~!");
                if (op == std::string_view::npos)
                {
                    requirement.Name = std::string(item);
                }
                else
                {
                    if (item.substr(op, 2) != ">=")
                    {
                        problem = "REQUIRES understands only >= in \"" + std::string(item) + "\"";
                        return false;
                    }
                    requirement.Name = Trim(item.substr(0, op));
                    requirement.MinVersion = Trim(item.substr(op + 2));
                    if (requirement.MinVersion.empty() || requirement.MinVersion.find_first_of("0123456789") != 0)
                    {
                        problem = "REQUIRES has no version after >= in \"" + std::string(item) + "\"";
                        return false;
                    }
                }
                if (requirement.Name.empty())
                {
                    problem = "REQUIRES has no name in \"" + std::string(item) + "\"";
                    return false;
                }
                out.push_back(std::move(requirement));
            }
            return true;
        }

        std::string RelativeText(const std::filesystem::path& path, const std::filesystem::path& base)
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::relative(path, base, error);
            return WideToUtf8((error || relative.empty() ? path : relative).wstring());
        }

        Module Make(Shape shape, const std::filesystem::path& entry, const std::filesystem::path& root, std::string category, const std::filesystem::path& folder)
        {
            Module module;
            module.Shape = shape;
            module.Entry = entry;
            module.Root = root;
            module.Category = std::move(category);
            module.Name = WideToUtf8(shape == Shape::Folder ? root.filename().wstring() : entry.stem().wstring());
            module.Where = RelativeText(root, folder);
            Refresh(module);
            return module;
        }

        void AddLoose(std::vector<Module>& modules, const std::filesystem::path& from, const std::filesystem::path& folder, const char* category)
        {
            std::error_code error;
            std::vector<std::filesystem::path> scripts;
            for (const auto& entry : std::filesystem::directory_iterator(from, error))
            {
                if (entry.is_regular_file() && IsLua(entry.path()))
                    scripts.push_back(entry.path());
            }
            std::sort(scripts.begin(), scripts.end());
            for (const auto& script : scripts)
                modules.push_back(Make(Shape::Loose, script, script, category, folder));
        }

        void AddModules(std::vector<Module>& modules, const std::filesystem::path& from, const std::filesystem::path& folder, const std::string& category, bool descend)
        {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(from, error))
            {
                const std::filesystem::path& path = entry.path();
                if (entry.is_regular_file() && IsLua(path))
                {
                    modules.push_back(Make(Shape::File, path, path, category, folder));
                }
                else if (entry.is_directory())
                {
                    std::error_code ignored;
                    const std::filesystem::path init = path / L"init.lua";
                    if (std::filesystem::is_regular_file(init, ignored))
                        modules.push_back(Make(Shape::Folder, init, path, category, folder));
                    else if (descend)
                        AddModules(modules, path, folder, WideToUtf8(path.filename().wstring()), false);
                }
            }
        }

        std::string StateWord(State state)
        {
            switch (state)
            {
            case State::Disabled: return "is disabled";
            case State::Held: return "is held";
            case State::Failed: return "failed";
            case State::Blocked: return "is blocked";
            default: return "is not loaded";
            }
        }
    }

    std::string KeyOf(std::string_view name)
    {
        return Lower(TrimView(name));
    }

    // ∑ The name becomes the folder autorun\data\<Name>: "..", "." or a name Windows trims at its ends would point at another folder.
    bool ValidName(std::string_view name)
    {
        if (name.empty() || name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ')
            return false;
        return std::none_of(name.begin(), name.end(), [](char c)
            {
                const unsigned char byte = static_cast<unsigned char>(c);
                return byte < 0x20 || byte == 0x7F || std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos;
            });
    }

    const char* BuiltInReplacement(std::string_view head)
    {
        head = head.substr(0, kHeadBytes);
        if (head.find("__MANIFOLD_UE_RUNTIME__") != std::string_view::npos)
            return "Unreal Engine support is built into the program now (Extensions > Unreal Engine, manifold.unreal)";
        if (head.find("__MANIFOLD_RTTI_RUNTIME__") != std::string_view::npos)
            return "C++ run-time type information is built into the program now (Extensions > C++ RTTI, manifold.rtti; ManifoldRTTI still works)";
        return nullptr;
    }

    Header ParseHeader(std::string_view text)
    {
        Header header;
        std::string joined;
        const std::string_view body = HeaderBody(text.substr(0, kHeadBytes), joined);
        bool sawRequires = false;
        bool sawApi = false;
        std::size_t start = 0;
        while (start < body.size())
        {
            std::size_t end = body.find('\n', start);
            if (end == std::string_view::npos)
                end = body.size();
            const std::string_view line = body.substr(start, end - start);
            start = end + 1;
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos)
                continue;
            const std::string_view key = TrimView(line.substr(0, colon));
            const std::string value = Trim(line.substr(colon + 1));
            if (!HeaderKey(key) || value.empty())
                continue;
            if (key == "NAME")
            {
                if (header.Name.empty() && ValidName(value))
                    header.Name = value;
            }
            else if (key == "VERSION")
            {
                if (header.Version.empty())
                    header.Version = value;
            }
            else if (key == "AUTHOR")
            {
                if (header.Author.empty())
                    header.Author = value;
            }
            else if (key == "DESCRIPTION")
            {
                if (header.Description.empty())
                    header.Description = value;
            }
            else if (key == "REQUIRES")
            {
                if (!sawRequires)
                {
                    sawRequires = true;
                    std::string problem;
                    if (!ParseRequires(value, header.Requires, problem))
                    {
                        header.Requires.clear();
                        header.Problem = problem;
                    }
                }
            }
            else if (key == "API")
            {
                if (!sawApi)
                {
                    sawApi = true;
                    char* stop = nullptr;
                    const long api = std::strtol(value.c_str(), &stop, 10);
                    if (stop == value.c_str() || *stop != '\0' || api < 0)
                        header.Problem = "API must be a number, not \"" + value + "\"";
                    else
                        header.Api = static_cast<int>(api);
                }
            }
            else if (std::none_of(header.Other.begin(), header.Other.end(), [&](const auto& other) { return other.first == key; }))
            {
                header.Other.emplace_back(std::string(key), value);
            }
        }
        return header;
    }

    int CompareVersions(std::string_view left, std::string_view right)
    {
        // ∑ Digit runs compare as text without leading zeros (longer is larger), so a run of any length cannot overflow.
        const auto next = [](std::string_view& text) -> std::string_view
        {
            std::size_t digits = 0;
            while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
                ++digits;
            std::string_view run = text.substr(0, digits);
            while (!run.empty() && run.front() == '0')
                run.remove_prefix(1);
            const std::size_t dot = text.find('.', digits);
            text = dot == std::string_view::npos || digits == 0 ? std::string_view() : text.substr(dot + 1);
            return run;
        };
        while (!left.empty() || !right.empty())
        {
            const std::string_view a = next(left);
            const std::string_view b = next(right);
            if (a.size() != b.size())
                return a.size() < b.size() ? -1 : 1;
            if (const int order = a.compare(b); order != 0)
                return order < 0 ? -1 : 1;
        }
        return 0;
    }

    std::string Sha256Hex(std::string_view bytes)
    {
        unsigned char digest[32] = {};
        if (::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) != 0)
            return {};
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (const unsigned char byte : digest)
        {
            text += kDigits[byte >> 4];
            text += kDigits[byte & 15];
        }
        return text;
    }

    std::string HashModule(const Module& module)
    {
        if (module.Shape != Shape::Folder)
            return Sha256Hex(ReadFile(module.Entry));
        std::vector<std::pair<std::string, std::string>> files;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(module.Root, error), end; !error && it != end; it.increment(error))
        {
            if (!it->is_regular_file() || !IsLua(it->path()))
                continue;
            std::string relative = Lower(RelativeText(it->path(), module.Root));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            files.emplace_back(std::move(relative), Sha256Hex(ReadFile(it->path())));
        }
        std::sort(files.begin(), files.end());
        std::string manifest;
        for (const auto& [name, hash] : files)
            manifest += name + "\n" + hash + "\n";
        return Sha256Hex(manifest);
    }

    void Refresh(Module& module)
    {
        const std::string text = ReadFile(module.Entry);
        module.Header = ParseHeader(text);
        if (!module.Header.Name.empty())
            module.Name = module.Header.Name;
        module.Key = KeyOf(module.Name);
        module.Hash = HashModule(module);
        if (const char* builtIn = BuiltInReplacement(text))
        {
            module.State = State::BuiltIn;
            module.Reason = builtIn;
        }
    }

    std::vector<Module> Scan(const std::filesystem::path& folder)
    {
        std::vector<Module> modules;
        std::error_code error;
        AddLoose(modules, folder, folder, "");
        if (std::filesystem::is_directory(folder / L"custom", error))
            AddLoose(modules, folder / L"custom", folder, "custom");
        const std::size_t loose = modules.size();
        if (std::filesystem::is_directory(folder / L"modules", error))
            AddModules(modules, folder / L"modules", folder, "", true);
        std::stable_sort(modules.begin() + static_cast<std::ptrdiff_t>(loose), modules.end(), [](const Module& a, const Module& b) { return a.Key < b.Key; });

        std::map<std::string, std::size_t> owners;
        for (std::size_t i = loose; i < modules.size(); ++i)
        {
            Module& module = modules[i];
            if (module.State == State::BuiltIn)
                continue;
            const auto [found, added] = owners.emplace(module.Key, i);
            if (!added)
            {
                module.State = State::Failed;
                module.Duplicate = true;
                module.Reason = "Its name is taken by " + modules[found->second].Where + "; give one of them another NAME";
            }
        }
        std::map<std::string, std::size_t> looseOwners;
        for (std::size_t i = 0; i < loose; ++i)
        {
            Module& module = modules[i];
            if (module.State == State::BuiltIn)
                continue;
            if (const auto owner = owners.find(module.Key); owner != owners.end())
            {
                module.State = State::Superseded;
                module.Reason = "Superseded by " + modules[owner->second].Where + ". The file stays where it is and can be deleted";
                continue;
            }
            const auto [found, added] = looseOwners.emplace(module.Key, i);
            if (!added)
            {
                module.State = State::Failed;
                module.Duplicate = true;
                module.Reason = "Its name is taken by " + modules[found->second].Where + "; give one of them another NAME";
            }
        }
        return modules;
    }

    Module* Find(std::vector<Module>& modules, std::string_view name)
    {
        const std::string key = KeyOf(name);
        Module* fallback = nullptr;
        for (Module& module : modules)
        {
            if (module.Key != key)
                continue;
            if (module.State != State::Superseded && !module.Duplicate)
                return &module;
            if (!fallback)
                fallback = &module;
        }
        return fallback;
    }

    const Module* Find(const std::vector<Module>& modules, std::string_view name)
    {
        return Find(const_cast<std::vector<Module>&>(modules), name);
    }

    void ApplyShipped(Module& module, std::span<const ShippedModule> shipped)
    {
        module.Origin = Origin::User;
        module.Unmodified = false;
        for (const ShippedModule& candidate : shipped)
        {
            if (candidate.Key != module.Key)
                continue;
            module.Origin = Origin::Shipped;
            if (candidate.Hash == module.Hash)
                module.Unmodified = true;
        }
    }

    void ApplyState(Module& module, StateStore& store, const ApplyOptions& options)
    {
        ApplyShipped(module, options.Shipped);
        if (module.State == State::Superseded || module.State == State::BuiltIn || module.Duplicate)
            return;
        module.Changed = false;
        module.New = false;
        Stored* stored = store.Find(module.Key);
        if (!stored)
        {
            if (store.Existed() && options.HoldNew && !module.Unmodified)
            {
                module.New = true;
                module.State = State::Held;
                module.Reason = "It is new in the autorun folder and runs once it is allowed";
                return;
            }
            stored = &store.Put(module.Key, module.Name);
            stored->Hash = module.Hash;
            stored->FirstSeen = Today();
        }
        stored->Name = module.Name;
        module.FirstSeen = stored->FirstSeen;
        module.Enabled = stored->Enabled;
        if (!stored->Enabled)
        {
            module.State = State::Disabled;
            module.Reason.clear();
            return;
        }
        if (!stored->Allowed)
        {
            if (module.Unmodified)
            {
                stored->Allowed = true;
                store.Touch();
            }
            else
            {
                module.State = State::Held;
                module.Reason = "A script wrote to its files; it runs again once it is allowed";
                return;
            }
        }
        if (stored->Hash != module.Hash)
        {
            module.Changed = !module.Unmodified && !stored->Hash.empty();
            stored->Hash = module.Hash;
            store.Touch();
        }
    }

    void ApplyState(std::vector<Module>& modules, StateStore& store, const ApplyOptions& options)
    {
        for (Module& module : modules)
            ApplyState(module, store, options);
    }

    bool Satisfied(const std::vector<Module>& modules, const Module& module, std::string& reason)
    {
        for (const Requirement& requirement : module.Header.Requires)
        {
            const Module* dependency = Find(modules, requirement.Name);
            const std::string wanted = requirement.MinVersion.empty() ? requirement.Name : requirement.Name + " >= " + requirement.MinVersion;
            if (!dependency || dependency->State == State::Superseded)
            {
                reason = "it requires " + wanted + ", which is not in the autorun folder";
                return false;
            }
            if (dependency->State == State::BuiltIn)
                continue;
            if (dependency->State != State::Loaded && dependency->State != State::Pending)
            {
                reason = "it requires " + requirement.Name + ", which " + StateWord(dependency->State);
                return false;
            }
            if (!requirement.MinVersion.empty())
            {
                if (dependency->Header.Version.empty())
                {
                    reason = "it requires " + wanted + ", and " + dependency->Name + " names no version";
                    return false;
                }
                if (CompareVersions(dependency->Header.Version, requirement.MinVersion) < 0)
                {
                    reason = "it requires " + wanted + ", found " + dependency->Header.Version;
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<std::size_t> Order(std::vector<Module>& modules, int apiVersion)
    {
        std::vector<std::size_t> order;
        std::vector<int> mark(modules.size(), 0);
        std::vector<std::size_t> stack;
        const auto indexOf = [&](const Module* module) { return static_cast<std::size_t>(module - modules.data()); };
        const auto visit = [&](auto&& self, std::size_t index) -> void
        {
            if (mark[index] == 2)
                return;
            if (mark[index] == 1)
            {
                const auto from = std::find(stack.begin(), stack.end(), index);
                std::string cycle;
                for (auto it = from; it != stack.end(); ++it)
                    cycle += modules[*it].Name + " -> ";
                cycle += modules[index].Name;
                for (auto it = from; it != stack.end(); ++it)
                {
                    Module& member = modules[*it];
                    if (member.State == State::Pending)
                    {
                        member.State = State::Blocked;
                        member.Reason = "its requirements form a circle: " + cycle;
                    }
                }
                return;
            }
            mark[index] = 1;
            stack.push_back(index);
            for (const Requirement& requirement : modules[index].Header.Requires)
            {
                if (const Module* dependency = Find(modules, requirement.Name))
                    self(self, indexOf(dependency));
            }
            stack.pop_back();
            mark[index] = 2;
            order.push_back(index);
        };
        for (std::size_t i = 0; i < modules.size(); ++i)
            visit(visit, i);
        for (const std::size_t index : order)
        {
            Module& module = modules[index];
            if (module.State != State::Pending)
                continue;
            std::string reason;
            if (!module.Header.Problem.empty())
                reason = "its header is not readable: " + module.Header.Problem;
            else if (module.Header.Api > apiVersion)
                reason = "it needs the manifold API " + std::to_string(module.Header.Api) + ", and this program has " + std::to_string(apiVersion);
            else
                Satisfied(modules, module, reason);
            if (!reason.empty())
            {
                module.State = State::Blocked;
                module.Reason = reason;
            }
        }
        return order;
    }

    const char* StateText(const Module& module)
    {
        switch (module.State)
        {
        case State::Pending: return "Not loaded";
        case State::Loaded: return module.Changed ? "Loaded, changed" : "Loaded";
        case State::Disabled: return module.ActiveUntilRestart ? "Disabled, active until restart" : "Disabled";
        case State::Failed: return "Failed";
        case State::Blocked: return "Blocked";
        case State::Held: return module.New ? "New, held" : "Held";
        case State::Superseded: return "Superseded";
        default: return "Built in";
        }
    }

    std::string Today()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char text[16] = "";
        std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
        return text;
    }

    bool StateStore::Load(const std::filesystem::path& file)
    {
        m_file = file;
        m_entries.clear();
        m_existed = false;
        m_dirty = false;
        if (file.empty())
            return false;
        const std::optional<std::string> text = Ini::ReadText(file);
        if (!text)
            return false;
        m_existed = true;
        for (const Ini::Section& section : Ini::Parse(*text))
        {
            const std::string key = KeyOf(section.Name);
            if (key.empty())
                continue;
            Stored& stored = m_entries[key];
            stored.Name = Trim(section.Name);
            for (const Ini::Entry& entry : section.Entries)
            {
                const std::string name = Trim(entry.Key);
                const std::string value = Trim(entry.Value);
                if (name == "Enabled")
                    stored.Enabled = value != "0";
                else if (name == "Allowed")
                    stored.Allowed = value != "0";
                else if (name == "Hash")
                    stored.Hash = value;
                else if (name == "FirstSeen")
                    stored.FirstSeen = value;
            }
        }
        return true;
    }

    std::string StateStore::Text() const
    {
        std::string text = "; Autorun modules of Manifold Engine: what is switched off, what was allowed to run and what each module's code looked like.\n";
        for (const auto& [key, stored] : m_entries)
        {
            text += "\n[" + stored.Name + "]\n";
            text += std::string("Enabled=") + (stored.Enabled ? "1" : "0") + "\n";
            text += std::string("Allowed=") + (stored.Allowed ? "1" : "0") + "\n";
            text += "Hash=" + stored.Hash + "\n";
            text += "FirstSeen=" + stored.FirstSeen + "\n";
        }
        return text;
    }

    bool StateStore::Save()
    {
        if (m_file.empty())
        {
            m_dirty = false;
            return true;
        }
        if (!m_dirty && m_existed)
            return true;
        if (!Ini::WriteAtomic(m_file, Text()))
            return false;
        m_existed = true;
        m_dirty = false;
        return true;
    }

    Stored* StateStore::Find(std::string_view key)
    {
        const auto found = m_entries.find(std::string(key));
        return found == m_entries.end() ? nullptr : &found->second;
    }

    Stored& StateStore::Put(std::string_view key, std::string_view name)
    {
        Stored& stored = m_entries[std::string(key)];
        stored.Name = std::string(name);
        m_dirty = true;
        return stored;
    }
}
