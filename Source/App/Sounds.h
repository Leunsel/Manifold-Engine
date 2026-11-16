#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace Manifold::Sounds
{
    enum class Cue
    {
        Success,
        Error
    };

    void SetEnabled(bool enabled);
    bool Enabled();
    void Play(Cue cue);
    void Preview(Cue cue);
    void Stop();
    const char* FileName(Cue cue);

    std::vector<std::uint8_t> DecodeToWave(const std::vector<std::uint8_t>& encoded);
}
