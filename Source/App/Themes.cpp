#include "App/Themes.h"

#include "Core/Ini.h"
#include "Core/Text.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <system_error>
#include <utility>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::Themes
{
    namespace
    {
        Model& ModelSlot()
        {
            static Model model;
            return model;
        }

        std::string PathText(const std::filesystem::path& path)
        {
            return WideToUtf8(path.wstring());
        }

        std::string ColorText(const Rgba& rgba)
        {
            std::ostringstream out;
            out << rgba[0] << "," << rgba[1] << "," << rgba[2] << "," << rgba[3];
            return out.str();
        }

        // A colour or size name as the model knows it; without a model every name is taken as it is.
        std::optional<std::string> KnownColor(std::string_view name)
        {
            const Model& model = ModelSlot();
            return model.ColorName ? model.ColorName(name) : std::optional<std::string>(std::string(name));
        }

        std::optional<std::string> KnownSize(std::string_view name)
        {
            const Model& model = ModelSlot();
            return model.SizeName ? model.SizeName(name) : std::optional<std::string>(std::string(name));
        }

        float ClampedSize(std::string_view name, float value)
        {
            const Model& model = ModelSlot();
            if (!model.ClampSize)
                return value;
            return model.ClampSize(name, value).value_or(value);
        }

        bool ShowsLight(const ThemeSettings& settings)
        {
            const Model& model = ModelSlot();
            return model.Light ? model.Light(settings) : settings.Mode == ThemeMode::Light;
        }

        bool ReservedStem(std::string_view stem)
        {
            static const char* const kReserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
            return std::any_of(std::begin(kReserved), std::end(kReserved), [&](const char* name) { return EqualsNoCase(stem, name); });
        }

        bool Recycle(const std::filesystem::path& file)
        {
            std::error_code error;
            std::wstring list = std::filesystem::absolute(file, error).wstring();
            list.push_back(L'\0');
            list.push_back(L'\0');
            SHFILEOPSTRUCTW operation{};
            operation.wFunc = FO_DELETE;
            operation.pFrom = list.c_str();
            // on a drive without a recycle bin, Windows asks before the file is gone for good
            operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT | FOF_NOERRORUI;
            return ::SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
        }

        bool IsThemeFile(const std::filesystem::path& path)
        {
            return EqualsNoCase(WideToUtf8(path.extension().wstring()), ".ini");
        }
    }

    void SetModel(Model model)
    {
        ModelSlot() = std::move(model);
    }

    const Model& CurrentModel()
    {
        return ModelSlot();
    }

    const char* FamilyLabel(ThemeFamily family)
    {
        switch (family)
        {
        case ThemeFamily::Red: return "Red";
        case ThemeFamily::Green: return "Green";
        case ThemeFamily::Blue: return "Blue";
        default: return "Standard";
        }
    }

    std::optional<ThemeFamily> PresetNamed(std::string_view name)
    {
        const std::string trimmed = Trim(name);
        for (ThemeFamily family : kThemeFamilies)
        {
            if (EqualsNoCase(trimmed, FamilyLabel(family)) || EqualsNoCase(trimmed, ThemeFamilyKey(family)))
                return family;
        }
        return std::nullopt;
    }

    std::string VariantPrefix(ThemeFamily family, bool light)
    {
        if (family == ThemeFamily::Standard && !light)
            return {};
        return std::string(ThemeFamilyKey(family)) + (light ? "-light." : "-dark.");
    }

    std::size_t PrefixLength(std::string_view key)
    {
        for (ThemeFamily family : kThemeFamilies)
        {
            for (const char* mode : { "-dark.", "-light." })
            {
                const std::string prefix = std::string(ThemeFamilyKey(family)) + mode;
                if (StartsWithNoCase(key, prefix))
                    return prefix.size();
            }
        }
        return 0;
    }

    std::optional<Rgba> ParseColor(std::string_view text)
    {
        const std::string value = Trim(text);
        if (!value.empty() && value[0] == '#')
        {
            const std::string_view digits = std::string_view(value).substr(1);
            if ((digits.size() != 6 && digits.size() != 8) || !std::all_of(digits.begin(), digits.end(), IsHexDigit))
                return std::nullopt;
            Rgba rgba{ 0.0f, 0.0f, 0.0f, 1.0f };
            for (std::size_t i = 0; i * 2 < digits.size(); ++i)
                rgba[i] = static_cast<float>(HexValue(digits[i * 2]) * 16 + HexValue(digits[i * 2 + 1])) / 255.0f;
            return rgba;
        }
        const std::vector<std::string> parts = Split(value, ',');
        if (parts.size() != 3 && parts.size() != 4)
            return std::nullopt;
        Rgba rgba{ 0.0f, 0.0f, 0.0f, 1.0f };
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            const auto number = ParseDouble(Trim(parts[i]));
            if (!number || !std::isfinite(*number))
                return std::nullopt;
            rgba[i] = std::clamp(static_cast<float>(*number), 0.0f, 1.0f);
        }
        return rgba;
    }

    std::string ToText(const CustomTheme& theme)
    {
        std::ostringstream out;
        out << "; Manifold Engine theme. Colours are r,g,b,a from 0 to 1 or #rrggbb[aa]; names as in Settings > Appearance.\n";
        out << "[Theme]\nName=" << theme.Name << "\nBase=" << ThemeFamilyKey(theme.Base) << "\n";
        const auto colors = [&](const char* section, const std::map<std::string, Rgba>& values) {
            out << "\n[" << section << "]\n";
            for (const auto& [name, rgba] : values)
                out << name << "=" << ColorText(rgba) << "\n";
        };
        colors("Dark", theme.Dark);
        colors("Light", theme.Light);
        out << "\n[Sizes]\n";
        for (const auto& [name, value] : theme.Sizes)
            out << name << "=" << value << "\n";
        return out.str();
    }

    std::optional<CustomTheme> FromText(std::string_view text, std::string id, std::vector<std::string>* problems)
    {
        const auto problem = [&](std::string line) {
            if (problems)
                problems->push_back(std::move(line));
        };
        CustomTheme theme;
        theme.Id = std::move(id);
        bool header = false;
        for (const Ini::Section& section : Ini::Parse(text))
        {
            const std::string name = Trim(section.Name);
            const bool dark = EqualsNoCase(name, "Dark");
            const bool light = EqualsNoCase(name, "Light");
            const bool sizes = EqualsNoCase(name, "Sizes");
            if (EqualsNoCase(name, "Theme"))
                header = true;
            for (const Ini::Entry& entry : section.Entries)
            {
                const std::string key = Trim(entry.Key);
                const std::string value = Trim(entry.Value);
                if (EqualsNoCase(name, "Theme"))
                {
                    if (EqualsNoCase(key, "Name"))
                        theme.Name = value;
                    else if (EqualsNoCase(key, "Base"))
                    {
                        if (const auto family = PresetNamed(value))
                            theme.Base = *family;
                        else
                            problem("unknown base " + value + ", Standard is used");
                    }
                }
                else if (dark || light)
                {
                    const auto known = KnownColor(key);
                    const auto rgba = ParseColor(value);
                    if (!known)
                        problem("unknown colour " + key);
                    else if (!rgba)
                        problem(key + " is not a colour: " + value);
                    else
                        (dark ? theme.Dark : theme.Light)[*known] = *rgba;
                }
                else if (sizes)
                {
                    const auto known = KnownSize(key);
                    const auto number = ParseDouble(value);
                    if (!known)
                        problem("unknown size " + key);
                    else if (!number || !std::isfinite(*number))
                        problem(key + " is not a number: " + value);
                    else
                        theme.Sizes[*known] = ClampedSize(*known, static_cast<float>(*number));
                }
            }
        }
        if (!header)
            return std::nullopt;
        if (theme.Name.empty())
            theme.Name = theme.Id.empty() ? "Theme" : theme.Id;
        return theme;
    }

    std::string StemFor(std::string_view name)
    {
        std::string stem;
        for (char c : Trim(name))
        {
            const unsigned char u = static_cast<unsigned char>(c);
            stem.push_back(u < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos ? '_' : c);
        }
        while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
            stem.pop_back();
        if (stem.size() > 80)
            stem.resize(80);
        if (stem.empty())
            stem = "Theme";
        if (ReservedStem(stem))
            stem += "_";
        return stem;
    }

    void ToSettings(const CustomTheme& theme, ThemeSettings& settings)
    {
        settings.Family = theme.Base;
        settings.Custom = theme.Id;
        settings.CustomName = theme.Name;
        settings.Colors.clear();
        const std::string dark = VariantPrefix(theme.Base, false);
        const std::string light = VariantPrefix(theme.Base, true);
        for (const auto& [name, rgba] : theme.Dark)
            settings.Colors[dark + name] = rgba;
        for (const auto& [name, rgba] : theme.Light)
            settings.Colors[light + name] = rgba;
        settings.Sizes = theme.Sizes;
        settings.Dirty = false;
    }

    CustomTheme FromSettings(const ThemeSettings& settings)
    {
        CustomTheme theme;
        theme.Id = settings.Custom;
        theme.Name = settings.CustomName.empty() ? settings.Custom : settings.CustomName;
        theme.Base = settings.Family;
        const std::string dark = VariantPrefix(settings.Family, false);
        const std::string light = VariantPrefix(settings.Family, true);
        for (const auto& [key, rgba] : settings.Colors)
        {
            const std::size_t length = PrefixLength(key);
            const std::string prefix = key.substr(0, length);
            const std::string name = key.substr(length);
            if (EqualsNoCase(prefix, dark) || (settings.Family == ThemeFamily::Standard && EqualsNoCase(prefix, "standard-dark.")))
                theme.Dark[name] = rgba;
            else if (EqualsNoCase(prefix, light))
                theme.Light[name] = rgba;
        }
        theme.Sizes = settings.Sizes;
        return theme;
    }

    void SelectPreset(ThemeSettings& settings, ThemeFamily family)
    {
        settings.Family = family;
        settings.Custom.clear();
        settings.CustomName.clear();
        settings.Colors.clear();
        settings.Sizes.clear();
        settings.Dirty = false;
    }

    MigrationResult MigrateLegacy(Settings& settings, const std::filesystem::path& folder, const std::filesystem::path& settingsFile)
    {
        MigrationResult result;
        if (settings.LegacyThemeColors.empty() && settings.LegacyThemeSizes.empty())
            return result;

        std::map<ThemeFamily, CustomTheme> groups;
        for (const auto& [key, rgba] : settings.LegacyThemeColors)
        {
            const std::size_t length = PrefixLength(key);
            ThemeFamily family = ThemeFamily::Standard;
            bool light = false;
            if (length > 0)
            {
                const std::string prefix = Lower(key.substr(0, length));
                light = prefix.find("-light.") != std::string::npos;
                family = PresetNamed(prefix.substr(0, prefix.find('-'))).value_or(ThemeFamily::Standard);
            }
            CustomTheme& theme = groups[family];
            theme.Base = family;
            (light ? theme.Light : theme.Dark)[key.substr(length)] = rgba;
        }
        if (groups.empty())
            groups[settings.Theme.Family].Base = settings.Theme.Family;
        for (auto& [family, theme] : groups)
            theme.Sizes = settings.LegacyThemeSizes;

        std::filesystem::path backup = settingsFile;
        backup += L".before-themes";
        if (!::CopyFileW(settingsFile.c_str(), backup.c_str(), TRUE) && ::GetLastError() != ERROR_FILE_EXISTS)
        {
            result.Problem = "the settings could not be copied to " + PathText(backup) + " first: " + SystemErrorText(::GetLastError());
            return result;
        }

        Library library(folder);
        std::vector<CustomTheme> written;
        for (auto& [family, theme] : groups)
        {
            // ∑ A migration that stopped half way (a theme file or the settings could not be
            //   written) runs again on the next start: the theme it already wrote is used again
            //   instead of a second copy called "(Custom) 2".
            const std::string wanted = std::string(FamilyLabel(family)) + " (Custom)";
            const CustomTheme* same = nullptr;
            for (const CustomTheme& existing : library.Custom())
            {
                CustomTheme probe = existing;
                probe.Name = wanted;
                CustomTheme mine = theme;
                mine.Name = wanted;
                if (existing.Name.starts_with(wanted) && ToText(probe) == ToText(mine))
                    same = &existing;
            }
            if (same)
            {
                written.push_back(*same);
                continue;
            }
            theme.Name = library.UniqueName(wanted);
            const Status saved = library.Save(theme);
            if (!saved.Ok())
            {
                result.Problem = saved.Text;
                return result;
            }
            const std::filesystem::path path = folder / Utf8ToWide(theme.Id + ".ini");
            const std::optional<std::string> back = Ini::ReadText(path);
            if (!back || *back != ToText(theme))
            {
                result.Problem = PathText(path) + " did not read back as written";
                return result;
            }
            written.push_back(theme);
        }

        settings.LegacyThemeColors.clear();
        settings.LegacyThemeSizes.clear();
        for (const CustomTheme& theme : written)
        {
            result.Created.push_back(theme.Name);
            if (settings.Theme.Custom.empty() && theme.Base == settings.Theme.Family)
            {
                const ThemeMode mode = settings.Theme.Mode;
                ToSettings(theme, settings.Theme);
                settings.Theme.Mode = mode;
            }
        }
        return result;
    }

    Library::Library(std::filesystem::path folder) : m_folder(std::move(folder))
    {
    }

    std::filesystem::path Library::PathOf(std::string_view id) const
    {
        return m_folder / Utf8ToWide(std::string(id) + ".ini");
    }

    void Library::Load()
    {
        m_loaded = true;
        m_themes.clear();
        m_problems.clear();
        std::error_code error;
        if (!std::filesystem::is_directory(m_folder, error))
            return;
        std::vector<std::filesystem::path> files;
        for (std::filesystem::directory_iterator it(m_folder, error), end; !error && it != end; it.increment(error))
        {
            if (it->is_regular_file(error) && IsThemeFile(it->path()))
                files.push_back(it->path());
        }
        for (const std::filesystem::path& file : files)
        {
            const std::string id = WideToUtf8(file.stem().wstring());
            const std::optional<std::string> text = Ini::ReadText(file);
            std::vector<std::string> problems;
            std::optional<CustomTheme> theme = text ? FromText(*text, id, &problems) : std::nullopt;
            const std::string where = WideToUtf8(file.filename().wstring());
            if (!theme)
            {
                m_problems.push_back(where + ": " + (text ? "not a theme file (no [Theme] section)" : "could not be read"));
                continue;
            }
            for (const std::string& problem : problems)
                m_problems.push_back(where + ": " + problem);
            const bool taken = std::any_of(m_themes.begin(), m_themes.end(), [&](const CustomTheme& other) { return EqualsNoCase(other.Name, theme->Name); });
            if (taken || PresetNamed(theme->Name))
            {
                m_problems.push_back(where + ": the name " + theme->Name + " is taken, the file name is used");
                theme->Name = id;
                if (PresetNamed(theme->Name) || std::any_of(m_themes.begin(), m_themes.end(), [&](const CustomTheme& other) { return EqualsNoCase(other.Name, id); }))
                    continue;
            }
            m_themes.push_back(std::move(*theme));
        }
        std::sort(m_themes.begin(), m_themes.end(), [](const CustomTheme& a, const CustomTheme& b) { return Lower(a.Name) < Lower(b.Name); });
    }

    const std::vector<CustomTheme>& Library::Custom()
    {
        if (!m_loaded)
            Load();
        return m_themes;
    }

    const std::vector<std::string>& Library::Problems()
    {
        if (!m_loaded)
            Load();
        return m_problems;
    }

    const CustomTheme* Library::Find(std::string_view nameOrId)
    {
        const std::string wanted = Trim(nameOrId);
        for (const CustomTheme& theme : Custom())
        {
            if (EqualsNoCase(theme.Name, wanted))
                return &theme;
        }
        for (const CustomTheme& theme : Custom())
        {
            if (EqualsNoCase(theme.Id, wanted))
                return &theme;
        }
        return nullptr;
    }

    bool Library::NameTaken(std::string_view name, std::string_view exceptId)
    {
        const std::string trimmed = Trim(name);
        if (PresetNamed(trimmed))
            return true;
        const std::string stem = StemFor(trimmed);
        return std::any_of(Custom().begin(), Custom().end(), [&](const CustomTheme& theme) {
            return !EqualsNoCase(theme.Id, exceptId) && (EqualsNoCase(theme.Name, trimmed) || EqualsNoCase(theme.Id, stem));
        });
    }

    std::string Library::UniqueName(std::string_view wanted)
    {
        const std::string base = Trim(wanted).empty() ? std::string("Theme") : Trim(wanted);
        if (!NameTaken(base))
            return base;
        for (int number = 2;; ++number)
        {
            const std::string name = base + " " + std::to_string(number);
            if (!NameTaken(name))
                return name;
        }
    }

    Status Library::Save(CustomTheme& theme)
    {
        if (Trim(theme.Name).empty())
            return Status::Fail(ErrorCode::ThemeInvalid, "the theme has no name");
        theme.Name = Trim(theme.Name);
        std::error_code error;
        if (theme.Id.empty())
        {
            const std::string stem = StemFor(theme.Name);
            theme.Id = stem;
            for (int number = 2; std::filesystem::exists(PathOf(theme.Id), error); ++number)
                theme.Id = stem + " " + std::to_string(number);
        }
        const std::filesystem::path path = PathOf(theme.Id);
        const bool written = Ini::WriteAtomic(path, ToText(theme));
        Refresh();
        if (!written)
            return Status::Fail(ErrorCode::ThemeNotSaved, PathText(path) + ": " + SystemErrorText(::GetLastError()));
        return Status::Success();
    }

    Status Library::Rename(std::string_view id, std::string_view name, std::string* newId)
    {
        const auto match = std::find_if(Custom().begin(), Custom().end(), [&](const CustomTheme& theme) { return EqualsNoCase(theme.Id, id); });
        if (match == Custom().end())
            return Status::Fail(ErrorCode::ThemeNotFound, std::string(id));
        const CustomTheme* found = &*match;
        if (Trim(name).empty())
            return Status::Fail(ErrorCode::ThemeInvalid, "the theme has no name");
        if (NameTaken(name, id))
            return Status::Fail(ErrorCode::ThemeExists, Trim(name));
        CustomTheme theme = *found;
        const std::filesystem::path old = PathOf(theme.Id);
        theme.Name = Trim(name);
        const std::string stem = StemFor(theme.Name);
        if (!EqualsNoCase(stem, theme.Id))
            theme.Id.clear();
        else
            theme.Id = stem;
        const Status saved = Save(theme);
        if (!saved.Ok())
            return saved;
        if (PathOf(theme.Id) != old)
        {
            std::error_code error;
            if (EqualsNoCase(WideToUtf8(PathOf(theme.Id).wstring()), WideToUtf8(old.wstring())))
                std::filesystem::rename(old, PathOf(theme.Id), error);
            else
                std::filesystem::remove(old, error);
        }
        Refresh();
        if (newId)
            *newId = theme.Id;
        return Status::Success();
    }

    Status Library::Remove(std::string_view id)
    {
        const std::filesystem::path path = PathOf(id);
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return Status::Fail(ErrorCode::ThemeNotFound, std::string(id));
        const bool moved = Recycle(path);
        Refresh();
        if (!moved)
            return Status::Fail(ErrorCode::ThemeNotSaved, "could not move " + PathText(path) + " to the recycle bin");
        return Status::Success();
    }

    Status Library::Import(const std::filesystem::path& file, std::string* name)
    {
        const std::optional<std::string> text = Ini::ReadText(file);
        if (!text)
            return Status::Fail(ErrorCode::ThemeNotFound, PathText(file));
        std::optional<CustomTheme> theme = FromText(*text, {});
        if (!theme)
            return Status::Fail(ErrorCode::ThemeInvalid, PathText(file) + " is not a theme file");
        theme->Name = UniqueName(theme->Name);
        const Status saved = Save(*theme);
        if (saved.Ok() && name)
            *name = theme->Name;
        return saved;
    }

    Status Library::Export(std::string_view id, const std::filesystem::path& file)
    {
        const CustomTheme* theme = Find(id);
        if (!theme)
            return Status::Fail(ErrorCode::ThemeNotFound, std::string(id));
        if (!Ini::WriteAtomic(file, ToText(*theme)))
            return Status::Fail(ErrorCode::ThemeNotSaved, PathText(file));
        return Status::Success();
    }

    Status Library::Activate(ThemeSettings& settings)
    {
        if (settings.Custom.empty())
        {
            SelectPreset(settings, settings.Family);
            return Status::Success();
        }
        const CustomTheme* theme = nullptr;
        for (const CustomTheme& candidate : Custom())
        {
            if (EqualsNoCase(candidate.Id, settings.Custom))
                theme = &candidate;
        }
        if (!theme)
        {
            const std::string missing = settings.Custom;
            SelectPreset(settings, settings.Family);
            return Status::Fail(ErrorCode::ThemeNotFound, missing + " (the " + FamilyLabel(settings.Family) + " preset is shown)");
        }
        ToSettings(*theme, settings);
        return Status::Success();
    }

    Status Library::SaveActive(ThemeSettings& settings)
    {
        if (!settings.Dirty || settings.Custom.empty())
            return Status::Success();
        CustomTheme theme = FromSettings(settings);
        const Status saved = Save(theme);
        if (saved.Ok())
            settings.Dirty = false;
        return saved;
    }

    void Library::SetScript(ThemeSettings theme, std::string owner)
    {
        theme.Dirty = false;
        m_script = std::move(theme);
        m_scriptOwner = std::move(owner);
        RequestRestyle();
    }

    bool Library::ClearScript(std::string_view owner)
    {
        if (!m_script || (!owner.empty() && owner != m_scriptOwner))
            return false;
        m_script.reset();
        m_scriptOwner.clear();
        RequestRestyle();
        return true;
    }

    void Library::Shown(const ThemeSettings& effective)
    {
        std::ostringstream signature;
        signature << effective.Custom << '|' << static_cast<int>(effective.Family) << '|' << ShowsLight(effective) << '|' << m_script.has_value() << '|';
        for (const auto& [name, rgba] : effective.Colors)
            signature << name << '=' << ColorText(rgba) << ';';
        for (const auto& [name, value] : effective.Sizes)
            signature << name << '=' << value << ';';
        std::string text = signature.str();
        if (text == m_shown)
            return;
        if (!m_shown.empty())
            m_changed = true;
        m_shown = std::move(text);
    }
}
