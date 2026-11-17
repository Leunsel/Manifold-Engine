#include "Lua/LuaEngine.h"

#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaDocs.h"

#include "lua.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>

namespace Manifold::Lua
{
    namespace
    {
        struct StandardEntry
        {
            const char* Owner;
            const char* Name;
            const char* Params;
            const char* Text;
        };

        constexpr StandardEntry kStandard[] = {
            { "", "print", "...", "Writes the values to the Lua output" },
            { "", "pairs", "table", "Iterates over all keys and values of a table" },
            { "", "ipairs", "table", "Iterates over the array part of a table" },
            { "", "next", "table, key OPTIONAL", "Returns the next key and value of a table" },
            { "", "type", "value", "Returns the type name of the value" },
            { "", "tostring", "value", "Converts the value to a string" },
            { "", "tonumber", "value, base OPTIONAL", "Converts the value to a number, or nil" },
            { "", "select", "index, ...", "Returns the arguments after index, or their count for \"#\"" },
            { "", "pcall", "function, ...", "Calls the function in protected mode and returns whether it succeeded" },
            { "", "xpcall", "function, handler, ...", "Like pcall, with a message handler" },
            { "", "error", "message, level OPTIONAL", "Raises an error" },
            { "", "assert", "value, message OPTIONAL", "Raises an error when the value is false or nil" },
            { "", "setmetatable", "table, metatable", "Sets the metatable of a table" },
            { "", "getmetatable", "value", "Returns the metatable of a value" },
            { "", "rawget", "table, key", "Reads a field without metamethods" },
            { "", "rawset", "table, key, value", "Writes a field without metamethods" },
            { "", "rawequal", "a, b", "Compares without metamethods" },
            { "", "rawlen", "value", "Length without metamethods" },
            { "", "require", "name", "Loads a module" },
            { "", "load", "chunk, name OPTIONAL, mode OPTIONAL, env OPTIONAL", "Compiles a chunk and returns it as a function" },
            { "", "loadfile", "filename OPTIONAL, mode OPTIONAL, env OPTIONAL", "Compiles a file and returns it as a function" },
            { "", "dofile", "filename OPTIONAL", "Runs a file" },
            { "", "collectgarbage", "option OPTIONAL, ...", "Controls the garbage collector" },
            { "string", "format", "format, ...", "Formats the values like printf" },
            { "string", "sub", "s, i, j OPTIONAL", "Returns the part from i to j" },
            { "string", "find", "s, pattern, init OPTIONAL, plain OPTIONAL", "Finds the first match of a pattern" },
            { "string", "match", "s, pattern, init OPTIONAL", "Returns the captures of the first match" },
            { "string", "gmatch", "s, pattern, init OPTIONAL", "Iterates over all matches" },
            { "string", "gsub", "s, pattern, replacement, n OPTIONAL", "Replaces the matches of a pattern" },
            { "string", "rep", "s, n, separator OPTIONAL", "Repeats a string" },
            { "string", "byte", "s, i OPTIONAL, j OPTIONAL", "Returns the character codes" },
            { "string", "char", "...", "Builds a string from character codes" },
            { "string", "upper", "s", "Converts to upper case" },
            { "string", "lower", "s", "Converts to lower case" },
            { "string", "len", "s", "Returns the length" },
            { "string", "reverse", "s", "Reverses a string" },
            { "string", "pack", "format, ...", "Packs the values into a binary string" },
            { "string", "unpack", "format, s, position OPTIONAL", "Unpacks values from a binary string" },
            { "string", "packsize", "format", "Size of a packed format" },
            { "table", "insert", "table, position OPTIONAL, value", "Inserts a value" },
            { "table", "remove", "table, position OPTIONAL", "Removes a value and returns it" },
            { "table", "concat", "table, separator OPTIONAL, i OPTIONAL, j OPTIONAL", "Joins the values into a string" },
            { "table", "sort", "table, compare OPTIONAL", "Sorts the table in place" },
            { "table", "unpack", "table, i OPTIONAL, j OPTIONAL", "Returns the values as separate results" },
            { "table", "pack", "...", "Returns the arguments as a table with the field n" },
            { "table", "move", "a1, f, e, t, a2 OPTIONAL", "Moves elements between tables" },
            { "math", "floor", "x", "Largest integer not above x" },
            { "math", "ceil", "x", "Smallest integer not below x" },
            { "math", "abs", "x", "Absolute value" },
            { "math", "max", "x, ...", "Largest value" },
            { "math", "min", "x, ...", "Smallest value" },
            { "math", "random", "m OPTIONAL, n OPTIONAL", "Random number" },
            { "math", "randomseed", "x OPTIONAL, y OPTIONAL", "Seeds the random generator" },
            { "math", "sqrt", "x", "Square root" },
            { "math", "fmod", "x, y", "Remainder of x / y" },
            { "math", "modf", "x", "Integral and fractional part" },
            { "math", "tointeger", "x", "Converts to an integer, or nil" },
            { "math", "type", "x", "\"integer\", \"float\" or nil" },
            { "math", "exp", "x", "e to the power of x" },
            { "math", "log", "x, base OPTIONAL", "Logarithm" },
            { "math", "sin", "x", "Sine" },
            { "math", "cos", "x", "Cosine" },
            { "math", "tan", "x", "Tangent" },
            { "math", "atan", "y, x OPTIONAL", "Arc tangent" },
            { "os", "time", "table OPTIONAL", "Current time, or the time described by the table" },
            { "os", "date", "format OPTIONAL, time OPTIONAL", "Formats a date" },
            { "os", "clock", "", "CPU time in seconds" },
            { "os", "difftime", "t2, t1", "Seconds between two times" },
            { "os", "getenv", "name", "Value of an environment variable" },
            { "os", "execute", "command OPTIONAL", "Runs a shell command" },
            { "os", "remove", "filename", "Deletes a file" },
            { "os", "rename", "old, new", "Renames a file" },
            { "io", "open", "filename, mode OPTIONAL", "Opens a file" },
            { "io", "read", "...", "Reads from the default input" },
            { "io", "write", "...", "Writes to the default output" },
            { "io", "lines", "filename OPTIONAL, ...", "Iterates over the lines of a file" },
            { "io", "close", "file OPTIONAL", "Closes a file" },
            { "coroutine", "create", "function", "Creates a coroutine" },
            { "coroutine", "resume", "co, ...", "Starts or continues a coroutine" },
            { "coroutine", "yield", "...", "Suspends the running coroutine" },
            { "coroutine", "wrap", "function", "Creates a coroutine that is called like a function" },
            { "coroutine", "status", "co", "Status of a coroutine" },
            { "coroutine", "running", "", "The running coroutine" },
            { "debug", "getinfo", "thread OPTIONAL, function, what OPTIONAL", "Information about a function or a stack level" },
            { "debug", "traceback", "thread OPTIONAL, message OPTIONAL, level OPTIONAL", "Stack traceback as a string" },
            { "debug", "getlocal", "thread OPTIONAL, level, index", "Name and value of a local variable" },
            { "debug", "getupvalue", "function, index", "Name and value of an upvalue" },
            { "debug", "setupvalue", "function, index, value", "Sets an upvalue" },
            { "debug", "sethook", "thread OPTIONAL, hook, mask, count OPTIONAL", "Sets a debug hook" },
            { "debug", "getmetatable", "value", "Metatable without __metatable protection" },
            { "debug", "setmetatable", "value, table", "Sets the metatable of any value" },
            { "utf8", "char", "...", "Builds a UTF-8 string" },
            { "utf8", "len", "s, i OPTIONAL, j OPTIONAL", "Number of UTF-8 characters" },
            { "utf8", "codepoint", "s, i OPTIONAL, j OPTIONAL", "Code points of the characters" },
        };

