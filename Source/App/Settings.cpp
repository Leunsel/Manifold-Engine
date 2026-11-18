#include "App/Settings.h"

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

        constexpr wchar_t kConfigFolder[] = L"configs";
        constexpr wchar_t kSettingsName[] = L"Manifold-Engine.ini";
        constexpr wchar_t kLayoutName[] = L"Manifold-Engine.layout.ini";
        constexpr wchar_t kScriptSettingsName[] = L"Manifold-Engine.scripts.ini";
        constexpr wchar_t kTablesFolder[] = L"tables";
        constexpr wchar_t kSoundsFolder[] = L"sounds";
        constexpr wchar_t kLogFolder[] = L"logs";
        constexpr wchar_t kLogName[] = L"Manifold-Engine.log";
        constexpr const wchar_t* kConfigNames[] = { kSettingsName, kLayoutName, kScriptSettingsName };
        constexpr const wchar_t* kLogNames[] = { kLogName, L"Manifold-Engine.previous.log", L"Manifold-CheatEngine.log", L"Manifold-CheatEngine.previous.log" };

        std::string SystemMessage(DWORD error)
        {
            wchar_t* buffer = nullptr;
            const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::string text = length && buffer ? Trim(WideToUtf8(std::wstring_view(buffer, length))) : "error " + std::to_string(error);
            if (buffer)
                ::LocalFree(buffer);
            while (!text.empty() && text.back() == '.')
                text.pop_back();
            return text;
        }

        bool Present(const std::filesystem::path& path)
        {
            return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        struct DiskEntry
        {
            std::wstring Name;
            bool Folder = false;
        };

        std::optional<DiskEntry> EntryOnDisk(const std::filesystem::path& path)
        {
            WIN32_FIND_DATAW data{};
            const HANDLE find = ::FindFirstFileExW(path.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
            if (find == INVALID_HANDLE_VALUE)
                return std::nullopt;
            ::FindClose(find);
            return DiskEntry{ data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 };
        }

        void RenameFolder(const std::filesystem::path& programFolder, const wchar_t* earlier, const wchar_t* wanted, Paths::Migration& migration)
        {
            const std::string earlierText = WideToUtf8(earlier);
            const std::string wantedText = WideToUtf8(wanted);
            const std::optional<DiskEntry> current = EntryOnDisk(programFolder / wanted);
            if (current && current->Name == wanted)
            {
                const std::optional<DiskEntry> old = EntryOnDisk(programFolder / earlier);
                if (old && old->Name == earlier)
                    migration.Problems.push_back("The folders " + earlierText + " and " + wantedText + " both exist; " + wantedText + " is used, " + earlierText + " is left as it is");
                return;
            }
            const std::optional<DiskEntry> source = current ? current : EntryOnDisk(programFolder / earlier);
            if (!source)
                return;
            const std::string sourceText = WideToUtf8(source->Name);
            if (!source->Folder)
            {
                migration.Problems.push_back(sourceText + " is a file, not a folder; it was not renamed to " + wantedText);
                return;
            }
            std::filesystem::path between;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                between = programFolder / (std::wstring(wanted) + L".renaming" + (attempt ? std::to_wstring(attempt) : std::wstring()));
                if (!Present(between))
                    break;
                between.clear();
            }
            const std::filesystem::path from = programFolder / source->Name;
            const std::filesystem::path to = programFolder / wanted;
            if (between.empty() || !::MoveFileExW(from.c_str(), between.c_str(), 0))
            {
                migration.Problems.push_back("The folder " + sourceText + " could not be renamed to " + wantedText + ": " + SystemMessage(::GetLastError()));
                return;
            }
            if (!::MoveFileExW(between.c_str(), to.c_str(), 0))
            {
                const DWORD error = ::GetLastError();
                const bool restored = ::MoveFileExW(between.c_str(), from.c_str(), 0) != FALSE;
                migration.Problems.push_back("The folder " + sourceText + " could not be renamed to " + wantedText + ": " + SystemMessage(error) +
                    (restored ? std::string() : "; it is now called " + WideToUtf8(between.filename().wstring())));
                return;
            }
            migration.Moved.push_back("The folder " + sourceText + " is now called " + wantedText);
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
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;
        std::stringstream content;
        content << file.rdbuf();
        std::map<std::string, std::string> values;
        std::map<std::string, std::array<float, 4>> themeColors;
        std::map<std::string, float> themeSizes;
        std::map<std::string, std::string> shortcuts;
        for (const std::string& raw : SplitLines(content.str()))
        {
            const std::string line = Trim(raw);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
                continue;
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = Trim(line.substr(0, equals));
            const std::string value = Trim(line.substr(equals + 1));
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

        std::filesystem::path temporary = path;
        temporary += L".tmp";
        if (path.has_parent_path())
        {
            std::error_code ignored;
            std::filesystem::create_directories(path.parent_path(), ignored);
        }
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
                return Status::Fail(ErrorCode::SettingsNotSaved, WideToUtf8(path.wstring()));
            const std::string text = out.str();
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file)
                return Status::Fail(ErrorCode::SettingsNotSaved, WideToUtf8(path.wstring()));
        }
        if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
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

    namespace Paths
    {
        std::filesystem::path ProgramFolder()
        {
            std::wstring program(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = ::GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
                if (length == 0)
                    return std::filesystem::current_path();
                if (length < program.size())
                {
                    program.resize(length);
                    break;
                }
                program.resize(program.size() * 2);
            }
            return std::filesystem::path(program).parent_path();
        }

        std::filesystem::path ConfigFolder()
        {
            return ProgramFolder() / kConfigFolder;
        }

        std::filesystem::path SettingsFile()
        {
            return ConfigFolder() / kSettingsName;
        }

        std::filesystem::path ScriptSettingsFile()
        {
            return ConfigFolder() / kScriptSettingsName;
        }

        std::filesystem::path LogFolder()
        {
            return ProgramFolder() / kLogFolder;
        }

        std::filesystem::path LogFile()
        {
            return LogFolder() / kLogName;
        }

        std::filesystem::path LayoutFile()
        {
            return ConfigFolder() / kLayoutName;
        }

        std::filesystem::path AutorunFolder()
        {
            return ProgramFolder() / L"autorun";
        }

        std::filesystem::path SoundsFolder()
        {
            return ProgramFolder() / kSoundsFolder;
        }

        std::filesystem::path TablesFolder()
        {
            return ProgramFolder() / kTablesFolder;
        }

        std::filesystem::path CrashFolder()
        {
            return ProgramFolder() / L"crashes";
        }

        std::filesystem::path ScanFolder()
        {
            wchar_t temp[MAX_PATH + 1] = L"";
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
            std::filesystem::path folder = length ? std::filesystem::path(temp) : ProgramFolder();
            folder /= L"Manifold-Engine";
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            return folder;
        }

        Migration AdoptEarlierFiles(const std::filesystem::path& programFolder)
        {
            Migration migration;
            const auto moveInto = [&](const wchar_t* folderName, std::span<const wchar_t* const> names)
                {
                    const std::string folderText = WideToUtf8(folderName);
                    const std::filesystem::path folder = programFolder / folderName;
                    std::error_code error;
                    if (!std::filesystem::is_directory(folder, error) && !std::filesystem::create_directories(folder, error))
                        migration.Problems.push_back("The " + folderText + " folder could not be created in " + WideToUtf8(programFolder.wstring()) + ": " + SystemMessage(static_cast<DWORD>(error.value())));
                    for (const wchar_t* name : names)
                    {
                        const std::filesystem::path source = programFolder / name;
                        const std::filesystem::path target = folder / name;
                        if (!Present(source) || Present(target))
                            continue;
                        if (::MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
                            migration.Moved.push_back(WideToUtf8(name) + " moved into the " + folderText + " folder");
                        else
                            migration.Problems.push_back(WideToUtf8(name) + " could not be moved into the " + folderText + " folder: " + SystemMessage(::GetLastError()));
                    }
                };
            moveInto(kConfigFolder, kConfigNames);
            moveInto(kLogFolder, kLogNames);
            const std::filesystem::path configs = programFolder / kConfigFolder;

            const std::pair<const wchar_t*, const wchar_t*> earlierNames[] = {
                { L"Manifold-CheatEngine.ini", kSettingsName },
                { L"Manifold-CheatEngine.layout.ini", kLayoutName },
            };
            for (const auto& [earlier, current] : earlierNames)
            {
                const std::filesystem::path source = programFolder / earlier;
                const std::filesystem::path target = configs / current;
                if (!Present(source) || Present(target) || Present(programFolder / current))
                    continue;
                if (::CopyFileW(source.c_str(), target.c_str(), TRUE))
                    migration.Moved.push_back(WideToUtf8(earlier) + " copied to " + WideToUtf8((std::filesystem::path(kConfigFolder) / current).wstring()));
                else
                    migration.Problems.push_back(WideToUtf8(earlier) + " could not be copied into the configs folder: " + SystemMessage(::GetLastError()));
            }

            RenameFolder(programFolder, L"Tables", kTablesFolder, migration);
            RenameFolder(programFolder, L"Sounds", kSoundsFolder, migration);
            return migration;
        }

        std::vector<std::string> LeftoverFiles(const std::filesystem::path& programFolder)
        {
            std::vector<std::string> names;
            for (const wchar_t* name : kConfigNames)
            {
                if (Present(programFolder / name) && Present(programFolder / kConfigFolder / name))
                    names.push_back(WideToUtf8(name));
            }
            return names;
        }

        std::string CurrentTablePath(const std::string& path, const std::filesystem::path& programFolder)
        {
            const std::string earlier = WideToUtf8((programFolder / L"Tables").wstring()) + "\\";
            if (path.size() <= earlier.size() || !EqualsNoCase(std::string_view(path).substr(0, earlier.size()), earlier))
                return path;
            return WideToUtf8((programFolder / kTablesFolder).wstring()) + "\\" + path.substr(earlier.size());
        }
    }
}
