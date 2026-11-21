#include "Lua/UiForm.h"

#include "Core/Text.h"
#include "Lua/LuaUi.h"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <regex>
#include <set>

namespace Manifold::Lua
{
    namespace
    {
        using T = FormPropType;

        constexpr int kMaxDepth = 32;
        constexpr int kMaxNodes = 20000;
        constexpr int kInstructionLimit = 2000000;

        constexpr FormProp kCommon[] = {
            { "visible", T::Bool, "Shown. A hidden widget keeps its place in the script and can be shown with update." },
            { "enabled", T::Bool, "Takes input. A disabled widget is drawn dimmed." },
            { "tooltip", T::String, "Text shown while the mouse rests on the widget." },
            { "width", T::Number, "Width in points. 0 is automatic, a negative width fills the rest of the line." },
            { "height", T::Number, "Height in points. 0 is automatic." },
            { "minWidth", T::Number, "Fields, buttons and lists are never narrower than this; 0 for no limit." },
            { "maxWidth", T::Number, "Fields, buttons and lists are never wider than this, and a text wraps here; 0 for no limit." },
        };

        constexpr FormProp kWindow[] = {
            { "title", T::String, "The window's title." },
            { "icon", T::Icon, "Font Awesome icon in front of the title." },
            { "width", T::Number, "First width in points." },
            { "height", T::Number, "First height in points." },
            { "dock", T::Bool, "Can dock into the workspace like Manifold's own windows." },
            { "opacity", T::Number, "0.1 to 1 while the window has the focus; 0 keeps the user's setting." },
            { "inactiveOpacity", T::Number, "0.1 to 1 while another window has the focus; 0 keeps the user's setting." },
            { "interval", T::Number, "Seconds between onUpdate calls; 0 is every frame." },
            { "minWidth", T::Number, "The user cannot make the window narrower; 0 for the program's minimum." },
            { "minHeight", T::Number, "The user cannot make the window lower; 0 for the program's minimum." },
            { "maxWidth", T::Number, "The user cannot make the window wider; 0 for no limit." },
            { "maxHeight", T::Number, "The user cannot make the window higher; 0 for no limit." },
            { "resizable", T::Bool, "The user can change the size." },
            { "autoSize", T::Bool, "The window fits itself to its widgets." },
            { "scrollbar", T::Bool, "Shows a scroll bar when the widgets do not fit." },
            { "topmost", T::Bool, "Stays above other windows while floating; it does not dock." },
            { "position", T::Choice, "Where the window opens the first time.", "default|center|mouse" },
        };

        constexpr FormProp kRow[] = {
            { "align", T::Choice, "Where the row's widgets sit in the line.", "left|center|right" },
            { "gap", T::Number, "Space between the widgets in points; empty for the usual space." },
        };
        constexpr FormProp kColumn[] = {
            { "gap", T::Number, "Space between the widgets in points; empty for the usual space." },
            { "scroll", T::Bool, "Scrolls inside its height (200 when the height is 0)." },
        };
        constexpr FormProp kGrid[] = { { "columns", T::Integer, "Number of columns. Widgets fill the cells row by row; the first column fits its content, the others share the rest." } };
        constexpr FormProp kSection[] = {
            { "title", T::String, "Title of the card." },
            { "icon", T::Icon, "Font Awesome icon in front of the title." },
            { "badge", T::String, "Short text at the right of the title." },
            { "collapsible", T::Bool, "The user can fold the section." },
            { "open", T::Bool, "A collapsible section starts unfolded." },
            { "gap", T::Number, "Space between the widgets in points; empty for the usual space." },
        };
        constexpr FormProp kTab[] = {
            { "title", T::String, "Text on the tab." },
            { "icon", T::Icon, "Font Awesome icon on the tab." },
            { "gap", T::Number, "Space between the widgets in points; empty for the usual space." },
            { "scroll", T::Bool, "Scrolls inside its height (200 when the height is 0)." },
        };
        constexpr FormProp kSplit[] = {
            { "vertical", T::Bool, "Stacks the two parts instead of placing them side by side." },
            { "ratio", T::Number, "Share of the first part, 0.05 to 0.95." },
        };
        constexpr FormProp kText[] = {
            { "text", T::Text, "The text." },
            { "color", T::Color, "Theme colour name or #RRGGBB." },
            { "wrap", T::Bool, "Wraps at the edge of the window." },
            { "mono", T::Bool, "Fixed-width font." },
        };
        constexpr FormProp kHeading[] = {
            { "text", T::String, "The heading." },
            { "icon", T::Icon, "Font Awesome icon in front of it." },
        };
        constexpr FormProp kSeparator[] = { { "text", T::String, "Optional text inside the line." } };
        constexpr FormProp kButton[] = {
            { "text", T::String, "Text on the button." },
            { "icon", T::Icon, "Font Awesome icon in front of the text." },
            { "style", T::Choice, "Look of the button.", "default|accent|danger" },
            { "shortcut", T::String, "Keys that click it while its window has the focus, like Ctrl+S or F5." },
            { "confirm", T::String, "Asks this before the click goes through; empty for no question." },
        };
        constexpr FormProp kCheckbox[] = {
            { "label", T::String, "Text beside the box." },
            { "value", T::Bool, "Ticked at the start." },
        };
        constexpr FormProp kInput[] = {
            { "label", T::String, "Text beside the field." },
            { "value", T::Text, "Text in the field at the start." },
            { "hint", T::String, "Grey text while the field is empty." },
            { "multiline", T::Bool, "Several lines." },
            { "password", T::Bool, "Shows dots instead of the text." },
            { "readOnly", T::Bool, "The text can be selected and copied, not changed." },
            { "maxLength", T::Integer, "Most characters; 0 for no limit." },
            { "focus", T::Bool, "The cursor is in this field when the window opens." },
        };
        constexpr FormProp kNumber[] = {
            { "label", T::String, "Text beside the field." },
            { "value", T::Number, "Value at the start." },
            { "min", T::Number, "Smallest value; empty for none." },
            { "max", T::Number, "Largest value; empty for none." },
            { "step", T::Number, "Step of the - and + buttons; 0 hides them for decimals." },
            { "integer", T::Bool, "Whole numbers only." },
            { "hex", T::Bool, "Shown and typed as hexadecimal (whole numbers)." },
            { "format", T::String, "printf format for decimals, like %.2f." },
            { "focus", T::Bool, "The cursor is in this field when the window opens." },
        };
        constexpr FormProp kSlider[] = {
            { "label", T::String, "Text beside the slider." },
            { "value", T::Number, "Value at the start." },
            { "min", T::Number, "Left end (0 when empty)." },
            { "max", T::Number, "Right end (1, or 100 for whole numbers, when empty)." },
            { "integer", T::Bool, "Whole numbers only." },
            { "format", T::String, "printf format, like %.1f x." },
        };
        constexpr FormProp kCombo[] = {
            { "label", T::String, "Text beside the box." },
            { "items", T::Items, "The choices, one per line." },
            { "value", T::Integer, "Number of the chosen item at the start; 0 for none." },
        };
        constexpr FormProp kRadio[] = {
            { "label", T::String, "Text beside the group." },
            { "items", T::Items, "The choices, one per line." },
            { "value", T::Integer, "Number of the chosen item at the start; 0 for none." },
            { "horizontal", T::Bool, "Places the choices in one line." },
        };
        constexpr FormProp kColor[] = {
            { "label", T::String, "Text beside the colour." },
            { "value", T::Color, "Colour at the start, #RRGGBB or #RRGGBBAA." },
            { "alpha", T::Bool, "Has an alpha channel; the value is #RRGGBBAA." },
        };
        constexpr FormProp kTable[] = {
            { "columns", T::Columns, "One column per line: title | width | align (width and align are optional)." },
            { "rows", T::Rows, "One row per line, cells separated by |." },
            { "value", T::Integer, "Number of the selected row at the start; 0 for none." },
            { "menu", T::Items, "Context menu entries, one per line; - is a separator." },
        };
        constexpr FormProp kTree[] = {
            { "items", T::Tree, "One item per line, two spaces deeper per level: text | id | icon | open." },
            { "value", T::String, "Id of the selected item at the start." },
            { "menu", T::Items, "Context menu entries, one per line; - is a separator." },
        };
        constexpr FormProp kProgress[] = {
            { "value", T::Number, "Fraction 0 to 1." },
            { "text", T::String, "Text on the bar." },
        };
        constexpr FormProp kImage[] = {
            { "image", T::File, "Picture file: PNG, JPEG, BMP, TGA or GIF. A relative path starts at the form file's folder with manifold.ui.form, at the program folder in a standalone script." },
        };
        constexpr FormProp kCanvas[] = {
            { "background", T::Color, "Theme colour name or #RRGGBB behind the drawings." },
            { "zoom", T::Bool, "The user can zoom with the wheel and pan by dragging." },
            { "menu", T::Items, "Context menu entries, one per line; - is a separator." },
        };

        template <std::size_t N>
        constexpr std::array<FormProp, N + std::size(kCommon)> WithCommon(const FormProp (&own)[N])
        {
            std::array<FormProp, N + std::size(kCommon)> all{};
            for (std::size_t i = 0; i < N; ++i)
                all[i] = own[i];
            for (std::size_t i = 0; i < std::size(kCommon); ++i)
                all[N + i] = kCommon[i];
            return all;
        }

