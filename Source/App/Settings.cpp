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
        const SettingChoice kTableFileNames{ { "asloaded", "ce75", "ce76" },
            [](const Settings& s) { return s.TableFileNames; },
            [](Settings& s, int value) { s.TableFileNames = value; } };
        const SettingChoice kCorners{ { "round", "small", "square" },
            [](const Settings& s) { return s.Theme.Corners; },
            [](Settings& s, int value) { s.Theme.Corners = value; } };
        const SettingChoice kExceptionBreak{ { "never", "always", "region" },
            [](const Settings& s) { return s.BreakOnException; },
            [](Settings& s, int value) { s.BreakOnException = value; } };
        const SettingChoice kStackOffsets{ { "rsp", "rbp", "address" },
            [](const Settings& s) { return s.StackOffsets; },
            [](Settings& s, int value) { s.StackOffsets = value; } };
        const SettingChoice kAddressColumn{ { "written", "resolved", "both" },
            [](const Settings& s) { return s.AddressColumn; },
            [](Settings& s, int value) { s.AddressColumn = value; } };
        const SettingChoice kAddressDigits{ { "short", "cheatengine", "pointer" },
            [](const Settings& s) { return s.AddressDigits; },
            [](Settings& s, int value) { s.AddressDigits = value; } };
        const SettingChoice kMode{ { "developer", "release" },
            [](const Settings& s) { return s.ReleaseMode ? 1 : 0; },
            [](Settings& s, int value) { s.ReleaseMode = value == 1; } };
        const SettingChoice kPalette{ { "standard", "red", "green", "blue" },
            [](const Settings& s) { return static_cast<int>(s.Theme.Family); },
            [](Settings& s, int value) { s.Theme.Family = static_cast<ThemeFamily>(value); } };
        const SettingChoice kSignatureWildcard{ { "double", "single", "doublestar", "star" },
            [](const Settings& s) { return s.SignatureWildcard; },
            [](Settings& s, int value) { s.SignatureWildcard = value; } };
        const SettingChoice kSignatureScope{ { "module", "process" },
            [](const Settings& s) { return s.SignatureScope; },
            [](Settings& s, int value) { s.SignatureScope = value; } };
        const SettingChoice kSignatureBranches{ { "keep", "long", "all" },
            [](const Settings& s) { return s.SignatureBranches; },
            [](Settings& s, int value) { s.SignatureBranches = value; } };
        const SettingChoice kSignatureOffsets{ { "keep", "long", "all" },
            [](const Settings& s) { return s.SignatureOffsets; },
            [](Settings& s, int value) { s.SignatureOffsets = value; } };
        const SettingChoice kSignatureImmediates{ { "keep", "large", "all" },
            [](const Settings& s) { return s.SignatureImmediates; },
            [](Settings& s, int value) { s.SignatureImmediates = value; } };
        const SettingChoice kSignatureCopyFormat{ { "aob", "code", "array", "autoassembler" },
            [](const Settings& s) { return s.SignatureCopyFormat; },
            [](Settings& s, int value) { s.SignatureCopyFormat = value; } };
        const SettingChoice kSignatureRegisters{ { "keep", "sib", "all" },
            [](const Settings& s) { return s.SignatureRegisters; },
            [](Settings& s, int value) { s.SignatureRegisters = value; } };
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
            { "Scanner", "SkipNoCache", &Settings::ScanSkipNoCache },
            { "Scanner", "SkipWriteCombine", &Settings::ScanSkipWriteCombine },
            { "Scanner", "ResultMemory", &Settings::ScanMemoryMb, -1, 1024 * 1024 },
            { "Scanner", "AllTypes", &Settings::AllTypes, 0, 0x3F },
            { "Scanner", "ResultRefresh", &Settings::ResultValueRefreshMs, 50, 10000 },
            { "Table", "AddressColumn", &kAddressColumn },
            { "Table", "AddressDigits", &kAddressDigits },
            { "Table", "FileNames", &kTableFileNames },
            { "Scripts", "Autorun", &Settings::AutorunEnabled },
            { "Scripts", "HoldNewModules", &Settings::HoldNewModules },
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
            { "Scripts", "AutoAssemblerLog", &Settings::AutoAssemblerLog },
            { "Scripts", "TableScripts", &kTableScripts },
            { "Scripts", "ActiveScriptsOnQuit", &kQuitPolicy },
            { "Backups", "BackupsEnabled", &Settings::BackupsEnabled },
            { "Backups", "BackupMinutes", &Settings::BackupIntervalMinutes, 1, 240 },
            { "Backups", "BackupVersions", &Settings::BackupVersions, 1, 500 },
            { "Backups", "BackupFolder", &Settings::BackupFolder },
            { "Interface", "Mode", &kMode },
            { "Interface", "Compact", &Settings::CompactMode },
            { "Interface", "ActiveWindowOpacity", &Settings::ActiveWindowOpacity, 20, 100 },
            { "Interface", "InactiveWindowOpacity", &Settings::InactiveWindowOpacity, 20, 100 },
            { "Interface", "PlaySounds", &Settings::PlaySounds },
            { "Interface", "TypeSubmenus", &Settings::TypeSubmenus },
            { "Interface", "Profiler", &Settings::ProfilerEnabled },
            { "Interface", "LayoutChecks", &Settings::LayoutChecks },
            { "Interface", "ProfilerStatusBar", &Settings::ProfilerInStatusBar },
            { "Interface", "Splash", &Settings::ShowSplash },
            { "Debugger", "VerboseLog", &Settings::VerboseLog },
            { "Debugger", "LogDebugStrings", &Settings::LogDebugStrings },
            { "Debugger", "HardwareExecute", &Settings::UseHardwareExecuteBreakpoints },
            { "Debugger", "RegistersOnBreak", &Settings::ShowRegistersOnBreak },
            { "Debugger", "InProcess", &Settings::UseInProcessDebugger },
            { "Debugger", "VehForHidden", &Settings::UseInProcessForHiddenThreads },
            { "Debugger", "BreakOnException", &kExceptionBreak },
            { "Debugger", "ExceptionRegionStart", &Settings::ExceptionRegionStart },
            { "Debugger", "ExceptionRegionStop", &Settings::ExceptionRegionStop },
            { "Debugger", "StackFollowPointers", &Settings::StackFollowPointers },
            { "Debugger", "StackPointerTargets", &Settings::StackPointerTargets },
            { "Debugger", "StackCollapseZeros", &Settings::StackCollapseZeros },
            { "Debugger", "RegisterSections", &Settings::RegisterSections, 0, 15 },
            { "Debugger", "VectorFormat", &Settings::VectorFormat, 0, 3 },
            { "Debugger", "RegisterEditing", &Settings::RegisterEditing },
            { "Debugger", "StackOffsets", &kStackOffsets },
            { "Debugger", "ShowSymbols", &Settings::ShowSymbols },
            { "Debugger", "HexView", &Settings::MemoryHexView },
            { "Debugger", "DissectTabs", &Settings::StructureTabs },
            { "Debugger", "DissectRtti", &Settings::StructureRtti },
            { "Debugger", "CodeShare", &Settings::MemoryCodeShare, 100, 950 },
            { "Debugger", "BreakInScriptMemory", &Settings::BreakInScriptMemory },
            { "Mono", "AutoExplorer", &Settings::AutoExplorer },
            { "Signatures", "Wildcard", &kSignatureWildcard },
            { "Signatures", "Spaces", &Settings::SignatureSpaces },
            { "Signatures", "Scope", &kSignatureScope },
            { "Signatures", "CodeOnly", &Settings::SignatureCodeOnly },
            { "Signatures", "MaskAddresses", &Settings::SignatureMaskAddresses },
            { "Signatures", "Branches", &kSignatureBranches },
            { "Signatures", "Offsets", &kSignatureOffsets },
            { "Signatures", "Immediates", &kSignatureImmediates },
            { "Signatures", "ImmediateThreshold", &Settings::SignatureImmediateThreshold },
            { "Signatures", "Registers", &kSignatureRegisters },
            { "Signatures", "MaxBytes", &Settings::SignatureMaxBytes, 8, 1024 },
            { "Signatures", "MinBytes", &Settings::SignatureMinBytes, 0, 256 },
            { "Signatures", "StopAtFunctionEnd", &Settings::SignatureStopAtFunctionEnd },
            { "Signatures", "AobOffset", &Settings::SignatureOffset },
            { "Signatures", "MaxOffset", &Settings::SignatureMaxOffset, 1, 512 },
            { "Signatures", "MinSaving", &Settings::SignatureMinSaving, 0, 64 },
            { "Signatures", "CopyFormat", &kSignatureCopyFormat },
            { "Signatures", "Label", &Settings::SignatureLabel },
            { "CodeFilter", "FilterOutKeys", &Settings::CodeFilterOutKeys },
            { "CodeFilter", "KeepOnlyKeys", &Settings::CodeFilterKeepKeys },
            { "CodeFilter", "CallTargets", &Settings::CodeFilterCalls },
            { "CodeFilter", "Hotkeys", &Settings::CodeFilterHotkeys },
            { "Crashes", "Watch", &Settings::CrashWatch },
            { "Recent", "LastProcess", &Settings::LastProcess },
            { "Recent", "PaletteRecent", &Settings::PaletteRecent },
            { "Theme", "Palette", &kPalette },
            { "Theme", "Appearance", &kAppearance },
            { "Theme", "Corners", &kCorners },
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
        std::string custom;
        for (const Ini::Section& section : Ini::Parse(*content))
        {
            for (const Ini::Entry& entry : section.Entries)
            {
                const std::string key = Trim(entry.Key);
                const std::string value = Trim(entry.Value);
                if (EqualsNoCase(Trim(section.Name), "Theme") && EqualsNoCase(key, "Custom"))
                {
                    custom = value;
                    continue;
                }
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
        LegacyThemeColors = std::move(themeColors);
        LegacyThemeSizes = std::move(themeSizes);
        Theme.Custom = std::move(custom);
        Theme.CustomName.clear();
        Theme.Colors.clear();
        Theme.Sizes.clear();
        Theme.Dirty = false;
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
                if (!Theme.Custom.empty())
                    out << "Custom=" << Theme.Custom << "\n";
                for (const auto& [name, rgba] : LegacyThemeColors)
                    out << "Color." << name << "=" << rgba[0] << "," << rgba[1] << "," << rgba[2] << "," << rgba[3] << "\n";
                for (const auto& [name, value] : LegacyThemeSizes)
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
