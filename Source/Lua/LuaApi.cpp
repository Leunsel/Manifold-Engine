#include "Lua/LuaApi.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Disasm/CheatEngineExtra.h"
#include "Lua/InputBox.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaEngine.h"
#include "Scan/Scanner.h"

#include "lua.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

#include <windows.h>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kEngineKey = "Manifold.Engine";

        std::set<std::string>& Names()
        {
            static std::set<std::string> names;
            return names;
        }

        std::shared_ptr<Target::Process> NeedProcess(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            if (!process)
                luaL_error(state, "%s", ErrorMessage(ErrorCode::NoProcess).c_str());
            return process;
        }

        void PushAddress(lua_State* state, std::uint64_t address)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(address));
        }

        template <class T>
        int ReadValue(lua_State* state, bool signedResult)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushnil(state);
                return 1;
            }
            const std::uint64_t address = AddressArgument(state, 1);
            T value{};
            if (!process->ReadExact(address, &value, sizeof(T)))
            {
                lua_pushnil(state);
                return 1;
            }
            if constexpr (std::is_floating_point_v<T>)
                lua_pushnumber(state, static_cast<lua_Number>(value));
            else if (signedResult)
                lua_pushinteger(state, static_cast<lua_Integer>(static_cast<std::make_signed_t<T>>(value)));
            else
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            return 1;
        }

        template <class T>
        int WriteValue(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            const std::uint64_t address = AddressArgument(state, 1);
            T value{};
            if constexpr (std::is_floating_point_v<T>)
                value = static_cast<T>(luaL_checknumber(state, 2));
            else
                value = static_cast<T>(lua_isinteger(state, 2) ? lua_tointeger(state, 2) : static_cast<lua_Integer>(luaL_checknumber(state, 2)));
            lua_pushboolean(state, process->Write(address, &value, sizeof(T)).Ok());
            return 1;
        }

        int Print(lua_State* state)
        {
            const int count = lua_gettop(state);
            ConsoleEntry entry;
            bool structured = false;
            for (int i = 1; i <= count; ++i)
            {
                std::size_t length = 0;
                const char* text = luaL_tolstring(state, i, &length);
                if (i > 1)
                    entry.Text += '\t';
                entry.Text.append(text, length);
                lua_pop(state, 1);
                structured = structured || lua_type(state, i) == LUA_TTABLE || lua_type(state, i) == LUA_TUSERDATA;
            }
            lua_Debug info{};
            if (lua_getstack(state, 1, &info) && lua_getinfo(state, "Sln", &info))
            {
                entry.Source.Chunk = ChunkLabel(info.source ? info.source : "", info.short_src);
                entry.Source.Function = info.name ? info.name : "";
                entry.Source.Line = info.currentline;
            }
            LuaEngine& engine = EngineOf(state);
            if (structured)
            {
                for (int i = 1; i <= count; ++i)
                    entry.Values.push_back(engine.Snapshot(state, i));
            }
            engine.AddEntry(std::move(entry));
            return 0;
        }

        int ShowMessage(lua_State* state)
        {
            AskMessage("Manifold Engine", TextArgument(state, 1), MessageKind::Custom, { ButtonOk });
            return 0;
        }

        int MessageDialog(lua_State* state)
        {
            const bool titled = lua_type(state, 1) == LUA_TSTRING && lua_type(state, 2) == LUA_TSTRING;
            const int first = titled ? 2 : 1;
            const std::string text = TextArgument(state, first);
            const int type = static_cast<int>(luaL_optinteger(state, first + 1, static_cast<lua_Integer>(MessageKind::Information)));
            const MessageKind kind = type >= 0 && type <= 4 ? static_cast<MessageKind>(type) : MessageKind::Information;
            std::vector<int> buttons;
            for (int i = first + 2; i <= lua_gettop(state); ++i)
            {
                if (lua_istable(state, i))
                {
                    for (lua_Integer item = 1; item <= luaL_len(state, i); ++item)
                    {
                        lua_rawgeti(state, i, item);
                        buttons.push_back(static_cast<int>(lua_tointeger(state, -1)));
                        lua_pop(state, 1);
                    }
                }
                else
                {
                    buttons.push_back(static_cast<int>(luaL_checkinteger(state, i)));
                }
            }
            static const char* kTitles[] = { "Warning", "Error", "Information", "Confirmation", "Manifold Engine" };
            const std::string title = titled ? TextArgument(state, 1) : kTitles[static_cast<int>(kind)];
            lua_pushinteger(state, AskMessage(title, text, kind, std::move(buttons)));
            return 1;
        }

        int ShowSelectionList(lua_State* state)
        {
            const std::string title = luaL_optstring(state, 1, "Manifold Engine");
            const std::string caption = luaL_optstring(state, 2, "");
            std::vector<std::string> items;
            if (lua_istable(state, 3))
            {
                for (lua_Integer item = 1; item <= luaL_len(state, 3); ++item)
                {
                    lua_rawgeti(state, 3, item);
                    items.push_back(TextArgument(state, -1));
                    lua_pop(state, 1);
                }
            }
            const auto [index, text] = AskSelection(title, caption, items, lua_toboolean(state, 4) != 0);
            lua_pushinteger(state, index);
            lua_pushstring(state, text.c_str());
            return 2;
        }

        int InputQuery(lua_State* state)
        {
            const char* caption = luaL_optstring(state, 1, "Manifold Engine");
            const char* prompt = luaL_optstring(state, 2, "");
            const char* initial = luaL_optstring(state, 3, "");
            std::string value = initial;
            if (AskText(caption, prompt, value))
                lua_pushstring(state, value.c_str());
            else
                lua_pushnil(state);
            return 1;
        }

        int SleepFunction(lua_State* state)
        {
            const lua_Integer milliseconds = luaL_checkinteger(state, 1);
            if (lua_isyieldable(state) && EngineOf(state).IsCooperative(state))
            {
                lua_pushinteger(state, milliseconds);
                return lua_yield(state, 1);
            }
            EngineOf(state).BlockingSleep(static_cast<std::uint32_t>(std::clamp<lua_Integer>(milliseconds, 0, 60000)));
            return 0;
        }

        int TickCount(lua_State* state)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(::GetTickCount64()));
            return 1;
        }

        int ProgramDir(lua_State* state)
        {
            const std::string folder = WideToUtf8(Paths::ProgramFolder().wstring()) + "\\";
            lua_pushstring(state, folder.c_str());
            return 1;
        }

        int AutorunPath(lua_State* state)
        {
            const std::string folder = WideToUtf8(Paths::AutorunFolder().wstring()) + "\\";
            lua_pushstring(state, folder.c_str());
            return 1;
        }

        int VersionFunction(lua_State* state)
        {
            lua_pushnumber(state, 7.5);
            return 1;
        }

        int ManifoldVersion(lua_State* state)
        {
            lua_pushstring(state, MANIFOLD_VERSION_TEXT);
            return 1;
        }

        int OpenProcessFunction(lua_State* state)
        {
            const Status status = OpenProcessArgument(Owner(state), state, 1);
            if (!status)
                EngineOf(state).PrintError(status.Text);
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int OpenedProcessId(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            lua_pushinteger(state, process ? Raw(process->Id()) : 0);
            return 1;
        }

        int ProcessIdFromName(lua_State* state)
        {
            const auto id = Target::FindProcessId(luaL_checkstring(state, 1));
            if (id)
                lua_pushinteger(state, Raw(*id));
            else
                lua_pushnil(state);
            return 1;
        }

        int ProcessList(lua_State* state)
        {
            lua_newtable(state);
            for (const Target::ProcessEntry& entry : Target::ListProcesses(false))
            {
                lua_pushstring(state, entry.Name.c_str());
                lua_rawseti(state, -2, Raw(entry.Id));
            }
            return 1;
        }

        int ForegroundProcess(lua_State* state)
        {
            DWORD id = 0;
            ::GetWindowThreadProcessId(::GetForegroundWindow(), &id);
            lua_pushinteger(state, id);
            return 1;
        }

        int TargetIs64Bit(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            lua_pushboolean(state, !process || process->Is64Bit());
            return 1;
        }

        int NameFromAddress(lua_State* state)
        {
            const std::uint64_t address = AddressArgument(state, 1);
            const std::string name = Owner(state).Describe(address);
            lua_pushstring(state, name.c_str());
            return 1;
        }

        int RegisterSymbol(lua_State* state)
        {
            const char* name = luaL_checkstring(state, 1);
            const std::uint64_t address = AddressArgument(state, 2);
            const Status status = Owner(state).Symbols().Register(name, address, "lua");
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int UnregisterSymbol(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).Symbols().Unregister(luaL_checkstring(state, 1)));
            return 1;
        }

        int ModuleSize(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushnil(state);
                return 1;
            }
            const auto module = process->FindModule(luaL_checkstring(state, 1));
            if (module)
                lua_pushinteger(state, static_cast<lua_Integer>(module->Size));
            else
                lua_pushnil(state);
            return 1;
        }

        int EnumModules(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_newtable(state);
                return 1;
            }
            lua_newtable(state);
            int index = 1;
            for (const Target::ModuleInfo& module : *process->Modules())
            {
                lua_newtable(state);
                lua_pushstring(state, module.Name.c_str());
                lua_setfield(state, -2, "Name");
                PushAddress(state, module.Base);
                lua_setfield(state, -2, "Address");
                lua_pushinteger(state, static_cast<lua_Integer>(module.Size));
                lua_setfield(state, -2, "Size");
                lua_pushboolean(state, process->Is64Bit());
                lua_setfield(state, -2, "Is64Bit");
                lua_pushstring(state, WideToUtf8(module.Path).c_str());
                lua_setfield(state, -2, "PathToFile");
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int ReadBytes(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushnil(state);
                return 1;
            }
            const std::uint64_t address = AddressArgument(state, 1);
            const lua_Integer count = luaL_optinteger(state, 2, 1);
            const bool asTable = lua_toboolean(state, 3) != 0;
            if (count <= 0 || count > 16 * 1024 * 1024)
                return luaL_error(state, "readBytes: count out of range");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
            if (!process->ReadExact(address, bytes.data(), bytes.size()))
            {
                lua_pushnil(state);
                return 1;
            }
            if (asTable)
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

        int WriteBytes(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            const std::uint64_t address = AddressArgument(state, 1);
            const std::vector<std::uint8_t> bytes = BytesFrom(state, 2);
            lua_pushboolean(state, process->Write(address, bytes).Ok());
            return 1;
        }

        int ReadString(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushnil(state);
                return 1;
            }
            const std::uint64_t address = AddressArgument(state, 1);
            const lua_Integer maximum = std::clamp<lua_Integer>(luaL_optinteger(state, 2, 1000), 1, 1024 * 1024);
            const bool wide = lua_toboolean(state, 3) != 0;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(maximum) * (wide ? 2 : 1));
            const std::size_t got = process->Read(address, bytes.data(), bytes.size());
            if (got == 0)
            {
                lua_pushnil(state);
                return 1;
            }
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
                lua_pushstring(state, WideToUtf8(text).c_str());
            }
            else
            {
                std::string text;
                for (std::size_t i = 0; i < got && bytes[i]; ++i)
                    text += static_cast<char>(bytes[i]);
                lua_pushlstring(state, text.data(), text.size());
            }
            return 1;
        }

        int WriteString(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            const std::uint64_t address = AddressArgument(state, 1);
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 2, &length);
            const bool wide = lua_toboolean(state, 3) != 0;
            Status status;
            if (wide)
            {
                const std::wstring converted = Utf8ToWide(std::string_view(text, length));
                status = process->Write(address, converted.data(), converted.size() * 2);
            }
            else
            {
                status = process->Write(address, text, length);
            }
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int ReadPointer(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushnil(state);
                return 1;
            }
            const auto value = process->ReadPointer(AddressArgument(state, 1));
            if (value)
                PushAddress(state, *value);
            else
                lua_pushnil(state);
            return 1;
        }

        int WritePointer(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            return process->Is64Bit() ? WriteValue<std::uint64_t>(state) : WriteValue<std::uint32_t>(state);
        }

        int AllocateMemory(lua_State* state)
        {
            const auto process = NeedProcess(state);
            const lua_Integer size = luaL_checkinteger(state, 1);
            const std::uint64_t base = lua_isnoneornil(state, 2) ? 0 : AddressArgument(state, 2);
            const DWORD protect = static_cast<DWORD>(luaL_optinteger(state, 3, PAGE_EXECUTE_READWRITE));
            const Result<Address> memory = process->Allocate(static_cast<std::size_t>(size), protect, base);
            if (!memory)
                return luaL_error(state, "%s", memory.Error().Text.c_str());
            PushAddress(state, *memory);
            return 1;
        }

        int DeAlloc(lua_State* state)
        {
            const auto process = NeedProcess(state);
            lua_pushboolean(state, process->Free(AddressArgument(state, 1)).Ok());
            return 1;
        }

        int FullAccess(lua_State* state)
        {
            const auto process = NeedProcess(state);
            const std::uint64_t address = AddressArgument(state, 1);
            const lua_Integer size = luaL_checkinteger(state, 2);
            lua_pushboolean(state, process->Protect(address, static_cast<std::size_t>(size), PAGE_EXECUTE_READWRITE).has_value());
            return 1;
        }

        int ExecuteCode(lua_State* state)
        {
            const auto process = NeedProcess(state);
            const std::uint64_t address = AddressArgument(state, 1);
            const std::uint64_t parameter = lua_isnoneornil(state, 2) ? 0 : AddressArgument(state, 2);
            const Status status = process->CreateThread(address, parameter, true);
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        int InjectLibrary(lua_State* state)
        {
            const auto process = NeedProcess(state);
            NoteOutsideAction(state, "injectDLL", luaL_checkstring(state, 1));
            const Status status = process->InjectLibrary(Utf8ToWide(luaL_checkstring(state, 1)));
            if (!status)
                EngineOf(state).PrintError(status.Text);
            lua_pushboolean(state, status.Ok());
            return 1;
        }

        constexpr const char* kDisableInfoType = "Manifold.DisableInfo";

        using SharedMemory = std::shared_ptr<Asm::AaMemory>;

        int DisableInfoIndex(lua_State* state)
        {
            const SharedMemory& memory = *static_cast<SharedMemory*>(luaL_checkudata(state, 1, kDisableInfoType));
            const std::string key = lua_type(state, 2) == LUA_TSTRING ? Lower(lua_tostring(state, 2)) : std::string();
            if (key == "allocs")
            {
                lua_newtable(state);
                for (const Asm::AaAllocation& allocation : memory->Allocations)
                {
                    lua_pushinteger(state, static_cast<lua_Integer>(allocation.Address));
                    lua_setfield(state, -2, allocation.Name.c_str());
                }
                return 1;
            }
            if (key == "registeredsymbols")
            {
                lua_newtable(state);
                int index = 1;
                for (const std::string& symbol : memory->Registered)
                {
                    lua_pushstring(state, symbol.c_str());
                    lua_rawseti(state, -2, index++);
                }
                return 1;
            }
            lua_pushnil(state);
            return 1;
        }

        int DisableInfoGc(lua_State* state)
        {
            static_cast<SharedMemory*>(luaL_checkudata(state, 1, kDisableInfoType))->~SharedMemory();
            return 0;
        }

        // ∑ The Auto Assembler step log names a Lua-run script after the record whose script called it
        //   (memrec), since a table framework runs many hook scripts through the same autoAssemble.
        std::string StepLogTitle(lua_State* state, const char* function)
        {
            std::string title = function;
            lua_pushcfunction(state, [](lua_State* s) -> int {
                lua_getglobal(s, "memrec");
                if (lua_type(s, -1) != LUA_TUSERDATA)
                    return 0;
                lua_getfield(s, -1, "Description");
                return 1;
            });
            if (lua_pcall(state, 0, 1, 0) == LUA_OK && lua_type(state, -1) == LUA_TSTRING)
                title += " from \"" + std::string(lua_tostring(state, -1)) + "\"";
            lua_pop(state, 1);
            return title;
        }

        int AutoAssemble(lua_State* state)
        {
            const char* script = luaL_checkstring(state, 1);
            auto* given = static_cast<SharedMemory*>(luaL_testudata(state, 2, kDisableInfoType));
            if (!given && lua_toboolean(state, 2))
            {
                const std::string message = "autoAssemble: assembling into the program itself (targetself) is not supported";
                LOG_WARN("Lua", message);
                lua_pushboolean(state, 0);
                lua_pushstring(state, message.c_str());
                return 2;
            }
            if (!given)
                given = static_cast<SharedMemory*>(luaL_testudata(state, 3, kDisableInfoType));
            const SharedMemory memory = given ? *given : std::make_shared<Asm::AaMemory>();
            const bool sections = Asm::HasSections(script);
            const Asm::AaSection section = !sections ? Asm::AaSection::Whole : given ? Asm::AaSection::Disable : Asm::AaSection::Enable;
            // ∑ The title names the run in the step log and, always, in the crash watch's notes.
            const Asm::AaResult result = Owner(state).AutoAssemble(script, section, *memory, false, "lua", nullptr, StepLogTitle(state, "autoAssemble"));
            if (!result.Ok())
            {
                const std::string message = result.Line > 0 ? "line " + std::to_string(result.Line) + ": " + result.Error.Text : result.Error.Text;
                std::string first = Trim(script);
                first = first.substr(0, std::min<std::size_t>(first.find('\n'), 80));
                // the step log's own entry already says all of this, with the steps before the failure
                if (!result.Logged)
                    LOG_WARN("Lua", "autoAssemble failed (" + std::string(section == Asm::AaSection::Disable ? "disable" : "enable") + "), " + message + " | script starts with: " + first);
                lua_pushboolean(state, 0);
                lua_pushstring(state, message.c_str());
                return 2;
            }
            lua_pushboolean(state, 1);
            if (given)
                return 1;
            void* storage = lua_newuserdatauv(state, sizeof(SharedMemory), 0);
            new (storage) SharedMemory(memory);
            luaL_setmetatable(state, kDisableInfoType);
            return 2;
        }

        int AutoAssembleCheck(lua_State* state)
        {
            const char* script = luaL_checkstring(state, 1);
            const bool enable = lua_isnoneornil(state, 2) || lua_toboolean(state, 2);
            Asm::AaMemory memory;
            const Asm::AaSection section = Asm::HasSections(script) ? (enable ? Asm::AaSection::Enable : Asm::AaSection::Disable) : Asm::AaSection::Whole;
            const bool log = Owner(state).Options().AutoAssemblerLog || Asm::WantsLog(script);
            const Asm::AaResult result = Owner(state).AutoAssemble(script, section, memory, true, "check", nullptr, log ? StepLogTitle(state, "autoAssembleCheck") : std::string());
            lua_pushboolean(state, result.Ok());
            if (!result.Ok())
                lua_pushstring(state, result.Error.Text.c_str());
            else
                lua_pushnil(state);
            return 2;
        }

        Scan::PatternFilter PatternFilterArguments(lua_State* state, int index)
        {
            Scan::PatternFilter filter;
            const std::string flags = luaL_optstring(state, index, "");
            for (std::size_t i = 0; i + 1 < flags.size(); i += 2)
            {
                const char mode = flags[i];
                const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(flags[i + 1])));
                const Scan::Tristate value = mode == '+' ? Scan::Tristate::Yes : mode == '-' ? Scan::Tristate::No : Scan::Tristate::Any;
                if (kind == 'W')
                    filter.Writable = value;
                else if (kind == 'X')
                    filter.Executable = value;
                else if (kind == 'C')
                    filter.CopyOnWrite = value;
            }
            const lua_Integer alignment = luaL_optinteger(state, index + 1, 0);
            const std::string parameter = Trim(luaL_optstring(state, index + 2, ""));
            const std::uint64_t value = parameter.empty() ? 0 : std::strtoull(parameter.c_str(), nullptr, 16);
            if (alignment == 1)
                filter.Divisor = value;
            else if (alignment == 2 && !parameter.empty())
            {
                filter.Modulo = 1ull << std::min<std::size_t>(parameter.size() * 4, 60);
                filter.Remainder = value % filter.Modulo;
            }
            return filter;
        }

        std::vector<std::uint64_t> FindPatternFor(lua_State* state, const std::string& text, std::uint64_t start, std::uint64_t stop, std::size_t limit, int filterIndex = 0)
        {
            const auto process = NeedProcess(state);
            const auto pattern = Scan::ParsePattern(text);
            if (!pattern)
                luaL_error(state, "%s", ErrorMessage(ErrorCode::ScanBadPattern, text).c_str());
            const Scan::PatternFilter filter = filterIndex > 0 ? PatternFilterArguments(state, filterIndex) : Scan::PatternFilter{};
            return Scan::FindPattern(*process, *pattern, start, stop, filter, limit);
        }

        int AobScan(lua_State* state)
        {
            const std::string text = luaL_checkstring(state, 1);
            const auto hits = FindPatternFor(state, text, 0, ~0ull, 100000, 2);
            if (hits.empty())
            {
                lua_pushnil(state);
                return 1;
            }
            lua_newtable(state);
            for (std::size_t i = 0; i < hits.size(); ++i)
            {
                lua_pushstring(state, Hex(hits[i]).c_str());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i));
            }
            lua_pushinteger(state, static_cast<lua_Integer>(hits.size()));
            lua_setfield(state, -2, "Count");
            lua_pushcfunction(state, [](lua_State*) -> int { return 0; });
            lua_setfield(state, -2, "destroy");
            lua_pushcfunction(state, [](lua_State* inner) -> int
                {
                    lua_rawgeti(inner, 1, luaL_checkinteger(inner, 2));
                    return 1;
                });
            lua_setfield(state, -2, "getString");
            return 1;
        }

        int AobScanUnique(lua_State* state)
        {
            const std::string text = luaL_checkstring(state, 1);
            const auto hits = FindPatternFor(state, text, 0, ~0ull, 2, 2);
            if (hits.size() == 1)
                PushAddress(state, hits.front());
            else
                lua_pushnil(state);
            return 1;
        }

        int AobScanModuleUnique(lua_State* state)
        {
            const auto process = Owner(state).Process();
            if (!process)
            {
                lua_pushnil(state);
                return 1;
            }
            const auto module = process->FindModule(luaL_checkstring(state, 1));
            if (!module)
            {
                lua_pushnil(state);
                return 1;
            }
            const auto hits = FindPatternFor(state, luaL_checkstring(state, 2), module->Base, module->Base + module->Size, 2, 3);
            if (hits.size() == 1)
                PushAddress(state, hits.front());
            else
                lua_pushnil(state);
            return 1;
        }

        // ∑ Cheat Engine's Lua disassemble() builds its own disassembler with showmodules, showsymbols and
        //   showsections off (LuaHandler.pas, disassemble_lua): every address in the text is plain hex with at
        //   least 8 digits, "mov rcx,[7FF68B34FF88]", and no " : comment" follows. Tables parse that with
        //   "%[([A-Fa-f0-9]+)%]". Ours wrote the memory view's "mov rcx,[<module>+offset]", so a table found
        //   its AOB and then could not read the static address behind it. The Lua text is now
        //   Cheat Engine's to the character (Disassembler::CheatEngineLine); the memory view keeps its own.
        int Disassemble(lua_State* state)
        {
            const auto process = NeedProcess(state);
            const std::uint64_t address = AddressArgument(state, 1);
            std::uint8_t bytes[16]{};
            const std::size_t got = process->Read(address, bytes, sizeof(bytes));
            lua_pushstring(state, Owner(state).Disassembler().CheatEngineLine(address, bytes, got).c_str());
            return 1;
        }

        std::vector<std::uint8_t> ByteArgument(lua_State* state, int index)
        {
            if (lua_istable(state, index))
                return ByteTable(state, index);
            std::vector<std::uint8_t> bytes;
            const std::string text = luaL_checkstring(state, index);
            int high = -1;
            for (const char c : text)
            {
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                if (digit < 0)
                    continue;
                if (high < 0)
                {
                    high = digit;
                    continue;
                }
                bytes.push_back(static_cast<std::uint8_t>(high * 16 + digit));
                high = -1;
            }
            return bytes;
        }

        int DisassembleBytes(lua_State* state)
        {
            const std::vector<std::uint8_t> bytes = ByteArgument(state, 1);
            const std::uint64_t address = lua_isnoneornil(state, 2) ? 0 : AddressArgument(state, 2);
            lua_pushstring(state, Owner(state).Disassembler().CheatEngineLine(address, bytes.data(), bytes.size()).c_str());
            return 1;
        }

        // ∑ Cheat Engine's fourth value is not the text after " : " (its Lua disassemble() writes none) but a
        //   guess at the value the instruction refers to, read from the process: [12345678], [(float)1.5000],
        //   [(double)...], ["text"], [module+offset], [->module.export] (Disasm::CheatEngineExtra).
        int SplitDisassembled(lua_State* state)
        {
            const std::string text = luaL_checkstring(state, 1);
            const auto process = Owner(state).Process();
            Disasm::CheatEngineMemory memory;
            if (process)
            {
                memory.Is64Bit = process->Is64Bit();
                memory.Read = [&process](std::uint64_t address, void* buffer, std::size_t size)
                {
                    return size <= ~address && process->ReadExact(address, buffer, size);
                };
                memory.ReadPrefix = [&process](std::uint64_t address, void* buffer, std::size_t size) -> std::size_t
                {
                    if (size > ~address)
                        return 0;
                    if (process->ReadExact(address, buffer, size))
                        return size;
                    const auto region = process->RegionAt(address);
                    if (!region || !region->Readable() || region->End() <= address)
                        return 0;
                    const std::size_t part = static_cast<std::size_t>(std::min<std::uint64_t>(size, region->End() - address));
                    return process->ReadExact(address, buffer, part) ? part : 0;
                };
                memory.Committed = [&process](std::uint64_t address)
                {
                    const auto region = process->RegionAt(address);
                    return region && region->Committed();
                };
                memory.Name = [state](std::uint64_t address) { return Owner(state).Describe(address); };
            }
            const Disasm::CheatEngineSplit split = Disasm::SplitCheatEngineLine(text, process ? &memory : nullptr);
            lua_pushstring(state, split.Extra.c_str());
            lua_pushstring(state, split.Opcode.c_str());
            lua_pushstring(state, split.Bytes.c_str());
            lua_pushstring(state, split.Address.c_str());
            return 4;
        }

        int InstructionSize(lua_State* state)
        {
            const auto process = NeedProcess(state);
            const Disasm::Instruction instruction = Owner(state).Disassembler().Read(*process, AddressArgument(state, 1), nullptr);
            lua_pushinteger(state, instruction.Length);
            return 1;
        }

        int PreviousOpcode(lua_State* state)
        {
            const auto process = NeedProcess(state);
            const std::uint64_t address = AddressArgument(state, 1);
            const auto previous = Owner(state).Disassembler().PreviousStart(*process, address);
            PushAddress(state, previous.value_or(address - 1));
            return 1;
        }

        int IsKeyPressed(lua_State* state)
        {
            lua_pushboolean(state, (::GetAsyncKeyState(static_cast<int>(luaL_checkinteger(state, 1))) & 0x8000) != 0);
            return 1;
        }

        int ShowMemoryViewer(lua_State* state)
        {
            Owner(state).Request({ UiRequestKind::MemoryViewer, lua_isnoneornil(state, 1) ? 0 : AddressArgument(state, 1), {} });
            return 0;
        }

        int ShowStructureDissect(lua_State* state)
        {
            Owner(state).Request({ UiRequestKind::StructureDissect, lua_isnoneornil(state, 1) ? 0 : AddressArgument(state, 1), {} });
            return 0;
        }

        int InMainThread(lua_State* state)
        {
            lua_pushboolean(state, 1);
            return 1;
        }

        int Synchronize(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TFUNCTION);
            const int arguments = lua_gettop(state) - 1;
            lua_call(state, arguments, LUA_MULTRET);
            return lua_gettop(state);
        }

        template <class T>
        int ToByteTable(lua_State* state)
        {
            T value{};
            if constexpr (std::is_floating_point_v<T>)
                value = static_cast<T>(luaL_checknumber(state, 1));
            else
                value = static_cast<T>(luaL_checkinteger(state, 1));
            std::uint8_t bytes[sizeof(T)];
            std::memcpy(bytes, &value, sizeof(T));
            lua_createtable(state, sizeof(T), 0);
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                lua_pushinteger(state, bytes[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        template <class T>
        int FromByteTable(lua_State* state)
        {
            luaL_checktype(state, 1, LUA_TTABLE);
            std::uint8_t bytes[sizeof(T)]{};
            for (std::size_t i = 0; i < sizeof(T); ++i)
            {
                lua_rawgeti(state, 1, static_cast<lua_Integer>(i + 1));
                bytes[i] = static_cast<std::uint8_t>(lua_tointeger(state, -1));
                lua_pop(state, 1);
            }
            T value{};
            std::memcpy(&value, bytes, sizeof(T));
            if constexpr (std::is_floating_point_v<T>)
                lua_pushnumber(state, static_cast<lua_Number>(value));
            else
                lua_pushinteger(state, static_cast<lua_Integer>(value));
            return 1;
        }

        int StringToByteTable(lua_State* state)
        {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, 1, &length);
            lua_createtable(state, static_cast<int>(length), 0);
            for (std::size_t i = 0; i < length; ++i)
            {
                lua_pushinteger(state, static_cast<unsigned char>(text[i]));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int WideStringToByteTable(lua_State* state)
        {
            const std::wstring text = Utf8ToWide(luaL_checkstring(state, 1));
            lua_createtable(state, static_cast<int>(text.size() * 2), 0);
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                lua_pushinteger(state, text[i] & 0xFF);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i * 2 + 1));
                lua_pushinteger(state, (text[i] >> 8) & 0xFF);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i * 2 + 2));
            }
            return 1;
        }

        int ByteTableToString(lua_State* state)
        {
            const std::vector<std::uint8_t> bytes = BytesFrom(state, 1);
            std::string text(bytes.begin(), bytes.end());
            const std::size_t end = text.find('\0');
            if (end != std::string::npos)
                text.resize(end);
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int ByteTableToWideString(lua_State* state)
        {
            const std::vector<std::uint8_t> bytes = BytesFrom(state, 1);
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

        int BitAnd(lua_State* state)
        {
            lua_pushinteger(state, luaL_checkinteger(state, 1) & luaL_checkinteger(state, 2));
            return 1;
        }

        int BitOr(lua_State* state)
        {
            lua_pushinteger(state, luaL_checkinteger(state, 1) | luaL_checkinteger(state, 2));
            return 1;
        }

        int BitXor(lua_State* state)
        {
            lua_pushinteger(state, luaL_checkinteger(state, 1) ^ luaL_checkinteger(state, 2));
            return 1;
        }

        int BitNot(lua_State* state)
        {
            lua_pushinteger(state, ~luaL_checkinteger(state, 1));
            return 1;
        }

        int BitShl(lua_State* state)
        {
            const lua_Integer count = luaL_checkinteger(state, 2);
            lua_pushinteger(state, count < 0 || count >= 64 ? 0 : static_cast<lua_Integer>(static_cast<std::uint64_t>(luaL_checkinteger(state, 1)) << count));
            return 1;
        }

        int BitShr(lua_State* state)
        {
            const lua_Integer count = luaL_checkinteger(state, 2);
            lua_pushinteger(state, count < 0 || count >= 64 ? 0 : static_cast<lua_Integer>(static_cast<std::uint64_t>(luaL_checkinteger(state, 1)) >> count));
            return 1;
        }

        int ToHex(lua_State* state)
        {
            const lua_Integer value = luaL_checkinteger(state, 1);
            const int digits = static_cast<int>(luaL_optinteger(state, 2, 0));
            lua_pushstring(state, Hex(static_cast<std::uint64_t>(value), digits).c_str());
            return 1;
        }

        struct Function
        {
            const char* Name;
            lua_CFunction Call;
        };

        const Function kFunctions[] = {
            { "print", &Print },
            { "showMessage", &ShowMessage },
            { "messageDialog", &MessageDialog },
            { "inputQuery", &InputQuery },
            { "__manifoldShowSelectionList", &ShowSelectionList },
            { "sleep", &SleepFunction },
            { "getTickCount", &TickCount },
            { "getCheatEngineDir", &ProgramDir },
            { "getManifoldDir", &ProgramDir },
            { "getAutorunPath", &AutorunPath },
            { "getCEVersion", &VersionFunction },
            { "getManifoldVersion", &ManifoldVersion },
            { "openProcess", &OpenProcessFunction },
            { "getOpenedProcessID", &OpenedProcessId },
            { "getProcessIDFromProcessName", &ProcessIdFromName },
            { "getProcessList", &ProcessList },
            { "getForegroundProcess", &ForegroundProcess },
            { "targetIs64Bit", &TargetIs64Bit },
            { "getNameFromAddress", &NameFromAddress },
            { "registerSymbol", &RegisterSymbol },
            { "unregisterSymbol", &UnregisterSymbol },
            { "getModuleSize", &ModuleSize },
            { "enumModules", &EnumModules },
            { "readBytes", &ReadBytes },
            { "writeBytes", &WriteBytes },
            { "readByte", [](lua_State* s) { return ReadValue<std::uint8_t>(s, false); } },
            { "readSmallInteger", [](lua_State* s) { return ReadValue<std::uint16_t>(s, lua_toboolean(s, 2) != 0); } },
            { "readInteger", [](lua_State* s) { return ReadValue<std::uint32_t>(s, lua_toboolean(s, 2) != 0); } },
            { "readQword", [](lua_State* s) { return ReadValue<std::uint64_t>(s, true); } },
            { "readFloat", [](lua_State* s) { return ReadValue<float>(s, true); } },
            { "readDouble", [](lua_State* s) { return ReadValue<double>(s, true); } },
            { "readPointer", &ReadPointer },
            { "readString", &ReadString },
            { "writeByte", [](lua_State* s) { return WriteValue<std::uint8_t>(s); } },
            { "writeSmallInteger", [](lua_State* s) { return WriteValue<std::uint16_t>(s); } },
            { "writeInteger", [](lua_State* s) { return WriteValue<std::uint32_t>(s); } },
            { "writeQword", [](lua_State* s) { return WriteValue<std::uint64_t>(s); } },
            { "writeFloat", [](lua_State* s) { return WriteValue<float>(s); } },
            { "writeDouble", [](lua_State* s) { return WriteValue<double>(s); } },
            { "writePointer", &WritePointer },
            { "writeString", &WriteString },
            { "allocateMemory", &AllocateMemory },
            { "deAlloc", &DeAlloc },
            { "fullAccess", &FullAccess },
            { "executeCode", &ExecuteCode },
            { "injectDLL", &InjectLibrary },
            { "injectLibrary", &InjectLibrary },
            { "autoAssemble", &AutoAssemble },
            { "autoAssembleCheck", &AutoAssembleCheck },
            { "AOBScan", &AobScan },
            { "AOBScanUnique", &AobScanUnique },
            { "AOBScanModuleUnique", &AobScanModuleUnique },
            { "disassemble", &Disassemble },
            { "disassembleBytes", &DisassembleBytes },
            { "splitDisassembledString", &SplitDisassembled },
            { "getInstructionSize", &InstructionSize },
            { "getPreviousOpcode", &PreviousOpcode },
            { "isKeyPressed", &IsKeyPressed },
            { "showMemoryViewer", &ShowMemoryViewer },
            { "showStructureDissect", &ShowStructureDissect },
            { "inMainThread", &InMainThread },
            { "synchronize", &Synchronize },
            { "queue", &Synchronize },
            { "byteTableToString", &ByteTableToString },
            { "byteTableToWideString", &ByteTableToWideString },
            { "stringToByteTable", &StringToByteTable },
            { "wideStringToByteTable", &WideStringToByteTable },
            { "wordToByteTable", &ToByteTable<std::uint16_t> },
            { "dwordToByteTable", &ToByteTable<std::uint32_t> },
            { "qwordToByteTable", &ToByteTable<std::uint64_t> },
            { "floatToByteTable", &ToByteTable<float> },
            { "doubleToByteTable", &ToByteTable<double> },
            { "byteTableToWord", &FromByteTable<std::uint16_t> },
            { "byteTableToDword", &FromByteTable<std::uint32_t> },
            { "byteTableToQword", &FromByteTable<std::uint64_t> },
            { "byteTableToFloat", &FromByteTable<float> },
            { "byteTableToDouble", &FromByteTable<double> },
            { "bAnd", &BitAnd },
            { "bOr", &BitOr },
            { "bXor", &BitXor },
            { "bNot", &BitNot },
            { "bShl", &BitShl },
            { "bShr", &BitShr },
            { "intToHex", &ToHex },
        };

        struct Constant
        {
            const char* Name;
            lua_Integer Value;
        };

        const Constant kConstants[] = {
            { "mrOk", 1 }, { "co_runtill", 3 },
        };
    }

    LuaEngine& EngineOf(lua_State* state)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kEngineKey);
        auto* engine = static_cast<LuaEngine*>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        return *engine;
    }

    std::optional<std::int64_t> ExactInteger(lua_State* state, int index)
    {
        if (lua_type(state, index) != LUA_TNUMBER)
            return std::nullopt;
        int exact = 0;
        const lua_Integer value = lua_tointegerx(state, index, &exact);
        if (!exact)
            return std::nullopt;
        return static_cast<std::int64_t>(value);
    }

    Status OpenProcessArgument(Engine& engine, lua_State* state, int index)
    {
        if (lua_type(state, index) != LUA_TNUMBER)
            return engine.OpenProcessByName(luaL_checkstring(state, index));
        const auto id = ExactInteger(state, index);
        if (!id || *id < 0 || *id > 0xFFFFFFFFll)
        {
            std::string text = luaL_tolstring(state, index, nullptr);
            lua_pop(state, 1);
            return Status::Fail(ErrorCode::ProcessNotFound, text);
        }
        return engine.OpenProcess(MakeProcessId(static_cast<std::uint32_t>(*id)));
    }

    void NoteFunction(const char* name)
    {
        Names().insert(name);
    }

    std::vector<std::string> ApiFunctionNames()
    {
        return { Names().begin(), Names().end() };
    }

    void RegisterApi(LuaEngine& engine, lua_State* state)
    {
        for (const Function& function : kFunctions)
        {
            lua_pushcfunction(state, function.Call);
            lua_setglobal(state, function.Name);
            if (function.Name[0] != '_')
                NoteFunction(function.Name);
        }
        for (const Constant& constant : kConstants)
        {
            lua_pushinteger(state, constant.Value);
            lua_setglobal(state, constant.Name);
        }
        for (int key = 0; key < 24; ++key)
        {
            lua_pushinteger(state, VK_F1 + key);
            lua_setglobal(state, ("VK_F" + std::to_string(key + 1)).c_str());
        }
        for (int key = 0; key < 10; ++key)
        {
            lua_pushinteger(state, '0' + key);
            lua_setglobal(state, ("VK_" + std::to_string(key)).c_str());
            lua_pushinteger(state, VK_NUMPAD0 + key);
            lua_setglobal(state, ("VK_NUMPAD" + std::to_string(key)).c_str());
        }
        for (char letter = 'A'; letter <= 'Z'; ++letter)
        {
            lua_pushinteger(state, letter);
            lua_setglobal(state, (std::string("VK_") + letter).c_str());
        }
        luaL_newmetatable(state, kDisableInfoType);
        lua_pushcfunction(state, &DisableInfoIndex);
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, &DisableInfoGc);
        lua_setfield(state, -2, "__gc");
        lua_pop(state, 1);
        RegisterObjects(engine, state);
    }
}
