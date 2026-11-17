#pragma once

#include "Lua/DocRecord.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Manifold::Lua
{
    struct CeProperty
    {
        std::string Type;
        std::string Text;
        std::vector<std::string> Choices;
    };

    std::vector<std::string> SplitTopLevel(std::string_view text, std::string_view separators, bool angles);
    std::size_t TypeEnd(std::string_view text, std::size_t from);
    std::string NormalizeType(std::string_view type);
    void ApplyTypeDetails(DocParam& param);
    DocSignature ParseLooseParams(std::string_view params);
    CeProperty ParseCeProperty(std::string_view text);
    bool ParseFunType(std::string_view text, DocSignature& signature);
}
