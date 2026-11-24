#pragma once

#include "Scan/TypeCatalog.h"

#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

namespace Manifold::Lua
{
    class LuaEngine;
}

namespace Manifold::Table
{
    struct MemoryRecord;
}

namespace Manifold::UI
{
    std::vector<Scan::CustomTypeInfo> CustomTypes(const Lua::LuaEngine& lua);
    Scan::TypeChoice RecordTypeChoice(const Table::MemoryRecord& record);
    // A built-in type clears the record's custom type; a custom type keeps the built-in one underneath, like CE.
    void SetRecordType(Table::MemoryRecord& record, const Scan::TypeChoice& choice);

    // The value type combo used by the scanner, the address list and the dissect: types under headings, sizes on the
    // right, a filter field once the list is long. The catalog is only built while the combo is open.
    bool TypeCombo(const char* id, Scan::TypeChoice& choice, const std::function<Scan::TypeCatalogOptions()>& options, ImGuiComboFlags flags = ImGuiComboFlags_HeightLarge);
    // The same list inside a combo the caller opened. True when the choice changed.
    bool TypeComboItems(const std::vector<Scan::TypeSection>& sections, Scan::TypeChoice& choice);
    // The list as menu items inside an open menu; detail fills the shortcut column (defaults to the entry's size).
    // Returns the clicked entry, which lives in sections.
    const Scan::TypeEntry* TypeMenuItems(const std::vector<Scan::TypeSection>& sections, const Scan::TypeChoice* current,
        const std::function<std::string(const Scan::TypeEntry&)>& detail = {});
}
