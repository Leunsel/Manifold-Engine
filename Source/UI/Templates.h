#pragma once

#include "App/Engine.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Manifold::UI
{
    enum class TemplateKind
    {
        AobInjection,
        CodeInjection,
        FullInjection,
        CheatTable,
        LuaCall
    };

    inline constexpr const char* kTemplateNames[] = { "AOB injection", "Code injection", "Full injection", "Cheat table framework", "Call Lua" };

    std::optional<std::string> MakeTemplate(Engine& engine, TemplateKind kind, std::uint64_t address, const std::string& name, std::string& error);
}
