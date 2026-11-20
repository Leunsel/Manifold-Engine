#include "App/Engine.h"
#include "Core/Errors.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaCompatScripts.h"
#include "Lua/LuaDocBinding.h"
#include "Lua/LuaEngine.h"
#include "Lua/LuaUi.h"
#include "Lua/LuaUnreal.h"
#include "Scan/Scanner.h"
#include "Target/Process.h"
#include "Target/Symbols.h"

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
            Raw,
            String
        };

        constexpr std::pair<std::string_view, MemoryType> kMemoryTypes[] = {
            { "i8", MemoryType::I8 }, { "u8", MemoryType::U8 }, { "i16", MemoryType::I16 }, { "u16", MemoryType::U16 },
            { "i32", MemoryType::I32 }, { "u32", MemoryType::U32 }, { "i64", MemoryType::I64 }, { "u64", MemoryType::U64 },
            { "float", MemoryType::Float }, { "double", MemoryType::Double }, { "pointer", MemoryType::Pointer },
            { "bytes", MemoryType::Bytes }, { "raw", MemoryType::Raw }, { "string", MemoryType::String },
        };

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
            lua_pushstring(state, FailureMessage(status).c_str());
            return 2;
        }

        bool Resolve(lua_State* state, int index, std::uint64_t& address, Status& error)
        {
            const auto value = ResolveAddress(state, index, error);
            if (value)
                address = *value;
            return value.has_value();
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

        std::shared_ptr<Target::Process> OpenedForAccess(lua_State* state, Status& error)
        {
            const auto& process = Owner(state).Process();
            if (!process)
                error = Status::Fail(ErrorCode::NoProcess);
            return process;
        }

        int AccessFailed(lua_State* state, const Target::Process& process, const Status& status)
        {
            if (!process.Alive())
                return PushFailure(state, ErrorCode::ProcessExited);
            return PushFailure(state, status);
        }

        int ReadFailure(lua_State* state, const Target::Process& process, std::uint64_t address)
        {
            return AccessFailed(state, process, Status::Fail(ErrorCode::ReadFailed, Hex(address)));
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
            SetInteger(state, "id", Raw(process.Id()));
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

        int LogRecord(lua_State* state)
        {
            const int level = std::clamp(static_cast<int>(luaL_checkinteger(state, 1)), 0, 4);
            LuaEngine& engine = EngineOf(state);
            ConsoleEntry entry;
            entry.Kind = EntryKind::Log;
            entry.Level = kLevels[level];
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 2, &length);
            entry.Text.assign(text, length);
            entry.Channel = luaL_optstring(state, 3, "");
            entry.FieldsText = luaL_optstring(state, 4, "");
            if (lua_istable(state, 5))
                entry.Fields = engine.Snapshot(state, 5);
            entry.Traceback = luaL_optstring(state, 6, "");
            entry.Code = luaL_optstring(state, 7, "");
            const std::string where = luaL_optstring(state, 8, "");
            entry.Source.Chunk = luaL_optstring(state, 9, "");
            entry.Source.Function = luaL_optstring(state, 10, "");
            entry.Source.Line = static_cast<int>(luaL_optinteger(state, 11, 0));
            if (const std::size_t colon = where.rfind(':'); colon != std::string::npos && colon + 1 < where.size())
            {
                entry.Source.Chunk = where.substr(0, colon);
                entry.Source.Line = std::atoi(where.c_str() + colon + 1);
                entry.Source.Function.clear();
            }
            if (level >= 3)
            {
                const std::size_t cut = entry.Text.find('\n');
                std::string first = entry.Text.substr(0, cut);
                if (!where.empty())
                    first = where + ": " + first;
                if (!entry.Code.empty())
                    first += " (" + entry.Code + ")";
                if (cut != std::string::npos)
                    first += entry.Text.substr(cut);
                if (!entry.FieldsText.empty())
                    first += "  " + entry.FieldsText;
                if (!entry.Traceback.empty())
                    first += "\n" + entry.Traceback;
                const std::string chunk = entry.Source.Chunk.empty() ? std::string("lua") : entry.Source.Chunk;
                const std::string function = entry.Source.Function.empty() ? std::string("chunk") : entry.Source.Function;
                RuntimeLog::Write(kLevels[level], "Lua", chunk.c_str(), function.c_str(), entry.Source.Line, entry.Channel.empty() ? first : "[" + entry.Channel + "] " + first);
            }
            engine.AddEntry(std::move(entry));
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

        int UiMode(lua_State* state)
        {
            const Settings& options = Owner(state).Options();
            lua_createtable(state, 0, 2);
            lua_pushboolean(state, options.ReleaseMode ? 1 : 0);
            lua_setfield(state, -2, "release");
            lua_pushboolean(state, options.CompactMode ? 1 : 0);
            lua_setfield(state, -2, "compact");
            return 1;
        }

        int SetUiMode(lua_State* state)
        {
            Engine& engine = Owner(state);
            Settings& options = engine.Options();
            bool changed = false;
            if (!lua_isnoneornil(state, 1) && options.ReleaseMode != (lua_toboolean(state, 1) != 0))
            {
                options.ReleaseMode = lua_toboolean(state, 1) != 0;
                changed = true;
            }
            if (!lua_isnoneornil(state, 2) && options.CompactMode != (lua_toboolean(state, 2) != 0))
            {
                options.CompactMode = lua_toboolean(state, 2) != 0;
                changed = true;
            }
            if (changed)
                engine.SaveOptionsSoon();
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
                return PushFailure(state, error);
            PushProcess(state, *process);
            return 1;
        }

        int ProcessOpen(lua_State* state)
        {
            const Status status = OpenProcessArgument(Owner(state), state, 1);
            if (!status)
                return PushFailure(state, status);
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
                SetInteger(state, "id", Raw(entry.Id));
                SetString(state, "name", entry.Name);
                SetString(state, "path", WideToUtf8(entry.Path));
                SetString(state, "windowTitle", entry.WindowTitle);
                SetBoolean(state, "is64Bit", entry.Is64Bit);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int ProcessName(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const auto main = process->MainModule();
            lua_pushstring(state, main ? main->Name.c_str() : process->Name().c_str());
            return 1;
        }

        int MainModule(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const auto module = process->MainModule();
            if (!module)
                return PushFailure(state, ErrorCode::ModulesUnreadable);
            PushModule(state, *module);
            return 1;
        }

        int Module(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const std::string name = luaL_checkstring(state, 1);
            const auto module = process->FindModule(name);
            if (!module)
                return PushFailure(state, ErrorCode::ModuleNotFound, name);
            PushModule(state, *module);
            return 1;
        }

        int Modules(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const Target::ModuleList modules = process->Modules();
            if (!modules)
                return PushFailure(state, ErrorCode::ModulesUnreadable);
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
                return PushFailure(state, error);
            lua_pushinteger(state, static_cast<lua_Integer>(address));
            return 1;
        }

        int Describe(lua_State* state)
        {
            std::uint64_t address = 0;
            Status error;
            if (!Resolve(state, 1, address, error))
                return PushFailure(state, error);
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
                return ReadFailure(state, process, address);
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
                return ReadFailure(state, process, address);
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
            const auto process = OpenedForAccess(state, error);
            if (!process)
                return PushFailure(state, error);
            std::uint64_t address = 0;
            if (!Resolve(state, 1, address, error))
                return PushFailure(state, error);
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
                    return ReadFailure(state, *process, address);
                lua_createtable(state, static_cast<int>(bytes.size()), 0);
                for (std::size_t i = 0; i < bytes.size(); ++i)
                {
                    lua_pushinteger(state, bytes[i]);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                }
                return 1;
            }
            case MemoryType::Raw:
            {
                const lua_Integer count = luaL_checkinteger(state, 3);
                if (count < 1 || count > 0x1000000)
                    return luaL_argerror(state, 3, "count out of range");
                luaL_Buffer buffer;
                char* data = luaL_buffinitsize(state, &buffer, static_cast<std::size_t>(count));
                if (!process->ReadExact(address, data, static_cast<std::size_t>(count)))
                {
                    lua_pop(state, 1);
                    return ReadFailure(state, *process, address);
                }
                luaL_pushresultsize(&buffer, static_cast<std::size_t>(count));
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
            const auto process = OpenedForAccess(state, error);
            if (!process)
                return PushFailure(state, error);
            std::uint64_t address = 0;
            if (!Resolve(state, 1, address, error))
                return PushFailure(state, error);
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
            case MemoryType::Raw:
                if (lua_type(state, 3) == LUA_TSTRING)
                {
                    std::size_t length = 0;
                    const char* text = lua_tolstring(state, 3, &length);
                    bytes.assign(reinterpret_cast<const std::uint8_t*>(text), reinterpret_cast<const std::uint8_t*>(text) + length);
                }
                else
                {
                    luaL_checktype(state, 3, LUA_TTABLE);
                    bytes = ByteTable(state, 3);
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
                    return AccessFailed(state, *process, status);
            }
            lua_pushboolean(state, 1);
            return 1;
        }

        int IsReadable(lua_State* state)
        {
            Status error;
            const auto process = OpenedForAccess(state, error);
            std::uint64_t address = 0;
            lua_pushboolean(state, process && Resolve(state, 1, address, error) && process->IsReadable(address) ? 1 : 0);
            return 1;
        }

        Scan::Tristate TristateArgument(lua_State* state, int index)
        {
            if (lua_isnoneornil(state, index))
                return Scan::Tristate::Any;
            return lua_toboolean(state, index) ? Scan::Tristate::Yes : Scan::Tristate::No;
        }

        int ScanMemory(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const std::string text = luaL_checkstring(state, 1);
            const auto pattern = Scan::ParsePattern(text);
            if (!pattern)
                return PushFailure(state, ErrorCode::ScanBadPattern, text);
            std::uint64_t start = 0;
            std::uint64_t stop = ~0ull;
            if (!lua_isnoneornil(state, 2))
            {
                const std::string name = luaL_checkstring(state, 2);
                const auto module = process->FindModule(name);
                if (!module)
                    return PushFailure(state, ErrorCode::ModuleNotFound, name);
                start = module->Base;
                stop = module->Base + module->Size;
            }
            Scan::PatternFilter filter;
            filter.Executable = TristateArgument(state, 3);
            filter.Writable = TristateArgument(state, 4);
            filter.Divisor = static_cast<std::uint64_t>(std::max<lua_Integer>(luaL_optinteger(state, 5, 0), 0));
            const auto limit = static_cast<std::size_t>(std::clamp<lua_Integer>(luaL_optinteger(state, 6, 100000), 1, 1000000));
            const std::vector<std::uint64_t> hits = Scan::FindPattern(*process, *pattern, start, stop, filter, limit);
            lua_createtable(state, static_cast<int>(hits.size()), 0);
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                lua_pushinteger(state, static_cast<lua_Integer>(hits[i]));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int ModuleAt(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            std::uint64_t address = 0;
            if (!Resolve(state, 1, address, error))
                return PushFailure(state, error);
            const auto module = process->ModuleAt(address);
            if (!module)
                return PushFailure(state, ErrorCode::NoModuleAt, Hex(address));
            PushModule(state, *module);
            return 1;
        }

        int RegisterSymbol(lua_State* state)
        {
            Status error;
            const auto process = Opened(state, error);
            if (!process)
                return PushFailure(state, error);
            const std::string name = luaL_checkstring(state, 1);
            std::uint64_t address = 0;
            if (!Resolve(state, 2, address, error))
                return PushFailure(state, error);
            const Status status = Owner(state).Symbols().Register(name, address, "lua");
            if (!status)
                return PushFailure(state, status);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UnregisterSymbol(lua_State* state)
        {
            const std::string name = luaL_checkstring(state, 1);
            if (!Owner(state).Symbols().Unregister(name))
                return PushFailure(state, ErrorCode::ExprUnknownSymbol, name);
            lua_pushboolean(state, 1);
            return 1;
        }

        int StructureHooks(lua_State* state)
        {
            const StructureCallback kind = luaL_checkinteger(state, 1) == 0 ? StructureCallback::NameLookup : StructureCallback::DissectOverride;
            const std::vector<int> references = EngineOf(state).StructureCallbackReferences(kind);
            lua_createtable(state, static_cast<int>(references.size()), 0);
            lua_Integer index = 1;
            for (const int reference : references)
            {
                lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

    }

    void RegisterManifoldApi(LuaEngine& engine, lua_State* state)
    {
        (void)engine;
        const luaL_Reg natives[] = {
            { "fail", &Fail },
            { "logRecord", &LogRecord },
            { "showOutput", &ShowOutput },
            { "clearOutput", &ClearOutput },
            { "showOnPrint", &ShowOnPrint },
            { "setShowOnPrint", &SetShowOnPrint },
            { "uiMode", &UiMode },
            { "setUiMode", &SetUiMode },
            { "version", &Version },
            { "processIsOpen", &ProcessIsOpen },
            { "is64Bit", &Is64Bit },
            { "processInfo", &ProcessInfo },
            { "processOpen", &ProcessOpen },
            { "processFind", &ProcessFind },
            { "processName", &ProcessName },
            { "mainModule", &MainModule },
            { "module", &Module },
            { "modules", &Modules },
            { "address", &Address },
            { "describe", &Describe },
            { "read", &Read },
            { "writeMemory", &WriteMemory },
            { "isReadable", &IsReadable },
            { "scan", &ScanMemory },
            { "moduleAt", &ModuleAt },
            { "registerSymbol", &RegisterSymbol },
            { "unregisterSymbol", &UnregisterSymbol },
            { "structureHooks", &StructureHooks },
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
            lua_createtable(state, 0, static_cast<int>(std::size(natives)) + 10);
            luaL_setfuncs(state, natives, 0);
            RegisterDocNatives(state, -1);
            RegisterUiNatives(state, -1);
            RegisterUnrealNatives(state, -1);
            if (lua_pcall(state, 1, 1, 0) != LUA_OK)
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
            lua_settop(state, top);
        }
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
