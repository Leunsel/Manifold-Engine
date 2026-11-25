#pragma once

#include <filesystem>
#include <string_view>

namespace Manifold
{
    // ∑ The one way files that matter are written (tables, settings, themes, module
    //   state, exports): into a temporary file of this write alone, flushed to the disk,
    //   then renamed over the target. A crash or power loss at any point leaves the
    //   old file or the new one, never a cut one; two writers of the same file (two
    //   instances, a background save and a copy) cannot cut each other's file either.
    //   With replace false an existing file is kept and the write fails.
    bool WriteFileSafely(const std::filesystem::path& path, std::string_view bytes, bool replace = true);
}
