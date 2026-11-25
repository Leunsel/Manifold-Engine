#pragma once

#include <span>
#include <string_view>

namespace Manifold
{
    struct CreditEntry
    {
        std::string_view Name;
        std::string_view For;    // what they gave, shown muted beside the name
    };

    // ∑ The one list of credits. About > Credits shows it in this order; a new
    // entry is one more line here and nothing else (the core suite checks
    // the first entries' order and that every entry has a name and a reason).
    inline constexpr CreditEntry kCredits[] = {
        { "Leunsel", "Author" },
        { "cfemen", "SysCalls" },
        { "Veydr", "Bug-Hunts" },
        { "ocornut", "ImGui" },
        { "zyantific", "Zydis" },
    };

    inline std::span<const CreditEntry> Credits()
    {
        return kCredits;
    }
}
