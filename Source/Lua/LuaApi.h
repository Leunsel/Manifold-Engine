#pragma once

#include "Disasm/Disassembler.h"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace Manifold::Table
{
    struct MemoryRecord;
    struct StructDefinition;
}

namespace Manifold::Lua
{
    class LuaEngine;

    LuaEngine& EngineOf(lua_State* state);

    void RegisterApi(LuaEngine& engine, lua_State* state);
    void RegisterMonoApi(LuaEngine& engine, lua_State* state);
    void RegisterObjects(LuaEngine& engine, lua_State* state);
    void RegisterCompat(LuaEngine& engine, lua_State* state);
    void RunCompatScripts(LuaEngine& engine);
    std::vector<std::string> ApiFunctionNames();
    void NoteFunction(const char* name);

    void PushTimer(lua_State* state, int id);
    void PushRecord(lua_State* state, const Table::MemoryRecord* record);
    void PushAddressList(lua_State* state);
    void PushStructureObject(lua_State* state, Table::StructDefinition* definition);
    bool PushExtra(lua_State* state, int id, std::string_view key);
    std::vector<int> RecordsWithFunction(lua_State* state, std::string_view key);
    void DropExtras(lua_State* state, int id);
    void ForgetRecords(lua_State* state);
    void PushBound(lua_State* state, int objectIndex);
    inline constexpr int kAddressListExtras = -1;
    void ResumeThreads(LuaEngine& engine, lua_State* state, double now);
    bool IsCooperativeThread(lua_State* thread);
    void PushRegisters(lua_State* state, const Disasm::RegisterValues& registers, bool is64Bit);
    bool PullRegisters(lua_State* state, Disasm::RegisterValues& registers, bool is64Bit);
}
