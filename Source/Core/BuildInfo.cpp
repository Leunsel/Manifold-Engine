#include "Core/BuildInfo.h"

#if __has_include("ManifoldBuild.h")
#include "ManifoldBuild.h"
#endif

#ifndef MANIFOLD_BUILD_COMMIT
#define MANIFOLD_BUILD_COMMIT ""
#define MANIFOLD_BUILD_DIRTY 0
#endif

namespace Manifold
{
    std::string BuildCommit()
    {
        std::string commit = MANIFOLD_BUILD_COMMIT;
        if (!commit.empty() && MANIFOLD_BUILD_DIRTY)
            commit += "+";
        return commit;
    }

    std::string VersionText()
    {
        const std::string commit = BuildCommit();
        return commit.empty() ? std::string(MANIFOLD_VERSION_TEXT) : std::string(MANIFOLD_VERSION_TEXT) + " (" + commit + ")";
    }
}
