#include "Lua/LuaTheme.h"

#include "App/Engine.h"
#include "App/Themes.h"
#include "Core/Text.h"
#include "Lua/LuaArgs.h"

#include "lua.hpp"

#include <optional>
#include <string>
#include <string_view>

// Natives of manifold.ui.theme. The Lua side (Manifold.lua) checks the argument types; these resolve names,
// read the theme files and keep a script's look in the theme library for the session.
namespace Manifold::Lua
{
    namespace
    {
        constexpr std::string_view kImGui = "ImGui.";

        // Names as Lua shows them: accent, codeKeyword, imgui.WindowBg (manifold.ui.theme.colors() spells them so).
        std::string LuaName(std::string_view canonical)
        {
            if (canonical.substr(0, kImGui.size()) == kImGui)
                return "imgui." + std::string(canonical.substr(kImGui.size()));
            std::string name(canonical);
            if (!name.empty())
                name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
            return name;
        }

        void PushColor(lua_State* state, const Themes::Rgba& rgba)
        {
            lua_createtable(state, 0, 4);
            const char* const keys[] = { "r", "g", "b", "a" };
            for (int i = 0; i < 4; ++i)
            {
                lua_pushnumber(state, rgba[static_cast<std::size_t>(i)]);
                lua_setfield(state, -2, keys[i]);
            }
        }

        void PushColors(lua_State* state, const std::map<std::string, Themes::Rgba>& colors)
        {
            lua_createtable(state, 0, static_cast<int>(colors.size()));
            for (const auto& [name, rgba] : colors)
            {
                PushColor(state, rgba);
                lua_setfield(state, -2, LuaName(name).c_str());
            }
        }

        void PushDefinition(lua_State* state, const Themes::CustomTheme& theme)
        {
            lua_createtable(state, 0, 4);
            lua_pushstring(state, ThemeFamilyKey(theme.Base));
            lua_setfield(state, -2, "base");
            PushColors(state, theme.Dark);
            lua_setfield(state, -2, "dark");
            PushColors(state, theme.Light);
            lua_setfield(state, -2, "light");
            lua_createtable(state, 0, static_cast<int>(theme.Sizes.size()));
            for (const auto& [name, value] : theme.Sizes)
            {
                lua_pushnumber(state, value);
                lua_setfield(state, -2, name.c_str());
            }
            lua_setfield(state, -2, "sizes");
        }

        std::optional<Themes::Rgba> ColorValue(lua_State* state, int index)
        {
            if (lua_type(state, index) == LUA_TSTRING)
                return Themes::ParseColor(lua_tostring(state, index));
            if (!lua_istable(state, index))
                return std::nullopt;
            index = lua_absindex(state, index);
            Themes::Rgba rgba{ 0.0f, 0.0f, 0.0f, 1.0f };
            const char* const keys[] = { "r", "g", "b", "a" };
            for (int i = 0; i < 4; ++i)
            {
                lua_getfield(state, index, keys[i]);
                if (lua_isnil(state, -1))
                {
                    lua_pop(state, 1);
                    lua_rawgeti(state, index, i + 1);
                }
                const bool given = !lua_isnil(state, -1);
                int isNumber = 0;
                const lua_Number number = lua_tonumberx(state, -1, &isNumber);
                lua_pop(state, 1);
                if (!given && i == 3)
                    break;
                if (!isNumber || number != number)
                    return std::nullopt;
                rgba[static_cast<std::size_t>(i)] = static_cast<float>(number < 0.0 ? 0.0 : number > 1.0 ? 1.0 : number);
            }
            return rgba;
        }

