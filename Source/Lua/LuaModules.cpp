#include "Lua/LuaModules.h"

#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaEngine.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <fstream>
#include <sstream>

namespace Manifold::Lua
{
    namespace
    {
        using Autorun::Module;
        using Autorun::State;

        int ModuleTraceback(lua_State* state)
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

        std::string FirstLineOf(const std::string& text)
        {
            const std::size_t end = text.find('\n');
            return end == std::string::npos ? text : text.substr(0, end);
        }

        std::string PathKey(const std::filesystem::path& path)
        {
            std::string text = Lower(WideToUtf8(path.lexically_normal().wstring()));
            std::replace(text.begin(), text.end(), '/', '\\');
            while (!text.empty() && text.back() == '\\')
                text.pop_back();
            return text;
        }

        bool Inside(const std::string& path, const std::string& folder)
        {
            return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '\\';
        }

        bool Selectable(const Module& module)
        {
            return module.State != State::Superseded && module.State != State::BuiltIn && !module.Duplicate;
        }

        std::string HeldLine(const Module& module)
        {
            return (module.New ? "New module " : "Module ") + module.Name + " (" + module.Where + ") is held until it is allowed in Extensions > Modules";
        }

        struct DepthGuard
        {
            int& Depth;
            explicit DepthGuard(int& depth) : Depth(depth) { ++Depth; }
            ~DepthGuard() { --Depth; }
            DepthGuard(const DepthGuard&) = delete;
            DepthGuard& operator=(const DepthGuard&) = delete;
        };

        std::optional<std::string> ReadScript(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::nullopt;
            std::ostringstream content;
            content << file.rdbuf();
            std::string text = content.str();
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
                text.erase(0, 3);
            return text;
        }

        std::string FolderText(const std::filesystem::path& folder)
        {
            std::string text = WideToUtf8(folder.wstring());
            if (!text.empty() && text.back() != '\\' && text.back() != '/')
                text += '\\';
            return text;
        }

        void SetText(lua_State* state, const char* key, std::string_view value)
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }

        void SetFlag(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        const char* StateName(State state)
        {
            switch (state)
            {
            case State::Pending: return "pending";
            case State::Loaded: return "loaded";
            case State::Disabled: return "disabled";
            case State::Failed: return "failed";
            case State::Blocked: return "blocked";
            case State::Held: return "held";
            case State::Superseded: return "superseded";
            default: return "builtIn";
            }
        }

        void PushModule(lua_State* state, LuaEngine& engine, const Module& module)
        {
            lua_createtable(state, 0, 18);
            SetText(state, "name", module.Name);
            SetText(state, "version", module.Header.Version);
            SetText(state, "author", module.Header.Author);
            SetText(state, "description", module.Header.Description);
            SetText(state, "category", module.Category);
            SetText(state, "kind", module.Shape == Autorun::Shape::Loose ? "loose" : module.Shape == Autorun::Shape::File ? "file" : "folder");
            SetText(state, "origin", module.Origin == Autorun::Origin::Shipped ? "shipped" : "user");
            SetText(state, "state", StateName(module.State));
            SetText(state, "reason", module.Reason);
            SetFlag(state, "enabled", module.Enabled);
            SetFlag(state, "changed", module.Changed);
            SetText(state, "path", FolderText(module.Shape == Autorun::Shape::Folder ? module.Root : module.Entry.parent_path()));
            SetText(state, "entry", WideToUtf8(module.Entry.wstring()));
            SetText(state, "dataPath", FolderText(engine.ModuleDataFolder(module)));
            lua_pushnumber(state, module.LoadMs);
            lua_setfield(state, -2, "loadTime");
            lua_createtable(state, static_cast<int>(module.Header.Requires.size()), 0);
            lua_Integer index = 1;
            for (const Autorun::Requirement& requirement : module.Header.Requires)
            {
                lua_createtable(state, 0, 2);
                SetText(state, "name", requirement.Name);
                SetText(state, "version", requirement.MinVersion);
                lua_rawseti(state, -2, index++);
            }
            lua_setfield(state, -2, "requires");
        }

        int ModuleOfSourceNative(lua_State* state)
        {
            std::size_t length = 0;
            const char* source = luaL_optlstring(state, 1, "", &length);
            const Module* module = EngineOf(state).ModuleOfSource(std::string_view(source, length));
            if (!module)
                return 0;
            lua_pushlstring(state, module->Key.data(), module->Key.size());
            return 1;
        }

        int ModuleInfoNative(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            const Module* module = Autorun::Find(engine.Modules(), luaL_checkstring(state, 1));
            if (!module)
                return PushFailure(state, ErrorCode::LuaModuleUnknown, luaL_checkstring(state, 1));
            PushModule(state, engine, *module);
            return 1;
        }

        int ModuleListNative(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            lua_createtable(state, static_cast<int>(engine.Modules().size()), 0);
            lua_Integer index = 1;
            for (const Module& module : engine.Modules())
            {
                PushModule(state, engine, module);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int ModuleResolveNative(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            const Module* module = Autorun::Find(engine.Modules(), luaL_checkstring(state, 1));
            const std::string name = luaL_checkstring(state, 2);
            if (!module || module->Shape != Autorun::Shape::Folder || name.empty() || name.find("..") != std::string::npos || name.find_first_of(":*?\"<>|") != std::string::npos ||
                name.front() == '\\' || name.front() == '/')
                return 0;
            std::vector<std::string> names{ name };
            if (name.find('.') != std::string::npos)
            {
                std::string nested = name;
                std::replace(nested.begin(), nested.end(), '.', '\\');
                names.push_back(nested);
            }
            for (const std::string& candidate : names)
            {
                for (const std::filesystem::path& path : { module->Root / Utf8ToWide(candidate + ".lua"), module->Root / Utf8ToWide(candidate) / L"init.lua" })
                {
                    std::error_code error;
                    if (std::filesystem::is_regular_file(path, error) && PathKey(path) != PathKey(module->Entry))
                    {
                        const std::string text = WideToUtf8(path.wstring());
                        lua_pushlstring(state, text.data(), text.size());
                        return 1;
                    }
                }
            }
            return 0;
        }

        int ModuleLoadFileNative(lua_State* state)
        {
            const std::string path = luaL_checkstring(state, 1);
            const std::optional<std::string> text = ReadScript(Utf8ToWide(path));
            if (!text)
            {
                lua_pushnil(state);
                lua_pushstring(state, ("cannot read " + path).c_str());
                return 2;
            }
            if (luaL_loadbuffer(state, text->data(), text->size(), ("@" + path).c_str()) != LUA_OK)
            {
                lua_pushnil(state);
                lua_insert(state, -2);
                return 2;
            }
            return 1;
        }

        int ModuleLookupNative(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            const Module* module = Autorun::Find(engine.Modules(), luaL_checkstring(state, 1));
            if (!module || !Selectable(*module))
                return 0;
            const Module* requester = lua_type(state, 2) == LUA_TSTRING ? Autorun::Find(engine.Modules(), lua_tostring(state, 2)) : nullptr;
            if (requester == module)
                return 0;
            bool declared = requester == nullptr;
            if (requester)
            {
                for (const Autorun::Requirement& requirement : requester->Header.Requires)
                    declared = declared || Autorun::KeyOf(requirement.Name) == module->Key;
            }
            lua_pushlstring(state, module->Key.data(), module->Key.size());
            lua_pushboolean(state, module->State == State::Loaded ? 1 : 0);
            lua_pushboolean(state, declared ? 1 : 0);
            lua_pushstring(state, (module->Name + " " + Lower(Autorun::StateText(*module))).c_str());
            return 4;
        }

        int Done(lua_State* state, const Status& status)
        {
            if (!status)
                return PushFailure(state, status);
            lua_pushboolean(state, 1);
            return 1;
        }

        int ModuleReloadNative(lua_State* state)
        {
            return Done(state, EngineOf(state).ReloadModule(luaL_checkstring(state, 1)));
        }

        int ModuleSetEnabledNative(lua_State* state)
        {
            return Done(state, EngineOf(state).SetModuleEnabled(luaL_checkstring(state, 1), lua_toboolean(state, 2) != 0));
        }

        int ModuleEnsureDataNative(lua_State* state)
        {
            LuaEngine& engine = EngineOf(state);
            if (const Module* module = Autorun::Find(engine.Modules(), luaL_checkstring(state, 1)))
            {
                std::error_code ignored;
                std::filesystem::create_directories(engine.ModuleDataFolder(*module), ignored);
                const std::string text = FolderText(engine.ModuleDataFolder(*module));
                lua_pushlstring(state, text.data(), text.size());
                return 1;
            }
            return 0;
        }

        int TracedWrite(lua_State* state)
        {
            const lua_Integer kind = lua_tointeger(state, lua_upvalueindex(3));
            const char* name = lua_tostring(state, lua_upvalueindex(2));
            if (kind == 0)
            {
                const char* mode = lua_type(state, 2) == LUA_TSTRING ? lua_tostring(state, 2) : "r";
                if (lua_type(state, 1) == LUA_TSTRING && std::strpbrk(mode, "wa+"))
                    NoteAutorunWrite(state, name, lua_tostring(state, 1));
            }
            else
            {
                for (int index = 1; index <= kind; ++index)
                {
                    if (lua_type(state, index) == LUA_TSTRING)
                        NoteAutorunWrite(state, name, lua_tostring(state, index));
                }
            }
            const int arguments = lua_gettop(state);
            lua_pushvalue(state, lua_upvalueindex(1));
            lua_insert(state, 1);
            lua_call(state, arguments, LUA_MULTRET);
            return lua_gettop(state);
        }

        void TraceWrite(lua_State* state, const char* library, const char* function, int kind)
        {
            lua_getglobal(state, library);
            if (lua_istable(state, -1))
            {
                lua_getfield(state, -1, function);
                if (lua_isfunction(state, -1))
                {
                    lua_pushfstring(state, "%s.%s", library, function);
                    lua_pushinteger(state, kind);
                    lua_pushcclosure(state, &TracedWrite, 3);
                    lua_setfield(state, -2, function);
                }
                else
                {
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 1);
        }
    }

    void RegisterModuleNatives(lua_State* state, int table)
    {
        const luaL_Reg natives[] = {
            { "moduleOfSource", &ModuleOfSourceNative },
            { "moduleInfo", &ModuleInfoNative },
            { "moduleList", &ModuleListNative },
            { "moduleResolve", &ModuleResolveNative },
            { "moduleLoadFile", &ModuleLoadFileNative },
            { "moduleLookup", &ModuleLookupNative },
            { "moduleReload", &ModuleReloadNative },
            { "moduleSetEnabled", &ModuleSetEnabledNative },
            { "moduleEnsureData", &ModuleEnsureDataNative },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, natives, 0);
        lua_pop(state, 1);
    }

    void TraceAutorunWrites(lua_State* state)
    {
        TraceWrite(state, "io", "open", 0);
        TraceWrite(state, "os", "remove", 1);
        TraceWrite(state, "os", "rename", 2);
    }

    void NoteAutorunWrite(lua_State* state, std::string_view function, std::string_view path)
    {
        EngineOf(state).NoteScriptWrite(state, function, path);
    }

    const Autorun::Module* LuaEngine::ModuleOfSource(std::string_view source) const
    {
        if (!source.empty() && source.front() == '@' && !m_modules.empty())
        {
            const std::string path = PathKey(Utf8ToWide(source.substr(1)));
            for (const Module& module : m_modules)
            {
                if (!Selectable(module))
                    continue;
                if (module.Shape == Autorun::Shape::Folder ? Inside(path, PathKey(module.Root)) : path == PathKey(module.Entry))
                    return &module;
            }
        }
        if (!m_loadingModule.empty())
            return Autorun::Find(m_modules, m_loadingModule);
        return nullptr;
    }

    std::filesystem::path LuaEngine::ModuleDataFolder(const Autorun::Module& module) const
    {
        // ∑ A NAME line cannot give "..", but a file name can ("...lua" has the stem ".."); dots and spaces at the ends become '_' so the folder stays inside data\.
        std::string name = module.Name;
        if (!Autorun::ValidName(name))
        {
            for (std::size_t i = 0; i < name.size() && (name[i] == '.' || name[i] == ' '); ++i)
                name[i] = '_';
            for (std::size_t i = name.size(); i > 0 && (name[i - 1] == '.' || name[i - 1] == ' '); --i)
                name[i - 1] = '_';
            if (name.empty())
                name = "_";
        }
        return m_autorunFolder / L"data" / Utf8ToWide(name);
    }

    int LuaEngine::ModulesNeedingAttention() const
    {
        return static_cast<int>(std::count_if(m_modules.begin(), m_modules.end(), [](const Module& module)
            {
                return module.State == State::Held || module.State == State::Blocked || (module.State == State::Failed) || module.HeldNext;
            }));
    }

    int LuaEngine::ManifoldApiVersion()
    {
        const int top = lua_gettop(m_state);
        int version = 0;
        lua_pushglobaltable(m_state);
        lua_pushstring(m_state, "manifold");
        if (lua_rawget(m_state, -2) == LUA_TTABLE && lua_getfield(m_state, -1, "apiVersion") == LUA_TNUMBER)
            version = static_cast<int>(lua_tointeger(m_state, -1));
        lua_settop(m_state, top);
        return version;
    }

    void LuaEngine::AddLibraryPath()
    {
        const std::string folder = FolderText(m_autorunFolder / L"lib");
        const std::string wanted = folder + "?.lua;" + folder + "?\\init.lua";
        const int top = lua_gettop(m_state);
        lua_getglobal(m_state, "package");
        if (lua_istable(m_state, -1))
        {
            lua_getfield(m_state, -1, "path");
            const std::string current = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "";
            if (current.find(wanted) == std::string::npos)
            {
                lua_pushstring(m_state, (current.empty() ? wanted : current + ";" + wanted).c_str());
                lua_setfield(m_state, -3, "path");
            }
        }
        lua_settop(m_state, top);
    }

    bool LuaEngine::UnloadModule(Autorun::Module& module)
    {
        m_loadedModules.erase(std::remove(m_loadedModules.begin(), m_loadedModules.end(), module.Key), m_loadedModules.end());
        const int top = lua_gettop(m_state);
        bool handled = false;
        if (lua_getfield(m_state, LUA_REGISTRYINDEX, "Manifold.ModuleUnload") == LUA_TFUNCTION)
        {
            // ∑ The onUnload handlers run Lua that may call manifold.module.*; while they run, ModulesBusy refuses those calls so nobody replaces m_modules under the caller's Module*.
            const DepthGuard unloading(m_unloadingModules);
            lua_pushlstring(m_state, module.Key.data(), module.Key.size());
            std::string error;
            if (CallStack(1, 1, error))
                handled = lua_toboolean(m_state, -1) != 0;
            else
                LOG_ERROR("Autorun", module.Name + " did not unload cleanly: " + FirstLineOf(error));
        }
        lua_settop(m_state, top);
        return handled;
    }

    bool LuaEngine::RunModuleEntry(Autorun::Module& module)
    {
        const std::optional<std::string> text = ReadScript(module.Entry);
        if (!text)
        {
            module.State = State::Failed;
            module.Reason = "The file could not be read";
            return false;
        }
        const std::size_t leftovers = m_timers.size() + m_hotkeys.size() + m_customTypes.size() + m_autoAssemblerCommands.size();
        const std::uint64_t menus = m_menuGeneration;
        const std::uint64_t forms = m_formsGeneration;
        const int top = lua_gettop(m_state);
        const std::string name = "@" + WideToUtf8(module.Entry.wstring());
        module.Traceback.clear();
        module.Reason.clear();
        lua_pushcfunction(m_state, &ModuleTraceback);
        if (luaL_loadbuffer(m_state, text->data(), text->size(), name.c_str()) != LUA_OK)
        {
            const std::string message = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "unknown error";
            lua_settop(m_state, top);
            PrintError(message);
            module.State = State::Failed;
            module.Reason = FirstLineOf(message);
            return false;
        }
        ChunkStarting(name);
        m_loadingModule = module.Key;
        const auto started = std::chrono::steady_clock::now();
        const int status = lua_pcall(m_state, 0, 1, top + 1);
        module.LoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        m_loadingModule.clear();
        if (status != LUA_OK)
        {
            const std::string message = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "unknown error";
            lua_settop(m_state, top);
            PrintError(message);
            SplitTraceback(message, module.Reason, module.Traceback);
            module.State = State::Failed;
            UnloadModule(module);
            return false;
        }
        if (lua_getfield(m_state, LUA_REGISTRYINDEX, "Manifold.ModuleLoaded") == LUA_TFUNCTION)
        {
            lua_pushlstring(m_state, module.Key.data(), module.Key.size());
            lua_pushvalue(m_state, -3);
            std::string error;
            CallStack(2, 0, error);
        }
        lua_settop(m_state, top);
        module.State = State::Loaded;
        module.ActiveUntilRestart = false;
        module.Untracked = leftovers != m_timers.size() + m_hotkeys.size() + m_customTypes.size() + m_autoAssemblerCommands.size() || menus != m_menuGeneration || forms != m_formsGeneration;
        m_loadedModules.push_back(module.Key);
        return true;
    }

    bool LuaEngine::ReadModuleAgain(Autorun::Module& module)
    {
        const std::string key = module.Key;
        const bool held = module.State == State::Held;
        Autorun::Refresh(module);
        if (module.Key != key)
            return false;
        if (module.State != State::Held && module.State != State::BuiltIn)
            module.State = State::Pending;
        Autorun::ApplyState(module, m_moduleStore, { m_autorunOptions.HoldNew, Autorun::ShippedModules() });
        if (module.State == State::Held)
        {
            module.HeldNext = false;
            if (!held)
                LOG_WARN("Autorun", HeldLine(module));
        }
        return true;
    }

    // ∑ The last check before a module runs, whoever made it pending: code a script wrote this session, or a stored state that is not
    // allowed (the same rules as Autorun::ApplyState), holds it even when the caller did not read the module again.
    bool LuaEngine::HoldUnlessAllowed(Autorun::Module& module)
    {
        const Autorun::Stored* stored = m_moduleStore.Find(module.Key);
        if (module.HeldNext || (!module.Unmodified && stored && !stored->Allowed))
        {
            module.Reason = "A script wrote to its files; it runs again once it is allowed";
        }
        else if (!module.Unmodified && !stored && m_moduleStore.Existed() && m_autorunOptions.HoldNew)
        {
            module.New = true;
            module.Reason = "It is new in the autorun folder and runs once it is allowed";
        }
        else
        {
            return false;
        }
        module.State = State::Held;
        module.HeldNext = false;
        return true;
    }

    const char* LuaEngine::ModulesBusy() const
    {
        if (m_unloadingModules > 0)
            return "unloads";
        if (!m_loadingModule.empty())
            return "loads";
        return nullptr;
    }

    void LuaEngine::LoadPendingModules(bool announce, Status& first)
    {
        const auto fail = [&](ErrorCode code, const Module& module)
        {
            const Status failed = Status::Fail(code, module.Name + ": " + module.Reason);
            LOG_ERROR("Autorun", failed.Text);
            if (first.Ok())
                first = failed;
        };
        std::map<std::string, std::string> blockedBefore;
        for (Module& module : m_modules)
        {
            if (module.State != State::Blocked)
                continue;
            blockedBefore[module.Key] = module.Reason;
            // ∑ A blocked module was read when the folder was scanned; it is read again so edits and script writes since then count before it runs.
            const Module before = module;
            if (ReadModuleAgain(module))
            {
                module.Changed = module.Changed || before.Changed;
            }
            else
            {
                module = before;
                module.Reason = "its NAME changed since the folder was read; reload all modules to read it again";
            }
        }
        const std::vector<std::size_t> order = Autorun::Order(m_modules, ManifoldApiVersion());
        for (const std::size_t index : order)
        {
            Module& module = m_modules[index];
            if (module.State == State::Pending)
            {
                std::string reason;
                if (!Autorun::Satisfied(m_modules, module, reason))
                {
                    module.State = State::Blocked;
                    module.Reason = reason;
                }
            }
            switch (module.State)
            {
            case State::Pending:
                if (HoldUnlessAllowed(module))
                    LOG_WARN("Autorun", HeldLine(module));
                else if (RunModuleEntry(module))
                {
                    std::string line = "Loaded " + module.Name;
                    if (!module.Header.Version.empty())
                        line += " " + module.Header.Version;
                    if (!module.Category.empty())
                        line += " (" + module.Category + ")";
                    LOG_INFO("Autorun", line + Format(" in %.0f ms", module.LoadMs));
                    if (module.Changed)
                        LOG_WARN("Autorun", module.Name + " changed since it last ran (" + module.Where + ")");
                }
                else
                {
                    fail(ErrorCode::LuaAutorunFailed, module);
                }
                break;
            case State::Blocked:
                if (const auto before = blockedBefore.find(module.Key); announce || before == blockedBefore.end() || before->second != module.Reason)
                    fail(ErrorCode::LuaModuleBlocked, module);
                break;
            case State::Failed:
                if (announce && module.Duplicate)
                    fail(ErrorCode::LuaAutorunFailed, module);
                break;
            case State::Held:
                if (announce)
                    LOG_WARN("Autorun", HeldLine(module));
                break;
            case State::Superseded:
            case State::BuiltIn:
                if (announce)
                    LOG_INFO("Autorun", "Skipped " + module.Where + ": " + module.Reason + (module.State == State::BuiltIn ? ". The file stays in the autorun folder and can be deleted." : "."));
                break;
            default:
                break;
            }
        }
    }

    Status LuaEngine::RunAutorun(const std::filesystem::path& folder, const AutorunOptions& options)
    {
        if (const char* busy = ModulesBusy())
            return Status::Fail(ErrorCode::LuaModuleBlocked, std::string("the modules cannot be reloaded while one of them ") + busy);
        std::error_code error;
        const bool missing = !std::filesystem::is_directory(folder, error);
        std::vector<std::string> lingering;
        for (const Module& module : m_modules)
        {
            if (module.ActiveUntilRestart)
                lingering.push_back(module.Key);
        }
        const std::vector<std::string> loaded = m_loadedModules;
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it)
        {
            if (Module* module = Autorun::Find(m_modules, *it))
                UnloadModule(*module);
        }
        m_loadedModules.clear();
        const std::filesystem::path full = std::filesystem::absolute(folder, error);
        m_autorunFolder = error ? folder : full;
        m_autorunOptions = options;
        if (missing)
        {
            // ∑ What was loaded is unloaded above so the list is not stale; the state file is left alone, so the first start with a real folder still adopts what is in it.
            std::filesystem::create_directories(folder, error);
            m_modules.clear();
            ++m_modulesRevision;
            return Status::Success();
        }
        m_modules = Autorun::Scan(m_autorunFolder);
        m_moduleStore.Load(options.StateFile);
        Autorun::ApplyState(m_modules, m_moduleStore, { options.HoldNew, Autorun::ShippedModules() });
        for (Module& module : m_modules)
        {
            if (module.State == State::Disabled && std::find(lingering.begin(), lingering.end(), module.Key) != lingering.end())
                module.ActiveUntilRestart = true;
        }
        AddLibraryPath();
        Status first;
        LoadPendingModules(true, first);
        m_moduleStore.Save();
        ++m_modulesRevision;
        int counts[8] = {};
        for (const Module& module : m_modules)
            ++counts[static_cast<int>(module.State)];
        std::string summary = Format("%d of %zu modules loaded", counts[static_cast<int>(State::Loaded)], m_modules.size());
        for (const auto& [state, word] : { std::pair{ State::Failed, "failed" }, { State::Blocked, "blocked" }, { State::Held, "held" }, { State::Disabled, "disabled" } })
        {
            if (counts[static_cast<int>(state)] > 0)
                summary += Format(", %d %s", counts[static_cast<int>(state)], word);
        }
        LOG_INFO("Autorun", summary);
        return first;
    }

    void LuaEngine::DropDependents()
    {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (Module& module : m_modules)
            {
                std::string reason;
                if (module.State != State::Loaded || Autorun::Satisfied(m_modules, module, reason))
                    continue;
                UnloadModule(module);
                module.State = State::Blocked;
                module.Reason = reason;
                LOG_WARN("Autorun", module.Name + " was unloaded: " + reason);
                changed = true;
            }
        }
    }

    Status LuaEngine::ModuleStatus(std::string_view name) const
    {
        const Module* module = Autorun::Find(m_modules, name);
        if (!module)
            return Status::Fail(ErrorCode::LuaModuleUnknown, name);
        if (module->State == State::Failed)
            return Status::Fail(ErrorCode::LuaAutorunFailed, module->Name + ": " + module->Reason);
        if (module->State == State::Blocked)
            return Status::Fail(ErrorCode::LuaModuleBlocked, module->Name + ": " + module->Reason);
        if (module->State == State::Held)
            return Status::Fail(ErrorCode::LuaModuleHeld, module->Name);
        return Status::Success();
    }

    Status LuaEngine::SetModuleEnabled(std::string_view name, bool enabled)
    {
        Module* module = Autorun::Find(m_modules, name);
        if (!module)
            return Status::Fail(ErrorCode::LuaModuleUnknown, name);
        if (!Selectable(*module))
            return Status::Fail(ErrorCode::LuaModuleBlocked, module->Name + ": " + module->Reason);
        if (const char* busy = ModulesBusy())
            return Status::Fail(ErrorCode::LuaModuleBlocked, module->Name + ": modules cannot be switched while one of them " + busy);
        Autorun::Stored* stored = m_moduleStore.Find(module->Key);
        if (!stored)
        {
            stored = &m_moduleStore.Put(module->Key, module->Name);
            stored->Allowed = module->State != State::Held;
            stored->Hash = module->Hash;
            stored->FirstSeen = Autorun::Today();
        }
        stored->Enabled = enabled;
        m_moduleStore.Touch();
        module->Enabled = enabled;
        const std::string key = module->Key;
        Status first;
        if (!enabled)
        {
            if (module->State == State::Loaded)
            {
                const bool handled = UnloadModule(*module);
                module->ActiveUntilRestart = module->Untracked && !handled;
            }
            module->State = State::Disabled;
            module->Reason.clear();
            LOG_INFO("Autorun", "Disabled " + module->Name + (module->ActiveUntilRestart ? "; what it added to menus, forms or timers stays until the program restarts" : ""));
            DropDependents();
        }
        else if (module->State == State::Disabled)
        {
            if (!ReadModuleAgain(*module))
            {
                m_moduleStore.Save();
                return RunAutorun(std::filesystem::path(m_autorunFolder), AutorunOptions(m_autorunOptions));
            }
            LoadPendingModules(false, first);
        }
        m_moduleStore.Save();
        ++m_modulesRevision;
        return enabled ? ModuleStatus(key) : Status::Success();
    }

    Status LuaEngine::AllowModule(std::string_view name)
    {
        Module* module = Autorun::Find(m_modules, name);
        if (!module)
            return Status::Fail(ErrorCode::LuaModuleUnknown, name);
        if (const char* busy = ModulesBusy())
            return Status::Fail(ErrorCode::LuaModuleBlocked, module->Name + ": modules cannot be allowed while one of them " + busy);
        // ∑ Read before the allow: when NAME now gives another key the user allowed a module that no longer exists under that name, so the folder is read again and the renamed one is judged on its own.
        const std::string before = module->Key;
        Autorun::Refresh(*module);
        if (module->Key != before)
            return RunAutorun(std::filesystem::path(m_autorunFolder), AutorunOptions(m_autorunOptions));
        Autorun::Stored* stored = m_moduleStore.Find(module->Key);
        if (!stored)
        {
            stored = &m_moduleStore.Put(module->Key, module->Name);
            stored->FirstSeen = Autorun::Today();
        }
        stored->Allowed = true;
        stored->Hash = module->Hash;
        m_moduleStore.Touch();
        module->HeldNext = false;
        module->New = false;
        const std::string key = module->Key;
        Status first;
        if (module->State == State::Held)
        {
            module->State = State::Pending;
            module->Reason.clear();
            Autorun::ApplyState(*module, m_moduleStore, { m_autorunOptions.HoldNew, Autorun::ShippedModules() });
            LOG_INFO("Autorun", "Allowed " + module->Name);
            LoadPendingModules(false, first);
        }
        m_moduleStore.Save();
        ++m_modulesRevision;
        return ModuleStatus(key);
    }

    Status LuaEngine::ReloadModule(std::string_view name)
    {
        Module* module = Autorun::Find(m_modules, name);
        if (!module)
            return Status::Fail(ErrorCode::LuaModuleUnknown, name);
        if (!Selectable(*module) || module->State == State::Disabled)
            return Status::Fail(ErrorCode::LuaModuleBlocked, module->Name + ": " + (module->State == State::Disabled ? std::string("it is disabled") : module->Reason));
        if (const char* busy = ModulesBusy())
            return Status::Fail(ErrorCode::LuaModuleBlocked, module->Name + ": modules cannot be reloaded while one of them " + busy);
        const std::string key = module->Key;
        std::vector<std::string> again{ key };
        for (std::size_t i = 0; i < again.size(); ++i)
        {
            for (const Module& other : m_modules)
            {
                if (other.State != State::Loaded || std::find(again.begin(), again.end(), other.Key) != again.end())
                    continue;
                for (const Autorun::Requirement& requirement : other.Header.Requires)
                {
                    if (Autorun::KeyOf(requirement.Name) == again[i])
                    {
                        again.push_back(other.Key);
                        break;
                    }
                }
            }
        }
        const std::vector<std::string> loaded = m_loadedModules;
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it)
        {
            if (std::find(again.begin(), again.end(), *it) == again.end())
                continue;
            if (Module* other = Autorun::Find(m_modules, *it))
            {
                UnloadModule(*other);
                other->State = State::Pending;
            }
        }
        // ∑ The dependents are read again like the module itself: a script may have written one of them, and a renamed one needs a new scan.
        for (const std::string& rerun : again)
        {
            Module* other = Autorun::Find(m_modules, rerun);
            if (other && !ReadModuleAgain(*other))
                return RunAutorun(std::filesystem::path(m_autorunFolder), AutorunOptions(m_autorunOptions));
        }
        Status first;
        LoadPendingModules(false, first);
        m_moduleStore.Save();
        ++m_modulesRevision;
        return ModuleStatus(key);
    }

