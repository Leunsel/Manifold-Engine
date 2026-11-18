#include "Lua/LuaEngine.h"

#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaDocBinding.h"
#include "Table/Structures.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include <windows.h>

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kEngineKey = "Manifold.Engine";
        constexpr int kCallHookInstructions = 10000;

        double CallClock()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        int Traceback(lua_State* state)
        {
            const char* message = lua_tostring(state, 1);
            if (!message)
            {
                if (luaL_callmeta(state, 1, "__tostring") && lua_type(state, -1) == LUA_TSTRING)
                    message = lua_tostring(state, -1);
                else
                    message = lua_pushfstring(state, "(error object is a %s value)", luaL_typename(state, 1));
            }
            luaL_traceback(state, state, message, 1);
            return 1;
        }

        std::string FirstLine(const std::string& text)
        {
            const std::size_t end = text.find('\n');
            return end == std::string::npos ? text : text.substr(0, end);
        }

        std::vector<std::string> GlobalFunctionNames(lua_State* state)
        {
            std::vector<std::string> names;
            lua_pushglobaltable(state);
            lua_pushnil(state);
            while (lua_next(state, -2))
            {
                if (lua_type(state, -2) == LUA_TSTRING && lua_isfunction(state, -1))
                    names.emplace_back(lua_tostring(state, -2));
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
            return names;
        }

        void AddCapitalAliases(lua_State* state, const std::vector<std::string>& standard)
        {
            lua_pushglobaltable(state);
            for (const std::string& name : GlobalFunctionNames(state))
            {
                if (name[0] < 'a' || name[0] > 'z' || std::find(standard.begin(), standard.end(), name) != standard.end())
                    continue;
                std::string alias = name;
                alias[0] = static_cast<char>(alias[0] - 'a' + 'A');
                lua_pushstring(state, alias.c_str());
                const bool taken = lua_rawget(state, -2) != LUA_TNIL;
                lua_pop(state, 1);
                if (taken)
                    continue;
                lua_pushstring(state, alias.c_str());
                lua_pushstring(state, name.c_str());
                lua_rawget(state, -3);
                lua_rawset(state, -3);
            }
            lua_pop(state, 1);
        }
    }

    int CustomType::ByteCount() const
    {
        std::int32_t count = 0;
        std::memcpy(&count, Object->Raw.data() + kCustomTypeByteSizeOffset, sizeof(count));
        return std::clamp<int>(count, 0, kCustomTypeMaxBytes);
    }

    void CustomType::SetByteCount(int count)
    {
        const std::int32_t value = std::clamp(count, 0, kCustomTypeMaxBytes);
        std::memcpy(Object->Raw.data() + kCustomTypeByteSizeOffset, &value, sizeof(value));
    }

    LuaEngine::LuaEngine(Engine& engine) : m_engine(engine)
    {
        Open();
        m_engine.Symbols().SetLookupCallback([this](std::string_view name, bool first) -> std::optional<std::uint64_t>
            {
                if (std::this_thread::get_id() != m_thread && Engine::OnActivationWorker())
                {
                    std::optional<std::uint64_t> result;
                    const std::string copy(name);
                    m_engine.RunOnMainAndWait([&]() { result = LookupSymbol(copy, first); });
                    return result;
                }
                return LookupSymbol(name, first);
            });
    }

    LuaEngine::~LuaEngine()
    {
        m_engine.Symbols().SetLookupCallback({});
        if (m_state)
            lua_close(m_state);
    }

    void LuaEngine::Open()
    {
        m_docs = std::make_shared<DocRegistry>(BuiltinDocs());
        m_state = luaL_newstate();
        luaL_openlibs(m_state);
        const std::vector<std::string> standard = GlobalFunctionNames(m_state);
        lua_pushlightuserdata(m_state, this);
        lua_setfield(m_state, LUA_REGISTRYINDEX, kEngineKey);
        Register();
        AddCapitalAliases(m_state, standard);
        lua_sethook(m_state, &LuaEngine::CallHook, LUA_MASKCOUNT, kCallHookInstructions);
    }

    void LuaEngine::CallHook(lua_State* state, lua_Debug*)
    {
        lua_getfield(state, LUA_REGISTRYINDEX, kEngineKey);
        auto* engine = static_cast<LuaEngine*>(lua_touserdata(state, -1));
        lua_pop(state, 1);
        if (!engine || engine->m_callDeadline <= 0.0)
        {
            lua_sethook(state, &LuaEngine::CallHook, LUA_MASKCOUNT, kCallHookInstructions);
            return;
        }
        if (CallClock() < engine->m_callDeadline)
            return;
        lua_sethook(state, &LuaEngine::CallHook, LUA_MASKCOUNT, 1);
        luaL_error(state, "stopped after %d ms: a callback has to return quickly, so long work belongs in createThread (Settings → Scripts sets the limit)", engine->m_callLimit);
    }

    bool LuaEngine::GuardedCall(int arguments, int results, int handler)
    {
        const double previous = m_callDeadline;
        const int limit = m_engine.Options().ScriptCallbackLimitMs;
        if (limit > 0 && previous <= 0.0)
        {
            m_callLimit = limit;
            m_callDeadline = CallClock() + limit / 1000.0;
        }
        const int status = lua_pcall(m_state, arguments, results, handler);
        m_callDeadline = previous;
        if (previous <= 0.0)
            lua_sethook(m_state, &LuaEngine::CallHook, LUA_MASKCOUNT, kCallHookInstructions);
        return status == LUA_OK;
    }

    void LuaEngine::Register()
    {
        RegisterApi(*this, m_state);
        RegisterMonoApi(*this, m_state);
        RegisterCompat(*this, m_state);
        RegisterManifoldApi(*this, m_state);
        RunCompatScripts(*this);
    }

    std::uint64_t LuaEngine::AddEntry(ConsoleEntry entry)
    {
        if (entry.TimeMs == 0)
            entry.TimeMs = ConsoleClock();
        const bool show = ShowOnPrint() && entry.Kind != EntryKind::Input;
        const std::uint64_t id = m_console.Add(std::move(entry));
        if (show)
            ++m_printSignal;
        return id;
    }

    void LuaEngine::PrintText(std::string text)
    {
        ConsoleEntry entry;
        entry.Kind = EntryKind::System;
        entry.Text = std::move(text);
        AddEntry(std::move(entry));
    }

    void LuaEngine::PrintError(const std::string& message, std::uint64_t inputId)
    {
        ConsoleEntry entry;
        entry.Kind = EntryKind::Error;
        entry.Level = LogLevel::Error;
        entry.InputId = inputId;
        std::string text;
        SplitTraceback(message, text, entry.Traceback);
        std::string_view rest;
        entry.Source = ParseSource(text, &rest);
        entry.Text = std::string(rest);
        const std::size_t code = entry.Text.rfind(" (ME-");
        if (code != std::string::npos && !entry.Text.empty() && entry.Text.back() == ')')
        {
            entry.Code = entry.Text.substr(code + 2, entry.Text.size() - code - 3);
            entry.Text.resize(code);
        }
        AddEntry(std::move(entry));
    }

    bool LuaEngine::ShowOnPrint() const
    {
        const int chosen = m_showOnPrintOverride.load();
        return chosen < 0 ? m_engine.Options().LuaShowOnPrint : chosen != 0;
    }

    void LuaEngine::RememberShowOnPrint(bool show)
    {
        m_showOnPrintOverride = -1;
        m_engine.Options().LuaShowOnPrint = show;
    }

    Status LuaEngine::RunWithResult(std::string_view code, std::string_view chunkName, std::string& result)
    {
        result.clear();
        const int top = lua_gettop(m_state);
        lua_pushcfunction(m_state, &Traceback);
        const std::string name = !chunkName.empty() && (chunkName.front() == '@' || chunkName.front() == '=') ? std::string(chunkName) : "=" + std::string(chunkName);
        if (luaL_loadbuffer(m_state, code.data(), code.size(), name.c_str()) != LUA_OK)
        {
            const std::string message = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "unknown error";
            lua_settop(m_state, top);
            PrintError(message);
            LOG_ERROR("Lua", ErrorMessage(ErrorCode::LuaSyntax, message));
            return Status::Fail(ErrorCode::LuaSyntax, message);
        }
        if (lua_pcall(m_state, 0, 1, top + 1) != LUA_OK)
        {
            const std::string message = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "unknown error";
            lua_settop(m_state, top);
            PrintError(message);
            LOG_ERROR("Lua", ErrorMessage(ErrorCode::LuaRuntime, FirstLine(message)));
            return Status::Fail(ErrorCode::LuaRuntime, FirstLine(message));
        }
        if (lua_type(m_state, -1) == LUA_TSTRING)
            result = lua_tostring(m_state, -1);
        lua_settop(m_state, top);
        return Status::Success();
    }

    Status LuaEngine::Run(std::string_view code, std::string_view chunkName)
    {
        std::string ignored;
        return RunWithResult(code, chunkName, ignored);
    }

    Status LuaEngine::RunFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return Status::Fail(ErrorCode::LuaAutorunFailed, WideToUtf8(path.wstring()));
        std::stringstream content;
        content << file.rdbuf();
        std::string text = content.str();
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
            text.erase(0, 3);
        std::error_code error;
        const std::filesystem::path full = std::filesystem::absolute(path, error);
        return Run(text, "@" + WideToUtf8((error ? path : full).wstring()));
    }

    Status LuaEngine::RunAutorun(const std::filesystem::path& folder)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(folder, error))
        {
            std::filesystem::create_directories(folder, error);
            return Status::Success();
        }
        std::vector<std::filesystem::path> scripts;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (entry.is_regular_file() && EqualsNoCase(WideToUtf8(entry.path().extension().wstring()), ".lua"))
                scripts.push_back(entry.path());
        }
        std::sort(scripts.begin(), scripts.end());
        Status first;
        for (const auto& script : scripts)
        {
            const Status status = RunFile(script);
            if (!status)
            {
                const Status failed = Status::Fail(ErrorCode::LuaAutorunFailed, WideToUtf8(script.filename().wstring()) + ": " + status.Text);
                LOG_ERROR("Autorun", failed.Text);
                if (first.Ok())
                    first = failed;
            }
            else
            {
                LOG_INFO("Autorun", "Ran " + WideToUtf8(script.filename().wstring()));
            }
        }
        return first;
    }

    bool LuaEngine::CallReference(int reference, int arguments, int results, std::string& error)
    {
        const int base = lua_gettop(m_state) - arguments;
        lua_pushcfunction(m_state, &Traceback);
        lua_insert(m_state, base + 1);
        lua_rawgeti(m_state, LUA_REGISTRYINDEX, reference);
        lua_insert(m_state, base + 2);
        if (!GuardedCall(arguments, results, base + 1))
        {
            error = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "unknown error";
            lua_settop(m_state, base);
            PrintError(error);
            return false;
        }
        lua_remove(m_state, base + 1);
        return true;
    }

    bool LuaEngine::CallStack(int arguments, int results, std::string& error)
    {
        const int base = lua_gettop(m_state) - arguments - 1;
        lua_pushcfunction(m_state, &Traceback);
        lua_insert(m_state, base + 1);
        if (!GuardedCall(arguments, results, base + 1))
        {
            error = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "unknown error";
            lua_settop(m_state, base);
            PrintError(error);
            return false;
        }
        lua_remove(m_state, base + 1);
        return true;
    }

    void LuaEngine::SetAutoAssemblerCommand(const std::string& name, int reference)
    {
        const std::string key = Lower(Trim(name));
        const auto found = m_autoAssemblerCommands.find(key);
        if (found != m_autoAssemblerCommands.end())
        {
            Release(found->second);
            m_autoAssemblerCommands.erase(found);
        }
        if (reference > 0 && !key.empty())
            m_autoAssemblerCommands[key] = reference;
    }

    bool LuaEngine::IsAutoAssemblerCommand(std::string_view name) const
    {
        return m_autoAssemblerCommands.count(Lower(name)) > 0;
    }

    Status LuaEngine::RunAutoAssemblerCommand(std::string_view name, std::string_view parameters, bool syntaxCheck, std::string& output)
    {
        output.clear();
        const auto found = m_autoAssemblerCommands.find(Lower(name));
        if (found == m_autoAssemblerCommands.end())
            return Status::Fail(ErrorCode::AaUnknownCommand, name);
        const int top = lua_gettop(m_state);
        lua_pushlstring(m_state, parameters.data(), parameters.size());
        lua_pushboolean(m_state, syntaxCheck);
        std::string error;
        if (!CallReference(found->second, 2, 2, error))
        {
            lua_settop(m_state, top);
            return Status::Fail(ErrorCode::AaLuaFailed, std::string(name) + ": " + FirstLine(error));
        }
        Status status;
        if (lua_type(m_state, -2) == LUA_TSTRING)
            output = lua_tostring(m_state, -2);
        else if (lua_isnil(m_state, -2) && lua_type(m_state, -1) == LUA_TSTRING)
            status = Status::Fail(ErrorCode::AaLuaFailed, std::string(name) + ": " + lua_tostring(m_state, -1));
        lua_settop(m_state, top);
        return status;
    }

    void LuaEngine::SetScriptGlobals(const Table::MemoryRecord* record, bool syntaxCheck)
    {
        lua_pushboolean(m_state, syntaxCheck);
        lua_setglobal(m_state, "syntaxcheck");
        if (!record)
            return;
        PushRecord(m_state, record);
        lua_setglobal(m_state, "memrec");
    }

    ScriptGlobals LuaEngine::SaveScriptGlobals()
    {
        ScriptGlobals saved;
        lua_getglobal(m_state, "syntaxcheck");
        saved.SyntaxCheck = luaL_ref(m_state, LUA_REGISTRYINDEX);
        lua_getglobal(m_state, "memrec");
        saved.Record = luaL_ref(m_state, LUA_REGISTRYINDEX);
        return saved;
    }

    void LuaEngine::RestoreScriptGlobals(const ScriptGlobals& saved)
    {
        const auto restore = [&](int reference, const char* name)
            {
                if (reference == LUA_REFNIL)
                    lua_pushnil(m_state);
                else
                    lua_rawgeti(m_state, LUA_REGISTRYINDEX, reference);
                lua_setglobal(m_state, name);
                luaL_unref(m_state, LUA_REGISTRYINDEX, reference);
            };
        restore(saved.SyntaxCheck, "syntaxcheck");
        restore(saved.Record, "memrec");
    }

    std::vector<int> LuaEngine::RecordsWithEvent(std::string_view name)
    {
        return RecordsWithFunction(m_state, name);
    }

    bool LuaEngine::HasRecordEvent(int id, std::string_view name)
    {
        if (!PushExtra(m_state, id, name))
            return false;
        const bool callable = lua_isfunction(m_state, -1);
        lua_pop(m_state, 1);
        return callable;
    }

    bool LuaEngine::BeforeActivation(Table::MemoryRecord& record, bool active)
    {
        std::string error;
        lua_getglobal(m_state, "onMemRecPreExecute");
        if (lua_isfunction(m_state, -1))
        {
            PushRecord(m_state, &record);
            lua_pushboolean(m_state, active);
            if (!CallStack(2, 0, error))
                m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
        }
        else
        {
            lua_pop(m_state, 1);
        }
        if (!PushExtra(m_state, record.Id, active ? "onactivate" : "ondeactivate"))
            return true;
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return true;
        }
        PushRecord(m_state, &record);
        lua_pushboolean(m_state, 1);
        lua_pushboolean(m_state, record.Active);
        if (!CallStack(3, 1, error))
        {
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
            return false;
        }
        const bool proceed = lua_toboolean(m_state, -1) != 0;
        lua_pop(m_state, 1);
        return proceed;
    }

    void LuaEngine::AfterActivation(Table::MemoryRecord& record, bool active, bool succeeded)
    {
        std::string error;
        if (succeeded && PushExtra(m_state, record.Id, active ? "onactivate" : "ondeactivate"))
        {
            if (lua_isfunction(m_state, -1))
            {
                PushRecord(m_state, &record);
                lua_pushboolean(m_state, 0);
                lua_pushboolean(m_state, record.Active);
                if (!CallStack(3, 0, error))
                    m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
            }
            else
            {
                lua_pop(m_state, 1);
            }
        }
        lua_getglobal(m_state, "onMemRecPostExecute");
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return;
        }
        PushRecord(m_state, &record);
        lua_pushboolean(m_state, active);
        lua_pushboolean(m_state, succeeded);
        if (!CallStack(3, 0, error))
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
    }

    bool LuaEngine::DisplayValue(Table::MemoryRecord& record, std::string& text)
    {
        if (!PushExtra(m_state, record.Id, "ongetdisplayvalue"))
            return false;
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return false;
        }
        PushRecord(m_state, &record);
        lua_pushstring(m_state, text.c_str());
        std::string error;
        if (!CallStack(2, 2, error))
            return false;
        const bool replaced = lua_toboolean(m_state, -2) != 0 && lua_type(m_state, -1) == LUA_TSTRING;
        if (replaced)
            text = lua_tostring(m_state, -1);
        lua_pop(m_state, 2);
        return replaced;
    }

    void LuaEngine::ValueChanged(Table::MemoryRecord& record, const std::string& before, const std::string& after)
    {
        if (!PushExtra(m_state, record.Id, "onvaluechanged"))
            return;
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return;
        }
        PushRecord(m_state, &record);
        lua_pushstring(m_state, before.c_str());
        lua_pushstring(m_state, after.c_str());
        std::string error;
        if (!CallStack(3, 0, error))
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
    }

    bool LuaEngine::AddressListEvent(std::string_view name, Table::MemoryRecord* record)
    {
        if (!PushExtra(m_state, kAddressListExtras, name))
            return false;
        if (!lua_isfunction(m_state, -1))
        {
            lua_pop(m_state, 1);
            return false;
        }
        PushAddressList(m_state);
        PushRecord(m_state, record);
        std::string error;
        if (!CallStack(2, 1, error))
        {
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
            return false;
        }
        const bool handled = lua_toboolean(m_state, -1) != 0;
        lua_pop(m_state, 1);
        return handled;
    }

    void LuaEngine::RecordDestroyed(int id)
    {
        if (PushExtra(m_state, id, "ondestroy"))
        {
            if (lua_isfunction(m_state, -1))
            {
                lua_pushnil(m_state);
                std::string error;
                CallStack(1, 0, error);
            }
            else
            {
                lua_pop(m_state, 1);
            }
        }
        DropExtras(m_state, id);
    }

    void LuaEngine::RegisterCustomType(CustomType type)
    {
        const std::string key = Lower(type.Name);
        const auto found = m_customTypes.find(key);
        if (found != m_customTypes.end())
        {
            Release(found->second.ToValue);
            Release(found->second.ToBytes);
            const int count = type.ByteCount();
            type.Object = found->second.Object;
            type.SetByteCount(count);
        }
        m_customTypes[key] = std::move(type);
    }

    const CustomType* LuaEngine::FindCustomType(std::string_view name) const
    {
        const auto found = m_customTypes.find(Lower(name));
        return found == m_customTypes.end() ? nullptr : &found->second;
    }

    CustomType* LuaEngine::FindCustomType(std::string_view name)
    {
        const auto found = m_customTypes.find(Lower(name));
        return found == m_customTypes.end() ? nullptr : &found->second;
    }

    std::vector<std::string> LuaEngine::CustomTypeNames() const
    {
        std::vector<std::string> names;
        for (const auto& [key, type] : m_customTypes)
            names.push_back(type.Name);
        return names;
    }

    bool LuaEngine::CustomToNumber(const CustomType& type, std::span<const std::uint8_t> bytes, std::uint64_t address, double& value)
    {
        if (type.ToValue <= 0 || static_cast<int>(bytes.size()) < type.ByteCount())
            return false;
        const int top = lua_gettop(m_state);
        luaL_checkstack(m_state, type.ByteCount() + 4, "custom type");
        for (int i = 0; i < type.ByteCount(); ++i)
            lua_pushinteger(m_state, bytes[static_cast<std::size_t>(i)]);
        lua_pushinteger(m_state, static_cast<lua_Integer>(address));
        std::string error;
        if (!CallReference(type.ToValue, type.ByteCount() + 1, 1, error))
        {
            lua_settop(m_state, top);
            return false;
        }
        const bool ok = lua_type(m_state, -1) == LUA_TNUMBER;
        if (ok)
            value = lua_tonumber(m_state, -1);
        lua_settop(m_state, top);
        return ok;
    }

    bool LuaEngine::CustomToText(const CustomType& type, std::span<const std::uint8_t> bytes, std::uint64_t address, std::string& text)
    {
        if (type.String)
        {
            if (type.ToValue <= 0 || static_cast<int>(bytes.size()) < type.ByteCount())
                return false;
            const int top = lua_gettop(m_state);
            luaL_checkstack(m_state, type.ByteCount() + 4, "custom type");
            for (int i = 0; i < type.ByteCount(); ++i)
                lua_pushinteger(m_state, bytes[static_cast<std::size_t>(i)]);
            lua_pushinteger(m_state, static_cast<lua_Integer>(address));
            std::string error;
            if (!CallReference(type.ToValue, type.ByteCount() + 1, 1, error))
            {
                lua_settop(m_state, top);
                return false;
            }
            const bool ok = lua_type(m_state, -1) == LUA_TSTRING || lua_type(m_state, -1) == LUA_TNUMBER;
            if (ok)
                text = lua_tostring(m_state, -1);
            lua_settop(m_state, top);
            return ok;
        }
        double value = 0.0;
        if (!CustomToNumber(type, bytes, address, value))
            return false;
        if (type.Float)
        {
            text = Format("%.6f", value);
            while (text.size() > 1 && text.back() == '0')
                text.pop_back();
            if (!text.empty() && text.back() == '.')
                text.pop_back();
        }
        else
        {
            text = std::to_string(static_cast<long long>(value));
        }
        return true;
    }

    bool LuaEngine::CustomFromText(const CustomType& type, std::string_view text, std::uint64_t address, std::vector<std::uint8_t>& bytes)
    {
        if (type.ToBytes <= 0)
            return false;
        const std::string trimmed = Trim(text);
        const auto number = type.String ? std::optional<double>() : ParseDouble(trimmed);
        if (!type.String && !number)
            return false;
        const int top = lua_gettop(m_state);
        if (type.String)
            lua_pushlstring(m_state, text.data(), text.size());
        else if (type.Float)
            lua_pushnumber(m_state, *number);
        else
            lua_pushinteger(m_state, static_cast<lua_Integer>(*number));
        lua_pushinteger(m_state, static_cast<lua_Integer>(address));
        std::string error;
        lua_rawgeti(m_state, LUA_REGISTRYINDEX, type.ToBytes);
        lua_insert(m_state, top + 1);
        if (!CallStack(2, LUA_MULTRET, error))
        {
            lua_settop(m_state, top);
            return false;
        }
        bytes.clear();
        const int results = lua_gettop(m_state) - top;
        if (results == 1 && lua_istable(m_state, -1))
        {
            const lua_Integer length = luaL_len(m_state, -1);
            for (lua_Integer i = 1; i <= length; ++i)
            {
                lua_rawgeti(m_state, -1, i);
                bytes.push_back(static_cast<std::uint8_t>(lua_tointeger(m_state, -1)));
                lua_pop(m_state, 1);
            }
        }
        else
        {
            for (int i = top + 1; i <= lua_gettop(m_state); ++i)
                bytes.push_back(static_cast<std::uint8_t>(lua_isinteger(m_state, i) ? lua_tointeger(m_state, i) : static_cast<lua_Integer>(lua_tonumber(m_state, i))));
        }
        lua_settop(m_state, top);
        if (bytes.empty())
            return false;
        bytes.resize(static_cast<std::size_t>(type.ByteCount()), 0);
        return true;
    }

    int LuaEngine::AddSymbolCallback(int reference, bool first)
    {
        const int id = m_nextSymbolCallback++;
        m_symbolLookups[id] = SymbolCallback{ reference, first };
        return id;
    }

    bool LuaEngine::RemoveSymbolCallback(int id)
    {
        const auto found = m_symbolLookups.find(id);
        if (found == m_symbolLookups.end())
            return false;
        Release(found->second.Reference);
        m_symbolLookups.erase(found);
        return true;
    }

    std::optional<std::uint64_t> LuaEngine::LookupSymbol(std::string_view name, bool first)
    {
        if (m_symbolLookups.empty() || m_inSymbolLookup || !m_state || std::this_thread::get_id() != m_thread)
            return std::nullopt;
        m_inSymbolLookup = true;
        std::optional<std::uint64_t> result;
        const std::map<int, SymbolCallback> callbacks = m_symbolLookups;
        for (const auto& [id, callback] : callbacks)
        {
            if (callback.First != first)
                continue;
            const int top = lua_gettop(m_state);
            lua_pushlstring(m_state, name.data(), name.size());
            std::string error;
            if (CallReference(callback.Reference, 1, 1, error))
            {
                if (const auto value = ExactInteger(m_state, -1))
                    result = static_cast<std::uint64_t>(*value);
                if (result && *result == 0)
                    result.reset();
            }
            lua_settop(m_state, top);
            if (result)
                break;
        }
        m_inSymbolLookup = false;
        return result;
    }

    int LuaEngine::AddStructureCallback(StructureCallback kind, int reference)
    {
        const int id = m_nextStructureCallback++;
        (kind == StructureCallback::NameLookup ? m_structureNameLookups : m_dissectOverrides)[id] = reference;
        return id;
    }

    std::vector<int> LuaEngine::StructureCallbackReferences(StructureCallback kind) const
    {
        std::vector<int> references;
        for (const auto& [id, reference] : kind == StructureCallback::NameLookup ? m_structureNameLookups : m_dissectOverrides)
            references.push_back(reference);
        return references;
    }

    bool LuaEngine::RemoveStructureCallback(StructureCallback kind, int id)
    {
        auto& callbacks = kind == StructureCallback::NameLookup ? m_structureNameLookups : m_dissectOverrides;
        const auto found = callbacks.find(id);
        if (found == callbacks.end())
            return false;
        Release(found->second);
        callbacks.erase(found);
        return true;
    }

    std::optional<StructureLookup> LuaEngine::LookupStructureName(std::uint64_t address)
    {
        const std::map<int, int> callbacks = m_structureNameLookups;
        for (const auto& [id, reference] : callbacks)
        {
            const int top = lua_gettop(m_state);
            lua_pushinteger(m_state, static_cast<lua_Integer>(address));
            std::string error;
            if (!CallReference(reference, 1, 2, error))
            {
                lua_settop(m_state, top);
                continue;
            }
            StructureLookup result;
            result.Address = address;
            if (lua_type(m_state, -2) == LUA_TSTRING)
                result.Name = lua_tostring(m_state, -2);
            if (const auto value = ExactInteger(m_state, -1))
                result.Address = static_cast<std::uint64_t>(*value);
            lua_settop(m_state, top);
            if (!result.Name.empty())
                return result;
        }
        return std::nullopt;
    }

    bool LuaEngine::DissectStructure(Table::StructDefinition& definition, std::uint64_t address)
    {
        const std::map<int, int> callbacks = m_dissectOverrides;
        for (const auto& [id, reference] : callbacks)
        {
            const int top = lua_gettop(m_state);
            PushStructureObject(m_state, &definition);
            lua_pushinteger(m_state, static_cast<lua_Integer>(address));
            std::string error;
            if (!CallReference(reference, 2, 1, error))
            {
                lua_settop(m_state, top);
                continue;
            }
            const bool handled = lua_toboolean(m_state, -1) != 0;
            lua_settop(m_state, top);
            if (handled)
            {
                definition.Sort();
                return true;
            }
        }
        return false;
    }

    namespace
    {
        struct LookupChain
        {
            std::vector<int> References;
            std::size_t Next = 0;
            std::uint64_t Address = 0;
            std::function<void(std::optional<StructureLookup>)> NameDone;
            std::function<void(bool)> DissectDone;
            Table::StructDefinition* Definition = nullptr;
        };

        bool StillRegistered(const std::map<int, int>& callbacks, int reference)
        {
            return std::any_of(callbacks.begin(), callbacks.end(), [reference](const auto& entry) { return entry.second == reference; });
        }
    }

    void LuaEngine::LookupStructureNameLater(std::uint64_t address, std::function<void(std::optional<StructureLookup>)> done)
    {
        if (!m_engine.Async() || m_structureNameLookups.empty())
        {
            done(LookupStructureName(address));
            return;
        }
        auto chain = std::make_shared<LookupChain>();
        for (const auto& [id, reference] : m_structureNameLookups)
            chain->References.push_back(reference);
        chain->Address = address;
        chain->NameDone = std::move(done);
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, chain, weak = std::weak_ptr<std::function<void()>>(step)]()
            {
                while (chain->Next < chain->References.size() && !StillRegistered(m_structureNameLookups, chain->References[chain->Next]))
                    ++chain->Next;
                if (chain->Next >= chain->References.size())
                {
                    chain->NameDone(std::nullopt);
                    return;
                }
                const int reference = chain->References[chain->Next++];
                auto found = std::make_shared<StructureLookup>();
                found->Address = chain->Address;
                auto keep = weak.lock();
                StartCall(reference, [address = chain->Address](lua_State* thread)
                    {
                        lua_pushinteger(thread, static_cast<lua_Integer>(address));
                        return 1;
                    }, "structure name lookup", [found](lua_State* thread, int results)
                    {
                        if (results >= 1 && lua_type(thread, -results) == LUA_TSTRING)
                            found->Name = lua_tostring(thread, -results);
                        if (results >= 2)
                        {
                            if (const auto value = ExactInteger(thread, -results + 1))
                                found->Address = static_cast<std::uint64_t>(*value);
                        }
                    }, [chain, found, keep](const Status&, const std::string&)
                    {
                        if (!found->Name.empty())
                            chain->NameDone(*found);
                        else if (keep)
                            (*keep)();
                    });
            };
        (*step)();
    }

    void LuaEngine::DissectStructureLater(Table::StructDefinition* definition, std::uint64_t address, std::function<void(bool handled)> done)
    {
        if (!m_engine.Async() || m_dissectOverrides.empty() || !definition)
        {
            const bool handled = definition && DissectStructure(*definition, address);
            done(handled);
            return;
        }
        auto chain = std::make_shared<LookupChain>();
        for (const auto& [id, reference] : m_dissectOverrides)
            chain->References.push_back(reference);
        chain->Address = address;
        chain->Definition = definition;
        chain->DissectDone = std::move(done);
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, chain, weak = std::weak_ptr<std::function<void()>>(step)]()
            {
                while (chain->Next < chain->References.size() && !StillRegistered(m_dissectOverrides, chain->References[chain->Next]))
                    ++chain->Next;
                if (chain->Next >= chain->References.size())
                {
                    chain->DissectDone(false);
                    return;
                }
                const int reference = chain->References[chain->Next++];
                auto handled = std::make_shared<bool>(false);
                auto keep = weak.lock();
                StartCall(reference, [chain](lua_State* thread)
                    {
                        PushStructureObject(thread, chain->Definition);
                        lua_pushinteger(thread, static_cast<lua_Integer>(chain->Address));
                        return 2;
                    }, "structure dissect override", [handled](lua_State* thread, int results)
                    {
                        *handled = results >= 1 && lua_toboolean(thread, -results) != 0;
                    }, [chain, handled, keep](const Status&, const std::string&)
                    {
                        if (*handled)
                            chain->DissectDone(true);
                        else if (keep)
                            (*keep)();
                    });
            };
        (*step)();
    }

    void LuaEngine::ForgetRecords()
    {
        Lua::ForgetRecords(m_state);
    }

    void LuaEngine::Release(int reference)
    {
        if (reference > 0)
            luaL_unref(m_state, LUA_REGISTRYINDEX, reference);
    }

    int LuaEngine::AddTimer(int interval, bool enabled)
    {
        Timer timer;
        timer.Id = m_nextTimer++;
        timer.Interval = std::max(1, interval);
        timer.Enabled = enabled;
        m_timers[timer.Id] = timer;
        return timer.Id;
    }

    Timer* LuaEngine::FindTimer(int id)
    {
        const auto found = m_timers.find(id);
        return found == m_timers.end() || found->second.Destroyed ? nullptr : &found->second;
    }

    int LuaEngine::AddHotkey(std::vector<int> keys, int callback)
    {
        LuaHotkey hotkey;
        hotkey.Id = m_nextHotkey++;
        hotkey.Keys = std::move(keys);
        hotkey.Callback = callback;
        m_hotkeys[hotkey.Id] = hotkey;
        return hotkey.Id;
    }

    void LuaEngine::RemoveHotkey(int id)
    {
        const auto found = m_hotkeys.find(id);
        if (found == m_hotkeys.end())
            return;
        Release(found->second.Callback);
        m_hotkeys.erase(found);
    }

    void LuaEngine::ProcessMessages()
    {
        if (std::this_thread::get_id() != m_thread)
            return;
        MSG message{};
        ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        Tick(m_engine.Now());
    }

    void LuaEngine::BlockingSleep(std::uint32_t milliseconds)
    {
        if (std::this_thread::get_id() != m_thread)
        {
            ::Sleep(milliseconds);
            return;
        }
        const ULONGLONG until = ::GetTickCount64() + milliseconds;
        for (;;)
        {
            MSG message{};
            ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            const ULONGLONG now = ::GetTickCount64();
            if (now >= until)
                return;
            ::Sleep(static_cast<DWORD>(std::min<ULONGLONG>(until - now, 100)));
        }
    }

    void LuaEngine::Tick(double now)
    {
        if (m_inTick)
            return;
        m_inTick = true;

        std::vector<int> due;
        for (auto& [id, timer] : m_timers)
        {
            if (timer.Destroyed || !timer.Enabled || timer.Callback <= 0)
                continue;
            if (timer.NextAt == 0.0)
                timer.NextAt = now + timer.Interval / 1000.0;
            if (now >= timer.NextAt)
            {
                timer.NextAt = now + timer.Interval / 1000.0;
                due.push_back(id);
            }
        }
        for (int id : due)
        {
            Timer* timer = FindTimer(id);
            if (!timer || !timer->Enabled)
                continue;
            const bool oneShot = timer->OneShot;
            PushTimer(m_state, id);
            std::string error;
            if (!CallReference(timer->Callback, 1, 0, error))
            {
                if (Timer* again = FindTimer(id))
                    again->Enabled = false;
                m_engine.Notify(Status::Fail(ErrorCode::LuaTimerFailed, FirstLine(error)));
            }
            if (oneShot)
            {
                if (Timer* again = FindTimer(id))
                    again->Destroyed = true;
            }
        }
        for (auto it = m_timers.begin(); it != m_timers.end();)
        {
            if (it->second.Destroyed)
            {
                Release(it->second.Callback);
                it = m_timers.erase(it);
            }
            else
            {
                ++it;
            }
        }

        const auto down = [](int key) { return (::GetAsyncKeyState(key) & 0x8000) != 0; };
        std::vector<int> pressed;
        for (auto& [id, hotkey] : m_hotkeys)
        {
            const bool all = !hotkey.Keys.empty() && std::all_of(hotkey.Keys.begin(), hotkey.Keys.end(), down);
            if (all && !hotkey.WasDown)
                pressed.push_back(id);
            hotkey.WasDown = all;
        }
        for (int id : pressed)
        {
            const auto found = m_hotkeys.find(id);
            if (found == m_hotkeys.end())
                continue;
            std::string error;
            if (!CallReference(found->second.Callback, 0, 0, error))
                m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
        }

        ResumeThreads(*this, m_state, now);
        m_inTick = false;
    }

    bool LuaEngine::HasBreakpointCallback(std::uint64_t address) const
    {
        return m_breakpointCallbacks.count(address) > 0;
    }

    void LuaEngine::SetBreakpointCallback(std::uint64_t address, int reference)
    {
        ClearBreakpointCallback(address);
        if (reference > 0)
            m_breakpointCallbacks[address] = reference;
    }

    void LuaEngine::ClearBreakpointCallback(std::uint64_t address)
    {
        const auto found = m_breakpointCallbacks.find(address);
        if (found == m_breakpointCallbacks.end())
            return;
        Release(found->second);
        m_breakpointCallbacks.erase(found);
    }

    bool LuaEngine::RunBreakpointCallback(std::uint64_t address)
    {
        const auto found = m_breakpointCallbacks.find(address);
        if (found == m_breakpointCallbacks.end())
            return false;
        const Debug::BreakInfo info = m_engine.Debugger().Break();
        PushRegisters(m_state, info.Registers, info.Is64Bit);
        std::string error;
        if (!CallReference(found->second, 0, 1, error))
        {
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
            return false;
        }
        const bool continueRunning = lua_toboolean(m_state, -1) != 0 || (lua_isinteger(m_state, -1) && lua_tointeger(m_state, -1) != 0);
        lua_pop(m_state, 1);
        Disasm::RegisterValues changed = info.Registers;
        if (PullRegisters(m_state, changed, info.Is64Bit))
            m_engine.Debugger().SetRegisters(changed);
        return continueRunning;
    }

    void LuaEngine::OnProcessOpened()
    {
        const auto& process = m_engine.Process();
        if (process)
        {
            const auto main = process->MainModule();
            lua_pushstring(m_state, main ? main->Name.c_str() : process->Name().c_str());
        }
        else
        {
            lua_pushnil(m_state);
        }
        lua_setglobal(m_state, "process");
        const std::uint32_t id = process ? Raw(process->Id()) : 0;
        std::string error;
        lua_getglobal(m_state, "onOpenProcess");
        if (lua_isfunction(m_state, -1))
        {
            const int reference = luaL_ref(m_state, LUA_REGISTRYINDEX);
            lua_pushinteger(m_state, id);
            if (!CallReference(reference, 1, 0, error))
                m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
            Release(reference);
        }
        else
        {
            lua_pop(m_state, 1);
        }
        if (!process)
            return;
        lua_pushcfunction(m_state, [](lua_State* state) -> int
            {
                if (lua_getglobal(state, "MainForm") == LUA_TNIL)
                    return 0;
                lua_getfield(state, -1, "OnProcessOpened");
                if (!lua_isfunction(state, -1))
                    return 0;
                lua_pushvalue(state, 1);
                lua_pushvalue(state, 2);
                lua_pushvalue(state, 3);
                lua_call(state, 3, 0);
                return 0;
            });
        lua_pushinteger(m_state, id);
        lua_pushinteger(m_state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(process->Handle())));
        lua_pushstring(m_state, Format("%08X-%s", id, process->Name().c_str()).c_str());
        if (!CallStack(3, 0, error))
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
        DispatchEvent("processOpened");
    }

    void LuaEngine::OnProcessClosed()
    {
        lua_pushnil(m_state);
        lua_setglobal(m_state, "process");
        DispatchEvent("processClosed");
    }

    void LuaEngine::DispatchEvent(const char* name)
    {
        if (std::this_thread::get_id() != m_thread)
            return;
        if (lua_getfield(m_state, LUA_REGISTRYINDEX, "Manifold.Dispatch") != LUA_TFUNCTION)
        {
            lua_pop(m_state, 1);
            return;
        }
        lua_pushstring(m_state, name);
        std::string error;
        if (!CallStack(1, 0, error))
            m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, FirstLine(error)));
    }

    std::vector<std::string> LuaEngine::FunctionNames() const
    {
        return ApiFunctionNames();
    }
}
