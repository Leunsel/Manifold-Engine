#include "Lua/LuaDocBinding.h"

#include "Core/Errors.h"
#include "Core/Text.h"
#include "Lua/DocAnnotations.h"
#include "Lua/DocText.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaCompatScripts.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kIdsKey = "Manifold.DocIds";
        constexpr const char* kAnchorsKey = "Manifold.DocAnchors";
        constexpr const char* kAnchorMeta = "Manifold.DocAnchor";
        constexpr std::size_t kScriptLimit = 20000;
        constexpr std::size_t kCacheFiles = 64;
        constexpr std::uintmax_t kMaxFileBytes = 4u << 20;

        constexpr const char* kKeywords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or",
            "repeat", "return", "then", "true", "until", "while" };

        constexpr std::pair<const char*, const char*> kLibraries[] = {
            { "string", "String functions; also the methods of every string, as in `(\"x\"):rep(3)`." },
            { "table", "Functions for lists and tables." },
            { "math", "Mathematical functions and the constants `math.pi`, `math.huge`, `math.maxinteger` and `math.mininteger`." },
            { "os", "Time, dates, environment variables and files." },
            { "io", "Files and the default input and output." },
            { "coroutine", "Coroutines: functions that can suspend and continue." },
            { "debug", "Introspection of functions, locals and the stack." },
            { "utf8", "UTF-8 encoding." },
        };

        class AnnotationCache
        {
        public:
            DocPtr Lookup(const std::string& key, int line, const std::function<std::optional<std::string>()>& read, std::filesystem::file_time_type stamp)
            {
                std::lock_guard lock(m_mutex);
                auto found = m_entries.find(key);
                if (found != m_entries.end() && found->second->Stamp != stamp)
                {
                    m_order.erase(found->second);
                    m_entries.erase(found);
                    found = m_entries.end();
                }
                if (found == m_entries.end())
                {
                    std::optional<std::string> text = read();
                    if (!text)
                        return nullptr;
                    Entry entry;
                    entry.Key = key;
                    entry.Stamp = stamp;
                    entry.File = ParseAnnotations(*text, key);
                    m_order.push_front(std::move(entry));
                    found = m_entries.emplace(key, m_order.begin()).first;
                    while (m_order.size() > kCacheFiles)
                    {
                        m_entries.erase(m_order.back().Key);
                        m_order.pop_back();
                    }
                }
                else if (found->second != m_order.begin())
                {
                    m_order.splice(m_order.begin(), m_order, found->second);
                }
                Entry& entry = *found->second;
                if (const auto record = entry.Records.find(line); record != entry.Records.end())
                    return record->second;
                const AnnotatedFunction* function = entry.File.At(line);
                DocPtr record = function && function->Documented ? std::make_shared<const DocRecord>(function->Doc) : nullptr;
                entry.Records.emplace(line, record);
                return record;
            }

        private:
            struct Entry
            {
                std::string Key;
                std::filesystem::file_time_type Stamp{};
                AnnotatedFile File;
                std::unordered_map<int, DocPtr> Records;
            };

            std::mutex m_mutex;
            std::list<Entry> m_order;
            std::unordered_map<std::string, std::list<Entry>::iterator> m_entries;
        };

        AnnotationCache& Cache()
        {
            static AnnotationCache cache;
            return cache;
        }

        std::optional<std::string> ReadFile(const std::filesystem::path& path)
        {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size > kMaxFileBytes)
                return std::nullopt;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::nullopt;
            std::ostringstream text;
            text << file.rdbuf();
            return text.str();
        }

        int IndexValue(lua_State* state)
        {
            lua_settop(state, 2);
            lua_gettable(state, 1);
            return 1;
        }

        void PushWeakTable(lua_State* state, const char* key)
        {
            if (lua_getfield(state, LUA_REGISTRYINDEX, key) == LUA_TTABLE)
                return;
            lua_pop(state, 1);
            lua_newtable(state);
            lua_createtable(state, 0, 1);
            lua_pushstring(state, "k");
            lua_setfield(state, -2, "__mode");
            lua_setmetatable(state, -2);
            lua_pushvalue(state, -1);
            lua_setfield(state, LUA_REGISTRYINDEX, key);
        }

        std::string StringField(lua_State* state, int index, const char* key)
        {
            std::string text;
            if (lua_getfield(state, index, key) == LUA_TSTRING)
                text = lua_tostring(state, -1);
            lua_pop(state, 1);
            return text;
        }

        bool BoolField(lua_State* state, int index, const char* key)
        {
            lua_getfield(state, index, key);
            const bool value = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            return value;
        }

        int MisuseResult(lua_State* state, std::string_view detail)
        {
            return PushFailure(state, ErrorCode::LuaApiMisuse, detail);
        }

        int MissingResult(lua_State* state, std::string_view what)
        {
            return PushFailure(state, ErrorCode::LuaDocMissing, what);
        }

        std::optional<DocItemKind> KindNamed(std::string_view name)
        {
            constexpr std::pair<std::string_view, DocItemKind> kKinds[] = { { "function", DocItemKind::Function }, { "method", DocItemKind::Method },
                { "property", DocItemKind::Property }, { "event", DocItemKind::Event }, { "table", DocItemKind::Table }, { "class", DocItemKind::Class },
                { "constant", DocItemKind::Constant }, { "keyword", DocItemKind::Keyword }, { "field", DocItemKind::Field } };
            for (const auto& [text, kind] : kKinds)
            {
                if (text == name)
                    return kind;
            }
            return std::nullopt;
        }

        std::optional<std::string> ReadStrings(lua_State* state, int index, const char* what, std::vector<std::string>& out)
        {
            index = lua_absindex(state, index);
            if (lua_type(state, index) == LUA_TSTRING)
            {
                out.emplace_back(lua_tostring(state, index));
                return std::nullopt;
            }
            if (!lua_istable(state, index))
                return std::string(what) + " must be a list of strings";
            const lua_Integer count = luaL_len(state, index);
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, index, i);
                if (lua_type(state, -1) != LUA_TSTRING)
                {
                    lua_pop(state, 1);
                    return std::string(what) + "[" + std::to_string(i) + "] must be a string";
                }
                out.emplace_back(lua_tostring(state, -1));
                lua_pop(state, 1);
            }
            return std::nullopt;
        }

        std::optional<std::string> ReadParams(lua_State* state, int index, const std::string& where, DocSignature& signature)
        {
            index = lua_absindex(state, index);
            if (lua_isnil(state, index))
                return std::nullopt;
            if (!lua_istable(state, index))
                return where + " must be a list of parameter tables";
            const lua_Integer count = luaL_len(state, index);
            for (lua_Integer i = 1; i <= count; ++i)
            {
                const std::string at = where + "[" + std::to_string(i) + "]";
                lua_rawgeti(state, index, i);
                const int param = lua_gettop(state);
                if (!lua_istable(state, param))
                {
                    lua_pop(state, 1);
                    return at + " must be a table such as { name = \"address\", type = \"integer\" }";
                }
                DocParam out;
                out.Name = StringField(state, param, "name");
                if (out.Name.empty())
                {
                    lua_pop(state, 1);
                    return at + ".name must be a non-empty string";
                }
                out.Type = StringField(state, param, "type");
                out.Text = StringField(state, param, "doc");
                if (out.Text.empty())
                    out.Text = StringField(state, param, "text");
                out.Default = StringField(state, param, "default");
                out.Optional = BoolField(state, param, "optional");
                out.Variadic = BoolField(state, param, "variadic") || out.Name == "...";
                for (const char* key : { "oneOf", "choices" })
                {
                    if (lua_getfield(state, param, key) != LUA_TNIL)
                    {
                        if (const auto problem = ReadStrings(state, -1, (at + "." + key).c_str(), out.Choices))
                        {
                            lua_pop(state, 2);
                            return problem;
                        }
                    }
                    lua_pop(state, 1);
                }
                if (lua_getfield(state, param, "keys") == LUA_TTABLE)
                {
                    lua_pushnil(state);
                    while (lua_next(state, -2))
                    {
                        if (lua_type(state, -2) == LUA_TSTRING)
                            out.Keys.emplace_back(lua_tostring(state, -2), lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "any");
                        lua_pop(state, 1);
                    }
                    std::sort(out.Keys.begin(), out.Keys.end());
                }
                lua_pop(state, 2);
                if (out.Type.find('"') != std::string::npos || out.Type.find('?') != std::string::npos)
                    ApplyTypeDetails(out);
                signature.Params.push_back(std::move(out));
            }
            return std::nullopt;
        }

        std::optional<std::string> ReadSignature(lua_State* state, int index, const std::string& where, DocSignature& signature)
        {
            lua_getfield(state, index, "params");
            const auto problem = ReadParams(state, -1, where + ".params", signature);
            lua_pop(state, 1);
            if (problem)
                return problem;
            if (BoolField(state, index, "varargs"))
            {
                DocParam rest;
                rest.Name = "...";
                rest.Variadic = true;
                signature.Params.push_back(std::move(rest));
            }
            signature.Returns = StringField(state, index, "returns");
            signature.ReturnsText = StringField(state, index, "returnsDoc");
            return std::nullopt;
        }

        std::optional<std::string> ReadSpec(lua_State* state, int index, DocRecord& record)
        {
            index = lua_absindex(state, index);
            if (!lua_istable(state, index))
                return std::string("the documentation must be a table");
            const std::string kind = StringField(state, index, "kind");
            if (!kind.empty())
            {
                const auto parsed = KindNamed(kind);
                if (!parsed)
                    return "spec.kind \"" + kind + "\" is not one of function, method, property, event, table, class, constant, field";
                record.Kind = *parsed;
            }
            record.Text = StringField(state, index, "doc");
            if (record.Text.empty())
                record.Text = StringField(state, index, "text");
            record.Summary = StringField(state, index, "summary");
            record.Title = StringField(state, index, "title");
            record.Type = StringField(state, index, "type");
            if (lua_getfield(state, index, "value") != LUA_TNIL)
            {
                record.Value = luaL_tolstring(state, -1, nullptr);
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
            record.Hidden = BoolField(state, index, "hidden");
            record.Async = BoolField(state, index, "async");
            const int deprecated = lua_getfield(state, index, "deprecated");
            record.IsDeprecated = deprecated == LUA_TSTRING || (deprecated == LUA_TBOOLEAN && lua_toboolean(state, -1));
            if (deprecated == LUA_TSTRING)
                record.Deprecated = lua_tostring(state, -1);
            lua_pop(state, 1);
            if (lua_getfield(state, index, "see") != LUA_TNIL)
            {
                if (const auto problem = ReadStrings(state, -1, "spec.see", record.See))
                {
                    lua_pop(state, 1);
                    return problem;
                }
            }
            lua_pop(state, 1);
            if (record.Callable())
            {
                DocSignature main;
                if (const auto problem = ReadSignature(state, index, "spec", main))
                    return problem;
                record.Signatures.push_back(std::move(main));
                if (lua_getfield(state, index, "overloads") == LUA_TTABLE)
                {
                    const lua_Integer count = luaL_len(state, -1);
                    for (lua_Integer i = 1; i <= count; ++i)
                    {
                        lua_rawgeti(state, -1, i);
                        if (!lua_istable(state, -1))
                        {
                            lua_pop(state, 2);
                            return "spec.overloads[" + std::to_string(i) + "] must be a table with params and returns";
                        }
                        DocSignature overload;
                        if (const auto problem = ReadSignature(state, lua_gettop(state), "spec.overloads[" + std::to_string(i) + "]", overload))
                        {
                            lua_pop(state, 2);
                            return problem;
                        }
                        record.Signatures.push_back(std::move(overload));
                        lua_pop(state, 1);
                    }
                }
                lua_pop(state, 1);
            }
            return std::nullopt;
        }

        void Finish(DocRegistry& registry, DocRecord& record, std::string_view path)
        {
            record.Path = std::string(path);
            SplitPath(record.Path, record.Owner, record.Name);
            const DocPtr owner = record.Owner.empty() ? nullptr : registry.Find(record.Owner);
            record.OwnerIsClass = owner && owner->Kind == DocItemKind::Class;
        }

        std::string FunctionPlace(lua_State* state, int index)
        {
            lua_pushvalue(state, index);
            lua_Debug info{};
            if (!lua_getinfo(state, ">S", &info))
                return "function";
            return std::string(info.short_src) + ":" + std::to_string(info.linedefined);
        }

        int AnchorGc(lua_State* state)
        {
            auto* path = static_cast<std::string*>(lua_touserdata(state, 1));
            if (!path)
                return 0;
            EngineOf(state).Docs().Remove(*path);
            std::destroy_at(path);
            return 0;
        }

        void Anchor(lua_State* state, int function, const std::string& path)
        {
            function = lua_absindex(state, function);
            PushWeakTable(state, kAnchorsKey);
            lua_pushvalue(state, function);
            void* memory = lua_newuserdatauv(state, sizeof(std::string), 0);
            new (memory) std::string(path);
            if (luaL_newmetatable(state, kAnchorMeta))
            {
                lua_pushcfunction(state, &AnchorGc);
                lua_setfield(state, -2, "__gc");
            }
            lua_setmetatable(state, -2);
            lua_rawset(state, -3);
            lua_pop(state, 1);
        }

        DocPtr Resolve(LuaEngine& engine, lua_State* state, int index, bool reflect)
        {
            index = lua_absindex(state, index);
            if (lua_isfunction(state, index))
                return DocOfValue(engine, state, index, {}, reflect);
            if (lua_type(state, index) != LUA_TSTRING)
                return nullptr;
            const std::string path = lua_tostring(state, index);
            if (DocPtr found = engine.Docs().Find(path))
                return found;
            const int top = lua_gettop(state);
            DocPtr found;
            if (PushChain(state, path))
                found = DocOfValue(engine, state, -1, path, reflect);
            lua_settop(state, top);
            return found;
        }

        std::string TargetText(lua_State* state, int index)
        {
            if (lua_type(state, index) == LUA_TSTRING)
                return "\"" + std::string(lua_tostring(state, index)) + "\"";
            if (lua_isfunction(state, index))
                return "the function at " + FunctionPlace(state, index);
            return luaL_typename(state, index);
        }

        int DocDefine(lua_State* state)
        {
            const std::string path = luaL_checkstring(state, 2);
            DocRecord record;
            if (lua_isnoneornil(state, 3))
            {
                lua_settop(state, 2);
                lua_newtable(state);
            }
            if (const auto problem = ReadSpec(state, 3, record))
                return luaL_error(state, "%s: %s", path.c_str(), problem->c_str());
            DocRegistry& registry = EngineOf(state).Docs();
            Finish(registry, record, path);
            record.Origin = DocOrigin::Manifold;
            registry.Add(std::move(record));
            if (lua_isfunction(state, 1))
                BindDoc(state, 1, path);
            return 0;
        }

        int DocBind(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            BindDoc(state, 1, luaL_checkstring(state, 2));
            return 0;
        }

        int DocMarkdown(lua_State* state)
        {
            const std::string text = RenderMarkdown(EngineOf(state).Docs(), luaL_checkstring(state, 1));
            if (text.empty())
                return MissingResult(state, "\"" + std::string(lua_tostring(state, 1)) + "\"");
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int DocRegister(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            DocRegistry& registry = engine.Docs();
            DocRecord record;
            if (const auto problem = ReadSpec(state, 2, record))
                return MisuseResult(state, "manifold.doc.register: " + *problem);
            std::string path = StringField(state, 2, "path");
            const bool function = lua_isfunction(state, 1);
            bool generated = false;
            if (!function)
                path = lua_tostring(state, 1);
            else if (path.empty())
            {
                static std::uint64_t counter = 0;
                path = "function@" + FunctionPlace(state, 1) + "#" + std::to_string(++counter);
                record.Hidden = true;
                generated = true;
            }
            if (TrimView(path).empty())
                return MisuseResult(state, "manifold.doc.register: the name must not be empty");
            if (registry.Size() >= kScriptLimit && !registry.Find(path))
                return MisuseResult(state, "manifold.doc.register: more than " + std::to_string(kScriptLimit) + " names are documented; a loop registers new names");
            Finish(registry, record, path);
            record.Origin = DocOrigin::Script;
            if (function)
            {
                lua_pushvalue(state, 1);
                lua_Debug info{};
                if (lua_getinfo(state, ">S", &info) && info.source && info.source[0] == '@')
                {
                    record.Source = info.source + 1;
                    record.Line = info.linedefined;
                }
            }
            const DocPtr stored = registry.Add(std::move(record));
            if (stored->Origin != DocOrigin::Script)
                return MisuseResult(state, "manifold.doc.register: " + path + " is documented by " + DocOriginName(stored->Origin) + " and cannot be replaced by a script");
            if (function)
            {
                BindDoc(state, 1, path);
                if (generated)
                    Anchor(state, 1, path);
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int DocRegisterModule(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            DocRegistry& registry = engine.Docs();
            const std::string path = luaL_checkstring(state, 1);
            luaL_checktype(state, 2, LUA_TTABLE);
            if (TrimView(path).empty())
                return MisuseResult(state, "manifold.doc.registerModule: the path must not be empty");
            DocRecord module;
            module.Kind = DocItemKind::Table;
            module.Origin = DocOrigin::Script;
            if (lua_istable(state, 3))
                module.Text = StringField(state, 3, "doc");
            Finish(registry, module, path);
            const DocPtr existing = registry.Find(path);
            if (existing && existing->Origin > DocOrigin::Script)
                return MisuseResult(state, "manifold.doc.registerModule: " + path + " is documented by " + DocOriginName(existing->Origin));
            if (module.Text.empty() && existing)
                module.Text = existing->Text;
            registry.Add(std::move(module));
            std::vector<std::string> names;
            const auto collect = [&](int table)
            {
                lua_pushnil(state);
                while (lua_next(state, table))
                {
                    if (lua_type(state, -2) == LUA_TSTRING && lua_isfunction(state, -1))
                    {
                        std::string name = lua_tostring(state, -2);
                        if (!name.empty() && name.front() != '_' && std::find(names.begin(), names.end(), name) == names.end())
                            names.push_back(std::move(name));
                    }
                    lua_pop(state, 1);
                }
            };
            collect(2);
            const int base = lua_gettop(state);
            lua_pushvalue(state, 2);
            for (int depth = 0; depth < 4; ++depth)
            {
                const int current = lua_gettop(state);
                if (!lua_getmetatable(state, current))
                    break;
                if (lua_getfield(state, -1, "__index") != LUA_TTABLE)
                    break;
                collect(lua_gettop(state));
                lua_replace(state, current);
                lua_settop(state, current);
            }
            lua_settop(state, base);
            std::sort(names.begin(), names.end());
            int count = 0;
            for (const std::string& name : names)
            {
                lua_getfield(state, 2, name.c_str());
                const int function = lua_gettop(state);
                const std::string memberPath = path + "." + name;
                DocPtr found = DocOfValue(engine, state, function, {}, true);
                if (found && registry.Size() < kScriptLimit)
                {
                    DocRecord copy = *found;
                    Finish(registry, copy, memberPath);
                    if (copy.Kind != DocItemKind::Method)
                        copy.Kind = DocItemKind::Function;
                    copy.Origin = found->Origin == DocOrigin::Reflection ? DocOrigin::Reflection : DocOrigin::Annotation;
                    copy.Hidden = found->Hidden && found->Path.rfind("function@", 0) != 0;
                    const DocPtr stored = registry.Add(std::move(copy));
                    if (stored->Origin <= DocOrigin::Script)
                    {
                        BindDoc(state, function, memberPath);
                        ++count;
                    }
                }
                lua_settop(state, function - 1);
            }
            lua_pushinteger(state, count);
            return 1;
        }

        int DocInherit(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            DocRegistry& registry = engine.Docs();
            luaL_checktype(state, 1, LUA_TFUNCTION);
            const DocPtr source = Resolve(engine, state, 2, true);
            if (!source)
                return MissingResult(state, TargetText(state, 2));
            if (registry.Find(source->Path) == source)
            {
                BindDoc(state, 1, source->Path);
                lua_pushboolean(state, 1);
                return 1;
            }
            static std::uint64_t counter = 0;
            DocRecord copy = *source;
            const std::string name = copy.Name;
            const std::string path = "function@" + FunctionPlace(state, 1) + "#" + std::to_string(++counter);
            Finish(registry, copy, path);
            copy.Name = name.empty() ? copy.Name : name;
            copy.Hidden = true;
            copy.Origin = std::min(source->Origin, DocOrigin::Script);
            registry.Add(std::move(copy));
            BindDoc(state, 1, path);
            Anchor(state, 1, path);
            lua_pushboolean(state, 1);
            return 1;
        }

        int DocUnregister(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            DocRegistry& registry = engine.Docs();
            std::string path;
            if (lua_isfunction(state, 1))
                path = BoundDocPath(state, 1);
            else
                path = luaL_checkstring(state, 1);
            const DocPtr found = path.empty() ? nullptr : registry.Find(path);
            if (!found)
                return MissingResult(state, TargetText(state, 1));
            if (found->Origin == DocOrigin::Manifold || found->Origin == DocOrigin::CheatEngine || found->Origin == DocOrigin::Standard || !registry.Remove(path))
                return MisuseResult(state, "manifold.doc.unregister: " + found->Path + " is documented by " + DocOriginName(found->Origin) + "; only documentation that a script registered can be removed");
            if (lua_isfunction(state, 1))
            {
                PushWeakTable(state, kIdsKey);
                lua_pushvalue(state, 1);
                lua_pushnil(state);
                lua_rawset(state, -3);
                lua_pop(state, 1);
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int DocEntry(lua_State* state)
        {
            const DocPtr found = Resolve(EngineOf(state), state, 1, true);
            if (!found)
                return MissingResult(state, TargetText(state, 1));
            PushDocEntry(state, *found);
            return 1;
        }

        int DocFind(lua_State* state)
        {
            const std::string query = luaL_checkstring(state, 1);
            const lua_Integer limit = luaL_optinteger(state, 2, 50);
            const std::string kind = luaL_optstring(state, 3, "");
            std::optional<DocItemKind> wanted;
            if (!kind.empty())
            {
                wanted = KindNamed(kind);
                if (!wanted)
                    return MisuseResult(state, "manifold.doc.find: options.kind \"" + kind + "\" is not one of function, method, property, event, table, class, constant, field");
            }
            const std::vector<DocPtr> found = EngineOf(state).Docs().Search(query, wanted ? 100000 : static_cast<std::size_t>(std::max<lua_Integer>(limit, 0)));
            lua_newtable(state);
            lua_Integer index = 0;
            for (const DocPtr& record : found)
            {
                if (wanted && record->Kind != *wanted)
                    continue;
                if (index >= limit)
                    break;
                lua_createtable(state, 0, 3);
                lua_pushstring(state, record->Path.c_str());
                lua_setfield(state, -2, "path");
                lua_pushstring(state, DocKindName(record->Kind));
                lua_setfield(state, -2, "kind");
                lua_pushstring(state, record->Summary.c_str());
                lua_setfield(state, -2, "summary");
                lua_rawseti(state, -2, ++index);
            }
            return 1;
        }

        int DocAnnotationsOf(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            const DocPtr found = AnnotatedDoc(EngineOf(state), state, 1);
            if (!found)
                return MissingResult(state, TargetText(state, 1));
            PushDocEntry(state, *found);
            return 1;
        }

        void PushParam(lua_State* state, const DocParam& param)
        {
            lua_createtable(state, 0, 6);
            lua_pushstring(state, param.Name.c_str());
            lua_setfield(state, -2, "name");
            if (!param.Type.empty())
            {
                lua_pushstring(state, param.Type.c_str());
                lua_setfield(state, -2, "type");
            }
            if (!param.Text.empty())
            {
                lua_pushstring(state, param.Text.c_str());
                lua_setfield(state, -2, "text");
            }
            if (!param.Default.empty())
            {
                lua_pushstring(state, param.Default.c_str());
                lua_setfield(state, -2, "default");
            }
            lua_pushboolean(state, param.Optional);
            lua_setfield(state, -2, "optional");
            lua_pushboolean(state, param.Variadic);
            lua_setfield(state, -2, "variadic");
            if (!param.Choices.empty())
            {
                lua_createtable(state, static_cast<int>(param.Choices.size()), 0);
                for (std::size_t i = 0; i < param.Choices.size(); ++i)
                {
                    lua_pushstring(state, param.Choices[i].c_str());
                    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                }
                lua_setfield(state, -2, "choices");
            }
            if (!param.Keys.empty())
            {
                lua_createtable(state, 0, static_cast<int>(param.Keys.size()));
                for (const auto& [key, type] : param.Keys)
                {
                    lua_pushstring(state, type.c_str());
                    lua_setfield(state, -2, key.c_str());
                }
                lua_setfield(state, -2, "keys");
            }
        }

        void SetString(lua_State* state, const char* key, const std::string& value)
        {
            if (value.empty())
                return;
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }
    }

    std::shared_ptr<const DocRegistry> BuiltinDocs()
    {
        static const std::shared_ptr<const DocRegistry> builtin = []
            {
                auto registry = std::make_shared<DocRegistry>();
                AddCheatEngineDocs(*registry);
                for (const auto& [name, text] : kLibraries)
                {
                    DocRecord library;
                    library.Path = name;
                    library.Kind = DocItemKind::Table;
                    library.Origin = DocOrigin::Standard;
                    library.Text = text;
                    registry->Add(std::move(library));
                }
                for (const CompatScript& script : DocScripts())
                {
                    const std::string text = script.Text();
                    AnnotatedFile file = ParseAnnotations(text, script.Name);
                    for (AnnotatedFunction& function : file.Functions)
                    {
                        DocRecord record = std::move(function.Doc);
                        record.Origin = DocOrigin::Standard;
                        record.Source.clear();
                        record.Line = 0;
                        registry->Add(std::move(record));
                    }
                }
                for (const char* keyword : kKeywords)
                {
                    DocRecord record;
                    record.Path = keyword;
                    record.Kind = DocItemKind::Keyword;
                    record.Origin = DocOrigin::Standard;
                    record.Hidden = true;
                    registry->Add(std::move(record));
                }
                return std::shared_ptr<const DocRegistry>(std::move(registry));
            }();
        return builtin;
    }

    void PushIndexed(lua_State* state, int object, std::string_view key)
    {
        object = lua_absindex(state, object);
        lua_pushcfunction(state, &IndexValue);
        lua_pushvalue(state, object);
        lua_pushlstring(state, key.data(), key.size());
        if (lua_pcall(state, 2, 1, 0) != LUA_OK)
        {
            lua_pop(state, 1);
            lua_pushnil(state);
        }
    }

    bool PushChain(lua_State* state, std::string_view chain)
    {
        lua_pushglobaltable(state);
        std::size_t start = 0;
        while (start <= chain.size())
        {
            const std::size_t dot = chain.find('.', start);
            const std::string_view part = TrimView(chain.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
            if (part.empty() || lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                lua_pushnil(state);
                return false;
            }
            PushIndexed(state, -1, part);
            lua_remove(state, -2);
            if (dot == std::string_view::npos)
                break;
            start = dot + 1;
        }
        return !lua_isnil(state, -1);
    }

    std::string ClassNameOf(lua_State* state, int index)
    {
        if (!lua_istable(state, index) && !lua_isuserdata(state, index))
            return {};
        PushIndexed(state, index, "ClassName");
        std::string name = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "";
        lua_pop(state, 1);
        return name;
    }

    void BindDoc(lua_State* state, int index, std::string_view path)
    {
        index = lua_absindex(state, index);
        PushWeakTable(state, kIdsKey);
        lua_pushvalue(state, index);
        lua_pushlstring(state, path.data(), path.size());
        lua_rawset(state, -3);
        lua_pop(state, 1);
    }

    std::string BoundDocPath(lua_State* state, int index)
    {
        index = lua_absindex(state, index);
        std::string path;
        if (lua_getfield(state, LUA_REGISTRYINDEX, kIdsKey) == LUA_TTABLE)
        {
            lua_pushvalue(state, index);
            if (lua_rawget(state, -2) == LUA_TSTRING)
                path = lua_tostring(state, -1);
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
        return path;
    }

    DocPtr AnnotatedDoc(LuaEngine& engine, lua_State* state, int index)
    {
        if (!lua_isfunction(state, index) || lua_iscfunction(state, index))
            return nullptr;
        lua_pushvalue(state, index);
        lua_Debug info{};
        if (!lua_getinfo(state, ">S", &info) || !info.source || info.linedefined <= 0)
            return nullptr;
        const std::string_view source = info.source;
        if (source.empty())
            return nullptr;
        if (source.front() == '@')
        {
            const std::filesystem::path path(Utf8ToWide(source.substr(1)));
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(path, error);
            if (error)
                return nullptr;
            return Cache().Lookup(std::string(source.substr(1)), info.linedefined, [&]() { return ReadFile(path); }, stamp);
        }
        if (source.front() == '=')
        {
            const std::string text = engine.ChunkText(source.substr(1));
            if (text.empty())
                return nullptr;
            return Cache().Lookup("=" + std::string(source.substr(1)) + "#" + std::to_string(std::hash<std::string>{}(text)), info.linedefined, [&]() { return std::optional<std::string>(text); }, {});
        }
        const std::string key = "#" + std::to_string(std::hash<std::string_view>{}(source)) + ":" + std::to_string(source.size());
        return Cache().Lookup(key, info.linedefined, [&]() { return std::optional<std::string>(std::string(source)); }, {});
    }

    DocPtr ReflectedDoc(lua_State* state, int index, std::string_view path)
    {
        if (!lua_isfunction(state, index) || lua_iscfunction(state, index))
            return nullptr;
        index = lua_absindex(state, index);
        lua_pushvalue(state, index);
        lua_Debug info{};
        if (!lua_getinfo(state, ">Su", &info))
            return nullptr;
        auto record = std::make_shared<DocRecord>();
        record->Path = std::string(path);
        SplitPath(record->Path, record->Owner, record->Name);
        record->Kind = DocItemKind::Function;
        record->Origin = DocOrigin::Reflection;
        if (info.source && info.source[0] == '@')
            record->Source = info.source + 1;
        record->Line = info.linedefined;
        DocSignature signature;
        for (int i = 1; i <= info.nparams; ++i)
        {
            lua_pushvalue(state, index);
            const char* name = lua_getlocal(state, nullptr, i);
            lua_pop(state, 1);
            DocParam param;
            param.Name = name ? name : "?";
            signature.Params.push_back(std::move(param));
        }
        if (info.isvararg)
        {
            DocParam rest;
            rest.Name = "...";
            rest.Variadic = true;
            signature.Params.push_back(std::move(rest));
        }
        record->Signatures.push_back(std::move(signature));
        return record;
    }

    DocPtr DocOfValue(LuaEngine& engine, lua_State* state, int index, std::string_view path, bool reflect)
    {
        index = lua_absindex(state, index);
        DocRegistry& registry = engine.Docs();
        if (lua_isfunction(state, index))
        {
            const std::string bound = BoundDocPath(state, index);
            if (!bound.empty())
            {
                if (DocPtr found = registry.Find(bound))
                    return found;
            }
            if (DocPtr annotated = AnnotatedDoc(engine, state, index))
            {
                if (path.empty() || annotated->Path == path)
                    return annotated;
                auto renamed = std::make_shared<DocRecord>(*annotated);
                const DocItemKind kind = renamed->Kind;
                Finish(registry, *renamed, path);
                renamed->Kind = kind;
                return renamed;
            }
        }
        if (!path.empty())
        {
            if (DocPtr found = registry.Find(path))
                return found;
        }
        return reflect ? ReflectedDoc(state, index, path) : nullptr;
    }

    void PushDocEntry(lua_State* state, const DocRecord& record)
    {
        lua_createtable(state, 0, 16);
        SetString(state, "path", record.Path);
        SetString(state, "name", record.Name);
        SetString(state, "owner", record.Owner);
        lua_pushstring(state, DocKindName(record.Kind));
        lua_setfield(state, -2, "kind");
        lua_pushstring(state, DocOriginName(record.Origin));
        lua_setfield(state, -2, "origin");
        lua_pushstring(state, record.Summary.c_str());
        lua_setfield(state, -2, "summary");
        lua_pushstring(state, record.Text.c_str());
        lua_setfield(state, -2, "text");
        SetString(state, "title", record.Title);
        SetString(state, "type", record.Type);
        SetString(state, "value", record.Value);
        SetString(state, "parent", record.Parent);
        SetString(state, "source", record.Source);
        if (record.Line > 0)
        {
            lua_pushinteger(state, record.Line);
            lua_setfield(state, -2, "line");
        }
        if (record.IsDeprecated)
        {
            if (record.Deprecated.empty())
                lua_pushboolean(state, 1);
            else
                lua_pushstring(state, record.Deprecated.c_str());
            lua_setfield(state, -2, "deprecated");
        }
        lua_pushboolean(state, record.Hidden);
        lua_setfield(state, -2, "hidden");
        if (!record.Choices.empty())
        {
            lua_createtable(state, static_cast<int>(record.Choices.size()), 0);
            for (std::size_t i = 0; i < record.Choices.size(); ++i)
            {
                lua_pushstring(state, record.Choices[i].c_str());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "choices");
        }
        if (!record.See.empty())
        {
            lua_createtable(state, static_cast<int>(record.See.size()), 0);
            for (std::size_t i = 0; i < record.See.size(); ++i)
            {
                lua_pushstring(state, record.See[i].c_str());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "see");
        }
        if (record.Callable())
        {
            lua_createtable(state, static_cast<int>(record.Signatures.size()), 0);
            for (std::size_t s = 0; s < record.Signatures.size(); ++s)
            {
                const DocSignature& signature = record.Signatures[s];
                lua_createtable(state, 0, 3);
                lua_createtable(state, static_cast<int>(signature.Params.size()), 0);
                for (std::size_t p = 0; p < signature.Params.size(); ++p)
                {
                    PushParam(state, signature.Params[p]);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(p + 1));
                }
                lua_setfield(state, -2, "params");
                SetString(state, "returns", signature.Returns);
                SetString(state, "returnsText", signature.ReturnsText);
                const std::string call = FormatCall(record, s, false);
                lua_pushstring(state, call.c_str());
                lua_setfield(state, -2, "text");
                lua_rawseti(state, -2, static_cast<lua_Integer>(s + 1));
            }
            lua_setfield(state, -2, "signatures");
        }
    }

    void RegisterDocNatives(lua_State* state, int natives)
    {
        natives = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "docDefine", &DocDefine },
            { "docBind", &DocBind },
            { "docMarkdown", &DocMarkdown },
            { "docRegister", &DocRegister },
            { "docRegisterModule", &DocRegisterModule },
            { "docInherit", &DocInherit },
            { "docUnregister", &DocUnregister },
            { "docEntry", &DocEntry },
            { "docFind", &DocFind },
            { "docAnnotations", &DocAnnotationsOf },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, natives);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