        constexpr auto kRowAll = WithCommon(kRow);
        constexpr auto kColumnAll = WithCommon(kColumn);
        constexpr auto kGridAll = WithCommon(kGrid);
        constexpr auto kSectionAll = WithCommon(kSection);
        constexpr auto kTabAll = WithCommon(kTab);
        constexpr auto kSplitAll = WithCommon(kSplit);
        constexpr auto kTextAll = WithCommon(kText);
        constexpr auto kHeadingAll = WithCommon(kHeading);
        constexpr auto kSeparatorAll = WithCommon(kSeparator);
        constexpr auto kButtonAll = WithCommon(kButton);
        constexpr auto kCheckboxAll = WithCommon(kCheckbox);
        constexpr auto kInputAll = WithCommon(kInput);
        constexpr auto kNumberAll = WithCommon(kNumber);
        constexpr auto kSliderAll = WithCommon(kSlider);
        constexpr auto kComboAll = WithCommon(kCombo);
        constexpr auto kRadioAll = WithCommon(kRadio);
        constexpr auto kColorAll = WithCommon(kColor);
        constexpr auto kTableAll = WithCommon(kTable);
        constexpr auto kTreeAll = WithCommon(kTree);
        constexpr auto kProgressAll = WithCommon(kProgress);
        constexpr auto kCanvasAll = WithCommon(kCanvas);
        constexpr auto kImageAll = WithCommon(kImage);

        constexpr FormHandler kWindowHandlers[] = {
            { "onClose", "window", "The user closed the window (it is hidden, show brings it back)." },
            { "onUpdate", "window", "Every interval seconds while the window is open." },
        };
        constexpr FormHandler kButtonHandlers[] = { { "onClick", "widget", "The button was clicked." } };
        constexpr FormHandler kChangeHandlers[] = { { "onChange", "value, widget", "The user changed the value." } };
        constexpr FormHandler kInputHandlers[] = {
            { "onChange", "text, widget", "The user changed the text." },
            { "onSubmit", "text, widget", "The user pressed Enter." },
        };
        constexpr FormHandler kTableHandlers[] = {
            { "onSelect", "row, widget", "A row was selected." },
            { "onActivate", "row, widget", "A row was double-clicked." },
            { "onMenu", "choice, text, row, widget", "A context menu entry was chosen." },
        };
        constexpr FormHandler kTreeHandlers[] = {
            { "onSelect", "id, widget", "An item was selected." },
            { "onActivate", "id, widget", "An item was double-clicked." },
            { "onMenu", "choice, text, id, widget", "A context menu entry was chosen." },
        };
        constexpr FormHandler kCanvasHandlers[] = {
            { "onClick", "x, y, button, widget", "A click, in canvas coordinates." },
            { "onHover", "x, y, widget", "The mouse moved over the canvas." },
            { "onMenu", "choice, text, x, y, widget", "A context menu entry was chosen." },
            { "onView", "x, y, scale, widget", "The user zoomed or panned." },
            { "onKey", "key, widget", "A key was pressed while the window has the focus." },
        };

        constexpr UiKind kPalette[] = {
            UiKind::Row, UiKind::Column, UiKind::Grid, UiKind::Section, UiKind::Tabs, UiKind::Tab, UiKind::Split,
            UiKind::Text, UiKind::Heading, UiKind::Separator, UiKind::Spacer,
            UiKind::Button, UiKind::Checkbox, UiKind::Input, UiKind::Number, UiKind::Slider, UiKind::Combo, UiKind::Radio, UiKind::Color,
            UiKind::Table, UiKind::Tree, UiKind::Progress, UiKind::Image, UiKind::Canvas,
        };

