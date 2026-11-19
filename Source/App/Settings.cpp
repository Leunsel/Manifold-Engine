#include "App/Settings.h"

#include "Core/Ini.h"
#include "Core/Text.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>

#include <windows.h>

namespace Manifold
{
    namespace
    {
        constexpr std::size_t kRecentTables = 10;

        bool ToBool(const std::string& value)
        {
            return value == "1" || EqualsNoCase(value, "true") || EqualsNoCase(value, "yes");
        }

        int ToInt(const std::string& value, int fallback)
        {
            const auto parsed = ParseDecimal(value);
            return parsed ? static_cast<int>(*parsed) : fallback;
        }
    }

    const char* ThemeFamilyKey(ThemeFamily family)
    {
        switch (family)
        {
        case ThemeFamily::Red: return "red";
        case ThemeFamily::Green: return "green";
        case ThemeFamily::Blue: return "blue";
        default: return "standard";
        }
    }

    const char* ThemeModeKey(ThemeMode mode)
    {
        switch (mode)
        {
        case ThemeMode::Light: return "light";
        case ThemeMode::System: return "system";
        default: return "dark";
        }
    }

    bool Settings::Load(const std::filesystem::path& path)
    {
        const std::optional<std::string> content = Ini::ReadText(path);
        if (!content)
            return false;
        std::map<std::string, std::string> values;
        std::map<std::string, std::array<float, 4>> themeColors;
        std::map<std::string, float> themeSizes;
        std::map<std::string, std::string> shortcuts;
        std::vector<Ini::Entry> entries;
        for (Ini::Section& section : Ini::Parse(*content))
            entries.insert(entries.end(), std::make_move_iterator(section.Entries.begin()), std::make_move_iterator(section.Entries.end()));
        for (const Ini::Entry& entry : entries)
        {
            const std::string key = Trim(entry.Key);
            const std::string value = Trim(entry.Value);
            if (key.size() > 6 && EqualsNoCase(key.substr(0, 6), "Color."))
            {
                const std::vector<std::string> parts = Split(value, ',');
                std::array<float, 4> rgba{ 0.0f, 0.0f, 0.0f, 1.0f };
                bool ok = parts.size() == 3 || parts.size() == 4;
                for (std::size_t i = 0; ok && i < parts.size(); ++i)
                {
                    const auto number = ParseDouble(Trim(parts[i]));
                    ok = number.has_value();
                    if (ok)
                        rgba[i] = std::clamp(static_cast<float>(*number), 0.0f, 1.0f);
                }
                if (ok)
                    themeColors[key.substr(6)] = rgba;
                continue;
            }
            if (key.size() > 5 && EqualsNoCase(key.substr(0, 5), "Size."))
            {
                if (const auto number = ParseDouble(value))
                    themeSizes[key.substr(5)] = static_cast<float>(*number);
                continue;
            }
            if (key.size() > 4 && EqualsNoCase(key.substr(0, 4), "Key."))
            {
                shortcuts[key.substr(4)] = value;
                continue;
            }
            values[Lower(key)] = value;
        }
        Theme.Colors = std::move(themeColors);
        Theme.Sizes = std::move(themeSizes);
        Shortcuts = std::move(shortcuts);
        const auto text = [&](const char* key) -> const std::string*
            {
                const auto found = values.find(key);
                return found == values.end() ? nullptr : &found->second;
            };
        if (auto v = text("x")) X = ToInt(*v, X);
        if (auto v = text("y")) Y = ToInt(*v, Y);
        if (auto v = text("width")) Width = ToInt(*v, Width);
        if (auto v = text("height")) Height = ToInt(*v, Height);
        if (auto v = text("placed")) Placed = ToBool(*v);
        if (auto v = text("maximised")) Maximised = ToBool(*v);
        if (auto v = text("updateinterval")) UpdateIntervalMs = std::clamp(ToInt(*v, UpdateIntervalMs), 16, 5000);
        if (auto v = text("freezeinterval")) FreezeIntervalMs = std::clamp(ToInt(*v, FreezeIntervalMs), 10, 5000);
        if (auto v = text("scanthreads")) ScanThreads = std::clamp(ToInt(*v, 0), 0, 64);
        if (auto v = text("fastscan")) FastScan = ToBool(*v);
        if (auto v = text("pausewhilescanning")) PauseWhileScanning = ToBool(*v);
        if (auto v = text("scanprivate")) ScanPrivate = ToBool(*v);
        if (auto v = text("scanimage")) ScanImage = ToBool(*v);
        if (auto v = text("scanmapped")) ScanMapped = ToBool(*v);
        if (auto v = text("alltypes")) AllTypes = static_cast<std::uint32_t>(std::clamp(ToInt(*v, 0x3F), 0, 0x3F));
        if (auto v = text("resultrefresh")) ResultValueRefreshMs = std::clamp(ToInt(*v, ResultValueRefreshMs), 50, 10000);
        if (auto v = text("autorun")) AutorunEnabled = ToBool(*v);
        if (auto v = text("scriptwindowsapplook")) ScriptWindowsAppLook = ToBool(*v);
        if (auto v = text("scriptcallbacklimit")) ScriptCallbackLimitMs = std::clamp(ToInt(*v, ScriptCallbackLimitMs), 250, 60000);
        if (auto v = text("showonprint")) LuaShowOnPrint = ToBool(*v);
        if (auto v = text("wrapoutput")) LuaWrapOutput = ToBool(*v);
        if (auto v = text("legacyeditor")) LegacyCodeEditor = ToBool(*v);
        if (auto v = text("consoletime")) LuaConsoleTime = ToBool(*v);
        if (auto v = text("consolehex")) LuaConsoleHex = ToBool(*v);
        if (auto v = text("docspanel")) LuaDocsPanel = ToBool(*v);
        if (auto v = text("autoattach")) AutoAttach = Split(*v, ';');
        if (auto v = text("autoattachenabled")) AutoAttachEnabled = ToBool(*v);
        if (auto v = text("tablescripts")) TableScripts = EqualsNoCase(*v, "always") ? TableScriptPolicy::Always : EqualsNoCase(*v, "never") ? TableScriptPolicy::Never : TableScriptPolicy::Ask;
        if (auto v = text("activescriptsonquit")) ActiveScriptsOnQuit = EqualsNoCase(*v, "disable") ? QuitPolicy::Disable : EqualsNoCase(*v, "leave") ? QuitPolicy::Leave : QuitPolicy::Ask;
        if (auto v = text("compact")) CompactMode = ToBool(*v);
        if (auto v = text("playsounds")) PlaySounds = ToBool(*v);
        if (auto v = text("mode")) ReleaseMode = EqualsNoCase(*v, "release");
        if (auto v = text("profiler")) ProfilerEnabled = ToBool(*v);
        if (auto v = text("profilerstatusbar")) ProfilerInStatusBar = ToBool(*v);
        if (auto v = text("splash")) ShowSplash = ToBool(*v);
        if (auto v = text("nopsledbest")) NopSledBest = std::max(0, ToInt(*v, 0));
        if (auto v = text("verboselog")) VerboseLog = ToBool(*v);
        if (auto v = text("logdebugstrings")) LogDebugStrings = ToBool(*v);
        if (auto v = text("hardwareexecute")) UseHardwareExecuteBreakpoints = ToBool(*v);
        if (auto v = text("inprocess")) UseInProcessDebugger = ToBool(*v);
        if (auto v = text("registersonbreak")) ShowRegistersOnBreak = ToBool(*v);
        if (auto v = text("showsymbols")) ShowSymbols = ToBool(*v);
        if (auto v = text("autoexplorer")) AutoExplorer = ToBool(*v);
        if (auto v = text("lastprocess")) LastProcess = *v;
        Theme.Family = ThemeFamily::Standard;
        Theme.Mode = ThemeMode::Dark;
        if (auto v = text("palette"))
        {
            for (ThemeFamily family : kThemeFamilies)
            {
                if (EqualsNoCase(*v, ThemeFamilyKey(family)))
                    Theme.Family = family;
            }
        }
        if (auto v = text("appearance"))
        {
            for (ThemeMode mode : { ThemeMode::Dark, ThemeMode::Light, ThemeMode::System })
            {
                if (EqualsNoCase(*v, ThemeModeKey(mode)))
                    Theme.Mode = mode;
            }
        }
        RecentTables.clear();
        for (std::size_t i = 0; i < kRecentTables; ++i)
        {
            if (auto v = text(("recent" + std::to_string(i)).c_str()); v && !v->empty())
                RecentTables.push_back(*v);
        }
        return true;
    }

