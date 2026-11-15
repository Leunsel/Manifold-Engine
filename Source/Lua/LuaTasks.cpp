#include "Lua/LuaEngine.h"

#include "Core/RuntimeLog.h"
#include "Lua/LuaApi.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>

namespace Manifold::Lua
{
    namespace
    {
        constexpr int kHookInstructions = 1000;

        thread_local LuaEngine* t_engine = nullptr;

        double Clock()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::string FirstLine(const std::string& text)
        {
            const std::size_t end = text.find('\n');
            return end == std::string::npos ? text : text.substr(0, end);
        }
    }

    int LuaEngine::StartTask(std::string_view code, std::string_view chunkName, TaskDone done, std::function<void()> enter, std::function<void()> leave)
    {
        lua_State* thread = lua_newthread(m_state);
        const int reference = luaL_ref(m_state, LUA_REGISTRYINDEX);
        const std::string name = !chunkName.empty() && (chunkName.front() == '@' || chunkName.front() == '=') ? std::string(chunkName) : "=" + std::string(chunkName);
        if (luaL_loadbuffer(thread, code.data(), code.size(), name.c_str()) != LUA_OK)
        {
            const std::string message = lua_tostring(thread, -1) ? lua_tostring(thread, -1) : "unknown error";
            luaL_unref(m_state, LUA_REGISTRYINDEX, reference);
            Print(OutputKind::Error, message);
            LOG_ERROR("Lua", ErrorMessage(ErrorCode::LuaSyntax, message));
            if (done)
                done(Status::Fail(ErrorCode::LuaSyntax, message), {});
            return 0;
        }
        lua_sethook(thread, &LuaEngine::TaskHook, LUA_MASKCOUNT, kHookInstructions);
        auto task = std::make_unique<Task>();
        task->Id = m_nextTask++;
        task->Thread = thread;
        task->Reference = reference;
        task->Name = std::string(chunkName);
        task->Done = std::move(done);
        task->Enter = std::move(enter);
        task->Leave = std::move(leave);
        const int id = task->Id;
        m_tasks.push_back(std::move(task));
        return id;
    }

    void LuaEngine::TaskHook(lua_State* state, lua_Debug*)
    {
        LuaEngine* engine = t_engine;
        if (!engine || engine->m_taskStack.empty() || engine->m_taskStack.back()->Thread != state)
            return;
        const Task& task = *engine->m_taskStack.back();
        if (task.Stop)
        {
            if (lua_isyieldable(state))
                lua_yield(state, 0);
            else
                luaL_error(state, "stopped");
            return;
        }
        if (Clock() >= task.Deadline && lua_isyieldable(state))
            lua_yield(state, 0);
    }

    void LuaEngine::RunTasks(double budgetSeconds)
    {
        if (m_tasks.empty())
            return;
        const double deadline = Clock() + budgetSeconds;
        LuaEngine* previous = t_engine;
        t_engine = this;
        std::vector<int> ids;
        ids.reserve(m_tasks.size());
        for (const auto& task : m_tasks)
            ids.push_back(task->Id);
        for (int id : ids)
        {
            if (Clock() >= deadline)
                break;
            const auto found = std::find_if(m_tasks.begin(), m_tasks.end(), [id](const auto& task) { return task->Id == id; });
            if (found == m_tasks.end())
                continue;
            Task& task = **found;
            if (task.Running)
                continue;
            if (task.Stop && lua_status(task.Thread) == LUA_YIELD)
            {
                lua_pushstring(task.Thread, "stopped");
                FinishTask(task, LUA_ERRRUN, 0);
                continue;
            }
            if (task.WakeAt > Clock())
                continue;
            lua_State* from = m_taskStack.empty() ? m_state : m_taskStack.back()->Thread;
            task.Deadline = deadline;
            task.Running = true;
            m_taskStack.push_back(&task);
            if (task.Enter)
                task.Enter();
            int results = 0;
            const int status = lua_resume(task.Thread, from, 0, &results);
            m_taskStack.pop_back();
            task.Running = false;
            if (task.Leave)
                task.Leave();
            if (status == LUA_YIELD)
            {
                task.WakeAt = 0.0;
                if (results > 0 && lua_isinteger(task.Thread, -1))
                    task.WakeAt = Clock() + static_cast<double>(std::clamp<lua_Integer>(lua_tointeger(task.Thread, -1), 0, 600000)) / 1000.0;
                lua_pop(task.Thread, results);
                continue;
            }
            FinishTask(task, status, results);
        }
        t_engine = previous;
    }

    void LuaEngine::FinishTask(Task& task, int status, int results)
    {
        Status outcome = Status::Success();
        std::string output;
        if (status == LUA_OK)
        {
            if (results > 0 && lua_type(task.Thread, -results) == LUA_TSTRING)
                output = lua_tostring(task.Thread, -results);
        }
        else
        {
            const char* text = lua_tostring(task.Thread, -1);
            luaL_traceback(m_state, task.Thread, text ? text : "error", 0);
            const std::string message = lua_tostring(m_state, -1) ? lua_tostring(m_state, -1) : "error";
            lua_pop(m_state, 1);
            Print(OutputKind::Error, message);
            LOG_ERROR("Lua", ErrorMessage(ErrorCode::LuaRuntime, FirstLine(message)));
            outcome = Status::Fail(ErrorCode::LuaRuntime, FirstLine(message));
        }
        TaskDone done = std::move(task.Done);
        const int id = task.Id;
        luaL_unref(m_state, LUA_REGISTRYINDEX, task.Reference);
        m_tasks.erase(std::remove_if(m_tasks.begin(), m_tasks.end(), [id](const auto& entry) { return entry->Id == id; }), m_tasks.end());
        if (done)
            done(outcome, output);
    }

    bool LuaEngine::TaskAlive(int id) const
    {
        return id > 0 && std::any_of(m_tasks.begin(), m_tasks.end(), [id](const auto& task) { return task->Id == id; });
    }

    void LuaEngine::StopTask(int id)
    {
        for (const auto& task : m_tasks)
        {
            if (task->Id == id)
            {
                task->Stop = true;
                task->WakeAt = 0.0;
            }
        }
    }

    bool LuaEngine::IsCooperative(lua_State* thread) const
    {
        if (!m_taskStack.empty() && m_taskStack.back()->Thread == thread)
            return true;
        return IsCooperativeThread(thread);
    }
}