        constexpr const char* kKeywords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or",
            "repeat", "return", "then", "true", "until", "while" };

        struct DocRef
        {
            std::string Name;
            std::string Params;
            std::string Text;
            DocKind Kind = DocKind::Function;
        };

        std::string Key(std::string_view owner, std::string_view name)
        {
            return Lower(owner) + '\n' + Lower(name);
        }

        struct DocIndex
        {
            std::unordered_map<std::string, std::vector<DocRef>> ByName;
            std::unordered_map<std::string, std::vector<DocRef>> ByOwner;
            std::unordered_map<std::string, std::pair<std::string, std::string>> Classes;
        };

        const DocIndex& Index()
        {
            static const DocIndex index = []
                {
                    DocIndex result;
                    for (const DocEntry& entry : DocEntries())
                    {
                        DocRef ref{ entry.Name, entry.Params, entry.Text, entry.Kind };
                        result.ByName[Key(entry.Owner, entry.Name)].push_back(ref);
                        result.ByOwner[Lower(entry.Owner)].push_back(std::move(ref));
                    }
                    for (const StandardEntry& entry : kStandard)
                    {
                        DocRef ref{ entry.Name, entry.Params, entry.Text, DocKind::Function };
                        result.ByName[Key(entry.Owner, entry.Name)].push_back(ref);
                        result.ByOwner[Lower(entry.Owner)].push_back(std::move(ref));
                    }
                    for (const DocClass& entry : DocClasses())
                        result.Classes[Lower(entry.Name)] = { entry.Name, entry.Parent };
                    return result;
                }();
            return index;
        }

