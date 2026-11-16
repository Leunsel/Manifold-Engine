#pragma once

#include "Core/Errors.h"

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Manifold
{
    enum class TableScriptPolicy
    {
        Ask,
        Always,
        Never
    };

    enum class QuitPolicy
    {
        Ask,
        Disable,
        Leave
    };

    struct ThemeSettings
    {
        std::map<std::string, std::array<float, 4>> Colors;
        std::map<std::string, float> Sizes;
    };

    struct Settings
    {
        int X = 0;
        int Y = 0;
        int Width = 1180;
        int Height = 760;
        bool Placed = false;
        bool Maximised = false;

        int UpdateIntervalMs = 250;
        int FreezeIntervalMs = 100;
        int ScanThreads = 0;
        bool FastScan = true;
        bool PauseWhileScanning = false;
        bool ScanPrivate = true;
        bool ScanImage = true;
        bool ScanMapped = false;
        std::uint32_t AllTypes = 0x3F;
        int ResultValueRefreshMs = 500;

        bool AutorunEnabled = true;
        bool ScriptWindowsAppLook = true;
        std::vector<std::string> AutoAttach;
        bool AutoAttachEnabled = false;
        TableScriptPolicy TableScripts = TableScriptPolicy::Ask;
        QuitPolicy ActiveScriptsOnQuit = QuitPolicy::Ask;

        bool CompactMode = false;
        bool ReleaseMode = false;

        bool VerboseLog = false;
        bool LogDebugStrings = true;
        bool UseHardwareExecuteBreakpoints = false;
        bool UseInProcessDebugger = false;
        bool ShowRegistersOnBreak = true;
        int DisassemblerRows = 0;

        std::vector<std::string> RecentTables;
        std::string LastProcess;
        ThemeSettings Theme;
        std::map<std::string, std::string> Shortcuts;

        bool Load(const std::filesystem::path& path);
        Status Save(const std::filesystem::path& path) const;
        void AddRecentTable(const std::string& path);
    };

    namespace Paths
    {
        std::filesystem::path ProgramFolder();
        std::filesystem::path SettingsFile();
        std::filesystem::path ScriptSettingsFile();
        std::filesystem::path LogFile();
        std::filesystem::path LayoutFile();
        std::filesystem::path AutorunFolder();
        std::filesystem::path TablesFolder();
        std::filesystem::path CrashFolder();
        std::filesystem::path ScanFolder();
        void AdoptEarlierFiles();
    }
}
