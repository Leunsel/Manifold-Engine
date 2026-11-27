#include "TestKit.h"

#include "App/Settings.h"
#include "App/Themes.h"
#include "Core/Ini.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "UI/Theme.h"

#include <cmath>
#include <filesystem>
#include <string>

// Custom theme files: colour parsing, the file text, file names, the library and the move of old ini lines.
// Everything happens below the tests' own data folder; nothing goes to the recycle bin.
namespace Manifold::Tests
{
    namespace
    {
        bool Near(float a, float b)
        {
            return std::abs(a - b) < 0.003f;
        }

        void Colors()
        {
            const auto orange = Themes::ParseColor("#ff8000");
            CHECK(orange && Near((*orange)[0], 1.0f) && Near((*orange)[1], 128.0f / 255.0f) && Near((*orange)[2], 0.0f) && Near((*orange)[3], 1.0f));
            const auto half = Themes::ParseColor("#FF800080");
            CHECK(half && Near((*half)[3], 128.0f / 255.0f));
            const auto plain = Themes::ParseColor(" 0.1, 0.2 ,0.3 ");
            CHECK(plain && Near((*plain)[0], 0.1f) && Near((*plain)[2], 0.3f) && Near((*plain)[3], 1.0f));
            CHECK(Themes::ParseColor("2,0,0") && Near((*Themes::ParseColor("2,0,0"))[0], 1.0f));
            CHECK(!Themes::ParseColor("#12") && !Themes::ParseColor("#gg0000") && !Themes::ParseColor("1,2") && !Themes::ParseColor("a,b,c"));
        }

        void FileText()
        {
            Themes::CustomTheme theme;
            theme.Id = "Midnight";
            theme.Name = "Midnight";
            theme.Base = ThemeFamily::Blue;
            theme.Dark["Accent"] = { 0.25f, 0.5f, 0.75f, 1.0f };
            theme.Dark["ImGui.WindowBg"] = { 0.0f, 0.0f, 0.1f, 1.0f };
            theme.Light["Text"] = { 0.1f, 0.1f, 0.1f, 1.0f };
            theme.Sizes["FrameRounding"] = 5.0f;
            std::vector<std::string> problems;
            const auto back = Themes::FromText(Themes::ToText(theme), "Midnight", &problems);
            CHECK_MSG(back && problems.empty(), problems.empty() ? std::string() : problems.front());
            CHECK(back && back->Name == "Midnight" && back->Base == ThemeFamily::Blue && back->Dark == theme.Dark && back->Light == theme.Light && back->Sizes == theme.Sizes);

            problems.clear();
            const auto rough = Themes::FromText("[Theme]\nBase=purple\n[Dark]\naccent=#ff0000\nNoSuchColour=#ff0000\nText=red\n[Sizes]\nframerounding=40\nNope=1\n", "Rough", &problems);
            CHECK(rough && rough->Name == "Rough" && rough->Base == ThemeFamily::Standard);
            CHECK(rough && rough->Dark.size() == 1 && rough->Dark.count("Accent") == 1);
            CHECK(rough && rough->Sizes.size() == 1 && rough->Sizes.count("FrameRounding") == 1 && Near(rough->Sizes.at("FrameRounding"), 12.0f));
            CHECK_MSG(problems.size() == 4, Join(problems, "; "));
            CHECK(!Themes::FromText("[Dark]\nAccent=#ffffff\n", "x"));
        }

        void Stems()
        {
            CHECK(Themes::StemFor("a/b:c*d") == "a_b_c_d");
            CHECK(Themes::StemFor("  Night.  ") == "Night");
            CHECK(Themes::StemFor("con") == "con_");
            CHECK(Themes::StemFor("") == "Theme");
            CHECK(Themes::PresetNamed("Red") == ThemeFamily::Red && Themes::PresetNamed(" blue ") == ThemeFamily::Blue && !Themes::PresetNamed("Purple"));
            CHECK(Themes::VariantPrefix(ThemeFamily::Standard, false).empty() && Themes::VariantPrefix(ThemeFamily::Red, true) == "red-light.");
            CHECK(Themes::PrefixLength("Green-Dark.Accent") == 11 && Themes::PrefixLength("Accent") == 0);
        }