        const std::vector<DocRef>* Docs(std::string_view owner, std::string_view name)
        {
            const std::string bare = name.size() > 2 && name.substr(name.size() - 2) == "[]" ? std::string(name.substr(0, name.size() - 2)) : std::string(name);
            const auto found = Index().ByName.find(Key(owner, bare));
            if (found != Index().ByName.end())
                return &found->second;
            const auto indexed = Index().ByName.find(Key(owner, bare + "[]"));
            return indexed == Index().ByName.end() ? nullptr : &indexed->second;
        }

        std::string DocClassFor(std::string_view className)
        {
            static const std::pair<const char*, const char*> kAliases[] = { { "tmainform", "Form" }, { "tfrmluaengine", "Form" }, { "tmemorybrowser", "Form" },
                { "tcustomform", "Form" }, { "tportablenetworkgraphic", "Graphic" }, { "tjpegimage", "Graphic" } };
            const auto& classes = Index().Classes;
            if (const auto found = classes.find(Lower(className)); found != classes.end())
                return found->second.first;
            if (className.size() > 1 && (className[0] == 'T' || className[0] == 't'))
            {
                if (const auto found = classes.find(Lower(className.substr(1))); found != classes.end())
                    return found->second.first;
            }
            for (const auto& [alias, target] : kAliases)
            {
                if (EqualsNoCase(className, alias))
                    return DocClassFor(target);
            }
            return {};
        }

        std::string DocParent(std::string_view docClass)
        {
            const auto found = Index().Classes.find(Lower(docClass));
            return found == Index().Classes.end() ? std::string() : found->second.second;
        }

