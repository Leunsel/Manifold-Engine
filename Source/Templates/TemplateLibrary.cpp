#include "Templates/TemplateLibrary.h"

#include "Core/Text.h"
#include "Templates/BuiltinTemplates.h"

#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace Manifold::Templates
{
    namespace
    {
        constexpr std::string_view kScriptExtension = ".cea";
        constexpr std::string_view kSettingsSuffix = ".Settings.lua";

        constexpr const char* kSettingsLoader = R"LUA(
local source, name = ...
local function copy(t) local c = {} for k, v in pairs(t) do c[k] = v end return c end
local env = {
    ipairs = ipairs, pairs = pairs, tonumber = tonumber, tostring = tostring,
    math = copy(math), string = copy(string), table = copy(table)
}
local chunk, err = load(source, name, "t", env)
if not chunk then return nil, "Settings syntax error: " .. tostring(err) end
local ok, settings = pcall(chunk)
if not ok then return nil, "Settings execution failed: " .. tostring(settings) end
if type(settings) ~= "table" then return nil, "Settings must return a table" end
return settings
)LUA";

        std::string TrimText(std::string_view text)
        {
            const std::size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            const std::size_t last = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(first, last - first + 1));
        }

        std::string Slugify(std::string_view text)
        {
            std::string slug;
            bool dash = false;
            for (const char c : text)
            {
                const unsigned char byte = static_cast<unsigned char>(c);
                if (std::isalnum(byte))
                {
                    if (dash && !slug.empty())
                        slug += '-';
                    slug += static_cast<char>(std::tolower(byte));
                    dash = false;
                }
                else
                    dash = true;
            }
            return slug.empty() ? "template" : slug;
        }

        bool EndsWithNoCase(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && EqualsNoCase(text.substr(text.size() - suffix.size()), suffix);
        }

        std::optional<std::string> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::nullopt;
            std::ostringstream content;
            content << file.rdbuf();
            return content.str();
        }

        std::string PathText(const std::filesystem::path& path)
        {
            return WideToUtf8(path.wstring());
        }

        Value ToValue(lua_State* state, int index)
        {
            switch (lua_type(state, index))
            {
            case LUA_TBOOLEAN: return lua_toboolean(state, index) != 0;
            case LUA_TNUMBER:
                if (lua_isinteger(state, index))
                    return static_cast<std::int64_t>(lua_tointeger(state, index));
                return lua_tonumber(state, index);
            case LUA_TSTRING:
            {
                std::size_t size = 0;
                const char* text = lua_tolstring(state, index, &size);
                return std::string(text, size);
            }
            default: return std::monostate{};
            }
        }

        std::optional<double> NumberOf(const Value& value)
        {
            if (const auto* integer = std::get_if<std::int64_t>(&value))
                return static_cast<double>(*integer);
            if (const auto* number = std::get_if<double>(&value))
                return *number;
            if (const auto* text = std::get_if<std::string>(&value))
            {
                const std::string trimmed = TrimText(*text);
                if (trimmed.empty())
                    return std::nullopt;
                char* end = nullptr;
                const double parsed = std::strtod(trimmed.c_str(), &end);
                if (end && *end == '\0')
                    return parsed;
                if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X'))
                {
                    const unsigned long long hex = std::strtoull(trimmed.c_str() + 2, &end, 16);
                    if (end && *end == '\0')
                        return static_cast<double>(hex);
                }
            }
            return std::nullopt;
        }

        std::string TextOf(const Value& value)
        {
            if (const auto* text = std::get_if<std::string>(&value))
                return *text;
            if (const auto* flag = std::get_if<bool>(&value))
                return *flag ? "true" : "false";
            if (const auto* integer = std::get_if<std::int64_t>(&value))
                return std::to_string(*integer);
            if (const auto* number = std::get_if<double>(&value))
            {
                std::ostringstream stream;
                stream << *number;
                return stream.str();
            }
            return {};
        }

        class Reader
        {
        public:
            Reader(lua_State* state, int table) : m_state(state), m_table(table) {}

            bool Has(const char* key) const
            {
                const bool present = lua_getfield(m_state, m_table, key) != LUA_TNIL;
                lua_pop(m_state, 1);
                return present;
            }

            Value Raw(const char* key) const
            {
                lua_getfield(m_state, m_table, key);
                Value value = ToValue(m_state, -1);
                lua_pop(m_state, 1);
                return value;
            }

            int Type(const char* key) const
            {
                const int type = lua_getfield(m_state, m_table, key);
                lua_pop(m_state, 1);
                return type;
            }

            bool Check(const char* key, int expected, const char* name, std::string& error) const
            {
                const int type = Type(key);
                if (type != LUA_TNIL && type != expected)
                {
                    error = std::string(key) + " must be a " + name;
                    return false;
                }
                return true;
            }

            std::optional<std::string> String(const char* key) const
            {
                lua_getfield(m_state, m_table, key);
                std::optional<std::string> result;
                if (lua_type(m_state, -1) == LUA_TSTRING)
                {
                    std::size_t size = 0;
                    const char* text = lua_tolstring(m_state, -1, &size);
                    result = std::string(text, size);
                }
                lua_pop(m_state, 1);
                return result;
            }

            std::optional<bool> Bool(const char* key) const
            {
                lua_getfield(m_state, m_table, key);
                std::optional<bool> result;
                if (lua_type(m_state, -1) == LUA_TBOOLEAN)
                    result = lua_toboolean(m_state, -1) != 0;
                lua_pop(m_state, 1);
                return result;
            }

            std::optional<double> Number(const char* key) const
            {
                lua_getfield(m_state, m_table, key);
                std::optional<double> result;
                if (lua_type(m_state, -1) == LUA_TNUMBER)
                    result = lua_tonumber(m_state, -1);
                lua_pop(m_state, 1);
                return result;
            }

            bool List(const char* key, std::vector<std::string>& out, std::string& error) const
            {
                out.clear();
                const int type = lua_getfield(m_state, m_table, key);
                if (type == LUA_TNIL)
                {
                    lua_pop(m_state, 1);
                    return true;
                }
                if (type != LUA_TTABLE)
                {
                    lua_pop(m_state, 1);
                    error = std::string(key) + " must be a list of strings";
                    return false;
                }
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(m_state, -1));
                for (lua_Integer index = 1; index <= count; ++index)
                {
                    lua_rawgeti(m_state, -1, index);
                    if (lua_type(m_state, -1) != LUA_TSTRING)
                    {
                        lua_pop(m_state, 2);
                        error = std::string(key) + " must contain only strings";
                        return false;
                    }
                    out.emplace_back(lua_tostring(m_state, -1));
                    lua_pop(m_state, 1);
                }
                lua_pop(m_state, 1);
                return true;
            }

        private:
            lua_State* m_state;
            int m_table;
        };

        bool IsIdentifier(std::string_view text)
        {
            static const std::regex pattern("^[A-Za-z_][A-Za-z0-9_]*$");
            return std::regex_match(text.begin(), text.end(), pattern);
        }

        bool ReadInputs(lua_State* state, int table, std::vector<InputDefinition>& inputs, std::string& error)
        {
            inputs.clear();
            const int type = lua_getfield(state, table, "Inputs");
            if (type == LUA_TNIL)
            {
                lua_pop(state, 1);
                return true;
            }
            if (type != LUA_TTABLE)
            {
                lua_pop(state, 1);
                error = "Inputs must be a list";
                return false;
            }
            const int list = lua_gettop(state);
            std::set<std::string> seen;
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, list));
            for (lua_Integer index = 1; index <= count; ++index)
            {
                lua_rawgeti(state, list, index);
                if (lua_type(state, -1) != LUA_TTABLE)
                {
                    lua_settop(state, list - 1);
                    error = "Input #" + std::to_string(index) + " must be a table";
                    return false;
                }
                const Reader entry(state, lua_gettop(state));
                InputDefinition input;
                input.Name = entry.String("Name").value_or("");
                if (!IsIdentifier(input.Name))
                {
                    lua_settop(state, list - 1);
                    error = "Input #" + std::to_string(index) + " has no valid Name (must be an identifier)";
                    return false;
                }
                input.Type = entry.String("Type").value_or("string");
                if (input.Type != "string" && input.Type != "boolean" && input.Type != "integer" && input.Type != "number" && input.Type != "enum")
                {
                    lua_settop(state, list - 1);
                    error = "Input '" + input.Name + "' has unknown Type '" + input.Type + "'";
                    return false;
                }
                input.Caption = entry.String("Caption").value_or(input.Name);
                const Value rawDefault = entry.Raw("Default");
                if (input.Type == "enum")
                {
                    if (!entry.List("Values", input.Values, error) || input.Values.empty())
                    {
                        lua_settop(state, list - 1);
                        error = "Enum input '" + input.Name + "' needs a non-empty Values list";
                        return false;
                    }
                    const std::string fallback = input.Values.front();
                    const std::string chosen = std::holds_alternative<std::monostate>(rawDefault) ? fallback : TextOf(rawDefault);
                    if (std::find(input.Values.begin(), input.Values.end(), chosen) == input.Values.end())
                    {
                        lua_settop(state, list - 1);
                        error = "Enum input '" + input.Name + "' default '" + chosen + "' is not in Values";
                        return false;
                    }
                    input.Default = chosen;
                }
                else if (input.Type == "boolean")
                {
                    const bool* flag = std::get_if<bool>(&rawDefault);
                    input.Default = flag && *flag;
                }
                else if (input.Type == "integer" || input.Type == "number")
                {
                    if (const auto minimum = NumberOf(entry.Raw("Min")))
                        input.Min = *minimum;
                    if (const auto maximum = NumberOf(entry.Raw("Max")))
                        input.Max = *maximum;
                    const double value = NumberOf(rawDefault).value_or(0.0);
                    if (input.Type == "integer")
                        input.Default = static_cast<std::int64_t>(std::floor(value));
                    else
                        input.Default = value;
                }
                else
                {
                    input.Default = TextOf(rawDefault);
                }
                if (!seen.insert(input.Name).second)
                {
                    lua_settop(state, list - 1);
                    error = "Input '" + input.Name + "' is declared twice";
                    return false;
                }
                inputs.push_back(std::move(input));
                lua_pop(state, 1);
            }
            lua_settop(state, list - 1);
            return true;
        }

        TemplateInfo DefaultInfo(const std::string& templateName)
        {
            TemplateInfo info;
            info.Name = templateName;
            info.Caption = templateName;
            info.Id = "legacy." + Slugify(templateName);
            info.SchemaVersion = 1;
            info.AllowUnsafeGlobals = true;
            return info;
        }

        std::uint64_t Mix(std::uint64_t hash, std::string_view text)
        {
            for (const char c : text)
            {
                hash ^= static_cast<unsigned char>(c);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        std::uint64_t Mix(std::uint64_t hash, std::uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                hash ^= (value >> shift) & 0xFF;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        const BuiltinFile* FindBuiltin(std::string_view path)
        {
            for (const BuiltinFile& file : BuiltinFiles())
            {
                if (EqualsNoCase(file.Path, path))
                    return &file;
            }
            return nullptr;
        }

        bool IsHeaderName(std::string_view stem)
        {
            return EqualsNoCase(stem, "header");
        }

        constexpr const char* kSerializer = R"LUA(
local value = ...
local function serialize(v)
    local kind = type(v)
    if kind == "string" then return string.format("%q", v) end
    if kind == "number" then
        if math.type(v) == "integer" then return tostring(v) end
        return string.format("%.17g", v)
    end
    if kind == "boolean" then return tostring(v) end
    if kind == "table" then
        local parts, count = {}, #v
        for i = 1, count do parts[#parts + 1] = serialize(v[i]) end
        local keys = {}
        for k in pairs(v) do
            if not (math.type(k) == "integer" and k >= 1 and k <= count) then keys[#keys + 1] = k end
        end
        table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
        for _, k in ipairs(keys) do
            local key = (type(k) == "string" and k:match("^[%a_][%w_]*$")) and k or ("[" .. serialize(k) .. "]")
            parts[#parts + 1] = key .. " = " .. serialize(v[k])
        end
        if #parts == 0 then return "{}" end
        return "{ " .. table.concat(parts, ", ") .. " }"
    end
    return "nil"
end
return serialize(value)
)LUA";

        bool IsKnownField(std::string_view key)
        {
            static const std::set<std::string, std::less<>> known = {
                "SchemaVersion", "Id", "Caption", "Description", "Author", "Version", "Tags", "Shortcut", "InSubMenu", "SubMenuName", "Category",
                "CategoryOrder", "MenuOrder", "Order", "Requires", "Optional", "Inputs", "Architectures", "Capabilities", "AllowUnsafeGlobals", "Memory",
                "AskForInjectionAddress", "AskForHookName", "AppendToHookName", "AllocationSize", "AllocationNear", "DefaultHookName"
            };
            return known.count(key) > 0;
        }

        bool HasLuaComment(std::string_view text)
        {
            bool inString = false;
            char quote = 0;
            for (std::size_t index = 0; index + 1 < text.size(); ++index)
            {
                const char c = text[index];
                if (inString)
                {
                    if (c == '\\')
                        ++index;
                    else if (c == quote)
                        inString = false;
                    continue;
                }
                if (c == '"' || c == '\'')
                {
                    inString = true;
                    quote = c;
                    continue;
                }
                if (c == '-' && text[index + 1] == '-')
                    return true;
            }
            return false;
        }

        void ReadCustomFields(lua_State* state, int table, std::vector<std::pair<std::string, std::string>>& out)
        {
            out.clear();
            lua_pushnil(state);
            while (lua_next(state, table) != 0)
            {
                if (lua_type(state, -2) == LUA_TSTRING && !IsKnownField(lua_tostring(state, -2)))
                {
                    const std::string key = lua_tostring(state, -2);
                    const int value = lua_gettop(state);
                    if (luaL_loadbuffer(state, kSerializer, std::char_traits<char>::length(kSerializer), "=settings serializer") == LUA_OK)
                    {
                        lua_pushvalue(state, value);
                        if (lua_pcall(state, 1, 1, 0) == LUA_OK && lua_type(state, -1) == LUA_TSTRING)
                            out.emplace_back(key, lua_tostring(state, -1));
                    }
                    lua_settop(state, value);
                }
                lua_pop(state, 1);
            }
            std::sort(out.begin(), out.end());
        }
    }

    std::optional<std::string> NormalizeAllocationSize(const Value& value)
    {
        if (const auto* integer = std::get_if<std::int64_t>(&value))
        {
            if (*integer > 0)
                return "$" + Hex(static_cast<std::uint64_t>(*integer));
            return std::nullopt;
        }
        if (const auto* number = std::get_if<double>(&value))
        {
            if (*number > 0 && *number == std::floor(*number) && *number < 1e15)
                return "$" + Hex(static_cast<std::uint64_t>(*number));
            return std::nullopt;
        }
        const auto* text = std::get_if<std::string>(&value);
        if (!text)
            return std::nullopt;
        const std::string trimmed = TrimText(*text);
        if (trimmed.empty())
            return std::nullopt;
        static const std::regex hex(R"(^\$[0-9A-Fa-f]+$)");
        static const std::regex decimal(R"(^[0-9]+$)");
        if (std::regex_match(trimmed, hex))
        {
            if (std::strtoull(trimmed.c_str() + 1, nullptr, 16) == 0)
                return std::nullopt;
            std::string upper = trimmed;
            std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return upper;
        }
        if (std::regex_match(trimmed, decimal) && std::strtoull(trimmed.c_str(), nullptr, 10) > 0)
            return trimmed;
        return std::nullopt;
    }

    bool PushSettingsTable(lua_State* state, std::string_view text, const std::string& chunkName, std::string& error)
    {
        if (text.starts_with("\xEF\xBB\xBF"))
            text.remove_prefix(3);
        const int top = lua_gettop(state);
        if (luaL_loadbuffer(state, kSettingsLoader, std::char_traits<char>::length(kSettingsLoader), "=template settings loader") != LUA_OK)
        {
            error = lua_tostring(state, -1) ? lua_tostring(state, -1) : "settings loader failed";
            lua_settop(state, top);
            return false;
        }
        lua_pushlstring(state, text.data(), text.size());
        lua_pushstring(state, chunkName.c_str());
        if (lua_pcall(state, 2, 2, 0) != LUA_OK)
        {
            error = lua_tostring(state, -1) ? lua_tostring(state, -1) : "settings loader failed";
            lua_settop(state, top);
            return false;
        }
        if (lua_type(state, -2) != LUA_TTABLE)
        {
            error = lua_tostring(state, -1) ? lua_tostring(state, -1) : "Settings must return a table";
            lua_settop(state, top);
            return false;
        }
        lua_pop(state, 1);
        return true;
    }

    std::optional<TemplateInfo> ReadSettings(lua_State* state, std::string_view text, const std::string& chunkName, const std::string& templateName, std::string& error)
    {
        const int top = lua_gettop(state);
        if (!PushSettingsTable(state, text, chunkName, error))
            return std::nullopt;
        const int table = lua_gettop(state);
        const Reader settings(state, table);
        auto fail = [&](std::string message) -> std::optional<TemplateInfo>
        {
            error = std::move(message);
            lua_settop(state, top);
            return std::nullopt;
        };

        TemplateInfo info = DefaultInfo(templateName);
        info.HasSettings = true;
        info.SettingsText = std::string(text);
        const Value schemaValue = settings.Raw("SchemaVersion");
        const double schema = std::holds_alternative<std::monostate>(schemaValue) ? 1.0 : NumberOf(schemaValue).value_or(-1.0);
        if (schema != 1.0 && schema != 2.0)
            return fail("Unsupported settings SchemaVersion " + TextOf(schemaValue) + " (supported are 1 and 2)");
        info.SchemaVersion = static_cast<int>(schema);
        for (const char* key : { "Caption", "Shortcut", "SubMenuName", "Category", "Description", "Author", "Version", "Id" })
        {
            if (!settings.Check(key, LUA_TSTRING, "string", error))
                return fail(error);
        }
        for (const char* key : { "InSubMenu", "AllowUnsafeGlobals" })
        {
            if (!settings.Check(key, LUA_TBOOLEAN, "boolean", error))
                return fail(error);
        }
        for (const char* key : { "MenuOrder", "Order", "CategoryOrder" })
        {
            if (!settings.Check(key, LUA_TNUMBER, "number", error))
                return fail(error);
        }
        info.Caption = TrimText(settings.String("Caption").value_or(templateName));
        if (info.Caption.empty())
            return fail("Caption must not be empty");
        info.Shortcut = TrimText(settings.String("Shortcut").value_or(""));
        info.InSubMenu = settings.Bool("InSubMenu").value_or(true);
        info.Category = TrimText(settings.String("Category").value_or(settings.String("SubMenuName").value_or("")));
        info.ExplicitCategory = !info.Category.empty();
        info.SettingsHasComments = HasLuaComment(text);
        ReadCustomFields(state, table, info.CustomFields);
        info.CategoryOrder = settings.Number("CategoryOrder");
        info.Order = settings.Number("Order");
        if (!info.Order)
            info.Order = settings.Number("MenuOrder");
        info.Description = settings.String("Description").value_or("");
        info.Author = settings.String("Author").value_or("");
        info.Version = settings.String("Version").value_or("");
        info.AllowUnsafeGlobals = info.SchemaVersion == 1 || settings.Bool("AllowUnsafeGlobals").value_or(false);
        if (const auto id = settings.String("Id"))
        {
            static const std::regex pattern(R"(^[A-Za-z0-9._\-]+$)");
            if (!std::regex_match(*id, pattern))
                return fail("Id may only contain letters, digits, '.', '_' and '-'");
            info.Id = *id;
        }
        if (!settings.List("Tags", info.Tags, error) || !settings.List("Requires", info.Requires, error) || !settings.List("Optional", info.Optional, error)
            || !settings.List("Capabilities", info.Capabilities, error) || !settings.List("Architectures", info.Architectures, error))
            return fail(error);
        for (const std::string& architecture : info.Architectures)
        {
            if (architecture != "x86" && architecture != "x64")
                return fail("Architectures may only contain 'x86' and 'x64', not '" + architecture + "'");
        }
        if (!ReadInputs(state, table, info.Inputs, error))
            return fail(error);

        const bool nested = settings.Type("Memory") == LUA_TTABLE;
        if (nested)
            lua_getfield(state, table, "Memory");
        const int memoryTable = nested ? lua_gettop(state) : table;
        const Reader memory(state, memoryTable);
        auto readBool = [&](const char* key, std::optional<bool>& target) -> bool
        {
            for (const Reader* source : { &memory, &settings })
            {
                const int type = source->Type(key);
                if (type == LUA_TNIL)
                    continue;
                if (type != LUA_TBOOLEAN)
                {
                    error = std::string(key) + " must be a boolean";
                    return false;
                }
                target = source->Bool(key);
                return true;
            }
            return true;
        };
        auto readString = [&](const char* key, std::optional<std::string>& target) -> bool
        {
            for (const Reader* source : { &memory, &settings })
            {
                const int type = source->Type(key);
                if (type == LUA_TNIL)
                    continue;
                if (type != LUA_TSTRING)
                {
                    error = std::string(key) + " must be a string";
                    return false;
                }
                target = source->String(key);
                return true;
            }
            return true;
        };
        if (!readBool("AskForInjectionAddress", info.Memory.AskForInjectionAddress) || !readBool("AskForHookName", info.Memory.AskForHookName)
            || !readBool("AllocationNear", info.Memory.AllocationNear) || !readString("AppendToHookName", info.Memory.AppendToHookName)
            || !readString("DefaultHookName", info.Memory.DefaultHookName))
            return fail(error);
        for (const Reader* source : { &memory, &settings })
        {
            const int type = source->Type("AllocationSize");
            if (type == LUA_TNIL)
                continue;
            if (type != LUA_TSTRING && type != LUA_TNUMBER)
                return fail("AllocationSize must be a string or number");
            const Value size = source->Raw("AllocationSize");
            info.Memory.AllocationSize = std::holds_alternative<std::string>(size) ? std::get<std::string>(size) : NormalizeAllocationSize(size).value_or("");
            break;
        }
        lua_settop(state, top);
        return info;
    }

    std::string CategoryLabel(std::string_view segment, std::optional<double>* order)
    {
        static const std::regex prefix(R"(^\s*\[(\d+)\]\s*(.*)$)");
        std::match_results<std::string_view::const_iterator> match;
        if (std::regex_match(segment.begin(), segment.end(), match, prefix))
        {
            if (order)
                *order = std::strtod(match[1].str().c_str(), nullptr);
            return TrimText(match[2].str());
        }
        return TrimText(segment);
    }

    Library::Library(std::filesystem::path folder) : m_folder(std::move(folder))
    {
    }

    void Library::SetFolder(std::filesystem::path folder)
    {
        m_folder = std::move(folder);
        m_loaded = false;
    }

    std::uint64_t Library::Fingerprint() const
    {
        std::uint64_t hash = 1469598103934665603ull;
        std::error_code error;
        if (m_folder.empty() || !std::filesystem::is_directory(m_folder, error))
            return hash;
        for (std::filesystem::recursive_directory_iterator it(m_folder, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error))
        {
            if (error)
                break;
            hash = Mix(hash, PathText(it->path()));
            if (it->is_regular_file(error))
            {
                hash = Mix(hash, static_cast<std::uint64_t>(it->file_size(error)));
                hash = Mix(hash, static_cast<std::uint64_t>(it->last_write_time(error).time_since_epoch().count()));
            }
        }
        return hash;
    }

    void Library::Load(lua_State* state)
    {
        m_templates.clear();
        m_problems.clear();
        std::map<std::string, std::size_t> byId;

        for (const BuiltinFile& file : BuiltinFiles())
        {
            const std::string path = file.Path;
            if (!EndsWithNoCase(path, kScriptExtension) || path.find('/') != std::string::npos)
                continue;
            const std::string stem = path.substr(0, path.size() - kScriptExtension.size());
            if (IsHeaderName(stem))
                continue;
            const BuiltinFile* settingsFile = FindBuiltin(stem + std::string(kSettingsSuffix));
            std::string error;
            std::optional<TemplateInfo> info = settingsFile ? ReadSettings(state, settingsFile->Text(), "=built-in/" + stem + std::string(kSettingsSuffix), stem, error) : DefaultInfo(stem);
            if (!info)
            {
                m_problems.push_back({ "built-in/" + path, error });
                continue;
            }
            info->BuiltIn = true;
            info->Name = stem;
            info->BuiltinScript = file.Text();
            info->SourceName = "built-in/" + path;
            info->SettingsName = "built-in/" + stem + std::string(kSettingsSuffix);
            if (info->Category.empty())
                info->Category = "Manifold Engine";
            byId[info->Id] = m_templates.size();
            m_templates.push_back(std::move(*info));
        }

        std::error_code error;
        if (!m_folder.empty() && std::filesystem::is_directory(m_folder, error))
        {
            std::vector<std::filesystem::path> scripts;
            for (std::filesystem::recursive_directory_iterator it(m_folder, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error))
            {
                if (error)
                    break;
                if (it->is_directory(error) && EqualsNoCase(WideToUtf8(it->path().filename().wstring()), "Partials"))
                {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(error) && EndsWithNoCase(WideToUtf8(it->path().filename().wstring()), kScriptExtension))
                    scripts.push_back(it->path());
            }
            std::sort(scripts.begin(), scripts.end());
            std::set<std::string> userIds;
            for (const std::filesystem::path& script : scripts)
            {
                const std::string fileName = WideToUtf8(script.filename().wstring());
                const std::string stem = fileName.substr(0, fileName.size() - kScriptExtension.size());
                const std::filesystem::path relative = script.parent_path().lexically_relative(m_folder);
                const bool topLevel = relative.empty() || relative == ".";
                if (topLevel && IsHeaderName(stem))
                    continue;
                const std::filesystem::path settingsPath = script.parent_path() / Utf8ToWide(stem + std::string(kSettingsSuffix));
                std::optional<TemplateInfo> info;
                std::string message;
                if (std::filesystem::is_regular_file(settingsPath, error))
                {
                    const auto text = ReadFile(settingsPath);
                    if (!text)
                        message = "The settings file could not be read";
                    else
                        info = ReadSettings(state, *text, "@" + PathText(settingsPath), stem, message);
                }
                else
                    info = DefaultInfo(stem);
                if (!info)
                {
                    m_problems.push_back({ PathText(script), message });
                    continue;
                }
                info->Name = stem;
                info->ScriptPath = script;
                info->SourceName = PathText(script);
                if (info->HasSettings)
                {
                    info->SettingsPath = settingsPath;
                    info->SettingsName = PathText(settingsPath);
                }
                if (info->Category.empty())
                {
                    std::string category;
                    if (!topLevel)
                    {
                        for (const auto& part : relative)
                            category += (category.empty() ? "" : " > ") + WideToUtf8(part.wstring());
                    }
                    info->Category = category.empty() ? "Templates" : category;
                }
                if (!userIds.insert(info->Id).second)
                {
                    m_problems.push_back({ PathText(script), "Id '" + info->Id + "' is already used by another template" });
                    continue;
                }
                const auto builtin = byId.find(info->Id);
                if (builtin != byId.end())
                {
                    m_templates[builtin->second] = std::move(*info);
                    continue;
                }
                byId[info->Id] = m_templates.size();
                m_templates.push_back(std::move(*info));
            }
        }

        std::stable_sort(m_templates.begin(), m_templates.end(), [](const TemplateInfo& a, const TemplateInfo& b)
            {
                const double orderA = a.Order.value_or(std::numeric_limits<double>::infinity());
                const double orderB = b.Order.value_or(std::numeric_limits<double>::infinity());
                if (orderA != orderB)
                    return orderA < orderB;
                return Lower(a.Caption) < Lower(b.Caption);
            });
        m_fingerprint = Fingerprint();
        m_loaded = true;
    }

    bool Library::ReloadIfChanged(lua_State* state)
    {
        if (m_loaded && Fingerprint() == m_fingerprint)
            return false;
        Load(state);
        return true;
    }

    const TemplateInfo* Library::Find(std::string_view id) const
    {
        for (const TemplateInfo& info : m_templates)
        {
            if (info.Id == id)
                return &info;
        }
        return nullptr;
    }

    std::optional<SourceText> Library::Script(const TemplateInfo& info) const
    {
        if (info.BuiltIn)
            return SourceText{ info.SourceName, info.BuiltinScript };
        const auto text = ReadFile(info.ScriptPath);
        if (!text)
            return std::nullopt;
        return SourceText{ info.SourceName, *text };
    }

    std::optional<SourceText> Library::Partial(std::string_view name, std::vector<std::string>* searched) const
    {
        if (name.empty())
            return std::nullopt;
        std::string fileName(name);
        if (!EndsWithNoCase(fileName, kScriptExtension))
            fileName += ".CEA";
        std::error_code error;
        if (!m_folder.empty())
        {
            for (const std::filesystem::path& candidate : { m_folder / L"Partials" / Utf8ToWide(fileName), m_folder / Utf8ToWide(fileName) })
            {
                if (searched)
                    searched->push_back(PathText(candidate));
                if (std::filesystem::is_regular_file(candidate, error))
                {
                    if (const auto text = ReadFile(candidate))
                        return SourceText{ PathText(candidate), *text };
                }
            }
        }
        for (const std::string& candidate : { "Partials/" + fileName, fileName })
        {
            if (searched)
                searched->push_back("built-in/" + candidate);
            if (const BuiltinFile* file = FindBuiltin(candidate))
                return SourceText{ "built-in/" + std::string(file->Path), file->Text() };
        }
        return std::nullopt;
    }

    std::optional<SourceText> Library::Header() const
    {
        std::error_code error;
        if (!m_folder.empty())
        {
            const std::filesystem::path candidate = m_folder / L"Header.CEA";
            if (std::filesystem::is_regular_file(candidate, error))
            {
                if (const auto text = ReadFile(candidate))
                    return SourceText{ PathText(candidate), *text };
            }
        }
        if (const BuiltinFile* file = FindBuiltin("Header.CEA"))
            return SourceText{ "built-in/Header.CEA", file->Text() };
        return std::nullopt;
    }
}
