#include "App/Engine.h"
#include "Core/Errors.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaCompatScripts.h"
#include "Lua/LuaEngine.h"
#include "Target/Process.h"

#include "lua.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <windows.h>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kDocsKey = "Manifold.Docs";
        constexpr const char* kMarkdownKey = "Manifold.Markdown";

        struct NamedCode
        {
            std::string_view Name;
            ErrorCode Code;
        };

        constexpr NamedCode kNamedCodes[] = {
#define MANIFOLD_NAMED_CODE(name, number) { #name, ErrorCode::name },
            MANIFOLD_ERROR_CODES(MANIFOLD_NAMED_CODE)
#undef MANIFOLD_NAMED_CODE
        };

        constexpr const char* kTags[] = { "DEBUG  ", "INFO   ", "SUCCESS", "WARNING", "ERROR  " };
        constexpr OutputKind kKinds[] = { OutputKind::Debug, OutputKind::Info, OutputKind::Success, OutputKind::Warning, OutputKind::Error };
        constexpr LogLevel kLevels[] = { LogLevel::Debug, LogLevel::Info, LogLevel::Success, LogLevel::Warning, LogLevel::Error };

        enum class MemoryType
        {
            I8,
            U8,
            I16,
            U16,
            I32,
            U32,
            I64,
            U64,
            Float,
            Double,
            Pointer,
            Bytes,
            String
        };

        constexpr std::pair<std::string_view, MemoryType> kMemoryTypes[] = {
            { "i8", MemoryType::I8 }, { "u8", MemoryType::U8 }, { "i16", MemoryType::I16 }, { "u16", MemoryType::U16 },
            { "i32", MemoryType::I32 }, { "u32", MemoryType::U32 }, { "i64", MemoryType::I64 }, { "u64", MemoryType::U64 },
            { "float", MemoryType::Float }, { "double", MemoryType::Double }, { "pointer", MemoryType::Pointer },
            { "bytes", MemoryType::Bytes }, { "string", MemoryType::String },
        };

        Engine& Owner(lua_State* state)
        {
            return EngineOf(state).Owner();
        }

        std::string MessageOf(const Status& status)
        {
            const std::string suffix = " (" + ErrorName(status.Code) + ")";
            std::string text = status.Text;
            if (text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0)
                text.resize(text.size() - suffix.size());
            return text;
        }

        int Failed(lua_State* state, const Status& status)
        {
            lua_pushnil(state);
            lua_pushstring(state, ErrorName(status.Code).c_str());
            lua_pushstring(state, MessageOf(status).c_str());
            return 3;
        }

        int Failed(lua_State* state, ErrorCode code, std::string_view detail = {})
        {
            return Failed(state, Status::Fail(code, detail));
        }

        int Fail(lua_State* state)
        {
            const std::string_view name = luaL_checkstring(state, 1);
            std::size_t length = 0;
            const char* detail = luaL_optlstring(state, 2, "", &length);
            ErrorCode code = ErrorCode::LuaRuntime;
            for (const NamedCode& named : kNamedCodes)
            {
                if (named.Name == name)
                {
                    code = named.Code;
                    break;
                }
            }
            const Status status = Status::Fail(code, std::string_view(detail, length));
            lua_pushstring(state, ErrorName(code).c_str());
            lua_pushstring(state, MessageOf(status).c_str());
            return 2;
        }

        bool Resolve(lua_State* state, int index, std::uint64_t& address, Status& error)
        {
            if (lua_type(state, index) == LUA_TNUMBER)
            {
                address = static_cast<std::uint64_t>(lua_tointeger(state, index));
                return true;
            }
            const char* text = luaL_checkstring(state, index);
            const auto value = Owner(state).Evaluate(text, &error);
            if (!value)
            {
                if (error.Ok())
                    error = Status::Fail(ErrorCode::ExprSyntax, text);
                return false;
            }
            address = *value;
            return true;
        }

        std::shared_ptr<Target::Process> Opened(lua_State* state, Status& error)
        {
            const auto& process = Owner(state).Process();
            if (!process)
            {
                error = Status::Fail(ErrorCode::NoProcess);
                return nullptr;
            }
            if (!process->Alive())
            {
                error = Status::Fail(ErrorCode::ProcessExited);
                return nullptr;
            }
            return process;
        }

        void SetString(lua_State* state, const char* key, std::string_view value)
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }

        void SetInteger(lua_State* state, const char* key, std::uint64_t value)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(value));
            lua_setfield(state, -2, key);
        }

        void SetBoolean(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        void PushProcess(lua_State* state, const Target::Process& process)
        {
            lua_createtable(state, 0, 4);
            SetInteger(state, "id", process.Id());
            SetString(state, "name", process.Name());
            SetString(state, "path", WideToUtf8(process.Path()));
            SetBoolean(state, "is64Bit", process.Is64Bit());
        }

        void PushModule(lua_State* state, const Target::ModuleInfo& module)
        {
            lua_createtable(state, 0, 4);
            SetString(state, "name", module.Name);
            SetInteger(state, "base", module.Base);
            SetInteger(state, "size", module.Size);
            SetString(state, "path", WideToUtf8(module.Path));
        }

        std::string ClockText()
        {
            SYSTEMTIME now{};
            ::GetLocalTime(&now);
            char text[16] = {};
            std::snprintf(text, sizeof(text), "%02u:%02u:%02u", static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond));
            return text;
        }

        int Write(lua_State* state)
        {
            const int level = std::clamp(static_cast<int>(luaL_checkinteger(state, 1)), 0, 4);
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 2, &length);
            const std::string channel = luaL_optstring(state, 3, "");
            const std::string file = luaL_optstring(state, 4, "");
            const std::string function = luaL_optstring(state, 5, "");
            const int line = static_cast<int>(luaL_optinteger(state, 6, 0));
            std::string message(text, length);
            if (level >= 3)
            {
                const std::string first = message.substr(0, message.find('\n'));
                RuntimeLog::Write(kLevels[level], "Lua", file.empty() ? "lua" : file.c_str(), function.empty() ? "chunk" : function.c_str(), line,
                    channel.empty() ? first : "[" + channel + "] " + first);
            }
            EngineOf(state).PrintEntry(kKinds[level], ClockText(), kTags[level], channel, std::move(message));
            return 0;
        }

        int ShowOutput(lua_State* state)
        {
            UiRequest request;
            request.Kind = UiRequestKind::LuaEngine;
            Owner(state).Request(std::move(request));
            return 0;
        }

        int ClearOutput(lua_State* state)
        {
            EngineOf(state).ClearOutput();
            return 0;
        }

        int ShowOnPrint(lua_State* state)
        {
            lua_pushboolean(state, EngineOf(state).ShowOnPrint() ? 1 : 0);
            return 1;
        }

        int SetShowOnPrint(lua_State* state)
        {
            EngineOf(state).SetShowOnPrint(lua_toboolean(state, 1) != 0);
            return 0;
        }

        int Version(lua_State* state)
        {
            lua_pushstring(state, MANIFOLD_VERSION_TEXT);
            return 1;
        }

        int ProcessIsOpen(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            lua_pushboolean(state, process && process->Alive() ? 1 : 0);
            return 1;
        }

        int Is64Bit(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            if (!process)
                lua_pushnil(state);
            else
                lua_pushboolean(state, process->Is64Bit() ? 1 : 0);
            return 1;
        }

        int ProcessInfo(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return Failed(state, error);
            PushProcess(state, *process);
            return 1;
        }

        int ProcessOpen(lua_State* state)
        {
            Engine& engine = Owner(state);
            const Status status = lua_type(state, 1) == LUA_TNUMBER ? engine.OpenProcess(static_cast<std::uint32_t>(lua_tointeger(state, 1)))
                                                                    : engine.OpenProcessByName(luaL_checkstring(state, 1));
            if (!status)
                return Failed(state, status);
            return ProcessInfo(state);
        }

        int ProcessFind(lua_State* state)
        {
            const std::string name = luaL_optstring(state, 1, "");
            lua_newtable(state);
            lua_Integer index = 1;
            for (const Target::ProcessEntry& entry : Target::ListProcesses(true))
            {
                if (!name.empty() && !ContainsNoCase(entry.Name, name))
                    continue;
                lua_createtable(state, 0, 5);
                SetInteger(state, "id", entry.Id);
                SetString(state, "name", entry.Name);
                SetString(state, "path", WideToUtf8(entry.Path));
                SetString(state, "windowTitle", entry.WindowTitle);
                SetBoolean(state, "is64Bit", entry.Is64Bit);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int MainModule(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return Failed(state, error);
            const auto module = process->MainModule();
            if (!module)
                return Failed(state, ErrorCode::ModulesUnreadable);
            PushModule(state, *module);
            return 1;
        }

        int Module(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return Failed(state, error);
            const std::string name = luaL_checkstring(state, 1);
            const auto module = process->FindModule(name);
            if (!module)
                return Failed(state, ErrorCode::ModuleNotFound, name);
            PushModule(state, *module);
            return 1;
        }

        int Modules(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return Failed(state, error);
            const Target::ModuleList modules = process->Modules();
            if (!modules)
                return Failed(state, ErrorCode::ModulesUnreadable);
            lua_createtable(state, static_cast<int>(modules->size()), 0);
            lua_Integer index = 1;
            for (const Target::ModuleInfo& module : *modules)
            {
                PushModule(state, module);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int Address(lua_State* state)
        {
            std::uint64_t address = 0;
            Status error;
            if (!Resolve(state, 1, address, error))
                return Failed(state, error);
            lua_pushinteger(state, static_cast<lua_Integer>(address));
            return 1;
        }

        int Describe(lua_State* state)
        {
            std::uint64_t address = 0;
            Status error;
            if (!Resolve(state, 1, address, error))
                return Failed(state, error);
            const std::string text = Owner(state).Describe(address);
            lua_pushstring(state, text.c_str());
            return 1;
        }

        std::optional<MemoryType> TypeNamed(std::string_view name)
        {
            for (const auto& [text, type] : kMemoryTypes)
            {
                if (text == name)
                    return type;
            }
            return std::nullopt;
        }

        template <class T>
        int PushRead(lua_State* state, const Target::Process& process, std::uint64_t address)
        {
            T value{};
            if (!process.ReadExact(address, &value, sizeof(T)))
                return Failed(state, ErrorCode::ReadFailed, Hex(address));
            if constexpr (std::is_floating_point_v<T>)
                lua_pushnumber(state, static_cast<lua_Number>(value));
            else
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            return 1;
        }

        int ReadString(lua_State* state, const Target::Process& process, std::uint64_t address, std::size_t length, bool wide)
        {
            std::vector<std::uint8_t> bytes(length * (wide ? 2 : 1));
            const std::size_t got = process.Read(address, bytes.data(), bytes.size());
            if (got == 0)
                return Failed(state, ErrorCode::ReadFailed, Hex(address));
            if (wide)
            {
                std::wstring text;
                for (std::size_t i = 0; i + 1 < got; i += 2)
                {
                    const wchar_t c = static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8));
                    if (c == 0)
                        break;
                    text += c;
                }
                const std::string utf8 = WideToUtf8(text);
                lua_pushlstring(state, utf8.data(), utf8.size());
                return 1;
            }
            const auto end = std::find(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(got), std::uint8_t(0));
            lua_pushlstring(state, reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(end - bytes.begin()));
            return 1;
        }

        int Read(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return Failed(state, error);
            std::uint64_t address = 0;
            if (!Resolve(state, 1, address, error))
                return Failed(state, error);
            const auto type = TypeNamed(luaL_checkstring(state, 2));
            if (!type)
                return luaL_argerror(state, 2, "unknown memory type");
            switch (*type)
            {
            case MemoryType::I8: return PushRead<std::int8_t>(state, *process, address);
            case MemoryType::U8: return PushRead<std::uint8_t>(state, *process, address);
            case MemoryType::I16: return PushRead<std::int16_t>(state, *process, address);
            case MemoryType::U16: return PushRead<std::uint16_t>(state, *process, address);
            case MemoryType::I32: return PushRead<std::int32_t>(state, *process, address);
            case MemoryType::U32: return PushRead<std::uint32_t>(state, *process, address);
            case MemoryType::I64: return PushRead<std::int64_t>(state, *process, address);
            case MemoryType::U64: return PushRead<std::uint64_t>(state, *process, address);
            case MemoryType::Float: return PushRead<float>(state, *process, address);
            case MemoryType::Double: return PushRead<double>(state, *process, address);
            case MemoryType::Pointer:
                return process->Is64Bit() ? PushRead<std::uint64_t>(state, *process, address) : PushRead<std::uint32_t>(state, *process, address);
            case MemoryType::Bytes:
            {
                const lua_Integer count = luaL_checkinteger(state, 3);
                if (count < 1 || count > 0x1000000)
                    return luaL_argerror(state, 3, "count out of range");
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
                if (!process->ReadExact(address, bytes.data(), bytes.size()))
                    return Failed(state, ErrorCode::ReadFailed, Hex(address));
                lua_createtable(state, static_cast<int>(bytes.size()), 0);
                for (std::size_t i = 0; i < bytes.size(); ++i)
                {
                    lua_pushinteger(state, bytes[i]);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                }
                return 1;
            }
            case MemoryType::String:
            {
                const lua_Integer length = std::clamp<lua_Integer>(luaL_optinteger(state, 4, 256), 1, 0x100000);
                return ReadString(state, *process, address, static_cast<std::size_t>(length), lua_toboolean(state, 5) != 0);
            }
            }
            return luaL_argerror(state, 2, "unknown memory type");
        }

        template <class T>
        void AppendValue(std::vector<std::uint8_t>& bytes, T value)
        {
            const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(T));
        }

        int WriteMemory(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return Failed(state, error);
            std::uint64_t address = 0;
            if (!Resolve(state, 1, address, error))
                return Failed(state, error);
            const auto type = TypeNamed(luaL_checkstring(state, 2));
            if (!type)
                return luaL_argerror(state, 2, "unknown memory type");
            std::vector<std::uint8_t> bytes;
            const auto integer = [&]() { return static_cast<std::int64_t>(lua_tointeger(state, 3)); };
            switch (*type)
            {
            case MemoryType::I8:
            case MemoryType::U8: AppendValue(bytes, static_cast<std::uint8_t>(integer())); break;
            case MemoryType::I16:
            case MemoryType::U16: AppendValue(bytes, static_cast<std::uint16_t>(integer())); break;
            case MemoryType::I32:
            case MemoryType::U32: AppendValue(bytes, static_cast<std::uint32_t>(integer())); break;
            case MemoryType::I64:
            case MemoryType::U64: AppendValue(bytes, static_cast<std::uint64_t>(integer())); break;
            case MemoryType::Float: AppendValue(bytes, static_cast<float>(lua_tonumber(state, 3))); break;
            case MemoryType::Double: AppendValue(bytes, static_cast<double>(lua_tonumber(state, 3))); break;
            case MemoryType::Pointer:
                if (process->Is64Bit())
                    AppendValue(bytes, static_cast<std::uint64_t>(integer()));
                else
                    AppendValue(bytes, static_cast<std::uint32_t>(integer()));
                break;
            case MemoryType::Bytes:
                if (lua_type(state, 3) == LUA_TSTRING)
                {
                    std::size_t length = 0;
                    const char* text = lua_tolstring(state, 3, &length);
                    bytes.assign(reinterpret_cast<const std::uint8_t*>(text), reinterpret_cast<const std::uint8_t*>(text) + length);
                }
                else
                {
                    luaL_checktype(state, 3, LUA_TTABLE);
                    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, 3));
                    bytes.reserve(static_cast<std::size_t>(count));
                    for (lua_Integer i = 1; i <= count; ++i)
                    {
                        lua_rawgeti(state, 3, i);
                        bytes.push_back(static_cast<std::uint8_t>(lua_tointeger(state, -1)));
                        lua_pop(state, 1);
                    }
                }
                break;
            case MemoryType::String:
            {
                std::size_t length = 0;
                const char* text = luaL_checklstring(state, 3, &length);
                const bool wide = lua_toboolean(state, 4) != 0;
                const bool terminate = lua_isnoneornil(state, 5) || lua_toboolean(state, 5) != 0;
                if (wide)
                {
                    for (const wchar_t c : Utf8ToWide(std::string_view(text, length)))
                        AppendValue(bytes, static_cast<std::uint16_t>(c));
                    if (terminate)
                        AppendValue(bytes, std::uint16_t(0));
                }
                else
                {
                    bytes.assign(reinterpret_cast<const std::uint8_t*>(text), reinterpret_cast<const std::uint8_t*>(text) + length);
                    if (terminate)
                        bytes.push_back(0);
                }
                break;
            }
            }
            if (!bytes.empty())
            {
                const Status status = process->Write(address, bytes.data(), bytes.size());
                if (!status)
                    return Failed(state, status);
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int IsReadable(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            std::uint64_t address = 0;
            lua_pushboolean(state, process && Resolve(state, 1, address, error) && process->IsReadable(address) ? 1 : 0);
            return 1;
        }

        std::string FieldText(lua_State* state, int index, const char* key)
        {
            lua_getfield(state, index, key);
            std::string text = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : "";
            lua_pop(state, 1);
            return text;
        }
    }

    void RegisterManifoldApi(LuaEngine& engine, lua_State* state)
    {
        (void)engine;
        const luaL_Reg natives[] = {
            { "fail", &Fail },
            { "writeLine", &Write },
            { "showOutput", &ShowOutput },
            { "clearOutput", &ClearOutput },
            { "showOnPrint", &ShowOnPrint },
            { "setShowOnPrint", &SetShowOnPrint },
            { "version", &Version },
            { "processIsOpen", &ProcessIsOpen },
            { "is64Bit", &Is64Bit },
            { "processInfo", &ProcessInfo },
            { "processOpen", &ProcessOpen },
            { "processFind", &ProcessFind },
            { "mainModule", &MainModule },
            { "module", &Module },
            { "modules", &Modules },
            { "address", &Address },
            { "describe", &Describe },
            { "read", &Read },
            { "writeMemory", &WriteMemory },
            { "isReadable", &IsReadable },
            { nullptr, nullptr },
        };
        for (const CompatScript& script : ManifoldScripts())
        {
            const int top = lua_gettop(state);
            const std::string text = script.Text();
            if (luaL_loadbuffer(state, text.data(), text.size(), "=manifold") != LUA_OK)
            {
                LOG_ERROR("Lua", std::string("The manifold API script ") + script.Name + " does not load: " + (lua_tostring(state, -1) ? lua_tostring(state, -1) : "error"));
                lua_settop(state, top);
                continue;
            }
            lua_createtable(state, 0, static_cast<int>(std::size(natives)));
            luaL_setfuncs(state, natives, 0);
            if (lua_pcall(state, 1, 2, 0) != LUA_OK)
            {
                std::string message = lua_tostring(state, -1) ? lua_tostring(state, -1) : "";
                if (message.empty() && luaL_callmeta(state, -1, "__tostring") && lua_type(state, -1) == LUA_TSTRING)
                    message = lua_tostring(state, -1);
                LOG_ERROR("Lua", std::string("The manifold API script ") + script.Name + " failed: " + message);
                lua_settop(state, top);
                continue;
            }
            if (lua_isfunction(state, -1))
                lua_setfield(state, LUA_REGISTRYINDEX, kMarkdownKey);
            else
                lua_pop(state, 1);
            if (lua_istable(state, -1))
                lua_setfield(state, LUA_REGISTRYINDEX, kDocsKey);
            lua_settop(state, top);
        }
    }

    std::optional<ManifoldFunctionDoc> ManifoldDocOf(lua_State* state, int index)
    {
        if (!lua_isfunction(state, index))
            return std::nullopt;
        index = lua_absindex(state, index);
        const int top = lua_gettop(state);
        if (lua_getfield(state, LUA_REGISTRYINDEX, kDocsKey) != LUA_TTABLE)
        {
            lua_settop(state, top);
            return std::nullopt;
        }
        lua_pushvalue(state, index);
        if (lua_rawget(state, -2) != LUA_TTABLE)
        {
            lua_settop(state, top);
            return std::nullopt;
        }
        ManifoldFunctionDoc doc;
        doc.Path = FieldText(state, -1, "path");
        doc.Params = FieldText(state, -1, "params");
        doc.Returns = FieldText(state, -1, "returns");
        doc.Text = FieldText(state, -1, "text");
        lua_settop(state, top);
        return doc;
    }

    std::string ManifoldApiMarkdown(LuaEngine& engine)
    {
        lua_State* state = engine.State();
        const int top = lua_gettop(state);
        std::string text;
        if (lua_getfield(state, LUA_REGISTRYINDEX, kMarkdownKey) == LUA_TFUNCTION && lua_pcall(state, 0, 1, 0) == LUA_OK && lua_type(state, -1) == LUA_TSTRING)
            text = lua_tostring(state, -1);
        lua_settop(state, top);
        return text;
    }
}