    void LuaEngine::NoteScriptWrite(lua_State* state, std::string_view function, std::string_view path)
    {
        if (m_autorunFolder.empty() || path.empty())
            return;
        std::error_code error;
        const std::filesystem::path full = std::filesystem::absolute(Utf8ToWide(path), error);
        if (error)
            return;
        const std::string target = PathKey(full);
        const std::string folder = PathKey(m_autorunFolder);
        if (!Inside(target, folder) || Inside(target, folder + "\\data"))
            return;
        std::string source;
        std::string where;
        lua_Debug frame{};
        for (int level = 0; lua_getstack(state, level, &frame); ++level)
        {
            lua_getinfo(state, "Sl", &frame);
            if (frame.what && std::string_view(frame.what) != "C")
            {
                source = frame.source ? frame.source : "";
                where = std::string(frame.short_src) + (frame.currentline > 0 ? ":" + std::to_string(frame.currentline) : std::string());
                break;
            }
        }
        const Module* writer = ModuleOfSource(source);
        Module* owner = nullptr;
        const bool code = EqualsNoCase(WideToUtf8(full.extension().wstring()), ".lua");
        if (code)
        {
            for (Module& module : m_modules)
            {
                if (Selectable(module) && (module.Shape == Autorun::Shape::Folder ? Inside(target, PathKey(module.Root)) : target == PathKey(module.Entry)))
                {
                    owner = &module;
                    break;
                }
            }
        }
        if (writer && (!owner || owner == writer))
        {
            if (code && !owner)
                LOG_WARN("Autorun", writer->Name + " wrote the script " + target.substr(folder.size() + 1) + " into the autorun folder (" + std::string(function) + ")");
            return;
        }
        LOG_WARN("Autorun", "Script wrote autorun\\" + target.substr(folder.size() + 1) + "  (" + std::string(function) + (where.empty() ? "" : ", " + where) + ")");
        if (!owner)
            return;
        Autorun::Stored* stored = m_moduleStore.Find(owner->Key);
        if (!stored)
        {
            stored = &m_moduleStore.Put(owner->Key, owner->Name);
            stored->Hash = owner->Hash;
            stored->FirstSeen = Autorun::Today();
        }
        if (stored->Allowed || !owner->HeldNext)
        {
            stored->Allowed = false;
            m_moduleStore.Touch();
            m_moduleStore.Save();
            owner->HeldNext = true;
            ++m_modulesRevision;
            LOG_WARN("Autorun", owner->Name + " is held from its next load until it is allowed in Extensions > Modules");
        }
    }
}