        // Reads { name = colour } from the table at index; problem gets the first bad entry.
        bool ReadColors(lua_State* state, int index, std::map<std::string, Themes::Rgba>& into, std::string& problem)
        {
            if (lua_isnoneornil(state, index))
                return true;
            index = lua_absindex(state, index);
            const Themes::Model& model = Themes::CurrentModel();
            lua_pushnil(state);
            while (lua_next(state, index))
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    problem = "colour names must be strings";
                    return false;
                }
                const std::string name = lua_tostring(state, -2);
                const std::optional<std::string> canonical = model.ColorName ? model.ColorName(name) : std::optional<std::string>(name);
                const std::optional<Themes::Rgba> value = ColorValue(state, -1);
                lua_pop(state, 1);
                if (!canonical)
                {
                    lua_pop(state, 1);
                    problem = "unknown colour " + name;
                    return false;
                }
                if (!value)
                {
                    lua_pop(state, 1);
                    problem = name + " is not a colour";
                    return false;
                }
                into[*canonical] = *value;
            }
            return true;
        }

        bool ReadSizes(lua_State* state, int index, std::map<std::string, float>& into, std::string& problem)
        {
            if (lua_isnoneornil(state, index))
                return true;
            index = lua_absindex(state, index);
            const Themes::Model& model = Themes::CurrentModel();
            lua_pushnil(state);
            while (lua_next(state, index))
            {
                const std::string name = lua_type(state, -2) == LUA_TSTRING ? lua_tostring(state, -2) : std::string();
                int isNumber = 0;
                const lua_Number number = lua_tonumberx(state, -1, &isNumber);
                lua_pop(state, 1);
                const std::optional<std::string> canonical = model.SizeName ? model.SizeName(name) : std::optional<std::string>(name);
                if (name.empty() || !canonical)
                {
                    lua_pop(state, 1);
                    problem = "unknown size " + name;
                    return false;
                }
                if (!isNumber || number != number)
                {
                    lua_pop(state, 1);
                    problem = name + " is not a number";
                    return false;
                }
                const float value = static_cast<float>(number);
                into[*canonical] = model.ClampSize ? model.ClampSize(*canonical, value).value_or(value) : value;
            }
            return true;
        }

        bool ReadField(lua_State* state, int table, const char* field, auto&& read, std::string& problem)
        {
            lua_getfield(state, table, field);
            const bool ok = read(state, -1, problem);
            lua_pop(state, 1);
            return ok;
        }

        void SetColor(ThemeSettings& theme, bool light, const std::string& canonical, const Themes::Rgba& rgba)
        {
            const std::string key = Themes::VariantPrefix(theme.Family, light) + canonical;
            for (auto it = theme.Colors.begin(); it != theme.Colors.end();)
                it = EqualsNoCase(it->first, key) ? theme.Colors.erase(it) : std::next(it);
            theme.Colors[key] = rgba;
        }

        std::optional<ThemeMode> ModeNamed(std::string_view name)
        {
            for (ThemeMode mode : { ThemeMode::Dark, ThemeMode::Light, ThemeMode::System })
            {
                if (EqualsNoCase(name, ThemeModeKey(mode)))
                    return mode;
            }
            return std::nullopt;
        }

        int ThemeSizes(lua_State* state)
        {
            const Themes::Model& model = Themes::CurrentModel();
            lua_newtable(state);
            if (model.SizeNames)
            {
                lua_Integer index = 1;
                for (const std::string& name : model.SizeNames())
                {
                    lua_pushstring(state, name.c_str());
                    lua_rawseti(state, -2, index++);
                }
            }
            return 1;
        }

        int ThemeCurrent(lua_State* state)
        {
            Engine& engine = Owner(state);
            Themes::Library& library = engine.ThemeLibrary();
            const ThemeSettings& shown = library.Effective(engine.Options().Theme);
            const Themes::Model& model = Themes::CurrentModel();
            const bool custom = !shown.Custom.empty();
            lua_createtable(state, 0, 7);
            lua_pushstring(state, custom ? (shown.CustomName.empty() ? shown.Custom.c_str() : shown.CustomName.c_str()) : Themes::FamilyLabel(shown.Family));
            lua_setfield(state, -2, "name");
            lua_pushboolean(state, custom ? 1 : 0);
            lua_setfield(state, -2, "custom");
            lua_pushstring(state, ThemeFamilyKey(shown.Family));
            lua_setfield(state, -2, "base");
            lua_pushstring(state, ThemeModeKey(shown.Mode));
            lua_setfield(state, -2, "mode");
            lua_pushboolean(state, (model.Light ? model.Light(shown) : shown.Mode == ThemeMode::Light) ? 1 : 0);
            lua_setfield(state, -2, "light");
            lua_pushboolean(state, library.Scripted() ? 1 : 0);
            lua_setfield(state, -2, "scripted");
            return 1;
        }

        int ThemeList(lua_State* state)
        {
            Themes::Library& library = Owner(state).ThemeLibrary();
            library.Refresh();
            lua_newtable(state);
            lua_Integer index = 1;
            const auto entry = [&](const std::string& name, bool custom, ThemeFamily base) {
                lua_createtable(state, 0, 3);
                lua_pushstring(state, name.c_str());
                lua_setfield(state, -2, "name");
                lua_pushboolean(state, custom ? 1 : 0);
                lua_setfield(state, -2, "custom");
                lua_pushstring(state, ThemeFamilyKey(base));
                lua_setfield(state, -2, "base");
                lua_rawseti(state, -2, index++);
            };
            for (ThemeFamily family : kThemeFamilies)
                entry(Themes::FamilyLabel(family), false, family);
            for (const Themes::CustomTheme& theme : library.Custom())
                entry(theme.Name, true, theme.Base);
            return 1;
        }

        int ThemeColor(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = luaL_checkstring(state, 1);
            const Themes::Model& model = Themes::CurrentModel();
            if (!model.ColorName || !model.Color || !model.ColorName(name))
                return PushFailure(state, ErrorCode::ThemeInvalid, "unknown colour " + name);
            const std::optional<Themes::Rgba> rgba = model.Color(engine.ThemeLibrary().Effective(engine.Options().Theme), name);
            if (!rgba)
                return PushFailure(state, ErrorCode::ThemeInvalid, name + " is known only once the window shows");
            PushColor(state, *rgba);
            return 1;
        }

        int ThemeDefinition(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = luaL_checkstring(state, 1);
            if (const std::optional<ThemeFamily> family = Themes::PresetNamed(name))
            {
                Themes::CustomTheme preset;
                preset.Base = *family;
                PushDefinition(state, preset);
                return 1;
            }
            Themes::Library& library = engine.ThemeLibrary();
            library.Refresh();
            const Themes::CustomTheme* theme = library.Find(name);
            if (!theme)
                return PushFailure(state, ErrorCode::ThemeNotFound, name);
            const ThemeSettings& saved = engine.Options().Theme;
            // ∑ the active theme may have changes that are not written yet
            PushDefinition(state, EqualsNoCase(saved.Custom, theme->Id) ? Themes::FromSettings(saved) : *theme);
            return 1;
        }

        int ThemeApply(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = luaL_checkstring(state, 1);
            const char* modeText = luaL_optstring(state, 2, nullptr);
            const std::string owner = luaL_checkstring(state, 3);
            Themes::Library& library = engine.ThemeLibrary();
            ThemeSettings look = engine.Options().Theme;
            look.Dirty = false;
            if (const std::optional<ThemeFamily> family = Themes::PresetNamed(name))
                Themes::SelectPreset(look, *family);
            else
            {
                library.Refresh();
                const Themes::CustomTheme* theme = library.Find(name);
                if (!theme)
                    return PushFailure(state, ErrorCode::ThemeNotFound, name);
                if (EqualsNoCase(engine.Options().Theme.Custom, theme->Id))
                    look = engine.Options().Theme;
                else
                    Themes::ToSettings(*theme, look);
                look.Dirty = false;
            }
            if (modeText)
            {
                const std::optional<ThemeMode> mode = ModeNamed(modeText);
                if (!mode)
                    return PushFailure(state, ErrorCode::ThemeInvalid, std::string("unknown mode ") + modeText);
                look.Mode = *mode;
            }
            library.SetScript(std::move(look), owner);
            lua_pushboolean(state, 1);
            return 1;
        }

        int ThemeOverride(lua_State* state)
        {
            Engine& engine = Owner(state);
            luaL_checktype(state, 1, LUA_TTABLE);
            const std::string owner = luaL_checkstring(state, 2);
            Themes::Library& library = engine.ThemeLibrary();
            std::map<std::string, Themes::Rgba> both;
            std::map<std::string, Themes::Rgba> dark;
            std::map<std::string, Themes::Rgba> light;
            std::map<std::string, float> sizes;
            std::string problem;
            const auto colors = [&](std::map<std::string, Themes::Rgba>& into) {
                return [&into](lua_State* s, int index, std::string& p) { return ReadColors(s, index, into, p); };
            };
            if (!ReadField(state, 1, "colors", colors(both), problem) || !ReadField(state, 1, "dark", colors(dark), problem) ||
                !ReadField(state, 1, "light", colors(light), problem) ||
                !ReadField(state, 1, "sizes", [&sizes](lua_State* s, int index, std::string& p) { return ReadSizes(s, index, sizes, p); }, problem))
                return PushFailure(state, ErrorCode::ThemeInvalid, problem);
            ThemeSettings look = library.Effective(engine.Options().Theme);
            look.Dirty = false;
            for (const auto& [name, rgba] : both)
            {
                SetColor(look, false, name, rgba);
                SetColor(look, true, name, rgba);
            }
            for (const auto& [name, rgba] : dark)
                SetColor(look, false, name, rgba);
            for (const auto& [name, rgba] : light)
                SetColor(look, true, name, rgba);
            for (const auto& [name, value] : sizes)
                look.Sizes[name] = value;
            library.SetScript(std::move(look), owner);
            lua_pushboolean(state, 1);
            return 1;
        }

        int ThemeRestore(lua_State* state)
        {
            const std::string owner = luaL_optstring(state, 1, "");
            lua_pushboolean(state, Owner(state).ThemeLibrary().ClearScript(owner) ? 1 : 0);
            return 1;
        }

        int ThemeSave(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = Trim(luaL_checkstring(state, 1));
            luaL_checktype(state, 2, LUA_TTABLE);
            const bool overwrite = lua_toboolean(state, 3) != 0;
            if (name.empty())
                return PushFailure(state, ErrorCode::ThemeInvalid, "the theme has no name");
            if (Themes::PresetNamed(name))
                return PushFailure(state, ErrorCode::ThemeExists, name + " (a preset)");
            Themes::Library& library = engine.ThemeLibrary();
            library.Refresh();
            Themes::CustomTheme theme;
            theme.Name = name;
            if (const Themes::CustomTheme* existing = library.Find(name); existing && EqualsNoCase(existing->Name, name))
            {
                if (!overwrite)
                    return PushFailure(state, ErrorCode::ThemeExists, name);
                theme.Id = existing->Id;
            }
            else if (library.NameTaken(name))
                return PushFailure(state, ErrorCode::ThemeExists, name + " (its file name is taken)");
            lua_getfield(state, 2, "base");
            if (!lua_isnil(state, -1))
            {
                const char* base = lua_tostring(state, -1);
                const std::optional<ThemeFamily> family = base ? Themes::PresetNamed(base) : std::nullopt;
                if (!family)
                {
                    lua_pop(state, 1);
                    return PushFailure(state, ErrorCode::ThemeInvalid, std::string("unknown base ") + (base ? base : "(not a string)"));
                }
                theme.Base = *family;
            }
            lua_pop(state, 1);
            std::string problem;
            const auto colors = [&](std::map<std::string, Themes::Rgba>& into) {
                return [&into](lua_State* s, int index, std::string& p) { return ReadColors(s, index, into, p); };
            };
            if (!ReadField(state, 2, "dark", colors(theme.Dark), problem) || !ReadField(state, 2, "light", colors(theme.Light), problem) ||
                !ReadField(state, 2, "sizes", [&theme](lua_State* s, int index, std::string& p) { return ReadSizes(s, index, theme.Sizes, p); }, problem))
                return PushFailure(state, ErrorCode::ThemeInvalid, problem);
            ThemeSettings& saved = engine.Options().Theme;
            const bool active = !theme.Id.empty() && EqualsNoCase(saved.Custom, theme.Id);
            const Status written = library.Save(theme);
            if (!written.Ok())
                return PushFailure(state, written);
            if (active)
            {
                // ∑ the user's own theme was replaced on disk: show what the file holds now
                Themes::ToSettings(theme, saved);
                library.RequestRestyle();
            }
            lua_pushboolean(state, 1);
            return 1;
        }
    }

    void RegisterThemeNatives(lua_State* state, int natives)
    {
        const luaL_Reg functions[] = {
            { "uiThemeSizes", &ThemeSizes },
            { "uiThemeCurrent", &ThemeCurrent },
            { "uiThemeList", &ThemeList },
            { "uiThemeColor", &ThemeColor },
            { "uiThemeDefinition", &ThemeDefinition },
            { "uiThemeApply", &ThemeApply },
            { "uiThemeOverride", &ThemeOverride },
            { "uiThemeRestore", &ThemeRestore },
            { "uiThemeSave", &ThemeSave },
            { nullptr, nullptr },
        };
        const int absolute = lua_absindex(state, natives);
        for (const luaL_Reg* entry = functions; entry->name; ++entry)
        {
            lua_pushcfunction(state, entry->func);
            lua_setfield(state, absolute, entry->name);
        }
    }
}
