#include "Lua/LuaRtti.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Table/RttiStructures.h"
#include "Target/RttiMsvc.h"

#include "lua.hpp"

#include <algorithm>

namespace Manifold::Lua
{
    namespace
    {
        using Target::RttiObject;
        using Target::RttiRuntime;

        std::shared_ptr<RttiRuntime> Runtime(lua_State* state, Status& error)
        {
            auto runtime = Owner(state).Rtti().Runtime();
            if (!runtime)
                error = Status::Fail(ErrorCode::NoProcess);
            return runtime;
        }

        void SetInteger(lua_State* state, const char* key, std::uint64_t value)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(value));
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

        int PushObject(lua_State* state, const std::optional<RttiObject>& object)
        {
            if (!object)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_createtable(state, 0, 8);
            SetInteger(state, "address", object->Address);
            SetInteger(state, "start", object->Start);
            SetInteger(state, "offset", object->Offset);
            SetString(state, "class", object->Class->Name);
            SetString(state, "decorated", object->Class->Decorated);
            SetInteger(state, "vtable", object->VTable);
            SetString(state, "module", object->Class->Module);
            SetBoolean(state, "viaVirtualBase", object->Via != 0);
            return 1;
        }

        void PushAddresses(lua_State* state, const std::vector<std::uint64_t>& list)
        {
            lua_createtable(state, static_cast<int>(list.size()), 0);
            for (std::size_t i = 0; i < list.size(); ++i)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(list[i]));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        template <class Body>
        int AddressQuery(lua_State* state, Body body)
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

