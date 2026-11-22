#pragma once

#include "Table/AddressList.h"

#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Table
{
    // A record named by its path: the descriptions of the groups above it, top down, and its own. Tables repeat names on
    // purpose ("Multiplier:", "[Debug]" below every script) and ids change when records are copied between tables, but the
    // path stays as long as the tree does. Descriptions are matched exactly, as the address list shows them.

    // The path of a record, from the top level down to the record itself.
    std::vector<std::string> RecordPath(const MemoryRecord& record);

    // Every record at the end of this path. The first step is matched among `from`'s children (the top level when `from` is
    // null); a name that several siblings have is followed down each of them, so only the full path has to be unique.
    std::vector<MemoryRecord*> FindByPath(AddressList& list, const std::vector<std::string>& path, MemoryRecord* from = nullptr);

    // Every record below `from` (at any depth) with this description, in table order.
    std::vector<MemoryRecord*> FindBelow(MemoryRecord& from, std::string_view description);

    // A path for messages and the Address List Debug window: the steps joined with " › ".
    std::string PathText(const std::vector<std::string>& path);
}