    Status Settings::Save(const std::filesystem::path& path) const
    {
        std::ostringstream out;
        out << "[Window]\n";
        out << "X=" << X << "\nY=" << Y << "\nWidth=" << Width << "\nHeight=" << Height << "\n";
        out << "Placed=" << (Placed ? 1 : 0) << "\nMaximised=" << (Maximised ? 1 : 0) << "\n\n";
        out << "[Scanner]\n";
        out << "UpdateInterval=" << UpdateIntervalMs << "\nFreezeInterval=" << FreezeIntervalMs << "\nScanThreads=" << ScanThreads << "\n";
        out << "FastScan=" << (FastScan ? 1 : 0) << "\nPauseWhileScanning=" << (PauseWhileScanning ? 1 : 0) << "\n";
        out << "ScanPrivate=" << (ScanPrivate ? 1 : 0) << "\nScanImage=" << (ScanImage ? 1 : 0) << "\nScanMapped=" << (ScanMapped ? 1 : 0) << "\n";
        out << "AllTypes=" << AllTypes << "\n";
        out << "ResultRefresh=" << ResultValueRefreshMs << "\n\n";
        out << "[Scripts]\n";
        out << "Autorun=" << (AutorunEnabled ? 1 : 0) << "\nAutoAttach=" << Join(AutoAttach, ";") << "\nAutoAttachEnabled=" << (AutoAttachEnabled ? 1 : 0) << "\n";
        out << "ScriptWindowsAppLook=" << (ScriptWindowsAppLook ? 1 : 0) << "\n";
        out << "ScriptCallbackLimit=" << ScriptCallbackLimitMs << "\n";
        out << "ShowOnPrint=" << (LuaShowOnPrint ? 1 : 0) << "\n";
        out << "WrapOutput=" << (LuaWrapOutput ? 1 : 0) << "\n";
        if (LegacyCodeEditor)
            out << "LegacyEditor=1\n";
        out << "ConsoleTime=" << (LuaConsoleTime ? 1 : 0) << "\nConsoleHex=" << (LuaConsoleHex ? 1 : 0) << "\nDocsPanel=" << (LuaDocsPanel ? 1 : 0) << "\n";
        out << "TableScripts=" << (TableScripts == TableScriptPolicy::Always ? "always" : TableScripts == TableScriptPolicy::Never ? "never" : "ask") << "\n";
        out << "ActiveScriptsOnQuit=" << (ActiveScriptsOnQuit == QuitPolicy::Disable ? "disable" : ActiveScriptsOnQuit == QuitPolicy::Leave ? "leave" : "ask") << "\n\n";
        out << "[Interface]\n";
        out << "Mode=" << (ReleaseMode ? "release" : "developer") << "\nCompact=" << (CompactMode ? 1 : 0) << "\nPlaySounds=" << (PlaySounds ? 1 : 0) << "\n";
        out << "Profiler=" << (ProfilerEnabled ? 1 : 0) << "\nProfilerStatusBar=" << (ProfilerInStatusBar ? 1 : 0) << "\n";
        out << "Splash=" << (ShowSplash ? 1 : 0) << "\n\n";
        out << "[Debugger]\n";
        out << "VerboseLog=" << (VerboseLog ? 1 : 0) << "\nLogDebugStrings=" << (LogDebugStrings ? 1 : 0) << "\n";
        out << "HardwareExecute=" << (UseHardwareExecuteBreakpoints ? 1 : 0) << "\nRegistersOnBreak=" << (ShowRegistersOnBreak ? 1 : 0) << "\n";
        out << "InProcess=" << (UseInProcessDebugger ? 1 : 0) << "\nShowSymbols=" << (ShowSymbols ? 1 : 0) << "\n\n";
        out << "[Mono]\n";
        out << "AutoExplorer=" << (AutoExplorer ? 1 : 0) << "\n\n";
        out << "[Recent]\n";
        out << "LastProcess=" << LastProcess << "\n";
        for (std::size_t i = 0; i < RecentTables.size() && i < kRecentTables; ++i)
            out << "Recent" << i << "=" << RecentTables[i] << "\n";
        out << "\n[Theme]\n";
        out << "Palette=" << ThemeFamilyKey(Theme.Family) << "\nAppearance=" << ThemeModeKey(Theme.Mode) << "\n";
        for (const auto& [name, rgba] : Theme.Colors)
            out << "Color." << name << "=" << rgba[0] << "," << rgba[1] << "," << rgba[2] << "," << rgba[3] << "\n";
        for (const auto& [name, value] : Theme.Sizes)
            out << "Size." << name << "=" << value << "\n";
        if (NopSledBest > 0)
            out << "\n[Extras]\nNopSledBest=" << NopSledBest << "\n";
        if (!Shortcuts.empty())
        {
            out << "\n[Shortcuts]\n";
            for (const auto& [name, keys] : Shortcuts)
                out << "Key." << name << "=" << keys << "\n";
        }

        if (!Ini::WriteAtomic(path, out.str()))
            return Status::Fail(ErrorCode::SettingsNotSaved, WideToUtf8(path.wstring()));
        return Status::Success();
    }

    void Settings::AddRecentTable(const std::string& path)
    {
        RecentTables.erase(std::remove_if(RecentTables.begin(), RecentTables.end(), [&](const std::string& known) { return EqualsNoCase(known, path); }), RecentTables.end());
        RecentTables.insert(RecentTables.begin(), path);
        if (RecentTables.size() > kRecentTables)
            RecentTables.resize(kRecentTables);
    }
}
