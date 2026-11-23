#include "Lua/LuaCodeFilter.h"

#include "App/CodeFilter.h"
#include "App/Engine.h"
#include "Core/Text.h"
#include "Lua/LuaArgs.h"

#include "lua.hpp"

#include <algorithm>
#include <string>

namespace Manifold::Lua
{
    namespace
    {
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

        void SetNumber(lua_State* state, const char* key, double value)
        {
            lua_pushnumber(state, value);
            lua_setfield(state, -2, key);
        }

        void SetBool(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        const char* RoundName(FilterRound round)
        {
            switch (round)
            {
            case FilterRound::FilterOut: return "filterOut";
            case FilterRound::KeepOnly: return "keepOnly";
            case FilterRound::Refine: return "refine";
            }
            return "";
        }

        void PushSources(lua_State* state, std::uint8_t sources)
        {
            lua_createtable(state, 4, 0);
            int index = 0;
            const auto add = [&](std::uint8_t bit, const char* name)
                {
                    if (!(sources & bit))
                        return;
                    lua_pushstring(state, name);
                    lua_rawseti(state, -2, ++index);
                };
            add(SourceUnwind, "unwind");
            add(SourceExport, "export");
            add(SourceCall, "call");
            add(SourceBlock, "block");
            add(SourcePointer, "pointer");
        }

        void PushRecord(lua_State* state, const RoundRecord& record)
        {
            lua_createtable(state, 0, 6);
            SetString(state, "kind", RoundName(record.Kind));
            SetInteger(state, "before", record.Before);
            SetInteger(state, "after", record.After);
            SetInteger(state, "hits", record.Hits);
            SetInteger(state, "skipped", record.Skipped);
            SetNumber(state, "seconds", record.Seconds);
        }

        int Prepare(lua_State* state)
        {
            CodeFilterOptions options;
            if (lua_istable(state, 1))
            {
                if (lua_getfield(state, 1, "modules") == LUA_TTABLE)
                {
                    const lua_Integer count = luaL_len(state, -1);
                    for (lua_Integer i = 1; i <= count; ++i)
                    {
                        lua_rawgeti(state, -1, i);
                        if (const char* name = lua_tostring(state, -1))
                            options.Modules.emplace_back(name);
                        lua_pop(state, 1);
                    }
                }
                lua_pop(state, 1);
                if (lua_getfield(state, 1, "unwind") != LUA_TNIL)
                    options.Unwind = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (lua_getfield(state, 1, "exports") != LUA_TNIL)
                    options.Exports = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (lua_getfield(state, 1, "pointers") != LUA_TNIL)
                    options.Pointers = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (lua_getfield(state, 1, "calls") != LUA_TNIL)
                    options.Calls = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
            }
            Engine& engine = Owner(state);
            if (!options.Calls && engine.Process() && engine.Process()->Is64Bit())
                options.Calls = engine.Options().CodeFilterCalls;
            const Result<CodeFilterReport> report = engine.Filter().Prepare(options);
            if (!report)
                return PushFailure(state, report.Error());
            lua_createtable(state, 0, 3);
            SetInteger(state, "total", report->Total);
            SetNumber(state, "seconds", report->Seconds);
            lua_createtable(state, static_cast<int>(report->Modules.size()), 0);
            for (std::size_t i = 0; i < report->Modules.size(); ++i)
            {
                const CodeModuleCount& count = report->Modules[i];
                lua_createtable(state, 0, 6);
                SetString(state, "name", count.Module);
                SetInteger(state, "functions", count.Total);
                SetInteger(state, "unwind", count.Unwind);
                SetInteger(state, "exports", count.Exports);
                SetInteger(state, "calls", count.Calls);
                SetInteger(state, "pointers", count.Pointers);
                SetInteger(state, "rejected", count.Rejected);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "modules");
            return 1;
        }

        int Start(lua_State* state)
        {
            const std::string kind = luaL_checkstring(state, 1);
            FilterRound round = FilterRound::FilterOut;
            if (EqualsNoCase(kind, "keepOnly"))
                round = FilterRound::KeepOnly;
            else if (!EqualsNoCase(kind, "filterOut"))
                return PushFailure(state, ErrorCode::LuaBadArgument, "the round is \"filterOut\" or \"keepOnly\", not \"" + kind + "\"");
            if (const Status started = Owner(state).Filter().Start(round); !started)
                return PushFailure(state, started);
            lua_pushboolean(state, 1);
            return 1;
        }

        int Stop(lua_State* state)
        {
            CodeFilter& filter = Owner(state).Filter();
            if (const Status stopped = filter.Stop(); !stopped)
                return PushFailure(state, stopped);
            const std::vector<RoundRecord> history = filter.History();
            if (history.empty())
                return PushFailure(state, ErrorCode::CodeFilterFailed, "the round left no record");
            PushRecord(state, history.back());
            return 1;
        }

        int Cancel(lua_State* state)
        {
            if (const Status cancelled = Owner(state).Filter().Cancel(); !cancelled)
                return PushFailure(state, cancelled);
            lua_pushboolean(state, 1);
            return 1;
        }

        int Undo(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).Filter().Undo() ? 1 : 0);
            return 1;
        }