        int IndexValue(lua_State* state)
        {
            lua_settop(state, 2);
            lua_gettable(state, 1);
            return 1;
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

        std::optional<std::string> LuaParameters(lua_State* state, int index, bool dropSelf)
        {
            index = lua_absindex(state, index);
            lua_pushvalue(state, index);
            lua_Debug info{};
            if (!lua_getinfo(state, ">Su", &info) || !info.what || std::strcmp(info.what, "C") == 0)
                return std::nullopt;
            std::vector<std::string> names;
            for (int i = 1; i <= info.nparams; ++i)
            {
                lua_pushvalue(state, index);
                const char* name = lua_getlocal(state, nullptr, i);
                lua_pop(state, 1);
                names.emplace_back(name ? name : "?");
            }
            if (info.isvararg)
                names.emplace_back("...");
            if (dropSelf && !names.empty() && names.front() == "self")
                names.erase(names.begin());
            std::string result;
            for (const std::string& name : names)
                result += (result.empty() ? "" : ", ") + name;
            return result;
        }

        std::string KindOf(lua_State* state, int index, bool method)
        {
            switch (lua_type(state, index))
            {
            case LUA_TFUNCTION: return method ? "method" : "function";
            case LUA_TTABLE: return "table";
            case LUA_TUSERDATA: return "object";
            default: return "value";
            }
        }

        bool Matches(std::string_view name, std::string_view prefix)
        {
            if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
                return false;
            return name.size() >= prefix.size() && EqualsNoCase(name.substr(0, prefix.size()), prefix);
        }

        bool Prefer(std::string_view candidate, std::string_view current, std::string_view prefix)
        {
            const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
            if (!prefix.empty())
                return upper(candidate[0]) == upper(prefix[0]) && upper(current[0]) != upper(prefix[0]);
            return !upper(candidate[0]) && upper(current[0]);
        }

        class Collector
        {
        public:
            Collector(lua_State* state, std::string_view prefix, bool method) : m_state(state), m_prefix(prefix), m_method(method) {}

            void Table(int index, std::string_view owner, int depth)
            {
                if (depth > 4 || !lua_istable(m_state, index))
                    return;
                index = lua_absindex(m_state, index);
                lua_pushnil(m_state);
                while (lua_next(m_state, index))
                {
                    if (lua_type(m_state, -2) == LUA_TSTRING)
                    {
                        std::size_t length = 0;
                        const char* raw = lua_tolstring(m_state, -2, &length);
                        const std::string name(raw, length);
                        if (Matches(name, m_prefix) && (!m_method || lua_isfunction(m_state, -1)))
                            Value(name, -1, owner);
                    }
                    lua_pop(m_state, 1);
                }
                if (lua_getmetatable(m_state, index))
                {
                    lua_pushstring(m_state, "__index");
                    lua_rawget(m_state, -2);
                    if (lua_istable(m_state, -1))
                        Table(-1, owner, depth + 1);
                    lua_pop(m_state, 2);
                }
            }

            void Value(const std::string& name, int index, std::string_view owner)
            {
                LuaCompletion completion;
                completion.Name = name;
                completion.Kind = KindOf(m_state, index, m_method);
                if (lua_isfunction(m_state, index))
                {
                    if (const auto manifold = ManifoldDocOf(m_state, index))
                    {
                        completion.Signature = "(" + manifold->Params + ")";
                        completion.Text = manifold->Text + (manifold->Returns.empty() ? "" : " Returns " + manifold->Returns + ".");
                    }
                    else if (const auto* docs = Docs(owner, name))
                    {
                        completion.Signature = "(" + docs->front().Params + ")";
                        completion.Text = docs->front().Text;
                    }
                    else if (const auto params = LuaParameters(m_state, index, m_method))
                        completion.Signature = "(" + *params + ")";
                    else
                        completion.Signature = "(...)";
                }
                else if (const auto* docs = Docs(owner, name))
                {
                    completion.Text = docs->front().Text;
                }
                Add(std::move(completion));
            }

            void Class(std::string docClass)
            {
                for (int guard = 0; !docClass.empty() && guard < 16; ++guard)
                {
                    if (const auto found = Index().ByOwner.find(Lower(docClass)); found != Index().ByOwner.end())
                    {
                        for (const DocRef& ref : found->second)
                        {
                            const bool isMethod = ref.Kind == DocKind::Method;
                            if ((m_method && !isMethod))
                                continue;
                            std::string name = ref.Name;
                            const bool indexed = name.size() > 2 && name.substr(name.size() - 2) == "[]";
                            if (indexed)
                                name.resize(name.size() - 2);
                            if (!Matches(name, m_prefix))
                                continue;
                            LuaCompletion completion;
                            completion.Name = name;
                            completion.Kind = isMethod ? "method" : name.rfind("On", 0) == 0 && ref.Text.rfind("function", 0) == 0 ? "event" : "property";
                            completion.Signature = isMethod ? "(" + ref.Params + ")" : indexed ? "[index]" : "";
                            completion.Text = ref.Text;
                            Add(std::move(completion));
                        }
                    }
                    docClass = DocParent(docClass);
                }
            }

            void Keywords()
            {
                for (const char* keyword : kKeywords)
                {
                    if (Matches(keyword, m_prefix))
                        Add({ keyword, "keyword", "", "" });
                }
            }

            std::vector<LuaCompletion> Take()
            {
                std::vector<LuaCompletion> result;
                result.reserve(m_found.size());
                for (auto& [key, completion] : m_found)
                    result.push_back(std::move(completion));
                std::sort(result.begin(), result.end(), [](const LuaCompletion& a, const LuaCompletion& b)
                    {
                        const std::string left = Lower(a.Name);
                        const std::string right = Lower(b.Name);
                        return left != right ? left < right : a.Name < b.Name;
                    });
                if (result.size() > 400)
                    result.resize(400);
                return result;
            }

        private:
            void Add(LuaCompletion completion)
            {
                const std::string key = Lower(completion.Name);
                const auto found = m_found.find(key);
                if (found == m_found.end())
                {
                    m_found.emplace(key, std::move(completion));
                    return;
                }
                if (found->second.Name != completion.Name && Prefer(completion.Name, found->second.Name, m_prefix))
                {
                    if (completion.Signature.empty())
                        completion.Signature = found->second.Signature;
                    if (completion.Text.empty())
                        completion.Text = found->second.Text;
                    found->second = std::move(completion);
                }
            }

            lua_State* m_state;
            std::string m_prefix;
            bool m_method;
            std::map<std::string, LuaCompletion> m_found;
        };

        std::string ClassNameOf(lua_State* state, int index)
        {
            if (!lua_istable(state, index) && !lua_isuserdata(state, index))
                return {};
            PushIndexed(state, index, "ClassName");
            std::string name = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "";
            lua_pop(state, 1);
            return name;
        }

        std::vector<std::string> FromDocs(const std::vector<DocRef>& docs, std::string_view name)
        {
            std::vector<std::string> result;
            for (const DocRef& doc : docs)
                result.push_back(std::string(name) + "(" + doc.Params + ")");
            return result;
        }
    }

