#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kAddressListType = "Manifold.AddressList";
        constexpr const char* kRecordType = "Manifold.MemoryRecord";
        constexpr const char* kPartType = "Manifold.RecordPart";
        constexpr const char* kIndexerType = "Manifold.RecordIndexer";
        constexpr const char* kDropDownType = "Manifold.DropDownList";
        constexpr const char* kRecordHotkeyType = "Manifold.RecordHotkey";
        constexpr const char* kTimerType = "Manifold.Timer";
        constexpr const char* kHotkeyType = "Manifold.Hotkey";
        constexpr const char* kThreadsKey = "Manifold.Threads";
        constexpr const char* kExtrasKey = "Manifold.Extras";
        constexpr const char* kRecordCacheKey = "Manifold.RecordCache";
        constexpr const char* kListKey = "Manifold.AddressListObject";
        constexpr const char* kFallbackKey = "Manifold.Fallback";

        enum class Part
        {
            String,
            Aob,
            Binary
        };

        enum class Indexer
        {
            Offset,
            OffsetText,
            Child,
            Hotkey,
            DropDownValue,
            DropDownDescription,
            ListRecord
        };

        struct Handle
        {
            int Id = 0;
            int Sub = 0;
        };

        Handle& CheckHandle(lua_State* state, int index, const char* type)
        {
            return *static_cast<Handle*>(luaL_checkudata(state, index, type));
        }

        int CheckId(lua_State* state, int index, const char* type)
        {
            return CheckHandle(state, index, type).Id;
        }

        void PushHandle(lua_State* state, const char* type, int id, int sub = 0)
        {
            auto* handle = static_cast<Handle*>(lua_newuserdatauv(state, sizeof(Handle), 0));
            handle->Id = id;
            handle->Sub = sub;
            luaL_setmetatable(state, type);
        }

        Table::MemoryRecord* FindRecord(lua_State* state, int id)
        {
            return Owner(state).Table().FindById(id);
        }

        Table::MemoryRecord& CheckRecord(lua_State* state, int index)
        {
            const int id = CheckId(state, index, kRecordType);
            Table::MemoryRecord* record = FindRecord(state, id);
            if (!record)
                luaL_error(state, "the memory record %d no longer exists", id);
            return *record;
        }

        Table::MemoryRecord* RecordArgument(lua_State* state, int index)
        {
            if (auto* handle = static_cast<Handle*>(luaL_testudata(state, index, kRecordType)))
                return FindRecord(state, handle->Id);
            return nullptr;
        }

        std::string AddressText(lua_State* state, int index)
        {
            if (lua_isinteger(state, index))
                return Hex(static_cast<std::uint64_t>(lua_tointeger(state, index)));
            return TextArgument(state, index);
        }

        bool FindMethod(lua_State* state, const luaL_Reg* methods, const std::string& key)
        {
            for (const luaL_Reg* method = methods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return true;
                }
            }
            return false;
        }

        void SetExtra(lua_State* state, int id, const std::string& key, int valueIndex)
        {
            valueIndex = lua_absindex(state, valueIndex);
            lua_getfield(state, LUA_REGISTRYINDEX, kExtrasKey);
            lua_rawgeti(state, -1, id);
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                lua_newtable(state);
                lua_pushvalue(state, -1);
                lua_rawseti(state, -3, id);
            }
            lua_pushlstring(state, key.data(), key.size());
            lua_pushvalue(state, valueIndex);
            lua_rawset(state, -3);
            lua_pop(state, 2);
        }

        int GenericGetter(lua_State* state)
        {
            lua_settop(state, 1);
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_gettable(state, 1);
            return 1;
        }

        int GenericSetter(lua_State* state)
        {
            lua_settop(state, 2);
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_pushvalue(state, 2);
            lua_settable(state, 1);
            return 0;
        }

        bool PushAccessor(lua_State* state, const std::string& key)
        {
            if (key.size() > 3 && (key.rfind("get", 0) == 0 || key.rfind("set", 0) == 0))
            {
                lua_pushstring(state, key.c_str() + 3);
                lua_pushcclosure(state, key[0] == 'g' ? &GenericGetter : &GenericSetter, 1);
                PushBound(state, 1);
                return true;
            }
            return false;
        }

        bool IsSelf(lua_State* state, int index, int self)
        {
            if (lua_rawequal(state, index, self))
                return true;
            if (lua_type(state, index) != LUA_TUSERDATA || lua_type(state, self) != LUA_TUSERDATA)
                return false;
            if (lua_rawlen(state, index) != lua_rawlen(state, self))
                return false;
            if (!lua_getmetatable(state, index))
                return false;
            if (!lua_getmetatable(state, self))
            {
                lua_pop(state, 1);
                return false;
            }
            const bool sameType = lua_rawequal(state, -1, -2);
            lua_pop(state, 2);
            return sameType && std::memcmp(lua_touserdata(state, index), lua_touserdata(state, self), lua_rawlen(state, index)) == 0;
        }

        int BoundCall(lua_State* state)
        {
            if (lua_gettop(state) == 0 || !IsSelf(state, 1, lua_upvalueindex(1)))
            {
                lua_pushvalue(state, lua_upvalueindex(1));
                lua_insert(state, 1);
            }
            lua_pushvalue(state, lua_upvalueindex(2));
            lua_insert(state, 1);
            lua_call(state, lua_gettop(state) - 1, LUA_MULTRET);
            return lua_gettop(state);
        }

        bool CallFallback(lua_State* state, const char* kind, int object, int key)
        {
            object = lua_absindex(state, object);
            key = lua_absindex(state, key);
            lua_getfield(state, LUA_REGISTRYINDEX, kFallbackKey);
            lua_getfield(state, -1, kind);
            lua_remove(state, -2);
            if (!lua_isfunction(state, -1))
            {
                lua_pop(state, 1);
                return false;
            }
            lua_pushvalue(state, object);
            lua_pushvalue(state, key);
            lua_call(state, 2, 1);
            return true;
        }

        int TypeConstant(const Table::MemoryRecord& record)
        {
            if (record.Kind == Table::RecordKind::Script)
                return 11;
            if (!record.CustomTypeName.empty())
                return 13;
            switch (record.Type)
            {
            case Scan::ValueType::Byte: return 0;
            case Scan::ValueType::TwoBytes: return 1;
            case Scan::ValueType::FourBytes: return 2;
            case Scan::ValueType::EightBytes: return 3;
            case Scan::ValueType::Float: return 4;
            case Scan::ValueType::Double: return 5;
            case Scan::ValueType::String: return record.Unicode ? 7 : 6;
            case Scan::ValueType::ByteArray: return 8;
            case Scan::ValueType::Binary: return 9;
            case Scan::ValueType::Pointer: return 12;
            }
            return 2;
        }

        void ApplyTypeConstant(Table::MemoryRecord& record, lua_Integer value)
        {
            const bool group = record.Kind == Table::RecordKind::Group;
            if (!group)
                record.Kind = Table::RecordKind::Value;
            switch (value)
            {
            case 0: record.Type = Scan::ValueType::Byte; break;
            case 1: record.Type = Scan::ValueType::TwoBytes; break;
            case 2: record.Type = Scan::ValueType::FourBytes; break;
            case 3: record.Type = Scan::ValueType::EightBytes; break;
            case 4: record.Type = Scan::ValueType::Float; break;
            case 5: record.Type = Scan::ValueType::Double; break;
            case 6: record.Type = Scan::ValueType::String; record.Unicode = false; break;
            case 7: record.Type = Scan::ValueType::String; record.Unicode = true; break;
            case 8: record.Type = Scan::ValueType::ByteArray; break;
            case 9: record.Type = Scan::ValueType::Binary; break;
            case 11: if (!group) record.Kind = Table::RecordKind::Script; break;
            case 12: record.Type = Scan::ValueType::Pointer; break;
            default: break;
            }
        }

        std::string AddressString(Engine& engine, Table::MemoryRecord& record)
        {
            if (record.Kind == Table::RecordKind::Script || (record.Kind == Table::RecordKind::Group && record.Address.empty()))
                return {};
            if (!record.IsPointer())
                return record.Address;
            engine.ResolveRecord(record);
            return record.AddressOk ? "P->" + Hex(record.ResolvedAddress, 8) : "P->????????";
        }

        void PushOffsetValue(lua_State* state, Engine& engine, const std::string& text)
        {
            if (const auto value = engine.Evaluate(text))
                lua_pushinteger(state, static_cast<lua_Integer>(*value));
            else
                lua_pushinteger(state, 0);
        }

        Table::Hotkey* FindHotkey(Table::MemoryRecord& record, int id)
        {
            for (Table::Hotkey& hotkey : record.Hotkeys)
            {
                if (hotkey.Id == id)
                    return &hotkey;
            }
            return nullptr;
        }

        void PushRecordHotkey(lua_State* state, const Table::MemoryRecord& record, const Table::Hotkey* hotkey)
        {
            if (!hotkey)
                lua_pushnil(state);
            else
                PushHandle(state, kRecordHotkeyType, record.Id, hotkey->Id);
        }

        std::vector<int> KeyTable(lua_State* state, int index)
        {
            std::vector<int> keys;
            if (lua_istable(state, index))
            {
                const lua_Integer count = luaL_len(state, index);
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(state, index, i);
                    if (lua_isinteger(state, -1))
                        keys.push_back(static_cast<int>(lua_tointeger(state, -1)));
                    lua_pop(state, 1);
                }
            }
            else if (lua_isinteger(state, index))
            {
                keys.push_back(static_cast<int>(lua_tointeger(state, index)));
            }
            return keys;
        }

        void PushKeyTable(lua_State* state, const std::vector<int>& keys)
        {
            lua_createtable(state, static_cast<int>(keys.size()), 0);
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                lua_pushinteger(state, keys[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        int CreateRecordHotkey(lua_State* state)
        {
            Table::MemoryRecord& record = CheckRecord(state, 1);
            Table::Hotkey hotkey;
            hotkey.Keys = KeyTable(state, 2);
            hotkey.Action = static_cast<Table::HotkeyAction>(std::clamp<lua_Integer>(luaL_optinteger(state, 3, 0), 0, 7));
            hotkey.Value = lua_isnoneornil(state, 4) ? std::string() : TextArgument(state, 4);
            hotkey.Description = luaL_optstring(state, 5, "");
            int next = 0;
            for (const Table::Hotkey& existing : record.Hotkeys)
                next = std::max(next, existing.Id + 1);
            hotkey.Id = next;
            record.Hotkeys.push_back(hotkey);
            Owner(state).Table().Touch();
            PushRecordHotkey(state, record, &record.Hotkeys.back());
            return 1;
        }

        int RecordIndex(lua_State* state)
        {
            Table::MemoryRecord& record = CheckRecord(state, 1);
            Engine& engine = Owner(state);
            if (lua_isinteger(state, 2))
            {
                const lua_Integer index = lua_tointeger(state, 2);
                PushRecord(state, index >= 0 && index < static_cast<lua_Integer>(record.Children.size()) ? record.Children[static_cast<std::size_t>(index)].get() : nullptr);
                return 1;
            }
            const std::string key = Key(state, 2);
            if (key == "id") { lua_pushinteger(state, record.Id); return 1; }
            if (key == "description") { lua_pushstring(state, record.Description.c_str()); return 1; }
            if (key == "address") { lua_pushstring(state, record.Address.c_str()); return 1; }
            if (key == "addressstring") { lua_pushstring(state, AddressString(engine, record).c_str()); return 1; }
            if (key == "currentaddress" || key == "realaddress")
            {
                engine.ResolveRecord(record);
                lua_pushinteger(state, static_cast<lua_Integer>(record.AddressOk ? record.ResolvedAddress : 0));
                return 1;
            }
            if (key == "type" || key == "vartype") { lua_pushinteger(state, TypeConstant(record)); return 1; }
            if (key == "customtypename") { lua_pushstring(state, record.CustomTypeName.c_str()); return 1; }
            if (key == "value")
            {
                if (record.Kind != Table::RecordKind::Value)
                {
                    lua_pushstring(state, record.Kind == Table::RecordKind::Script ? "<script>" : "");
                    return 1;
                }
                const std::vector<std::uint8_t> bytes = engine.ReadRecord(record);
                const std::string text = bytes.empty() ? "??" : engine.FormatRecord(record, bytes);
                lua_pushstring(state, text.c_str());
                return 1;
            }
            if (key == "numericalvalue")
            {
                const std::vector<std::uint8_t> bytes = record.Kind == Table::RecordKind::Value ? engine.ReadRecord(record) : std::vector<std::uint8_t>();
                const auto value = bytes.empty() ? std::nullopt : Scan::AsDouble(record.Type, bytes, record.ShowSigned);
                if (value)
                    lua_pushnumber(state, *value);
                else
                    lua_pushnil(state);
                return 1;
            }
            if (key == "active") { lua_pushboolean(state, record.Active); return 1; }
            if (key == "script")
            {
                if (record.Kind == Table::RecordKind::Script)
                    lua_pushstring(state, record.Script.c_str());
                else
                    lua_pushnil(state);
                return 1;
            }
            if (key == "count") { lua_pushinteger(state, static_cast<lua_Integer>(record.Children.size())); return 1; }
            if (key == "parent") { PushRecord(state, record.Parent); return 1; }
            if (key == "isgroupheader") { lua_pushboolean(state, record.Kind == Table::RecordKind::Group); return 1; }
            if (key == "isaddressgroupheader") { lua_pushboolean(state, record.Kind == Table::RecordKind::Group && record.AddressGroupHeader); return 1; }
            if (key == "showashex") { lua_pushboolean(state, record.ShowHex); return 1; }
            if (key == "showassigned") { lua_pushboolean(state, record.ShowSigned); return 1; }
            if (key == "showasbinary") { lua_pushboolean(state, false); return 1; }
            if (key == "color") { lua_pushinteger(state, record.HasColor ? static_cast<lua_Integer>(record.Color) : 0x80000008); return 1; }
            if (key == "offsetcount") { lua_pushinteger(state, static_cast<lua_Integer>(record.Offsets.size())); return 1; }
            if (key == "selected") { lua_pushboolean(state, engine.Table().Selected(record.Id)); return 1; }
            if (key == "options") { lua_pushstring(state, Table::OptionsText(record.Options).c_str()); return 1; }
            if (key == "collapsed") { lua_pushboolean(state, !record.Expanded); return 1; }
            if (key == "expanded") { lua_pushboolean(state, record.Expanded); return 1; }
            if (key == "dontsave") { lua_pushboolean(state, record.DontSave); return 1; }
            if (key == "async") { lua_pushboolean(state, record.Async || record.Options.AsyncScript); return 1; }
            if (key == "asyncprocessing") { lua_pushboolean(state, false); return 1; }
            if (key == "asyncprocessingtime") { lua_pushinteger(state, 0); return 1; }
            if (key == "allowincrease") { lua_pushboolean(state, record.Freeze == Table::FreezeMode::AllowIncrease); return 1; }
            if (key == "allowdecrease") { lua_pushboolean(state, record.Freeze == Table::FreezeMode::AllowDecrease); return 1; }
            if (key == "dropdownlinked") { lua_pushboolean(state, !record.DropDownLink.empty()); return 1; }
            if (key == "dropdownlinkedmemrec") { lua_pushstring(state, record.DropDownLink.c_str()); return 1; }
            if (key == "dropdownreadonly") { lua_pushboolean(state, record.DropDownOnly); return 1; }
            if (key == "dropdowndescriptiononly") { lua_pushboolean(state, record.DropDownDescriptionOnly); return 1; }
            if (key == "dropdowncount") { lua_pushinteger(state, static_cast<lua_Integer>(engine.DropDownOf(record).size())); return 1; }
            if (key == "hotkeycount") { lua_pushinteger(state, static_cast<lua_Integer>(record.Hotkeys.size())); return 1; }
            if (key == "comment") { lua_pushstring(state, record.Comment.c_str()); return 1; }
            if (key == "lasterror") { lua_pushstring(state, record.LastError.c_str()); return 1; }
            if (key == "length") { lua_pushinteger(state, record.Length); return 1; }
            if (key == "unicode") { lua_pushboolean(state, record.Unicode); return 1; }
            if (key == "index")
            {
                const auto all = engine.Table().Flatten();
                const auto found = std::find(all.begin(), all.end(), &record);
                lua_pushinteger(state, found == all.end() ? -1 : static_cast<lua_Integer>(found - all.begin()));
                return 1;
            }
            if (key == "isreadable")
            {
                const std::vector<std::uint8_t> bytes = record.Kind == Table::RecordKind::Value ? engine.ReadRecord(record) : std::vector<std::uint8_t>();
                lua_pushboolean(state, !bytes.empty());
                return 1;
            }
            if (key == "string") { PushHandle(state, kPartType, record.Id, static_cast<int>(Part::String)); return 1; }
            if (key == "aob") { PushHandle(state, kPartType, record.Id, static_cast<int>(Part::Aob)); return 1; }
            if (key == "binary") { PushHandle(state, kPartType, record.Id, static_cast<int>(Part::Binary)); return 1; }
            if (key == "dropdownlist") { PushHandle(state, kDropDownType, record.Id); return 1; }
            if (key == "offset") { PushHandle(state, kIndexerType, record.Id, static_cast<int>(Indexer::Offset)); return 1; }
            if (key == "offsettext") { PushHandle(state, kIndexerType, record.Id, static_cast<int>(Indexer::OffsetText)); return 1; }
            if (key == "child") { PushHandle(state, kIndexerType, record.Id, static_cast<int>(Indexer::Child)); return 1; }
            if (key == "hotkey") { PushHandle(state, kIndexerType, record.Id, static_cast<int>(Indexer::Hotkey)); return 1; }
            if (key == "dropdownvalue") { PushHandle(state, kIndexerType, record.Id, static_cast<int>(Indexer::DropDownValue)); return 1; }
            if (key == "dropdowndescription") { PushHandle(state, kIndexerType, record.Id, static_cast<int>(Indexer::DropDownDescription)); return 1; }
            if (key == "owner") { PushAddressList(state); return 1; }
            if (key == "classname") { lua_pushstring(state, "TMemoryRecord"); return 1; }

            static const luaL_Reg kMethods[] = {
                { "delete", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        if (target.Active)
                            Owner(s).SetActive(target, false, true);
                        const int id = target.Id;
                        Owner(s).Table().Remove(target);
                        EngineOf(s).RecordDestroyed(id);
                        return 0;
                    } },
                { "destroy", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        const int id = target.Id;
                        Owner(s).Table().Remove(target);
                        EngineOf(s).RecordDestroyed(id);
                        return 0;
                    } },
                { "getaddress", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        lua_pushstring(s, target.Address.c_str());
                        if (!target.IsPointer())
                            return 1;
                        lua_createtable(s, static_cast<int>(target.Offsets.size()), 0);
                        for (std::size_t i = 0; i < target.Offsets.size(); ++i)
                        {
                            PushOffsetValue(s, Owner(s), target.Offsets[target.Offsets.size() - 1 - i]);
                            lua_rawseti(s, -2, static_cast<lua_Integer>(i + 1));
                        }
                        return 2;
                    } },
                { "setaddress", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        target.Address = AddressText(s, 2);
                        if (lua_istable(s, 3))
                        {
                            target.Offsets.clear();
                            const lua_Integer count = luaL_len(s, 3);
                            for (lua_Integer i = count; i >= 1; --i)
                            {
                                lua_rawgeti(s, 3, i);
                                target.Offsets.push_back(AddressText(s, -1));
                                lua_pop(s, 1);
                            }
                        }
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "getoffset", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        if (index < 0 || index >= static_cast<lua_Integer>(target.Offsets.size()))
                            return luaL_error(s, "offset index out of range");
                        PushOffsetValue(s, Owner(s), target.Offsets[target.Offsets.size() - 1 - static_cast<std::size_t>(index)]);
                        return 1;
                    } },
                { "setoffset", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        if (index < 0)
                            return luaL_error(s, "offset index out of range");
                        if (index >= static_cast<lua_Integer>(target.Offsets.size()))
                            target.Offsets.insert(target.Offsets.begin(), static_cast<std::size_t>(index) + 1 - target.Offsets.size(), "0");
                        target.Offsets[target.Offsets.size() - 1 - static_cast<std::size_t>(index)] = AddressText(s, 3);
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "appendtoentry", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        Table::MemoryRecord& parent = CheckRecord(s, 2);
                        Owner(s).Table().Move(target, &parent, -1);
                        return 0;
                    } },
                { "setvalue", [](lua_State* s) -> int
                    {
                        const std::string text = TextArgument(s, 2);
                        const Status status = Owner(s).WriteRecord(CheckRecord(s, 1), text);
                        lua_pushboolean(s, status.Ok());
                        return 1;
                    } },
                { "setactive", [](lua_State* s) -> int
                    {
                        const Status status = Owner(s).SetActive(CheckRecord(s, 1), lua_toboolean(s, 2) != 0, true);
                        lua_pushboolean(s, status.Ok());
                        return 1;
                    } },
                { "disablewithoutexecute", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        target.Active = false;
                        target.ScriptMemory.Values.clear();
                        Owner(s).Table().Refresh();
                        return 0;
                    } },
                { "getchild", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        PushRecord(s, index >= 0 && index < static_cast<lua_Integer>(target.Children.size()) ? target.Children[static_cast<std::size_t>(index)].get() : nullptr);
                        return 1;
                    } },
                { "gethotkey", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        PushRecordHotkey(s, target, index >= 0 && index < static_cast<lua_Integer>(target.Hotkeys.size()) ? &target.Hotkeys[static_cast<std::size_t>(index)] : nullptr);
                        return 1;
                    } },
                { "gethotkeybyid", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        PushRecordHotkey(s, target, FindHotkey(target, static_cast<int>(luaL_checkinteger(s, 2))));
                        return 1;
                    } },
                { "createhotkey", &CreateRecordHotkey },
                { "isselected", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckRecord(s, 1);
                        lua_pushboolean(s, Owner(s).Table().Selected(target.Id));
                        return 1;
                    } },
                { "beginedit", [](lua_State*) -> int { return 0; } },
                { "endedit", [](lua_State* s) -> int { Owner(s).Table().Touch(); return 0; } },
                { "reinterpret", [](lua_State* s) -> int { CheckRecord(s, 1).AddressOk = false; return 0; } },
                { "refresh", [](lua_State* s) -> int { Owner(s).RefreshValues(true); return 0; } },
                { nullptr, nullptr },
            };
            if (FindMethod(state, kMethods, key))
                return 1;
            if (PushExtra(state, record.Id, key))
                return 1;
            if (PushAccessor(state, key))
                return 1;
            if (CallFallback(state, "record", 1, 2))
                return 1;
            lua_pushnil(state);
            return 1;
        }

        int AwaitActivation(lua_State* state, int, lua_KContext context)
        {
            if (Owner(state).ActivationPending(static_cast<int>(context)))
                return lua_yieldk(state, 0, context, &AwaitActivation);
            return 0;
        }

        int RecordNewIndex(lua_State* state)
        {
            Table::MemoryRecord& record = CheckRecord(state, 1);
            Engine& engine = Owner(state);
            const std::string key = Key(state, 2);
            if (key == "description")
                record.Description = TextArgument(state, 3);
            else if (key == "address")
            {
                record.Address = AddressText(state, 3);
                record.AddressOk = false;
            }
            else if (key == "type" || key == "vartype")
                ApplyTypeConstant(record, luaL_checkinteger(state, 3));
            else if (key == "customtypename")
                record.CustomTypeName = luaL_checkstring(state, 3);
            else if (key == "value")
            {
                const std::string text = TextArgument(state, 3);
                const Status status = engine.WriteRecord(record, text);
                if (!status)
                    EngineOf(state).PrintError(status.Text);
                engine.Table().Refresh();
                return 0;
            }
            else if (key == "active")
            {
                const bool wanted = lua_toboolean(state, 3) != 0;
                if (engine.Async() && Manifold::OnMainThread() && lua_isyieldable(state) && EngineOf(state).IsCooperative(state))
                {
                    if (!engine.InsideActivation())
                    {
                        engine.RequestActive(record, wanted, true);
                        if (engine.ActivationPending(record.Id))
                            return lua_yieldk(state, 0, static_cast<lua_KContext>(record.Id), &AwaitActivation);
                        return 0;
                    }
                    engine.DropQueued(record.Id);
                }
                const Status status = engine.SetActive(record, wanted, true);
                if (!status)
                    EngineOf(state).PrintError(status.Text);
                return 0;
            }
            else if (key == "script")
            {
                record.Script = luaL_checkstring(state, 3);
                if (record.Kind == Table::RecordKind::Value)
                    record.Kind = Table::RecordKind::Script;
            }
            else if (key == "isgroupheader")
                record.Kind = lua_toboolean(state, 3) ? Table::RecordKind::Group : (record.Script.empty() ? Table::RecordKind::Value : Table::RecordKind::Script);
            else if (key == "isaddressgroupheader")
                record.AddressGroupHeader = lua_toboolean(state, 3) != 0;
            else if (key == "showashex")
                record.ShowHex = lua_toboolean(state, 3) != 0;
            else if (key == "showassigned")
                record.ShowSigned = lua_toboolean(state, 3) != 0;
            else if (key == "color")
            {
                const lua_Integer color = luaL_checkinteger(state, 3);
                record.HasColor = color >= 0 && color < 0x1000000;
                record.Color = static_cast<std::uint32_t>(color);
            }
            else if (key == "offsetcount")
            {
                const std::size_t count = static_cast<std::size_t>(std::max<lua_Integer>(0, luaL_checkinteger(state, 3)));
                if (count > record.Offsets.size())
                    record.Offsets.insert(record.Offsets.begin(), count - record.Offsets.size(), "0");
                else
                    record.Offsets.erase(record.Offsets.begin(), record.Offsets.begin() + static_cast<std::ptrdiff_t>(record.Offsets.size() - count));
                record.AddressOk = false;
            }
            else if (key == "selected")
            {
                if (lua_toboolean(state, 3))
                    engine.Table().Select(record.Id, true);
                else
                    engine.Table().Selection().erase(std::remove(engine.Table().Selection().begin(), engine.Table().Selection().end(), record.Id), engine.Table().Selection().end());
                engine.Table().Refresh();
                return 0;
            }
            else if (key == "options")
                Table::ParseOptionsText(record.Options, luaL_checkstring(state, 3));
            else if (key == "collapsed")
                record.Expanded = !lua_toboolean(state, 3);
            else if (key == "expanded")
                record.Expanded = lua_toboolean(state, 3) != 0;
            else if (key == "dontsave")
                record.DontSave = lua_toboolean(state, 3) != 0;
            else if (key == "async")
                record.Async = lua_toboolean(state, 3) != 0;
            else if (key == "allowincrease")
                record.Freeze = lua_toboolean(state, 3) ? Table::FreezeMode::AllowIncrease : Table::FreezeMode::Normal;
            else if (key == "allowdecrease")
                record.Freeze = lua_toboolean(state, 3) ? Table::FreezeMode::AllowDecrease : Table::FreezeMode::Normal;
            else if (key == "dropdownlinked")
            {
                if (!lua_toboolean(state, 3))
                    record.DropDownLink.clear();
            }
            else if (key == "dropdownlinkedmemrec")
                record.DropDownLink = lua_isstring(state, 3) ? lua_tostring(state, 3) : "";
            else if (key == "dropdownreadonly")
                record.DropDownOnly = lua_toboolean(state, 3) != 0;
            else if (key == "dropdowndescriptiononly")
                record.DropDownDescriptionOnly = lua_toboolean(state, 3) != 0;
            else if (key == "dropdownlist")
            {
                if (lua_type(state, 3) == LUA_TSTRING)
                    record.DropDown = Table::ParseDropDown(lua_tostring(state, 3));
                else if (auto* other = static_cast<Handle*>(luaL_testudata(state, 3, kDropDownType)))
                {
                    if (Table::MemoryRecord* source = FindRecord(state, other->Id))
                        record.DropDown = source->DropDown;
                }
                else
                {
                    lua_getfield(state, 3, "Text");
                    if (lua_type(state, -1) == LUA_TSTRING)
                        record.DropDown = Table::ParseDropDown(lua_tostring(state, -1));
                    lua_pop(state, 1);
                }
            }
            else if (key == "comment")
                record.Comment = TextArgument(state, 3);
            else if (key == "length")
                record.SetLength(luaL_checkinteger(state, 3));
            else if (key == "unicode")
                record.Unicode = lua_toboolean(state, 3) != 0;
            else
            {
                SetExtra(state, record.Id, key, 3);
                return 0;
            }
            engine.Table().Touch();
            return 0;
        }

        int RecordToString(lua_State* state)
        {
            Table::MemoryRecord& record = CheckRecord(state, 1);
            lua_pushfstring(state, "MemoryRecord %d: %s", record.Id, record.Description.c_str());
            return 1;
        }

        int HandleEqual(lua_State* state)
        {
            const Handle* a = static_cast<Handle*>(lua_touserdata(state, 1));
            const Handle* b = static_cast<Handle*>(lua_touserdata(state, 2));
            lua_pushboolean(state, a && b && a->Id == b->Id && a->Sub == b->Sub);
            return 1;
        }

        int PartIndex(lua_State* state)
        {
            const Handle& handle = CheckHandle(state, 1, kPartType);
            Table::MemoryRecord* record = FindRecord(state, handle.Id);
            const std::string key = Key(state, 2);
            if (!record)
                return luaL_error(state, "the memory record %d no longer exists", handle.Id);
            switch (static_cast<Part>(handle.Sub))
            {
            case Part::String:
                if (key == "size") { lua_pushinteger(state, record->Length); return 1; }
                if (key == "unicode") { lua_pushboolean(state, record->Unicode); return 1; }
                if (key == "codepage") { lua_pushboolean(state, false); return 1; }
                if (key == "zeroterminate") { lua_pushboolean(state, record->ZeroTerminate); return 1; }
                break;
            case Part::Aob:
                if (key == "size") { lua_pushinteger(state, record->Length); return 1; }
                break;
            case Part::Binary:
                if (key == "startbit") { lua_pushinteger(state, record->BitStart); return 1; }
                if (key == "size") { lua_pushinteger(state, record->BitLength); return 1; }
                break;
            }
            lua_pushnil(state);
            return 1;
        }

        int PartNewIndex(lua_State* state)
        {
            const Handle& handle = CheckHandle(state, 1, kPartType);
            Table::MemoryRecord* record = FindRecord(state, handle.Id);
            const std::string key = Key(state, 2);
            if (!record)
                return luaL_error(state, "the memory record %d no longer exists", handle.Id);
            switch (static_cast<Part>(handle.Sub))
            {
            case Part::String:
                if (key == "size") record->SetLength(luaL_checkinteger(state, 3));
                else if (key == "unicode") record->Unicode = lua_toboolean(state, 3) != 0;
                else if (key == "zeroterminate") record->ZeroTerminate = lua_toboolean(state, 3) != 0;
                break;
            case Part::Aob:
                if (key == "size") record->SetLength(luaL_checkinteger(state, 3));
                break;
            case Part::Binary:
                if (key == "startbit") record->SetBits(luaL_checkinteger(state, 3), record->BitLength);
                else if (key == "size") record->SetBits(record->BitStart, luaL_checkinteger(state, 3));
                break;
            }
            Owner(state).Table().Touch();
            return 0;
        }

        int IndexerIndex(lua_State* state)
        {
            const Handle& handle = CheckHandle(state, 1, kIndexerType);
            Engine& engine = Owner(state);
            const Indexer which = static_cast<Indexer>(handle.Sub);
            if (which == Indexer::ListRecord)
            {
                const auto all = engine.Table().Flatten();
                const lua_Integer index = luaL_checkinteger(state, 2);
                PushRecord(state, index >= 0 && index < static_cast<lua_Integer>(all.size()) ? all[static_cast<std::size_t>(index)] : nullptr);
                return 1;
            }
            Table::MemoryRecord* record = FindRecord(state, handle.Id);
            if (!record)
                return luaL_error(state, "the memory record %d no longer exists", handle.Id);
            const lua_Integer index = luaL_checkinteger(state, 2);
            const auto inRange = [&](std::size_t size) { return index >= 0 && index < static_cast<lua_Integer>(size); };
            switch (which)
            {
            case Indexer::Offset:
            case Indexer::OffsetText:
                if (!inRange(record->Offsets.size()))
                    break;
                if (which == Indexer::Offset)
                    PushOffsetValue(state, engine, record->Offsets[record->Offsets.size() - 1 - static_cast<std::size_t>(index)]);
                else
                    lua_pushstring(state, record->Offsets[record->Offsets.size() - 1 - static_cast<std::size_t>(index)].c_str());
                return 1;
            case Indexer::Child:
                PushRecord(state, inRange(record->Children.size()) ? record->Children[static_cast<std::size_t>(index)].get() : nullptr);
                return 1;
            case Indexer::Hotkey:
                PushRecordHotkey(state, *record, inRange(record->Hotkeys.size()) ? &record->Hotkeys[static_cast<std::size_t>(index)] : nullptr);
                return 1;
            case Indexer::DropDownValue:
            case Indexer::DropDownDescription:
            {
                const auto& entries = engine.DropDownOf(*record);
                if (!inRange(entries.size()))
                    break;
                const Table::DropDownEntry& entry = entries[static_cast<std::size_t>(index)];
                lua_pushstring(state, (which == Indexer::DropDownValue ? entry.Value : entry.Text).c_str());
                return 1;
            }
            case Indexer::ListRecord:
                break;
            }
            lua_pushnil(state);
            return 1;
        }

        int IndexerNewIndex(lua_State* state)
        {
            const Handle& handle = CheckHandle(state, 1, kIndexerType);
            Table::MemoryRecord* record = FindRecord(state, handle.Id);
            if (!record)
                return luaL_error(state, "the memory record %d no longer exists", handle.Id);
            const lua_Integer index = luaL_checkinteger(state, 2);
            const Indexer which = static_cast<Indexer>(handle.Sub);
            if ((which == Indexer::Offset || which == Indexer::OffsetText) && index >= 0)
            {
                if (index >= static_cast<lua_Integer>(record->Offsets.size()))
                    record->Offsets.insert(record->Offsets.begin(), static_cast<std::size_t>(index) + 1 - record->Offsets.size(), "0");
                record->Offsets[record->Offsets.size() - 1 - static_cast<std::size_t>(index)] = AddressText(state, 3);
                record->AddressOk = false;
                Owner(state).Table().Touch();
            }
            else if ((which == Indexer::DropDownValue || which == Indexer::DropDownDescription) && index >= 0)
            {
                if (index >= static_cast<lua_Integer>(record->DropDown.size()))
                    record->DropDown.resize(static_cast<std::size_t>(index) + 1);
                Table::DropDownEntry& entry = record->DropDown[static_cast<std::size_t>(index)];
                (which == Indexer::DropDownValue ? entry.Value : entry.Text) = TextArgument(state, 3);
                Owner(state).Table().Touch();
            }
            return 0;
        }

        int IndexerLength(lua_State* state)
        {
            const Handle& handle = CheckHandle(state, 1, kIndexerType);
            const Indexer which = static_cast<Indexer>(handle.Sub);
            if (which == Indexer::ListRecord)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(Owner(state).Table().Count()));
                return 1;
            }
            Table::MemoryRecord* record = FindRecord(state, handle.Id);
            std::size_t size = 0;
            if (record)
            {
                switch (which)
                {
                case Indexer::Offset: case Indexer::OffsetText: size = record->Offsets.size(); break;
                case Indexer::Child: size = record->Children.size(); break;
                case Indexer::Hotkey: size = record->Hotkeys.size(); break;
                case Indexer::DropDownValue: case Indexer::DropDownDescription: size = Owner(state).DropDownOf(*record).size(); break;
                case Indexer::ListRecord: break;
                }
            }
            lua_pushinteger(state, static_cast<lua_Integer>(size));
            return 1;
        }

        Table::MemoryRecord& CheckDropDownRecord(lua_State* state, int index)
        {
            const int id = CheckId(state, index, kDropDownType);
            Table::MemoryRecord* record = FindRecord(state, id);
            if (!record)
                luaL_error(state, "the memory record %d no longer exists", id);
            return *record;
        }

        std::string EntryLine(const Table::DropDownEntry& entry)
        {
            return entry.Value + ":" + entry.Text;
        }

        int DropDownIndex(lua_State* state)
        {
            Table::MemoryRecord& record = CheckDropDownRecord(state, 1);
            if (lua_isinteger(state, 2))
            {
                const lua_Integer index = lua_tointeger(state, 2);
                if (index >= 0 && index < static_cast<lua_Integer>(record.DropDown.size()))
                    lua_pushstring(state, EntryLine(record.DropDown[static_cast<std::size_t>(index)]).c_str());
                else
                    lua_pushnil(state);
                return 1;
            }
            const std::string key = Key(state, 2);
            if (key == "text") { lua_pushstring(state, Table::DropDownText(record.DropDown).c_str()); return 1; }
            if (key == "count") { lua_pushinteger(state, static_cast<lua_Integer>(record.DropDown.size())); return 1; }
            if (key == "classname") { lua_pushstring(state, "TStringList"); return 1; }
            if (key == "strings")
            {
                lua_pushvalue(state, 1);
                return 1;
            }
            static const luaL_Reg kMethods[] = {
                { "add", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckDropDownRecord(s, 1);
                        auto parsed = Table::ParseDropDown(luaL_checkstring(s, 2));
                        target.DropDown.insert(target.DropDown.end(), parsed.begin(), parsed.end());
                        Owner(s).Table().Touch();
                        lua_pushinteger(s, static_cast<lua_Integer>(target.DropDown.size()) - 1);
                        return 1;
                    } },
                { "clear", [](lua_State* s) -> int
                    {
                        CheckDropDownRecord(s, 1).DropDown.clear();
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "delete", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckDropDownRecord(s, 1);
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        if (index >= 0 && index < static_cast<lua_Integer>(target.DropDown.size()))
                            target.DropDown.erase(target.DropDown.begin() + index);
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "indexof", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckDropDownRecord(s, 1);
                        const std::string wanted = luaL_checkstring(s, 2);
                        for (std::size_t i = 0; i < target.DropDown.size(); ++i)
                        {
                            if (EqualsNoCase(EntryLine(target.DropDown[i]), wanted))
                            {
                                lua_pushinteger(s, static_cast<lua_Integer>(i));
                                return 1;
                            }
                        }
                        lua_pushinteger(s, -1);
                        return 1;
                    } },
                { "gettext", [](lua_State* s) -> int { lua_pushstring(s, Table::DropDownText(CheckDropDownRecord(s, 1).DropDown).c_str()); return 1; } },
                { "settext", [](lua_State* s) -> int
                    {
                        CheckDropDownRecord(s, 1).DropDown = Table::ParseDropDown(luaL_checkstring(s, 2));
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "getcount", [](lua_State* s) -> int { lua_pushinteger(s, static_cast<lua_Integer>(CheckDropDownRecord(s, 1).DropDown.size())); return 1; } },
                { "getstring", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckDropDownRecord(s, 1);
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        if (index >= 0 && index < static_cast<lua_Integer>(target.DropDown.size()))
                            lua_pushstring(s, EntryLine(target.DropDown[static_cast<std::size_t>(index)]).c_str());
                        else
                            lua_pushnil(s);
                        return 1;
                    } },
                { "assign", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& target = CheckDropDownRecord(s, 1);
                        lua_getfield(s, 2, "Text");
                        if (lua_type(s, -1) == LUA_TSTRING)
                            target.DropDown = Table::ParseDropDown(lua_tostring(s, -1));
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "beginupdate", [](lua_State*) -> int { return 0; } },
                { "endupdate", [](lua_State*) -> int { return 0; } },
                { "destroy", [](lua_State*) -> int { return 0; } },
                { nullptr, nullptr },
            };
            if (FindMethod(state, kMethods, key))
                return 1;
            lua_pushnil(state);
            return 1;
        }

        int DropDownNewIndex(lua_State* state)
        {
            Table::MemoryRecord& record = CheckDropDownRecord(state, 1);
            if (lua_isinteger(state, 2))
            {
                const lua_Integer index = lua_tointeger(state, 2);
                auto parsed = Table::ParseDropDown(luaL_checkstring(state, 3));
                if (index >= 0 && index < static_cast<lua_Integer>(record.DropDown.size()) && !parsed.empty())
                    record.DropDown[static_cast<std::size_t>(index)] = parsed.front();
            }
            else if (Key(state, 2) == "text")
                record.DropDown = Table::ParseDropDown(luaL_checkstring(state, 3));
            Owner(state).Table().Touch();
            return 0;
        }

        int DropDownLength(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(CheckDropDownRecord(state, 1).DropDown.size()));
            return 1;
        }

        struct HotkeyRef
        {
            Table::MemoryRecord* Record = nullptr;
            Table::Hotkey* Hotkey = nullptr;
        };

        HotkeyRef CheckRecordHotkey(lua_State* state, int index)
        {
            const Handle& handle = CheckHandle(state, index, kRecordHotkeyType);
            HotkeyRef ref;
            ref.Record = FindRecord(state, handle.Id);
            if (ref.Record)
                ref.Hotkey = FindHotkey(*ref.Record, handle.Sub);
            if (!ref.Hotkey)
                luaL_error(state, "the hotkey no longer exists");
            return ref;
        }

        std::string HotkeyExtraKey(int id, const std::string& key)
        {
            return "hotkey" + std::to_string(id) + "." + key;
        }

        int RecordHotkeyIndex(lua_State* state)
        {
            const Handle& handle = CheckHandle(state, 1, kRecordHotkeyType);
            const std::string key = Key(state, 2);
            if (key == "destroy" || key == "delete")
            {
                lua_pushcfunction(state, [](lua_State* s) -> int
                    {
                        const Handle& target = CheckHandle(s, 1, kRecordHotkeyType);
                        if (Table::MemoryRecord* record = FindRecord(s, target.Id))
                        {
                            record->Hotkeys.erase(std::remove_if(record->Hotkeys.begin(), record->Hotkeys.end(), [&](const Table::Hotkey& h) { return h.Id == target.Sub; }), record->Hotkeys.end());
                            Owner(s).Table().Touch();
                        }
                        return 0;
                    });
                PushBound(state, 1);
                return 1;
            }
            const HotkeyRef ref = CheckRecordHotkey(state, 1);
            if (key == "id") { lua_pushinteger(state, ref.Hotkey->Id); return 1; }
            if (key == "description") { lua_pushstring(state, ref.Hotkey->Description.c_str()); return 1; }
            if (key == "action") { lua_pushinteger(state, static_cast<lua_Integer>(ref.Hotkey->Action)); return 1; }
            if (key == "value") { lua_pushstring(state, ref.Hotkey->Value.c_str()); return 1; }
            if (key == "keys") { PushKeyTable(state, ref.Hotkey->Keys); return 1; }
            if (key == "hotkeystring") { lua_pushstring(state, Table::KeysText(ref.Hotkey->Keys).c_str()); return 1; }
            if (key == "owner") { PushRecord(state, ref.Record); return 1; }
            if (key == "classname") { lua_pushstring(state, "TMemoryRecordHotkey"); return 1; }
            static const luaL_Reg kMethods[] = {
                { "dohotkey", [](lua_State* s) -> int
                    {
                        const HotkeyRef target = CheckRecordHotkey(s, 1);
                        const Table::Hotkey copy = *target.Hotkey;
                        Owner(s).ApplyHotkey(*target.Record, copy);
                        return 0;
                    } },
                { "getkeys", [](lua_State* s) -> int { PushKeyTable(s, CheckRecordHotkey(s, 1).Hotkey->Keys); return 1; } },
                { "setkeys", [](lua_State* s) -> int
                    {
                        const HotkeyRef target = CheckRecordHotkey(s, 1);
                        std::vector<int> keys;
                        for (int i = 2; i <= lua_gettop(s); ++i)
                        {
                            const std::vector<int> part = KeyTable(s, i);
                            keys.insert(keys.end(), part.begin(), part.end());
                        }
                        target.Hotkey->Keys = std::move(keys);
                        Owner(s).Table().Touch();
                        return 0;
                    } },
                { "gethotkeystring", [](lua_State* s) -> int { lua_pushstring(s, Table::KeysText(CheckRecordHotkey(s, 1).Hotkey->Keys).c_str()); return 1; } },
                { nullptr, nullptr },
            };
            if (FindMethod(state, kMethods, key))
                return 1;
            if (PushExtra(state, handle.Id, HotkeyExtraKey(handle.Sub, key)))
                return 1;
            if (PushAccessor(state, key))
                return 1;
            lua_pushnil(state);
            return 1;
        }

        int RecordHotkeyNewIndex(lua_State* state)
        {
            const HotkeyRef ref = CheckRecordHotkey(state, 1);
            const std::string key = Key(state, 2);
            if (key == "description")
                ref.Hotkey->Description = TextArgument(state, 3);
            else if (key == "action")
                ref.Hotkey->Action = static_cast<Table::HotkeyAction>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 7));
            else if (key == "value")
                ref.Hotkey->Value = TextArgument(state, 3);
            else if (key == "keys")
                ref.Hotkey->Keys = KeyTable(state, 3);
            else
            {
                SetExtra(state, ref.Record->Id, HotkeyExtraKey(ref.Hotkey->Id, key), 3);
                return 0;
            }
            Owner(state).Table().Touch();
            return 0;
        }

        Table::MemoryRecord* SelectedRecord(Table::AddressList& list)
        {
            return list.Selection().empty() ? nullptr : list.FindById(list.Selection().front());
        }

        int AddressListIndex(lua_State* state)
        {
            Table::AddressList& list = Owner(state).Table();
            if (lua_isinteger(state, 2))
            {
                const auto all = list.Flatten();
                const lua_Integer index = lua_tointeger(state, 2);
                PushRecord(state, index >= 0 && index < static_cast<lua_Integer>(all.size()) ? all[static_cast<std::size_t>(index)] : nullptr);
                return 1;
            }
            const std::string key = Key(state, 2);
            if (key == "count") { lua_pushinteger(state, static_cast<lua_Integer>(list.Count())); return 1; }
            if (key == "selcount") { lua_pushinteger(state, static_cast<lua_Integer>(list.Selection().size())); return 1; }
            if (key == "selectedrecord") { PushRecord(state, SelectedRecord(list)); return 1; }
            if (key == "memoryrecord") { PushHandle(state, kIndexerType, 0, static_cast<int>(Indexer::ListRecord)); return 1; }
            if (key == "classname") { lua_pushstring(state, "TAddresslist"); return 1; }
            static const luaL_Reg kMethods[] = {
                { "getcount", [](lua_State* s) -> int { lua_pushinteger(s, static_cast<lua_Integer>(Owner(s).Table().Count())); return 1; } },
                { "getmemoryrecord", [](lua_State* s) -> int
                    {
                        const auto all = Owner(s).Table().Flatten();
                        const lua_Integer index = luaL_checkinteger(s, 2);
                        PushRecord(s, index >= 0 && index < static_cast<lua_Integer>(all.size()) ? all[static_cast<std::size_t>(index)] : nullptr);
                        return 1;
                    } },
                { "getmemoryrecordbydescription", [](lua_State* s) -> int { PushRecord(s, Owner(s).Table().FindByDescription(luaL_checkstring(s, 2))); return 1; } },
                { "getmemoryrecordbyid", [](lua_State* s) -> int { PushRecord(s, Owner(s).Table().FindById(static_cast<int>(luaL_checkinteger(s, 2)))); return 1; } },
                { "creatememoryrecord", [](lua_State* s) -> int
                    {
                        Table::MemoryRecord& record = Owner(s).Table().AddValue("No description", "0", Scan::ValueType::FourBytes);
                        PushRecord(s, &record);
                        return 1;
                    } },
                { "getselectedrecords", [](lua_State* s) -> int
                    {
                        lua_newtable(s);
                        int index = 1;
                        for (int id : Owner(s).Table().Selection())
                        {
                            if (Table::MemoryRecord* record = Owner(s).Table().FindById(id))
                            {
                                PushRecord(s, record);
                                lua_rawseti(s, -2, index++);
                            }
                        }
                        return 1;
                    } },
                { "getselectedrecord", [](lua_State* s) -> int { PushRecord(s, SelectedRecord(Owner(s).Table())); return 1; } },
                { "setselectedrecord", [](lua_State* s) -> int
                    {
                        Owner(s).Table().ClearSelection();
                        if (Table::MemoryRecord* record = RecordArgument(s, 2))
                            Owner(s).Table().Select(record->Id, false);
                        return 0;
                    } },
                { "disableallwithoutexecute", [](lua_State* s) -> int
                    {
                        Owner(s).Table().ForEach([](Table::MemoryRecord& record)
                            {
                                record.Active = false;
                                record.ScriptMemory.Values.clear();
                            });
                        Owner(s).Table().Refresh();
                        return 0;
                    } },
                { "refresh", [](lua_State* s) -> int { Owner(s).RefreshValues(true); return 0; } },
                { "rebuilddescriptioncache", [](lua_State*) -> int { return 0; } },
                { "clear", [](lua_State* s) -> int { Owner(s).NewTable(); return 0; } },
                { nullptr, nullptr },
            };
            if (FindMethod(state, kMethods, key))
                return 1;
            if (PushExtra(state, kAddressListExtras, key))
                return 1;
            if (CallFallback(state, "list", 1, 2))
                return 1;
            if (PushAccessor(state, key))
                return 1;
            lua_pushnil(state);
            return 1;
        }

        int AddressListNewIndex(lua_State* state)
        {
            const std::string key = Key(state, 2);
            Table::AddressList& list = Owner(state).Table();
            if (key == "selectedrecord")
            {
                list.ClearSelection();
                if (Table::MemoryRecord* record = RecordArgument(state, 3))
                    list.Select(record->Id, false);
                return 0;
            }
            SetExtra(state, kAddressListExtras, key, 3);
            return 0;
        }

        int GetAddressList(lua_State* state)
        {
            PushAddressList(state);
            return 1;
        }

        Timer& CheckTimer(lua_State* state, int index)
        {
            const int id = CheckId(state, index, kTimerType);
            Timer* timer = EngineOf(state).FindTimer(id);
            if (!timer)
                luaL_error(state, "the timer has been destroyed");
            return *timer;
        }

        void SetTimerCallback(lua_State* state, Timer& timer, int index)
        {
            LuaEngine& engine = EngineOf(state);
            engine.Release(timer.Callback);
            timer.Callback = 0;
            if (lua_isfunction(state, index))
            {
                lua_pushvalue(state, index);
                timer.Callback = luaL_ref(state, LUA_REGISTRYINDEX);
            }
        }

        int CreateTimer(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            if (lua_isnumber(state, 1) && lua_isfunction(state, 2))
            {
                const int id = engine.AddTimer(static_cast<int>(lua_tointeger(state, 1)), true);
                Timer& timer = *engine.FindTimer(id);
                timer.OneShot = true;
                SetTimerCallback(state, timer, 2);
                PushTimer(state, id);
                return 1;
            }
            const bool enabled = lua_isnoneornil(state, 2) || lua_toboolean(state, 2);
            PushTimer(state, engine.AddTimer(1000, enabled));
            return 1;
        }

        int TimerIndex(lua_State* state)
        {
            const int id = CheckId(state, 1, kTimerType);
            const std::string key = Key(state, 2);
            if (key == "destroy" || key == "free")
            {
                lua_pushcfunction(state, [](lua_State* s) -> int
                    {
                        if (Timer* target = EngineOf(s).FindTimer(CheckId(s, 1, kTimerType)))
                            target->Destroyed = true;
                        return 0;
                    });
                PushBound(state, 1);
                return 1;
            }
            Timer* found = EngineOf(state).FindTimer(id);
            if (!found)
            {
                if (key == "enabled")
                {
                    lua_pushboolean(state, 0);
                    return 1;
                }
                lua_pushnil(state);
                return 1;
            }
            Timer& timer = *found;
            if (key == "interval") { lua_pushinteger(state, timer.Interval); return 1; }
            if (key == "enabled") { lua_pushboolean(state, timer.Enabled); return 1; }
            if (key == "classname") { lua_pushstring(state, "TTimer"); return 1; }
            if (key == "ontimer")
            {
                if (timer.Callback > 0)
                    lua_rawgeti(state, LUA_REGISTRYINDEX, timer.Callback);
                else
                    lua_pushnil(state);
                return 1;
            }
            static const luaL_Reg kMethods[] = {
                { "setinterval", [](lua_State* s) -> int { CheckTimer(s, 1).Interval = std::max(1, static_cast<int>(luaL_checkinteger(s, 2))); return 0; } },
                { "getinterval", [](lua_State* s) -> int { lua_pushinteger(s, CheckTimer(s, 1).Interval); return 1; } },
                { "setenabled", [](lua_State* s) -> int { CheckTimer(s, 1).Enabled = lua_toboolean(s, 2) != 0; return 0; } },
                { "getenabled", [](lua_State* s) -> int { lua_pushboolean(s, CheckTimer(s, 1).Enabled); return 1; } },
                { "setontimer", [](lua_State* s) -> int { SetTimerCallback(s, CheckTimer(s, 1), 2); return 0; } },
                { nullptr, nullptr },
            };
            if (FindMethod(state, kMethods, key))
                return 1;
            if (PushExtra(state, -1000 - id, key))
                return 1;
            lua_pushnil(state);
            return 1;
        }

        int TimerNewIndex(lua_State* state)
        {
            const int id = CheckId(state, 1, kTimerType);
            Timer* found = EngineOf(state).FindTimer(id);
            if (!found)
                return 0;
            Timer& timer = *found;
            const std::string key = Key(state, 2);
            if (key == "interval")
            {
                timer.Interval = std::max(1, static_cast<int>(luaL_checkinteger(state, 3)));
                timer.NextAt = 0.0;
            }
            else if (key == "enabled")
            {
                timer.Enabled = lua_toboolean(state, 3) != 0;
                timer.NextAt = 0.0;
            }
            else if (key == "ontimer")
                SetTimerCallback(state, timer, 3);
            else
                SetExtra(state, -1000 - id, key, 3);
            return 0;
        }

        int CreateHotkey(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            std::vector<int> keys;
            for (int i = 2; i <= lua_gettop(state); ++i)
            {
                const std::vector<int> part = KeyTable(state, i);
                keys.insert(keys.end(), part.begin(), part.end());
            }
            if (keys.empty())
                return luaL_error(state, "createHotkey needs at least one key");
            lua_pushvalue(state, 1);
            const int callback = luaL_ref(state, LUA_REGISTRYINDEX);
            PushHandle(state, kHotkeyType, EngineOf(state).AddHotkey(std::move(keys), callback));
            return 1;
        }

        int HotkeyIndex(lua_State* state)
        {
            const std::string key = Key(state, 2);
            if (key == "destroy" || key == "free")
            {
                lua_pushcfunction(state, [](lua_State* s) -> int
                    {
                        EngineOf(s).RemoveHotkey(CheckId(s, 1, kHotkeyType));
                        return 0;
                    });
                PushBound(state, 1);
                return 1;
            }
            if (PushAccessor(state, key))
                return 1;
            lua_pushnil(state);
            return 1;
        }

        int CreateThread(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            const int arguments = lua_gettop(state) - 1;
            lua_getglobal(state, "__manifoldNewThreadObject");
            if (lua_isfunction(state, -1))
                lua_call(state, 0, 1);
            else
            {
                lua_pop(state, 1);
                lua_newtable(state);
                lua_pushboolean(state, 0);
                lua_setfield(state, -2, "Terminated");
            }
            const int object = lua_gettop(state);
            lua_getfield(state, LUA_REGISTRYINDEX, kThreadsKey);
            lua_State* thread = lua_newthread(state);
            lua_pushvalue(state, 1);
            lua_pushvalue(state, object);
            for (int i = 0; i < arguments; ++i)
                lua_pushvalue(state, 2 + i);
            lua_xmove(state, thread, arguments + 2);
            lua_pushnumber(state, 0.0);
            lua_rawset(state, -3);
            lua_pop(state, 1);
            lua_pushvalue(state, object);
            return 1;
        }

        int DebugProcess(lua_State* state)
        {
            const Status status = Owner(state).EnsureDebugger();
            if (!status)
                EngineOf(state).PrintError(status.Text);
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int DebugSetBreakpoint(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::uint64_t address = AddressArgument(state, 1);
            int size = 1;
            int trigger = 0;
            int callbackIndex = 0;
            int numbers = 0;
            for (int i = 2; i <= lua_gettop(state); ++i)
            {
                if (lua_isfunction(state, i))
                    callbackIndex = i;
                else if (lua_isinteger(state, i))
                {
                    if (numbers == 0)
                        size = static_cast<int>(lua_tointeger(state, i));
                    else if (numbers == 1)
                        trigger = static_cast<int>(lua_tointeger(state, i));
                    ++numbers;
                }
            }
            Status status = engine.EnsureDebugger();
            if (status)
            {
                const Debug::BreakpointType type = trigger == 1 ? Debug::BreakpointType::Access : trigger == 2 ? Debug::BreakpointType::Write : Debug::BreakpointType::Execute;
                status = engine.Debugger().SetBreakpoint(address, type, std::max(1, size), type != Debug::BreakpointType::Execute);
            }
            if (!status)
            {
                EngineOf(state).PrintError(status.Text);
                lua_pushboolean(state, 0);
                return 1;
            }
            if (callbackIndex)
            {
                lua_pushvalue(state, callbackIndex);
                EngineOf(state).SetBreakpointCallback(address, luaL_ref(state, LUA_REGISTRYINDEX));
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int DebugRemoveBreakpoint(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::uint64_t address = ResolveAddress(state, 1).value_or(0);
            EngineOf(state).ClearBreakpointCallback(address);
            const Status status = engine.Debugger().RemoveBreakpoint(address);
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int DebugContinue(lua_State* state)
        {
            const lua_Integer method = luaL_optinteger(state, 1, 0);
            const Debug::ResumeMode mode = method == 1 ? Debug::ResumeMode::StepInto : method == 2 ? Debug::ResumeMode::StepOver : method == 3 ? Debug::ResumeMode::RunToReturn : Debug::ResumeMode::Continue;
            Owner(state).Debugger().Resume(mode);
            return 0;
        }

        int DebugIsBroken(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).Debugger().Broken());
            return 1;
        }

        int DebugIsDebugging(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).Debugger().Attached());
            return 1;
        }

        int DebugBreakpointList(lua_State* state)
        {
            lua_newtable(state);
            int index = 1;
            for (const Debug::Breakpoint& breakpoint : Owner(state).Debugger().Breakpoints())
            {
                lua_pushinteger(state, static_cast<lua_Integer>(breakpoint.Address));
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        void PushVectors(lua_State* state, const Debug::VectorRegisters& vectors)
        {
            for (int i = 0; i < vectors.Count; ++i)
            {
                lua_createtable(state, 16, 0);
                for (int b = 0; b < 16; ++b)
                {
                    lua_pushinteger(state, vectors.Xmm[static_cast<std::size_t>(i)][static_cast<std::size_t>(b)]);
                    lua_rawseti(state, -2, b + 1);
                }
                lua_setglobal(state, ("XMM" + std::to_string(i)).c_str());
            }
            lua_pushinteger(state, vectors.Mxcsr);
            lua_setglobal(state, "MXCSR");
        }

        bool PullVectors(lua_State* state, Debug::VectorRegisters& vectors)
        {
            bool changed = false;
            for (int i = 0; i < vectors.Count; ++i)
            {
                lua_getglobal(state, ("XMM" + std::to_string(i)).c_str());
                if (lua_istable(state, -1))
                {
                    for (int b = 0; b < 16; ++b)
                    {
                        lua_rawgeti(state, -1, b + 1);
                        if (lua_isinteger(state, -1))
                        {
                            const auto byte = static_cast<std::uint8_t>(lua_tointeger(state, -1));
                            std::uint8_t& slot = vectors.Xmm[static_cast<std::size_t>(i)][static_cast<std::size_t>(b)];
                            changed = changed || slot != byte;
                            slot = byte;
                        }
                        lua_pop(state, 1);
                    }
                }
                lua_pop(state, 1);
            }
            lua_getglobal(state, "MXCSR");
            if (lua_isinteger(state, -1))
            {
                const auto mxcsr = static_cast<std::uint32_t>(lua_tointeger(state, -1));
                changed = changed || mxcsr != vectors.Mxcsr;
                vectors.Mxcsr = mxcsr;
            }
            lua_pop(state, 1);
            return changed;
        }

        int DebugGetContext(lua_State* state)
        {
            const Debug::BreakInfo info = Owner(state).Debugger().Break();
            if (!info.Broken)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            PushRegisters(state, info.Registers, info.Is64Bit);
            if (lua_toboolean(state, 1))
                PushVectors(state, info.Vectors);
            lua_pushboolean(state, 1);
            return 1;
        }

        int DebugSetContext(lua_State* state)
        {
            const Debug::BreakInfo info = Owner(state).Debugger().Break();
            if (!info.Broken)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            Disasm::RegisterValues registers = info.Registers;
            if (PullRegisters(state, registers, info.Is64Bit))
                Owner(state).Debugger().SetRegisters(registers);
            Debug::VectorRegisters vectors = info.Vectors;
            if (lua_toboolean(state, 1) && vectors.Count > 0 && PullVectors(state, vectors))
                Owner(state).Debugger().SetVectors(vectors);
            lua_pushboolean(state, 1);
            return 1;
        }

        int DebugBreakNow(lua_State* state)
        {
            Status status = Owner(state).EnsureDebugger();
            if (status)
                status = Owner(state).Debugger().BreakNow();
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int SetFallback(lua_State* state)
        {
            const char* kind = luaL_checkstring(state, 1);
            luaL_checktype(state, 2, LUA_TFUNCTION);
            lua_getfield(state, LUA_REGISTRYINDEX, kFallbackKey);
            lua_pushvalue(state, 2);
            lua_setfield(state, -2, kind);
            lua_pop(state, 1);
            return 0;
        }

        int RecordIdOf(lua_State* state)
        {
            if (auto* handle = static_cast<Handle*>(luaL_testudata(state, 1, kRecordType)))
                lua_pushinteger(state, handle->Id);
            else
                lua_pushnil(state);
            return 1;
        }

        struct RegisterName
        {
            const char* Wide;
            const char* Narrow;
            std::uint64_t Disasm::RegisterValues::*Field;
        };

        const RegisterName kRegisters[] = {
            { "RAX", "EAX", &Disasm::RegisterValues::Rax }, { "RBX", "EBX", &Disasm::RegisterValues::Rbx }, { "RCX", "ECX", &Disasm::RegisterValues::Rcx },
            { "RDX", "EDX", &Disasm::RegisterValues::Rdx }, { "RSI", "ESI", &Disasm::RegisterValues::Rsi }, { "RDI", "EDI", &Disasm::RegisterValues::Rdi },
            { "RBP", "EBP", &Disasm::RegisterValues::Rbp }, { "RSP", "ESP", &Disasm::RegisterValues::Rsp }, { "RIP", "EIP", &Disasm::RegisterValues::Rip },
            { "R8", nullptr, &Disasm::RegisterValues::R8 }, { "R9", nullptr, &Disasm::RegisterValues::R9 }, { "R10", nullptr, &Disasm::RegisterValues::R10 },
            { "R11", nullptr, &Disasm::RegisterValues::R11 }, { "R12", nullptr, &Disasm::RegisterValues::R12 }, { "R13", nullptr, &Disasm::RegisterValues::R13 },
            { "R14", nullptr, &Disasm::RegisterValues::R14 }, { "R15", nullptr, &Disasm::RegisterValues::R15 }, { "EFLAGS", "EFLAGS", &Disasm::RegisterValues::Flags },
        };

        void Metatable(lua_State* state, const char* name, lua_CFunction index, lua_CFunction newIndex, lua_CFunction extra = nullptr, const char* extraName = nullptr)
        {
            luaL_newmetatable(state, name);
            lua_pushcfunction(state, index);
            lua_setfield(state, -2, "__index");
            if (newIndex)
            {
                lua_pushcfunction(state, newIndex);
                lua_setfield(state, -2, "__newindex");
            }
            lua_pushcfunction(state, &HandleEqual);
            lua_setfield(state, -2, "__eq");
            if (extra)
            {
                lua_pushcfunction(state, extra);
                lua_setfield(state, -2, extraName);
            }
            lua_pop(state, 1);
        }
    }

    void PushBound(lua_State* state, int objectIndex)
    {
        objectIndex = lua_absindex(state, objectIndex);
        lua_pushvalue(state, objectIndex);
        lua_insert(state, -2);
        lua_pushcclosure(state, &BoundCall, 2);
    }

    void PushTimer(lua_State* state, int id)
    {
        PushHandle(state, kTimerType, id);
    }

    void PushRecord(lua_State* state, const Table::MemoryRecord* record)
    {
        if (!record)
        {
            lua_pushnil(state);
            return;
        }
        lua_getfield(state, LUA_REGISTRYINDEX, kRecordCacheKey);
        lua_rawgeti(state, -1, record->Id);
        if (lua_isuserdata(state, -1))
        {
            lua_remove(state, -2);
            return;
        }
        lua_pop(state, 1);
        PushHandle(state, kRecordType, record->Id);
        lua_pushvalue(state, -1);
        lua_rawseti(state, -3, record->Id);
        lua_remove(state, -2);
    }

    void PushAddressList(lua_State* state)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kListKey);
        if (lua_isuserdata(state, -1))
            return;
        lua_pop(state, 1);
        PushHandle(state, kAddressListType, 0);
        lua_pushvalue(state, -1);
        lua_setfield(state, LUA_REGISTRYINDEX, kListKey);
    }

    std::vector<int> RecordsWithFunction(lua_State* state, std::string_view key)
    {
        std::vector<int> ids;
        lua_getfield(state, LUA_REGISTRYINDEX, kExtrasKey);
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return ids;
        }
        lua_pushnil(state);
        while (lua_next(state, -2))
        {
            if (lua_isinteger(state, -2) && lua_istable(state, -1))
            {
                lua_pushlstring(state, key.data(), key.size());
                lua_rawget(state, -2);
                if (lua_isfunction(state, -1))
                    ids.push_back(static_cast<int>(lua_tointeger(state, -3)));
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
        return ids;
    }

    bool PushExtra(lua_State* state, int id, std::string_view key)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kExtrasKey);
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return false;
        }
        lua_rawgeti(state, -1, id);
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 2);
            return false;
        }
        lua_pushlstring(state, key.data(), key.size());
        lua_rawget(state, -2);
        if (lua_isnil(state, -1))
        {
            lua_pop(state, 3);
            return false;
        }
        lua_replace(state, -3);
        lua_pop(state, 1);
        return true;
    }

    void DropExtras(lua_State* state, int id)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kExtrasKey);
        if (lua_istable(state, -1))
        {
            lua_pushnil(state);
            lua_rawseti(state, -2, id);
        }
        lua_pop(state, 1);
        lua_getfield(state, LUA_REGISTRYINDEX, kRecordCacheKey);
        if (lua_istable(state, -1))
        {
            lua_pushnil(state);
            lua_rawseti(state, -2, id);
        }
        lua_pop(state, 1);
    }

    void ForgetRecords(lua_State* state)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kExtrasKey);
        if (lua_istable(state, -1))
        {
            std::vector<lua_Integer> stale;
            lua_pushnil(state);
            while (lua_next(state, -2))
            {
                if (lua_isinteger(state, -2) && lua_tointeger(state, -2) >= 0)
                    stale.push_back(lua_tointeger(state, -2));
                lua_pop(state, 1);
            }
            for (lua_Integer id : stale)
            {
                lua_pushnil(state);
                lua_rawseti(state, -2, id);
            }
        }
        lua_pop(state, 1);
        lua_newtable(state);
        lua_newtable(state);
        lua_pushstring(state, "v");
        lua_setfield(state, -2, "__mode");
        lua_setmetatable(state, -2);
        lua_setfield(state, LUA_REGISTRYINDEX, kRecordCacheKey);
    }

    void PushRegisters(lua_State* state, const Disasm::RegisterValues& registers, bool is64Bit)
    {
        for (const RegisterName& name : kRegisters)
        {
            const std::uint64_t value = registers.*name.Field;
            if (is64Bit || !name.Narrow)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
                lua_setglobal(state, name.Wide);
            }
            if (name.Narrow)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(value & 0xFFFFFFFFull));
                lua_setglobal(state, name.Narrow);
            }
        }
    }

    bool PullRegisters(lua_State* state, Disasm::RegisterValues& registers, bool is64Bit)
    {
        bool changed = false;
        for (const RegisterName& name : kRegisters)
        {
            const char* global = is64Bit ? name.Wide : name.Narrow;
            if (!global)
                continue;
            lua_getglobal(state, global);
            if (lua_isinteger(state, -1))
            {
                const std::uint64_t value = static_cast<std::uint64_t>(lua_tointeger(state, -1));
                const std::uint64_t old = registers.*name.Field;
                const std::uint64_t masked = is64Bit ? value : (value & 0xFFFFFFFFull);
                if (masked != (is64Bit ? old : (old & 0xFFFFFFFFull)))
                {
                    registers.*name.Field = masked;
                    changed = true;
                }
            }
            lua_pop(state, 1);
        }
        return changed;
    }

    bool IsCooperativeThread(lua_State* thread)
    {
        lua_getfield(thread, LUA_REGISTRYINDEX, kThreadsKey);
        if (!lua_istable(thread, -1))
        {
            lua_pop(thread, 1);
            return false;
        }
        lua_pushthread(thread);
        lua_rawget(thread, -2);
        const bool found = !lua_isnil(thread, -1);
        lua_pop(thread, 2);
        return found;
    }

    void ResumeThreads(LuaEngine& engine, lua_State* state, double now)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kThreadsKey);
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return;
        }
        std::vector<lua_State*> due;
        lua_pushnil(state);
        while (lua_next(state, -2))
        {
            const double wake = lua_tonumber(state, -1);
            lua_State* thread = lua_tothread(state, -2);
            if (thread && wake <= now)
                due.push_back(thread);
            lua_pop(state, 1);
        }
        for (lua_State* thread : due)
        {
            const int status = lua_status(thread);
            int results = 0;
            int arguments = 0;
            if (status == LUA_OK && lua_gettop(thread) >= 1 && lua_isfunction(thread, 1))
                arguments = lua_gettop(thread) - 1;
            const int resumed = lua_resume(thread, state, arguments, &results);
            if (resumed == LUA_YIELD)
            {
                const lua_Integer milliseconds = results > 0 && lua_isinteger(thread, -1) ? lua_tointeger(thread, -1) : 0;
                lua_pop(thread, results);
                lua_pushthread(thread);
                lua_xmove(thread, state, 1);
                lua_pushnumber(state, now + static_cast<double>(milliseconds) / 1000.0);
                lua_rawset(state, -3);
                continue;
            }
            if (resumed != LUA_OK)
            {
                const char* message = lua_tostring(thread, -1);
                luaL_traceback(state, thread, message ? message : "error", 0);
                engine.PrintError(std::string("thread: ") + (lua_tostring(state, -1) ? lua_tostring(state, -1) : "error"));
                lua_pop(state, 1);
            }
            lua_pushthread(thread);
            lua_xmove(thread, state, 1);
            lua_pushnil(state);
            lua_rawset(state, -3);
        }
        lua_pop(state, 1);
    }

    void RegisterObjects(LuaEngine& engine, lua_State* state)
    {
        (void)engine;
        Metatable(state, kAddressListType, &AddressListIndex, &AddressListNewIndex);
        Metatable(state, kRecordType, &RecordIndex, &RecordNewIndex, &RecordToString, "__tostring");
        Metatable(state, kPartType, &PartIndex, &PartNewIndex);
        Metatable(state, kIndexerType, &IndexerIndex, &IndexerNewIndex, &IndexerLength, "__len");
        Metatable(state, kDropDownType, &DropDownIndex, &DropDownNewIndex, &DropDownLength, "__len");
        Metatable(state, kRecordHotkeyType, &RecordHotkeyIndex, &RecordHotkeyNewIndex);
        Metatable(state, kTimerType, &TimerIndex, &TimerNewIndex);
        Metatable(state, kHotkeyType, &HotkeyIndex, nullptr);

        lua_newtable(state);
        lua_setfield(state, LUA_REGISTRYINDEX, kThreadsKey);
        lua_newtable(state);
        lua_setfield(state, LUA_REGISTRYINDEX, kExtrasKey);
        lua_newtable(state);
        lua_setfield(state, LUA_REGISTRYINDEX, kFallbackKey);
        lua_newtable(state);
        lua_newtable(state);
        lua_pushstring(state, "v");
        lua_setfield(state, -2, "__mode");
        lua_setmetatable(state, -2);
        lua_setfield(state, LUA_REGISTRYINDEX, kRecordCacheKey);

        const luaL_Reg functions[] = {
            { "getAddressList", &GetAddressList },
            { "createTimer", &CreateTimer },
            { "createHotkey", &CreateHotkey },
            { "createThread", &CreateThread },
            { "debugProcess", &DebugProcess },
            { "debug_setBreakpoint", &DebugSetBreakpoint },
            { "debug_removeBreakpoint", &DebugRemoveBreakpoint },
            { "debug_continueFromBreakpoint", &DebugContinue },
            { "debug_isBroken", &DebugIsBroken },
            { "debug_isDebugging", &DebugIsDebugging },
            { "debug_getBreakpointList", &DebugBreakpointList },
            { "debug_getContext", &DebugGetContext },
            { "debug_setContext", &DebugSetContext },
            { "debug_breakThread", &DebugBreakNow },
            { "__manifoldSetFallback", &SetFallback },
            { "__manifoldRecordId", &RecordIdOf },
            { nullptr, nullptr },
        };
        for (const luaL_Reg* function = functions; function->name; ++function)
        {
            lua_pushcfunction(state, function->func);
            lua_setglobal(state, function->name);
            if (function->name[0] != '_')
                NoteFunction(function->name);
        }
        PushAddressList(state);
        lua_setglobal(state, "AddressList");
    }
}
