#include "Templates/TemplateCheck.h"

#include "Templates/TemplateEngine.h"
#include "Templates/TemplateGenerator.h"

#include "lua.hpp"

#include <map>
#include <regex>
#include <set>

namespace Manifold::Templates
{
    namespace
    {
        const std::set<std::string>& Keywords()
        {
            static const std::set<std::string> keywords = {
                "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
                "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"
            };
            return keywords;
        }

        const std::set<std::string>& BaseNames()
        {
            static const std::set<std::string> names = {
                "hex", "join", "default", "isEmpty", "byteCount", "bytesPrefix", "trim", "include", "_safe",
                "string", "math", "table", "ipairs", "pairs", "tonumber", "tostring", "select", "TemplateSettings", "FinalCompilation"
            };
            return names;
        }

        std::map<std::string, int> UnknownNames(const std::vector<Node>& nodes)
        {
            static const std::regex comment("--[^\n]*");
            static const std::regex doubleQuoted(R"("(?:[^"\\]|\\.)*")");
            static const std::regex singleQuoted(R"('(?:[^'\\]|\\.)*')");
            static const std::regex number(R"(\b[0-9][A-Za-z0-9_.]*)");
            static const std::regex localNames(R"(local\s+([A-Za-z_][A-Za-z0-9_,\s]*))");
            static const std::regex numericFor(R"(for\s+([A-Za-z_][A-Za-z0-9_]*)\s*=)");
            static const std::regex genericFor(R"(for\s+([A-Za-z_][A-Za-z0-9_,\s]*?)\s+in\s)");
            static const std::regex parameters(R"(function[^(]*\(([^)]*)\))");
            static const std::regex identifier("[A-Za-z_][A-Za-z0-9_]*");
            static const std::regex reference(R"(([.:]?)\s*([A-Za-z_][A-Za-z0-9_]*))");

            std::vector<std::pair<std::string, int>> cleaned;
            for (const Node& node : nodes)
            {
                if (node.Kind == NodeKind::Text)
                    continue;
                std::string code = std::regex_replace(node.Text, comment, " ");
                code = std::regex_replace(code, doubleQuoted, " ");
                code = std::regex_replace(code, singleQuoted, " ");
                code = std::regex_replace(code, number, " ");
                cleaned.emplace_back(std::move(code), node.Line);
            }
            std::set<std::string> declared;
            auto declare = [&](const std::string& list)
            {
                for (auto it = std::sregex_iterator(list.begin(), list.end(), identifier); it != std::sregex_iterator(); ++it)
                    declared.insert(it->str());
            };
            for (const auto& [code, line] : cleaned)
            {
                for (const std::regex* pattern : { &localNames, &numericFor, &genericFor, &parameters })
                {
                    for (auto it = std::sregex_iterator(code.begin(), code.end(), *pattern); it != std::sregex_iterator(); ++it)
                        declare((*it)[1].str());
                }
            }
            std::map<std::string, int> unknown;
            for (const auto& [code, line] : cleaned)
            {
                for (auto it = std::sregex_iterator(code.begin(), code.end(), reference); it != std::sregex_iterator(); ++it)
                {
                    const std::string name = (*it)[2].str();
                    if (!(*it)[1].str().empty() || Keywords().count(name) || declared.count(name))
                        continue;
                    unknown.emplace(name, line);
                }
            }
            return unknown;
        }

        bool GlobalExists(lua_State* state, const std::string& name)
        {
            const bool present = lua_getglobal(state, name.c_str()) != LUA_TNIL;
            lua_pop(state, 1);
            return present;
        }
    }

    const std::vector<HelperInfo>& Helpers()
    {
        static const std::vector<HelperInfo> helpers = {
            { "hex", "value", "The number as upper-case hex without a prefix; anything else as text." },
            { "join", "separator, ...", "The values that are not nil or empty, joined with the separator." },
            { "default", "value, fallback", "The value, or the fallback when the value is nil or empty." },
            { "isEmpty", "value", "Whether the value is nil or an empty string." },
            { "byteCount", "bytes", "How many bytes a byte string like \"48 8B 05\" holds." },
            { "bytesPrefix", "bytes, count", "The first count bytes of a byte string." },
            { "trim", "value", "The text without spaces at the start and the end." },
            { "include", "name", "Renders a template from the Partials folder at this place." },
        };
        return helpers;
    }

    std::vector<std::string> HelperNames()
    {
        std::vector<std::string> names;
        for (const HelperInfo& helper : Helpers())
            names.push_back(helper.Name);
        return names;
    }

    const std::vector<std::string>& LuaBaseNames()
    {
        static const std::vector<std::string> names = { "string", "math", "table", "ipairs", "pairs", "tonumber", "tostring", "select", "TemplateSettings", "FinalCompilation" };
        return names;
    }

    CheckEntry CheckTemplate(lua_State* state, const Library& library, const TemplateInfo& info, const std::string* script)
    {
        CheckEntry entry;
        entry.Id = info.Id;
        entry.Caption = info.Caption;
        entry.Source = info.SourceName;
        std::string text;
        if (script)
            text = *script;
        else if (const auto source = library.Script(info))
            text = source->Text;
        else
        {
            entry.Errors.push_back("The template file could not be read");
            return entry;
        }
        const ParseResult parsed = Parse(text);
        if (!parsed.Ok())
        {
            entry.Errors.push_back(parsed.Error + " (line " + std::to_string(parsed.ErrorLine) + ")");
            return entry;
        }
        const CompiledTemplate compiled = Compile(parsed.Nodes, info.SourceName);
        const int top = lua_gettop(state);
        if (luaL_loadbufferx(state, compiled.Chunk.data(), compiled.Chunk.size(), "=check", "t") != LUA_OK)
        {
            const std::string message = lua_tostring(state, -1) ? lua_tostring(state, -1) : "syntax error";
            static const std::regex location(R"(check:(\d+):\s*([\s\S]*))");
            std::smatch match;
            if (std::regex_search(message, match, location))
                entry.Errors.push_back("Syntax error: " + match[2].str() + " (line " + std::to_string(compiled.TemplateLine(std::stoi(match[1].str()))) + ")");
            else
                entry.Errors.push_back("Syntax error: " + message);
        }
        lua_settop(state, top);

        for (const std::string& name : IncludedNames(parsed.Nodes))
        {
            if (!library.Partial(name))
                entry.Errors.push_back("Included template '" + name + "' was not found");
        }
        std::set<std::string> inputs;
        for (const InputDefinition& input : info.Inputs)
        {
            inputs.insert(input.Name);
            if (IsVariable(input.Name))
                entry.Warnings.push_back("Input '" + input.Name + "' hides the value of the same name");
        }
        for (const std::string& name : info.Requires)
        {
            if (!IsVariable(name) && !inputs.count(name) && !BaseNames().count(name))
                entry.Errors.push_back("Requires names '" + name + "', which no provider knows");
        }
        for (const std::string& name : info.Optional)
        {
            if (!IsVariable(name) && !inputs.count(name) && !BaseNames().count(name))
                entry.Warnings.push_back("Optional names '" + name + "', which no provider knows");
        }
        for (const auto& [name, line] : UnknownNames(parsed.Nodes))
        {
            if (IsVariable(name) || inputs.count(name) || BaseNames().count(name))
                continue;
            if (info.AllowUnsafeGlobals && GlobalExists(state, name))
                continue;
            entry.Warnings.push_back("Unknown name '" + name + "' on line " + std::to_string(line) + " is empty when generated");
        }
        return entry;
    }

    std::vector<CheckEntry> CheckAll(lua_State* state, const Library& library)
    {
        std::vector<CheckEntry> entries;
        for (const TemplateInfo& info : library.Templates())
            entries.push_back(CheckTemplate(state, library, info));
        return entries;
    }
}