        int Reset(lua_State* state)
        {
            Owner(state).Filter().Reset();
            lua_pushboolean(state, 1);
            return 1;
        }

        int Refine(lua_State* state)
        {
            const lua_Integer most = luaL_optinteger(state, 1, static_cast<lua_Integer>(CodeFilter::kMaxRefine));
            const Result<std::size_t> refined = Owner(state).Filter().Refine(static_cast<std::size_t>(std::clamp<lua_Integer>(most, 1, 4096)));
            if (!refined)
                return PushFailure(state, refined.Error());
            lua_pushinteger(state, static_cast<lua_Integer>(*refined));
            return 1;
        }

        int Candidates(lua_State* state)
        {
            const lua_Integer limit = std::clamp<lua_Integer>(luaL_optinteger(state, 1, 1000), 1, 1000000);
            Engine& engine = Owner(state);
            const std::vector<CodeCandidate> candidates = engine.Filter().Candidates();
            const std::size_t count = std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(limit));
            lua_createtable(state, static_cast<int>(count), 0);
            for (std::size_t i = 0; i < count; ++i)
            {
                const CodeCandidate& candidate = candidates[i];
                lua_createtable(state, 0, 4);
                SetInteger(state, "address", candidate.Address);
                SetString(state, "name", engine.Describe(candidate.Address));
                if (candidate.Function)
                    SetInteger(state, "function", candidate.Function);
                PushSources(state, candidate.Sources);
                lua_setfield(state, -2, "sources");
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int StatusOf(lua_State* state)
        {
            CodeFilter& filter = Owner(state).Filter();
            const auto running = filter.RunningKind();
            lua_createtable(state, 0, 7);
            SetBool(state, "prepared", filter.Prepared());
            SetBool(state, "running", running.has_value());
            if (running)
                SetString(state, "kind", RoundName(*running));
            SetInteger(state, "remaining", filter.Remaining());
            SetInteger(state, "total", filter.Report().Total);
            SetInteger(state, "liveHits", filter.LiveHits());
            SetNumber(state, "seconds", filter.RoundSeconds());
            return 1;
        }

        int History(lua_State* state)
        {
            const std::vector<RoundRecord> history = Owner(state).Filter().History();
            lua_createtable(state, static_cast<int>(history.size()), 0);
            for (std::size_t i = 0; i < history.size(); ++i)
            {
                PushRecord(state, history[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }
    }

    void RegisterCodeFilterNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "codefilterPrepare", &Prepare },
            { "codefilterStart", &Start },
            { "codefilterStop", &Stop },
            { "codefilterCancel", &Cancel },
            { "codefilterUndo", &Undo },
            { "codefilterReset", &Reset },
            { "codefilterRefine", &Refine },
            { "codefilterCandidates", &Candidates },
            { "codefilterStatus", &StatusOf },
            { "codefilterHistory", &History },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
