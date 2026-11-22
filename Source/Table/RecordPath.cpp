#include "Table/RecordPath.h"

#include <algorithm>
#include <functional>

namespace Manifold::Table
{
    std::vector<std::string> RecordPath(const MemoryRecord& record)
    {
        std::vector<std::string> path;
        for (const MemoryRecord* at = &record; at; at = at->Parent)
            path.push_back(at->Description);
        std::reverse(path.begin(), path.end());
        return path;
    }

    std::vector<MemoryRecord*> FindByPath(AddressList& list, const std::vector<std::string>& path, MemoryRecord* from)
    {
        std::vector<MemoryRecord*> found;
        if (path.empty())
            return found;
        const std::function<void(const std::vector<std::unique_ptr<MemoryRecord>>&, std::size_t)> walk =
            [&](const std::vector<std::unique_ptr<MemoryRecord>>& level, std::size_t step)
            {
                for (const auto& record : level)
                {
                    if (record->Description != path[step])
                        continue;
                    if (step + 1 == path.size())
                        found.push_back(record.get());
                    else
                        walk(record->Children, step + 1);
                }
            };
        walk(from ? from->Children : list.Roots(), 0);
        return found;
    }

    std::vector<MemoryRecord*> FindBelow(MemoryRecord& from, std::string_view description)
    {
        std::vector<MemoryRecord*> found;
        const std::function<void(MemoryRecord&)> walk = [&](MemoryRecord& record)
            {
                for (const auto& child : record.Children)
                {
                    if (child->Description == description)
                        found.push_back(child.get());
                    walk(*child);
                }
            };
        walk(from);
        return found;
    }

    std::string PathText(const std::vector<std::string>& path)
    {
        std::string text;
        for (const std::string& step : path)
        {
            if (!text.empty())
                text += " › ";
            text += step.empty() ? std::string("(no description)") : step;
        }
        return text;
    }
}