        template <class Body>
        int NameQuery(lua_State* state, Body body)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            return body(*runtime, std::string(luaL_checkstring(state, 1)));
        }

        int ClassName(lua_State* state)
        {
            return AddressQuery(state, [&](RttiRuntime& runtime, std::uint64_t address)
                {
                    if (const auto object = runtime.ObjectAt(address))
                        lua_pushlstring(state, object->Class->Name.data(), object->Class->Name.size());
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int ObjectAt(lua_State* state)
        {
            return AddressQuery(state, [&](RttiRuntime& runtime, std::uint64_t address)
                {
                    auto object = runtime.ObjectAt(address);
                    if (object)
                        object->Offset = address - object->Start;
                    return PushObject(state, object);
                });
        }

        int OwnerOf(lua_State* state)
        {
            const lua_Integer window = luaL_optinteger(state, 2, static_cast<lua_Integer>(RttiRuntime::kOwnerWindow));
            return AddressQuery(state, [&](RttiRuntime& runtime, std::uint64_t address)
                {
                    return PushObject(state, runtime.Owner(address, static_cast<std::uint64_t>(std::clamp<lua_Integer>(window, 0, 0x1000000))));
                });
        }

        int Whois(lua_State* state)
        {
            return AddressQuery(state, [&](RttiRuntime& runtime, std::uint64_t address)
                {
                    const auto text = runtime.Whois(address);
                    if (!text)
                        return PushFailure(state, ErrorCode::RttiNoObject, Hex(address));
                    lua_pushlstring(state, text->data(), text->size());
                    return 1;
                });
        }

        int Bases(lua_State* state)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            Target::RttiClassPtr type;
            std::optional<std::uint64_t> start;
            if (lua_type(state, 1) == LUA_TSTRING)
            {
                const std::string name = lua_tostring(state, 1);
                const auto primary = runtime->Primary(name);
                if (!primary)
                    return PushFailure(state, ErrorCode::RttiUnknownClass, name);
                type = primary->second;
            }
            else
            {
                const auto address = ResolveAddress(state, 1, error);
                if (!address)
                    return PushFailure(state, error);
                const auto object = runtime->Locate(*address);
                if (!object)
                    return PushFailure(state, ErrorCode::RttiNoObject, Hex(*address));
                type = object->Class;
                start = object->Start;
            }
            const Target::RttiBases bases = runtime->Bases(*type);
            lua_createtable(state, static_cast<int>(bases->size()), 0);
            lua_Integer index = 1;
            for (const Target::RttiBase& base : *bases)
            {
                lua_createtable(state, 0, 5);
                SetString(state, "name", base.Name);
                if (!base.Decorated.empty())
                    SetString(state, "decorated", base.Decorated);
                SetInteger(state, "depth", static_cast<std::uint64_t>(base.Depth));
                if (const auto offset = runtime->BaseOffset(base, start))
                {
                    lua_pushinteger(state, static_cast<lua_Integer>(*offset));
                    lua_setfield(state, -2, "offset");
                }
                SetBoolean(state, "virtual", base.Virtual);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int VTable(lua_State* state)
        {
            const std::string which = luaL_optstring(state, 2, "");
            return NameQuery(state, [&](RttiRuntime& runtime, const std::string& name)
                {
                    if (const auto primary = runtime.Primary(name, which))
                        lua_pushinteger(state, static_cast<lua_Integer>(primary->first));
                    else
                        lua_pushnil(state);
                    return 1;
                });
        }

        int VTables(lua_State* state)
        {
            return NameQuery(state, [&](RttiRuntime& runtime, const std::string& name)
                {
                    const std::vector<Target::RttiTable> tables = runtime.Tables(name);
                    if (tables.empty())
                        return PushFailure(state, ErrorCode::RttiUnknownClass, name);
                    lua_createtable(state, static_cast<int>(tables.size()), 0);
                    lua_Integer index = 1;
                    for (const Target::RttiTable& table : tables)
                    {
                        lua_createtable(state, 0, 4);
                        SetInteger(state, "address", table.Address);
                        SetInteger(state, "offset", table.Offset);
                        SetString(state, "symbol", table.Symbol);
                        SetString(state, "module", table.Module);
                        lua_rawseti(state, -2, index++);
                    }
                    return 1;
                });
        }

        int Instances(lua_State* state)
        {
            const lua_Integer limit = luaL_optinteger(state, 2, static_cast<lua_Integer>(RttiRuntime::kInstanceLimit));
            return NameQuery(state, [&](RttiRuntime& runtime, const std::string& name)
                {
                    const auto list = runtime.Instances(name, static_cast<std::size_t>(std::clamp<lua_Integer>(limit, 1, static_cast<lua_Integer>(RttiRuntime::kScanLimit))));
                    if (!list)
                        return PushFailure(state, ErrorCode::RttiUnknownClass, name);
                    PushAddresses(state, *list);
                    return 1;
                });
        }

        int Classes(lua_State* state)
        {
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            const std::vector<std::string> names = runtime->Classes();
            lua_createtable(state, static_cast<int>(names.size()), 0);
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                lua_pushlstring(state, names[i].data(), names[i].size());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int Demangle(lua_State* state)
        {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 1, &length);
            const std::string name = Target::MsvcDemangle(std::string_view(text, length));
            lua_pushlstring(state, name.data(), name.size());
            return 1;
        }

        int Mangle(lua_State* state)
        {
            const std::vector<std::string> names = Target::MsvcMangle(luaL_checkstring(state, 1));
            if (names.empty())
            {
                lua_pushnil(state);
                return 1;
            }
            lua_createtable(state, static_cast<int>(names.size()), 0);
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                lua_pushlstring(state, names[i].data(), names[i].size());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int Dissect(lua_State* state)
        {
            return AddressQuery(state, [&](RttiRuntime& runtime, std::uint64_t address)
                {
                    const auto object = runtime.Locate(address);
                    if (!object)
                        return PushFailure(state, ErrorCode::RttiNoObject, Hex(address));
                    Engine& engine = Owner(state);
                    Table::StructureStore& store = engine.Structures();
                    const std::string& name = object->Class->Name;
                    if (!store.Find(name))
                    {
                        Table::RttiFill fill = Table::FillRttiObject(runtime, *object, Table::kRttiGuessSize, Table::StructureNames(store));
                        if (fill.Handled)
                            Table::ApplyRttiFill(store, store.Add(name), std::move(fill));
                    }
                    engine.Request(UiRequest{ UiRequestKind::StructureDissect, object->Start, store.Find(name) ? name : std::string() });
                    lua_pushboolean(state, 1);
                    return 1;
                });
        }

        int Label(lua_State* state)
        {
            Table::StructDefinition* definition = StructureObject(state, 1);
            if (!definition)
                return PushFailure(state, ErrorCode::LuaApiMisuse, "manifold.rtti.label needs a structure object, such as createStructure gives");
            Status error;
            const auto runtime = Runtime(state, error);
            if (!runtime)
                return PushFailure(state, error);
            const auto address = ResolveAddress(state, 2, error);
            if (!address)
                return PushFailure(state, error);
            Table::StructureStore& store = Owner(state).Structures();
            Table::RttiFill fill = Table::LabelFromRtti(*runtime, definition->Fields, *address, Table::StructureNames(store));
            const int labelled = fill.Labelled;
            if (fill.Handled)
                Table::ApplyRttiFill(store, *definition, std::move(fill));
            lua_pushinteger(state, labelled);
            return 1;
        }

        int StatusOf(lua_State* state)
        {
            Engine& engine = Owner(state);
            const auto runtime = engine.Rtti().Runtime();
            lua_createtable(state, 0, 7);
            SetString(state, "text", runtime ? runtime->StatusText() : std::string("no process"));
            SetBoolean(state, "dissect", engine.Rtti().Enabled());
            if (runtime)
            {
                const Target::RttiCounts counts = runtime->Counts();
                SetInteger(state, "process", Raw(runtime->ProcessHandle()->Id()));
                SetBoolean(state, "is64Bit", runtime->PointerSize() == 8);
                SetInteger(state, "modules", counts.Modules);
                SetInteger(state, "classes", counts.Classes);
                SetInteger(state, "vtables", counts.Tables);
            }
            return 1;
        }

        int Reset(lua_State* state)
        {
            Owner(state).Rtti().Reset();
            lua_pushboolean(state, 1);
            return 1;
        }

        int Claims(lua_State* state)
        {
            Engine& engine = Owner(state);
            Status ignored;
            const auto address = engine.HasProcess() ? ResolveAddress(state, 1, ignored) : std::nullopt;
            const bool claimed = address && engine.Rtti().Enabled() && engine.Rtti().Names(*address) && !engine.Rtti().OthersClaim(*address);
            lua_pushboolean(state, claimed ? 1 : 0);
            return 1;
        }
    }

    void RegisterRttiNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "rttiClassName", &ClassName },
            { "rttiObject", &ObjectAt },
            { "rttiOwner", &OwnerOf },
            { "rttiWhois", &Whois },
            { "rttiBases", &Bases },
            { "rttiVTable", &VTable },
            { "rttiVTables", &VTables },
            { "rttiInstances", &Instances },
            { "rttiClasses", &Classes },
            { "rttiDemangle", &Demangle },
            { "rttiMangle", &Mangle },
            { "rttiDissect", &Dissect },
            { "rttiLabel", &Label },
            { "rttiStatus", &StatusOf },
            { "rttiReset", &Reset },
            { "rttiClaims", &Claims },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
