#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Manifold
{
    namespace Paths
    {
        struct Migration
        {
            std::vector<std::string> Moved;
            std::vector<std::string> Problems;
        };

        std::filesystem::path ProgramFolder();
        std::filesystem::path DataFolder();
        void SetDataFolder(std::filesystem::path folder);
        std::filesystem::path ConfigFolder();
        std::filesystem::path SettingsFile();
        std::filesystem::path ScriptSettingsFile();
        std::filesystem::path LogFolder();
        std::filesystem::path LogFile();
        std::filesystem::path LayoutFile();
        std::filesystem::path AutorunFolder();
        std::filesystem::path SoundsFolder();
        std::filesystem::path TablesFolder();
        std::filesystem::path CrashFolder();
        std::filesystem::path ScanFolder();
        Migration AdoptEarlierFiles(const std::filesystem::path& programFolder);
        std::vector<std::string> LeftoverFiles(const std::filesystem::path& programFolder);
        std::string CurrentTablePath(const std::string& path, const std::filesystem::path& programFolder);
    }
}