        constexpr std::string_view kToolWindow = R"FORM(return {
  form = 1,
  title = "My Tool",
  icon = "tools",
  width = 420,
  height = 330,
  key = "my tool",
  children = {
    {
      kind = "section",
      title = "Options",
      icon = "sliders-h",
      children = {
        {
          kind = "grid",
          children = {
            { kind = "text", text = "Enabled" },
            { kind = "checkbox", name = "enabled", onChange = "enabledChanged" },
            { kind = "text", text = "Amount" },
            { kind = "slider", name = "amount", value = 50, max = 100, integer = true, onChange = "amountChanged" },
            { kind = "text", text = "Name" },
            { kind = "input", name = "name", hint = "Name" },
          },
        },
      },
    },
    {
      kind = "row",
      align = "right",
      children = {
        { kind = "button", name = "reset", text = "Reset", icon = "undo", onClick = "resetClicked" },
        { kind = "button", name = "apply", text = "Apply", icon = "check", style = "accent", onClick = "applyClicked" },
      },
    },
  },
})FORM";

        constexpr std::string_view kSettingsDialog = R"FORM(return {
  form = 1,
  title = "Settings",
  icon = "cog",
  width = 460,
  height = 360,
  key = "settings",
  children = {
    {
      kind = "tabs",
      children = {
        {
          kind = "tab",
          title = "General",
          icon = "sliders-h",
          children = {
            {
              kind = "grid",
              children = {
                { kind = "text", text = "Language" },
                { kind = "combo", name = "language", items = { "English", "Deutsch" }, value = 1 },
                { kind = "text", text = "Refresh (ms)" },
                { kind = "number", name = "refresh", value = 250, min = 50, max = 5000, step = 50, integer = true },
                { kind = "text", text = "Start Hidden" },
                { kind = "checkbox", name = "hidden" },
              },
            },
          },
        },
        {
          kind = "tab",
          title = "Look",
          icon = "palette",
          children = {
            {
              kind = "grid",
              children = {
                { kind = "text", text = "Accent" },
                { kind = "color", name = "accent", value = "#4C9AFF" },
                { kind = "text", text = "Density" },
                { kind = "radio", name = "density", items = { "Compact", "Normal", "Roomy" }, value = 2, horizontal = true },
              },
            },
          },
        },
      },
    },
    { kind = "separator" },
    {
      kind = "row",
      align = "right",
      children = {
        { kind = "button", name = "cancel", text = "Cancel", onClick = "cancelClicked" },
        { kind = "button", name = "save", text = "Save", icon = "save", style = "accent", onClick = "saveClicked" },
      },
    },
  },
})FORM";

        constexpr std::string_view kListDetails = R"FORM(return {
  form = 1,
  title = "Browser",
  icon = "list",
  width = 640,
  height = 420,
  key = "browser",
  children = {
    {
      kind = "row",
      children = {
        { kind = "input", name = "search", hint = "Search", width = 260, onChange = "searchChanged" },
        { kind = "button", name = "refresh", text = "Refresh", icon = "undo", onClick = "refreshClicked" },
      },
    },
    {
      kind = "split",
      ratio = 0.45,
      children = {
        {
          kind = "column",
          children = {
            { kind = "table", name = "list", columns = { "Name", "Value" }, rows = { { "First", "1" }, { "Second", "2" } }, height = 320, onSelect = "listSelected" },
          },
        },
        {
          kind = "column",
          children = {
            { kind = "heading", text = "Details", icon = "info-circle" },
            { kind = "text", name = "details", text = "Select an entry on the left.", color = "muted", wrap = true },
          },
        },
      },
    },
  },
})FORM";

        constexpr std::string_view kCheatList = R"FORM(return {
  form = 1,
  title = "Cheats",
  icon = "gamepad",
  width = 380,
  height = 380,
  key = "cheats",
  children = {
    {
      kind = "section",
      title = "Player",
      icon = "user",
      collapsible = true,
      children = {
        { kind = "checkbox", name = "godMode", label = "God Mode", onChange = "godModeChanged" },
        { kind = "checkbox", name = "infiniteAmmo", label = "Infinite Ammo", onChange = "infiniteAmmoChanged" },
        { kind = "slider", name = "speed", label = "Speed", value = 1, min = 0.5, max = 5, format = "%.1f x", onChange = "speedChanged" },
      },
    },
    {
      kind = "section",
      title = "World",
      icon = "globe",
      collapsible = true,
      children = {
        { kind = "checkbox", name = "freezeTime", label = "Freeze Time", onChange = "freezeTimeChanged" },
        { kind = "button", name = "teleport", text = "Teleport to Waypoint", icon = "map-marker-alt", width = -1, onClick = "teleportClicked" },
      },
    },
  },
})FORM";

        constexpr std::string_view kLabelField = R"FORM(return {
  form = 1,
  fragment = true,
  title = "",
  children = {
    { kind = "text", text = "Label" },
    { kind = "input", name = "field", hint = "Value" },
  },
})FORM";

        constexpr std::string_view kOkCancel = R"FORM(return {
  form = 1,
  fragment = true,
  title = "",
  children = {
    {
      kind = "row",
      align = "right",
      children = {
        { kind = "button", name = "cancel", text = "Cancel", onClick = "cancelClicked" },
        { kind = "button", name = "ok", text = "OK", style = "accent", onClick = "okClicked" },
      },
    },
  },
})FORM";

        constexpr std::string_view kSearchBar = R"FORM(return {
  form = 1,
  fragment = true,
  title = "",
  children = {
    {
      kind = "row",
      children = {
        { kind = "input", name = "search", hint = "Search", width = 240, onSubmit = "searchSubmitted" },
        { kind = "button", name = "find", text = "Find", icon = "search", onClick = "findClicked" },
      },
    },
  },
})FORM";

        constexpr std::string_view kSectionGrid = R"FORM(return {
  form = 1,
  fragment = true,
  title = "",
  children = {
    {
      kind = "section",
      title = "Section",
      children = {
        {
          kind = "grid",
          children = {
            { kind = "text", text = "First" },
            { kind = "input", name = "first" },
            { kind = "text", text = "Second" },
            { kind = "number", name = "second" },
          },
        },
      },
    },
  },
})FORM";

        constexpr std::string_view kStatusLine = R"FORM(return {
  form = 1,
  fragment = true,
  title = "",
  children = {
    {
      kind = "row",
      children = {
        { kind = "progress", name = "progress", value = 0.4, width = 160 },
        { kind = "text", name = "status", text = "Working...", color = "muted" },
      },
    },
  },
})FORM";

        constexpr FormTemplate kTemplates[] = {
            { "tool", "Tool Window", "A section with labelled fields and Reset and Apply buttons.", false, kToolWindow },
            { "settings", "Settings Dialog", "Tabs with fields, Cancel and Save.", false, kSettingsDialog },
            { "browser", "List and Details", "A search field, a table and a details pane side by side.", false, kListDetails },
            { "cheats", "Cheat List", "Foldable sections of switches, a slider and a button.", false, kCheatList },
            { "labelfield", "Label and Field", "A text and a field: one row of a grid.", true, kLabelField },
            { "okcancel", "OK and Cancel", "Two buttons at the right of a row.", true, kOkCancel },
            { "search", "Search Bar", "A search field with a Find button.", true, kSearchBar },
            { "sectiongrid", "Section with Fields", "A card with a grid of two labelled fields.", true, kSectionGrid },
            { "status", "Status Line", "A progress bar and a status text.", true, kStatusLine },
        };


        constexpr std::string_view kKeywords[] = {
            "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
            "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while",
        };

        UiNode Defaults(UiKind kind)
        {
            UiNode node;
            node.Kind = kind;
            return node;
        }

        std::string NumberText(double value)
        {
            if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 1.0e15)
                return std::to_string(static_cast<long long>(value));
            char buffer[64];
            const auto done = std::to_chars(buffer, buffer + sizeof(buffer), value);
            return std::string(buffer, done.ptr);
        }

        bool ParseNumber(std::string_view text, double& value)
        {
            const std::string trimmed = Trim(text);
            if (trimmed.empty())
                return false;
            const char* begin = trimmed.data();
            const char* end = begin + trimmed.size();
            if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X'))
            {
                unsigned long long whole = 0;
                const auto done = std::from_chars(begin + 2, end, whole, 16);
                if (done.ec != std::errc() || done.ptr != end)
                    return false;
                value = static_cast<double>(whole);
                return true;
            }
            const auto done = std::from_chars(begin, end, value);
            return done.ec == std::errc() && done.ptr == end;
        }

        std::string Quote(std::string_view text)
        {
            std::string out = "\"";
            for (const char c : text)
            {
                switch (c)
                {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
                    {
                        char code[8];
                        std::snprintf(code, sizeof(code), "\\%03u", static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += code;
                    }
                    else
                        out.push_back(c);
                    break;
                }
            }
            out.push_back('"');
            return out;
        }

        std::string Join(const std::vector<std::string>& lines)
        {
            std::string out;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                if (i)
                    out.push_back('\n');
                out += lines[i];
            }
            return out;
        }

        std::vector<std::string> Lines(std::string_view text)
        {
            std::vector<std::string> lines;
            for (std::string& line : SplitLines(text))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(std::move(line));
            }
            while (!lines.empty() && Trim(lines.back()).empty())
                lines.pop_back();
            return lines;
        }

        std::vector<std::string> Cells(std::string_view line)
        {
            std::vector<std::string> cells;
            std::size_t start = 0;
            while (true)
            {
                const std::size_t bar = line.find('|', start);
                cells.push_back(Trim(line.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start)));
                if (bar == std::string_view::npos)
                    break;
                start = bar + 1;
            }
            return cells;
        }

        void TreeLines(const std::vector<UiTreeItem>& items, int depth, std::vector<std::string>& lines)
        {
            for (const UiTreeItem& item : items)
            {
                std::string line(static_cast<std::size_t>(depth) * 2, ' ');
                line += item.Text;
                const bool id = item.Id != item.Text;
                if (id || !item.Icon.empty() || item.Open)
                    line += " | " + (id ? item.Id : std::string());
                if (!item.Icon.empty() || item.Open)
                    line += " | " + item.Icon;
                if (item.Open)
                    line += " | open";
                lines.push_back(std::move(line));
                TreeLines(item.Children, depth + 1, lines);
            }
        }

        std::vector<UiTreeItem> ParseTree(const std::vector<std::string>& lines)
        {
            std::vector<UiTreeItem> root;
            std::vector<std::pair<int, std::vector<UiTreeItem>*>> stack{ { -1, &root } };
            for (const std::string& line : lines)
            {
                if (Trim(line).empty())
                    continue;
                int indent = 0;
                for (const char c : line)
                {
                    if (c == ' ')
                        ++indent;
                    else if (c == '\t')
                        indent += 2;
                    else
                        break;
                }
                const int depth = std::min(indent / 2, kMaxDepth);
                const std::vector<std::string> cells = Cells(TrimView(line));
                UiTreeItem item;
                item.Text = cells[0];
                item.Id = cells.size() > 1 && !cells[1].empty() ? cells[1] : item.Text;
                item.Icon = cells.size() > 2 ? cells[2] : std::string();
                item.Open = cells.size() > 3 && EqualsNoCase(cells[3], "open");
                while (stack.size() > 1 && stack.back().first >= depth)
                    stack.pop_back();
                std::vector<UiTreeItem>* list = stack.back().second;
                list->push_back(std::move(item));
                stack.push_back({ depth, &list->back().Children });
            }
            return root;
        }

        std::string TreeLiteral(const std::vector<UiTreeItem>& items)
        {
            std::string out = "{ ";
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                const UiTreeItem& item = items[i];
                if (i)
                    out += ", ";
                if (item.Id == item.Text && item.Icon.empty() && !item.Open && item.Children.empty())
                {
                    out += Quote(item.Text);
                    continue;
                }
                out += "{ text = " + Quote(item.Text);
                if (item.Id != item.Text)
                    out += ", id = " + Quote(item.Id);
                if (!item.Icon.empty())
                    out += ", icon = " + Quote(item.Icon);
                if (item.Open)
                    out += ", open = true";
                if (!item.Children.empty())
                    out += ", children = " + TreeLiteral(item.Children);
                out += " }";
            }
            out += " }";
            return out;
        }

        std::string StringsLiteral(const std::vector<std::string>& items)
        {
            std::string out = "{ ";
            for (std::size_t i = 0; i < items.size(); ++i)
                out += (i ? ", " : "") + Quote(items[i]);
            out += " }";
            return out;
        }

        const FormProp* FindProp(UiKind kind, std::string_view key)
        {
            for (const FormProp& prop : FormProps(kind))
            {
                if (prop.Key == key)
                    return &prop;
            }
            return nullptr;
        }

        const FormHandler* FindHandler(UiKind kind, std::string_view key)
        {
            for (const FormHandler& handler : FormHandlers(kind))
            {
                if (handler.Key == key)
                    return &handler;
            }
            return nullptr;
        }

        void Seed(UiNode& node)
        {
            switch (node.Kind)
            {
            case UiKind::Section: node.Title = "Section"; break;
            case UiKind::Tab: node.Title = "Tab"; break;
            case UiKind::Text: node.Text = "Text"; break;
            case UiKind::Heading: node.Text = "Heading"; break;
            case UiKind::Button: node.Text = "Button"; break;
            case UiKind::Checkbox: node.Label = "Checkbox"; break;
            case UiKind::Input: node.Label = "Input"; break;
            case UiKind::Number: node.Label = "Number"; break;
            case UiKind::Slider:
                node.Label = "Slider";
                node.Min = 0.0;
                node.Max = 100.0;
                node.HasMin = true;
                node.HasMax = true;
                break;
            case UiKind::Combo:
                node.Label = "Combo";
                node.Items = { "First", "Second", "Third" };
                node.Selected = 1;
                break;
            case UiKind::Radio:
                node.Label = "Radio";
                node.Items = { "First", "Second" };
                node.Selected = 1;
                node.Horizontal = true;
                break;
            case UiKind::Color:
                node.Label = "Color";
                node.String = "#4C9AFF";
                break;
            case UiKind::Table:
                node.Columns = { { "Name", 0.0f, {} }, { "Value", 0.0f, {} } };
                node.Rows = { { "First", "1" }, { "Second", "2" } };
                break;
            case UiKind::Tree:
                node.Tree = { { "root", "Root", {}, true, { { "child", "Child", {}, false, {} } } } };
                break;
            case UiKind::Progress: node.Fraction = 0.5f; break;
            case UiKind::Canvas:
                node.Height = 200.0f;
                node.Background = "surface";
                break;
            default: break;
            }
        }

        bool AutoName(UiKind kind)
        {
            switch (kind)
            {
            case UiKind::Window:
            case UiKind::Row:
            case UiKind::Column:
            case UiKind::Grid:
            case UiKind::Section:
            case UiKind::Image:
            case UiKind::Tabs:
            case UiKind::Tab:
            case UiKind::Split:
            case UiKind::Text:
            case UiKind::Heading:
            case UiKind::Separator:
            case UiKind::Spacer:
                return false;
            default:
                return true;
            }
        }

        bool IsDescendant(const FormDocument& form, int node, int ancestor)
        {
            for (int id = node; id != 0;)
            {
                if (id == ancestor)
                    return true;
                const UiNode* found = form.Model.Find(id);
                id = found ? found->Parent : 0;
            }
            return false;
        }

        struct Writer
        {
            std::string Out;

            void Line(int depth, std::string_view text)
            {
                Out.append(static_cast<std::size_t>(depth) * 2, ' ');
                Out += text;
                Out.push_back('\n');
            }
        };

        std::vector<std::pair<std::string, std::string>> NodeFields(const UiNode& node)
        {
            std::vector<std::pair<std::string, std::string>> fields;
            for (const FormProp& prop : FormProps(node.Kind))
            {
                if (!FormPropIsDefault(node, prop.Key))
                    fields.push_back({ std::string(prop.Key), FormPropLiteral(node, prop.Key) });
            }
            return fields;
        }

        const FormMeta* MetaOf(const FormDocument& form, int node)
        {
            const auto found = form.Meta.find(node);
            return found == form.Meta.end() ? nullptr : &found->second;
        }

        void SaveNode(const FormDocument& form, const UiNode& node, int depth, Writer& writer)
        {
            std::vector<std::string> parts{ "kind = " + Quote(UiKindName(node.Kind)) };
            const FormMeta* meta = MetaOf(form, node.Id);
            if (meta && !meta->Name.empty())
                parts.push_back("name = " + Quote(meta->Name));
            for (const auto& [key, literal] : NodeFields(node))
                parts.push_back(key + " = " + literal);
            if (meta)
            {
                for (const FormHandler& handler : FormHandlers(node.Kind))
                {
                    const auto found = meta->Handlers.find(std::string(handler.Key));
                    if (found != meta->Handlers.end() && !found->second.empty())
                        parts.push_back(std::string(handler.Key) + " = " + Quote(found->second));
                }
            }
            std::vector<const UiNode*> children;
            for (const int child : node.Children)
            {
                if (const UiNode* item = form.Model.Find(child))
                    children.push_back(item);
            }
            if (children.empty() && !UiIsContainer(node.Kind))
            {
                std::string line = "{ ";
                for (std::size_t i = 0; i < parts.size(); ++i)
                    line += (i ? ", " : "") + parts[i];
                line += " },";
                if (line.size() + static_cast<std::size_t>(depth) * 2 <= 110)
                {
                    writer.Line(depth, line);
                    return;
                }
            }
            writer.Line(depth, "{");
            for (const std::string& part : parts)
                writer.Line(depth + 1, part + ",");
            if (!children.empty())
            {
                writer.Line(depth + 1, "children = {");
                for (const UiNode* child : children)
                    SaveNode(form, *child, depth + 2, writer);
                writer.Line(depth + 1, "},");
            }
            writer.Line(depth, "},");
        }

        int Hook(lua_State* state)
        {
            return luaL_error(state, "the form file does too much work; it must be a plain table");
        }

        void CountHook(lua_State* state, lua_Debug*)
        {
            Hook(state);
        }

        struct Loader
        {
            lua_State* State = nullptr;
            FormDocument& Form;
            std::vector<std::string>* Warnings = nullptr;
            std::string Error;
            int Nodes = 0;

            void Warn(std::string text)
            {
                if (Warnings)
                    Warnings->push_back(std::move(text));
            }

            std::string Where(UiKind kind, const std::string& name) const
            {
                return std::string(UiKindName(kind)) + (name.empty() ? std::string() : " \"" + name + "\"");
            }

            bool ReadMeta(int table, UiNode& node, FormMeta& meta)
            {
                lua_pushnil(State);
                while (lua_next(State, table))
                {
                    if (lua_type(State, -2) != LUA_TSTRING)
                    {
                        lua_pop(State, 1);
                        continue;
                    }
                    const std::string key = lua_tostring(State, -2);
                    if (key == "name" || key == "kind" || key == "children" || key == "form" || (node.Kind == UiKind::Window && (key == "key" || key == "fragment" || key == "script")))
                    {
                        lua_pop(State, 1);
                        continue;
                    }
                    if (FindHandler(node.Kind, key))
                    {
                        if (lua_type(State, -1) != LUA_TSTRING || !FormIsIdentifier(lua_tostring(State, -1)))
                        {
                            Error = Where(node.Kind, meta.Name) + ": " + key + " must name a function, like \"onSave\"";
                            lua_pop(State, 2);
                            return false;
                        }
                        meta.Handlers[key] = lua_tostring(State, -1);
                    }
                    else if (!FindProp(node.Kind, key) && !(node.Kind == UiKind::Window && key == "open"))
                        Warn(Where(node.Kind, meta.Name) + ": " + key + " is not an option of a " + std::string(UiKindName(node.Kind)) + " and was left out");
                    lua_pop(State, 1);
                }
                return true;
            }

            bool ReadNode(int table, int parent, int depth)
            {
                if (depth > kMaxDepth)
                {
                    Error = "the form nests deeper than " + std::to_string(kMaxDepth) + " levels";
                    return false;
                }
                if (++Nodes > kMaxNodes)
                {
                    Error = "the form has more than " + std::to_string(kMaxNodes) + " widgets";
                    return false;
                }
                lua_getfield(State, table, "kind");
                const std::string kindName = lua_type(State, -1) == LUA_TSTRING ? lua_tostring(State, -1) : std::string();
                lua_pop(State, 1);
                const std::optional<UiKind> kind = UiKindFromName(kindName);
                const UiNode* above = Form.Model.Find(parent);
                if (!kind || *kind == UiKind::Window)
                {
                    Error = "unknown widget kind \"" + kindName + "\"";
                    return false;
                }
                const bool loose = Form.Fragment && parent == Form.Window;
                if (!above || !(loose || FormCanHold(above->Kind, *kind)))
                {
                    Error = "a " + std::string(UiKindName(above ? above->Kind : UiKind::Window)) + " cannot hold a " + kindName;
                    return false;
                }
                const int id = Form.Model.Create(*kind, parent, {});
                UiNode& node = *Form.Model.Find(id);
                FormMeta meta;
                lua_getfield(State, table, "name");
                if (lua_type(State, -1) == LUA_TSTRING)
                    meta.Name = lua_tostring(State, -1);
                lua_pop(State, 1);
                if (!meta.Name.empty() && !FormIsIdentifier(meta.Name))
                {
                    Error = Where(*kind, meta.Name) + ": the name must be a Lua name (letters, digits, _)";
                    return false;
                }
                ApplyUiProps(State, table, node);
                node.Handlers = 0;
                if (node.Kind == UiKind::Image)
                {
                    node.ImageId = 0;
                    lua_getfield(State, table, "image");
                    if (lua_type(State, -1) == LUA_TSTRING)
                        node.Source = lua_tostring(State, -1);
                    lua_pop(State, 1);
                }
                if (!ReadMeta(table, node, meta))
                    return false;
                if (!meta.Name.empty() || !meta.Handlers.empty())
                    Form.Meta[id] = std::move(meta);
                return ReadChildren(table, id, depth);
            }

            bool ReadChildren(int table, int parent, int depth)
            {
                const int type = lua_getfield(State, table, "children");
                if (type == LUA_TNIL)
                {
                    lua_pop(State, 1);
                    return true;
                }
                if (type != LUA_TTABLE)
                {
                    lua_pop(State, 1);
                    Error = "children must be a list of widgets";
                    return false;
                }
                const int list = lua_gettop(State);
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(State, list));
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    if (lua_rawgeti(State, list, i) != LUA_TTABLE)
                    {
                        lua_pop(State, 2);
                        Error = "every child must be a table with a kind";
                        return false;
                    }
                    const bool ok = ReadNode(lua_gettop(State), parent, depth + 1);
                    lua_pop(State, 1);
                    if (!ok)
                    {
                        lua_pop(State, 1);
                        return false;
                    }
                }
                lua_pop(State, 1);
                return true;
            }

            bool Run(int table)
            {
                lua_getfield(State, table, "form");
                const bool versioned = lua_isinteger(State, -1);
                const lua_Integer version = versioned ? lua_tointeger(State, -1) : 0;
                lua_pop(State, 1);
                if (!versioned)
                {
                    Error = "this is no form file: the table has no form = 1";
                    return false;
                }
                if (version > 1)
                {
                    Error = "the form file is from a newer Manifold (form = " + std::to_string(version) + ")";
                    return false;
                }
                lua_getfield(State, table, "fragment");
                Form.Fragment = lua_toboolean(State, -1) != 0;
                lua_pop(State, 1);
                UiNode& window = *Form.Model.Find(Form.Window);
                ApplyUiProps(State, table, window);
                window.Handlers = 0;
                lua_getfield(State, table, "key");
                if (lua_type(State, -1) == LUA_TSTRING)
                    Form.Key = lua_tostring(State, -1);
                lua_pop(State, 1);
                lua_getfield(State, table, "script");
                if (lua_type(State, -1) == LUA_TSTRING)
                    Form.Script = lua_tostring(State, -1);
                lua_pop(State, 1);
                FormMeta meta;
                if (!ReadMeta(table, window, meta))
                    return false;
                if (!meta.Handlers.empty())
                    Form.Meta[Form.Window] = std::move(meta);
                return ReadChildren(table, Form.Window, 0);
            }
        };

        struct ScriptWriter
        {
            const FormDocument& Form;
            std::string Body;
            std::map<int, std::string> Names;
            std::map<std::string, int> Counters;
            std::vector<std::pair<std::string, std::string_view>> Functions;
            std::set<std::string> Seen;

            std::string Variable(const UiNode& node)
            {
                const FormMeta* meta = MetaOf(Form, node.Id);
                if (meta && !meta->Name.empty())
                    return "w." + meta->Name;
                if (node.Kind == UiKind::Window)
                    return "window";
                if (node.Children.empty())
                    return {};
                const std::string kind(UiKindName(node.Kind));
                return kind + std::to_string(++Counters[kind]);
            }

            void Collect(const UiNode& node)
            {
                if (const FormMeta* meta = MetaOf(Form, node.Id))
                {
                    for (const FormHandler& handler : FormHandlers(node.Kind))
                    {
                        const auto found = meta->Handlers.find(std::string(handler.Key));
                        if (found != meta->Handlers.end() && !found->second.empty() && Seen.insert(found->second).second)
                            Functions.push_back({ found->second, handler.Params });
                    }
                }
                for (const int child : node.Children)
                {
                    if (const UiNode* item = Form.Model.Find(child))
                        Collect(*item);
                }
            }

            std::string Options(const UiNode& node, bool handlersAsNames)
            {
                std::vector<std::string> parts;
                for (const auto& [key, literal] : NodeFields(node))
                {
                    if (node.Kind == UiKind::Window && key == "title")
                        continue;
                    if (node.Kind == UiKind::Image && key == "image")
                        parts.push_back("image = ui.image.load(" + literal + ")");
                    else
                        parts.push_back(key + " = " + literal);
                }
                if (node.Kind == UiKind::Window && !Form.Key.empty())
                    parts.push_back("key = " + Quote(Form.Key));
                if (const FormMeta* meta = MetaOf(Form, node.Id))
                {
                    for (const FormHandler& handler : FormHandlers(node.Kind))
                    {
                        const auto found = meta->Handlers.find(std::string(handler.Key));
                        if (found != meta->Handlers.end() && !found->second.empty())
                            parts.push_back(std::string(handler.Key) + " = " + (handlersAsNames ? Quote(found->second) : found->second));
                    }
                }
                if (parts.empty())
                    return {};
                std::string out = "{ ";
                for (std::size_t i = 0; i < parts.size(); ++i)
                    out += (i ? ", " : "") + parts[i];
                return out + " }";
            }

            void Build(const UiNode& node, const std::string& parent)
            {
                for (const int child : node.Children)
                {
                    const UiNode* item = Form.Model.Find(child);
                    if (!item)
                        continue;
                    const std::string name = Variable(*item);
                    const std::string options = Options(*item, false);
                    std::string call = parent + ".add(" + Quote(UiKindName(item->Kind)) + (options.empty() ? std::string() : ", " + options) + ")";
                    if (name.empty())
                        Body += call + "\n";
                    else if (name.rfind("w.", 0) == 0)
                        Body += name + " = " + call + "\n";
                    else
                        Body += "local " + name + " = " + call + "\n";
                    if (!item->Children.empty())
                        Build(*item, name);
                }
            }

            std::string Stubs(bool table)
            {
                std::string out;
                for (const auto& [name, params] : Functions)
                {
                    if (table)
                        out += "function handlers." + name + "(" + std::string(params) + ")\nend\n\n";
                    else
                        out += "local function " + name + "(" + std::string(params) + ")\nend\n\n";
                }
                return out;
            }
        };
    }

    std::span<const FormProp> FormProps(UiKind kind)
    {
        switch (kind)
        {
        case UiKind::Window: return kWindow;
        case UiKind::Row: return kRowAll;
        case UiKind::Column: return kColumnAll;
        case UiKind::Tabs:
        case UiKind::Spacer:
            return kCommon;
        case UiKind::Grid: return kGridAll;
        case UiKind::Section: return kSectionAll;
        case UiKind::Tab: return kTabAll;
        case UiKind::Split: return kSplitAll;
        case UiKind::Text: return kTextAll;
        case UiKind::Heading: return kHeadingAll;
        case UiKind::Separator: return kSeparatorAll;
        case UiKind::Button: return kButtonAll;
        case UiKind::Checkbox: return kCheckboxAll;
        case UiKind::Input: return kInputAll;
        case UiKind::Number: return kNumberAll;
        case UiKind::Slider: return kSliderAll;
        case UiKind::Combo: return kComboAll;
        case UiKind::Radio: return kRadioAll;
        case UiKind::Color: return kColorAll;
        case UiKind::Table: return kTableAll;
        case UiKind::Tree: return kTreeAll;
        case UiKind::Progress: return kProgressAll;
        case UiKind::Canvas: return kCanvasAll;
        case UiKind::Image: return kImageAll;
        default: return {};
        }
    }

    std::span<const FormHandler> FormHandlers(UiKind kind)
    {
        switch (kind)
        {
        case UiKind::Window: return kWindowHandlers;
        case UiKind::Button: return kButtonHandlers;
        case UiKind::Checkbox:
        case UiKind::Number:
        case UiKind::Slider:
        case UiKind::Combo:
        case UiKind::Radio:
        case UiKind::Color:
            return kChangeHandlers;
        case UiKind::Input: return kInputHandlers;
        case UiKind::Table: return kTableHandlers;
        case UiKind::Tree: return kTreeHandlers;
        case UiKind::Canvas: return kCanvasHandlers;
        default: return {};
        }
    }

    std::span<const UiKind> FormKinds()
    {
        return kPalette;
    }

    std::string_view FormKindTitle(UiKind kind)
    {
        switch (kind)
        {
        case UiKind::Window: return "Window";
        case UiKind::Row: return "Row";
        case UiKind::Column: return "Column";
        case UiKind::Grid: return "Grid";
        case UiKind::Section: return "Section";
        case UiKind::Tabs: return "Tabs";
        case UiKind::Tab: return "Tab";
        case UiKind::Split: return "Split";
        case UiKind::Text: return "Text";
        case UiKind::Heading: return "Heading";
        case UiKind::Separator: return "Separator";
        case UiKind::Spacer: return "Spacer";
        case UiKind::Button: return "Button";
        case UiKind::Checkbox: return "Checkbox";
        case UiKind::Input: return "Input";
        case UiKind::Number: return "Number";
        case UiKind::Slider: return "Slider";
        case UiKind::Combo: return "Combo";
        case UiKind::Radio: return "Radio Group";
        case UiKind::Color: return "Color";
        case UiKind::Table: return "Table";
        case UiKind::Tree: return "Tree";
        case UiKind::Progress: return "Progress";
        case UiKind::Image: return "Image";
        case UiKind::Canvas: return "Canvas";
        }
        return "?";
    }

    std::string_view FormKindHelp(UiKind kind)
    {
        switch (kind)
        {
        case UiKind::Window: return "The tool window that holds the form.";
        case UiKind::Row: return "Places its widgets side by side.";
        case UiKind::Column: return "Places its widgets one below the other.";
        case UiKind::Grid: return "Places its widgets in cells, row by row: good for a label in front of each field.";
        case UiKind::Section: return "A card with a title around its widgets.";
        case UiKind::Tabs: return "Holds tabs; one is shown at a time.";
        case UiKind::Tab: return "One page of a tabs widget.";
        case UiKind::Split: return "Shows its first two widgets side by side with a bar to drag between them.";
        case UiKind::Text: return "A line or paragraph of text.";
        case UiKind::Heading: return "Bold text with an icon.";
        case UiKind::Separator: return "A line between widgets, optionally with text.";
        case UiKind::Spacer: return "Empty space.";
        case UiKind::Button: return "A button with onClick.";
        case UiKind::Checkbox: return "A box to tick.";
        case UiKind::Input: return "A text field.";
        case UiKind::Number: return "A number field with - and + buttons.";
        case UiKind::Slider: return "A slider between two values.";
        case UiKind::Combo: return "A drop-down list.";
        case UiKind::Radio: return "A group of choices, one of them picked.";
        case UiKind::Color: return "A colour field with a picker.";
        case UiKind::Table: return "Rows and columns, with selection and a context menu.";
        case UiKind::Tree: return "Items that fold, with selection and a context menu.";
        case UiKind::Progress: return "A progress bar.";
        case UiKind::Image: return "A picture from manifold.ui.image.load.";
        case UiKind::Canvas: return "A surface the script draws on.";
        }
        return {};
    }

    bool FormCanHold(UiKind parent, UiKind child)
    {
        if (child == UiKind::Window || !UiIsContainer(parent))
            return false;
        if (parent == UiKind::Tabs)
            return child == UiKind::Tab;
        return child != UiKind::Tab;
    }

    FormDocument NewForm(std::string title)
    {
        FormDocument form;
        form.Window = form.Model.Create(UiKind::Window, 0, {});
        UiNode& window = *form.Model.Find(form.Window);
        window.Title = std::move(title);
        window.Width = 420.0f;
        window.Height = 320.0f;
        return form;
    }

    FormDocument CopyForm(const FormDocument& form)
    {
        return form;
    }

    int FormAdd(FormDocument& form, UiKind kind, int parent, int index)
    {
        const UiNode* above = form.Model.Find(parent);
        if (!above || !FormCanHold(above->Kind, kind))
            return 0;
        const int id = form.Model.Create(kind, parent, {});
        if (!id)
            return 0;
        Seed(*form.Model.Find(id));
        if (index >= 0)
        {
            UiNode& holder = *form.Model.Find(parent);
            holder.Children.pop_back();
            holder.Children.insert(holder.Children.begin() + std::min<std::size_t>(static_cast<std::size_t>(index), holder.Children.size()), id);
        }
        if (AutoName(kind))
            form.Meta[id].Name = FormUniqueName(form, UiKindName(kind));
        if (kind == UiKind::Tabs)
            FormAdd(form, UiKind::Tab, id);
        if (kind == UiKind::Split)
        {
            FormAdd(form, UiKind::Column, id);
            FormAdd(form, UiKind::Column, id);
        }
        return id;
    }

    bool FormMove(FormDocument& form, int node, int parent, int index)
    {
        UiNode* item = form.Model.Find(node);
        UiNode* target = form.Model.Find(parent);
        if (!item || !target || item->Kind == UiKind::Window || IsDescendant(form, parent, node) || !FormCanHold(target->Kind, item->Kind))
            return false;
        UiNode* old = form.Model.Find(item->Parent);
        if (!old)
            return false;
        auto& from = old->Children;
        const auto at = std::find(from.begin(), from.end(), node);
        if (at == from.end())
            return false;
        const int oldIndex = static_cast<int>(at - from.begin());
        from.erase(at);
        if (old == target && index > oldIndex)
            --index;
        auto& to = target->Children;
        index = std::clamp(index < 0 ? static_cast<int>(to.size()) : index, 0, static_cast<int>(to.size()));
        to.insert(to.begin() + index, node);
        item->Parent = parent;
        form.Model.Touch(*old);
        form.Model.Touch(*target);
        return true;
    }

    bool FormRemove(FormDocument& form, int node)
    {
        if (node == form.Window)
            return false;
        std::function<void(int)> forget = [&](int id) {
            form.Meta.erase(id);
            if (const UiNode* item = form.Model.Find(id))
            {
                for (const int child : item->Children)
                    forget(child);
            }
        };
        forget(node);
        return form.Model.Remove(node);
    }

    int FormGraft(FormDocument& form, const FormDocument& from, int node, int parent, int index)
    {
        const UiNode* source = from.Model.Find(node);
        const UiNode* holder = form.Model.Find(parent);
        const bool loose = form.Fragment && parent == form.Window;
        if (!source || !holder || source->Kind == UiKind::Window || !(loose || FormCanHold(holder->Kind, source->Kind)))
            return 0;
        const UiNode original = *source;
        const std::optional<FormMeta> meta = MetaOf(from, node) ? std::optional<FormMeta>(*MetaOf(from, node)) : std::nullopt;
        const int id = form.Model.Create(original.Kind, parent, {});
        if (!id)
            return 0;
        UiNode& made = *form.Model.Find(id);
        made = original;
        made.Id = id;
        made.Parent = parent;
        made.Children.clear();
        made.Revision = 1;
        if (&from != &form)
            made.ImageId = 0;
        if (index >= 0)
        {
            UiNode& into = *form.Model.Find(parent);
            into.Children.pop_back();
            into.Children.insert(into.Children.begin() + std::min<std::size_t>(static_cast<std::size_t>(index), into.Children.size()), id);
        }
        if (meta)
        {
            FormMeta fresh = *meta;
            if (!fresh.Name.empty())
                fresh.Name = FormUniqueName(form, fresh.Name);
            form.Meta[id] = std::move(fresh);
        }
        for (const int child : original.Children)
            FormGraft(form, from, child, id, -1);
        return id;
    }

    int FormDuplicate(FormDocument& form, int node)
    {
        const UiNode* source = form.Model.Find(node);
        if (!source || node == form.Window)
            return 0;
        return FormGraft(form, form, node, source->Parent, FormIndexOf(form, node) + 1);
    }

    std::string FormFragment(const FormDocument& form, const std::vector<int>& nodes)
    {
        FormDocument piece = NewForm({});
        piece.Fragment = true;
        const std::set<int> chosen(nodes.begin(), nodes.end());
        for (const int id : nodes)
        {
            bool inside = false;
            for (const UiNode* up = form.Model.Find(id); up && !inside; up = form.Model.Find(up->Parent))
                inside = up->Id != id && chosen.count(up->Id) != 0;
            const UiNode* node = form.Model.Find(id);
            if (inside || !node || node->Kind == UiKind::Window)
                continue;
            FormGraft(piece, form, id, piece.Window, -1);
        }
        return FormSave(piece);
    }

    std::vector<int> FormPaste(FormDocument& form, std::string_view text, int parent, int index, std::string& error)
    {
        std::vector<int> made;
        FormDocument piece;
        if (!FormLoad(text, piece, error))
            return made;
        const UiNode* window = piece.Model.Find(piece.Window);
        const UiNode* holder = form.Model.Find(parent);
        if (!window || !holder)
        {
            error = "nothing to paste into";
            return made;
        }
        std::vector<std::string> refused;
        for (const int child : window->Children)
        {
            const UiNode* node = piece.Model.Find(child);
            if (!node)
                continue;
            const int id = FormGraft(form, piece, child, parent, index);
            if (!id)
            {
                refused.push_back(std::string(FormKindTitle(node->Kind)));
                continue;
            }
            made.push_back(id);
            if (index >= 0)
                ++index;
        }
        if (!refused.empty())
        {
            error = "a " + std::string(FormKindTitle(holder->Kind)) + " cannot hold ";
            for (std::size_t i = 0; i < refused.size(); ++i)
                error += (i ? ", " : "") + refused[i];
        }
        return made;
    }

    std::span<const FormTemplate> FormTemplates()
    {
        return kTemplates;
    }

    std::vector<std::pair<std::string, std::string>> FormHandlerList(const FormDocument& form)
    {
        ScriptWriter writer{ form };
        if (const UiNode* window = form.Model.Find(form.Window))
            writer.Collect(*window);
        std::vector<std::pair<std::string, std::string>> list;
        for (const auto& [name, params] : writer.Functions)
            list.push_back({ name, std::string(params) });
        return list;
    }

    FormMerge FormMergeScript(std::string_view script, const FormDocument& form)
    {
        FormMerge merge;
        merge.Text = std::string(script);
        static const std::regex kDefined(R"(handlers\s*(?:\.\s*([A-Za-z_][A-Za-z0-9_]*)|\[\s*["']([A-Za-z_][A-Za-z0-9_]*)["']\s*\])|function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
        static const std::regex kFormAnchor(R"((^|\n)[^\n]*\bform\s*\.\s*(load|build)\s*\()");
        static const std::regex kWindowAnchor(R"((^|\n)[^\n]*\bwindow\s*\.\s*create\s*\()");
        std::set<std::string> defined;
        for (auto it = std::sregex_iterator(merge.Text.begin(), merge.Text.end(), kDefined); it != std::sregex_iterator(); ++it)
        {
            for (int group = 1; group <= 3; ++group)
            {
                if ((*it)[group].matched)
                    defined.insert((*it)[group].str());
            }
        }
        // ∑ A script that loads the form gets handlers.X stubs (the handlers
        // table it passes to form.load); a standalone script that builds the
        // window itself gets local functions above window.create.
        std::smatch anchor;
        bool table = true;
        if (!std::regex_search(merge.Text, anchor, kFormAnchor))
        {
            if (!std::regex_search(merge.Text, anchor, kWindowAnchor))
                return merge;
            table = false;
        }
        merge.Anchored = true;
        std::string stubs;
        for (const auto& [name, params] : FormHandlerList(form))
        {
            if (defined.count(name))
                continue;
            stubs += (table ? "function handlers." : "local function ") + name + "(" + params + ")\nend\n\n";
            merge.Added.push_back(name);
        }
        if (stubs.empty())
            return merge;
        const std::size_t at = static_cast<std::size_t>(anchor.position(0)) + static_cast<std::size_t>(anchor.length(1));
        merge.Text.insert(at, stubs);
        return merge;
    }

    int FormFunctionLine(std::string_view script, std::string_view name)
    {
        if (!FormIsIdentifier(name))
            return 0;
        const std::string word(name);
        const std::regex pattern("(^|\\n)[^\\n]*(function\\s+(handlers\\s*\\.\\s*)?" + word + "\\s*\\(|handlers\\s*(\\.\\s*" + word + "|\\[\\s*[\"']" + word + "[\"']\\s*\\])\\s*=\\s*function)");
        const std::string text(script);
        std::smatch found;
        if (!std::regex_search(text, found, pattern))
            return 0;
        const std::size_t at = static_cast<std::size_t>(found.position(0)) + static_cast<std::size_t>(found.length(1));
        return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
    }

    int FormIndexOf(const FormDocument& form, int node)
    {
        const UiNode* item = form.Model.Find(node);
        const UiNode* parent = item ? form.Model.Find(item->Parent) : nullptr;
        if (!parent)
            return -1;
        const auto at = std::find(parent->Children.begin(), parent->Children.end(), node);
        return at == parent->Children.end() ? -1 : static_cast<int>(at - parent->Children.begin());
    }

    std::string FormUniqueName(const FormDocument& form, std::string_view base, int except)
    {
        const auto taken = [&](const std::string& name) {
            for (const auto& [id, meta] : form.Meta)
            {
                if (id != except && meta.Name == name)
                    return true;
            }
            return false;
        };
        const std::string wanted(base);
        if (FormIsIdentifier(wanted) && !UiKindFromName(wanted) && !taken(wanted))
            return wanted;
        std::string stem = wanted;
        while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back())))
            stem.pop_back();
        if (stem.empty() || !FormIsIdentifier(stem))
            stem = "widget";
        for (int n = 1;; ++n)
        {
            const std::string name = stem + std::to_string(n);
            if (!taken(name))
                return name;
        }
    }

    bool FormIsIdentifier(std::string_view name)
    {
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
            return false;
        for (const char c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return false;
        }
        return std::find(std::begin(kKeywords), std::end(kKeywords), name) == std::end(kKeywords);
    }

    std::string FormPropValue(const UiNode& node, std::string_view key)
    {
        const auto flag = [](bool value) { return std::string(value ? "true" : "false"); };
        if (key == "visible") return flag(node.Visible);
        if (key == "enabled") return flag(node.Enabled);
        if (key == "tooltip") return node.Tooltip;
        if (key == "width") return NumberText(node.Width);
        if (key == "height") return NumberText(node.Height);
        if (key == "title") return node.Title;
        if (key == "icon") return node.Icon;
        if (key == "dock") return flag(node.Dock);
        if (key == "opacity") return node.Opacity < 0.0f ? std::string("0") : NumberText(node.Opacity);
        if (key == "inactiveOpacity") return node.InactiveOpacity < 0.0f ? std::string("0") : NumberText(node.InactiveOpacity);
        if (key == "interval") return NumberText(node.Interval);
        if (key == "align") return node.Align.empty() ? std::string("left") : node.Align;
        if (key == "badge") return node.Badge;
        if (key == "collapsible") return flag(node.Collapsible);
        if (key == "open") return flag(node.Open);
        if (key == "vertical") return flag(node.Vertical);
        if (key == "horizontal") return flag(node.Horizontal);
        if (key == "ratio") return NumberText(node.Ratio);
        if (key == "text") return node.Text;
        if (key == "color") return node.Color;
        if (key == "wrap") return flag(node.Wrap);
        if (key == "mono") return flag(node.Mono);
        if (key == "style") return node.Style.empty() ? std::string("default") : node.Style;
        if (key == "label") return node.Label;
        if (key == "hint") return node.Hint;
        if (key == "multiline") return flag(node.Multiline);
        if (key == "password") return flag(node.Password);
        if (key == "min") return node.HasMin ? NumberText(node.Min) : std::string();
        if (key == "max") return node.HasMax ? NumberText(node.Max) : std::string();
        if (key == "step") return NumberText(node.Step);
        if (key == "integer") return flag(node.Integer);
        if (key == "hex") return flag(node.Hex);
        if (key == "format") return node.Format;
        if (key == "alpha") return flag(node.Alpha);
        if (key == "background") return node.Background;
        if (key == "zoom") return flag(node.Zoom);
        if (key == "image") return node.Source;
        if (key == "minWidth") return NumberText(node.MinWidth);
        if (key == "minHeight") return NumberText(node.MinHeight);
        if (key == "maxWidth") return NumberText(node.MaxWidth);
        if (key == "maxHeight") return NumberText(node.MaxHeight);
        if (key == "resizable") return flag(node.Resizable);
        if (key == "autoSize") return flag(node.AutoSize);
        if (key == "scrollbar") return flag(node.Scrollbar);
        if (key == "topmost") return flag(node.Topmost);
        if (key == "position") return node.Position.empty() ? std::string("default") : node.Position;
        if (key == "gap") return node.Gap < 0.0f ? std::string() : NumberText(node.Gap);
        if (key == "scroll") return flag(node.Scroll);
        if (key == "shortcut") return node.Shortcut;
        if (key == "confirm") return node.Confirm;
        if (key == "readOnly") return flag(node.ReadOnly);
        if (key == "maxLength") return std::to_string(node.MaxLength);
        if (key == "focus") return flag(node.Focus);
        if (key == "items")
        {
            if (node.Kind == UiKind::Tree)
            {
                std::vector<std::string> lines;
                TreeLines(node.Tree, 0, lines);
                return Join(lines);
            }
            return Join(node.Items);
        }
        if (key == "menu") return Join(node.Menu);
        if (key == "rows")
        {
            std::vector<std::string> lines;
            for (const auto& row : node.Rows)
            {
                std::string line;
                for (std::size_t i = 0; i < row.size(); ++i)
                    line += (i ? " | " : "") + row[i];
                lines.push_back(std::move(line));
            }
            return Join(lines);
        }
        if (key == "columns")
        {
            if (node.Kind == UiKind::Grid)
                return std::to_string(node.GridColumns);
            std::vector<std::string> lines;
            for (const UiColumn& column : node.Columns)
            {
                std::string line = column.Title;
                if (column.Width > 0.0f || !column.Align.empty())
                    line += " | " + (column.Width > 0.0f ? NumberText(column.Width) : std::string());
                if (!column.Align.empty())
                    line += " | " + column.Align;
                lines.push_back(std::move(line));
            }
            return Join(lines);
        }
        if (key == "value")
        {
            switch (node.Kind)
            {
            case UiKind::Checkbox: return flag(node.Bool);
            case UiKind::Input:
            case UiKind::Color:
                return node.String;
            case UiKind::Number:
            case UiKind::Slider:
                return NumberText(node.Number);
            case UiKind::Combo:
            case UiKind::Radio:
            case UiKind::Table:
                return std::to_string(node.Selected);
            case UiKind::Tree: return node.SelectedId;
            case UiKind::Progress: return NumberText(node.Fraction);
            default: return {};
            }
        }
        return {};
    }

    bool FormSetProp(UiNode& node, std::string_view key, std::string_view value)
    {
        const bool yes = value == "true";
        double number = 0.0;
        const bool numeric = ParseNumber(value, number);
        const float real = numeric ? static_cast<float>(number) : 0.0f;
        const std::string text(value);
        if (key == "visible") node.Visible = yes;
        else if (key == "enabled") node.Enabled = yes;
        else if (key == "tooltip") node.Tooltip = text;
        else if (key == "width") node.Width = real;
        else if (key == "height") node.Height = std::max(0.0f, real);
        else if (key == "title") node.Title = text;
        else if (key == "icon") node.Icon = Trim(text);
        else if (key == "dock") node.Dock = yes;
        else if (key == "opacity") node.Opacity = numeric && number > 0.0 ? std::clamp(real, 0.1f, 1.0f) : -1.0f;
        else if (key == "inactiveOpacity") node.InactiveOpacity = numeric && number > 0.0 ? std::clamp(real, 0.1f, 1.0f) : -1.0f;
        else if (key == "interval") node.Interval = numeric ? std::max(0.0, number) : 0.0;
        else if (key == "align") node.Align = value == "left" ? std::string() : text;
        else if (key == "badge") node.Badge = text;
        else if (key == "collapsible") node.Collapsible = yes;
        else if (key == "open") node.Open = yes;
        else if (key == "vertical") node.Vertical = yes;
        else if (key == "horizontal") node.Horizontal = yes;
        else if (key == "ratio") node.Ratio = numeric ? std::clamp(real, 0.05f, 0.95f) : 0.5f;
        else if (key == "text") node.Text = text;
        else if (key == "color") node.Color = Trim(text);
        else if (key == "wrap") node.Wrap = yes;
        else if (key == "mono") node.Mono = yes;
        else if (key == "style") node.Style = value == "default" ? std::string() : text;
        else if (key == "label") node.Label = text;
        else if (key == "hint") node.Hint = text;
        else if (key == "multiline") node.Multiline = yes;
        else if (key == "password") node.Password = yes;
        else if (key == "min")
        {
            node.HasMin = numeric;
            node.Min = numeric ? number : 0.0;
        }
        else if (key == "max")
        {
            node.HasMax = numeric;
            node.Max = numeric ? number : 0.0;
        }
        else if (key == "step") node.Step = numeric ? std::max(0.0, number) : 0.0;
        else if (key == "integer") node.Integer = yes;
        else if (key == "hex") node.Hex = yes;
        else if (key == "format") node.Format = text;
        else if (key == "alpha") node.Alpha = yes;
        else if (key == "background") node.Background = Trim(text);
        else if (key == "zoom") node.Zoom = yes;
        else if (key == "image") node.Source = Trim(text);
        else if (key == "minWidth") node.MinWidth = std::max(0.0f, real);
        else if (key == "minHeight") node.MinHeight = std::max(0.0f, real);
        else if (key == "maxWidth") node.MaxWidth = std::max(0.0f, real);
        else if (key == "maxHeight") node.MaxHeight = std::max(0.0f, real);
        else if (key == "resizable") node.Resizable = yes;
        else if (key == "autoSize") node.AutoSize = yes;
        else if (key == "scrollbar") node.Scrollbar = yes;
        else if (key == "topmost") node.Topmost = yes;
        else if (key == "position") node.Position = value == "default" ? std::string() : text;
        else if (key == "gap") node.Gap = numeric ? std::max(0.0f, real) : -1.0f;
        else if (key == "scroll") node.Scroll = yes;
        else if (key == "shortcut") node.Shortcut = Trim(text);
        else if (key == "confirm") node.Confirm = text;
        else if (key == "readOnly") node.ReadOnly = yes;
        else if (key == "maxLength") node.MaxLength = numeric ? std::clamp(static_cast<int>(number), 0, 1048576) : 0;
        else if (key == "focus") node.Focus = yes;
        else if (key == "items")
        {
            if (node.Kind == UiKind::Tree)
                node.Tree = ParseTree(Lines(value));
            else
                node.Items = Lines(value);
        }
        else if (key == "menu") node.Menu = Lines(value);
        else if (key == "rows")
        {
            node.Rows.clear();
            for (const std::string& line : Lines(value))
                node.Rows.push_back(Cells(line));
        }
        else if (key == "columns")
        {
            if (node.Kind == UiKind::Grid)
                node.GridColumns = numeric ? std::clamp(static_cast<int>(number), 1, 16) : 2;
            else
            {
                node.Columns.clear();
                for (const std::string& line : Lines(value))
                {
                    const std::vector<std::string> cells = Cells(line);
                    UiColumn column;
                    column.Title = cells[0];
                    double width = 0.0;
                    if (cells.size() > 1 && ParseNumber(cells[1], width))
                        column.Width = static_cast<float>(std::max(0.0, width));
                    if (cells.size() > 2)
                        column.Align = cells[2];
                    node.Columns.push_back(std::move(column));
                }
            }
        }
        else if (key == "value")
        {
            switch (node.Kind)
            {
            case UiKind::Checkbox: node.Bool = yes; break;
            case UiKind::Input: node.String = text; break;
            case UiKind::Color: node.String = Trim(text); break;
            case UiKind::Number:
            case UiKind::Slider:
                node.Number = numeric ? number : 0.0;
                break;
            case UiKind::Combo:
            case UiKind::Radio:
            case UiKind::Table:
                node.Selected = numeric ? std::max(0, static_cast<int>(number)) : 0;
                break;
            case UiKind::Tree: node.SelectedId = text; break;
            case UiKind::Progress: node.Fraction = numeric ? std::clamp(real, 0.0f, 1.0f) : 0.0f; break;
            default: return false;
            }
        }
        else
            return false;
        ++node.Revision;
        return true;
    }

    bool FormPropIsDefault(const UiNode& node, std::string_view key)
    {
        if (node.Kind == UiKind::Window && key == "title")
            return false;
        return FormPropValue(node, key) == FormPropValue(Defaults(node.Kind), key);
    }

    std::string FormPropDefault(UiKind kind, std::string_view key)
    {
        return FormPropValue(Defaults(kind), key);
    }

    std::string FormPropLiteral(const UiNode& node, std::string_view key)
    {
        const FormProp* prop = FindProp(node.Kind, key);
        if (!prop)
            return "nil";
        const std::string value = FormPropValue(node, key);
        switch (prop->Type)
        {
        case T::Bool:
            return value;
        case T::Number:
        case T::Integer:
            return value.empty() ? std::string("nil") : value;
        case T::Items:
            return StringsLiteral(key == "menu" ? node.Menu : node.Items);
        case T::Tree:
            return TreeLiteral(node.Tree);
        case T::Rows:
        {
            std::string out = "{ ";
            for (std::size_t i = 0; i < node.Rows.size(); ++i)
                out += (i ? ", " : "") + StringsLiteral(node.Rows[i]);
            return out + " }";
        }
        case T::Columns:
        {
            std::string out = "{ ";
            for (std::size_t i = 0; i < node.Columns.size(); ++i)
            {
                const UiColumn& column = node.Columns[i];
                out += i ? ", " : "";
                if (column.Width <= 0.0f && column.Align.empty())
                    out += Quote(column.Title);
                else
                {
                    out += "{ title = " + Quote(column.Title);
                    if (column.Width > 0.0f)
                        out += ", width = " + NumberText(column.Width);
                    if (!column.Align.empty())
                        out += ", align = " + Quote(column.Align);
                    out += " }";
                }
            }
            return out + " }";
        }
        default:
            return Quote(value);
        }
    }

    std::vector<FormProblem> FormProblems(const FormDocument& form)
    {
        std::vector<FormProblem> problems;
        std::map<std::string, int> names;
        std::map<std::string, std::string> signatures;
        std::function<void(int)> visit = [&](int id) {
            const UiNode* node = form.Model.Find(id);
            if (!node)
                return;
            const std::string kind(FormKindTitle(node->Kind));
            if (const FormMeta* meta = MetaOf(form, id))
            {
                if (!meta->Name.empty())
                {
                    if (!FormIsIdentifier(meta->Name))
                        problems.push_back({ id, kind + " \"" + meta->Name + "\": the name must be a Lua name (letters, digits, _, no keyword)" });
                    else if (const auto [at, fresh] = names.emplace(meta->Name, id); !fresh)
                        problems.push_back({ id, kind + " \"" + meta->Name + "\": another widget has this name" });
                }
                for (const FormHandler& handler : FormHandlers(node->Kind))
                {
                    const auto found = meta->Handlers.find(std::string(handler.Key));
                    if (found == meta->Handlers.end() || found->second.empty())
                        continue;
                    if (!FormIsIdentifier(found->second))
                        problems.push_back({ id, kind + ": " + std::string(handler.Key) + " \"" + found->second + "\" is no Lua name" });
                    else if (const auto [at, fresh] = signatures.emplace(found->second, std::string(handler.Params)); !fresh && at->second != handler.Params)
                        problems.push_back({ id, kind + ": " + found->second + " is also used for a handler with other arguments (" + at->second + ")" });
                }
            }
            if (node->Kind == UiKind::Tabs && node->Children.empty())
                problems.push_back({ id, "Tabs: holds no tab" });
            if (node->Kind == UiKind::Split && node->Children.size() != 2)
                problems.push_back({ id, "Split: shows its first two widgets; it has " + std::to_string(node->Children.size()) });
            if ((node->Kind == UiKind::Combo || node->Kind == UiKind::Radio) && node->Items.empty())
                problems.push_back({ id, kind + ": has no items" });
            if ((node->Kind == UiKind::Combo || node->Kind == UiKind::Radio) && node->Selected > static_cast<int>(node->Items.size()))
                problems.push_back({ id, kind + ": value " + std::to_string(node->Selected) + " is past the last item" });
            if (node->Kind == UiKind::Color && !node->String.empty() && node->String.front() != '#')
                problems.push_back({ id, "Color: the value must be #RRGGBB or #RRGGBBAA" });
            if (node->Kind == UiKind::Window && node->Title.empty())
                problems.push_back({ id, "Window: has no title" });
            for (const int child : node->Children)
                visit(child);
        };
        visit(form.Window);
        return problems;
    }

    std::string FormSave(const FormDocument& form)
    {
        Writer writer;
        writer.Line(0, "-- Manifold form: made with the Form Editor. manifold.ui.form.load builds the window from it.");
        writer.Line(0, "return {");
        writer.Line(1, "form = 1,");
        if (form.Fragment)
            writer.Line(1, "fragment = true,");
        const UiNode* window = form.Model.Find(form.Window);
        if (window)
        {
            writer.Line(1, "title = " + Quote(window->Title) + ",");
            for (const auto& [key, literal] : NodeFields(*window))
            {
                if (key != "title")
                    writer.Line(1, key + " = " + literal + ",");
            }
            if (!form.Key.empty())
                writer.Line(1, "key = " + Quote(form.Key) + ",");
            if (!form.Script.empty())
                writer.Line(1, "script = " + Quote(form.Script) + ",");
            if (const FormMeta* meta = MetaOf(form, form.Window))
            {
                for (const FormHandler& handler : kWindowHandlers)
                {
                    const auto found = meta->Handlers.find(std::string(handler.Key));
                    if (found != meta->Handlers.end() && !found->second.empty())
                        writer.Line(1, std::string(handler.Key) + " = " + Quote(found->second) + ",");
                }
            }
            writer.Line(1, "children = {");
            for (const int child : window->Children)
            {
                if (const UiNode* item = form.Model.Find(child))
                    SaveNode(form, *item, 2, writer);
            }
            writer.Line(1, "},");
        }
        writer.Line(0, "}");
        return writer.Out;
    }

    bool FormLoad(std::string_view text, FormDocument& form, std::string& error, std::vector<std::string>* warnings)
    {
        form = NewForm({});
        lua_State* state = luaL_newstate();
        if (!state)
        {
            error = "out of memory";
            return false;
        }
        bool ok = false;
        if (luaL_loadbufferx(state, text.data(), text.size(), "=form", "t") != LUA_OK)
            error = lua_tostring(state, -1);
        else
        {
            lua_newtable(state);
            lua_setupvalue(state, -2, 1);
            lua_sethook(state, &CountHook, LUA_MASKCOUNT, kInstructionLimit);
            if (lua_pcall(state, 0, 1, 0) != LUA_OK)
                error = lua_tostring(state, -1) ? lua_tostring(state, -1) : "the form file failed";
            else if (!lua_istable(state, -1))
                error = "the form file must return a table";
            else
            {
                lua_sethook(state, nullptr, 0, 0);
                Loader loader{ state, form, warnings };
                ok = loader.Run(lua_gettop(state));
                if (!ok)
                    error = loader.Error;
            }
        }
        lua_close(state);
        if (!ok)
            form = NewForm({});
        return ok;
    }

    std::string FormScript(const FormDocument& form, FormScriptStyle style, std::string_view formFile)
    {
        ScriptWriter writer{ form };
        const UiNode* window = form.Model.Find(form.Window);
        if (!window)
            return {};
        writer.Collect(*window);
        std::string out = "local ui = manifold.ui\n\n";
        if (style == FormScriptStyle::FormFile)
        {
            out += "local window, w\nlocal handlers = {}\n\n";
            out += writer.Stubs(true);
            out += "window, w = ui.form.load(" + Quote(formFile.empty() ? std::string_view("Form.form.lua") : formFile) + ", handlers)\n";
            return out;
        }
        out += "local window\nlocal w = {}\n\n";
        out += writer.Stubs(false);
        const std::string options = writer.Options(*window, false);
        writer.Body = "window = ui.window.create(" + Quote(window->Title) + (options.empty() ? std::string() : ", " + options) + ")\n";
        writer.Build(*window, "window");
        return out + writer.Body;
    }
}
