#pragma once

#include "App/Settings.h"
#include "Core/Errors.h"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Custom themes: one .ini file per theme in Paths::ThemesFolder(). A custom theme is based on a preset
// (Standard, Red, Green, Blue) and holds its own colour overrides for dark and light plus its sizes.
// This layer knows no ImGui; the names a theme may use come from the UI through SetModel().
namespace Manifold::Themes
{
    using Rgba = std::array<float, 4>;

    struct CustomTheme
    {
        std::string Id;    // file stem, unique in the folder
        std::string Name;  // display name, unique without case
        ThemeFamily Base = ThemeFamily::Standard;
        std::map<std::string, Rgba> Dark;   // colour name (Accent, ImGui.WindowBg) -> value
        std::map<std::string, Rgba> Light;
        std::map<std::string, float> Sizes; // size name (FrameRounding) -> value
    };

    // What the UI knows: which colour and size names exist, and how a theme resolves.
    struct Model
    {
        std::function<std::optional<std::string>(std::string_view)> ColorName;  // canonical name, or none
        std::function<std::optional<std::string>(std::string_view)> SizeName;
        std::function<std::optional<float>(std::string_view, float)> ClampSize; // canonical name, value
        std::function<bool(const ThemeSettings&)> Light;                        // the mode that shows
        std::function<std::optional<Rgba>(const ThemeSettings&, std::string_view)> Color;
        std::function<std::vector<std::string>()> SizeNames;
    };
    void SetModel(Model model);
    const Model& CurrentModel();

    const char* FamilyLabel(ThemeFamily family);
    std::optional<ThemeFamily> PresetNamed(std::string_view name);  // "Red" or "red"
    std::string VariantPrefix(ThemeFamily family, bool light);       // "" for standard dark, "red-light."
    std::size_t PrefixLength(std::string_view key);                  // length of a variant prefix at the start, or 0

    // File text.
    std::string ToText(const CustomTheme& theme);
    std::optional<CustomTheme> FromText(std::string_view text, std::string id, std::vector<std::string>* problems = nullptr);
    std::string StemFor(std::string_view name);

    // ThemeSettings holds the active custom theme with prefixed keys, which ManifoldTheme::Configure reads.
    void ToSettings(const CustomTheme& theme, ThemeSettings& settings);
    CustomTheme FromSettings(const ThemeSettings& settings);
    void SelectPreset(ThemeSettings& settings, ThemeFamily family);

    std::optional<Rgba> ParseColor(std::string_view text);  // "#rrggbb", "#rrggbbaa" or "r,g,b[,a]"

    // Moves Color.* / Size.* lines of an ini from before custom themes into theme files, once.
    struct MigrationResult
    {
        std::vector<std::string> Created;  // theme names
        std::string Problem;               // empty when it worked or there was nothing to do
    };
    MigrationResult MigrateLegacy(Settings& settings, const std::filesystem::path& folder, const std::filesystem::path& settingsFile);

    class Library
    {
    public:
        explicit Library(std::filesystem::path folder);

        const std::filesystem::path& Folder() const { return m_folder; }
        const std::vector<CustomTheme>& Custom();
        const std::vector<std::string>& Problems();
        void Refresh() { m_loaded = false; }
        const CustomTheme* Find(std::string_view nameOrId);
        std::string UniqueName(std::string_view wanted);
        bool NameTaken(std::string_view name, std::string_view exceptId = {});

        Status Save(CustomTheme& theme);  // gives the theme an Id when it has none
        Status Rename(std::string_view id, std::string_view name, std::string* newId = nullptr);
        Status Remove(std::string_view id);  // to the recycle bin
        Status Import(const std::filesystem::path& file, std::string* name = nullptr);
        Status Export(std::string_view id, const std::filesystem::path& file);

        // Loads settings.Custom into settings; falls back to the base preset when the file is gone.
        Status Activate(ThemeSettings& settings);
        // Writes the active custom theme when settings.Dirty.
        Status SaveActive(ThemeSettings& settings);

        // A script's look for this session; the saved settings stay as they are.
        const ThemeSettings& Effective(const ThemeSettings& saved) const { return m_script ? *m_script : saved; }
        bool Scripted() const { return m_script.has_value(); }
        const std::string& ScriptOwner() const { return m_scriptOwner; }
        void SetScript(ThemeSettings theme, std::string owner);
        bool ClearScript(std::string_view owner = {});  // empty owner clears any

        // Restyle requests from scripts, and the change signal for the themeChanged event.
        void RequestRestyle() { m_restyle = true; }
        bool TakeRestyle() { return std::exchange(m_restyle, false); }
        void Shown(const ThemeSettings& effective);  // after a restyle: marks a change when the look differs
        bool TakeChanged() { return std::exchange(m_changed, false); }

    private:
        void Load();
        std::filesystem::path PathOf(std::string_view id) const;

        std::filesystem::path m_folder;
        bool m_loaded = false;
        std::vector<CustomTheme> m_themes;
        std::vector<std::string> m_problems;
        std::optional<ThemeSettings> m_script;
        std::string m_scriptOwner;
        bool m_restyle = false;
        bool m_changed = false;
        std::string m_shown;
    };
}
