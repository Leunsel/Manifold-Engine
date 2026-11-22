#include "Lua/LuaUi.h"

#include "App/Engine.h"
#include "Core/IconNames.h"
#include "Core/ImageDecode.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaEngine.h"
#include "Lua/UiForm.h"
#include "Lua/UiModel.h"

#include "lua.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Manifold::Lua
{
    namespace
    {
        constexpr int kMaxItems = 100000;
        constexpr int kMaxDraws = 200000;
        constexpr int kMaxDepth = 32;
        constexpr int kMaxImageSide = 8192;

        UiModel& Model(lua_State* state)
        {
            return EngineOf(state).Ui();
        }

        int Gone(lua_State* state, const char* what)
        {
            return PushFailure(state, ErrorCode::LuaUiGone, what);
        }

        bool ReadString(lua_State* state, int index, const char* name, std::string& out)
        {
            lua_getfield(state, index, name);
            const bool found = lua_type(state, -1) == LUA_TSTRING || lua_type(state, -1) == LUA_TNUMBER;
            if (found)
            {
                std::size_t length = 0;
                const char* text = lua_tolstring(state, -1, &length);
                out.assign(text, length);
            }
            lua_pop(state, 1);
            return found;
        }

        bool ReadBool(lua_State* state, int index, const char* name, bool& out)
        {
            lua_getfield(state, index, name);
            const bool found = lua_isboolean(state, -1);
            if (found)
                out = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            return found;
        }

        bool ReadNumber(lua_State* state, int index, const char* name, double& out)
        {
            lua_getfield(state, index, name);
            const bool found = lua_isnumber(state, -1) != 0;
            if (found)
                out = lua_tonumber(state, -1);
            lua_pop(state, 1);
            return found;
        }

        bool ReadFloat(lua_State* state, int index, const char* name, float& out)
        {
            double value = 0.0;
            if (!ReadNumber(state, index, name, value))
                return false;
            out = static_cast<float>(value);
            return true;
        }

        std::string CellText(lua_State* state, int index)
        {
            if (lua_type(state, index) == LUA_TNUMBER && lua_isinteger(state, index))
                return std::to_string(lua_tointeger(state, index));
            if (lua_type(state, index) == LUA_TNUMBER)
            {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%.6g", lua_tonumber(state, index));
                return buffer;
            }
            if (lua_type(state, index) == LUA_TBOOLEAN)
                return lua_toboolean(state, index) ? "true" : "false";
            std::size_t length = 0;
            const char* text = luaL_tolstring(state, index, &length);
            std::string value(text, length);
            lua_pop(state, 1);
            return value;
        }

        std::vector<std::string> ReadStrings(lua_State* state, int list)
        {
            std::vector<std::string> items;
            const lua_Integer count = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, list)), kMaxItems);
            items.reserve(static_cast<std::size_t>(count));
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, list, i);
                items.push_back(CellText(state, -1));
                lua_pop(state, 1);
            }
            return items;
        }

        std::vector<UiTreeItem> ReadTree(lua_State* state, int list, int depth)
        {
            std::vector<UiTreeItem> items;
            if (depth > kMaxDepth)
                return items;
            const lua_Integer count = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, list)), kMaxItems);
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, list, i);
                const int entry = lua_gettop(state);
                UiTreeItem item;
                if (lua_istable(state, entry))
                {
                    ReadString(state, entry, "text", item.Text);
                    ReadString(state, entry, "icon", item.Icon);
                    ReadBool(state, entry, "open", item.Open);
                    if (!ReadString(state, entry, "id", item.Id))
                        item.Id = item.Text;
                    if (lua_getfield(state, entry, "children") == LUA_TTABLE)
                        item.Children = ReadTree(state, lua_gettop(state), depth + 1);
                    lua_pop(state, 1);
                }
                else
                {
                    item.Text = CellText(state, entry);
                    item.Id = item.Text;
                }
                lua_pop(state, 1);
                items.push_back(std::move(item));
            }
            return items;
        }

        void ReadColumns(lua_State* state, int list, std::vector<UiColumn>& columns)
        {
            columns.clear();
            const lua_Integer count = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, list)), 64);
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, list, i);
                UiColumn column;
                if (lua_istable(state, -1))
                {
                    const int entry = lua_gettop(state);
                    ReadString(state, entry, "title", column.Title);
                    ReadFloat(state, entry, "width", column.Width);
                    ReadString(state, entry, "align", column.Align);
                }
                else
                    column.Title = CellText(state, -1);
                lua_pop(state, 1);
                columns.push_back(std::move(column));
            }
        }

        void ReadRows(lua_State* state, int list, std::vector<std::vector<std::string>>& rows)
        {
            rows.clear();
            const lua_Integer count = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, list)), kMaxItems);
            rows.reserve(static_cast<std::size_t>(count));
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, list, i);
                if (lua_istable(state, -1))
                    rows.push_back(ReadStrings(state, lua_gettop(state)));
                else
                    rows.push_back({ CellText(state, -1) });
                lua_pop(state, 1);
            }
        }

        bool ReadDraw(lua_State* state, int entry, UiDraw& draw, std::string& error)
        {
            std::string kind;
            ReadString(state, entry, "kind", kind);
            if (kind == "line")
                draw.Kind = UiDrawKind::Line;
            else if (kind == "rect")
                draw.Kind = UiDrawKind::Rect;
            else if (kind == "circle")
                draw.Kind = UiDrawKind::Circle;
            else if (kind == "polygon")
                draw.Kind = UiDrawKind::Polygon;
            else if (kind == "text")
                draw.Kind = UiDrawKind::Text;
            else if (kind == "image")
                draw.Kind = UiDrawKind::Image;
            else
            {
                error = "unknown drawing kind \"" + kind + "\"";
                return false;
            }
            ReadFloat(state, entry, "x", draw.X);
            ReadFloat(state, entry, "y", draw.Y);
            ReadFloat(state, entry, "x2", draw.X2);
            ReadFloat(state, entry, "y2", draw.Y2);
            float width = 0.0f;
            float height = 0.0f;
            if (ReadFloat(state, entry, "width", width))
                draw.X2 = draw.X + width;
            if (ReadFloat(state, entry, "height", height))
                draw.Y2 = draw.Y + height;
            ReadFloat(state, entry, "radius", draw.Radius);
            ReadFloat(state, entry, "thickness", draw.Thickness);
            ReadFloat(state, entry, "rounding", draw.Rounding);
            ReadFloat(state, entry, "size", draw.Size);
            ReadBool(state, entry, "filled", draw.Filled);
            ReadString(state, entry, "color", draw.Color);
            ReadString(state, entry, "text", draw.Text);
            double image = 0.0;
            if (ReadNumber(state, entry, "image", image))
                draw.Image = static_cast<int>(image);
            if (lua_getfield(state, entry, "points") == LUA_TTABLE)
            {
                const int list = lua_gettop(state);
                const lua_Integer count = std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, list)), 2 * 4096);
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(state, list, i);
                    draw.Points.push_back(static_cast<float>(lua_tonumber(state, -1)));
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 1);
            if (draw.Kind == UiDrawKind::Polygon && draw.Points.size() < 6)
            {
                error = "a polygon needs at least three points";
                return false;
            }
            return true;
        }

        void ApplyValue(lua_State* state, int index, UiNode& node)
        {
            switch (node.Kind)
            {
            case UiKind::Checkbox:
                node.Bool = lua_toboolean(state, index) != 0;
                break;
            case UiKind::Input:
            case UiKind::Color:
                node.String = CellText(state, index);
                break;
            case UiKind::Number:
            case UiKind::Slider:
                node.Number = lua_tonumber(state, index);
                break;
            case UiKind::Combo:
                if (node.Multi)
                {
                    // a list of item numbers (or one number, or nil for none)
                    node.Chosen.clear();
                    if (lua_istable(state, index))
                    {
                        const lua_Integer count = luaL_len(state, index);
                        for (lua_Integer i = 1; i <= count && i <= 4096; ++i)
                        {
                            lua_geti(state, index, i);
                            if (lua_isnumber(state, -1))
                                node.Chosen.push_back(static_cast<int>(lua_tointeger(state, -1)));
                            lua_pop(state, 1);
                        }
                    }
                    else if (lua_isnumber(state, index))
                        node.Chosen.push_back(static_cast<int>(lua_tointeger(state, index)));
                    std::sort(node.Chosen.begin(), node.Chosen.end());
                    node.Chosen.erase(std::unique(node.Chosen.begin(), node.Chosen.end()), node.Chosen.end());
                    std::erase_if(node.Chosen, [&](int item) { return item < 1 || item > static_cast<int>(node.Items.size()); });
                    break;
                }
                node.Selected = lua_isnumber(state, index) ? static_cast<int>(lua_tointeger(state, index)) : 0;
                break;
            case UiKind::Radio:
            case UiKind::Table:
                node.Selected = lua_isnumber(state, index) ? static_cast<int>(lua_tointeger(state, index)) : 0;
                break;
            case UiKind::Tree:
                node.SelectedId = lua_isnil(state, index) ? std::string() : CellText(state, index);
                break;
            case UiKind::Progress:
                node.Fraction = static_cast<float>(std::clamp(lua_tonumber(state, index), 0.0, 1.0));
                break;
            default:
                node.Text = lua_isnil(state, index) ? std::string() : CellText(state, index);
                break;
            }
        }

        void PushValue(lua_State* state, const UiNode& node)
        {
            switch (node.Kind)
            {
            case UiKind::Checkbox:
                lua_pushboolean(state, node.Bool ? 1 : 0);
                break;
            case UiKind::Input:
            case UiKind::Color:
                lua_pushlstring(state, node.String.data(), node.String.size());
                break;
            case UiKind::Number:
            case UiKind::Slider:
                if (node.Integer)
                    lua_pushinteger(state, static_cast<lua_Integer>(node.Number));
                else
                    lua_pushnumber(state, node.Number);
                break;
            case UiKind::Combo:
                if (node.Multi)
                {
                    lua_createtable(state, static_cast<int>(node.Chosen.size()), 0);
                    for (std::size_t i = 0; i < node.Chosen.size(); ++i)
                    {
                        lua_pushinteger(state, node.Chosen[i]);
                        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
                    }
                    break;
                }
                [[fallthrough]];
            case UiKind::Radio:
            case UiKind::Table:
                if (node.Selected > 0)
                    lua_pushinteger(state, node.Selected);
                else
                    lua_pushnil(state);
                break;
            case UiKind::Tree:
                if (node.SelectedId.empty())
                    lua_pushnil(state);
                else
                    lua_pushlstring(state, node.SelectedId.data(), node.SelectedId.size());
                break;
            case UiKind::Progress:
                lua_pushnumber(state, node.Fraction);
                break;
            case UiKind::Window:
                lua_pushlstring(state, node.Title.data(), node.Title.size());
                break;
            default:
                lua_pushlstring(state, node.Text.data(), node.Text.size());
                break;
            }
        }

        void ApplyProps(lua_State* state, int props, UiNode& node)
        {
            ReadString(state, props, "text", node.Text);
            ReadString(state, props, "label", node.Label);
            ReadString(state, props, "tooltip", node.Tooltip);
            ReadString(state, props, "icon", node.Icon);
            ReadString(state, props, "color", node.Color);
            ReadString(state, props, "badge", node.Badge);
            ReadString(state, props, "hint", node.Hint);
            ReadString(state, props, "format", node.Format);
            ReadString(state, props, "style", node.Style);
            ReadString(state, props, "title", node.Title);
            ReadString(state, props, "background", node.Background);
            ReadString(state, props, "align", node.Align);
            ReadString(state, props, "position", node.Position);
            ReadString(state, props, "key", node.Key);
            ReadString(state, props, "shortcut", node.Shortcut);
            ReadString(state, props, "confirm", node.Confirm);
            ReadFloat(state, props, "minWidth", node.MinWidth);
            ReadFloat(state, props, "minHeight", node.MinHeight);
            ReadFloat(state, props, "maxWidth", node.MaxWidth);
            ReadFloat(state, props, "maxHeight", node.MaxHeight);
            ReadBool(state, props, "resizable", node.Resizable);
            ReadBool(state, props, "autoSize", node.AutoSize);
            ReadBool(state, props, "scrollbar", node.Scrollbar);
            ReadBool(state, props, "topmost", node.Topmost);
            ReadBool(state, props, "modal", node.Modal);
            ReadBool(state, props, "readOnly", node.ReadOnly);
            ReadBool(state, props, "scroll", node.Scroll);
            if (ReadBool(state, props, "focus", node.Focus))
                node.Focused = false;
            if (ReadFloat(state, props, "x", node.PosX))
                node.HasPos = true;
            if (ReadFloat(state, props, "y", node.PosY))
                node.HasPos = true;
            if (ReadFloat(state, props, "gap", node.Gap))
                node.Gap = std::max(0.0f, node.Gap);
            double maxLength = 0.0;
            if (ReadNumber(state, props, "maxLength", maxLength))
                node.MaxLength = static_cast<int>(std::clamp(maxLength, 0.0, 1048576.0));
            ReadBool(state, props, "enabled", node.Enabled);
            ReadBool(state, props, "visible", node.Visible);
            ReadBool(state, props, "wrap", node.Wrap);
            ReadBool(state, props, "mono", node.Mono);
            ReadBool(state, props, "integer", node.Integer);
            ReadBool(state, props, "hex", node.Hex);
            ReadBool(state, props, "multiline", node.Multiline);
            ReadBool(state, props, "password", node.Password);
            ReadBool(state, props, "vertical", node.Vertical);
            ReadBool(state, props, "collapsible", node.Collapsible);
            ReadBool(state, props, "zoom", node.Zoom);
            ReadBool(state, props, "dock", node.Dock);
            ReadBool(state, props, "horizontal", node.Horizontal);
            ReadBool(state, props, "alpha", node.Alpha);
            if (node.Kind == UiKind::Combo && ReadBool(state, props, "multi", node.Multi))
            {
                // switching between one and several chosen items keeps the choice
                if (node.Multi && node.Chosen.empty() && node.Selected > 0)
                    node.Chosen = { node.Selected };
                else if (!node.Multi && node.Selected == 0 && !node.Chosen.empty())
                    node.Selected = node.Chosen.front();
            }
            if (node.Kind == UiKind::Window)
                ReadBool(state, props, "open", node.Open);
            if (node.Kind == UiKind::Section && ReadBool(state, props, "open", node.Open))
                node.OpenChanged = true;
            ReadString(state, props, "labelPosition", node.LabelPosition);
            ReadFloat(state, props, "size", node.FontSize);
            ReadBool(state, props, "bold", node.Bold);
            ReadFloat(state, props, "width", node.Width);
            ReadFloat(state, props, "height", node.Height);
            ReadFloat(state, props, "ratio", node.Ratio);
            if (ReadFloat(state, props, "opacity", node.Opacity))
                node.Opacity = std::clamp(node.Opacity, 0.1f, 1.0f);
            if (ReadFloat(state, props, "inactiveOpacity", node.InactiveOpacity))
                node.InactiveOpacity = std::clamp(node.InactiveOpacity, 0.1f, 1.0f);
            node.Ratio = std::clamp(node.Ratio, 0.05f, 0.95f);
            ReadNumber(state, props, "step", node.Step);
            if (ReadNumber(state, props, "min", node.Min))
                node.HasMin = true;
            if (ReadNumber(state, props, "max", node.Max))
                node.HasMax = true;
            double image = 0.0;
            if (ReadNumber(state, props, "image", image))
                node.ImageId = static_cast<int>(image);
            double interval = 0.0;
            if (ReadNumber(state, props, "interval", interval))
                node.Interval = std::max(0.0, interval);
            double handlers = 0.0;
            if (ReadNumber(state, props, "handlers", handlers))
                node.Handlers = static_cast<std::uint32_t>(handlers);
            bool updates = false;
            if (ReadBool(state, props, "updates", updates))
            {
                node.Updates = updates;
                node.NextUpdate = 0.0;
            }
            bool binds = false;
            if (node.Kind == UiKind::Window && ReadBool(state, props, "binds", binds))
            {
                node.Binds = binds;
                node.NextBind = 0.0;
            }
            if (lua_getfield(state, props, "items") == LUA_TTABLE)
            {
                if (node.Kind == UiKind::Tree)
                    node.Tree = ReadTree(state, lua_gettop(state), 0);
                else
                    node.Items = ReadStrings(state, lua_gettop(state));
            }
            lua_pop(state, 1);
            const int columns = lua_getfield(state, props, "columns");
            if (columns == LUA_TTABLE)
                ReadColumns(state, lua_gettop(state), node.Columns);
            else if (columns == LUA_TNUMBER && node.Kind == UiKind::Grid)
                node.GridColumns = static_cast<int>(std::clamp<lua_Integer>(static_cast<lua_Integer>(lua_tonumber(state, -1)), 1, 16));
            lua_pop(state, 1);
            if (lua_getfield(state, props, "rows") == LUA_TTABLE)
                ReadRows(state, lua_gettop(state), node.Rows);
            lua_pop(state, 1);
            if (lua_getfield(state, props, "menu") == LUA_TTABLE)
                node.Menu = ReadStrings(state, lua_gettop(state));
            lua_pop(state, 1);
            if (lua_getfield(state, props, "value") != LUA_TNIL)
                ApplyValue(state, lua_gettop(state), node);
            lua_pop(state, 1);
            if (node.Kind == UiKind::Table && node.Selected > static_cast<int>(node.Rows.size()))
                node.Selected = 0;
            if ((node.Kind == UiKind::Combo || node.Kind == UiKind::Radio) && node.Selected > static_cast<int>(node.Items.size()))
                node.Selected = 0;
            if (node.Kind == UiKind::Combo)
                std::erase_if(node.Chosen, [&](int item) { return item < 1 || item > static_cast<int>(node.Items.size()); });
        }

        int UiCreate(lua_State* state)
        {
            const std::string kindName = luaL_checkstring(state, 1);
            const int parent = static_cast<int>(luaL_optinteger(state, 2, 0));
            const std::string owner = luaL_optstring(state, 3, "");
            const std::optional<UiKind> kind = UiKindFromName(kindName);
            if (!kind)
                return luaL_error(state, "unknown widget kind %s", kindName.c_str());
            UiModel& model = Model(state);
            const int id = model.Create(*kind, parent, owner);
            if (!id)
                return Gone(state, "container");
            if (lua_istable(state, 4))
                ApplyProps(state, 4, *model.Find(id));
            lua_pushinteger(state, id);
            return 1;
        }

        int UiUpdate(lua_State* state)
        {
            UiModel& model = Model(state);
            UiNode* node = model.Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node)
                return Gone(state, "widget");
            luaL_checktype(state, 2, LUA_TTABLE);
            ApplyProps(state, 2, *node);
            model.Touch(*node);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiGet(lua_State* state)
        {
            const UiNode* node = Model(state).Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node)
                return Gone(state, "widget");
            PushValue(state, *node);
            return 1;
        }

        int UiSet(lua_State* state)
        {
            UiModel& model = Model(state);
            UiNode* node = model.Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node)
                return Gone(state, "widget");
            ApplyValue(state, 2, *node);
            if (node->Kind == UiKind::Window)
                node->Title = node->Text;
            model.Touch(*node);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiRemove(lua_State* state)
        {
            lua_pushboolean(state, Model(state).Remove(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        int UiClear(lua_State* state)
        {
            UiModel& model = Model(state);
            const int id = static_cast<int>(luaL_checkinteger(state, 1));
            UiNode* node = model.Find(id);
            if (!node)
                return Gone(state, "widget");
            if (node->Kind == UiKind::Canvas)
            {
                node->Draws.clear();
                model.Touch(*node);
            }
            else
                model.Clear(id);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiShow(lua_State* state)
        {
            UiModel& model = Model(state);
            UiNode* node = model.Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node || node->Kind != UiKind::Window)
                return Gone(state, "window");
            node->Open = lua_toboolean(state, 2) != 0;
            if (node->Open && lua_toboolean(state, 3))
                node->FocusRequest = true;
            model.Touch(*node);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiIsOpen(lua_State* state)
        {
            const UiNode* node = Model(state).Find(static_cast<int>(luaL_checkinteger(state, 1)));
            lua_pushboolean(state, node && node->Kind == UiKind::Window && node->Open ? 1 : 0);
            return 1;
        }

        int UiExists(lua_State* state)
        {
            lua_pushboolean(state, Model(state).Find(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        int UiDrawCanvas(lua_State* state)
        {
            UiModel& model = Model(state);
            UiNode* node = model.Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node || node->Kind != UiKind::Canvas)
                return Gone(state, "canvas");
            luaL_checktype(state, 2, LUA_TTABLE);
            const bool append = lua_toboolean(state, 3) != 0;
            std::vector<UiDraw> draws;
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, 2));
            if (count > kMaxDraws)
                return luaL_error(state, "a canvas takes at most %d drawings at once", kMaxDraws);
            draws.reserve(static_cast<std::size_t>(count));
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, 2, i);
                if (!lua_istable(state, -1))
                    return luaL_error(state, "drawing %d is not a table", static_cast<int>(i));
                UiDraw draw;
                std::string error;
                if (!ReadDraw(state, lua_gettop(state), draw, error))
                    return luaL_error(state, "drawing %d: %s", static_cast<int>(i), error.c_str());
                lua_pop(state, 1);
                draws.push_back(std::move(draw));
            }
            if (append)
                node->Draws.insert(node->Draws.end(), std::make_move_iterator(draws.begin()), std::make_move_iterator(draws.end()));
            else
                node->Draws = std::move(draws);
            model.Touch(*node);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiView(lua_State* state)
        {
            const UiNode* node = Model(state).Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node || node->Kind != UiKind::Canvas)
                return Gone(state, "canvas");
            lua_createtable(state, 0, 3);
            lua_pushnumber(state, node->ViewX);
            lua_setfield(state, -2, "x");
            lua_pushnumber(state, node->ViewY);
            lua_setfield(state, -2, "y");
            lua_pushnumber(state, node->Scale);
            lua_setfield(state, -2, "scale");
            return 1;
        }

        int UiSetView(lua_State* state)
        {
            UiModel& model = Model(state);
            UiNode* node = model.Find(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!node || node->Kind != UiKind::Canvas)
                return Gone(state, "canvas");
            luaL_checktype(state, 2, LUA_TTABLE);
            ReadFloat(state, 2, "x", node->ViewX);
            ReadFloat(state, 2, "y", node->ViewY);
            ReadFloat(state, 2, "scale", node->Scale);
            node->Scale = std::clamp(node->Scale, 0.05f, 64.0f);
            node->ViewChanged = true;
            model.Touch(*node);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiDue(lua_State* state)
        {
            const double now = luaL_checknumber(state, 1);
            UiModel& model = Model(state);
            lua_newtable(state);
            lua_Integer index = 1;
            for (const int id : model.Windows())
            {
                UiNode* node = model.Find(id);
                if (!node || !node->Updates || !node->Open || now < node->NextUpdate)
                    continue;
                node->NextUpdate = now + std::max(node->Interval, 0.016);
                lua_pushinteger(state, id);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        // windows whose record-bound widgets are due for a refresh
        int UiBindsDue(lua_State* state)
        {
            const double now = luaL_checknumber(state, 1);
            UiModel& model = Model(state);
            lua_newtable(state);
            lua_Integer index = 1;
            for (const int id : model.Windows())
            {
                UiNode* node = model.Find(id);
                if (!node || !node->Binds || !node->Open || now < node->NextBind)
                    continue;
                node->NextBind = now + kBindInterval;
                lua_pushinteger(state, id);
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        void PushValue(lua_State* state, const UiValue& value)
        {
            if (const bool* flag = std::get_if<bool>(&value))
                lua_pushboolean(state, *flag ? 1 : 0);
            else if (const double* number = std::get_if<double>(&value))
            {
                if (*number == static_cast<double>(static_cast<lua_Integer>(*number)) && std::abs(*number) < 9.0e15)
                    lua_pushinteger(state, static_cast<lua_Integer>(*number));
                else
                    lua_pushnumber(state, *number);
            }
            else if (const std::string* text = std::get_if<std::string>(&value))
                lua_pushlstring(state, text->data(), text->size());
            else
                lua_pushnil(state);
        }

        int UiTakeEvents(lua_State* state)
        {
            const std::vector<UiEvent> events = Model(state).TakeEvents();
            lua_createtable(state, static_cast<int>(events.size()), 0);
            lua_Integer index = 1;
            for (const UiEvent& event : events)
            {
                lua_createtable(state, static_cast<int>(event.Values.size()), 2);
                lua_pushinteger(state, event.Target);
                lua_setfield(state, -2, "target");
                lua_pushlstring(state, event.Name.data(), event.Name.size());
                lua_setfield(state, -2, "name");
                lua_pushinteger(state, static_cast<lua_Integer>(event.Values.size()));
                lua_setfield(state, -2, "n");
                lua_Integer slot = 1;
                for (const UiValue& value : event.Values)
                {
                    PushValue(state, value);
                    lua_rawseti(state, -2, slot++);
                }
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        int UiPost(lua_State* state)
        {
            UiEvent event;
            event.Target = static_cast<int>(luaL_checkinteger(state, 1));
            event.Name = luaL_checkstring(state, 2);
            const int top = lua_gettop(state);
            for (int i = 3; i <= top; ++i)
            {
                if (lua_isboolean(state, i))
                    event.Values.emplace_back(lua_toboolean(state, i) != 0);
                else if (lua_isnumber(state, i))
                    event.Values.emplace_back(static_cast<double>(lua_tonumber(state, i)));
                else if (lua_isstring(state, i))
                    event.Values.emplace_back(std::string(lua_tostring(state, i)));
                else
                    event.Values.emplace_back(std::monostate{});
            }
            Model(state).Post(std::move(event));
            return 0;
        }

        int UiForgetOwner(lua_State* state)
        {
            Model(state).ForgetOwner(luaL_checkstring(state, 1));
            return 0;
        }

        int UiFormRead(lua_State* state)
        {
            const std::string path = luaL_checkstring(state, 1);
            const std::string folder = luaL_optstring(state, 2, "");
            std::filesystem::path file(Utf8ToWide(path));
            if (file.is_relative())
                file = (folder.empty() ? Paths::ProgramFolder() : std::filesystem::path(Utf8ToWide(folder))) / file;
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
                return PushFailure(state, ErrorCode::LuaFormUnreadable, path);
            const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (text.size() > 16u * 1024u * 1024u)
                return PushFailure(state, ErrorCode::LuaFormUnreadable, path);
            lua_pushlstring(state, text.data(), text.size());
            const std::string shown = WideToUtf8(file.wstring());
            lua_pushlstring(state, shown.data(), shown.size());
            return 2;
        }

        // The form table a script holds ("local form = { form = 1, ... }") as
        // "return { ... }", padded to the script's own line numbers; nil when
        // the script holds none.
        int UiFormBlock(lua_State* state)
        {
            std::size_t size = 0;
            const char* text = luaL_checklstring(state, 1, &size);
            const std::string chunk = FormBlockChunk(std::string_view(text, size));
            if (chunk.empty())
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushlstring(state, chunk.data(), chunk.size());
            return 1;
        }

        int UiWatchFile(lua_State* state)
        {
            const int window = static_cast<int>(luaL_checkinteger(state, 1));
            const std::string path = luaL_checkstring(state, 2);
            UiModel& model = Model(state);
            const UiNode* node = model.Find(window);
            if (!node || node->Kind != UiKind::Window)
                return Gone(state, "window");
            model.Watch(window, path);
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiUnwatchFile(lua_State* state)
        {
            Model(state).Unwatch(static_cast<int>(luaL_checkinteger(state, 1)));
            return 0;
        }

        int UiImageLoad(lua_State* state)
        {
            const std::string path = luaL_checkstring(state, 1);
            const std::string owner = luaL_optstring(state, 2, "");
            std::filesystem::path file(Utf8ToWide(path));
            if (file.is_relative())
                file = Paths::ProgramFolder() / file;
            std::ifstream stream(file, std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            std::optional<DecodedImage> decoded = DecodeImage(bytes);
            if (!decoded || decoded->Width <= 0 || decoded->Height <= 0 || decoded->Width > kMaxImageSide || decoded->Height > kMaxImageSide)
            {
                return PushFailure(state, ErrorCode::LuaImageUnreadable, path);
            }
            UiImage image;
            image.Width = decoded->Width;
            image.Height = decoded->Height;
            image.Rgba = std::move(decoded->Rgba);
            image.Owner = owner;
            image.Path = path;
            const int width = image.Width;
            const int height = image.Height;
            const int id = Model(state).AddImage(std::move(image));
            lua_pushinteger(state, id);
            lua_pushinteger(state, width);
            lua_pushinteger(state, height);
            return 3;
        }

        int UiImageRemove(lua_State* state)
        {
            lua_pushboolean(state, Model(state).RemoveImage(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        void ApplySlot(lua_State* state, int props, UiSlot& slot)
        {
            ReadString(state, props, "path", slot.Path);
            ReadString(state, props, "text", slot.Text);
            ReadString(state, props, "icon", slot.Icon);
            ReadString(state, props, "tooltip", slot.Tooltip);
            ReadString(state, props, "color", slot.Color);
            ReadBool(state, props, "enabled", slot.Enabled);
            if (ReadBool(state, props, "checked", slot.Checked))
                slot.Checkable = true;
        }

        int UiSlotAdd(lua_State* state)
        {
            const std::string kind = luaL_checkstring(state, 1);
            UiSlot slot;
            slot.Kind = kind == "toolbar" ? UiSlotKind::Toolbar : kind == "status" ? UiSlotKind::Status : UiSlotKind::Menu;
            slot.Owner = luaL_optstring(state, 2, "");
            luaL_checktype(state, 3, LUA_TTABLE);
            ApplySlot(state, 3, slot);
            lua_pushinteger(state, Model(state).AddSlot(std::move(slot)));
            return 1;
        }

        int UiSlotUpdate(lua_State* state)
        {
            UiSlot* slot = Model(state).FindSlot(static_cast<int>(luaL_checkinteger(state, 1)));
            if (!slot)
                return Gone(state, "entry");
            luaL_checktype(state, 2, LUA_TTABLE);
            ApplySlot(state, 2, *slot);
            Model(state).Changed();
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiSlotRemove(lua_State* state)
        {
            lua_pushboolean(state, Model(state).RemoveSlot(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        int UiOpenDialog(lua_State* state)
        {
            const std::string kind = luaL_checkstring(state, 1);
            UiDialog dialog;
            dialog.Kind = kind == "input" ? UiDialogKind::Input : kind == "choose" ? UiDialogKind::Choose : kind == "openFile" ? UiDialogKind::OpenFile
                        : kind == "saveFile" ? UiDialogKind::SaveFile : UiDialogKind::Confirm;
            dialog.Owner = luaL_optstring(state, 2, "");
            luaL_checktype(state, 3, LUA_TTABLE);
            ReadString(state, 3, "title", dialog.Title);
            ReadString(state, 3, "text", dialog.Text);
            ReadString(state, 3, "value", dialog.Value);
            ReadBool(state, 3, "danger", dialog.Danger);
            if (lua_getfield(state, 3, "items") == LUA_TTABLE)
                dialog.Items = ReadStrings(state, lua_gettop(state));
            lua_pop(state, 1);
            if (lua_getfield(state, 3, "filters") == LUA_TTABLE)
            {
                const std::vector<std::string> flat = ReadStrings(state, lua_gettop(state));
                for (std::size_t i = 0; i + 1 < flat.size(); i += 2)
                    dialog.Filters.emplace_back(flat[i], flat[i + 1]);
            }
            lua_pop(state, 1);
            lua_pushinteger(state, Model(state).AddDialog(std::move(dialog)));
            return 1;
        }

        int UiDialogClose(lua_State* state)
        {
            lua_pushboolean(state, Model(state).RemoveDialog(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        LogLevel LevelOf(std::string_view name)
        {
            if (name == "success")
                return LogLevel::Success;
            if (name == "warning")
                return LogLevel::Warning;
            if (name == "error")
                return LogLevel::Error;
            if (name == "debug")
                return LogLevel::Debug;
            return LogLevel::Info;
        }

        int UiNotice(lua_State* state)
        {
            Engine& engine = EngineOf(state).Owner();
            const std::string text = luaL_checkstring(state, 1);
            const LogLevel level = LevelOf(luaL_optstring(state, 2, "info"));
            const double progress = luaL_optnumber(state, 3, -1.0);
            const bool sticky = lua_toboolean(state, 4) != 0;
            lua_pushinteger(state, engine.Notifications().Show(level, text, engine.Now(), progress, sticky));
            return 1;
        }

        int UiNoticeUpdate(lua_State* state)
        {
            Engine& engine = EngineOf(state).Owner();
            const int id = static_cast<int>(luaL_checkinteger(state, 1));
            const std::string text = luaL_checkstring(state, 2);
            const LogLevel level = LevelOf(luaL_optstring(state, 3, "info"));
            const double progress = luaL_optnumber(state, 4, -1.0);
            const bool sticky = lua_toboolean(state, 5) != 0;
            if (!engine.Notifications().Update(id, level, text, engine.Now(), progress, sticky))
                return Gone(state, "notice");
            lua_pushboolean(state, 1);
            return 1;
        }

        int UiNoticeRemove(lua_State* state)
        {
            lua_pushboolean(state, EngineOf(state).Owner().Notifications().Remove(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        int UiNoticeExists(lua_State* state)
        {
            lua_pushboolean(state, EngineOf(state).Owner().Notifications().Has(static_cast<int>(luaL_checkinteger(state, 1))) ? 1 : 0);
            return 1;
        }

        int UiIsIcon(lua_State* state)
        {
            lua_pushboolean(state, !IconGlyph(luaL_checkstring(state, 1)).empty() ? 1 : 0);
            return 1;
        }

        int UiIcons(lua_State* state)
        {
            const std::string filter = Lower(luaL_optstring(state, 1, ""));
            lua_newtable(state);
            lua_Integer index = 1;
            for (const IconName& icon : IconNames())
            {
                if (!filter.empty() && icon.Name.find(filter) == std::string_view::npos)
                    continue;
                lua_pushlstring(state, icon.Name.data(), icon.Name.size());
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }
    }

    void ApplyUiProps(lua_State* state, int props, UiNode& node)
    {
        ApplyProps(state, lua_absindex(state, props), node);
    }

    void RegisterUiNatives(lua_State* state, int natives)
    {
        const luaL_Reg functions[] = {
            { "uiCreate", &UiCreate },
            { "uiUpdate", &UiUpdate },
            { "uiGet", &UiGet },
            { "uiSet", &UiSet },
            { "uiRemove", &UiRemove },
            { "uiClear", &UiClear },
            { "uiShow", &UiShow },
            { "uiIsOpen", &UiIsOpen },
            { "uiExists", &UiExists },
            { "uiDraw", &UiDrawCanvas },
            { "uiView", &UiView },
            { "uiSetView", &UiSetView },
            { "uiDue", &UiDue },
            { "uiBindsDue", &UiBindsDue },
            { "uiTakeEvents", &UiTakeEvents },
            { "uiPost", &UiPost },
            { "uiForgetOwner", &UiForgetOwner },
            { "uiImageLoad", &UiImageLoad },
            { "uiFormRead", &UiFormRead },
            { "uiFormBlock", &UiFormBlock },
            { "uiWatch", &UiWatchFile },
            { "uiUnwatch", &UiUnwatchFile },
            { "uiImageRemove", &UiImageRemove },
            { "uiSlotAdd", &UiSlotAdd },
            { "uiSlotUpdate", &UiSlotUpdate },
            { "uiSlotRemove", &UiSlotRemove },
            { "uiDialog", &UiOpenDialog },
            { "uiDialogClose", &UiDialogClose },
            { "uiNotice", &UiNotice },
            { "uiNoticeUpdate", &UiNoticeUpdate },
            { "uiNoticeRemove", &UiNoticeRemove },
            { "uiNoticeExists", &UiNoticeExists },
            { "uiIsIcon", &UiIsIcon },
            { "uiIcons", &UiIcons },
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
