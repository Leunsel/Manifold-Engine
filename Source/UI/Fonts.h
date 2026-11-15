#pragma once

struct ImFont;

namespace Manifold::Fonts
{
    inline constexpr float kSize = 13.0f;
    inline constexpr float kMonoSize = 13.0f;

    bool Build(const char** error);

    ImFont* Bold();
    ImFont* Mono();
}
