#include "App/Settings.h"

#include "Core/Text.h"

#include <algorithm>
#include <fstream>
#include <map>
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
        if (auto v = text("autoattach")) AutoAttach = Split(*v, ';');
        if (auto v = text("autoattachenabled")) AutoAttachEnabled = ToBool(*v);
        if (auto v = text("tablescripts")) TableScripts = EqualsNoCase(*v, "always") ? TableScriptPolicy::Always : EqualsNoCase(*v, "never") ? TableScriptPolicy::Never : TableScriptPolicy::Ask;
        if (auto v = text("activescriptsonquit")) ActiveScriptsOnQuit = EqualsNoCase(*v, "disable") ? QuitPolicy::Disable : EqualsNoCase(*v, "leave") ? QuitPolicy::Leave : QuitPolicy::Ask;
        if (auto v = text("compact")) CompactMode = ToBool(*v);
        if (auto v = text("mode")) ReleaseMode = EqualsNoCase(*v, "release");
        if (auto v = text("verboselog")) VerboseLog = ToBool(*v);
        if (auto v = text("logdebugstrings")) LogDebugStrings = ToBool(*v);
        if (auto v = text("hardwareexecute")) UseHardwareExecuteBreakpoints = ToBool(*v);
        if (auto v = text("inprocess")) UseInProcessDebugger = ToBool(*v);
        if (auto v = text("registersonbreak")) ShowRegistersOnBreak = ToBool(*v);
        if (auto v = text("lastprocess")) LastProcess = *v;
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
        out << "TableScripts=" << (TableScripts == TableScriptPolicy::Always ? "always" : TableScripts == TableScriptPolicy::Never ? "never" : "ask") << "\n";
        out << "ActiveScriptsOnQuit=" << (ActiveScriptsOnQuit == QuitPolicy::Disable ? "disable" : ActiveScriptsOnQuit == QuitPolicy::Leave ? "leave" : "ask") << "\n\n";
        out << "[Interface]\n";
        out << "Mode=" << (ReleaseMode ? "release" : "developer") << "\nCompact=" << (CompactMode ? 1 : 0) << "\n\n";
        out << "[Debugger]\n";
        out << "VerboseLog=" << (VerboseLog ? 1 : 0) << "\nLogDebugStrings=" << (LogDebugStrings ? 1 : 0) << "\n";
        out << "HardwareExecute=" << (UseHardwareExecuteBreakpoints ? 1 : 0) << "\nRegistersOnBreak=" << (ShowRegistersOnBreak ? 1 : 0) << "\n";
        out << "InProcess=" << (UseInProcessDebugger ? 1 : 0) << "\n\n";
        out << "[Recent]\n";
        out << "LastProcess=" << LastProcess << "\n";
        for (std::size_t i = 0; i < RecentTables.size() && i < kRecentTables; ++i)
            out << "Recent" << i << "=" << RecentTables[i] << "\n";
        if (!Theme.Colors.empty() || !Theme.Sizes.empty())
        {
            out << "\n[Theme]\n";
            for (const auto& [name, rgba] : Theme.Colors)
                out << "Color." << name << "=" << rgba[0] << "," << rgba[1] << "," << rgba[2] << "," << rgba[3] << "\n";
            for (const auto& [name, value] : Theme.Sizes)
                out << "Size." << name << "=" << value << "\n";
        }
        if (!Shortcuts.empty())
        {
            out << "\n[Shortcuts]\n";
            for (const auto& [name, keys] : Shortcuts)
                out << "Key." << name << "=" << keys << "\n";
        }

        std::filesystem::path temporary = path;
        temporary += L".tmp";
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

        std::filesystem::path SettingsFile()
        {
            return ProgramFolder() / L"Manifold-Engine.ini";
        }

        std::filesystem::path ScriptSettingsFile()
        {
            return ProgramFolder() / L"Manifold-Engine.scripts.ini";
        }

        std::filesystem::path LogFile()
        {
            return ProgramFolder() / L"Manifold-Engine.log";
        }

        std::filesystem::path LayoutFile()
        {
            return ProgramFolder() / L"Manifold-Engine.layout.ini";
        }

        std::filesystem::path AutorunFolder()
        {
            return ProgramFolder() / L"autorun";
        }

        std::filesystem::path TablesFolder()
        {
            return ProgramFolder() / L"Tables";
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

        void AdoptEarlierFiles()
        {
            const std::filesystem::path folder = ProgramFolder();
            const std::pair<const wchar_t*, std::filesystem::path> files[] = {
                { L"Manifold-CheatEngine.ini", SettingsFile() },
                { L"Manifold-CheatEngine.layout.ini", LayoutFile() },
            };
            for (const auto& [earlier, current] : files)
            {
                std::error_code error;
                const std::filesystem::path source = folder / earlier;
                if (std::filesystem::exists(current, error) || !std::filesystem::exists(source, error))
                    continue;
                std::filesystem::copy_file(source, current, error);
            }
        }
    }
}
