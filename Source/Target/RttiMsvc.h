#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Target
{
    bool IsMsvcTypeName(std::string_view decorated);
    std::string MsvcDemangle(std::string_view decorated);
    std::vector<std::string> MsvcMangle(std::string_view name);
}
