#pragma once

#include "App/Settings.h"

#include <filesystem>

namespace Manifold
{
    class SettingsStore
    {
    public:
        SettingsStore(Settings& settings, std::filesystem::path path) : m_settings(settings), m_path(std::move(path)) {}

        void Changed(double now) { m_changedAt = now; }
        bool Pending() const { return m_changedAt >= 0.0; }
        bool Due(double now) const { return Pending() && now - m_changedAt >= kQuietSeconds; }
        Status Flush();

    private:
        static constexpr double kQuietSeconds = 0.75;

        Settings& m_settings;
        std::filesystem::path m_path;
        double m_changedAt = -1.0;
    };
}
