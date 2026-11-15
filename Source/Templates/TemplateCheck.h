#pragma once

#include "Templates/TemplateLibrary.h"

#include <string>
#include <vector>

struct lua_State;

namespace Manifold::Templates
{
    struct CheckEntry
    {
        std::string Id;
        std::string Caption;
        std::string Source;
        std::vector<std::string> Errors;
        std::vector<std::string> Warnings;
    };

    CheckEntry CheckTemplate(lua_State* state, const Library& library, const TemplateInfo& info, const std::string* script = nullptr);
    std::vector<CheckEntry> CheckAll(lua_State* state, const Library& library);
    std::vector<std::string> HelperNames();
}