        void SettingsBridge()
        {
            Themes::CustomTheme theme;
            theme.Id = "probe";
            theme.Name = "Probe";
            theme.Base = ThemeFamily::Red;
            theme.Dark["Accent"] = { 1.0f, 0.0f, 0.0f, 1.0f };
            theme.Light["Accent"] = { 0.5f, 0.0f, 0.0f, 1.0f };
            ThemeSettings settings;
            settings.Mode = ThemeMode::Light;
            settings.Corners = 2;
            Themes::ToSettings(theme, settings);
            CHECK(settings.Family == ThemeFamily::Red && settings.Custom == "probe" && settings.CustomName == "Probe" && settings.Mode == ThemeMode::Light && settings.Corners == 2);
            CHECK(settings.Colors.count("red-dark.Accent") == 1 && settings.Colors.count("red-light.Accent") == 1);
            const Themes::CustomTheme back = Themes::FromSettings(settings);
            CHECK(back.Dark == theme.Dark && back.Light == theme.Light && back.Base == ThemeFamily::Red && back.Name == "Probe");

            const Themes::Model& model = Themes::CurrentModel();
            const auto accent = model.Color(settings, "accent");
            CHECK(accent && Near((*accent)[0], 0.5f));
            settings.Mode = ThemeMode::Dark;
            CHECK(model.Color(settings, "Accent") && Near((*model.Color(settings, "Accent"))[0], 1.0f));
            Themes::SelectPreset(settings, ThemeFamily::Green);
            CHECK(settings.Custom.empty() && settings.Colors.empty() && settings.Family == ThemeFamily::Green);
        }

        void Library(const std::filesystem::path& folder)
        {
            Themes::Library library(folder);
            CHECK(library.Custom().empty() && library.Problems().empty());
            CHECK(library.NameTaken("red") && library.UniqueName("Red") == "Red 2");

            Themes::CustomTheme theme;
            theme.Name = "Night/Shift";
            theme.Base = ThemeFamily::Blue;
            theme.Dark["Accent"] = { 0.1f, 0.2f, 0.3f, 1.0f };
            CHECK(library.Save(theme).Ok() && theme.Id == "Night_Shift");
            CHECK(library.Custom().size() == 1 && library.Find("night/shift") && library.Find("Night_Shift"));
            CHECK(library.UniqueName("Night/Shift") == "Night/Shift 2");

            std::string id;
            CHECK(library.Rename("Night_Shift", "Dawn", &id).Ok() && id == "Dawn");
            CHECK(library.Custom().size() == 1 && library.Find("Dawn") && !library.Find("Night/Shift") && !std::filesystem::exists(folder / "Night_Shift.ini"));
            CHECK(library.Rename("Dawn", "Blue").Code == ErrorCode::ThemeExists);

            const std::filesystem::path exported = folder.parent_path() / "exported-theme.ini";
            CHECK(library.Export("Dawn", exported).Ok());
            std::string imported;
            CHECK(library.Import(exported, &imported).Ok() && imported == "Dawn 2");
            CHECK(library.Custom().size() == 2);

            Ini::WriteAtomic(folder / "broken.ini", "[Dark]\nAccent=#ffffff\n");
            library.Refresh();
            CHECK(library.Custom().size() == 2 && library.Problems().size() == 1);

            ThemeSettings settings;
            settings.Family = ThemeFamily::Green;
            settings.Custom = "Dawn";
            CHECK(library.Activate(settings).Ok() && settings.Family == ThemeFamily::Blue && settings.CustomName == "Dawn" && settings.Colors.count("blue-dark.Accent") == 1);
            settings.Colors["blue-light.Text"] = { 0.0f, 0.0f, 0.0f, 1.0f };
            settings.Dirty = true;
            CHECK(library.SaveActive(settings).Ok() && !settings.Dirty && library.Find("Dawn")->Light.count("Text") == 1);
            settings.Custom = "Gone";
            CHECK(library.Activate(settings).Code == ErrorCode::ThemeNotFound && settings.Custom.empty() && settings.Colors.empty());

            // the change signal only fires when the look differs from the one shown before
            library.Shown(settings);
            library.TakeChanged();
            library.Shown(settings);
            CHECK(!library.TakeChanged());
            ThemeSettings script = settings;
            script.Family = ThemeFamily::Red;
            library.SetScript(script, "=probe");
            CHECK(library.TakeRestyle() && library.Scripted() && library.Effective(settings).Family == ThemeFamily::Red);
            library.Shown(library.Effective(settings));
            CHECK(library.TakeChanged());
            CHECK(!library.ClearScript("=other") && library.ClearScript("=probe") && !library.Scripted());

            Themes::CustomTheme wide;
            wide.Name = "a";
            for (int i = 0; i < 40; ++i)
                wide.Name += "\xC3\x84";
            CHECK(library.Save(wide).Ok());
            CHECK_MSG(WideToUtf8(Utf8ToWide(wide.Id)) == wide.Id, "the file name is not the id: " + std::to_string(wide.Id.size()) + " bytes");
            ThemeSettings chosen;
            chosen.Custom = wide.Id;
            const Status activated = library.Activate(chosen);
            CHECK_MSG(activated.Ok(), activated.Text);
        }