    std::vector<LuaCompletion> LuaEngine::Complete(std::string_view chain, std::string_view prefix, bool method)
    {
        lua_State* state = m_state;
        const int top = lua_gettop(state);
        Collector collector(state, prefix, method);
        if (TrimView(chain).empty())
        {
            lua_pushglobaltable(state);
            collector.Keywords();
            collector.Table(-1, "", 0);
        }
        else if (PushChain(state, chain))
        {
            std::string owner(chain);
            if (lua_type(state, -1) == LUA_TSTRING)
            {
                lua_pop(state, 1);
                lua_getglobal(state, "string");
                owner = "string";
            }
            std::string docClass = ClassNameOf(state, -1);
            docClass = docClass.empty() ? std::string() : DocClassFor(docClass);
            collector.Table(-1, docClass.empty() ? std::string_view(owner) : std::string_view(docClass), 0);
            if (!docClass.empty())
                collector.Class(docClass);
        }
        lua_settop(state, top);
        return collector.Take();
    }

    std::vector<std::string> LuaEngine::CallSignatures(std::string_view chain, bool method)
    {
        lua_State* state = m_state;
        const int top = lua_gettop(state);
        const std::string text(TrimView(chain));
        const std::size_t dot = text.rfind('.');
        const std::string objectChain = dot == std::string::npos ? std::string() : text.substr(0, dot);
        const std::string name = dot == std::string::npos ? text : text.substr(dot + 1);
        std::vector<std::string> result;
        if (name.empty())
            return result;

        std::string owner;
        if (objectChain.empty())
        {
            lua_getglobal(state, name.c_str());
        }
        else if (PushChain(state, objectChain))
        {
            owner = objectChain;
            if (lua_type(state, -1) == LUA_TSTRING)
            {
                lua_pop(state, 1);
                lua_getglobal(state, "string");
                owner = "string";
            }
            if (const std::string className = ClassNameOf(state, -1); !className.empty())
            {
                for (std::string docClass = DocClassFor(className); !docClass.empty(); docClass = DocParent(docClass))
                {
                    if (const auto* docs = Docs(docClass, name))
                    {
                        lua_settop(state, top);
                        return FromDocs(*docs, name);
                    }
                }
            }
            PushIndexed(state, -1, name);
            lua_remove(state, -2);
        }
        else
        {
            lua_settop(state, top);
            return result;
        }

        if (const auto manifold = ManifoldDocOf(state, -1))
            result.push_back(name + "(" + manifold->Params + ")");
        else if (const auto* docs = Docs(owner, name); docs && (lua_isfunction(state, -1) || lua_isnil(state, -1)))
            result = FromDocs(*docs, name);
        else if (lua_isfunction(state, -1))
        {
            const auto params = LuaParameters(state, -1, method);
            result.push_back(name + "(" + (params ? *params : "...") + ")");
        }
        lua_settop(state, top);
        return result;
    }

    std::vector<std::string> LuaEngine::DocumentedSignatures(std::string_view name)
    {
        const auto* docs = Docs("", name);
        return docs ? FromDocs(*docs, name) : std::vector<std::string>{};
    }

    std::string LuaEngine::DocumentedText(std::string_view name)
    {
        const auto* docs = Docs("", name);
        return docs ? docs->front().Text : std::string();
    }
}
