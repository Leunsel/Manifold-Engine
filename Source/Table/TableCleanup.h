#pragma once

#include "Core/Errors.h"
#include "Core/Types.h"
#include "Table/CheatTable.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Table
{
    enum class CleanupKind
    {
        Structure,       // a saved Structure Dissect definition nothing names
        Symbol,          // a user-defined symbol nothing uses
        File,            // a table file no script, Lua or other file names
        EmptyGroup,      // a group without records, Lua or a reference
        DisasmComments,  // the disassembler comments and headers
        TableComment,    // the table's own comment (Table Extras > Comments)
        ScriptComments,  // // and { } comments in Auto Assembler scripts
        FileComments     // the same in .CEA/.AA table files
    };

    const char* CleanupKindName(CleanupKind kind);

    struct CleanupItem
    {
        CleanupKind Kind = CleanupKind::Structure;
        std::string Name;    // the structure, symbol, file or group; empty for the whole-table kinds
        std::string Reason;
        std::size_t Bytes = 0;  // about what it takes in the saved file
        bool Recommended = true;
        int RecordId = -1;   // EmptyGroup: the group's ID in the file
        std::string Key;     // kind:name, stable between plans
    };

    struct CleanupPlan
    {
        std::vector<CleanupItem> Items;
        std::size_t OriginalBytes = 0;
    };

    // ∑ Only what nothing names is offered. "Names" is a whole-word search, case
    //   ignored, through every script, Lua event, the table's Lua, the text
    //   table files, the records' addresses and offsets and the structures'
    //   field types; a name built at run time from pieces is missed, which is
    //   why every item is listed for a look before it goes.
    CleanupPlan PlanCleanup(const LoadedTable& table, std::size_t originalBytes);

    // The table text without the chosen items; the text it was given is not changed.
    Result<std::string> CleanTableText(std::string_view original, const std::vector<CleanupItem>& chosen);

    // // and { } comments out of an Auto Assembler script: not {$...}
    // directives, not inside quotes, not in {$lua} parts. Blank runs shrink to one line.
    std::string StripAutoAssemblerComments(std::string_view script);

    // Whether `name` occurs in `text` as a word of its own, case ignored.
    bool NamedIn(std::string_view text, std::string_view name);
}