        void Migration(const std::filesystem::path& root)
        {
            const std::filesystem::path folder = root / "migrated";
            const std::filesystem::path file = root / "Manifold-Engine.ini";
            Ini::WriteAtomic(file, "[Theme]\nPalette=red\n");
            Manifold::Settings settings;
            settings.Theme.Family = ThemeFamily::Red;
            settings.Theme.Mode = ThemeMode::Light;
            settings.LegacyThemeColors["Accent"] = { 0.1f, 0.1f, 0.1f, 1.0f };
            settings.LegacyThemeColors["red-light.Text"] = { 0.2f, 0.2f, 0.2f, 1.0f };
            settings.LegacyThemeColors["red-dark.Text"] = { 0.3f, 0.3f, 0.3f, 1.0f };
            settings.LegacyThemeColors["blue-dark.ImGui.WindowBg"] = { 0.4f, 0.4f, 0.4f, 1.0f };
            settings.LegacyThemeSizes["FrameRounding"] = 5.0f;

            // without the settings file there is nothing to copy first: everything stays as it was
            Manifold::Settings failing = settings;
            const Themes::MigrationResult refused = Themes::MigrateLegacy(failing, folder, root / "missing" / "Manifold-Engine.ini");
            CHECK(!refused.Problem.empty() && refused.Created.empty() && failing.LegacyThemeColors.size() == 4 && !std::filesystem::exists(folder));

            const Manifold::Settings original = settings;
            const Themes::MigrationResult moved = Themes::MigrateLegacy(settings, folder, file);
            CHECK_MSG(moved.Problem.empty() && moved.Created.size() == 3, moved.Problem);
            CHECK(settings.LegacyThemeColors.empty() && settings.LegacyThemeSizes.empty());
            CHECK(std::filesystem::exists(root / "Manifold-Engine.ini.before-themes"));
            CHECK(settings.Theme.Custom == "Red (Custom)" && settings.Theme.Mode == ThemeMode::Light && settings.Theme.Colors.count("red-light.Text") == 1);
            Themes::Library library(folder);
            const Themes::CustomTheme* standard = library.Find("Standard (Custom)");
            const Themes::CustomTheme* red = library.Find("Red (Custom)");
            const Themes::CustomTheme* blue = library.Find("Blue (Custom)");
            CHECK(standard && standard->Base == ThemeFamily::Standard && standard->Dark.count("Accent") == 1 && standard->Sizes.count("FrameRounding") == 1);
            CHECK(red && red->Dark.count("Text") == 1 && red->Light.count("Text") == 1 && red->Sizes.count("FrameRounding") == 1);
            CHECK(blue && blue->Dark.count("ImGui.WindowBg") == 1);
            CHECK(Themes::MigrateLegacy(settings, folder, file).Created.empty());
            // the settings were not saved after the migration (or a theme file failed): the next start
            // migrates again and uses the themes it wrote, it does not add "(Custom) 2" copies
            {
                Manifold::Settings again = original;
                const Themes::MigrationResult rerun = Themes::MigrateLegacy(again, folder, file);
                CHECK_MSG(rerun.Problem.empty() && rerun.Created.size() == 3, rerun.Problem);
                Themes::Library fresh(folder);
                CHECK_MSG(fresh.Custom().size() == 3, Format("%zu themes after migrating twice", fresh.Custom().size()));
                CHECK(again.Theme.Custom == "Red (Custom)");
            }

            // the ini keeps the selection, the old lines are gone
            CHECK(settings.Save(file).Ok());
            const std::string text = Ini::ReadText(file).value_or("");
            CHECK_MSG(text.find("Custom=Red (Custom)") != std::string::npos && text.find("Color.") == std::string::npos && text.find("Size.") == std::string::npos, text);
        }
    }

    int RunThemeTests()
    {
        const int before = Totals().Failures;
        ManifoldTheme::InstallThemeModel();
        const std::filesystem::path root = Paths::DataFolder() / "theme-tests";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        Colors();
        FileText();
        Stems();
        SettingsBridge();
        Library(root / "themes");
        Migration(root);
        std::filesystem::remove_all(root, ignored);
        return Totals().Failures - before;
    }
}

namespace Manifold::Tests
{
    MANIFOLD_SUITE("themes", "Custom themes", 11, [](const SuiteContext&) { return RunThemeTests(); });
}
