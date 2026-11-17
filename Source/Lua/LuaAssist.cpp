#include "Lua/LuaEngine.h"

#include "Core/Text.h"
#include "Lua/DocRegistry.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaDocBinding.h"
#include "Text/LuaContext.h"

#include "lua.hpp"

#include <algorithm>
#include <map>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kKeywords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or",
            "repeat", "return", "then", "true", "until", "while" };
        constexpr std::size_t kChunkLimit = 64;

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
            return Text::FuzzyScore(name, prefix) >= 0;
        }

        bool Prefer(std::string_view candidate, std::string_view current, std::string_view prefix)
        {
            const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
            if (!prefix.empty())
                return upper(candidate[0]) == upper(prefix[0]) && upper(current[0]) != upper(prefix[0]);
            return !upper(candidate[0]) && upper(current[0]);
        }

        std::string CallParams(const DocSignature& signature, bool method)
        {
            DocSignature shown = signature;
            if (method && !shown.Params.empty() && shown.Params.front().Name == "self")
                shown.Params.erase(shown.Params.begin());
            return FormatParams(shown);
        }

        std::string HelpText(const DocRecord& record)
        {
            std::string text = record.Text;
            if (record.Callable() && !record.Signatures.empty() && !record.Signatures.front().Returns.empty())
                text += (text.empty() ? "Returns " : " Returns ") + record.Signatures.front().Returns + ".";
            return text;
        }

        std::string ParentPath(std::string_view chain, std::string& name)
        {
            const std::string text(TrimView(chain));
            const std::size_t dot = text.rfind('.');
            name = dot == std::string::npos ? text : text.substr(dot + 1);
            return dot == std::string::npos ? std::string() : text.substr(0, dot);
        }

        class Collector
        {
        public:
            Collector(LuaEngine& engine, lua_State* state, std::string_view prefix, bool method) : m_engine(engine), m_state(state), m_prefix(prefix), m_method(method) {}

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
                const std::string path = owner.empty() ? name : std::string(owner) + "." + name;
                const DocPtr doc = DocOfValue(m_engine, m_state, index, path, lua_isfunction(m_state, index));
                if (lua_isfunction(m_state, index))
                    completion.Signature = doc && doc->Callable() && !doc->Signatures.empty() ? "(" + CallParams(doc->Signatures.front(), m_method) + ")" : "(...)";
                if (doc)
                    completion.Text = HelpText(*doc);
                completion.Doc = doc;
                Add(std::move(completion));
            }

            void Class(const std::string& docClass)
            {
                for (const DocPtr& member : m_engine.Docs().ClassMembers(docClass))
                {
                    const bool isMethod = member->Kind == DocItemKind::Method;
                    if (m_method && !isMethod)
                        continue;
                    if (!Matches(member->Name, m_prefix))
                        continue;
                    LuaCompletion completion;
                    completion.Name = member->Name;
                    completion.Kind = isMethod ? "method" : member->Kind == DocItemKind::Event ? "event" : "property";
                    completion.Signature = isMethod && !member->Signatures.empty() ? "(" + FormatParams(member->Signatures.front()) + ")" : member->Indexed ? "[index]" : "";
                    completion.Text = member->Text;
                    completion.Doc = member;
                    Add(std::move(completion));
                }
            }

            void Keywords()
            {
                for (const char* keyword : kKeywords)
                {
                    if (Matches(keyword, m_prefix))
                        Add({ keyword, "keyword", "", "", nullptr });
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
                    if (!completion.Doc)
                        completion.Doc = found->second.Doc;
                    found->second = std::move(completion);
                }
            }

            LuaEngine& m_engine;
            lua_State* m_state;
            std::string m_prefix;
            bool m_method;
            std::map<std::string, LuaCompletion> m_found;
        };

        std::vector<std::string> SignatureTexts(const DocRecord& record, bool method)
        {
            std::vector<std::string> result;
            for (const DocSignature& signature : record.Signatures)
                result.push_back(record.Name + "(" + CallParams(signature, method) + ")");
            return result;
        }
    }

    std::vector<LuaCompletion> LuaEngine::Complete(std::string_view chain, std::string_view prefix, bool method)
    {
        lua_State* state = m_state;
        const int top = lua_gettop(state);
        Collector collector(*this, state, prefix, method);
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
            const std::string className = ClassNameOf(state, -1);
            const std::string docClass = className.empty() ? std::string() : m_docs->ResolveClass(className);
            collector.Table(-1, docClass.empty() ? std::string_view(owner) : std::string_view(docClass), 0);
            if (!docClass.empty())
                collector.Class(docClass);
        }
        lua_settop(state, top);
        return collector.Take();
    }

    std::shared_ptr<const DocRecord> LuaEngine::Describe(std::string_view chain, bool method)
    {
        (void)method;
        lua_State* state = m_state;
        const int top = lua_gettop(state);
        std::string name;
        const std::string objectChain = ParentPath(chain, name);
        if (name.empty())
            return nullptr;
        std::string path = name;
        if (objectChain.empty())
        {
            lua_getglobal(state, name.c_str());
        }
        else if (PushChain(state, objectChain))
        {
            std::string owner = objectChain;
            if (lua_type(state, -1) == LUA_TSTRING)
            {
                lua_pop(state, 1);
                lua_getglobal(state, "string");
                owner = "string";
            }
            if (const std::string className = ClassNameOf(state, -1); !className.empty())
            {
                for (std::string docClass = m_docs->ResolveClass(className); !docClass.empty(); docClass = m_docs->ParentOf(docClass))
                {
                    if (DocPtr member = m_docs->Find(docClass + "." + name); member && member->OwnerIsClass)
                    {
                        lua_settop(state, top);
                        return member;
                    }
                }
            }
            PushIndexed(state, -1, name);
            lua_remove(state, -2);
            path = owner + "." + name;
        }
        else
        {
            lua_settop(state, top);
            return m_docs->Find(TrimView(chain));
        }
        DocPtr doc = DocOfValue(*this, state, -1, path, lua_isfunction(state, -1));
        lua_settop(state, top);
        return doc;
    }

    std::vector<std::string> LuaEngine::CallSignatures(std::string_view chain, bool method)
    {
        const DocPtr doc = Describe(chain, method);
        if (!doc || !doc->Callable())
            return {};
        return SignatureTexts(*doc, method);
    }

    std::vector<std::string> LuaEngine::CallArgumentChoices(std::string_view chain, bool method, int argument)
    {
        const DocPtr doc = Describe(chain, method);
        if (!doc || !doc->Callable() || argument < 0)
            return {};
        for (const DocSignature& signature : doc->Signatures)
        {
            std::vector<DocParam> params = signature.Params;
            if (method && !params.empty() && params.front().Name == "self")
                params.erase(params.begin());
            if (static_cast<std::size_t>(argument) < params.size())
                return params[static_cast<std::size_t>(argument)].Choices;
            if (!params.empty() && params.back().Variadic)
                return params.back().Choices;
        }
        return {};
    }

    std::vector<std::string> LuaEngine::DocumentedSignatures(std::string_view name)
    {
        const DocPtr doc = BuiltinDocs()->Find(name);
        return doc && doc->Callable() ? SignatureTexts(*doc, false) : std::vector<std::string>{};
    }

    std::string LuaEngine::DocumentedText(std::string_view name)
    {
        const DocPtr doc = BuiltinDocs()->Find(name);
        return doc ? doc->Text : std::string();
    }

    void LuaEngine::RememberChunk(std::string name, std::string text)
    {
        if (m_chunks.find(name) == m_chunks.end())
        {
            m_chunkOrder.push_back(name);
            while (m_chunkOrder.size() > kChunkLimit)
            {
                m_chunks.erase(m_chunkOrder.front());
                m_chunkOrder.pop_front();
            }
        }
        m_chunks[std::move(name)] = std::move(text);
    }

    std::string LuaEngine::ChunkText(std::string_view name) const
    {
        const auto found = m_chunks.find(std::string(name));
        return found == m_chunks.end() ? std::string() : found->second;
    }
}
