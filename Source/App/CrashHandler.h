#pragma once

#include <filesystem>
#include <optional>

namespace Manifold::CrashHandler
{
    void Install(const std::filesystem::path& folder);
    std::optional<std::filesystem::path> TakeLastCrash(const std::filesystem::path& folder);
}
