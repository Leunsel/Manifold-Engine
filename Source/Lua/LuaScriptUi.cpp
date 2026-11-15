#include "Lua/LuaEngine.h"

#include "App/Engine.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include "lua.hpp"

#include <string>
#include <thread>

namespace Manifold::Lua
{
    namespace
    {
        std::string StringField(lua_State* state, int index, const char* name)
        {
            lua_getfield(state, index, name);
            std::string text;
            if (lua_type(state, -1) == LUA_TSTRING || lua_type(state, -1) == LUA_TNUMBER)
            {
                std::size_t length = 0;
                const char* raw = lua_tolstring(state, -1, &length);
                text.assign(raw, length);
            }
            lua_pop(state, 1);
            return text;
        }

        int IntField(lua_State* state, int index, const char* name, int fallback)
        {
            lua_getfield(state, index, name);
            int value = fallback;
            if (lua_isnumber(state, -1))
                value = static_cast<int>(lua_tonumber(state, -1));
            lua_pop(state, 1);
            return value;
        }

        float FloatField(lua_State* state, int index, const char* name, float fallback)
        {
            lua_getfield(state, index, name);
            float value = fallback;
            if (lua_isnumber(state, -1))
                value = static_cast<float>(lua_tonumber(state, -1));
            lua_pop(state, 1);
            return value;
        }

        bool BoolField(lua_State* state, int index, const char* name, bool fallback)
        {
            lua_getfield(state, index, name);
            const bool value = lua_isnil(state, -1) ? fallback : lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            return value;
        }

