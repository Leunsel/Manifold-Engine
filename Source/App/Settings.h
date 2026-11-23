#pragma once

#include "Core/Errors.h"
#include "Core/Paths.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <variant>
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

    enum class ThemeFamily
    {
        Standard,
        Red,
        Green,
        Blue
    };

    enum class ThemeMode
    {
        Dark,
        Light,
        System
    };

    inline constexpr ThemeFamily kThemeFamilies[] = { ThemeFamily::Standard, ThemeFamily::Red, ThemeFamily::Green, ThemeFamily::Blue };

    const char* ThemeFamilyKey(ThemeFamily family);
    const char* ThemeModeKey(ThemeMode mode);

    struct ThemeSettings
    {
        ThemeFamily Family = ThemeFamily::Standard;
        ThemeMode Mode = ThemeMode::Dark;
        int Corners = 0;
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
        int AddressColumn = 2;
        int AddressDigits = 0;

        bool AutorunEnabled = true;
        bool HoldNewModules = true;
        bool ScriptWindowsAppLook = true;
        bool ScriptEditorTabs = true;
        bool StructureTabs = true;
        bool StructureRtti = true;
        int ScriptCallbackLimitMs = 2000;
        bool LuaShowOnPrint = true;
        bool LuaWrapOutput = true;
        bool LegacyCodeEditor = false;
        bool LuaConsoleTime = true;
        bool LuaConsoleHex = false;
        bool LuaDocsPanel = false;
        bool AutoAssemblerLog = false;
        std::vector<std::string> AutoAttach;
        bool AutoAttachEnabled = false;
        TableScriptPolicy TableScripts = TableScriptPolicy::Ask;
        QuitPolicy ActiveScriptsOnQuit = QuitPolicy::Ask;
        int TableFileNames = 0;

        bool BackupsEnabled = true;
        int BackupIntervalMinutes = 5;
        int BackupVersions = 20;
        std::string BackupFolder;

        bool CompactMode = false;
        int ActiveWindowOpacity = 100;
        int InactiveWindowOpacity = 100;
        bool PlaySounds = true;
        bool ReleaseMode = false;
        bool ProfilerEnabled = false;
        bool LayoutChecks = false;
        bool ProfilerInStatusBar = true;
        bool ShowSplash = true;
        int NopSledBest = 0;

        bool VerboseLog = false;
        bool LogDebugStrings = true;
        bool UseHardwareExecuteBreakpoints = false;
        bool UseInProcessDebugger = false;
        bool ShowRegistersOnBreak = true;
        int BreakOnException = 0;
        std::string ExceptionRegionStart;
        std::string ExceptionRegionStop;
        bool StackFollowPointers = true;
        bool StackPointerTargets = false;
        bool StackCollapseZeros = true;
        int RegisterSections = 7;
        int VectorFormat = 0;
        bool RegisterEditing = true;
        int StackOffsets = 0;
        bool ShowSymbols = true;
        bool MemoryHexView = true;
        int MemoryCodeShare = 620;
        int DisassemblerRows = 0;
        bool AutoExplorer = true;

        std::vector<std::string> RecentTables;
        std::string LastProcess;
        ThemeSettings Theme;
        std::map<std::string, std::string> Shortcuts;

        bool Load(const std::filesystem::path& path);
        Status Save(const std::filesystem::path& path) const;
        void AddRecentTable(const std::string& path);
    };

    struct SettingChoice
    {
        std::vector<const char*> Names;
        int (*Get)(const Settings&);
        void (*Set)(Settings&, int);
    };

    using SettingMember = std::variant<bool Settings::*, int Settings::*, std::uint32_t Settings::*, std::string Settings::*, std::vector<std::string> Settings::*, const SettingChoice*>;

    struct SettingField
    {
        const char* Section;
        const char* Key;
        SettingMember Member;
        int Min = std::numeric_limits<int>::min();
        int Max = std::numeric_limits<int>::max();
        bool OnlyWhenChanged = false;
    };

    std::span<const SettingField> SettingFields();
}
