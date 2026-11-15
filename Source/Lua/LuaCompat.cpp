#include "Lua/LuaApi.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Core/RuntimeLog.h"
#include "Lua/LuaCompatScripts.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include <windows.h>
#include <bcrypt.h>
#include <wininet.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kStreamType = "Manifold.Stream";
        constexpr const char* kTableFileType = "Manifold.TableFile";
        constexpr const char* kMemScanType = "Manifold.MemScan";
        constexpr const char* kFoundListType = "Manifold.FoundList";
        constexpr const char* kStructureType = "Manifold.Structure";
        constexpr const char* kElementType = "Manifold.StructureElement";
        constexpr const char* kElementListType = "Manifold.StructureElements";
        constexpr const char* kDetachedKey = "Manifold.DetachedStructures";
        constexpr const char* kCustomTypeType = "Manifold.CustomType";
        constexpr const char* kInternetType = "Manifold.Internet";

        Engine& Owner(lua_State* state)
        {
            return EngineOf(state).Owner();
        }

        std::string Key(lua_State* state, int index)
        {
            return lua_type(state, index) == LUA_TSTRING ? Lower(lua_tostring(state, index)) : std::string();
        }

        bool ReadLocal(std::uint64_t address, void* buffer, std::size_t size)
        {
            SIZE_T done = 0;
            return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer, size, &done) && done == size;
        }

        bool WriteLocal(std::uint64_t address, const void* data, std::size_t size)
        {
            SIZE_T done = 0;
            return ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPVOID>(address), data, size, &done) && done == size;
        }

        std::uint64_t Number(lua_State* state, int index)
        {
            if (lua_isinteger(state, index))
                return static_cast<std::uint64_t>(lua_tointeger(state, index));
            if (lua_type(state, index) == LUA_TNUMBER)
                return static_cast<std::uint64_t>(static_cast<std::int64_t>(lua_tonumber(state, index)));
            if (lua_type(state, index) == LUA_TSTRING)
            {
                if (const auto value = ParseHex(lua_tostring(state, index)))
                    return *value;
            }
            return 0;
        }

        std::string Md5(const void* data, std::size_t size)
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0)
                return {};
            std::uint8_t digest[16]{};
            const LONG status = ::BCryptHash(algorithm, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), digest, sizeof(digest));
            ::BCryptCloseAlgorithmProvider(algorithm, 0);
            if (status < 0)
                return {};
            return Lower(HexBytes(digest, ""));
        }

        std::optional<std::vector<std::uint8_t>> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::nullopt;
            return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        bool WriteFile(const std::filesystem::path& path, std::span<const std::uint8_t> data)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            return static_cast<bool>(file);
        }

        struct StreamState
        {
            std::shared_ptr<std::vector<std::uint8_t>> Data = std::make_shared<std::vector<std::uint8_t>>();
            std::size_t Position = 0;
            bool StringStream = false;
        };

        StreamState& CheckStream(lua_State* state, int index)
        {
            auto* holder = static_cast<std::shared_ptr<StreamState>*>(luaL_checkudata(state, index, kStreamType));
            return **holder;
        }

        void PushStream(lua_State* state, std::shared_ptr<StreamState> stream)
        {
            void* memory = lua_newuserdatauv(state, sizeof(std::shared_ptr<StreamState>), 0);
            new (memory) std::shared_ptr<StreamState>(std::move(stream));
            luaL_setmetatable(state, kStreamType);
        }

        template <class T>
        int StreamRead(lua_State* state)
        {
            StreamState& stream = CheckStream(state, 1);
            T value{};
            if (stream.Position + sizeof(T) <= stream.Data->size())
                std::memcpy(&value, stream.Data->data() + stream.Position, sizeof(T));
            stream.Position = std::min(stream.Data->size(), stream.Position + sizeof(T));
            lua_pushinteger(state, static_cast<lua_Integer>(value));
            return 1;
        }

        void StreamPut(StreamState& stream, const void* data, std::size_t size)
        {
            if (stream.Position + size > stream.Data->size())
                stream.Data->resize(stream.Position + size);
            std::memcpy(stream.Data->data() + stream.Position, data, size);
            stream.Position += size;
        }

        template <class T>
        int StreamWrite(lua_State* state)
        {
            StreamState& stream = CheckStream(state, 1);
            const T value = static_cast<T>(Number(state, 2));
            StreamPut(stream, &value, sizeof(T));
            return 0;
        }

        int StreamIndex(lua_State* state)
        {
            StreamState& stream = CheckStream(state, 1);
            const std::string key = Key(state, 2);
            if (key == "size") { lua_pushinteger(state, static_cast<lua_Integer>(stream.Data->size())); return 1; }
            if (key == "position") { lua_pushinteger(state, static_cast<lua_Integer>(stream.Position)); return 1; }
            if (key == "memory") { lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(stream.Data->data()))); return 1; }
            if (key == "datastring") { lua_pushlstring(state, reinterpret_cast<const char*>(stream.Data->data()), stream.Data->size()); return 1; }
            if (key == "classname") { lua_pushstring(state, stream.StringStream ? "TStringStream" : "TMemoryStream"); return 1; }
            static const luaL_Reg kMethods[] = {
                { "read", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(luaL_optinteger(s, 2, 0)), target.Data->size() - std::min(target.Position, target.Data->size()));
                        lua_createtable(s, static_cast<int>(count), 0);
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            lua_pushinteger(s, (*target.Data)[target.Position + i]);
                            lua_rawseti(s, -2, static_cast<lua_Integer>(i + 1));
                        }
                        target.Position += count;
                        return 1;
                    } },
                { "write", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        luaL_checktype(s, 2, LUA_TTABLE);
                        const lua_Integer length = luaL_len(s, 2);
                        const lua_Integer count = std::min<lua_Integer>(length, luaL_optinteger(s, 3, length));
                        for (lua_Integer i = 1; i <= count; ++i)
                        {
                            lua_rawgeti(s, 2, i);
                            const std::uint8_t byte = static_cast<std::uint8_t>(lua_tointeger(s, -1));
                            lua_pop(s, 1);
                            StreamPut(target, &byte, 1);
                        }
                        return 0;
                    } },
                { "readbyte", &StreamRead<std::uint8_t> },
                { "readword", &StreamRead<std::uint16_t> },
                { "readdword", &StreamRead<std::uint32_t> },
                { "readqword", &StreamRead<std::uint64_t> },
                { "writebyte", &StreamWrite<std::uint8_t> },
                { "writeword", &StreamWrite<std::uint16_t> },
                { "writedword", &StreamWrite<std::uint32_t> },
                { "writeqword", &StreamWrite<std::uint64_t> },
                { "readansistring", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        std::uint32_t length = 0;
                        if (target.Position + 4 <= target.Data->size())
                            std::memcpy(&length, target.Data->data() + target.Position, 4);
                        target.Position = std::min(target.Data->size(), target.Position + 4);
                        const std::size_t count = std::min<std::size_t>(length, target.Data->size() - target.Position);
                        lua_pushlstring(s, reinterpret_cast<const char*>(target.Data->data() + target.Position), count);
                        target.Position += count;
                        return 1;
                    } },
                { "writeansistring", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        std::size_t length = 0;
                        const char* text = luaL_checklstring(s, 2, &length);
                        const std::uint32_t size = static_cast<std::uint32_t>(length);
                        StreamPut(target, &size, 4);
                        StreamPut(target, text, length);
                        return 0;
                    } },
                { "readstring", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        const std::size_t available = target.Data->size() - std::min(target.Position, target.Data->size());
                        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(luaL_optinteger(s, 2, static_cast<lua_Integer>(available))), available);
                        lua_pushlstring(s, reinterpret_cast<const char*>(target.Data->data() + target.Position), count);
                        target.Position += count;
                        return 1;
                    } },
                { "writestring", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        std::size_t length = 0;
                        const char* text = luaL_checklstring(s, 2, &length);
                        StreamPut(target, text, length);
                        if (lua_toboolean(s, 3))
                        {
                            const char zero = 0;
                            StreamPut(target, &zero, 1);
                        }
                        return 0;
                    } },
                { "copyfrom", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        StreamState& source = CheckStream(s, 2);
                        lua_Integer count = luaL_optinteger(s, 3, 0);
                        if (count <= 0)
                        {
                            source.Position = 0;
                            count = static_cast<lua_Integer>(source.Data->size());
                        }
                        const std::size_t available = std::min<std::size_t>(static_cast<std::size_t>(count), source.Data->size() - std::min(source.Position, source.Data->size()));
                        StreamPut(target, source.Data->data() + source.Position, available);
                        source.Position += available;
                        lua_pushinteger(s, static_cast<lua_Integer>(available));
                        return 1;
                    } },
                { "loadfromfile", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        const auto data = ReadFile(Utf8ToWide(luaL_checkstring(s, 2)));
                        if (!data)
                            return luaL_error(s, "the file %s could not be read", lua_tostring(s, 2));
                        *target.Data = *data;
                        target.Position = 0;
                        return 0;
                    } },
                { "loadfromfilenoerror", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        const auto data = ReadFile(Utf8ToWide(luaL_checkstring(s, 2)));
                        if (!data)
                        {
                            lua_pushboolean(s, 0);
                            lua_pushstring(s, "the file could not be read");
                            return 2;
                        }
                        *target.Data = *data;
                        target.Position = 0;
                        lua_pushboolean(s, 1);
                        return 1;
                    } },
                { "savetofile", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        if (!WriteFile(Utf8ToWide(luaL_checkstring(s, 2)), *target.Data))
                            return luaL_error(s, "the file %s could not be written", lua_tostring(s, 2));
                        return 0;
                    } },
                { "savetofilenoerror", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        const bool ok = WriteFile(Utf8ToWide(luaL_checkstring(s, 2)), *target.Data);
                        lua_pushboolean(s, ok);
                        if (!ok)
                        {
                            lua_pushstring(s, "the file could not be written");
                            return 2;
                        }
                        return 1;
                    } },
                { "clear", [](lua_State* s) -> int
                    {
                        StreamState& target = CheckStream(s, 1);
                        target.Data->clear();
                        target.Position = 0;
                        return 0;
                    } },
                { "destroy", [](lua_State*) -> int { return 0; } },
                { "getclassname", [](lua_State* s) -> int { lua_pushstring(s, "TMemoryStream"); return 1; } },
                { nullptr, nullptr },
            };
            for (const luaL_Reg* method = kMethods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return 1;
                }
            }
            lua_pushnil(state);
            return 1;
        }

        int StreamNewIndex(lua_State* state)
        {
            StreamState& stream = CheckStream(state, 1);
            const std::string key = Key(state, 2);
            if (key == "size")
            {
                stream.Data->resize(static_cast<std::size_t>(std::max<lua_Integer>(0, luaL_checkinteger(state, 3))));
                stream.Position = std::min(stream.Position, stream.Data->size());
            }
            else if (key == "position")
                stream.Position = static_cast<std::size_t>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, static_cast<lua_Integer>(stream.Data->size())));
            else if (key == "datastring")
            {
                std::size_t length = 0;
                const char* text = luaL_checklstring(state, 3, &length);
                stream.Data->assign(text, text + length);
                stream.Position = 0;
            }
            return 0;
        }

        int StreamGc(lua_State* state)
        {
            auto* holder = static_cast<std::shared_ptr<StreamState>*>(luaL_checkudata(state, 1, kStreamType));
            holder->~shared_ptr<StreamState>();
            return 0;
        }

        Table::TableFile* CheckTableFile(lua_State* state, int index)
        {
            const char* name = static_cast<const char*>(lua_touserdata(state, index));
            luaL_checkudata(state, index, kTableFileType);
            return Owner(state).Table().FindFile(name);
        }

        void PushTableFile(lua_State* state, const std::string& name)
        {
            char* memory = static_cast<char*>(lua_newuserdatauv(state, name.size() + 1, 0));
            std::memcpy(memory, name.c_str(), name.size() + 1);
            luaL_setmetatable(state, kTableFileType);
        }

        int TableFileIndex(lua_State* state)
        {
            Table::TableFile* file = CheckTableFile(state, 1);
            const std::string key = Key(state, 2);
            if (!file)
            {
                if (key == "delete" || key == "destroy")
                {
                    lua_pushcfunction(state, [](lua_State*) -> int { return 0; });
                    PushBound(state, 1);
                    return 1;
                }
                return luaL_error(state, "the table file was deleted");
            }
            if (key == "name") { lua_pushstring(state, file->Name.c_str()); return 1; }
            if (key == "stream")
            {
                auto stream = std::make_shared<StreamState>();
                stream->Data = file->Data;
                PushStream(state, stream);
                return 1;
            }
            if (key == "classname") { lua_pushstring(state, "TLuafile"); return 1; }
            static const luaL_Reg kMethods[] = {
                { "delete", [](lua_State* s) -> int
                    {
                        const char* name = static_cast<const char*>(lua_touserdata(s, 1));
                        Owner(s).Table().RemoveFile(name);
                        return 0;
                    } },
                { "destroy", [](lua_State*) -> int { return 0; } },
                { "savetofile", [](lua_State* s) -> int
                    {
                        Table::TableFile* target = CheckTableFile(s, 1);
                        if (!target || !WriteFile(Utf8ToWide(luaL_checkstring(s, 2)), *target->Data))
                            return luaL_error(s, "the table file could not be saved");
                        return 0;
                    } },
                { "getdata", [](lua_State* s) -> int
                    {
                        Table::TableFile* target = CheckTableFile(s, 1);
                        auto stream = std::make_shared<StreamState>();
                        if (target)
                            *stream->Data = *target->Data;
                        PushStream(s, stream);
                        return 1;
                    } },
                { nullptr, nullptr },
            };
            for (const luaL_Reg* method = kMethods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return 1;
                }
            }
            lua_pushnil(state);
            return 1;
        }

        int FindTableFile(lua_State* state)
        {
            const std::string name = luaL_checkstring(state, 1);
            Table::TableFile* file = Owner(state).Table().FindFile(name);
            if (!file)
            {
                lua_pushnil(state);
                return 1;
            }
            PushTableFile(state, file->Name);
            return 1;
        }

        int CreateTableFile(lua_State* state)
        {
            const std::string name = luaL_checkstring(state, 1);
            Table::TableFile& file = Owner(state).Table().AddFile(name);
            if (lua_type(state, 2) == LUA_TSTRING)
            {
                const auto data = ReadFile(Utf8ToWide(lua_tostring(state, 2)));
                if (!data)
                    return luaL_error(state, "the file %s could not be read", lua_tostring(state, 2));
                *file.Data = *data;
            }
            PushTableFile(state, file.Name);
            return 1;
        }

        void PushCustomType(lua_State* state, const std::string& name)
        {
            const CustomType* type = EngineOf(state).FindCustomType(name);
            CustomTypeObject* object = type ? type->Object.get() : nullptr;
            char* memory = static_cast<char*>(lua_newuserdatauv(state, sizeof(object) + name.size() + 1, 0));
            std::memcpy(memory, &object, sizeof(object));
            std::memcpy(memory + sizeof(object), name.c_str(), name.size() + 1);
            luaL_setmetatable(state, kCustomTypeType);
        }

        const char* CustomTypeName(lua_State* state, int index)
        {
            return static_cast<const char*>(luaL_checkudata(state, index, kCustomTypeType)) + sizeof(CustomTypeObject*);
        }

        CustomType& CheckCustomType(lua_State* state, int index)
        {
            const char* name = CustomTypeName(state, index);
            CustomType* type = EngineOf(state).FindCustomType(name);
            if (!type)
                luaL_error(state, "the custom type %s is not registered", name);
            return *type;
        }

        const CustomType& CheckCustomTypeName(lua_State* state, const char* name)
        {
            const CustomType* type = EngineOf(state).FindCustomType(name);
            if (!type)
                luaL_error(state, "the custom type %s is not registered", name);
            return *type;
        }

        int RegisterStructureCallback(lua_State* state, StructureCallback kind)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            lua_pushvalue(state, 1);
            const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_pushinteger(state, EngineOf(state).AddStructureCallback(kind, reference));
            return 1;
        }

        int RegisterStructureNameLookup(lua_State* state)
        {
            return RegisterStructureCallback(state, StructureCallback::NameLookup);
        }

        int UnregisterStructureNameLookup(lua_State* state)
        {
            lua_pushboolean(state, EngineOf(state).RemoveStructureCallback(StructureCallback::NameLookup, static_cast<int>(luaL_checkinteger(state, 1))));
            return 1;
        }

        int RegisterStructureDissectOverride(lua_State* state)
        {
            return RegisterStructureCallback(state, StructureCallback::DissectOverride);
        }

        int UnregisterStructureDissectOverride(lua_State* state)
        {
            lua_pushboolean(state, EngineOf(state).RemoveStructureCallback(StructureCallback::DissectOverride, static_cast<int>(luaL_checkinteger(state, 1))));
            return 1;
        }

        int RegisterSymbolLookupCallback(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            const lua_Integer location = luaL_optinteger(state, 2, 5);
            lua_pushvalue(state, 1);
            const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_pushinteger(state, EngineOf(state).AddSymbolCallback(reference, location == 0));
            return 1;
        }

        int UnregisterSymbolLookupCallback(lua_State* state)
        {
            lua_pushboolean(state, EngineOf(state).RemoveSymbolCallback(static_cast<int>(luaL_checkinteger(state, 1))));
            return 1;
        }

        int RegisterCustomTypeLua(lua_State* state)
        {
            CustomType type;
            type.Name = luaL_checkstring(state, 1);
            type.SetByteCount(static_cast<int>(std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, kCustomTypeMaxBytes)));
            luaL_checktype(state, 3, LUA_TFUNCTION);
            luaL_checktype(state, 4, LUA_TFUNCTION);
            lua_pushvalue(state, 3);
            type.ToValue = luaL_ref(state, LUA_REGISTRYINDEX);
            lua_pushvalue(state, 4);
            type.ToBytes = luaL_ref(state, LUA_REGISTRYINDEX);
            type.Float = lua_toboolean(state, 5) != 0;
            type.String = lua_toboolean(state, 6) != 0;
            const std::string name = type.Name;
            EngineOf(state).RegisterCustomType(std::move(type));
            PushCustomType(state, name);
            return 1;
        }

        int GetCustomType(lua_State* state)
        {
            const CustomType* type = EngineOf(state).FindCustomType(luaL_checkstring(state, 1));
            if (!type)
            {
                lua_pushnil(state);
                return 1;
            }
            PushCustomType(state, type->Name);
            return 1;
        }

        int GetCustomTypeList(lua_State* state)
        {
            lua_newtable(state);
            int index = 1;
            for (const std::string& name : EngineOf(state).CustomTypeNames())
            {
                PushCustomType(state, name);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int PushCustomValue(lua_State* state, const CustomType& type, std::span<const std::uint8_t> bytes, std::uint64_t address)
        {
            if (type.String)
            {
                std::string text;
                if (EngineOf(state).CustomToText(type, bytes, address, text))
                    lua_pushlstring(state, text.data(), text.size());
                else
                    lua_pushnil(state);
                return 1;
            }
            double value = 0.0;
            if (EngineOf(state).CustomToNumber(type, bytes, address, value))
                lua_pushnumber(state, value);
            else
                lua_pushnil(state);
            return 1;
        }

        int CustomTypeIndex(lua_State* state)
        {
            const CustomType& type = CheckCustomType(state, 1);
            const std::string key = Key(state, 2);
            if (key == "name" || key == "functiontypename") { lua_pushstring(state, type.Name.c_str()); return 1; }
            if (key == "customtypetype") { lua_pushinteger(state, 1); return 1; }
            if (key == "bytesize") { lua_pushinteger(state, type.ByteCount()); return 1; }
            if (key == "scriptusesfloat") { lua_pushboolean(state, type.Float); return 1; }
            if (key == "scriptusesstring") { lua_pushboolean(state, type.String); return 1; }
            static const luaL_Reg kMethods[] = {
                { "getvalue", [](lua_State* s) -> int
                    {
                        const CustomType& target = CheckCustomType(s, 1);
                        const std::uint64_t address = Number(s, 2);
                        const auto& process = Owner(s).Process();
                        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(target.ByteCount()));
                        if (!process || !process->ReadExact(address, bytes.data(), bytes.size()))
                        {
                            lua_pushnil(s);
                            return 1;
                        }
                        return PushCustomValue(s, target, bytes, address);
                    } },
                { "setvalue", [](lua_State* s) -> int
                    {
                        const CustomType& target = CheckCustomType(s, 1);
                        const std::uint64_t address = Number(s, 2);
                        std::vector<std::uint8_t> bytes;
                        const auto& process = Owner(s).Process();
                        const bool ok = process && EngineOf(s).CustomFromText(target, luaL_tolstring(s, 3, nullptr), address, bytes) && process->Write(address, bytes).Ok();
                        lua_pushboolean(s, ok);
                        return 1;
                    } },
                { "bytetabletovalue", [](lua_State* s) -> int
                    {
                        const CustomType& target = CheckCustomType(s, 1);
                        luaL_checktype(s, 2, LUA_TTABLE);
                        std::vector<std::uint8_t> bytes;
                        for (lua_Integer i = 1; i <= luaL_len(s, 2); ++i)
                        {
                            lua_rawgeti(s, 2, i);
                            bytes.push_back(static_cast<std::uint8_t>(lua_tointeger(s, -1)));
                            lua_pop(s, 1);
                        }
                        bytes.resize(static_cast<std::size_t>(target.ByteCount()), 0);
                        return PushCustomValue(s, target, bytes, Number(s, 3));
                    } },
                { "valuetobytetable", [](lua_State* s) -> int
                    {
                        const CustomType& target = CheckCustomType(s, 1);
                        std::vector<std::uint8_t> bytes;
                        if (!EngineOf(s).CustomFromText(target, luaL_tolstring(s, 2, nullptr), Number(s, 3), bytes))
                        {
                            lua_pushnil(s);
                            return 1;
                        }
                        lua_createtable(s, static_cast<int>(bytes.size()), 0);
                        for (std::size_t i = 0; i < bytes.size(); ++i)
                        {
                            lua_pushinteger(s, bytes[i]);
                            lua_rawseti(s, -2, static_cast<lua_Integer>(i + 1));
                        }
                        return 1;
                    } },
                { nullptr, nullptr },
            };
            for (const luaL_Reg* method = kMethods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return 1;
                }
            }
            lua_pushnil(state);
            return 1;
        }

        int CustomTypeNewIndex(lua_State* state)
        {
            CustomType& type = CheckCustomType(state, 1);
            const std::string key = Key(state, 2);
            if (key == "scriptusesstring")
                type.String = lua_toboolean(state, 3) != 0;
            else if (key == "scriptusesfloat")
                type.Float = lua_toboolean(state, 3) != 0;
            else if (key == "bytesize")
                type.SetByteCount(static_cast<int>(luaL_checkinteger(state, 3)));
            else
                return luaL_error(state, "the custom type has no property %s", lua_tostring(state, 2));
            return 0;
        }

        bool Headless(lua_State* state)
        {
            lua_getglobal(state, "__manifoldHeadless");
            const bool headless = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            return headless;
        }

        struct InternetHandle
        {
            HINTERNET Session = nullptr;
        };

        void CloseInternet(InternetHandle& handle)
        {
            if (handle.Session)
            {
                ::InternetCloseHandle(handle.Session);
                handle.Session = nullptr;
            }
        }

        InternetHandle& CheckInternet(lua_State* state, int index)
        {
            return *static_cast<InternetHandle*>(luaL_checkudata(state, index, kInternetType));
        }

        bool ReadAll(HINTERNET request, std::string& body)
        {
            char buffer[16384];
            DWORD read = 0;
            while (::InternetReadFile(request, buffer, sizeof(buffer), &read) && read > 0)
            {
                body.append(buffer, read);
                if (body.size() > 256u * 1024u * 1024u)
                    return false;
            }
            return true;
        }

        int InternetGetUrl(lua_State* state)
        {
            InternetHandle& handle = CheckInternet(state, 1);
            const std::wstring url = Utf8ToWide(luaL_checkstring(state, 2));
            if (Headless(state) || !handle.Session)
            {
                lua_pushnil(state);
                return 1;
            }
            HINTERNET request = ::InternetOpenUrlW(handle.Session, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
            if (!request)
            {
                lua_pushnil(state);
                return 1;
            }
            std::string body;
            const bool ok = ReadAll(request, body);
            ::InternetCloseHandle(request);
            if (!ok)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushlstring(state, body.data(), body.size());
            return 1;
        }

        int InternetPostUrl(lua_State* state)
        {
            InternetHandle& handle = CheckInternet(state, 1);
            const std::string address = luaL_checkstring(state, 2);
            std::size_t length = 0;
            const char* data = luaL_optlstring(state, 3, "", &length);
            if (Headless(state) || !handle.Session)
            {
                lua_pushnil(state);
                return 1;
            }
            URL_COMPONENTSW parts{ sizeof(parts) };
            wchar_t host[256] = L"";
            wchar_t path[2048] = L"";
            parts.lpszHostName = host;
            parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
            const std::wstring url = Utf8ToWide(address);
            if (!::InternetCrackUrlW(url.c_str(), 0, 0, &parts))
            {
                lua_pushnil(state);
                return 1;
            }
            HINTERNET connection = ::InternetConnectW(handle.Session, host, parts.nPort, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
            if (!connection)
            {
                lua_pushnil(state);
                return 1;
            }
            const DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI | (parts.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_FLAG_SECURE : 0);
            HINTERNET request = ::HttpOpenRequestW(connection, L"POST", path, nullptr, nullptr, nullptr, flags, 0);
            std::string body;
            bool ok = false;
            if (request)
            {
                const wchar_t* headers = L"Content-Type: application/x-www-form-urlencoded";
                if (::HttpSendRequestW(request, headers, static_cast<DWORD>(-1), const_cast<char*>(data), static_cast<DWORD>(length)))
                    ok = ReadAll(request, body);
                ::InternetCloseHandle(request);
            }
            ::InternetCloseHandle(connection);
            if (!ok)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushlstring(state, body.data(), body.size());
            return 1;
        }

        int InternetIndex(lua_State* state)
        {
            CheckInternet(state, 1);
            const std::string key = Key(state, 2);
            lua_CFunction function = nullptr;
            if (key == "geturl")
                function = &InternetGetUrl;
            else if (key == "posturl")
                function = &InternetPostUrl;
            else if (key == "destroy" || key == "free")
                function = [](lua_State* s) -> int
                    {
                        CloseInternet(CheckInternet(s, 1));
                        return 0;
                    };
            if (!function)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushcfunction(state, function);
            PushBound(state, 1);
            return 1;
        }

        int InternetGc(lua_State* state)
        {
            CloseInternet(CheckInternet(state, 1));
            return 0;
        }

        int GetInternet(lua_State* state)
        {
            const std::wstring agent = Utf8ToWide(luaL_optstring(state, 1, "Manifold Engine"));
            auto* handle = static_cast<InternetHandle*>(lua_newuserdatauv(state, sizeof(InternetHandle), 0));
            new (handle) InternetHandle();
            luaL_setmetatable(state, kInternetType);
            handle->Session = ::InternetOpenW(agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
            return 1;
        }

        int ShellExecuteCompat(lua_State* state)
        {
            const std::wstring command = Utf8ToWide(luaL_checkstring(state, 1));
            const std::wstring parameters = Utf8ToWide(luaL_optstring(state, 2, ""));
            const std::wstring folder = Utf8ToWide(luaL_optstring(state, 3, ""));
            const int show = static_cast<int>(luaL_optinteger(state, 4, SW_SHOWNORMAL));
            const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", command.c_str(), parameters.empty() ? nullptr : parameters.c_str(), folder.empty() ? nullptr : folder.c_str(), show);
            lua_pushboolean(state, reinterpret_cast<std::intptr_t>(result) > 32);
            return 1;
        }

        int TableFileNames(lua_State* state)
        {
            const auto& files = Owner(state).Table().Files;
            lua_createtable(state, static_cast<int>(files.size()), 0);
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                lua_pushstring(state, files[i].Name.c_str());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int CreateMemoryStream(lua_State* state)
        {
            PushStream(state, std::make_shared<StreamState>());
            return 1;
        }

        int CreateStringStream(lua_State* state)
        {
            auto stream = std::make_shared<StreamState>();
            stream->StringStream = true;
            std::size_t length = 0;
            const char* text = luaL_optlstring(state, 1, "", &length);
            stream->Data->assign(text, text + length);
            PushStream(state, stream);
            return 1;
        }

        int CreateFileStream(lua_State* state)
        {
            auto stream = std::make_shared<StreamState>();
            const std::string path = luaL_checkstring(state, 1);
            const lua_Integer mode = luaL_optinteger(state, 2, 0);
            if ((mode & 0xFF00) != 0xFF00)
            {
                if (auto data = ReadFile(Utf8ToWide(path)))
                    *stream->Data = std::move(*data);
            }
            PushStream(state, stream);
            return 1;
        }

        template <class T>
        int ReadLocalValue(lua_State* state)
        {
            T value{};
            if (!ReadLocal(Number(state, 1), &value, sizeof(T)))
            {
                lua_pushnil(state);
                return 1;
            }
            if constexpr (std::is_floating_point_v<T>)
                lua_pushnumber(state, static_cast<lua_Number>(value));
            else if (lua_toboolean(state, 2))
                lua_pushinteger(state, static_cast<lua_Integer>(static_cast<std::make_signed_t<T>>(value)));
            else
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            return 1;
        }

        template <class T>
        int WriteLocalValue(lua_State* state)
        {
            T value{};
            if constexpr (std::is_floating_point_v<T>)
                value = static_cast<T>(luaL_checknumber(state, 2));
            else
                value = static_cast<T>(Number(state, 2));
            lua_pushboolean(state, WriteLocal(Number(state, 1), &value, sizeof(T)));
            return 1;
        }

        int ReadStringLocal(lua_State* state)
        {
            const std::uint64_t address = Number(state, 1);
            const lua_Integer maximum = std::clamp<lua_Integer>(luaL_optinteger(state, 2, 1000), 1, 64 * 1024 * 1024);
            const bool wide = lua_toboolean(state, 3) != 0;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(maximum) * (wide ? 2 : 1));
            if (!ReadLocal(address, bytes.data(), bytes.size()))
            {
                std::size_t got = 0;
                for (; got < bytes.size(); ++got)
                {
                    if (!ReadLocal(address + got, bytes.data() + got, 1))
                        break;
                }
                if (got == 0)
                {
                    lua_pushnil(state);
                    return 1;
                }
                bytes.resize(got);
            }
            if (wide)
            {
                std::wstring text;
                for (std::size_t i = 0; i + 1 < bytes.size(); i += 2)
                {
                    const wchar_t c = static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8));
                    if (c == 0)
                        break;
                    text += c;
                }
                lua_pushstring(state, WideToUtf8(text).c_str());
                return 1;
            }
            std::size_t length = 0;
            while (length < bytes.size() && bytes[length] != 0)
                ++length;
            lua_pushlstring(state, reinterpret_cast<const char*>(bytes.data()), length);
            return 1;
        }

        int WriteStringLocal(lua_State* state)
        {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 2, &length);
            if (lua_toboolean(state, 3))
            {
                const std::wstring wide = Utf8ToWide(std::string_view(text, length));
                lua_pushboolean(state, WriteLocal(Number(state, 1), wide.c_str(), (wide.size() + 1) * 2));
            }
            else
                lua_pushboolean(state, WriteLocal(Number(state, 1), text, length + 1));
            return 1;
        }

        int ReadBytesLocal(lua_State* state)
        {
            const std::uint64_t address = Number(state, 1);
            const lua_Integer count = std::clamp<lua_Integer>(luaL_optinteger(state, 2, 1), 1, 16 * 1024 * 1024);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
            if (!ReadLocal(address, bytes.data(), bytes.size()))
            {
                lua_pushnil(state);
                return 1;
            }
            if (lua_toboolean(state, 3))
            {
                lua_createtable(state, static_cast<int>(count), 0);
                for (std::size_t i = 0; i < bytes.size(); ++i)
                {
                    lua_pushinteger(state, bytes[i]);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                }
                return 1;
            }
            luaL_checkstack(state, static_cast<int>(count), "too many bytes");
            for (std::uint8_t byte : bytes)
                lua_pushinteger(state, byte);
            return static_cast<int>(count);
        }

        int WriteBytesLocal(lua_State* state)
        {
            const std::uint64_t address = Number(state, 1);
            std::vector<std::uint8_t> bytes;
            for (int i = 2; i <= lua_gettop(state); ++i)
            {
                if (lua_istable(state, i))
                {
                    const lua_Integer length = luaL_len(state, i);
                    for (lua_Integer n = 1; n <= length; ++n)
                    {
                        lua_rawgeti(state, i, n);
                        bytes.push_back(static_cast<std::uint8_t>(lua_tointeger(state, -1)));
                        lua_pop(state, 1);
                    }
                }
                else
                    bytes.push_back(static_cast<std::uint8_t>(luaL_checkinteger(state, i)));
            }
            lua_pushboolean(state, WriteLocal(address, bytes.data(), bytes.size()));
            return 1;
        }

        std::uint64_t LocalAddress(std::string_view name)
        {
            std::string text = Trim(name);
            if (text.size() > 2 && text.front() == '"')
                text = text.substr(1, text.find('"', 1) - 1);
            if (const auto number = ParseHex(text))
                return *number;
            for (const char separator : { '!', '.' })
            {
                const std::size_t at = text.rfind(separator);
                if (at == std::string::npos || at == 0)
                    continue;
                std::string module = text.substr(0, at);
                const std::string function = text.substr(at + 1);
                HMODULE handle = ::GetModuleHandleW(Utf8ToWide(module).c_str());
                if (!handle && module.find('.') == std::string::npos)
                    handle = ::GetModuleHandleW(Utf8ToWide(module + ".dll").c_str());
                if (handle)
                {
                    if (FARPROC procedure = ::GetProcAddress(handle, function.c_str()))
                        return reinterpret_cast<std::uint64_t>(procedure);
                }
            }
            if (HMODULE handle = ::GetModuleHandleW(Utf8ToWide(text).c_str()))
                return reinterpret_cast<std::uint64_t>(handle);
            for (const wchar_t* module : { L"kernel32.dll", L"user32.dll", L"gdi32.dll", L"ntdll.dll", L"kernelbase.dll", L"dwmapi.dll", L"uxtheme.dll" })
            {
                if (HMODULE handle = ::GetModuleHandleW(module))
                {
                    if (FARPROC procedure = ::GetProcAddress(handle, text.c_str()))
                        return reinterpret_cast<std::uint64_t>(procedure);
                }
            }
            return 0;
        }

        int GetAddressCompat(lua_State* state)
        {
            if (lua_type(state, 1) == LUA_TNUMBER)
            {
                lua_pushvalue(state, 1);
                return 1;
            }
            const char* text = luaL_checkstring(state, 1);
            if (lua_toboolean(state, 2))
            {
                const std::uint64_t address = LocalAddress(text);
                if (!address)
                    return luaL_error(state, "%s", ErrorMessage(ErrorCode::ExprUnknownSymbol, text).c_str());
                lua_pushinteger(state, static_cast<lua_Integer>(address));
                return 1;
            }
            Status error;
            const auto value = Owner(state).Evaluate(text, &error);
            if (!value)
                return luaL_error(state, "%s", error.Text.c_str());
            lua_pushinteger(state, static_cast<lua_Integer>(*value));
            return 1;
        }

        int GetAddressSafeCompat(lua_State* state)
        {
            if (lua_type(state, 1) == LUA_TNUMBER)
            {
                lua_pushvalue(state, 1);
                return 1;
            }
            const char* text = luaL_checkstring(state, 1);
            if (lua_toboolean(state, 2))
            {
                const std::uint64_t address = LocalAddress(text);
                if (address)
                    lua_pushinteger(state, static_cast<lua_Integer>(address));
                else
                    lua_pushnil(state);
                return 1;
            }
            const auto value = Owner(state).Evaluate(text);
            if (value)
                lua_pushinteger(state, static_cast<lua_Integer>(*value));
            else
                lua_pushnil(state);
            return 1;
        }

        using LocalFunction = std::uint64_t(__stdcall*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

        bool CallLocal(LocalFunction function, const std::uint64_t* values, std::uint64_t* result)
        {
            __try
            {
                *result = function(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        struct LocalArgument
        {
            std::uint64_t Value = 0;
            std::string Narrow;
            std::wstring Wide;
            bool IsNarrow = false;
            bool IsWide = false;
        };

        int ExecuteCodeLocalEx(lua_State* state)
        {
            const std::uint64_t address = lua_type(state, 1) == LUA_TSTRING ? LocalAddress(lua_tostring(state, 1)) : Number(state, 1);
            if (address < 0x10000)
                return luaL_error(state, "executeCodeLocalEx: invalid address");
            std::vector<LocalArgument> arguments;
            for (int i = 2; i <= lua_gettop(state) && arguments.size() < 8; ++i)
            {
                LocalArgument argument;
                if (lua_istable(state, i))
                {
                    lua_getfield(state, i, "type");
                    const lua_Integer type = lua_tointeger(state, -1);
                    lua_pop(state, 1);
                    lua_getfield(state, i, "value");
                    if (type == 3 && lua_type(state, -1) == LUA_TSTRING)
                    {
                        argument.Narrow = lua_tostring(state, -1);
                        argument.IsNarrow = true;
                    }
                    else if (type == 4 && lua_type(state, -1) == LUA_TSTRING)
                    {
                        argument.Wide = Utf8ToWide(lua_tostring(state, -1));
                        argument.IsWide = true;
                    }
                    else if (type == 1)
                    {
                        const float value = static_cast<float>(lua_tonumber(state, -1));
                        std::uint32_t raw = 0;
                        std::memcpy(&raw, &value, 4);
                        argument.Value = raw;
                    }
                    else if (type == 2)
                    {
                        const double value = lua_tonumber(state, -1);
                        std::memcpy(&argument.Value, &value, 8);
                    }
                    else
                        argument.Value = Number(state, -1);
                    lua_pop(state, 1);
                }
                else if (lua_type(state, i) == LUA_TSTRING)
                {
                    argument.Narrow = lua_tostring(state, i);
                    argument.IsNarrow = true;
                }
                else
                {
                    argument.Value = Number(state, i);
                }
                arguments.push_back(std::move(argument));
            }
            std::uint64_t values[8]{};
            for (std::size_t i = 0; i < arguments.size(); ++i)
            {
                if (arguments[i].IsWide)
                    values[i] = reinterpret_cast<std::uint64_t>(arguments[i].Wide.c_str());
                else if (arguments[i].IsNarrow)
                    values[i] = reinterpret_cast<std::uint64_t>(arguments[i].Narrow.c_str());
                else
                    values[i] = arguments[i].Value;
            }
            std::uint64_t result = 0;
            if (!CallLocal(reinterpret_cast<LocalFunction>(address), values, &result))
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, static_cast<lua_Integer>(result));
            return 1;
        }

        int ExecuteCodeLocal(lua_State* state)
        {
            lua_settop(state, 2);
            return ExecuteCodeLocalEx(state);
        }

        std::string FileName(std::string_view path)
        {
            const std::size_t slash = path.find_last_of("\\/");
            return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
        }

        int ExtractFileName(lua_State* state)
        {
            lua_pushstring(state, FileName(luaL_checkstring(state, 1)).c_str());
            return 1;
        }

        int ExtractFileNameWithoutExt(lua_State* state)
        {
            std::string name = FileName(luaL_checkstring(state, 1));
            const std::size_t dot = name.rfind('.');
            if (dot != std::string::npos && dot > 0)
                name.resize(dot);
            lua_pushstring(state, name.c_str());
            return 1;
        }

        int ExtractFilePath(lua_State* state)
        {
            const std::string path = luaL_checkstring(state, 1);
            const std::size_t slash = path.find_last_of("\\/");
            lua_pushstring(state, slash == std::string::npos ? "" : path.substr(0, slash + 1).c_str());
            return 1;
        }

        int ExtractFileExt(lua_State* state)
        {
            const std::string name = FileName(luaL_checkstring(state, 1));
            const std::size_t dot = name.rfind('.');
            lua_pushstring(state, dot == std::string::npos ? "" : name.substr(dot).c_str());
            return 1;
        }

        int WriteToClipboard(lua_State* state)
        {
            std::size_t length = 0;
            const char* text = luaL_tolstring(state, 1, &length);
            const std::wstring wide = Utf8ToWide(std::string_view(text, length));
            if (::OpenClipboard(nullptr))
            {
                ::EmptyClipboard();
                if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t)))
                {
                    std::memcpy(::GlobalLock(memory), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
                    ::GlobalUnlock(memory);
                    ::SetClipboardData(CF_UNICODETEXT, memory);
                }
                ::CloseClipboard();
            }
            return 0;
        }

        int ReadFromClipboard(lua_State* state)
        {
            std::string text;
            if (::OpenClipboard(nullptr))
            {
                if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT))
                {
                    if (const auto* wide = static_cast<const wchar_t*>(::GlobalLock(data)))
                    {
                        text = WideToUtf8(wide);
                        ::GlobalUnlock(data);
                    }
                }
                ::CloseClipboard();
            }
            lua_pushstring(state, text.c_str());
            return 1;
        }

        int GetFileVersion(lua_State* state)
        {
            const std::wstring path = Utf8ToWide(luaL_checkstring(state, 1));
            DWORD ignored = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
            std::vector<std::uint8_t> data(size);
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (size == 0 || !::GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info)
            {
                lua_pushnil(state);
                return 1;
            }
            const std::uint64_t version = (static_cast<std::uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
            lua_pushinteger(state, static_cast<lua_Integer>(version));
            lua_newtable(state);
            lua_pushinteger(state, HIWORD(info->dwFileVersionMS));
            lua_setfield(state, -2, "major");
            lua_pushinteger(state, LOWORD(info->dwFileVersionMS));
            lua_setfield(state, -2, "minor");
            lua_pushinteger(state, HIWORD(info->dwFileVersionLS));
            lua_setfield(state, -2, "release");
            lua_pushinteger(state, LOWORD(info->dwFileVersionLS));
            lua_setfield(state, -2, "build");
            return 2;
        }

        int Md5File(lua_State* state)
        {
            const auto data = ReadFile(Utf8ToWide(luaL_checkstring(state, 1)));
            if (!data)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushstring(state, Md5(data->data(), data->size()).c_str());
            return 1;
        }

        int Md5Memory(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
                return luaL_error(state, "%s", ErrorMessage(ErrorCode::NoProcess).c_str());
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, 256 * 1024 * 1024)));
            if (!process->ReadExact(Number(state, 1), bytes.data(), bytes.size()))
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushstring(state, Md5(bytes.data(), bytes.size()).c_str());
            return 1;
        }

        int StringToMd5(lua_State* state)
        {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 1, &length);
            lua_pushstring(state, Md5(text, length).c_str());
            return 1;
        }

        int UserDataToInteger(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(lua_touserdata(state, 1))));
            return 1;
        }

        int IntegerToUserData(lua_State* state)
        {
            lua_pushlightuserdata(state, reinterpret_cast<void*>(static_cast<std::uintptr_t>(Number(state, 1))));
            return 1;
        }

        int PauseProcess(lua_State* state)
        {
            if (Owner(state).Process())
                Owner(state).Process()->Suspend();
            return 0;
        }

        int UnpauseProcess(lua_State* state)
        {
            if (Owner(state).Process())
                Owner(state).Process()->Resume();
            return 0;
        }

        int CloseCe(lua_State* state)
        {
            Owner(state).Request({ UiRequestKind::Quit, 0, {} });
            return 0;
        }

        int GetFileList(lua_State* state)
        {
            const std::filesystem::path folder = Utf8ToWide(luaL_checkstring(state, 1));
            const std::string mask = luaL_optstring(state, 2, "*");
            const bool recursive = lua_toboolean(state, 3) != 0;
            const lua_Integer depth = luaL_optinteger(state, 4, 0);
            const auto matches = [&](const std::string& name)
                {
                    if (mask == "*" || mask == "*.*" || mask.empty())
                        return true;
                    for (const std::string& part : Split(mask, ';'))
                    {
                        const std::string trimmed = Trim(part);
                        if (trimmed.rfind("*.", 0) == 0 && name.size() >= trimmed.size() - 1 && EqualsNoCase(name.substr(name.size() - (trimmed.size() - 1)), trimmed.substr(1)))
                            return true;
                        if (EqualsNoCase(name, trimmed))
                            return true;
                    }
                    return false;
                };
            lua_newtable(state);
            int index = 1;
            std::error_code error;
            if (recursive)
            {
                for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(error))
                {
                    if (error)
                        break;
                    if (depth > 0 && it.depth() >= depth)
                        it.disable_recursion_pending();
                    if (it->is_regular_file(error) && matches(WideToUtf8(it->path().filename().wstring())))
                    {
                        lua_pushstring(state, WideToUtf8(it->path().wstring()).c_str());
                        lua_rawseti(state, -2, index++);
                    }
                }
            }
            else
            {
                for (const auto& entry : std::filesystem::directory_iterator(folder, error))
                {
                    if (entry.is_regular_file(error) && matches(WideToUtf8(entry.path().filename().wstring())))
                    {
                        lua_pushstring(state, WideToUtf8(entry.path().wstring()).c_str());
                        lua_rawseti(state, -2, index++);
                    }
                }
            }
            return 1;
        }

        int GetDirectoryList(lua_State* state)
        {
            const std::filesystem::path folder = Utf8ToWide(luaL_checkstring(state, 1));
            const bool recursive = lua_toboolean(state, 2) != 0;
            lua_newtable(state);
            int index = 1;
            std::error_code error;
            const auto add = [&](const std::filesystem::directory_entry& entry)
                {
                    if (entry.is_directory(error))
                    {
                        lua_pushstring(state, WideToUtf8(entry.path().wstring()).c_str());
                        lua_rawseti(state, -2, index++);
                    }
                };
            if (recursive)
            {
                for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(error))
                {
                    if (error)
                        break;
                    add(*it);
                }
            }
            else
            {
                for (const auto& entry : std::filesystem::directory_iterator(folder, error))
                    add(entry);
            }
            return 1;
        }

        int EnumMemoryRegions(lua_State* state)
        {
            lua_newtable(state);
            const auto process = Owner(state).Process();
            if (!process)
                return 1;
            int index = 1;
            for (const Target::RegionInfo& region : process->Regions(0, process->HighestAddress()))
            {
                lua_newtable(state);
                lua_pushinteger(state, static_cast<lua_Integer>(region.Base));
                lua_setfield(state, -2, "BaseAddress");
                lua_pushinteger(state, static_cast<lua_Integer>(region.AllocationBase));
                lua_setfield(state, -2, "AllocationBase");
                lua_pushinteger(state, region.AllocationProtect);
                lua_setfield(state, -2, "AllocationProtect");
                lua_pushinteger(state, static_cast<lua_Integer>(region.Size));
                lua_setfield(state, -2, "RegionSize");
                lua_pushinteger(state, region.State);
                lua_setfield(state, -2, "State");
                lua_pushinteger(state, region.Protect);
                lua_setfield(state, -2, "Protect");
                lua_pushinteger(state, region.Type);
                lua_setfield(state, -2, "Type");
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int InModule(lua_State* state)
        {
            const auto process = Owner(state).Process();
            lua_pushboolean(state, process && process->ModuleAt(Number(state, 1)).has_value());
            return 1;
        }

        int InSystemModule(lua_State* state)
        {
            const auto process = Owner(state).Process();
            const auto module = process ? process->ModuleAt(Number(state, 1)) : std::nullopt;
            lua_pushboolean(state, module && module->System);
            return 1;
        }

        int GetModuleNameFromAddress(lua_State* state)
        {
            const auto process = Owner(state).Process();
            const auto module = process ? process->ModuleAt(Number(state, 1)) : std::nullopt;
            if (module)
                lua_pushstring(state, module->Name.c_str());
            else
                lua_pushnil(state);
            return 1;
        }

        HWND WindowArgument(lua_State* state, int index)
        {
            return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(Number(state, index)));
        }

        void PushWindow(lua_State* state, HWND window)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(window)));
        }

        std::wstring OptionalWide(lua_State* state, int index)
        {
            return lua_type(state, index) == LUA_TSTRING ? Utf8ToWide(lua_tostring(state, index)) : std::wstring();
        }

        int GetCheatEngineProcessId(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(::GetCurrentProcessId()));
            return 1;
        }

        int FindWindowCompat(lua_State* state)
        {
            const std::wstring className = OptionalWide(state, 1);
            const std::wstring caption = OptionalWide(state, 2);
            PushWindow(state, ::FindWindowW(className.empty() ? nullptr : className.c_str(), caption.empty() ? nullptr : caption.c_str()));
            return 1;
        }

        int GetWindowCompat(lua_State* state)
        {
            PushWindow(state, ::GetWindow(WindowArgument(state, 1), static_cast<UINT>(Number(state, 2))));
            return 1;
        }

        int GetWindowCaption(lua_State* state)
        {
            const HWND window = WindowArgument(state, 1);
            std::wstring text(static_cast<std::size_t>(std::max(0, ::GetWindowTextLengthW(window))) + 1, L'\0');
            text.resize(static_cast<std::size_t>(::GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
            lua_pushstring(state, WideToUtf8(text).c_str());
            return 1;
        }

        int GetWindowClassName(lua_State* state)
        {
            wchar_t name[256] = L"";
            ::GetClassNameW(WindowArgument(state, 1), name, static_cast<int>(std::size(name)));
            lua_pushstring(state, WideToUtf8(name).c_str());
            return 1;
        }

        int GetWindowProcessId(lua_State* state)
        {
            DWORD id = 0;
            ::GetWindowThreadProcessId(WindowArgument(state, 1), &id);
            lua_pushinteger(state, static_cast<lua_Integer>(id));
            return 1;
        }

        int GetForegroundWindowCompat(lua_State* state)
        {
            PushWindow(state, ::GetForegroundWindow());
            return 1;
        }

        int Printf(lua_State* state)
        {
            const int count = lua_gettop(state);
            lua_getglobal(state, "print");
            lua_getglobal(state, "string");
            lua_getfield(state, -1, "format");
            lua_remove(state, -2);
            for (int i = 1; i <= count; ++i)
                lua_pushvalue(state, i);
            lua_call(state, count, 1);
            lua_call(state, 1, 0);
            return 0;
        }

        int DetachIfPossible(lua_State* state)
        {
            Debug::Debugger& debugger = Owner(state).Debugger();
            const bool attached = debugger.Attached();
            if (attached)
                debugger.Detach();
            lua_pushboolean(state, attached);
            return 1;
        }

        int DeleteAllRegisteredSymbols(lua_State* state)
        {
            for (const Target::UserSymbol& symbol : Owner(state).Symbols().Registered())
                Owner(state).Symbols().Unregister(symbol.Name);
            return 0;
        }

        int RegisterAutoAssemblerCommand(lua_State* state)
        {
            const std::string name = luaL_checkstring(state, 1);
            luaL_checktype(state, 2, LUA_TFUNCTION);
            lua_pushvalue(state, 2);
            EngineOf(state).SetAutoAssemblerCommand(name, luaL_ref(state, LUA_REGISTRYINDEX));
            return 0;
        }

        int UnregisterAutoAssemblerCommand(lua_State* state)
        {
            EngineOf(state).SetAutoAssemblerCommand(luaL_checkstring(state, 1), 0);
            return 0;
        }

        int RegisterAutoAssemblerTemplate(lua_State* state)
        {
            static int next = 1;
            lua_pushinteger(state, next++);
            return 1;
        }

        int Nothing(lua_State*)
        {
            return 0;
        }

        int EnableWindowsSymbols(lua_State* state)
        {
            const bool windows = lua_gettop(state) < 1 || lua_toboolean(state, 1) != 0;
            Owner(state).LoadSymbols(windows);
            return 0;
        }

        int WaitForPdb(lua_State* state)
        {
            const unsigned timeout = static_cast<unsigned>(luaL_optinteger(state, 1, 120000));
            lua_pushboolean(state, Owner(state).WaitForSymbols(timeout) ? 1 : 0);
            return 1;
        }

        int ReturnTrue(lua_State* state)
        {
            lua_pushboolean(state, 1);
            return 1;
        }

        int FileDialog(lua_State* state)
        {
            const bool save = lua_toboolean(state, 1) != 0;
            const std::wstring title = Utf8ToWide(luaL_optstring(state, 2, ""));
            std::string filterText = luaL_optstring(state, 3, "All files|*.*");
            const std::wstring initial = Utf8ToWide(luaL_optstring(state, 5, ""));
            const std::wstring extension = Utf8ToWide(luaL_optstring(state, 6, ""));
            std::wstring filter = Utf8ToWide(filterText);
            std::replace(filter.begin(), filter.end(), L'|', L'\0');
            filter.push_back(L'\0');
            filter.push_back(L'\0');
            wchar_t file[MAX_PATH * 4] = L"";
            ::wcsncpy_s(file, Utf8ToWide(luaL_optstring(state, 4, "")).c_str(), _TRUNCATE);
            OPENFILENAMEW dialog{ sizeof(dialog) };
            dialog.hwndOwner = ::GetActiveWindow();
            dialog.lpstrFilter = filter.c_str();
            dialog.lpstrFile = file;
            dialog.nMaxFile = static_cast<DWORD>(std::size(file));
            dialog.lpstrTitle = title.empty() ? nullptr : title.c_str();
            dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
            dialog.lpstrDefExt = extension.empty() ? nullptr : extension.c_str();
            dialog.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
            const BOOL chosen = save ? ::GetSaveFileNameW(&dialog) : ::GetOpenFileNameW(&dialog);
            if (!chosen)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushstring(state, WideToUtf8(file).c_str());
            return 1;
        }

        int FolderDialog(lua_State* state)
        {
            const std::wstring title = Utf8ToWide(luaL_optstring(state, 1, "Select a folder"));
            BROWSEINFOW info{};
            info.hwndOwner = ::GetActiveWindow();
            info.lpszTitle = title.c_str();
            info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE list = ::SHBrowseForFolderW(&info);
            if (!list)
            {
                lua_pushnil(state);
                return 1;
            }
            wchar_t path[MAX_PATH] = L"";
            ::SHGetPathFromIDListW(list, path);
            ::CoTaskMemFree(list);
            lua_pushstring(state, WideToUtf8(path).c_str());
            return 1;
        }

        int ColorDialog(lua_State* state)
        {
            static COLORREF custom[16]{};
            CHOOSECOLORW choose{ sizeof(choose) };
            choose.hwndOwner = ::GetActiveWindow();
            choose.rgbResult = static_cast<COLORREF>(luaL_optinteger(state, 1, 0));
            choose.lpCustColors = custom;
            choose.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (!::ChooseColorW(&choose))
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, choose.rgbResult);
            return 1;
        }

        int SelectedCodeAddress(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(Owner(state).SelectedCodeAddress));
            return 1;
        }

        int SetSelectedCodeAddress(lua_State* state)
        {
            const std::uint64_t address = Number(state, 1);
            Owner(state).SelectedCodeAddress = address;
            Owner(state).Request({ UiRequestKind::MemoryViewer, address, {} });
            return 0;
        }

        int SelectedHexAddress(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(Owner(state).SelectedHexAddress));
            return 1;
        }

        struct MemScanState
        {
            std::unique_ptr<Scan::Scanner> Scanner = std::make_unique<Scan::Scanner>();
            Scan::ScanOptions Options;
            bool OnlyOne = false;
            std::uint64_t Result = 0;
            bool RegionScan = false;
            bool Initialized = false;
        };

        MemScanState& CheckMemScan(lua_State* state, int index)
        {
            return **static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(state, index, kMemScanType));
        }

        Scan::Compare CompareFromCe(lua_Integer option)
        {
            switch (option)
            {
            case 0: return Scan::Compare::Unknown;
            case 2: return Scan::Compare::Between;
            case 3: return Scan::Compare::Bigger;
            case 4: return Scan::Compare::Smaller;
            case 5: return Scan::Compare::Increased;
            case 6: return Scan::Compare::IncreasedBy;
            case 7: return Scan::Compare::Decreased;
            case 8: return Scan::Compare::DecreasedBy;
            case 9: return Scan::Compare::Changed;
            case 10: return Scan::Compare::Unchanged;
            default: return Scan::Compare::Exact;
            }
        }

        Scan::ValueType TypeFromCe(lua_Integer type)
        {
            switch (type)
            {
            case 0: return Scan::ValueType::Byte;
            case 1: return Scan::ValueType::TwoBytes;
            case 3: return Scan::ValueType::EightBytes;
            case 4: return Scan::ValueType::Float;
            case 5: return Scan::ValueType::Double;
            case 6: case 7: return Scan::ValueType::String;
            case 8: return Scan::ValueType::ByteArray;
            default: return Scan::ValueType::FourBytes;
            }
        }

        Scan::FloatMode RoundingFromCe(lua_Integer rounding)
        {
            return rounding == 2 ? Scan::FloatMode::Truncated : Scan::FloatMode::Rounded;
        }

        void PushFoundList(lua_State* state, int scanIndex)
        {
            auto* holder = static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(state, scanIndex, kMemScanType));
            void* memory = lua_newuserdatauv(state, sizeof(std::shared_ptr<MemScanState>), 0);
            new (memory) std::shared_ptr<MemScanState>(*holder);
            luaL_setmetatable(state, kFoundListType);
        }

        void ApplyProtection(Scan::ScanOptions& options, std::string_view flags)
        {
            options.Writable = Scan::Tristate::Any;
            options.Executable = Scan::Tristate::Any;
            options.CopyOnWrite = Scan::Tristate::Any;
            for (std::size_t i = 0; i + 1 < flags.size(); i += 2)
            {
                const char mode = flags[i];
                const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(flags[i + 1])));
                const Scan::Tristate value = mode == '+' ? Scan::Tristate::Yes : mode == '-' ? Scan::Tristate::No : Scan::Tristate::Any;
                if (kind == 'W')
                    options.Writable = value;
                else if (kind == 'X')
                    options.Executable = value;
                else if (kind == 'C')
                    options.CopyOnWrite = value;
            }
        }

        bool WaitForScan(MemScanState& scan)
        {
            while (scan.Scanner->Busy())
            {
                if (scan.Scanner->Poll())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            scan.Scanner->Poll();
            if (scan.OnlyOne)
            {
                Scan::ScanRecord record;
                scan.Result = scan.Scanner->Count() > 0 && scan.Scanner->Record(0, record) ? record.Address : 0;
            }
            return scan.Scanner->LastError().Ok();
        }

        int MemScanIndex(lua_State* state)
        {
            MemScanState& scan = CheckMemScan(state, 1);
            const std::string key = Key(state, 2);
            if (key == "lastscanwasregionscan") { lua_pushboolean(state, scan.RegionScan); return 1; }
            if (key == "onlyoneresult" || key == "isunique") { lua_pushboolean(state, scan.OnlyOne); return 1; }
            if (key == "result") { lua_pushinteger(state, static_cast<lua_Integer>(scan.Result)); return 1; }
            if (key == "foundlist")
            {
                PushFoundList(state, 1);
                return 1;
            }
            static const luaL_Reg kMethods[] = {
                { "firstscan", [](lua_State* s) -> int
                    {
                        MemScanState& target = CheckMemScan(s, 1);
                        const auto& process = Owner(s).Process();
                        if (!process)
                            return luaL_error(s, "%s", ErrorMessage(ErrorCode::NoProcess).c_str());
                        Scan::ScanOptions options;
                        options.Mode = CompareFromCe(luaL_optinteger(s, 2, 1));
                        options.Type = TypeFromCe(luaL_optinteger(s, 3, 2));
                        options.Floats = RoundingFromCe(luaL_optinteger(s, 4, 0));
                        options.Value = luaL_optstring(s, 5, "");
                        options.Value2 = luaL_optstring(s, 6, "");
                        options.Start = Number(s, 7);
                        const std::uint64_t stop = Number(s, 8);
                        options.Stop = stop ? stop : process->HighestAddress();
                        ApplyProtection(options, luaL_optstring(s, 9, "+W*X-C"));
                        const lua_Integer alignment = luaL_optinteger(s, 10, 1);
                        const std::string parameter = luaL_optstring(s, 11, "1");
                        options.Alignment = alignment == 1 ? static_cast<int>(ParseHex(parameter).value_or(1)) : 1;
                        options.Hex = lua_toboolean(s, 12) != 0;
                        options.Unicode = lua_toboolean(s, 14) != 0;
                        options.CaseSensitive = lua_isnoneornil(s, 15) || lua_toboolean(s, 15);
                        options.Mapped = false;
                        if (luaL_optinteger(s, 3, 2) == 7)
                            options.Unicode = true;
                        target.Options = options;
                        target.RegionScan = options.Mode == Scan::Compare::Unknown;
                        target.Scanner->Reset();
                        const Status status = target.Scanner->Start(process, options, true);
                        if (!status)
                            return luaL_error(s, "%s", status.Text.c_str());
                        return 0;
                    } },
                { "nextscan", [](lua_State* s) -> int
                    {
                        MemScanState& target = CheckMemScan(s, 1);
                        const auto& process = Owner(s).Process();
                        if (!process)
                            return luaL_error(s, "%s", ErrorMessage(ErrorCode::NoProcess).c_str());
                        WaitForScan(target);
                        Scan::ScanOptions options = target.Options;
                        options.Mode = CompareFromCe(luaL_optinteger(s, 2, 1));
                        options.Floats = RoundingFromCe(luaL_optinteger(s, 3, 0));
                        options.Value = luaL_optstring(s, 4, "");
                        options.Value2 = luaL_optstring(s, 5, "");
                        options.Hex = lua_toboolean(s, 6) != 0;
                        target.Options = options;
                        target.RegionScan = false;
                        const Status status = target.Scanner->Start(process, options, false);
                        if (!status)
                            return luaL_error(s, "%s", status.Text.c_str());
                        return 0;
                    } },
                { "waittilldone", [](lua_State* s) -> int
                    {
                        WaitForScan(CheckMemScan(s, 1));
                        return 0;
                    } },
                { "newscan", [](lua_State* s) -> int
                    {
                        CheckMemScan(s, 1).Scanner->Reset();
                        return 0;
                    } },
                { "setonlyoneresult", [](lua_State* s) -> int
                    {
                        CheckMemScan(s, 1).OnlyOne = lua_toboolean(s, 2) != 0;
                        return 0;
                    } },
                { "getonlyresult", [](lua_State* s) -> int
                    {
                        MemScanState& target = CheckMemScan(s, 1);
                        if (target.Result)
                            lua_pushinteger(s, static_cast<lua_Integer>(target.Result));
                        else
                            lua_pushnil(s);
                        return 1;
                    } },
                { "getattachedfoundlist", [](lua_State* s) -> int
                    {
                        PushFoundList(s, 1);
                        return 1;
                    } },
                { "getprogress", [](lua_State* s) -> int
                    {
                        MemScanState& target = CheckMemScan(s, 1);
                        lua_newtable(s);
                        lua_pushinteger(s, 100);
                        lua_setfield(s, -2, "TotalAddressesToScan");
                        lua_pushinteger(s, static_cast<lua_Integer>(target.Scanner->Progress() * 100.0));
                        lua_setfield(s, -2, "CurrentlyScanned");
                        lua_pushinteger(s, static_cast<lua_Integer>(target.Scanner->Count()));
                        lua_setfield(s, -2, "ResultsFound");
                        return 1;
                    } },
                { "savecurrentresults", &Nothing },
                { "destroy", [](lua_State* s) -> int
                    {
                        CheckMemScan(s, 1).Scanner->Reset();
                        return 0;
                    } },
                { nullptr, nullptr },
            };
            for (const luaL_Reg* method = kMethods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return 1;
                }
            }
            lua_pushnil(state);
            return 1;
        }

        int MemScanNewIndex(lua_State* state)
        {
            MemScanState& scan = CheckMemScan(state, 1);
            const std::string key = Key(state, 2);
            if (key == "onlyoneresult" || key == "isunique")
                scan.OnlyOne = lua_toboolean(state, 3) != 0;
            return 0;
        }

        int ScanGc(lua_State* state)
        {
            auto* holder = static_cast<std::shared_ptr<MemScanState>*>(lua_touserdata(state, 1));
            if (holder)
                holder->~shared_ptr<MemScanState>();
            return 0;
        }

        int CreateMemScan(lua_State* state)
        {
            void* memory = lua_newuserdatauv(state, sizeof(std::shared_ptr<MemScanState>), 0);
            new (memory) std::shared_ptr<MemScanState>(std::make_shared<MemScanState>());
            luaL_setmetatable(state, kMemScanType);
            return 1;
        }

        int CreateFoundList(lua_State* state)
        {
            PushFoundList(state, 1);
            return 1;
        }

        int FoundListIndex(lua_State* state)
        {
            MemScanState& scan = **static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(state, 1, kFoundListType));
            const std::string key = Key(state, 2);
            if (key == "count")
            {
                lua_pushinteger(state, static_cast<lua_Integer>(scan.Scanner->Count()));
                return 1;
            }
            if (key == "address" || key == "value")
            {
                const bool value = key == "value";
                lua_newtable(state);
                lua_newtable(state);
                lua_pushvalue(state, 1);
                lua_pushboolean(state, value);
                lua_pushcclosure(state, [](lua_State* s) -> int
                    {
                        MemScanState& target = **static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(s, lua_upvalueindex(1), kFoundListType));
                        const bool wantValue = lua_toboolean(s, lua_upvalueindex(2)) != 0;
                        Scan::ScanRecord record;
                        if (!target.Scanner->Record(static_cast<std::uint64_t>(luaL_checkinteger(s, 2)), record))
                        {
                            lua_pushnil(s);
                            return 1;
                        }
                        if (wantValue)
                            lua_pushstring(s, Scan::FormatValue(target.Scanner->Type(), record.Previous, target.Scanner->Format()).c_str());
                        else
                            lua_pushstring(s, Hex(record.Address).c_str());
                        return 1;
                    }, 2);
                lua_setfield(state, -2, "__index");
                lua_setmetatable(state, -2);
                return 1;
            }
            if (key == "getaddress")
            {
                lua_pushcfunction(state, [](lua_State* s) -> int
                    {
                        MemScanState& target = **static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(s, 1, kFoundListType));
                        Scan::ScanRecord record;
                        if (target.Scanner->Record(static_cast<std::uint64_t>(luaL_checkinteger(s, 2)), record))
                            lua_pushstring(s, Hex(record.Address).c_str());
                        else
                            lua_pushnil(s);
                        return 1;
                    });
                PushBound(state, 1);
                return 1;
            }
            if (key == "getvalue")
            {
                lua_pushcfunction(state, [](lua_State* s) -> int
                    {
                        MemScanState& target = **static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(s, 1, kFoundListType));
                        Scan::ScanRecord record;
                        if (target.Scanner->Record(static_cast<std::uint64_t>(luaL_checkinteger(s, 2)), record))
                            lua_pushstring(s, Scan::FormatValue(target.Scanner->Type(), record.Previous, target.Scanner->Format()).c_str());
                        else
                            lua_pushnil(s);
                        return 1;
                    });
                PushBound(state, 1);
                return 1;
            }
            if (key == "getcount")
            {
                lua_pushcfunction(state, [](lua_State* s) -> int
                    {
                        MemScanState& target = **static_cast<std::shared_ptr<MemScanState>*>(luaL_checkudata(s, 1, kFoundListType));
                        lua_pushinteger(s, static_cast<lua_Integer>(target.Scanner->Count()));
                        return 1;
                    });
                PushBound(state, 1);
                return 1;
            }
            if (key == "initialize" || key == "deinitialize" || key == "destroy" || key == "refresh")
            {
                lua_pushcfunction(state, &Nothing);
                PushBound(state, 1);
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }

        using DetachedList = std::vector<std::unique_ptr<Table::StructDefinition>>;

        DetachedList& Detached(lua_State* state)
        {
            lua_getfield(state, LUA_REGISTRYINDEX, kDetachedKey);
            auto* list = static_cast<DetachedList*>(lua_touserdata(state, -1));
            lua_pop(state, 1);
            return *list;
        }

        bool IsGlobal(lua_State* state, const Table::StructDefinition* definition)
        {
            for (const auto& item : Owner(state).Structures().Items)
            {
                if (item.get() == definition)
                    return true;
            }
            return false;
        }

        bool StructureAlive(lua_State* state, const Table::StructDefinition* definition)
        {
            if (IsGlobal(state, definition))
                return true;
            for (const auto& item : Detached(state))
            {
                if (item.get() == definition)
                    return true;
            }
            return false;
        }

        void PushStructure(lua_State* state, Table::StructDefinition* definition)
        {
            if (!definition)
            {
                lua_pushnil(state);
                return;
            }
            auto** memory = static_cast<Table::StructDefinition**>(lua_newuserdatauv(state, sizeof(Table::StructDefinition*), 0));
            *memory = definition;
            luaL_setmetatable(state, kStructureType);
        }

        Table::StructDefinition& CheckStructure(lua_State* state, int index)
        {
            auto* definition = *static_cast<Table::StructDefinition**>(luaL_checkudata(state, index, kStructureType));
            if (!StructureAlive(state, definition))
                luaL_error(state, "the structure has been destroyed");
            return *definition;
        }

        struct ElementHandle
        {
            Table::StructDefinition* Definition = nullptr;
            int Index = 0;
        };

        void PushElement(lua_State* state, Table::StructDefinition* definition, int index)
        {
            if (!definition || index < 0 || index >= static_cast<int>(definition->Fields.size()))
            {
                lua_pushnil(state);
                return;
            }
            auto* handle = static_cast<ElementHandle*>(lua_newuserdatauv(state, sizeof(ElementHandle), 0));
            handle->Definition = definition;
            handle->Index = index;
            luaL_setmetatable(state, kElementType);
        }

        Table::StructField& CheckElement(lua_State* state, int index, Table::StructDefinition** owner = nullptr)
        {
            auto* handle = static_cast<ElementHandle*>(luaL_checkudata(state, index, kElementType));
            if (!StructureAlive(state, handle->Definition) || handle->Index < 0 || handle->Index >= static_cast<int>(handle->Definition->Fields.size()))
                luaL_error(state, "the structure element no longer exists");
            if (owner)
                *owner = handle->Definition;
            return handle->Definition->Fields[static_cast<std::size_t>(handle->Index)];
        }

        int ElementVarType(const Table::StructField& field)
        {
            if (!field.CustomType.empty())
                return 13;
            switch (field.Type)
            {
            case Scan::ValueType::Byte: return 0;
            case Scan::ValueType::TwoBytes: return 1;
            case Scan::ValueType::FourBytes: return 2;
            case Scan::ValueType::EightBytes: return 3;
            case Scan::ValueType::Float: return 4;
            case Scan::ValueType::Double: return 5;
            case Scan::ValueType::String: return field.Unicode ? 7 : 6;
            case Scan::ValueType::ByteArray: return 8;
            case Scan::ValueType::Binary: return 9;
            case Scan::ValueType::Pointer: return 12;
            }
            return 2;
        }

        void ApplyElementVarType(Table::StructField& field, lua_Integer type)
        {
            static const Scan::ValueType kTypes[] = { Scan::ValueType::Byte, Scan::ValueType::TwoBytes, Scan::ValueType::FourBytes, Scan::ValueType::EightBytes,
                Scan::ValueType::Float, Scan::ValueType::Double, Scan::ValueType::String, Scan::ValueType::String, Scan::ValueType::ByteArray, Scan::ValueType::Binary };
            if (type == 13)
            {
                field.Type = Scan::ValueType::ByteArray;
                return;
            }
            field.CustomType.clear();
            if (type == 12)
                field.Type = Scan::ValueType::Pointer;
            else if (type >= 0 && type < static_cast<lua_Integer>(std::size(kTypes)))
                field.Type = kTypes[type];
            field.Unicode = type == 7;
            const int sizes[] = { 1, 2, 4, 8, 4, 8 };
            if (type >= 0 && type <= 5)
                field.Size = sizes[type];
            else if (type == 12)
                field.Size = 8;
            else if (type == 9)
            {
                field.BitStart = std::clamp(field.BitStart, 0, 63);
                field.BitLength = std::clamp(field.BitLength, 1, 64 - field.BitStart);
                field.Size = Table::BitFieldSize(field.BitStart, field.BitLength);
            }
        }

        void Touch(lua_State* state)
        {
            ++Owner(state).Structures().Generation;
        }

        int StructureIndex(lua_State* state)
        {
            Table::StructDefinition& definition = CheckStructure(state, 1);
            const std::string key = Key(state, 2);
            if (key == "name") { lua_pushstring(state, definition.Name.c_str()); return 1; }
            if (key == "size") { lua_pushinteger(state, definition.Size()); return 1; }
            if (key == "count") { lua_pushinteger(state, static_cast<lua_Integer>(definition.Fields.size())); return 1; }
            if (key == "classname") { lua_pushstring(state, "TDissectedStruct"); return 1; }
            if (key == "globalindex")
            {
                const auto& items = Owner(state).Structures().Items;
                for (std::size_t i = 0; i < items.size(); ++i)
                {
                    if (items[i].get() == &definition)
                    {
                        lua_pushinteger(state, static_cast<lua_Integer>(i));
                        return 1;
                    }
                }
                lua_pushinteger(state, -1);
                return 1;
            }
            if (key == "element")
            {
                auto** memory = static_cast<Table::StructDefinition**>(lua_newuserdatauv(state, sizeof(Table::StructDefinition*), 0));
                *memory = &definition;
                luaL_setmetatable(state, kElementListType);
                return 1;
            }
            static const luaL_Reg kMethods[] = {
                { "getname", [](lua_State* s) -> int { lua_pushstring(s, CheckStructure(s, 1).Name.c_str()); return 1; } },
                { "setname", [](lua_State* s) -> int { CheckStructure(s, 1).Name = luaL_checkstring(s, 2); Touch(s); return 0; } },
                { "getsize", [](lua_State* s) -> int { lua_pushinteger(s, CheckStructure(s, 1).Size()); return 1; } },
                { "getelementcount", [](lua_State* s) -> int { lua_pushinteger(s, static_cast<lua_Integer>(CheckStructure(s, 1).Fields.size())); return 1; } },
                { "getelement", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        PushElement(s, &target, static_cast<int>(luaL_checkinteger(s, 2)));
                        return 1;
                    } },
                { "getelementbyoffset", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        const lua_Integer offset = luaL_checkinteger(s, 2);
                        for (std::size_t i = 0; i < target.Fields.size(); ++i)
                        {
                            const Table::StructField& field = target.Fields[i];
                            if (offset >= field.Offset && offset < field.Offset + std::max(1, field.Size))
                            {
                                PushElement(s, &target, static_cast<int>(i));
                                return 1;
                            }
                        }
                        lua_pushnil(s);
                        return 1;
                    } },
                { "addelement", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        Table::StructField field;
                        field.Offset = target.Size();
                        target.Fields.push_back(field);
                        Touch(s);
                        PushElement(s, &target, static_cast<int>(target.Fields.size()) - 1);
                        return 1;
                    } },
                { "addtoglobalstructurelist", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        auto& detached = Detached(s);
                        for (auto it = detached.begin(); it != detached.end(); ++it)
                        {
                            if (it->get() == &target)
                            {
                                Owner(s).Structures().Items.push_back(std::move(*it));
                                detached.erase(it);
                                Touch(s);
                                break;
                            }
                        }
                        return 0;
                    } },
                { "removefromglobalstructurelist", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        auto& items = Owner(s).Structures().Items;
                        for (auto it = items.begin(); it != items.end(); ++it)
                        {
                            if (it->get() == &target)
                            {
                                Detached(s).push_back(std::move(*it));
                                items.erase(it);
                                Touch(s);
                                break;
                            }
                        }
                        return 0;
                    } },
                { "destroy", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        auto& items = Owner(s).Structures().Items;
                        items.erase(std::remove_if(items.begin(), items.end(), [&](const auto& item) { return item.get() == &target; }), items.end());
                        auto& detached = Detached(s);
                        detached.erase(std::remove_if(detached.begin(), detached.end(), [&](const auto& item) { return item.get() == &target; }), detached.end());
                        Touch(s);
                        return 0;
                    } },
                { "beginupdate", &Nothing },
                { "endupdate", [](lua_State* s) -> int { Touch(s); return 0; } },
                { "autoguess", [](lua_State* s) -> int
                    {
                        Table::StructDefinition& target = CheckStructure(s, 1);
                        const auto& process = Owner(s).Process();
                        const std::uint64_t base = Number(s, 2);
                        const lua_Integer offset = luaL_optinteger(s, 3, 0);
                        const lua_Integer size = luaL_optinteger(s, 4, 4096);
                        if (!process || !base || offset < 0 || offset > 0x1000000 || size <= 0 || size > 0x100000)
                            return 0;
                        Table::GuessFields(*process, target, base, static_cast<int>(offset), static_cast<int>(size));
                        Touch(s);
                        return 0;
                    } },
                { "filldotnetaddress", &Nothing },
                { "fillfromdotnetaddress", &Nothing },
                { "clear", [](lua_State* s) -> int { CheckStructure(s, 1).Fields.clear(); Touch(s); return 0; } },
                { nullptr, nullptr },
            };
            for (const luaL_Reg* method = kMethods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return 1;
                }
            }
            lua_pushnil(state);
            return 1;
        }

        int StructureNewIndex(lua_State* state)
        {
            Table::StructDefinition& definition = CheckStructure(state, 1);
            if (Key(state, 2) == "name")
            {
                definition.Name = luaL_checkstring(state, 3);
                Touch(state);
            }
            return 0;
        }

        int ElementListIndex(lua_State* state)
        {
            auto* definition = *static_cast<Table::StructDefinition**>(luaL_checkudata(state, 1, kElementListType));
            if (!StructureAlive(state, definition))
                return luaL_error(state, "the structure has been destroyed");
            PushElement(state, definition, static_cast<int>(luaL_checkinteger(state, 2)));
            return 1;
        }

        int ElementListLength(lua_State* state)
        {
            auto* definition = *static_cast<Table::StructDefinition**>(luaL_checkudata(state, 1, kElementListType));
            lua_pushinteger(state, StructureAlive(state, definition) ? static_cast<lua_Integer>(definition->Fields.size()) : 0);
            return 1;
        }

        int PropertyGetter(lua_State* state)
        {
            lua_settop(state, 1);
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_gettable(state, 1);
            return 1;
        }

        int PropertySetter(lua_State* state)
        {
            lua_settop(state, 2);
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_pushvalue(state, 2);
            lua_settable(state, 1);
            return 0;
        }

        int ElementIndex(lua_State* state)
        {
            Table::StructDefinition* owner = nullptr;
            Table::StructField& field = CheckElement(state, 1, &owner);
            const std::string key = Key(state, 2);
            if (key == "owner") { PushStructure(state, owner); return 1; }
            if (key == "offset") { lua_pushinteger(state, field.Offset); return 1; }
            if (key == "offsethex") { lua_pushstring(state, Hex(static_cast<std::uint64_t>(field.Offset)).c_str()); return 1; }
            if (key == "name") { lua_pushstring(state, field.Name.c_str()); return 1; }
            if (key == "vartype") { lua_pushinteger(state, ElementVarType(field)); return 1; }
            if (key == "bytesize") { lua_pushinteger(state, field.Size); return 1; }
            if (key == "childstructstart") { lua_pushinteger(state, 0); return 1; }
            if (key == "classname") { lua_pushstring(state, "TStructelement"); return 1; }
            if (key == "bitstart") { lua_pushinteger(state, field.BitStart); return 1; }
            if (key == "bitlength") { lua_pushinteger(state, field.BitLength); return 1; }
            if (key == "displaymethod")
            {
                lua_pushstring(state, field.Display == Table::FieldDisplay::Hex ? "dtHexadecimal" : field.Display == Table::FieldDisplay::Signed ? "dtSignedInteger" :
                    field.Display == Table::FieldDisplay::Binary ? "dtBinary" : "dtUnsignedInteger");
                return 1;
            }
            if (key == "childstruct")
            {
                PushStructure(state, field.ChildStructure.empty() ? nullptr : Owner(state).Structures().Find(field.ChildStructure));
                return 1;
            }
            if (key == "customtype")
            {
                if (field.CustomType.empty() || !EngineOf(state).FindCustomType(field.CustomType))
                    lua_pushnil(state);
                else
                    PushCustomType(state, EngineOf(state).FindCustomType(field.CustomType)->Name);
                return 1;
            }
            if (key == "customtypename") { lua_pushstring(state, field.CustomType.c_str()); return 1; }
            static const luaL_Reg kMethods[] = {
                { "getownerstructure", [](lua_State* s) -> int
                    {
                        Table::StructDefinition* target = nullptr;
                        CheckElement(s, 1, &target);
                        PushStructure(s, target);
                        return 1;
                    } },
                { "delete", [](lua_State* s) -> int
                    {
                        auto* handle = static_cast<ElementHandle*>(luaL_checkudata(s, 1, kElementType));
                        CheckElement(s, 1);
                        handle->Definition->Fields.erase(handle->Definition->Fields.begin() + handle->Index);
                        Touch(s);
                        return 0;
                    } },
                { "destroy", [](lua_State* s) -> int
                    {
                        auto* handle = static_cast<ElementHandle*>(luaL_checkudata(s, 1, kElementType));
                        CheckElement(s, 1);
                        handle->Definition->Fields.erase(handle->Definition->Fields.begin() + handle->Index);
                        Touch(s);
                        return 0;
                    } },
                { nullptr, nullptr },
            };
            for (const luaL_Reg* method = kMethods; method->name; ++method)
            {
                if (key == method->name)
                {
                    lua_pushcfunction(state, method->func);
                    PushBound(state, 1);
                    return 1;
                }
            }
            if (key.size() > 3 && (key.rfind("get", 0) == 0 || key.rfind("set", 0) == 0))
            {
                lua_pushstring(state, key.c_str() + 3);
                lua_pushcclosure(state, key[0] == 'g' ? &PropertyGetter : &PropertySetter, 1);
                PushBound(state, 1);
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }

        int ElementNewIndex(lua_State* state)
        {
            Table::StructField& field = CheckElement(state, 1);
            const std::string key = Key(state, 2);
            if (key == "offset")
                field.Offset = static_cast<int>(luaL_checkinteger(state, 3));
            else if (key == "name")
                field.Name = luaL_checkstring(state, 3);
            else if (key == "vartype")
                ApplyElementVarType(field, luaL_checkinteger(state, 3));
            else if (key == "bytesize")
                field.Size = static_cast<int>(luaL_checkinteger(state, 3));
            else if (key == "displaymethod")
            {
                const std::string method = Lower(luaL_checkstring(state, 3));
                field.Display = method.find("hex") != std::string::npos ? Table::FieldDisplay::Hex :
                    method.find("binary") != std::string::npos ? Table::FieldDisplay::Binary :
                    method.find("signed") != std::string::npos && method.find("unsigned") == std::string::npos ? Table::FieldDisplay::Signed : Table::FieldDisplay::Unsigned;
            }
            else if (key == "bitstart" || key == "bitlength")
            {
                const int value = static_cast<int>(luaL_checkinteger(state, 3));
                if (key == "bitstart")
                    field.BitStart = std::clamp(value, 0, 63);
                else
                    field.BitLength = std::clamp(value, 1, 64);
                field.BitLength = std::clamp(field.BitLength, 1, 64 - field.BitStart);
                if (field.Type == Scan::ValueType::Binary)
                    field.Size = Table::BitFieldSize(field.BitStart, field.BitLength);
            }
            else if (key == "childstruct")
            {
                if (lua_isnil(state, 3))
                    field.ChildStructure.clear();
                else
                    field.ChildStructure = CheckStructure(state, 3).Name;
            }
            else if (key == "customtype" || key == "customtypename")
            {
                if (lua_isnil(state, 3))
                {
                    field.CustomType.clear();
                }
                else
                {
                    const CustomType& type = lua_type(state, 3) == LUA_TSTRING ? CheckCustomTypeName(state, lua_tostring(state, 3)) : CheckCustomType(state, 3);
                    field.CustomType = type.Name;
                    field.Type = Scan::ValueType::ByteArray;
                    field.Size = type.ByteCount();
                }
            }
            Touch(state);
            return 0;
        }

        int GetStructureCount(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(Owner(state).Structures().Items.size()));
            return 1;
        }

        int GetStructure(lua_State* state)
        {
            const lua_Integer index = luaL_checkinteger(state, 1);
            auto& items = Owner(state).Structures().Items;
            PushStructure(state, index >= 0 && index < static_cast<lua_Integer>(items.size()) ? items[static_cast<std::size_t>(index)].get() : nullptr);
            return 1;
        }

        int CreateStructure(lua_State* state)
        {
            auto definition = std::make_unique<Table::StructDefinition>();
            definition->Name = luaL_optstring(state, 1, "unnamed structure");
            Table::StructDefinition* raw = definition.get();
            Detached(state).push_back(std::move(definition));
            PushStructure(state, raw);
            return 1;
        }

        int CreateStructureForm(lua_State* state)
        {
            std::uint64_t address = 0;
            if (lua_type(state, 1) == LUA_TSTRING)
                address = Owner(state).Evaluate(lua_tostring(state, 1)).value_or(0);
            else
                address = Number(state, 1);
            UiRequest request{ UiRequestKind::StructureDissect, address, {} };
            if (lua_type(state, 3) == LUA_TSTRING)
                request.Text = lua_tostring(state, 3);
            Owner(state).Request(std::move(request));
            return 0;
        }

        int DetachedGc(lua_State* state)
        {
            static_cast<DetachedList*>(lua_touserdata(state, 1))->~DetachedList();
            return 0;
        }

        void RegisterStructures(lua_State* state)
        {
            void* memory = lua_newuserdatauv(state, sizeof(DetachedList), 0);
            new (memory) DetachedList();
            lua_newtable(state);
            lua_pushcfunction(state, &DetachedGc);
            lua_setfield(state, -2, "__gc");
            lua_setmetatable(state, -2);
            lua_setfield(state, LUA_REGISTRYINDEX, kDetachedKey);
        }

        std::int64_t FileSeconds(std::filesystem::file_time_type time)
        {
            const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
            return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
        }

        int LfsAttributes(lua_State* state)
        {
            const std::filesystem::path path = Utf8ToWide(luaL_checkstring(state, 1));
            std::error_code error;
            const std::filesystem::file_status status = std::filesystem::status(path, error);
            if (error || !std::filesystem::exists(status))
            {
                lua_pushnil(state);
                lua_pushfstring(state, "cannot obtain information from file '%s': No such file or directory", lua_tostring(state, 1));
                lua_pushinteger(state, 2);
                return 3;
            }
            const bool directory = std::filesystem::is_directory(status);
            const std::uintmax_t size = directory ? 0 : std::filesystem::file_size(path, error);
            const std::int64_t modified = FileSeconds(std::filesystem::last_write_time(path, error));
            const auto field = [&](const char* name)
                {
                    const std::string wanted = name;
                    if (wanted == "mode") lua_pushstring(state, directory ? "directory" : "file");
                    else if (wanted == "size") lua_pushinteger(state, static_cast<lua_Integer>(size));
                    else if (wanted == "modification" || wanted == "access" || wanted == "change") lua_pushinteger(state, modified);
                    else if (wanted == "permissions") lua_pushstring(state, "rw-rw-rw-");
                    else if (wanted == "nlink") lua_pushinteger(state, 1);
                    else lua_pushinteger(state, 0);
                };
            if (lua_type(state, 2) == LUA_TSTRING)
            {
                field(lua_tostring(state, 2));
                return 1;
            }
            lua_newtable(state);
            for (const char* name : { "mode", "size", "modification", "access", "change", "permissions", "nlink", "dev", "ino", "uid", "gid", "rdev", "blocks", "blksize" })
            {
                field(name);
                lua_setfield(state, -2, name);
            }
            return 1;
        }

        int LfsResult(lua_State* state, bool ok, const std::error_code& error)
        {
            if (ok)
            {
                lua_pushboolean(state, 1);
                return 1;
            }
            lua_pushnil(state);
            lua_pushstring(state, error ? error.message().c_str() : "File exists");
            lua_pushinteger(state, error ? error.value() : 17);
            return 3;
        }

        int LfsMkdir(lua_State* state)
        {
            std::error_code error;
            const bool created = std::filesystem::create_directory(Utf8ToWide(luaL_checkstring(state, 1)), error);
            return LfsResult(state, created && !error, error);
        }

        int LfsRmdir(lua_State* state)
        {
            std::error_code error;
            const std::filesystem::path path = Utf8ToWide(luaL_checkstring(state, 1));
            const bool removed = std::filesystem::is_directory(path, error) && std::filesystem::is_empty(path, error) && std::filesystem::remove(path, error);
            return LfsResult(state, removed && !error, error);
        }

        int LfsCurrentDir(lua_State* state)
        {
            std::error_code error;
            lua_pushstring(state, WideToUtf8(std::filesystem::current_path(error).wstring()).c_str());
            return 1;
        }

        int LfsChdir(lua_State* state)
        {
            std::error_code error;
            std::filesystem::current_path(Utf8ToWide(luaL_checkstring(state, 1)), error);
            return LfsResult(state, !error, error);
        }

        int LfsTouch(lua_State* state)
        {
            const std::filesystem::path path = Utf8ToWide(luaL_checkstring(state, 1));
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                return LfsResult(state, false, std::make_error_code(std::errc::no_such_file_or_directory));
            std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), error);
            return LfsResult(state, !error, error);
        }

        int LfsDirNext(lua_State* state)
        {
            const lua_Integer index = lua_tointeger(state, lua_upvalueindex(2));
            lua_rawgeti(state, lua_upvalueindex(1), index);
            if (lua_isnil(state, -1))
                return 1;
            lua_pushinteger(state, index + 1);
            lua_replace(state, lua_upvalueindex(2));
            return 1;
        }

        int LfsDir(lua_State* state)
        {
            const std::filesystem::path path = Utf8ToWide(luaL_checkstring(state, 1));
            std::error_code error;
            if (!std::filesystem::is_directory(path, error))
                return luaL_error(state, "cannot open %s: No such file or directory", lua_tostring(state, 1));
            lua_newtable(state);
            lua_pushstring(state, ".");
            lua_rawseti(state, -2, 1);
            lua_pushstring(state, "..");
            lua_rawseti(state, -2, 2);
            lua_Integer index = 3;
            for (const auto& entry : std::filesystem::directory_iterator(path, error))
            {
                lua_pushstring(state, WideToUtf8(entry.path().filename().wstring()).c_str());
                lua_rawseti(state, -2, index++);
            }
            lua_pushinteger(state, 1);
            lua_pushcclosure(state, &LfsDirNext, 2);
            return 1;
        }

        void RegisterLfs(lua_State* state)
        {
            lua_getglobal(state, "lfs");
            if (lua_istable(state, -1))
            {
                lua_pop(state, 1);
                return;
            }
            lua_pop(state, 1);
            const luaL_Reg functions[] = {
                { "attributes", &LfsAttributes },
                { "symlinkattributes", &LfsAttributes },
                { "mkdir", &LfsMkdir },
                { "rmdir", &LfsRmdir },
                { "currentdir", &LfsCurrentDir },
                { "chdir", &LfsChdir },
                { "touch", &LfsTouch },
                { "dir", &LfsDir },
                { nullptr, nullptr },
            };
            luaL_newlib(state, functions);
            lua_pushstring(state, "LuaFileSystem 1.8.0");
            lua_setfield(state, -2, "_VERSION");
            lua_pushvalue(state, -1);
            lua_setglobal(state, "lfs");
            lua_getglobal(state, "package");
            lua_getfield(state, -1, "loaded");
            lua_pushvalue(state, -3);
            lua_setfield(state, -2, "lfs");
            lua_pop(state, 3);
        }

        int ScreenMetric(lua_State* state, int metric)
        {
            lua_pushinteger(state, ::GetSystemMetrics(metric));
            return 1;
        }

        int WorkArea(lua_State* state, bool width)
        {
            RECT area{};
            ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
            lua_pushinteger(state, width ? area.right - area.left : area.bottom - area.top);
            return 1;
        }

        void Metatable(lua_State* state, const char* name, lua_CFunction index, lua_CFunction newIndex, lua_CFunction gc)
        {
            luaL_newmetatable(state, name);
            lua_pushcfunction(state, index);
            lua_setfield(state, -2, "__index");
            if (newIndex)
            {
                lua_pushcfunction(state, newIndex);
                lua_setfield(state, -2, "__newindex");
            }
            if (gc)
            {
                lua_pushcfunction(state, gc);
                lua_setfield(state, -2, "__gc");
            }
            lua_pop(state, 1);
        }
    }

    void PushStructureObject(lua_State* state, Table::StructDefinition* definition)
    {
        PushStructure(state, definition);
    }

    void RunCompatScripts(LuaEngine& engine)
    {
        for (const CompatScript& script : CompatScripts())
        {
            const Status status = engine.Run(script.Text(), script.Name);
            if (!status)
                LOG_ERROR("Lua", "Compatibility script " + std::string(script.Name) + " failed: " + status.Text);
        }
    }

    void RegisterCompat(LuaEngine& engine, lua_State* state)
    {
        (void)engine;
        Metatable(state, kStreamType, &StreamIndex, &StreamNewIndex, &StreamGc);
        Metatable(state, kTableFileType, &TableFileIndex, nullptr, nullptr);
        Metatable(state, kMemScanType, &MemScanIndex, &MemScanNewIndex, &ScanGc);
        Metatable(state, kFoundListType, &FoundListIndex, nullptr, &ScanGc);
        Metatable(state, kStructureType, &StructureIndex, &StructureNewIndex, nullptr);
        Metatable(state, kCustomTypeType, &CustomTypeIndex, &CustomTypeNewIndex, nullptr);
        Metatable(state, kInternetType, &InternetIndex, nullptr, &InternetGc);
        Metatable(state, kElementType, &ElementIndex, &ElementNewIndex, nullptr);
        Metatable(state, kElementListType, &ElementListIndex, nullptr, nullptr);
        luaL_getmetatable(state, kElementListType);
        lua_pushcfunction(state, &ElementListLength);
        lua_setfield(state, -2, "__len");
        lua_pop(state, 1);
        RegisterStructures(state);
        RegisterLfs(state);

        const luaL_Reg functions[] = {
            { "registerLuaFunctionHighlight", &Nothing },
            { "unregisterLuaFunctionHighlight", &Nothing },
            { "processMessages", &Nothing },
            { "checkSynchronize", &Nothing },
            { "getAddress", &GetAddressCompat },
            { "getAddressFromName", &GetAddressCompat },
            { "getAddressSafe", &GetAddressSafeCompat },
            { "enableWindowsSymbols", &EnableWindowsSymbols },
            { "waitForPDB", &WaitForPdb },
            { "searchPDBWhileLoading", &Nothing },
            { "findTableFile", &FindTableFile },
            { "createTableFile", &CreateTableFile },
            { "createMemoryStream", &CreateMemoryStream },
            { "createStringStream", &CreateStringStream },
            { "createFileStream", &CreateFileStream },
            { "readByteLocal", &ReadLocalValue<std::uint8_t> },
            { "readSmallIntegerLocal", &ReadLocalValue<std::uint16_t> },
            { "readIntegerLocal", &ReadLocalValue<std::uint32_t> },
            { "readQwordLocal", &ReadLocalValue<std::uint64_t> },
            { "readPointerLocal", &ReadLocalValue<std::uint64_t> },
            { "readFloatLocal", &ReadLocalValue<float> },
            { "readDoubleLocal", &ReadLocalValue<double> },
            { "readStringLocal", &ReadStringLocal },
            { "readBytesLocal", &ReadBytesLocal },
            { "writeByteLocal", &WriteLocalValue<std::uint8_t> },
            { "writeSmallIntegerLocal", &WriteLocalValue<std::uint16_t> },
            { "writeIntegerLocal", &WriteLocalValue<std::uint32_t> },
            { "writeQwordLocal", &WriteLocalValue<std::uint64_t> },
            { "writePointerLocal", &WriteLocalValue<std::uint64_t> },
            { "writeFloatLocal", &WriteLocalValue<float> },
            { "writeDoubleLocal", &WriteLocalValue<double> },
            { "writeStringLocal", &WriteStringLocal },
            { "writeBytesLocal", &WriteBytesLocal },
            { "executeCodeLocal", &ExecuteCodeLocal },
            { "executeCodeLocalEx", &ExecuteCodeLocalEx },
            { "extractFileName", &ExtractFileName },
            { "extractFileNameWithoutExt", &ExtractFileNameWithoutExt },
            { "extractFilePath", &ExtractFilePath },
            { "extractFileExt", &ExtractFileExt },
            { "writeToClipboard", &WriteToClipboard },
            { "readFromClipboard", &ReadFromClipboard },
            { "cheatEngineIs64Bit", &ReturnTrue },
            { "getFileVersion", &GetFileVersion },
            { "md5file", &Md5File },
            { "md5memory", &Md5Memory },
            { "stringToMD5String", &StringToMd5 },
            { "userDataToInteger", &UserDataToInteger },
            { "integerToUserData", &IntegerToUserData },
            { "pause", &PauseProcess },
            { "unpause", &UnpauseProcess },
            { "closeCE", &CloseCe },
            { "getFileList", &GetFileList },
            { "getDirectoryList", &GetDirectoryList },
            { "enumMemoryRegions", &EnumMemoryRegions },
            { "inModule", &InModule },
            { "inSystemModule", &InSystemModule },
            { "getModuleNameFromAddress", &GetModuleNameFromAddress },
            { "deleteAllRegisteredSymbols", &DeleteAllRegisteredSymbols },
            { "getCheatEngineProcessID", &GetCheatEngineProcessId },
            { "findWindow", &FindWindowCompat },
            { "getWindow", &GetWindowCompat },
            { "getWindowCaption", &GetWindowCaption },
            { "getWindowClassName", &GetWindowClassName },
            { "getWindowProcessID", &GetWindowProcessId },
            { "getForegroundWindow", &GetForegroundWindowCompat },
            { "printf", &Printf },
            { "detachIfPossible", &DetachIfPossible },
            { "registerAutoAssemblerCommand", &RegisterAutoAssemblerCommand },
            { "unregisterAutoAssemblerCommand", &UnregisterAutoAssemblerCommand },
            { "registerAutoAssemblerTemplate", &RegisterAutoAssemblerTemplate },
            { "unregisterAutoAssemblerTemplate", &Nothing },
            { "createMemScan", &CreateMemScan },
            { "createFoundList", &CreateFoundList },
            { "getScreenWidth", [](lua_State* s) -> int { return ScreenMetric(s, SM_CXSCREEN); } },
            { "getScreenHeight", [](lua_State* s) -> int { return ScreenMetric(s, SM_CYSCREEN); } },
            { "getWorkAreaWidth", [](lua_State* s) -> int { return WorkArea(s, true); } },
            { "getWorkAreaHeight", [](lua_State* s) -> int { return WorkArea(s, false); } },
            { "getScreenDPI", [](lua_State* s) -> int { lua_pushinteger(s, static_cast<lua_Integer>(::GetDpiForSystem())); return 1; } },
            { "getMousePos", [](lua_State* s) -> int
                {
                    POINT point{};
                    ::GetCursorPos(&point);
                    lua_pushinteger(s, point.x);
                    lua_pushinteger(s, point.y);
                    return 2;
                } },
            { "getTempFolder", [](lua_State* s) -> int
                {
                    wchar_t path[MAX_PATH] = L"";
                    ::GetTempPathW(MAX_PATH, path);
                    lua_pushstring(s, WideToUtf8(path).c_str());
                    return 1;
                } },
            { "getInternet", &GetInternet },
            { "shellExecute", &ShellExecuteCompat },
            { "ShellExecute", &ShellExecuteCompat },
            { "registerCustomTypeLua", &RegisterCustomTypeLua },
            { "getCustomType", &GetCustomType },
            { "getCustomTypeList", &GetCustomTypeList },
            { "getStructureCount", &GetStructureCount },
            { "getStructure", &GetStructure },
            { "createStructure", &CreateStructure },
            { "createStructureForm", &CreateStructureForm },
            { "registerStructureNameLookup", &RegisterStructureNameLookup },
            { "unregisterStructureNameLookup", &UnregisterStructureNameLookup },
            { "registerStructureDissectOverride", &RegisterStructureDissectOverride },
            { "unregisterStructureDissectOverride", &UnregisterStructureDissectOverride },
            { "registerSymbolLookupCallback", &RegisterSymbolLookupCallback },
            { "unregisterSymbolLookupCallback", &UnregisterSymbolLookupCallback },
            { "__manifoldTableFileNames", &TableFileNames },
            { "__manifoldFileDialog", &FileDialog },
            { "__manifoldFolderDialog", &FolderDialog },
            { "__manifoldColorDialog", &ColorDialog },
            { "__manifoldSelectedCodeAddress", &SelectedCodeAddress },
            { "__manifoldSetSelectedCodeAddress", &SetSelectedCodeAddress },
            { "__manifoldSelectedHexAddress", &SelectedHexAddress },
            { nullptr, nullptr },
        };
        for (const luaL_Reg* function = functions; function->name; ++function)
        {
            lua_pushcfunction(state, function->func);
            lua_setglobal(state, function->name);
            if (function->name[0] != '_')
                NoteFunction(function->name);
        }
    }
}
