#include "Lua/LuaCrash.h"

#include "App/CrashReport.h"
#include "App/Engine.h"
#include "Lua/LuaArgs.h"

#include "lua.hpp"

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

        const char* LinkName(CrashLink link)
        {
            switch (link)
            {
            case CrashLink::InCode: return "code";
            case CrashLink::InPatch: return "patch";
            case CrashLink::BehindPatch: return "behindPatch";
            case CrashLink::RanFreed: return "freed";
            case CrashLink::Unbalanced: return "unbalanced";
            case CrashLink::NearPatch: return "nearPatch";
            case CrashLink::Accessed: return "accessed";
            case CrashLink::OnStack: return "stack";
            case CrashLink::Earlier: return "earlier";
            }
            return "";
        }

        void PushStrings(lua_State* state, const std::vector<std::string>& values)
        {
            lua_createtable(state, static_cast<int>(values.size()), 0);
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                lua_pushlstring(state, values[i].data(), values[i].size());
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        int Last(lua_State* state)
        {
            const std::optional<CrashReport> report = Owner(state).LastCrash();
            if (!report)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_createtable(state, 0, 14);
            SetString(state, "process", report->Process);
            SetString(state, "headline", report->Headline);
            SetString(state, "details", report->Details);
            SetInteger(state, "exitCode", report->ExitCode);
            SetBool(state, "exact", report->Exact);
            SetBool(state, "watched", report->Watched);
            SetNumber(state, "secondsBeforeEnd", report->SecondsBeforeEnd);
            if (report->Fatal)
            {
                SetInteger(state, "code", report->Fatal->Code);
                SetInteger(state, "address", report->Fatal->Ip());
                SetInteger(state, "thread", report->Fatal->ThreadId);
                if (report->Fatal->Info.size() >= 2)
                    SetInteger(state, "accessed", report->Fatal->Info[1]);
            }
            lua_createtable(state, static_cast<int>(report->Blames.size()), 0);
            for (std::size_t i = 0; i < report->Blames.size(); ++i)
            {
                const CrashBlame& blame = report->Blames[i];
                lua_createtable(state, 0, 5);
                SetString(state, "script", blame.Label);
                if (blame.RecordId)
                    SetInteger(state, "recordId", static_cast<std::uint64_t>(blame.RecordId));
                SetString(state, "link", LinkName(blame.Link));
                SetString(state, "where", blame.Where);
                SetInteger(state, "address", blame.Address);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            lua_setfield(state, -2, "scripts");
            PushStrings(state, report->SwitchedOn);
            lua_setfield(state, -2, "switchedOn");
            if (!report->LastSwitched.empty())
            {
                SetString(state, "lastSwitched", report->LastSwitched);
                SetNumber(state, "lastSwitchedSeconds", report->LastSwitchedSeconds);
            }
            return 1;
        }

        int Watching(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).Crashes().Running() ? 1 : 0);
            return 1;
        }

        int Start(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.HasProcess())
                return PushFailure(state, ErrorCode::NoProcess);
            if (const Status started = engine.Crashes().Running() ? Status::Success() : engine.Crashes().Start(); !started)
                return PushFailure(state, started);
            lua_pushboolean(state, 1);
            return 1;
        }

        int Scripts(lua_State* state)
        {
            const std::vector<ScriptFootprint> footprints = Owner(state).ScriptLedger().Footprints();
            lua_createtable(state, static_cast<int>(footprints.size()), 0);
            for (std::size_t i = 0; i < footprints.size(); ++i)
            {
                const ScriptFootprint& footprint = footprints[i];
                lua_createtable(state, 0, 5);
                SetString(state, "script", footprint.Label);
                if (footprint.RecordId)
                    SetInteger(state, "recordId", static_cast<std::uint64_t>(footprint.RecordId));
                SetBool(state, "on", footprint.OffTick == 0);
                lua_createtable(state, static_cast<int>(footprint.Ranges.size()), 0);
                for (std::size_t r = 0; r < footprint.Ranges.size(); ++r)
                {
                    const ScriptRange& range = footprint.Ranges[r];
                    lua_createtable(state, 0, 4);
                    SetInteger(state, "address", range.Begin);
                    SetInteger(state, "size", range.End - range.Begin);
                    SetString(state, "kind", range.Kind == ScriptRangeKind::Allocation ? "alloc" : "patch");
                    if (!range.Name.empty())
                        SetString(state, "name", range.Name);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(r + 1));
                }
                lua_setfield(state, -2, "ranges");
                lua_createtable(state, static_cast<int>(footprint.Warnings.size()), 0);
                for (std::size_t w = 0; w < footprint.Warnings.size(); ++w)
                {
                    lua_createtable(state, 0, 2);
                    SetInteger(state, "line", static_cast<std::uint64_t>(footprint.Warnings[w].Line));
                    SetString(state, "text", footprint.Warnings[w].Text);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(w + 1));
                }
                lua_setfield(state, -2, "warnings");
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }
    }

    void RegisterCrashNatives(lua_State* state, int natives)
    {
        const int table = lua_absindex(state, natives);
        const luaL_Reg functions[] = {
            { "crashLast", &Last },
            { "crashWatching", &Watching },
            { "crashStart", &Start },
            { "crashScripts", &Scripts },
            { nullptr, nullptr },
        };
        lua_pushvalue(state, table);
        luaL_setfuncs(state, functions, 0);
        lua_pop(state, 1);
    }
}
