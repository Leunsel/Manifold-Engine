#include "Lua/LuaRecords.h"

#include "App/Engine.h"
#include "Core/MainThread.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaEngine.h"
#include "Scan/ValueType.h"
#include "Table/AddressList.h"
#include "Table/RecordPath.h"

#include "lua.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace Manifold::Lua
{
    namespace
    {
        using Table::MemoryRecord;
        using Table::RecordKind;

        // ∑ A record is named by its id and the table it came from: a table
        // that was replaced (new, opened, restored) may reuse ids, so a handle
        // from the old one must not reach a record of the new one.
        MemoryRecord* Find(lua_State* state, int index)
        {
            Engine& engine = Owner(state);
            const lua_Integer identity = luaL_checkinteger(state, index + 1);
            if (static_cast<std::uint64_t>(identity) != engine.Table().Identity())
                return nullptr;
            return engine.Table().FindById(static_cast<int>(luaL_checkinteger(state, index)));
        }

        int Gone(lua_State* state, int index)
        {
            return PushFailure(state, ErrorCode::TableRecordGone, std::to_string(luaL_checkinteger(state, index)));
        }

        const char* KindName(RecordKind kind)
        {
            switch (kind)
            {
            case RecordKind::Script: return "script";
            case RecordKind::Group: return "group";
            default: return "value";
            }
        }

        struct TypeEntry
        {
            const char* Name;
            Scan::ValueType Type;
        };

        constexpr TypeEntry kTypes[] = {
            { "byte", Scan::ValueType::Byte },
            { "2bytes", Scan::ValueType::TwoBytes },
            { "4bytes", Scan::ValueType::FourBytes },
            { "8bytes", Scan::ValueType::EightBytes },
            { "float", Scan::ValueType::Float },
            { "double", Scan::ValueType::Double },
            { "string", Scan::ValueType::String },
            { "bytes", Scan::ValueType::ByteArray },
            { "binary", Scan::ValueType::Binary },
            { "pointer", Scan::ValueType::Pointer },
        };

        const char* RecordTypeName(Scan::ValueType type)
        {
            for (const TypeEntry& entry : kTypes)
            {
                if (entry.Type == type)
                    return entry.Name;
            }
            return "4bytes";
        }

        void PushIds(lua_State* state, const std::vector<MemoryRecord*>& records)
        {
            lua_createtable(state, static_cast<int>(records.size()), 0);
            lua_Integer index = 1;
            for (const MemoryRecord* record : records)
            {
                lua_pushinteger(state, record->Id);
                lua_rawseti(state, -2, index++);
            }
        }

        int RecordIdentity(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(Owner(state).Table().Identity()));
            return 1;
        }

        // find(id or description) -> id, or nil, code, message
        int RecordFind(lua_State* state)
        {
            Table::AddressList& table = Owner(state).Table();
            MemoryRecord* record = nullptr;
            std::string shown;
            if (lua_type(state, 1) == LUA_TNUMBER)
            {
                record = table.FindById(static_cast<int>(lua_tointeger(state, 1)));
                shown = std::to_string(lua_tointeger(state, 1));
            }
            else
            {
                // ∑ a description names a record only when no other record has it
                const std::string description = luaL_checkstring(state, 1);
                record = table.FindByDescription(description);
                shown = "\"" + description + "\"";
                if (const int count = record ? table.CountByDescription(description) : 0; count > 1)
                    return PushFailure(state, ErrorCode::TableRecordAmbiguous, shown + " (" + std::to_string(count) + " records)");
            }
            if (!record)
                return PushFailure(state, ErrorCode::TableRecordNotFound, shown);
            lua_pushinteger(state, record->Id);
            return 1;
        }

        int RecordFindAll(lua_State* state)
        {
            const std::string description = luaL_checkstring(state, 1);
            std::vector<MemoryRecord*> found;
            for (MemoryRecord* record : Owner(state).Table().Flatten())
            {
                if (record->Description == description)
                    found.push_back(record);
            }
            PushIds(state, found);
            return 1;
        }

        // ∑ One record or an error: none is ME-6008, several are ME-6011 with the count, so a script never gets "the first of
        //   three" without knowing.
        int PushOne(lua_State* state, const std::vector<MemoryRecord*>& found, const std::string& shown)
        {
            if (found.empty())
                return PushFailure(state, ErrorCode::TableRecordNotFound, shown);
            if (found.size() > 1)
                return PushFailure(state, ErrorCode::TableRecordAmbiguous, shown + " (" + std::to_string(found.size()) + " records)");
            lua_pushinteger(state, found.front()->Id);
            return 1;
        }

        std::vector<std::string> PathArgument(lua_State* state, int index)
        {
            luaL_checktype(state, index, LUA_TTABLE);
            std::vector<std::string> path;
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, index));
            for (lua_Integer step = 1; step <= count; ++step)
            {
                lua_rawgeti(state, index, step);
                if (lua_type(state, -1) != LUA_TSTRING)
                    luaL_error(state, "step %d of the path is a %s, not a description", static_cast<int>(step), luaL_typename(state, -1));
                path.emplace_back(lua_tostring(state, -1));
                lua_pop(state, 1);
            }
            if (path.empty())
                luaL_error(state, "the path is empty");
            return path;
        }

        // findPath(path, fromId?, identity?) -> id, or nil, code, message
        int RecordFindPath(lua_State* state)
        {
            const std::vector<std::string> path = PathArgument(state, 1);
            MemoryRecord* from = nullptr;
            if (!lua_isnoneornil(state, 2))
            {
                from = Find(state, 2);
                if (!from)
                    return Gone(state, 2);
            }
            Table::AddressList& table = Owner(state).Table();
            return PushOne(state, Table::FindByPath(table, path, from), "\"" + Table::PathText(path) + "\"" + (from ? " below \"" + from->Description + "\"" : std::string()));
        }

        // findBelow(id, identity, description) -> id, or nil, code, message
        int RecordFindBelow(lua_State* state)
        {
            MemoryRecord* from = Find(state, 1);
            if (!from)
                return Gone(state, 1);
            const std::string description = luaL_checkstring(state, 3);
            return PushOne(state, Table::FindBelow(*from, description), "\"" + description + "\" below \"" + from->Description + "\"");
        }

        int RecordPathOf(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            const std::vector<std::string> path = Table::RecordPath(*record);
            lua_createtable(state, static_cast<int>(path.size()), 0);
            lua_Integer index = 1;
            for (const std::string& step : path)
            {
                lua_pushlstring(state, step.data(), step.size());
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int RecordAll(lua_State* state)
        {
            PushIds(state, Owner(state).Table().Flatten());
            return 1;
        }

        int RecordSelected(lua_State* state)
        {
            Table::AddressList& table = Owner(state).Table();
            std::vector<MemoryRecord*> records;
            for (const int id : table.Selection())
            {
                if (MemoryRecord* record = table.FindById(id))
                    records.push_back(record);
            }
            PushIds(state, records);
            return 1;
        }

        int RecordChildren(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            std::vector<MemoryRecord*> children;
            for (const auto& child : record->Children)
                children.push_back(child.get());
            PushIds(state, children);
            return 1;
        }

        int RecordInfo(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            Engine& engine = Owner(state);
            lua_createtable(state, 0, 12);
            lua_pushinteger(state, record->Id);
            lua_setfield(state, -2, "id");
            lua_pushstring(state, record->Description.c_str());
            lua_setfield(state, -2, "description");
            lua_pushstring(state, KindName(record->Kind));
            lua_setfield(state, -2, "kind");
            if (record->Kind == RecordKind::Value)
            {
                lua_pushstring(state, record->CustomTypeName.empty() ? RecordTypeName(record->Type) : record->CustomTypeName.c_str());
                lua_setfield(state, -2, "type");
                lua_pushstring(state, record->Address.c_str());
                lua_setfield(state, -2, "address");
                lua_pushinteger(state, static_cast<lua_Integer>(record->Offsets.size()));
                lua_setfield(state, -2, "offsets");
                engine.ResolveRecord(*record);
                if (record->AddressOk)
                {
                    lua_pushinteger(state, static_cast<lua_Integer>(record->ResolvedAddress));
                    lua_setfield(state, -2, "resolved");
                }
                lua_pushboolean(state, record->ShowHex ? 1 : 0);
                lua_setfield(state, -2, "hex");
            }
            lua_pushboolean(state, record->Active ? 1 : 0);
            lua_setfield(state, -2, "active");
            if (record->Parent)
            {
                lua_pushinteger(state, record->Parent->Id);
                lua_setfield(state, -2, "parent");
            }
            lua_pushinteger(state, static_cast<lua_Integer>(record->Children.size()));
            lua_setfield(state, -2, "children");
            lua_pushinteger(state, static_cast<lua_Integer>(record->Hotkeys.size()));
            lua_setfield(state, -2, "hotkeys");
            if (!record->Comment.empty())
            {
                lua_pushstring(state, record->Comment.c_str());
                lua_setfield(state, -2, "comment");
            }
            return 1;
        }

        // getValue -> value (a number for number types shown as numbers, else
        // the text) and the text as the address list shows it
        int RecordGet(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            if (record->Kind != RecordKind::Value)
                return PushFailure(state, ErrorCode::TableRecordNoValue, "\"" + record->Description + "\" (a " + KindName(record->Kind) + ")");
            Engine& engine = Owner(state);
            engine.ResolveRecord(*record);
            if (!record->AddressOk)
                return PushFailure(state, ErrorCode::ReadFailed, record->AddressError.empty() ? record->Address : record->AddressError);
            const std::vector<std::uint8_t> bytes = engine.ReadRecord(*record);
            if (bytes.empty())
                return PushFailure(state, ErrorCode::ReadFailed, engine.Tables().AddressHex(record->ResolvedAddress));
            const std::string text = engine.FormatRecord(*record, bytes);
            // ∑ the number is the value itself, also when the address list shows it
            // as hex or as a drop-down entry; the text is what it shows
            const bool numeric = record->CustomTypeName.empty() &&
                                 (record->Type == Scan::ValueType::Byte || record->Type == Scan::ValueType::TwoBytes || record->Type == Scan::ValueType::FourBytes ||
                                  record->Type == Scan::ValueType::EightBytes || record->Type == Scan::ValueType::Float || record->Type == Scan::ValueType::Double);
            const std::optional<double> number = numeric ? Scan::AsDouble(record->Type, bytes, record->ShowSigned) : std::nullopt;
            if (number && record->Type != Scan::ValueType::Float && record->Type != Scan::ValueType::Double && std::abs(*number) < 9.0e18)
                lua_pushinteger(state, static_cast<lua_Integer>(*number));
            else if (number)
                lua_pushnumber(state, *number);
            else
                lua_pushlstring(state, text.data(), text.size());
            lua_pushlstring(state, text.data(), text.size());
            // the drop-down entry the value matches, by its description
            for (const Table::DropDownEntry& entry : engine.DropDownOf(*record))
            {
                if (EqualsNoCase(text, entry.Text) || EqualsNoCase(text, entry.Value + " : " + entry.Text))
                {
                    lua_pushlstring(state, entry.Text.data(), entry.Text.size());
                    return 3;
                }
            }
            return 2;
        }

        // setValue(number or text): text as typed into the address list
        int RecordSet(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            if (record->Kind != RecordKind::Value)
                return PushFailure(state, ErrorCode::TableRecordNoValue, "\"" + record->Description + "\" (a " + KindName(record->Kind) + ")");
            std::string text;
            if (lua_isinteger(state, 3))
            {
                const lua_Integer value = lua_tointeger(state, 3);
                char buffer[32];
                if (record->ShowHex)
                    std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
                else
                    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
                text = buffer;
            }
            else if (lua_type(state, 3) == LUA_TNUMBER)
            {
                char buffer[48];
                std::snprintf(buffer, sizeof(buffer), "%.17g", lua_tonumber(state, 3));
                text = buffer;
            }
            else
                text = luaL_checkstring(state, 3);
            Engine& engine = Owner(state);
            const Status status = engine.WriteRecord(*record, text);
            engine.Table().Refresh();
            if (!status)
                return PushFailure(state, status);
            lua_pushboolean(state, 1);
            return 1;
        }

        int ActivationDone(lua_State* state, int, lua_KContext context)
        {
            if (Owner(state).ActivationPending(static_cast<int>(context)))
                return lua_yieldk(state, 0, context, &ActivationDone);
            lua_pushboolean(state, 1);
            return 1;
        }

        // ∑ the same way as memrec.Active: inside a script's own coroutine
        // with async activation on, the activation is queued and this waits
        // for it without blocking the program; otherwise it runs right away
        int RecordSetActive(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            const bool wanted = lua_toboolean(state, 3) != 0;
            Engine& engine = Owner(state);
            if (engine.Async() && Manifold::OnMainThread() && lua_isyieldable(state) && EngineOf(state).IsCooperative(state))
            {
                if (!engine.InsideActivation())
                {
                    engine.RequestActive(*record, wanted, true);
                    if (engine.ActivationPending(record->Id))
                        return lua_yieldk(state, 0, static_cast<lua_KContext>(record->Id), &ActivationDone);
                    lua_pushboolean(state, 1);
                    return 1;
                }
                engine.DropQueued(record->Id);
            }
            const Status status = engine.SetActive(*record, wanted, true);
            if (!status)
                return PushFailure(state, status);
            lua_pushboolean(state, 1);
            return 1;
        }

        // ∑ a form's switch or button: like a click on the record's box, queued
        // when activation runs in the background (a long [ENABLE] does not
        // freeze the program), and noted as a switch by hand
        int RecordRequestActive(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            const bool wanted = lua_toboolean(state, 3) != 0;
            Engine& engine = Owner(state);
            if (engine.Async() && !engine.InsideActivation())
                engine.RequestActive(*record, wanted, false);
            else
            {
                const Status status = engine.SetActive(*record, wanted, false);
                if (!status)
                    return PushFailure(state, status);
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int RecordPending(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            lua_pushboolean(state, record && Owner(state).ActivationPending(record->Id) ? 1 : 0);
            return 1;
        }

        int RecordIsActive(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            lua_pushboolean(state, record->Active ? 1 : 0);
            return 1;
        }

        int RecordSetDescription(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            record->Description = luaL_checkstring(state, 3);
            Owner(state).Table().Touch();
            lua_pushboolean(state, 1);
            return 1;
        }

        int RecordAddress(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            if (record->Kind != RecordKind::Value)
                return PushFailure(state, ErrorCode::TableRecordNoValue, "\"" + record->Description + "\" (a " + KindName(record->Kind) + ")");
            Owner(state).ResolveRecord(*record);
            if (!record->AddressOk)
                return PushFailure(state, ErrorCode::ReadFailed, record->AddressError.empty() ? record->Address : record->AddressError);
            lua_pushinteger(state, static_cast<lua_Integer>(record->ResolvedAddress));
            return 1;
        }

        // create(kind, description, address or script, type name, parent id, parent identity) -> id
        int RecordCreate(lua_State* state)
        {
            Engine& engine = Owner(state);
            Table::AddressList& table = engine.Table();
            const std::string kind = luaL_checkstring(state, 1);
            const std::string description = luaL_optstring(state, 2, "");
            MemoryRecord* parent = nullptr;
            if (!lua_isnoneornil(state, 5))
            {
                parent = Find(state, 5);
                if (!parent)
                    return Gone(state, 5);
            }
            MemoryRecord* made = nullptr;
            if (kind == "group")
                made = &table.AddGroup(description, parent);
            else if (kind == "script")
                made = &table.AddScript(description, luaL_optstring(state, 3, ""), parent);
            else
            {
                Scan::ValueType type = Scan::ValueType::FourBytes;
                const std::string wanted = Lower(luaL_optstring(state, 4, "4bytes"));
                bool known = false;
                for (const TypeEntry& entry : kTypes)
                {
                    if (wanted == entry.Name || wanted == Lower(Scan::TypeName(entry.Type)))
                    {
                        type = entry.Type;
                        known = true;
                    }
                }
                if (!known)
                    return luaL_error(state, "unknown record type %s", wanted.c_str());
                made = &table.AddValue(description, luaL_optstring(state, 3, ""), type, parent);
            }
            table.Touch();
            lua_pushinteger(state, made->Id);
            return 1;
        }

        int RecordRemove(lua_State* state)
        {
            MemoryRecord* record = Find(state, 1);
            if (!record)
                return Gone(state, 1);
            Engine& engine = Owner(state);
            if (record->Active)
            {
                const Status status = engine.SetActive(*record, false, true);
                if (!status)
                    return PushFailure(state, status);
            }
            const int id = record->Id;
            engine.Table().Remove(*record);
            EngineOf(state).RecordDestroyed(id);
            engine.Table().Touch();
            lua_pushboolean(state, 1);
            return 1;
        }
    }

    void RegisterRecordNatives(lua_State* state, int natives)
    {
        static const luaL_Reg functions[] = {
            { "recordIdentity", &RecordIdentity },
            { "recordFind", &RecordFind },
            { "recordFindPath", &RecordFindPath },
            { "recordFindBelow", &RecordFindBelow },
            { "recordPath", &RecordPathOf },
            { "recordAll", &RecordAll },
            { "recordFindAll", &RecordFindAll },
            { "recordSelected", &RecordSelected },
            { "recordChildren", &RecordChildren },
            { "recordInfo", &RecordInfo },
            { "recordGet", &RecordGet },
            { "recordSet", &RecordSet },
            { "recordSetActive", &RecordSetActive },
            { "recordIsActive", &RecordIsActive },
            { "recordRequestActive", &RecordRequestActive },
            { "recordPending", &RecordPending },
            { "recordSetDescription", &RecordSetDescription },
            { "recordAddress", &RecordAddress },
            { "recordCreate", &RecordCreate },
            { "recordRemove", &RecordRemove },
            { nullptr, nullptr },
        };
        const int absolute = lua_absindex(state, natives);
        for (const luaL_Reg* entry = functions; entry->name; ++entry)
        {
            lua_pushcfunction(state, entry->func);
            lua_setfield(state, absolute, entry->name);
        }
    }
}
