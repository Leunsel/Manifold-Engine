#pragma once

#include "Table/AddressList.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Table
{
    // The table checks of the Address List Debug window: everything that can be found from the table alone, without a process.
    // What needs the process (addresses that don't resolve, scripts that don't assemble) the window adds on top.

    // Record IDs start at 0, so the table itself is -1 where a record id is expected.
    constexpr int kTableLevel = -1;

    enum class IssueLevel
    {
        Error,
        Warning,
        Info
    };

    struct RecordIssue
    {
        int RecordId = kTableLevel;  // kTableLevel for the table itself (its Lua script, its files)
        IssueLevel Level = IssueLevel::Warning;
        std::string Check;         // short name of the check, such as "dropdown-link"; the window groups and filters by it
        std::string Text;
    };

    // Where Lua code names a record with a literal: getMemoryRecordByDescription("Health"), getMemoryRecordByID(12),
    // manifold.record.find("Health") and the AddressList methods of the same names.
    struct RecordReference
    {
        int FromRecord = kTableLevel;  // kTableLevel for the table's Lua script or a table file
        std::string Where;         // "Lua script of the table", "script", "On Activate", "file helpers.lua"
        int Line = 0;
        std::string Function;
        std::string Target;        // the description or the id as written
        bool ById = false;
        int TargetId = -1;         // the record found, -1 when none
    };

    struct CheckHooks
    {
        // Answers whether a custom type of that name is registered; without it the custom type check is skipped.
        std::function<bool(std::string_view name)> CustomTypeExists;
    };

    std::vector<RecordReference> FindRecordReferences(AddressList& list);
    std::vector<RecordReference> FindRecordReferencesIn(std::string_view code);

    // Every issue in table order; issues of the same record keep the order of the checks.
    std::vector<RecordIssue> CheckRecords(AddressList& list, const CheckHooks& hooks = {});

    const char* IssueLevelName(IssueLevel level);

    // The DropDownListLink as TableRuntime::DropDownOf reads it, without the quotes.
    std::string DropDownLinkTarget(const MemoryRecord& record);

    // Records whose DropDownListLink names this record.
    std::vector<MemoryRecord*> RecordsLinkingTo(AddressList& list, const MemoryRecord& record);

    // How many records have exactly this description; lookups by description find the first one in table order.
    int CountWithDescription(AddressList& list, std::string_view description);
}
