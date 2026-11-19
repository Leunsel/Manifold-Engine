#include "Lua/LuaUnreal.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaArgs.h"

#include "lua.hpp"

#include <algorithm>
#include <functional>

namespace Manifold::Lua
{
    namespace
    {
        using Target::UnrealField;
        using Target::UnrealRuntime;

        std::shared_ptr<const UnrealRuntime> Runtime(lua_State* state, Status& error)
        {
            Engine& engine = Owner(state);
            if (!engine.HasProcess())
            {
                error = Status::Fail(ErrorCode::NoProcess);
                return nullptr;
            }
            auto runtime = engine.Unreal().Runtime();
            if (!runtime)
                error = Status::Fail(ErrorCode::UnrealNotResolved, engine.Unreal().StatusText());
            return runtime;
        }

        template <class Body>
        int WithRuntime(lua_State* state, Body body)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            return body(*runtime);
        }

        void PushAddress(lua_State* state, std::uint64_t value)
        {
            if (value)
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            else
                lua_pushnil(state);
        }

        void SetInteger(lua_State* state, const char* key, lua_Integer value)
        {
            lua_pushinteger(state, value);
            lua_setfield(state, -2, key);
        }

        void SetString(lua_State* state, const char* key, const std::string& value)
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }

        void SetBoolean(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        void SetAddress(lua_State* state, const char* key, std::uint64_t value)
        {
            if (!value)
                return;
            lua_pushinteger(state, static_cast<lua_Integer>(value));
            lua_setfield(state, -2, key);
        }

        std::optional<std::uint64_t> TypeArgument(lua_State* state, int index, const UnrealRuntime& runtime, Status& error)
        {
            if (lua_type(state, index) == LUA_TSTRING)
            {
                const std::string name = lua_tostring(state, index);
                const std::uint64_t type = runtime.Type(name);
                if (!type)
                {
                    error = Status::Fail(ErrorCode::UnrealUnknownType, name);
                    return std::nullopt;
                }
                return type;
            }
            return ResolveAddress(state, index, error);
        }

        void PushField(lua_State* state, const UnrealRuntime& runtime, const UnrealField& field)
        {
            lua_createtable(state, 0, 14);
            SetString(state, "name", field.Name);
            SetString(state, "type", field.Type);
            SetString(state, "typeText", runtime.TypeText(field));
            SetString(state, "owner", field.Owner);
            SetInteger(state, "offset", field.Offset);
            SetInteger(state, "start", field.Start());
            SetInteger(state, "size", field.Size);
            SetInteger(state, "dim", field.Dim);
            SetInteger(state, "flags", static_cast<lua_Integer>(field.Flags));
            SetAddress(state, "address", field.Address);
            SetAddress(state, "inner", field.Inner);
            SetAddress(state, "metaClass", field.MetaClass);
            if (field.Bitfield)
            {
                SetInteger(state, "byteOffset", field.ByteOffset);
                SetInteger(state, "bitMask", field.Mask);
            }
        }

        void PushFields(lua_State* state, const UnrealRuntime& runtime, const std::vector<UnrealField>& fields)
        {
            lua_createtable(state, static_cast<int>(fields.size()), 0);
            for (std::size_t i = 0; i < fields.size(); ++i)
            {
                PushField(state, runtime, fields[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        std::vector<std::string> StringList(lua_State* state, int index)
        {
            std::vector<std::string> list;
            if (!lua_istable(state, index))
                return list;
            const lua_Integer length = luaL_len(state, index);
            for (lua_Integer i = 1; i <= length; ++i)
            {
                lua_rawgeti(state, index, i);
                if (const char* text = lua_tostring(state, -1))
                    list.emplace_back(text);
                lua_pop(state, 1);
            }
            return list;
        }

        bool FieldBoolean(lua_State* state, int index, const char* key, bool& target)
        {
            lua_getfield(state, index, key);
            const bool present = !lua_isnil(state, -1);
            if (present)
                target = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            return present;
        }

        bool FieldAddress(lua_State* state, int index, const char* key, std::uint64_t& target, Status& error)
        {
            lua_getfield(state, index, key);
            bool ok = true;
            if (!lua_isnil(state, -1))
            {
                if (lua_toboolean(state, -1) == 0)
                {
                    target = 0;
                }
                else if (const auto value = ResolveAddress(state, lua_gettop(state), error))
                {
                    target = *value;
                }
                else
                {
                    ok = false;
                }
            }
            lua_pop(state, 1);
            return ok;
        }

        std::vector<Target::UnrealPattern> Patterns(lua_State* state, int index)
        {
            std::vector<Target::UnrealPattern> list;
            if (!lua_istable(state, index))
                return list;
            const lua_Integer length = luaL_len(state, index);
            for (lua_Integer i = 1; i <= length; ++i)
            {
                lua_rawgeti(state, index, i);
                Target::UnrealPattern pattern;
                if (lua_type(state, -1) == LUA_TSTRING)
                {
                    pattern.Bytes = lua_tostring(state, -1);
                }
                else if (lua_istable(state, -1))
                {
                    const int entry = lua_gettop(state);
                    lua_getfield(state, entry, "pattern");
                    if (const char* text = lua_tostring(state, -1))
                        pattern.Bytes = text;
                    lua_getfield(state, entry, "offset");
                    pattern.Offset = static_cast<int>(lua_tointeger(state, -1));
                    lua_getfield(state, entry, "length");
                    pattern.Length = static_cast<int>(lua_tointeger(state, -1));
                    lua_getfield(state, entry, "relative");
                    pattern.Relative = lua_isnil(state, -1) || lua_toboolean(state, -1) != 0;
                    lua_getfield(state, entry, "add");
                    pattern.Add = static_cast<std::int64_t>(lua_tointeger(state, -1));
                    lua_getfield(state, entry, "module");
                    if (const char* text = lua_tostring(state, -1))
                        pattern.Module = text;
                    lua_settop(state, entry);
                }
                lua_pop(state, 1);
                if (!pattern.Bytes.empty())
                    list.push_back(std::move(pattern));
            }
            return list;
        }

        void PushPatterns(lua_State* state, const std::vector<Target::UnrealPattern>& list)
        {
            lua_createtable(state, static_cast<int>(list.size()), 0);
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                const Target::UnrealPattern& p = list[i];
                lua_createtable(state, 0, 6);
                SetString(state, "pattern", p.Bytes);
                SetInteger(state, "offset", p.Offset);
                SetInteger(state, "length", p.Length);
                SetBoolean(state, "relative", p.Relative);
                SetInteger(state, "add", static_cast<lua_Integer>(p.Add));
                if (!p.Module.empty())
                    SetString(state, "module", p.Module);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        const char* StateName(UnrealState state)
        {
            switch (state)
            {
            case UnrealState::NotUnreal: return "notUnreal";
            case UnrealState::Resolving: return "resolving";
            case UnrealState::Ready: return "ready";
            case UnrealState::Failed: return "failed";
            case UnrealState::Stopped: return "stopped";
            default: return "idle";
            }
        }

        int Status_(lua_State* state)
        {
            UnrealSession& session = Owner(state).Unreal();
            lua_createtable(state, 0, 10);
            SetString(state, "state", StateName(session.State()));
            SetString(state, "text", session.StatusText());
            if (!session.Failure().empty())
                SetString(state, "failure", session.Failure());
            if (const auto runtime = session.Runtime())
            {
                const Target::UnrealSummary& summary = runtime->Summary();
                SetInteger(state, "objects", runtime->Count());
                SetInteger(state, "types", static_cast<lua_Integer>(summary.TypeCount));
                SetAddress(state, "namePool", summary.NamePool);
                SetAddress(state, "objectArray", summary.Objects);
                SetBoolean(state, "legacyNames", summary.LegacyNames);
                SetString(state, "objectArraySource", summary.ObjectsSource);
                lua_pushnumber(state, summary.Seconds);
                lua_setfield(state, -2, "seconds");
                SetBoolean(state, "doubleVectors", summary.Layout.Component == 8);
            }
            return 1;
        }

        int Start(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.HasProcess())
                return PushFailure(state, ErrorCode::NoProcess);
            if (lua_toboolean(state, 1))
            {
                const Status status = engine.Unreal().ResolveNow();
                if (!status)
                    return PushFailure(state, status);
            }
            else
            {
                engine.Unreal().Start(false);
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int Stop(lua_State* state)
        {
            Owner(state).Unreal().Stop();
            lua_pushboolean(state, 1);
            return 1;
        }

        int Configure(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TTABLE);
            Target::UnrealConfig config = Owner(state).Unreal().Config();
            FieldBoolean(state, 1, "autoStart", config.AutoStart);
            FieldBoolean(state, 1, "dissect", config.Dissect);
            FieldBoolean(state, 1, "names", config.Names);
            FieldBoolean(state, 1, "symbols", config.Symbols);
            FieldBoolean(state, 1, "types", config.Types);
            FieldBoolean(state, 1, "legacyNames", config.LegacyNames);
            Status error;
            if (!FieldAddress(state, 1, "namePool", config.NamePool, error) || !FieldAddress(state, 1, "objectArray", config.Objects, error))
                return PushFailure(state, error);
            lua_getfield(state, 1, "layout");
            if (lua_istable(state, -1))
            {
                const int table = lua_gettop(state);
                lua_pushnil(state);
                while (lua_next(state, table) != 0)
                {
                    if (lua_type(state, -2) == LUA_TSTRING)
                    {
                        const std::string key = lua_tostring(state, -2);
                        if (lua_isinteger(state, -1))
                            config.Layout[key] = static_cast<int>(lua_tointeger(state, -1));
                        else if (lua_isboolean(state, -1))
                            config.Layout[key] = lua_toboolean(state, -1);
                        else if (lua_toboolean(state, -1) == 0)
                            config.Layout.erase(key);
                    }
                    lua_pop(state, 1);
                }
            }
            else if (lua_isboolean(state, -1) && !lua_toboolean(state, -1))
            {
                config.Layout.clear();
            }
            lua_pop(state, 1);
            lua_getfield(state, 1, "patterns");
            if (lua_istable(state, -1))
            {
                const int table = lua_gettop(state);
                lua_getfield(state, table, "names");
                if (!lua_isnil(state, -1))
                    config.NamePatterns = Patterns(state, lua_gettop(state));
                lua_pop(state, 1);
                lua_getfield(state, table, "objects");
                if (!lua_isnil(state, -1))
                    config.ObjectPatterns = Patterns(state, lua_gettop(state));
                lua_pop(state, 1);
            }
            else if (lua_isboolean(state, -1) && !lua_toboolean(state, -1))
            {
                config.NamePatterns.clear();
                config.ObjectPatterns.clear();
            }
            lua_pop(state, 1);
            Owner(state).Unreal().Config() = config;
            lua_pushboolean(state, 1);
            return 1;
        }

        int Config(lua_State* state)
        {
            const Target::UnrealConfig& config = Owner(state).Unreal().Config();
            lua_createtable(state, 0, 12);
            SetBoolean(state, "autoStart", config.AutoStart);
            SetBoolean(state, "dissect", config.Dissect);
            SetBoolean(state, "names", config.Names);
            SetBoolean(state, "symbols", config.Symbols);
            SetBoolean(state, "types", config.Types);
            SetBoolean(state, "legacyNames", config.LegacyNames);
            SetAddress(state, "namePool", config.NamePool);
            SetAddress(state, "objectArray", config.Objects);
            lua_createtable(state, 0, static_cast<int>(config.Layout.size()));
            for (const auto& [key, value] : config.Layout)
                SetInteger(state, key.c_str(), value);
            lua_setfield(state, -2, "layout");
            lua_createtable(state, 0, 2);
            PushPatterns(state, config.NamePatterns);
            lua_setfield(state, -2, "names");
            PushPatterns(state, config.ObjectPatterns);
            lua_setfield(state, -2, "objects");
            lua_setfield(state, -2, "patterns");
            return 1;
        }

        int Layout(lua_State* state)
        {
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    const Target::UnrealLayout& l = runtime.Layout();
                    lua_createtable(state, 0, 24);
                    SetBoolean(state, "chunked", l.Chunked);
                    SetInteger(state, "itemSize", l.ItemSize);
                    SetInteger(state, "itemOffset", l.ItemOffset);
                    SetInteger(state, "index", l.Index);
                    SetInteger(state, "class", l.Class);
                    SetInteger(state, "name", l.Name);
                    SetInteger(state, "outer", l.Outer);
                    SetInteger(state, "super", l.Super);
                    SetInteger(state, "childProperties", l.ChildProperties);
                    SetInteger(state, "structSize", l.StructSize);
                    SetInteger(state, "children", l.Children);
                    SetInteger(state, "childNext", l.ChildNext);
                    SetInteger(state, "functionFlags", l.FunctionFlags);
                    SetBoolean(state, "fField", l.FField);
                    SetInteger(state, "fieldClass", l.FieldClass);
                    SetInteger(state, "fieldName", l.FieldName);
                    SetInteger(state, "next", l.Next);
                    SetInteger(state, "offset", l.Offset);
                    SetInteger(state, "size", l.Size);
                    SetInteger(state, "extra", l.Extra);
                    SetInteger(state, "component", l.Component);
                    return 1;
                });
        }

        int Count(lua_State* state)
        {
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    lua_pushinteger(state, runtime.Count());
                    return 1;
                });
        }

        int ObjectAt(lua_State* state)
        {
            const lua_Integer index = luaL_checkinteger(state, 1);
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    PushAddress(state, runtime.ObjectAt(static_cast<int>(index)).value_or(0));
                    return 1;
                });
        }

        int AddressQuery(lua_State* state, const std::function<int(const UnrealRuntime&, std::uint64_t)>& body)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            const auto address = ResolveAddress(state, 1, error);
            if (!address)
                return PushFailure(state, error);
            return body(*runtime, *address);
        }

        int TypeQuery(lua_State* state, const std::function<int(const UnrealRuntime&, std::uint64_t)>& body)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            const auto type = TypeArgument(state, 1, *runtime, error);
            if (!type)
                return PushFailure(state, error);
            return body(*runtime, *type);
        }

        int PushOptionalString(lua_State* state, const std::optional<std::string>& text)
        {
            if (text)
                lua_pushlstring(state, text->data(), text->size());
            else
                lua_pushnil(state);
            return 1;
        }

        int IsObject(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address)
                {
                    lua_pushboolean(state, runtime.IsObject(address) ? 1 : 0);
                    return 1;
                });
        }

        int Name(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address) { return PushOptionalString(state, runtime.NameAt(address)); });
        }

        int NameOf(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address) { return PushOptionalString(state, runtime.NameOf(address)); });
        }

        int FullName(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address)
                {
                    const std::string text = runtime.FullName(address);
                    lua_pushlstring(state, text.data(), text.size());
                    return 1;
                });
        }

        int ClassOf(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address)
                {
                    PushAddress(state, runtime.ClassOf(address));
                    return 1;
                });
        }

        int Type(lua_State* state)
        {
            const std::string name = luaL_checkstring(state, 1);
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    PushAddress(state, runtime.Type(name));
                    return 1;
                });
        }

        int Types(lua_State* state)
        {
            const std::string filter = luaL_optstring(state, 1, "");
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    lua_newtable(state);
                    lua_Integer index = 1;
                    for (const Target::UnrealType& type : runtime.Types())
                    {
                        if (!filter.empty() && !ContainsNoCase(type.Name, filter))
                            continue;
                        lua_createtable(state, 0, 3);
                        SetString(state, "name", type.Name);
                        SetString(state, "kind", Target::UnrealKindName(type.Kind));
                        SetAddress(state, "address", type.Object);
                        lua_rawseti(state, -2, index++);
                    }
                    return 1;
                });
        }

        int Super(lua_State* state)
        {
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    PushAddress(state, runtime.HasStructLayout(type) ? runtime.SuperOf(type) : 0);
                    return 1;
                });
        }

        int Size(lua_State* state)
        {
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    if (const auto size = runtime.HasStructLayout(type) ? runtime.StructSize(type) : std::nullopt)
                        lua_pushinteger(state, *size);
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int CppName(lua_State* state)
        {
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    const std::string text = runtime.CppName(type);
                    lua_pushlstring(state, text.data(), text.size());
                    return 1;
                });
        }

        int Fields(lua_State* state)
        {
            const bool own = lua_toboolean(state, 2) != 0;
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    PushFields(state, runtime, runtime.HasStructLayout(type) ? runtime.Fields(type, own) : std::vector<Target::UnrealField>{});
                    return 1;
                });
        }

        int Field(lua_State* state)
        {
            const std::string name = luaL_checkstring(state, 2);
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    const auto field = runtime.HasStructLayout(type) ? runtime.FindField(type, name) : std::nullopt;
                    if (!field)
                        return PushFailure(state, ErrorCode::UnrealNoProperty, runtime.NameOf(type).value_or("?") + "." + name);
                    PushField(state, runtime, *field);
                    return 1;
                });
        }

        int Functions(lua_State* state)
        {
            const bool inherited = lua_toboolean(state, 2) != 0;
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    const auto functions = runtime.HasStructLayout(type) ? runtime.Functions(type, inherited) : std::vector<Target::UnrealFunction>{};
                    lua_createtable(state, static_cast<int>(functions.size()), 0);
                    for (std::size_t i = 0; i < functions.size(); ++i)
                    {
                        const Target::UnrealFunction& f = functions[i];
                        lua_createtable(state, 0, 5);
                        SetString(state, "name", f.Name);
                        SetString(state, "owner", f.Owner);
                        SetAddress(state, "address", f.Address);
                        SetInteger(state, "flags", f.Flags);
                        PushFields(state, runtime, f.Parameters);
                        lua_setfield(state, -2, "parameters");
                        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                    }
                    return 1;
                });
        }

        int EnumEntries(lua_State* state)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            std::uint64_t object = 0;
            if (lua_type(state, 1) == LUA_TSTRING)
            {
                const std::string name = lua_tostring(state, 1);
                object = runtime->Enum(name);
                if (!object)
                    return PushFailure(state, ErrorCode::UnrealUnknownType, name);
            }
            else if (const auto address = ResolveAddress(state, 1, error))
            {
                object = *address;
            }
            else
            {
                return PushFailure(state, error);
            }
            const auto entries = runtime->EnumEntries(object);
            lua_createtable(state, static_cast<int>(entries.size()), 0);
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                lua_createtable(state, 0, 2);
                SetString(state, "name", entries[i].Name);
                SetInteger(state, "value", static_cast<lua_Integer>(entries[i].Value));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int Instances(lua_State* state)
        {
            const lua_Integer limit = luaL_optinteger(state, 2, 0);
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    const auto found = runtime.Instances(type, static_cast<std::size_t>(std::max<lua_Integer>(limit, 0)));
                    lua_createtable(state, static_cast<int>(found.size()), 0);
                    for (std::size_t i = 0; i < found.size(); ++i)
                    {
                        lua_pushinteger(state, static_cast<lua_Integer>(found[i]));
                        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                    }
                    return 1;
                });
        }

        int Find(lua_State* state)
        {
            const std::string text = luaL_checkstring(state, 1);
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    PushAddress(state, runtime.Find(text).value_or(0));
                    return 1;
                });
        }

        int OwnerOf(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address)
                {
                    const auto owner = runtime.OwnerOf(address);
                    if (!owner)
                        return PushFailure(state, ErrorCode::UnrealNoObject, Hex(address));
                    lua_pushinteger(state, static_cast<lua_Integer>(owner->first));
                    PushAddress(state, owner->second);
                    return 2;
                });
        }

        int Whois(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address)
                {
                    const auto text = runtime.Whois(address);
                    if (!text)
                        return PushFailure(state, ErrorCode::UnrealNoObject, Hex(address));
                    lua_pushlstring(state, text->data(), text->size());
                    return 1;
                });
        }

        int Locate(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t address)
                {
                    const auto hit = runtime.Locate(address);
                    if (!hit)
                        return PushFailure(state, ErrorCode::UnrealNoObject, Hex(address));
                    lua_createtable(state, 0, 6);
                    SetAddress(state, "object", hit->Object);
                    SetAddress(state, "class", hit->Class);
                    SetInteger(state, "offset", static_cast<lua_Integer>(address - hit->Object));
                    SetString(state, "path", Join(hit->Path, "."));
                    if (hit->Field)
                    {
                        PushField(state, runtime, *hit->Field);
                        lua_setfield(state, -2, "field");
                        SetInteger(state, "within", hit->Within);
                    }
                    SetAddress(state, "struct", hit->Struct);
                    return 1;
                });
        }

        int World(lua_State* state, std::uint64_t (UnrealRuntime::*get)() const)
        {
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    PushAddress(state, (runtime.*get)());
                    return 1;
                });
        }

        int EngineObject(lua_State* state) { return World(state, &UnrealRuntime::Engine); }
        int WorldObject(lua_State* state) { return World(state, &UnrealRuntime::World); }
        int ControllerObject(lua_State* state) { return World(state, &UnrealRuntime::Controller); }
        int PawnObject(lua_State* state) { return World(state, &UnrealRuntime::Pawn); }

        int IsOffline(lua_State* state)
        {
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    const auto offline = runtime.IsOffline();
                    if (offline)
                        lua_pushboolean(state, *offline ? 1 : 0);
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int Walk(lua_State* state)
        {
            return AddressQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t object)
                {
                    const std::vector<std::string> steps = StringList(state, 2);
                    const std::string root = luaL_optstring(state, 3, Hex(object).c_str());
                    const Result<Target::UnrealWalk> walk = runtime.Walk(object, root, steps);
                    if (!walk)
                        return PushFailure(state, walk.Error());
                    lua_pushinteger(state, static_cast<lua_Integer>(walk->Address));
                    lua_pushlstring(state, walk->Expression.data(), walk->Expression.size());
                    if (walk->Field)
                        PushField(state, runtime, *walk->Field);
                    else
                        lua_pushnil(state);
                    return 3;
                });
        }

        int Dump(lua_State* state)
        {
            return TypeQuery(state, [&](const UnrealRuntime& runtime, std::uint64_t type)
                {
                    if (!runtime.HasStructLayout(type))
                        return PushFailure(state, ErrorCode::UnrealUnknownType, runtime.NameOf(type).value_or(Hex(type)));
                    const std::uint64_t super = runtime.SuperOf(type);
                    std::vector<std::string> lines;
                    lines.push_back(Format("%s%s  // size 0x%X", runtime.CppName(type).c_str(), super ? (" : " + runtime.CppName(super)).c_str() : "",
                        static_cast<unsigned>(runtime.StructSize(type).value_or(0))));
                    for (const UnrealField& f : runtime.Fields(type))
                    {
                        const std::string mask = f.MaskedBit() ? Format(" : 1 (mask %02X)", f.Mask) : std::string();
                        lines.push_back(Format("  +%04X  %-32s %s%s%s  (%s)", static_cast<unsigned>(f.Start()), runtime.TypeText(f).c_str(), f.Name.c_str(),
                            f.Dim > 1 ? ("[" + std::to_string(f.Dim) + "]").c_str() : "", mask.c_str(), f.Owner.c_str()));
                    }
                    lua_createtable(state, static_cast<int>(lines.size()), 0);
                    for (std::size_t i = 0; i < lines.size(); ++i)
                    {
                        lua_pushlstring(state, lines[i].data(), lines[i].size());
                        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                    }
                    return 1;
                });
        }

        int Claims(lua_State* state)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            const auto address = runtime ? ResolveAddress(state, 1, error) : std::nullopt;
            bool claimed = false;
            if (runtime && address)
                claimed = Owner(state).Unreal().ArrayAt(*address).has_value() || runtime->OwnerOf(*address).has_value();
            lua_pushboolean(state, claimed ? 1 : 0);
            return 1;
        }

        int Symbol(lua_State* state)
        {
            const std::string type = luaL_checkstring(state, 1);
            const std::string field = luaL_checkstring(state, 2);
            return WithRuntime(state, [&](const UnrealRuntime& runtime)
                {
                    const std::uint64_t owner = runtime.Type(type);
                    if (!owner)
                        return PushFailure(state, ErrorCode::UnrealUnknownType, type);
                    const auto f = runtime.FindField(owner, field);
                    if (!f)
                        return PushFailure(state, ErrorCode::UnrealNoProperty, type + "." + field);
                    const std::string symbol = Target::UnrealRuntime::OffsetSymbol(f->Owner, f->Name);
                    lua_pushlstring(state, symbol.data(), symbol.size());
                    lua_pushinteger(state, f->Start());
                    return 2;
                });
        }
    }

    void RegisterUnrealNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "unrealStatus", &Status_ },
            { "unrealStart", &Start },
            { "unrealStop", &Stop },
            { "unrealConfigure", &Configure },
            { "unrealConfig", &Config },
            { "unrealLayout", &Layout },
            { "unrealCount", &Count },
            { "unrealObjectAt", &ObjectAt },
            { "unrealIsObject", &IsObject },
            { "unrealName", &Name },
            { "unrealNameOf", &NameOf },
            { "unrealFullName", &FullName },
            { "unrealClassOf", &ClassOf },
            { "unrealType", &Type },
            { "unrealTypes", &Types },
            { "unrealSuper", &Super },
            { "unrealSize", &Size },
            { "unrealCppName", &CppName },
            { "unrealFields", &Fields },
            { "unrealField", &Field },
            { "unrealFunctions", &Functions },
            { "unrealEnum", &EnumEntries },
            { "unrealInstances", &Instances },
            { "unrealFind", &Find },
            { "unrealOwner", &OwnerOf },
            { "unrealWhois", &Whois },
            { "unrealLocate", &Locate },
            { "unrealEngine", &EngineObject },
            { "unrealWorld", &WorldObject },
            { "unrealController", &ControllerObject },
            { "unrealPawn", &PawnObject },
            { "unrealIsOffline", &IsOffline },
            { "unrealWalk", &Walk },
            { "unrealDump", &Dump },
            { "unrealClaims", &Claims },
            { "unrealSymbol", &Symbol },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