        template <typename Item, typename Read>
        std::vector<Item> ListField(lua_State* state, int index, const char* name, Read read)
        {
            std::vector<Item> items;
            lua_getfield(state, index, name);
            if (lua_istable(state, -1))
            {
                const int list = lua_gettop(state);
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, list));
                items.reserve(static_cast<std::size_t>(count));
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(state, list, i);
                    if (lua_istable(state, -1))
                        items.push_back(read(state, lua_gettop(state)));
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 1);
            return items;
        }

        ScriptMenuItem ReadMenuItem(lua_State* state, int index)
        {
            ScriptMenuItem item;
            item.Id = IntField(state, index, "id", 0);
            item.Caption = StringField(state, index, "caption");
            item.Shortcut = StringField(state, index, "shortcut");
            item.Enabled = BoolField(state, index, "enabled", true);
            item.Checked = BoolField(state, index, "checked", false);
            item.Children = ListField<ScriptMenuItem>(state, index, "children", &ReadMenuItem);
            return item;
        }

        ScriptNode ReadNode(lua_State* state, int index)
        {
            ScriptNode node;
            node.Id = IntField(state, index, "id", 0);
            node.Text = StringField(state, index, "text");
            node.Expanded = BoolField(state, index, "expanded", false);
            node.HasChildren = BoolField(state, index, "haschildren", false);
            node.Children = ListField<ScriptNode>(state, index, "children", &ReadNode);
            return node;
        }

        ScriptFont ReadFont(lua_State* state, int index)
        {
            ScriptFont font;
            font.Points = FloatField(state, index, "fontsize", 9.0f);
            font.Color = IntField(state, index, "fontcolor", -1);
            font.Bold = BoolField(state, index, "bold", false);
            font.Mono = BoolField(state, index, "mono", false);
            return font;
        }

        ScriptControl ReadControl(lua_State* state, int index)
        {
            ScriptControl control;
            control.Id = IntField(state, index, "id", 0);
            control.Kind = StringField(state, index, "kind");
            control.Text = StringField(state, index, "text");
            control.X = IntField(state, index, "x", 0);
            control.Y = IntField(state, index, "y", 0);
            control.W = IntField(state, index, "w", 0);
            control.H = IntField(state, index, "h", 0);
            control.Enabled = BoolField(state, index, "enabled", true);
            control.Color = IntField(state, index, "color", -1);
            control.Font = ReadFont(state, index);
            control.Cursor = IntField(state, index, "cursor", 0);
            control.Click = BoolField(state, index, "click", false);
            control.DoubleClick = BoolField(state, index, "dblclick", false);
            control.Hover = BoolField(state, index, "hover", false);
            control.Popup = IntField(state, index, "popup", 0);
            control.Bevel = IntField(state, index, "bevel", 0);
            control.BevelInner = IntField(state, index, "bevelinner", 0);
            control.BevelWidth = IntField(state, index, "bevelwidth", 1);
            control.BevelColor = IntField(state, index, "bevelcolor", -1);
            control.Alignment = IntField(state, index, "alignment", 0);
            control.Layout = IntField(state, index, "layout", 0);
            control.WordWrap = BoolField(state, index, "wordwrap", false);
            control.Checked = BoolField(state, index, "checked", false);
            control.ReadOnly = BoolField(state, index, "readonly", false);
            control.Password = BoolField(state, index, "password", false);
            control.Frame = BoolField(state, index, "frame", true);
            control.Hint = StringField(state, index, "hint");
            control.ItemIndex = IntField(state, index, "itemindex", -1);
            lua_getfield(state, index, "items");
            if (lua_istable(state, -1))
            {
                const int list = lua_gettop(state);
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, list));
                control.Items.reserve(static_cast<std::size_t>(count));
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(state, list, i);
                    std::size_t length = 0;
                    const char* raw = lua_tolstring(state, -1, &length);
                    control.Items.emplace_back(raw ? std::string(raw, length) : std::string());
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 1);
            control.Resizes = IntField(state, index, "resizes", 0);
            control.MinSize = IntField(state, index, "minsize", 30);
            control.Align = IntField(state, index, "align", 0);
            control.Nodes = ListField<ScriptNode>(state, index, "nodes", &ReadNode);
            control.Selected = IntField(state, index, "selected", 0);
            control.RightClickSelect = BoolField(state, index, "rightclickselect", false);
            control.Canvas = IntField(state, index, "canvas", 0);
            control.Mouse = BoolField(state, index, "mouse", false);
            control.Wheel = BoolField(state, index, "wheel", false);
            control.Unsupported = StringField(state, index, "unsupported");
            control.Children = ListField<ScriptControl>(state, index, "children", &ReadControl);
            return control;
        }

        ScriptForm ReadForm(lua_State* state, int index)
        {
            ScriptForm form;
            form.Id = IntField(state, index, "id", 0);
            form.Caption = StringField(state, index, "caption");
            form.Left = IntField(state, index, "left", 0);
            form.Top = IntField(state, index, "top", 0);
            form.Width = IntField(state, index, "width", 400);
            form.Height = IntField(state, index, "height", 300);
            form.ClientWidth = IntField(state, index, "clientwidth", -1);
            form.ClientHeight = IntField(state, index, "clientheight", -1);
            form.MinWidth = IntField(state, index, "minwidth", 0);
            form.MinHeight = IntField(state, index, "minheight", 0);
            form.Center = BoolField(state, index, "center", false);
            form.Raise = IntField(state, index, "raise", 0);
            form.Menu = IntField(state, index, "menu", 0);
            form.Color = IntField(state, index, "color", -1);
            form.KeyDown = BoolField(state, index, "keydown", false);
            form.Font = ReadFont(state, index);
            form.Unsupported = StringField(state, index, "unsupported");
            form.Controls = ListField<ScriptControl>(state, index, "controls", &ReadControl);
            return form;
        }

        std::string Quoted(std::string_view text)
        {
            std::string out = "\"";
            for (const char c : text)
            {
                const unsigned char byte = static_cast<unsigned char>(c);
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += c;
                }
                else if (byte < 0x20)
                    out += Format("\\%03d", byte);
                else
                    out += c;
            }
            return out + "\"";
        }
    }

    void LuaEngine::RunCallback(const std::string& code, const char* name)
    {
        if (m_engine.Async())
        {
            StartTask(code, name, {});
            return;
        }
        Run(code, name);
    }

    std::vector<ScriptMenuItem> LuaEngine::ReadMenu(bool byId, int id, const char* which)
    {
        std::vector<ScriptMenuItem> items;
        if (std::this_thread::get_id() != m_thread)
            return items;
        const int top = lua_gettop(m_state);
        lua_getglobal(m_state, "__manifoldMenuSnapshot");
        if (!lua_isfunction(m_state, -1))
        {
            lua_settop(m_state, top);
            return items;
        }
        if (byId)
            lua_pushinteger(m_state, id);
        else
            lua_pushstring(m_state, which);
        std::string error;
        if (!CallStack(1, 1, error))
        {
            LOG_WARN("Lua", "Reading the script menu entries failed: " + error);
            lua_settop(m_state, top);
            return items;
        }
        if (lua_istable(m_state, -1))
        {
            const int list = lua_gettop(m_state);
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(m_state, list));
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(m_state, list, i);
                if (lua_istable(m_state, -1))
                    items.push_back(ReadMenuItem(m_state, lua_gettop(m_state)));
                lua_pop(m_state, 1);
            }
        }
        lua_settop(m_state, top);
        return items;
    }

    void LuaEngine::OpenMenu(bool byId, int id, const char* which)
    {
        if (std::this_thread::get_id() != m_thread)
            return;
        const int top = lua_gettop(m_state);
        lua_getglobal(m_state, "__manifoldMenuPopup");
        if (lua_isfunction(m_state, -1))
        {
            if (byId)
                lua_pushinteger(m_state, id);
            else
                lua_pushstring(m_state, which);
            std::string error;
            if (!CallStack(1, 0, error))
                m_engine.Notify(Status::Fail(ErrorCode::LuaRuntime, error));
        }
        lua_settop(m_state, top);
    }

    std::vector<ScriptMenuItem> LuaEngine::MenuItems(ScriptMenu menu)
    {
        return ReadMenu(false, 0, menu == ScriptMenu::MemoryViewPopup ? "popup" : "menu");
    }

    std::vector<ScriptMenuItem> LuaEngine::MenuItemsOf(int menu)
    {
        return ReadMenu(true, menu, nullptr);
    }

    void LuaEngine::MenuOpening(ScriptMenu menu)
    {
        OpenMenu(false, 0, menu == ScriptMenu::MemoryViewPopup ? "popup" : "menu");
    }

    void LuaEngine::MenuOpeningOf(int menu)
    {
        OpenMenu(true, menu, nullptr);
    }

    void LuaEngine::ClickMenuItem(int id)
    {
        RunCallback("__manifoldMenuClick(" + std::to_string(id) + ")", "menu item");
    }

    std::vector<ScriptForm> LuaEngine::ShownForms()
    {
        std::vector<ScriptForm> forms;
        if (!m_formsShown || std::this_thread::get_id() != m_thread)
            return forms;
        const int top = lua_gettop(m_state);
        lua_getglobal(m_state, "__manifoldFormSnapshot");
        if (!lua_isfunction(m_state, -1))
        {
            lua_settop(m_state, top);
            m_formsShown = false;
            return forms;
        }
        std::string error;
        if (!CallStack(0, 1, error))
        {
            LOG_WARN("Lua", "Reading the script forms failed: " + error);
            lua_settop(m_state, top);
            m_formsShown = false;
            return forms;
        }
        if (lua_istable(m_state, -1))
        {
            const int list = lua_gettop(m_state);
            const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(m_state, list));
            for (lua_Integer i = 1; i <= count; ++i)
            {
                lua_rawgeti(m_state, list, i);
                if (lua_istable(m_state, -1))
                    forms.push_back(ReadForm(m_state, lua_gettop(m_state)));
                lua_pop(m_state, 1);
            }
        }
        lua_settop(m_state, top);
        if (forms.empty())
            m_formsShown = false;
        return forms;
    }

    void LuaEngine::SetFormValue(int control, std::string_view field, std::string_view value)
    {
        if (std::this_thread::get_id() != m_thread)
            return;
        const int top = lua_gettop(m_state);
        lua_getglobal(m_state, "__manifoldFormSet");
        if (lua_isfunction(m_state, -1))
        {
            lua_pushinteger(m_state, control);
            lua_pushlstring(m_state, field.data(), field.size());
            lua_pushlstring(m_state, value.data(), value.size());
            std::string error;
            if (!CallStack(3, 0, error))
                LOG_WARN("Lua", "Updating a script form failed: " + error);
        }
        lua_settop(m_state, top);
    }

    void LuaEngine::FireFormEvent(int control, std::string_view event, std::initializer_list<int> arguments)
    {
        std::string code = "__manifoldFormFire(" + std::to_string(control) + ", " + Quoted(event);
        for (const int argument : arguments)
            code += ", " + std::to_string(argument);
        RunCallback(code + ")", "form event");
    }

    ScriptCanvas& LuaEngine::CanvasFor(int id)
    {
        return m_canvases[id];
    }

    const ScriptCanvas* LuaEngine::FindCanvas(int id) const
    {
        const auto found = m_canvases.find(id);
        return found == m_canvases.end() ? nullptr : &found->second;
    }

    void LuaEngine::ResetCanvas(int id)
    {
        ScriptCanvas& canvas = m_canvases[id];
        canvas.Commands.clear();
        canvas.Texts.clear();
        canvas.Coordinates.clear();
        ++canvas.Generation;
    }

    void LuaEngine::FreeCanvas(int id)
    {
        m_canvases.erase(id);
    }

    void LuaEngine::CloseForm(int form)
    {
        RunCallback("__manifoldFormClose(" + std::to_string(form) + ")", "form close");
    }

    void LuaEngine::SetFormBounds(int form, int left, int top, int width, int height, int clientWidth, int clientHeight)
    {
        if (std::this_thread::get_id() != m_thread)
            return;
        const int stackTop = lua_gettop(m_state);
        lua_getglobal(m_state, "__manifoldFormBounds");
        if (lua_isfunction(m_state, -1))
        {
            lua_pushinteger(m_state, form);
            lua_pushinteger(m_state, left);
            lua_pushinteger(m_state, top);
            lua_pushinteger(m_state, width);
            lua_pushinteger(m_state, height);
            lua_pushinteger(m_state, clientWidth);
            lua_pushinteger(m_state, clientHeight);
            std::string error;
            if (!CallStack(7, 0, error))
                LOG_WARN("Lua", "Resizing a script form failed: " + error);
        }
        lua_settop(m_state, stackTop);
    }
}
