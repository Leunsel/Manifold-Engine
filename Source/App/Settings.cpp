#include "App/Settings.h"

#include "Core/Ini.h"
#include "Core/Text.h"

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>

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

        const SettingChoice kTableScripts{ { "ask", "always", "never" },
            [](const Settings& s) { return static_cast<int>(s.TableScripts); },
            [](Settings& s, int value) { s.TableScripts = static_cast<TableScriptPolicy>(value); } };
        const SettingChoice kQuitPolicy{ { "ask", "disable", "leave" },
            [](const Settings& s) { return static_cast<int>(s.ActiveScriptsOnQuit); },
            [](Settings& s, int value) { s.ActiveScriptsOnQuit = static_cast<QuitPolicy>(value); } };
        const SettingChoice kExceptionBreak{ { "never", "always", "region" },
            [](const Settings& s) { return s.BreakOnException; },
            [](Settings& s, int value) { s.BreakOnException = value; } };
        const SettingChoice kStackOffsets{ { "rsp", "rbp", "address" },
            [](const Settings& s) { return s.StackOffsets; },
            [](Settings& s, int value) { s.StackOffsets = value; } };
        const SettingChoice kMode{ { "developer", "release" },
            [](const Settings& s) { return s.ReleaseMode ? 1 : 0; },
            [](Settings& s, int value) { s.ReleaseMode = value == 1; } };
        const SettingChoice kPalette{ { "standard", "red", "green", "blue" },
            [](const Settings& s) { return static_cast<int>(s.Theme.Family); },
            [](Settings& s, int value) { s.Theme.Family = static_cast<ThemeFamily>(value); } };
        const SettingChoice kAppearance{ { "dark", "light", "system" },
            [](const Settings& s) { return static_cast<int>(s.Theme.Mode); },
            [](Settings& s, int value) { s.Theme.Mode = static_cast<ThemeMode>(value); } };

        const SettingField kFields[] = {
            { "Window", "X", &Settings::X },
            { "Window", "Y", &Settings::Y },
            { "Window", "Width", &Settings::Width },
            { "Window", "Height", &Settings::Height },
            { "Window", "Placed", &Settings::Placed },
            { "Window", "Maximised", &Settings::Maximised },
            { "Scanner", "UpdateInterval", &Settings::UpdateIntervalMs, 16, 5000 },
            { "Scanner", "FreezeInterval", &Settings::FreezeIntervalMs, 10, 5000 },
            { "Scanner", "ScanThreads", &Settings::ScanThreads, 0, 64 },
            { "Scanner", "FastScan", &Settings::FastScan },
            { "Scanner", "PauseWhileScanning", &Settings::PauseWhileScanning },
            { "Scanner", "ScanPrivate", &Settings::ScanPrivate },
            { "Scanner", "ScanImage", &Settings::ScanImage },
            { "Scanner", "ScanMapped", &Settings::ScanMapped },
            { "Scanner", "AllTypes", &Settings::AllTypes, 0, 0x3F },
            { "Scanner", "ResultRefresh", &Settings::ResultValueRefreshMs, 50, 10000 },
            { "Scripts", "Autorun", &Settings::AutorunEnabled },
            { "Scripts", "AutoAttach", &Settings::AutoAttach },
            { "Scripts", "AutoAttachEnabled", &Settings::AutoAttachEnabled },
            { "Scripts", "ScriptWindowsAppLook", &Settings::ScriptWindowsAppLook },
            { "Scripts", "EditorTabs", &Settings::ScriptEditorTabs },
            { "Scripts", "ScriptCallbackLimit", &Settings::ScriptCallbackLimitMs, 250, 60000 },
            { "Scripts", "ShowOnPrint", &Settings::LuaShowOnPrint },
            { "Scripts", "WrapOutput", &Settings::LuaWrapOutput },
            { "Scripts", "LegacyEditor", &Settings::LegacyCodeEditor, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), true },
            { "Scripts", "ConsoleTime", &Settings::LuaConsoleTime },
            { "Scripts", "ConsoleHex", &Settings::LuaConsoleHex },
            { "Scripts", "DocsPanel", &Settings::LuaDocsPanel },
            { "Scripts", "TableScripts", &kTableScripts },
            { "Scripts", "ActiveScriptsOnQuit", &kQuitPolicy },
            { "Interface", "Mode", &kMode },
            { "Interface", "Compact", &Settings::CompactMode },
            { "Interface", "PlaySounds", &Settings::PlaySounds },
            { "Interface", "Profiler", &Settings::ProfilerEnabled },
            { "Interface", "ProfilerStatusBar", &Settings::ProfilerInStatusBar },
            { "Interface", "Splash", &Settings::ShowSplash },
            { "Debugger", "VerboseLog", &Settings::VerboseLog },
            { "Debugger", "LogDebugStrings", &Settings::LogDebugStrings },
            { "Debugger", "HardwareExecute", &Settings::UseHardwareExecuteBreakpoints },
            { "Debugger", "RegistersOnBreak", &Settings::ShowRegistersOnBreak },
            { "Debugger", "InProcess", &Settings::UseInProcessDebugger },
            { "Debugger", "BreakOnException", &kExceptionBreak },
            { "Debugger", "ExceptionRegionStart", &Settings::ExceptionRegionStart },
            { "Debugger", "ExceptionRegionStop", &Settings::ExceptionRegionStop },
            { "Debugger", "StackFollowPointers", &Settings::StackFollowPointers },
            { "Debugger", "RegisterSections", &Settings::RegisterSections, 0, 15 },
            { "Debugger", "VectorFormat", &Settings::VectorFormat, 0, 3 },
            { "Debugger", "RegisterEditing", &Settings::RegisterEditing },
            { "Debugger", "StackOffsets", &kStackOffsets },
            { "Debugger", "ShowSymbols", &Settings::ShowSymbols },
            { "Debugger", "HexView", &Settings::MemoryHexView },
            { "Debugger", "CodeShare", &Settings::MemoryCodeShare, 100, 950 },
            { "Mono", "AutoExplorer", &Settings::AutoExplorer },
            { "Recent", "LastProcess", &Settings::LastProcess },
            { "Theme", "Palette", &kPalette },
            { "Theme", "Appearance", &kAppearance },
            { "Extras", "NopSledBest", &Settings::NopSledBest, 0, std::numeric_limits<int>::max(), true },
        };

        template <class... Ts>
        struct Overloaded : Ts...
        {
            using Ts::operator()...;
        };

        void ReadField(Settings& settings, const SettingField& field, const std::string& value)
        {
            std::visit(Overloaded{
                [&](bool Settings::* member) { settings.*member = ToBool(value); },
                [&](int Settings::* member) { settings.*member = std::clamp(ToInt(value, settings.*member), field.Min, field.Max); },
                [&](std::uint32_t Settings::* member)
                {
                    const int current = static_cast<int>(std::min<std::uint32_t>(settings.*member, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
                    settings.*member = static_cast<std::uint32_t>(std::clamp(ToInt(value, current), std::max(0, field.Min), field.Max));
                },
                [&](std::string Settings::* member) { settings.*member = value; },
                [&](std::vector<std::string> Settings::* member) { settings.*member = Split(value, ';'); },
                [&](const SettingChoice* choice)
                {
                    int index = 0;
                    for (std::size_t i = 0; i < choice->Names.size(); ++i)
                    {
                        if (EqualsNoCase(value, choice->Names[i]))
                            index = static_cast<int>(i);
                    }
                    choice->Set(settings, index);
                },
            }, field.Member);
        }

        std::string FieldText(const Settings& settings, const SettingField& field)
        {
            return std::visit(Overloaded{
                [&](bool Settings::* member) { return std::string(settings.*member ? "1" : "0"); },
                [&](int Settings::* member) { return std::to_string(settings.*member); },
                [&](std::uint32_t Settings::* member) { return std::to_string(settings.*member); },
                [&](std::string Settings::* member) { return settings.*member; },
                [&](std::vector<std::string> Settings::* member) { return Join(settings.*member, ";"); },
                [&](const SettingChoice* choice)
                {
                    const int index = choice->Get(settings);
                    return std::string(index >= 0 && static_cast<std::size_t>(index) < choice->Names.size() ? choice->Names[static_cast<std::size_t>(index)] : choice->Names.front());
                },
            }, field.Member);
        }
    }

    std::span<const SettingField> SettingFields()
    {
        return kFields;
    }

    const char* ThemeFamilyKey(ThemeFamily family)
    {
        const std::size_t index = static_cast<std::size_t>(family);
        return index < kPalette.Names.size() ? kPalette.Names[index] : kPalette.Names.front();
    }

    const char* ThemeModeKey(ThemeMode mode)
    {
        const std::size_t index = static_cast<std::size_t>(mode);
        return index < kAppearance.Names.size() ? kAppearance.Names[index] : kAppearance.Names.front();
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
        for (const Ini::Section& section : Ini::Parse(*content))
        {
            for (const Ini::Entry& entry : section.Entries)
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
        }
        Theme.Colors = std::move(themeColors);
        Theme.Sizes = std::move(themeSizes);
        Shortcuts = std::move(shortcuts);
        for (const SettingField& field : kFields)
        {
            const auto found = values.find(Lower(field.Key));
            if (found != values.end())
                ReadField(*this, field, found->second);
        }
        RecentTables.clear();
        for (std::size_t i = 0; i < kRecentTables; ++i)
        {
            const auto found = values.find("recent" + std::to_string(i));
            if (found != values.end() && !found->second.empty())
                RecentTables.push_back(found->second);
        }
        return true;
    }

    Status Settings::Save(const std::filesystem::path& path) const
    {
        const Settings defaults;
        std::ostringstream out;
        std::string section;
        const auto open = [&](const char* name)
            {
                if (section == name)
                    return;
                if (!section.empty())
                    out << "\n";
                out << "[" << name << "]\n";
                section = name;
            };
        for (std::size_t i = 0; i < std::size(kFields); ++i)
        {
            const SettingField& field = kFields[i];
            const std::string text = FieldText(*this, field);
            if (!field.OnlyWhenChanged || text != FieldText(defaults, field))
            {
                open(field.Section);
                out << field.Key << "=" << text << "\n";
            }
            const bool last = i + 1 == std::size(kFields) || std::string_view(kFields[i + 1].Section) != field.Section;
            if (!last || section != field.Section)
                continue;
            if (section == "Recent")
            {
                for (std::size_t recent = 0; recent < RecentTables.size() && recent < kRecentTables; ++recent)
                    out << "Recent" << recent << "=" << RecentTables[recent] << "\n";
            }
            else if (section == "Theme")
            {
                for (const auto& [name, rgba] : Theme.Colors)
                    out << "Color." << name << "=" << rgba[0] << "," << rgba[1] << "," << rgba[2] << "," << rgba[3] << "\n";
                for (const auto& [name, value] : Theme.Sizes)
                    out << "Size." << name << "=" << value << "\n";
            }
        }
        if (!Shortcuts.empty())
        {
            open("Shortcuts");
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
