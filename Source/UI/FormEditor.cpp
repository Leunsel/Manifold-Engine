#include "UI/FormEditor.h"

#include "App/Engine.h"
#include "Core/IconNames.h"
#include "Core/ImageDecode.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/DocRecord.h"
#include "Lua/LuaEngine.h"
#include "Lua/UiFormImport.h"
#include "UI/Fonts.h"
#include "UI/LuaWindow.h"
#include "UI/ScriptWindow.h"
#include "UI/Shortcuts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        using Lua::FormDocument;
        using Lua::FormPropType;
        using Lua::UiKind;
        using Lua::UiNode;

        constexpr const char* kKindPayload = "MFORM_KIND";
        constexpr const char* kNodePayload = "MFORM_NODE";
        constexpr const char* kRecordPayload = "MANIFOLD_RECORD"; // a record dragged from the address list (its id)
        constexpr std::size_t kUndoLimit = 200;
        constexpr const char* kRunChunk = "=Form Editor";

        const char* KindIcon(UiKind kind)
        {
            switch (kind)
            {
            case UiKind::Window: return ICON_FA_WINDOW_MAXIMIZE;
            case UiKind::Row: return ICON_FA_GRIP_HORIZONTAL;
            case UiKind::Column: return ICON_FA_GRIP_VERTICAL;
            case UiKind::Grid: return ICON_FA_BORDER_ALL;
            case UiKind::Section: return ICON_FA_OBJECT_GROUP;
            case UiKind::Tabs: return ICON_FA_FOLDER;
            case UiKind::Tab: return ICON_FA_FILE;
            case UiKind::Split: return ICON_FA_COLUMNS;
            case UiKind::Text: return ICON_FA_FONT;
            case UiKind::Heading: return ICON_FA_HEADING;
            case UiKind::Separator: return ICON_FA_MINUS;
            case UiKind::Spacer: return ICON_FA_ARROWS_ALT_V;
            case UiKind::Button: return ICON_FA_HAND_POINTER;
            case UiKind::Checkbox: return ICON_FA_CHECK_SQUARE;
            case UiKind::Input: return ICON_FA_KEYBOARD;
            case UiKind::Number: return ICON_FA_SORT_NUMERIC_UP;
            case UiKind::Slider: return ICON_FA_SLIDERS_H;
            case UiKind::Combo: return ICON_FA_CARET_SQUARE_DOWN;
            case UiKind::Radio: return ICON_FA_DOT_CIRCLE;
            case UiKind::Color: return ICON_FA_PALETTE;
            case UiKind::Table: return ICON_FA_TABLE;
            case UiKind::Tree: return ICON_FA_SITEMAP;
            case UiKind::Progress: return ICON_FA_TASKS;
            case UiKind::Image: return ICON_FA_IMAGE;
            case UiKind::Canvas: return ICON_FA_PAINT_BRUSH;
            }
            return ICON_FA_SQUARE;
        }

        std::string TitleCase(std::string_view key)
        {
            std::string out;
            for (std::size_t i = 0; i < key.size(); ++i)
            {
                const char c = key[i];
                if (i == 0)
                    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                else if (std::isupper(static_cast<unsigned char>(c)))
                {
                    out.push_back(' ');
                    out.push_back(c);
                }
                else
                    out.push_back(c);
            }
            return out;
        }

        std::string Caption(const FormDocument& form, const UiNode& node)
        {
            const auto meta = form.Meta.find(node.Id);
            std::string text = std::string(Lua::FormKindTitle(node.Kind));
            const std::string name = meta == form.Meta.end() ? std::string() : meta->second.Name;
            std::string detail;
            switch (node.Kind)
            {
            case UiKind::Window:
            case UiKind::Section:
            case UiKind::Tab:
                detail = node.Title;
                break;
            case UiKind::Text:
            case UiKind::Heading:
            case UiKind::Button:
            case UiKind::Separator:
                detail = node.Text;
                break;
            default:
                detail = node.Label;
                break;
            }
            if (const std::size_t line = detail.find('\n'); line != std::string::npos)
                detail = detail.substr(0, line) + "...";
            if (detail.size() > 40)
                detail = detail.substr(0, 37) + "...";
            if (!name.empty())
                text += "  " + name;
            if (!detail.empty())
                text += "  \"" + detail + "\"";
            return text;
        }

        enum class Family
        {
            Layout,
            Text,
            Input,
            Data,
        };

        Family FamilyOf(UiKind kind)
        {
            switch (kind)
            {
            case UiKind::Window:
            case UiKind::Row:
            case UiKind::Column:
            case UiKind::Grid:
            case UiKind::Section:
            case UiKind::Tabs:
            case UiKind::Tab:
            case UiKind::Split:
                return Family::Layout;
            case UiKind::Text:
            case UiKind::Heading:
            case UiKind::Separator:
            case UiKind::Spacer:
            case UiKind::Progress:
            case UiKind::Image:
                return Family::Text;
            case UiKind::Table:
            case UiKind::Tree:
            case UiKind::Canvas:
                return Family::Data;
            default:
                return Family::Input;
            }
        }

        const char* FamilyTitle(Family family)
        {
            switch (family)
            {
            case Family::Layout: return "Layout";
            case Family::Text: return "Text and Media";
            case Family::Input: return "Input";
            case Family::Data: return "Data";
            }
            return "";
        }

        ImVec4 FamilyColor(Family family)
        {
            switch (family)
            {
            case Family::Layout: return ManifoldTheme::Accent;
            case Family::Text: return ManifoldTheme::CodeString;
            case Family::Input: return ManifoldTheme::CodeKeyword;
            case Family::Data: return ManifoldTheme::CodeNumber;
            }
            return ManifoldTheme::Text;
        }

        void DashedLine(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color, float thickness, float dash, float gap)
        {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length <= 0.0f)
                return;
            const float ux = dx / length;
            const float uy = dy / length;
            for (float at = 0.0f; at < length; at += dash + gap)
            {
                const float end = std::min(length, at + dash);
                draw->AddLine(ImVec2(a.x + ux * at, a.y + uy * at), ImVec2(a.x + ux * end, a.y + uy * end), color, thickness);
            }
        }

        void DashedRect(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color, float thickness, float dash, float gap)
        {
            DashedLine(draw, min, ImVec2(max.x, min.y), color, thickness, dash, gap);
            DashedLine(draw, ImVec2(max.x, min.y), max, color, thickness, dash, gap);
            DashedLine(draw, max, ImVec2(min.x, max.y), color, thickness, dash, gap);
            DashedLine(draw, ImVec2(min.x, max.y), min, color, thickness, dash, gap);
        }

        void Hatch(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 color, float step)
        {
            draw->PushClipRect(min, max, true);
            const float height = max.y - min.y;
            for (float x = min.x - height; x < max.x; x += step)
                draw->AddLine(ImVec2(x, max.y), ImVec2(x + height, min.y), color, 1.0f);
            draw->PopClipRect();
        }

        bool GroupHeader(const char* label, bool open)
        {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            const bool shown = ImGui::CollapsingHeader(label, open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            ImGui::PopStyleColor(4);
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), ImGui::GetColorU32(ManifoldTheme::Border), 1.0f);
            return shown;
        }

        bool Matches(std::string_view text, std::string_view filter)
        {
            if (filter.empty())
                return true;
            return Lower(text).find(Lower(filter)) != std::string::npos;
        }

        int GroupOf(std::string_view key, UiKind kind)
        {
            static constexpr std::string_view kWindowKeys[] = { "width", "height", "minWidth", "minHeight", "maxWidth", "maxHeight", "resizable", "autoSize", "scrollbar",
                "topmost", "position", "dock", "opacity", "inactiveOpacity", "interval", "open" };
            if (kind == UiKind::Window)
                for (std::string_view entry : kWindowKeys)
                    if (entry == key)
                        return 4;
            static constexpr std::string_view kContent[] = { "text", "title", "label", "icon", "badge", "hint", "items", "menu", "rows", "image" };
            static constexpr std::string_view kValue[] = { "multi", "value", "min", "max", "step", "integer", "hex", "format", "alpha" };
            static constexpr std::string_view kBehaviour[] = { "visible", "enabled", "tooltip", "shortcut", "confirm", "readOnly", "maxLength", "focus" };
            for (std::string_view entry : kContent)
                if (entry == key)
                    return 0;
            for (std::string_view entry : kValue)
                if (entry == key)
                    return 1;
            for (std::string_view entry : kBehaviour)
                if (entry == key)
                    return 3;
            return 2;
        }

        constexpr const char* kGroupTitles[] = { "Content", "Value", "Look and Layout", "Behaviour", "Window" };

        bool ReadFile(const std::filesystem::path& path, std::string& text)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            std::stringstream content;
            content << file.rdbuf();
            text = content.str();
            return true;
        }

        bool WriteFile(const std::filesystem::path& path, std::string_view text)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            return static_cast<bool>(file);
        }

        std::string ThemeName(const char* name)
        {
            std::string out(name);
            if (!out.empty())
                out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
            return out;
        }

        std::string FormStem(const std::filesystem::path& path)
        {
            std::string name = WideToUtf8(path.filename().wstring());
            for (const char* suffix : { ".form.lua", ".lua" })
            {
                const std::string_view end(suffix);
                if (name.size() > end.size() && EqualsNoCase(std::string_view(name).substr(name.size() - end.size()), end))
                    return name.substr(0, name.size() - end.size());
            }
            return name;
        }

        std::string SafeFileName(std::string_view title)
        {
            std::string out;
            for (const char c : title)
            {
                if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
                    continue;
                out.push_back(c);
            }
            out = Trim(out);
            return out.empty() ? std::string("Form") : out;
        }

        // why a name is no Lua name, so the message says how to fix it
        std::string NameError(const std::string& name)
        {
            const bool plain = !name.empty() && !std::isdigit(static_cast<unsigned char>(name[0])) &&
                               std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
            if (plain)
                return "\"" + name + "\" is a Lua keyword; pick another name, like " + name + "Value";
            return "A name is letters, digits and _, and does not start with a digit";
        }

        // ∑ A script as the editor works on it: LF line ends, no UTF-8 BOM. What
        // the file had is kept and put back when the file is written.
        struct FileText
        {
            std::string Text;
            bool Crlf = false;
            bool Bom = false;
        };

        FileText PlainText(std::string_view raw)
        {
            FileText out;
            if (raw.size() >= 3 && raw.substr(0, 3) == "\xEF\xBB\xBF")
            {
                out.Bom = true;
                raw.remove_prefix(3);
            }
            out.Crlf = raw.find("\r\n") != std::string_view::npos;
            out.Text.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
                    continue;
                out.Text.push_back(raw[i]);
            }
            return out;
        }

        std::string FileBytes(const std::string& text, bool crlf, bool bom)
        {
            std::string out = bom ? std::string("\xEF\xBB\xBF") : std::string();
            out.reserve(text.size() + text.size() / 20 + 3);
            for (const char c : text)
            {
                if (c == '\n' && crlf)
                    out.push_back('\r');
                out.push_back(c);
            }
            return out;
        }

        // ∑ What a widget of the open form holds, for get/set in the script pane.
        std::string ValueText(const UiNode& widget)
        {
            const auto item = [&](int number) {
                return number >= 1 && number <= static_cast<int>(widget.Items.size()) ? " (" + std::to_string(number) + " = \"" + widget.Items[static_cast<std::size_t>(number - 1)] + "\")" : std::string();
            };
            switch (widget.Kind)
            {
            case UiKind::Checkbox: return std::string(widget.Style == "switch" ? "a boolean: the switch is on" : "a boolean: the box is ticked");
            case UiKind::Input: return "its text, a string";
            case UiKind::Number:
            case UiKind::Slider: return widget.Integer ? "a whole number" : "a number";
            case UiKind::Combo:
                if (widget.Multi)
                    return "the list of the ticked items' numbers, like { 1, 3 }" + item(1) + "; an empty list for none";
                return "the chosen item's number" + item(1) + ", or nil for none";
            case UiKind::Radio: return "the chosen item's number" + item(1) + ", or nil for none";
            case UiKind::Color: return "a colour as \"#RRGGBB\"";
            case UiKind::Table: return "the selected row's number, or nil";
            case UiKind::Tree: return "the selected item's id, or nil";
            case UiKind::Progress: return "the fraction, 0 to 1";
            default: return "its text";
            }
        }

        std::string PropType(const Lua::FormProp& prop)
        {
            switch (prop.Type)
            {
            case FormPropType::Bool: return "boolean";
            case FormPropType::Number: return "number";
            case FormPropType::Integer: return "integer";
            case FormPropType::Choice:
            {
                std::string out;
                std::string_view choices = prop.Choices;
                while (!choices.empty())
                {
                    const std::size_t bar = choices.find('|');
                    out += (out.empty() ? "\"" : " | \"") + std::string(choices.substr(0, bar)) + "\"";
                    choices = bar == std::string_view::npos ? std::string_view() : choices.substr(bar + 1);
                }
                return out;
            }
            case FormPropType::Color: return "colour";
            case FormPropType::Icon: return "icon name";
            case FormPropType::File: return "image";
            case FormPropType::Items:
            case FormPropType::Columns:
            case FormPropType::Rows:
            case FormPropType::Tree: return "table";
            default: return "string";
            }
        }

        // ∑ update/get/set of a named widget of the open form: the general
        // text plus what this widget's kind takes, key by key
        std::shared_ptr<const Lua::DocRecord> WidgetDoc(const Lua::DocRecord& base, const UiNode& widget, const std::string& name, const std::string& what)
        {
            auto record = std::make_shared<Lua::DocRecord>(base);
            const std::string kind(Lua::FormKindTitle(widget.Kind));
            if (what == "update")
            {
                std::vector<std::pair<std::string, std::string>> keys;
                std::string list;
                for (const Lua::FormProp& prop : Lua::FormProps(widget.Kind))
                {
                    keys.push_back({ std::string(prop.Key), PropType(prop) });
                    list += "\n- `" + std::string(prop.Key) + "` (" + PropType(prop) + "): " + std::string(prop.Help);
                }
                for (const Lua::FormHandler& handler : Lua::FormHandlers(widget.Kind))
                {
                    keys.push_back({ std::string(handler.Key), "function" });
                    list += "\n- `" + std::string(handler.Key) + "(" + std::string(handler.Params) + ")`: " + std::string(handler.Help);
                }
                record->Summary = "Changes options of `" + name + "` (" + kind + "). Pass only the keys to change; the others keep their values, and a new `value` does not call `onChange`. Example: `" + name +
                                  ".update({ enabled = false })`.\n\nWhat a " + kind + " takes:" + list;
                record->Text = record->Summary + "\n\n" + base.Text;
                if (!record->Signatures.empty() && !record->Signatures.front().Params.empty())
                    record->Signatures.front().Params.front().Keys = std::move(keys);
            }
            else
            {
                record->Summary = (what == "get" ? "The value of `" + name + "` (" + kind + "): " : "Sets the value of `" + name + "` (" + kind + "), without calling `onChange`: ") + ValueText(widget) + ".";
                record->Text = record->Summary + "\n\n" + base.Text;
            }
            return record;
        }
    }

    FormEditorWindow::FormEditorWindow(Workspace& workspace) :
        m_workspace(workspace),
        m_engine(workspace.Core()),
        m_unsavedDialog("formunsaved", ICON_FA_SAVE, "Unsaved Changes"),
        m_problemsDialog("formproblems", ICON_FA_EXCLAMATION_TRIANGLE, "Form Problems", ImVec2(560.0f, 320.0f)),
        m_importDialog("formimport", ICON_FA_FILE_IMPORT, "Import Cheat Engine Form", ImVec2(520.0f, 320.0f)),
        m_notesDialog("formimportnotes", ICON_FA_FILE_IMPORT, "Import Notes", ImVec2(680.0f, 420.0f))
    {
        m_host = std::make_unique<UiHost>(m_engine, workspace.Window(), &m_form.Model);
        m_tryHost = std::make_unique<UiHost>(m_engine, workspace.Window(), &m_try.Model);
        m_codeEditor.ReadOnly = true;
        // ∑ The script pane completes like a script window (the Lua engine's
        // globals and documented classes) and knows the open form: after the
        // widgets table of form.load (`w.` in the generated script) it offers
        // the named widgets, and after one of them, after the window variable
        // or after a handler's `widget` parameter, the widget/window methods.
        m_assist = MakeLuaAssist(m_engine);
        {
            const auto base = m_assist.Complete;
            const auto baseDescribe = m_assist.Describe;
            const auto classOf = [this](const std::string& chain, std::string& member) -> std::string {
                std::string windowName = "window";
                std::string widgetsName = "w";
                std::string modalsName = "modals";
                {
                    // `window, w = ui.form.load(` / `local win, widgets = manifold.ui.form.build(`
                    const std::string& text = m_form.ScriptText;
                    for (const std::string_view call : { std::string_view("form.load("), std::string_view("form.build(") })
                    {
                        const std::size_t at = text.find(call);
                        if (at == std::string::npos)
                            continue;
                        std::size_t lineStart = text.rfind('\n', at);
                        lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
                        std::string head = text.substr(lineStart, at - lineStart);
                        const std::size_t equals = head.find('=');
                        if (equals == std::string::npos)
                            continue;
                        head = head.substr(0, equals);
                        if (head.rfind("local ", 0) == 0)
                            head = head.substr(6);
                        const std::size_t comma = head.find(',');
                        if (comma == std::string::npos)
                            continue;
                        const std::string first = Trim(std::string_view(head).substr(0, comma));
                        std::string second = Trim(std::string_view(head).substr(comma + 1));
                        if (const std::size_t more = second.find(','); more != std::string::npos)
                        {
                            const std::string third = Trim(std::string_view(second).substr(more + 1));
                            second = Trim(std::string_view(second).substr(0, more));
                            if (Lua::FormIsIdentifier(third))
                                modalsName = third;
                        }
                        if (Lua::FormIsIdentifier(first))
                            windowName = first;
                        if (Lua::FormIsIdentifier(second))
                            widgetsName = second;
                        break;
                    }
                }
                member.clear();
                if (chain == windowName)
                    return "manifold.ui.window.Window";
                if (chain == modalsName)
                    return "*modals";
                if (chain.size() > modalsName.size() + 1 && chain.compare(0, modalsName.size() + 1, modalsName + ".") == 0 &&
                    Lua::FormIsIdentifier(chain.substr(modalsName.size() + 1)))
                    return "manifold.ui.window.Window";
                if (chain == "widget")
                    return "manifold.ui.window.Widget";
                if (chain == widgetsName)
                    return "*widgets";
                if (chain.size() > widgetsName.size() + 1 && chain.compare(0, widgetsName.size() + 1, widgetsName + ".") == 0)
                {
                    const std::string rest = chain.substr(widgetsName.size() + 1);
                    if (!Lua::FormIsIdentifier(rest))
                        return {};
                    for (const auto& [id, meta] : m_form.Meta)
                    {
                        if (meta.Name == rest)
                            return "manifold.ui.window.Widget";
                    }
                    if (rest.find('.') == std::string::npos)
                        member = rest;
                    return member.empty() ? std::string() : std::string("*widgets");
                }
                return {};
            };
            m_assist.Complete = [this, base, classOf](const std::string& chain, const std::string& prefix, bool method) {
                std::string member;
                const std::string owner = classOf(chain, member);
                if (owner == "*modals")
                {
                    std::vector<CodeSuggestion> names;
                    for (const int modal : m_form.Modals)
                    {
                        const std::string name = Lua::FormModalName(m_form, modal);
                        const UiNode* node = m_form.Model.Find(modal);
                        if (name.empty() || name.compare(0, prefix.size(), prefix) != 0)
                            continue;
                        names.push_back({ name, "object", "  Modal", "Modal form \"" + (node ? node->Title : name) + "\": a window object (show, hide, ...).", nullptr });
                    }
                    return names;
                }
                if (owner == "*widgets" && member.empty())
                {
                    std::vector<CodeSuggestion> names;
                    for (const auto& [id, meta] : m_form.Meta)
                    {
                        if (meta.Name.empty() || id == m_form.Window || Lua::FormIsModal(m_form, id) || meta.Name.compare(0, prefix.size(), prefix) != 0)
                            continue;
                        const UiNode* node = m_form.Model.Find(id);
                        const std::string kind = node ? std::string(Lua::FormKindTitle(node->Kind)) : std::string("Widget");
                        names.push_back({ meta.Name, "object", "  " + kind, kind + " \"" + meta.Name + "\" of this form: a widget object (value, set, get, ...).", nullptr });
                    }
                    std::sort(names.begin(), names.end(), [](const CodeSuggestion& a, const CodeSuggestion& b) { return a.Name < b.Name; });
                    return names;
                }
                if (!owner.empty() && owner[0] != '*')
                {
                    std::vector<Lua::LuaCompletion> items = m_engine.Lua().CompleteClass(owner, prefix, method);
                    std::vector<CodeSuggestion> result;
                    result.reserve(items.size());
                    for (Lua::LuaCompletion& item : items)
                        result.push_back({ std::move(item.Name), std::move(item.Kind), std::move(item.Signature), std::move(item.Text), std::move(item.Doc) });
                    return result;
                }
                return base(chain, prefix, method);
            };
            m_assist.Describe = [this, baseDescribe, classOf](const std::string& chain, bool method) -> std::shared_ptr<const Lua::DocRecord> {
                const std::size_t dot = chain.find_last_of(".:");
                if (dot != std::string::npos)
                {
                    std::string member;
                    const std::string owner = classOf(chain.substr(0, dot), member);
                    if (!owner.empty() && owner[0] != '*')
                    {
                        std::shared_ptr<const Lua::DocRecord> record = m_engine.Lua().ClassMember(owner, chain.substr(dot + 1));
                        // a widget of this form: its own options and value, not just the general text
                        const std::string what = chain.substr(dot + 1);
                        const std::string holder = chain.substr(0, dot);
                        const std::size_t nameDot = holder.find_last_of('.');
                        const UiNode* widget = m_form.Model.Find(Lua::FormFindNamed(m_form, nameDot == std::string::npos ? holder : holder.substr(nameDot + 1)));
                        if (record && widget && owner == "manifold.ui.window.Widget" && (what == "update" || what == "get" || what == "set"))
                            return WidgetDoc(*record, *widget, holder.substr(nameDot == std::string::npos ? 0 : nameDot + 1), what);
                        return record;
                    }
                }
                return baseDescribe(chain, method);
            };
        }
        m_scriptEditor.Assist = &m_assist;
        Lua::FormDocument form = Lua::NewForm("New Form");
        const int section = Lua::FormAdd(form, UiKind::Section, form.Window);
        Lua::FormAdd(form, UiKind::Checkbox, section);
        Lua::FormAdd(form, UiKind::Slider, section);
        const int row = Lua::FormAdd(form, UiKind::Row, form.Window);
        form.Model.Find(row)->Align = "right";
        Lua::FormAdd(form, UiKind::Button, row);
        Reset(std::move(form), {}, true);
    }

    FormEditorWindow::~FormEditorWindow() = default;

    void FormEditorWindow::Reset(Lua::FormDocument form, std::filesystem::path path, bool oneFile)
    {
        m_form = std::move(form);
        m_path = std::move(path);
        m_oneFile = oneFile;
        m_fileDisk.clear();
        m_crlf = false;
        m_bom = false;
        m_undo.clear();
        m_redo.clear();
        m_lastSource = 0;
        m_dirty = false;
        m_trying = false;
        m_selected = m_form.Window;
        m_hovered = 0;
        m_scriptPath.clear();
        m_scriptDisk.clear();
        m_scriptConflict = false;
        m_chipPlaces.clear();
        m_pan = ImVec2(0.0f, 0.0f);
        if (m_oneFile)
        {
            // the script is part of the document from the start, saved or not
            m_form.Script.clear();
            // only a new document gets a generated script: an opened one is never replaced
            if (m_form.ScriptText.empty())
                m_form.ScriptText = Lua::FormScript(m_form, Lua::FormScriptStyle::OneFile);
            m_form.ScriptLoaded = true;
            m_scriptDisk = m_form.ScriptText;
            m_scriptPath = m_path;
            m_scriptStamp = m_path.empty() ? 0 : Lua::UiModel::FileStamp(WideToUtf8(m_path.wstring()));
            m_nextScriptPoll = 0.0;
        }
        else
            m_form.ScriptLoaded = false;
        ++m_revision;
    }

    bool FormEditorWindow::ScriptDirty() const
    {
        return m_form.ScriptLoaded && m_form.ScriptText != m_scriptDisk;
    }

    bool FormEditorWindow::Dirty() const
    {
        return m_dirty || ScriptDirty();
    }

    std::string FormEditorWindow::Title() const
    {
        std::string title = ICON_FA_PENCIL_RULER "  Form Editor";
        if (!m_path.empty())
            title += "  -  " + WideToUtf8(m_path.filename().wstring());
        if (Dirty())
            title += " *";
        return title + "###formeditor";
    }

    std::string FormEditorWindow::ScriptFileName() const
    {
        if (!m_path.empty() && m_oneFile)
            return WideToUtf8(m_path.filename().wstring());
        if (!m_path.empty())
            return FormStem(m_path) + ".lua";
        const UiNode* window = m_form.Model.Find(m_form.Window);
        return SafeFileName(window ? window->Title : std::string()) + ".lua";
    }

    const std::string& FormEditorWindow::Code()
    {
        if (m_codeRevision != m_revision || m_codeStyle != m_style)
        {
            const std::string file = m_path.empty() || m_oneFile ? FormStem(ScriptFileName()) + ".form.lua" : WideToUtf8(m_path.filename().wstring());
            if (m_style == 2)
                m_code = Lua::FormComposeScript(Lua::FormScript(m_form, Lua::FormScriptStyle::OneFile), m_form).value_or(std::string());
            else
                m_code = Lua::FormScript(m_form, m_style == 0 ? Lua::FormScriptStyle::Standalone : Lua::FormScriptStyle::FormFile, file);
            m_codeRevision = m_revision;
            m_codeStyle = m_style;
        }
        return m_code;
    }

    bool FormEditorWindow::OpenFile(const std::filesystem::path& path)
    {
        std::string text;
        if (!ReadFile(path, text))
        {
            m_engine.Notify(LogLevel::Error, "Could not read " + WideToUtf8(path.wstring()));
            return false;
        }
        Lua::FormDocument form;
        std::string error;
        std::vector<std::string> warnings;
        const std::string name = WideToUtf8(path.filename().wstring());
        const bool formFile = name.size() > 9 && EqualsNoCase(std::string_view(name).substr(name.size() - 9), ".form.lua");
        // ∑ the editor works on LF text without a BOM; the file keeps its own
        // line ends and BOM when it is written back (see SaveScript)
        const FileText plain = PlainText(text);
        if (!(formFile ? Lua::FormLoad(plain.Text, form, error, &warnings) : Lua::FormLoadScript(plain.Text, form, error, &warnings)))
        {
            m_engine.Notify(LogLevel::Error, name + ": " + error);
            return false;
        }
        std::string view;
        if (!formFile)
        {
            view = Lua::FormScriptView(plain.Text);
            if (!Lua::FormViewIntact(view))
            {
                // never replace a script the editor cannot read back exactly
                m_engine.Notify(LogLevel::Error, name + " already holds the line \"local form = " + std::string(Lua::kFormStandIn) +
                                                     "\" that the Form Editor shows in place of the form, so it was not opened. Remove that line in a script window and open it again.");
                return false;
            }
            form.ScriptText = view;
        }
        Reset(std::move(form), path, !formFile);
        if (!formFile)
        {
            m_fileDisk = text;
            m_crlf = plain.Crlf;
            m_bom = plain.Bom;
        }
        for (const std::string& warning : warnings)
            m_engine.Notify(LogLevel::Warning, WideToUtf8(path.filename().wstring()) + ": " + warning);
        Show();
        return true;
    }

    void FormEditorWindow::Remember(ImGuiID source)
    {
        if (source != 0 && source == m_lastSource)
            return;
        m_undo.push_back(m_form);
        if (m_undo.size() > kUndoLimit)
            m_undo.pop_front();
        m_redo.clear();
        m_lastSource = source;
    }

    void FormEditorWindow::Changed()
    {
        m_dirty = true;
        ++m_revision;
        m_form.Model.Changed();
    }

    void FormEditorWindow::Undo()
    {
        if (m_undo.empty())
            return;
        m_redo.push_back(std::move(m_form));
        m_form = std::move(m_undo.back());
        m_undo.pop_back();
        m_lastSource = 0;
        m_scriptEditor.KeepCursor = true;
        if (!m_form.Model.Find(m_selected))
            m_selected = m_form.Window;
        m_dirty = true;
        ++m_revision;
    }

    void FormEditorWindow::Redo()
    {
        if (m_redo.empty())
            return;
        m_undo.push_back(std::move(m_form));
        m_form = std::move(m_redo.back());
        m_redo.pop_back();
        m_lastSource = 0;
        m_scriptEditor.KeepCursor = true;
        if (!m_form.Model.Find(m_selected))
            m_selected = m_form.Window;
        m_dirty = true;
        ++m_revision;
    }

    void FormEditorWindow::Select(int id)
    {
        if (!m_form.Model.Find(id))
            id = m_form.Window;
        if (id != m_selected)
        {
            m_selected = id;
            m_scrollOutline = true;
            m_lastSource = 0;
        }
        const int tab = TabOf(id);
        if (tab && tab != m_lastTab)
            m_design.ForceTab = tab;
        m_lastTab = tab;
    }

    int FormEditorWindow::Container(int id) const
    {
        const UiNode* node = m_form.Model.Find(id);
        if (!node)
            return m_form.Window;
        return Lua::UiIsContainer(node->Kind) ? id : node->Parent;
    }

    int FormEditorWindow::TabOf(int id) const
    {
        for (const UiNode* node = m_form.Model.Find(id); node; node = m_form.Model.Find(node->Parent))
        {
            if (node->Kind == UiKind::Tab)
                return node->Id;
        }
        return 0;
    }

    void FormEditorWindow::AddInto(UiKind kind, int parent, int index)
    {
        const UiNode* holder = m_form.Model.Find(parent);
        if (!holder || !Lua::FormCanHold(holder->Kind, kind))
            return;
        Remember();
        const int id = Lua::FormAdd(m_form, kind, parent, index);
        if (!id)
        {
            m_undo.pop_back();
            return;
        }
        Changed();
        Select(id);
    }

    void FormEditorWindow::Add(UiKind kind)
    {
        const UiNode* selected = m_form.Model.Find(m_selected);
        if (!selected)
            selected = m_form.Model.Find(m_form.Window);
        if (Lua::FormCanHold(selected->Kind, kind) && (selected->Kind == UiKind::Window || selected->Kind != UiKind::Tabs || kind == UiKind::Tab))
        {
            AddInto(kind, selected->Id, -1);
            return;
        }
        if (const UiNode* parent = m_form.Model.Find(selected->Parent); parent && Lua::FormCanHold(parent->Kind, kind))
        {
            AddInto(kind, parent->Id, Lua::FormIndexOf(m_form, selected->Id) + 1);
            return;
        }
        for (const UiNode* node = selected; node; node = m_form.Model.Find(node->Parent))
        {
            if (Lua::FormCanHold(node->Kind, kind))
            {
                AddInto(kind, node->Id, -1);
                return;
            }
        }
        m_engine.Notify(LogLevel::Warning, kind == UiKind::Tab ? "A tab goes into a Tabs widget: select one first." : "Select a container to add this widget to.");
    }

    bool FormEditorWindow::DropAt(int target, Drop where, int& parent, int& index) const
    {
        const UiNode* node = m_form.Model.Find(target);
        if (!node)
            return false;
        if (where == Drop::Inside || node->Kind == UiKind::Window)
        {
            parent = Lua::UiIsContainer(node->Kind) ? target : node->Parent;
            index = Lua::UiIsContainer(node->Kind) ? -1 : Lua::FormIndexOf(m_form, target) + 1;
            return true;
        }
        parent = node->Parent;
        index = Lua::FormIndexOf(m_form, target) + (where == Drop::After ? 1 : 0);
        return parent != 0;
    }

    void FormEditorWindow::Move(int delta)
    {
        const UiNode* node = m_form.Model.Find(m_selected);
        if (!node || node->Kind == UiKind::Window)
            return;
        const UiNode* parent = m_form.Model.Find(node->Parent);
        const int index = Lua::FormIndexOf(m_form, m_selected);
        const int target = index + delta;
        if (!parent || target < 0 || target >= static_cast<int>(parent->Children.size()))
            return;
        Remember();
        Lua::FormMove(m_form, m_selected, node->Parent, delta > 0 ? target + 1 : target);
        Changed();
    }

    void FormEditorWindow::Wrap(int id, UiKind kind)
    {
        const UiNode* node = m_form.Model.Find(id);
        if (!node || node->Kind == UiKind::Window || node->Kind == UiKind::Tab)
            return;
        const int parent = node->Parent;
        const UiNode* holder = m_form.Model.Find(parent);
        if (!holder || !Lua::FormCanHold(holder->Kind, kind))
            return;
        Remember();
        const int index = Lua::FormIndexOf(m_form, id);
        const int wrapper = Lua::FormAdd(m_form, kind, parent, index);
        for (const int child : std::vector<int>(m_form.Model.Find(wrapper)->Children))
            Lua::FormRemove(m_form, child);
        Lua::FormMove(m_form, id, wrapper, 0);
        Changed();
        Select(wrapper);
    }

    void FormEditorWindow::ChangeKind(int id, UiKind kind)
    {
        Remember();
        if (!Lua::FormChangeKind(m_form, id, kind))
        {
            m_undo.pop_back();
            return;
        }
        Changed();
        Select(id);
    }

    void FormEditorWindow::Unwrap(int id)
    {
        const UiNode* node = m_form.Model.Find(id);
        if (!node)
            return;
        const int parent = node->Parent;
        const int first = node->Children.empty() ? 0 : node->Children.front();
        Remember();
        if (!Lua::FormUnwrap(m_form, id))
        {
            m_undo.pop_back();
            m_engine.Notify(LogLevel::Warning, "The widgets inside cannot go where this container is.");
            return;
        }
        Changed();
        Select(first ? first : parent);
    }

    void FormEditorWindow::FixProblem(const Lua::FormProblem& problem)
    {
        Remember();
        if (!Lua::FormApplyFix(m_form, problem))
        {
            m_undo.pop_back();
            return;
        }
        Changed();
        Select(problem.Node);
    }

    // a problem the editor can repair gets a link under its text
    void FormEditorWindow::FixLink(const Lua::FormProblem& problem)
    {
        if (problem.Fix == Lua::FormFix::None)
            return;
        ImGui::Indent(Px(22.0f));
        const std::string label = std::string(ICON_FA_MAGIC "  ") + std::string(Lua::FormFixLabel(problem.Fix)) + "##fix";
        if (ImGui::TextLink(label.c_str()))
            FixProblem(problem);
        ItemTooltip("Repairs it now; Undo takes it back.");
        ImGui::Unindent(Px(22.0f));
    }

    void FormEditorWindow::SelectParent()
    {
        const UiNode* node = m_form.Model.Find(m_selected);
        if (node && node->Parent)
            Select(node->Parent);
    }

    // ∑ a double click on a widget shows its handler: the first one
    // that has a function, else the first it can have (added like the
    // handler's button in the properties does)
    void FormEditorWindow::OpenFirstHandler(int id)
    {
        UiNode* node = m_form.Model.Find(id);
        if (!node)
            return;
        const std::span<const Lua::FormHandler> handlers = Lua::FormHandlers(node->Kind);
        if (handlers.empty())
            return;
        const Lua::FormHandler* chosen = &handlers.front();
        if (const auto meta = m_form.Meta.find(id); meta != m_form.Meta.end())
        {
            for (const Lua::FormHandler& handler : handlers)
            {
                if (const auto found = meta->second.Handlers.find(std::string(handler.Key)); found != meta->second.Handlers.end() && !found->second.empty())
                {
                    chosen = &handler;
                    break;
                }
            }
        }
        OpenHandler(*node, *chosen);
    }

    // ∑ handlers named after the widget (goldChanged for gold) follow
    // its new name, in the form and in the script, unless another widget
    // still uses the old function or the new name is taken
    void FormEditorWindow::RenameFunctions(int id, const std::string& from, const std::string& to)
    {
        UiNode* node = m_form.Model.Find(id);
        const auto meta = m_form.Meta.find(id);
        if (!node || meta == m_form.Meta.end() || !Lua::FormIsIdentifier(to))
            return;
        const std::string oldBase = from.empty() ? std::string(Lua::UiKindName(node->Kind)) : from;
        std::vector<std::string> renamed;
        for (auto& [key, function] : meta->second.Handlers)
        {
            if (key.size() < 3 || function != oldBase + key.substr(2))
                continue;
            const std::string fresh = to + key.substr(2);
            bool shared = false;
            bool taken = Lua::FormFunctionLine(m_form.ScriptText, fresh) > 0;
            for (const auto& [other, otherMeta] : m_form.Meta)
            {
                for (const auto& [otherKey, otherFunction] : otherMeta.Handlers)
                {
                    if (other == id && otherKey == key)
                        continue;
                    shared = shared || otherFunction == function;
                    taken = taken || otherFunction == fresh;
                }
            }
            if (taken)
                continue;
            if (!shared && m_form.ScriptLoaded && Lua::FormFunctionLine(m_form.ScriptText, function) > 0)
            {
                m_form.ScriptText = Lua::FormRenameIdentifier(m_form.ScriptText, function, fresh);
                m_scriptEditor.KeepCursor = true;
            }
            renamed.push_back(function + " to " + fresh);
            function = fresh;
        }
        if (renamed.empty())
            return;
        std::string text = "Renamed ";
        for (std::size_t i = 0; i < renamed.size(); ++i)
            text += (i ? ", " : "") + renamed[i];
        m_engine.Notify(LogLevel::Info, text + ".");
    }

    void FormEditorWindow::Delete()
    {
        if (m_selected == m_form.Window || !m_form.Model.Find(m_selected))
            return;
        if (m_form.Model.Find(m_selected)->Kind == UiKind::Window && !Lua::FormIsModal(m_form, m_selected))
            return;
        const UiNode* node = m_form.Model.Find(m_selected);
        const int parent = node->Parent;
        const int index = Lua::FormIndexOf(m_form, m_selected);
        Remember();
        Lua::FormRemove(m_form, m_selected);
        Changed();
        const UiNode* holder = m_form.Model.Find(parent);
        if (holder && !holder->Children.empty())
            Select(holder->Children[static_cast<std::size_t>(std::min(index, static_cast<int>(holder->Children.size()) - 1))]);
        else
            Select(parent);
    }

    void FormEditorWindow::Duplicate()
    {
        if (!m_form.Model.Find(m_selected) || m_form.Model.Find(m_selected)->Kind == UiKind::Window)
            return;
        Remember();
        const int copy = Lua::FormDuplicate(m_form, m_selected);
        if (!copy)
        {
            m_undo.pop_back();
            return;
        }
        Changed();
        Select(copy);
    }

    void FormEditorWindow::ToggleTry()
    {
        m_trying = !m_trying;
        if (m_trying)
            m_try = Lua::CopyForm(m_form);
    }

    void FormEditorWindow::RunScript()
    {
        const std::filesystem::path script = LinkedScript();
        std::error_code missing;
        if (!script.empty() && std::filesystem::exists(script, missing))
        {
            if (Dirty() && !Save(false))
                return;
            const Status status = m_engine.Lua().RunFile(script);
            if (!status)
                m_engine.Notify(status);
            return;
        }
        // an unsaved one-file form runs as it stands, with its handlers; an older
        // form without a script is built from its table, so modals, rules and
        // record bindings work (a standalone script would leave them out)
        const std::string code = m_oneFile ? Lua::FormComposeScript(m_form.ScriptText, m_form).value_or(std::string())
                                           : Lua::FormBlockText(m_form, "form") + "manifold.ui.form.build(form)\n";
        const Status status = m_engine.Lua().Run(code, kRunChunk);
        if (!status)
            m_engine.Notify(status);
    }

    void FormEditorWindow::AskThen(Pending pending)
    {
        if (!Dirty())
        {
            m_pending = pending;
            DoPending();
            return;
        }
        m_pending = pending;
        m_unsavedDialog.Open();
    }

    void FormEditorWindow::DoPending()
    {
        const Pending pending = std::exchange(m_pending, Pending::None);
        switch (pending)
        {
        case Pending::New:
            Reset(Lua::NewForm("New Form"), {}, true);
            break;
        case Pending::Open:
            Open();
            break;
        case Pending::Close:
            m_dirty = false;
            m_open = false;
            break;
        case Pending::Import:
            Reset(std::move(m_pendingImport), {}, true);
            m_dirty = true;
            m_notesDialog.Open();
            break;
        case Pending::Template:
        {
            Lua::FormDocument form;
            std::string error;
            if (Lua::FormLoad(m_pendingTemplate, form, error))
            {
                form.Fragment = false;
                Reset(std::move(form), {}, true);
            }
            else
                m_engine.Notify(LogLevel::Error, "The template could not be read: " + error);
            m_pendingTemplate.clear();
            break;
        }
        case Pending::None:
            break;
        }
    }

    void FormEditorWindow::Open()
    {
        const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
        if (const auto path = m_workspace.OpenFileDialog({ { L"Forms and their scripts (*.lua)", L"*.lua" }, { L"All files", L"*.*" } }, folder))
            OpenFile(*path);
    }

    bool FormEditorWindow::Save(bool as)
    {
        if (m_oneFile)
        {
            std::filesystem::path path = m_path;
            if (as || path.empty())
            {
                std::error_code ignored;
                std::filesystem::create_directories(Paths::AutorunFolder(), ignored);
                const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
                const auto chosen = m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } }, WideToUtf8((folder / Utf8ToWide(FormStem(ScriptFileName()) + ".lua")).wstring()), L"lua");
                if (!chosen)
                    return false;
                path = *chosen;
                // a ".form.lua" name would make it open as an old form file
                path.replace_filename(Utf8ToWide(FormStem(path) + ".lua"));
            }
            if (path != m_path)
            {
                m_path = path;
                m_scriptPath = path;
                m_fileDisk.clear();
            }
            const bool wasDirty = m_dirty;
            m_dirty = true; // a form change alone must still be written
            if (!SaveScript())
            {
                m_dirty = wasDirty;
                return false;
            }
            ++m_revision;
            m_engine.Notify(LogLevel::Success, "Saved " + WideToUtf8(path.filename().wstring()));
            return true;
        }
        std::filesystem::path path = m_path;
        if (as || path.empty())
        {
            const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
            const auto chosen = m_workspace.SaveFileDialog({ { L"Manifold forms (*.form.lua)", L"*.form.lua" } }, WideToUtf8((folder / Utf8ToWide(FormStem(ScriptFileName()) + ".form.lua")).wstring()), L"lua");
            if (!chosen)
                return false;
            path = *chosen;
            const std::string name = WideToUtf8(path.filename().wstring());
            if (name.size() < 9 || !EqualsNoCase(std::string_view(name).substr(name.size() - 9), ".form.lua"))
                path.replace_filename(Utf8ToWide(FormStem(path) + ".form.lua"));
        }
        if (!WriteFile(path, Lua::FormSave(m_form)))
        {
            m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(path.wstring()));
            return false;
        }
        m_path = path;
        m_dirty = false;
        ++m_revision;
        const bool script = SaveScript();
        m_engine.Notify(LogLevel::Success, "Saved " + WideToUtf8(path.filename().wstring()) + (script && !m_scriptPath.empty() ? " and " + WideToUtf8(m_scriptPath.filename().wstring()) : std::string()));
        return true;
    }

    void FormEditorWindow::ExportStandalone()
    {
        std::error_code ignored;
        std::filesystem::create_directories(Paths::AutorunFolder(), ignored);
        const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
        const auto chosen = m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } },
            WideToUtf8((folder / Utf8ToWide(FormStem(ScriptFileName()) + " Standalone.lua")).wstring()), L"lua");
        if (!chosen)
            return;
        std::error_code same;
        if (!m_path.empty() && (std::filesystem::path(*chosen).lexically_normal() == m_path.lexically_normal() || std::filesystem::equivalent(*chosen, m_path, same)))
        {
            m_engine.Notify(LogLevel::Warning, "That is the form's own script; a standalone script would replace it. Export it to another file.");
            return;
        }
        if (!WriteFile(*chosen, Lua::FormScript(m_form, Lua::FormScriptStyle::Standalone)))
        {
            m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(chosen->wstring()));
            return;
        }
        m_engine.Notify(LogLevel::Success, "Exported " + WideToUtf8(chosen->filename().wstring()) + ": it builds the window with plain manifold.ui calls and no longer opens in the Form Editor.");
    }

    // ∑ An older form (.form.lua + a script that loads it) becomes one file:
    // the script gets the form's table in place of form.load("..."); the
    // .form.lua file stays on disk as it is, nothing is deleted.
    void FormEditorWindow::ConvertToOneFile()
    {
        if (m_oneFile)
            return;
        std::filesystem::path target = LinkedScript();
        std::error_code missing;
        std::string view;
        if (!target.empty() && std::filesystem::exists(target, missing))
        {
            EnsureScript();
            if (m_form.ScriptLoaded)
                view = Lua::FormConvertScript(m_form.ScriptText);
            if (view.empty())
            {
                m_engine.Notify(LogLevel::Warning, WideToUtf8(target.filename().wstring()) + " does not load the form with manifold.ui.form.load, so it was left alone; choose a new script for the form.");
                target.clear();
            }
        }
        if (target.empty())
        {
            const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
            const auto chosen = m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } }, WideToUtf8((folder / Utf8ToWide(FormStem(ScriptFileName()) + ".lua")).wstring()), L"lua");
            if (!chosen)
                return;
            target = *chosen;
            target.replace_filename(Utf8ToWide(FormStem(target) + ".lua"));
            view = Lua::FormScript(m_form, Lua::FormScriptStyle::OneFile);
        }
        // pictures: form.load took them relative to the form file, build takes them relative to the script
        bool pictures = false;
        if (!m_path.empty() && m_path.parent_path().lexically_normal() != target.parent_path().lexically_normal())
        {
            std::vector<int> stack = Lua::FormWindows(m_form);
            while (!stack.empty())
            {
                const UiNode* node = m_form.Model.Find(stack.back());
                stack.pop_back();
                if (!node)
                    continue;
                pictures = pictures || (!node->Source.empty() && std::filesystem::path(Utf8ToWide(node->Source)).is_relative());
                stack.insert(stack.end(), node->Children.begin(), node->Children.end());
            }
        }
        const std::filesystem::path old = m_path;
        Lua::FormDocument form = Lua::CopyForm(m_form);
        form.ScriptText = view;
        Reset(std::move(form), target, true);
        if (!Save(false))
            return;
        std::string note = "The form now lives in " + WideToUtf8(target.filename().wstring()) + ".";
        if (!old.empty())
            note += " " + WideToUtf8(old.filename().wstring()) + " was left as it is; the script no longer reads it.";
        if (pictures)
            note += " Its pictures are now looked up next to the script; move them there or give their full paths.";
        m_engine.Notify(LogLevel::Success, note);
    }

    void FormEditorWindow::Export()
    {
        if (m_oneFile)
        {
            ExportStandalone();
            return;
        }
        const bool withFile = m_style == 1;
        if (withFile && (m_path.empty() || Dirty()) && !Save(false))
            return;
        if (!withFile && ScriptDirty() && !SaveScript())
            return;
        std::error_code ignored;
        if (m_path.empty())
            std::filesystem::create_directories(Paths::AutorunFolder(), ignored);
        const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
        const std::filesystem::path linked = LinkedScript();
        const auto chosen = m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } },
            WideToUtf8((linked.empty() ? folder / Utf8ToWide(ScriptFileName()) : linked).wstring()), L"lua");
        if (!chosen)
            return;
        std::error_code missing;
        if (!withFile && m_form.ScriptLoaded && !m_form.ScriptText.empty() && std::filesystem::exists(*chosen, missing) &&
            (LinkedScript() == std::filesystem::path(*chosen).lexically_normal() || std::filesystem::equivalent(LinkedScript(), *chosen, missing)))
        {
            m_engine.Notify(LogLevel::Warning, WideToUtf8(chosen->filename().wstring()) + " holds this form's handler code; a standalone script would replace it. Export it to another file, or use Script + Form File.");
            return;
        }
        m_form.ScriptLoaded = false;
        if (LinkedScript() != std::filesystem::path(*chosen).lexically_normal())
        {
            SetLink(*chosen);
            if (withFile && !Save(false))
                return;
        }
        std::string existing;
        if (withFile && std::filesystem::exists(*chosen) && ReadFile(*chosen, existing))
        {
            const Lua::FormMerge merge = Lua::FormMergeScript(existing, m_form);
            if (merge.Anchored)
            {
                if (merge.Added.empty())
                {
                    m_engine.Notify(LogLevel::Info, WideToUtf8(chosen->filename().wstring()) + " already has every handler; it was left as it is.");
                    return;
                }
                if (!WriteFile(*chosen, merge.Text))
                {
                    m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(chosen->wstring()));
                    return;
                }
                std::string names;
                for (const std::string& name : merge.Added)
                    names += (names.empty() ? "" : ", ") + name;
                m_engine.Notify(LogLevel::Success, "Added " + names + " to " + WideToUtf8(chosen->filename().wstring()) + "; the rest of the script was kept.");
                return;
            }
        }
        std::string code;
        if (withFile)
        {
            std::filesystem::path relative = std::filesystem::relative(m_path, chosen->parent_path(), ignored);
            if (ignored || relative.empty())
                relative = m_path;
            std::string file = WideToUtf8(relative.generic_wstring());
            code = Lua::FormScript(m_form, Lua::FormScriptStyle::FormFile, file);
        }
        else
            code = Lua::FormScript(m_form, Lua::FormScriptStyle::Standalone);
        if (!WriteFile(*chosen, code))
        {
            m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(chosen->wstring()));
            return;
        }
        m_engine.Notify(LogLevel::Success, "Exported " + WideToUtf8(chosen->filename().wstring()));
    }

    void FormEditorWindow::RunCommand(Command command)
    {
        switch (command)
        {
        case Command::FormNew: AskThen(Pending::New); break;
        case Command::FormOpen: AskThen(Pending::Open); break;
        case Command::FormSave: Save(false); break;
        case Command::FormSaveAs: Save(true); break;
        case Command::FormExport: Export(); break;
        case Command::FormUndo: Undo(); break;
        case Command::FormRedo: Redo(); break;
        case Command::FormDuplicate: Duplicate(); break;
        case Command::FormDelete: Delete(); break;
        case Command::FormMoveUp: Move(-1); break;
        case Command::FormMoveDown: Move(1); break;
        case Command::FormTry: ToggleTry(); break;
        case Command::FormRun: RunScript(); break;
        case Command::FormCopy: Copy(); break;
        case Command::FormCut: Cut(); break;
        case Command::FormPaste: Paste(); break;
        case Command::FormRename: m_focusName = true; break;
        case Command::FormSelectParent: SelectParent(); break;
        case Command::FormFind: m_focusOutlineFilter = true; break;
        default: break;
        }
    }

    void FormEditorWindow::Shortcuts()
    {
        const bool typing = ImGui::GetIO().WantTextInput;
        for (const Command command : { Command::FormNew, Command::FormOpen, Command::FormSave, Command::FormSaveAs, Command::FormExport, Command::FormTry, Command::FormRun })
        {
            if (Pressed(command))
                RunCommand(command);
        }
        for (const Command command : { Command::FormUndo, Command::FormRedo, Command::FormDuplicate, Command::FormDelete, Command::FormMoveUp, Command::FormMoveDown, Command::FormCopy, Command::FormCut, Command::FormPaste,
                 Command::FormRename, Command::FormSelectParent, Command::FormFind })
        {
            if (Pressed(command) && !typing && !m_trying)
                RunCommand(command);
        }
    }

    void FormEditorWindow::Menu()
    {
        if (!ImGui::BeginMenuBar())
            return;
        if (ImGui::BeginMenu("File"))
        {
            if (CommandItem(ICON_FA_FILE "  New Form", Command::FormNew))
                RunCommand(Command::FormNew);
            TemplateMenu();
            if (CommandItem(ICON_FA_FOLDER_OPEN "  Open Form...", Command::FormOpen))
                RunCommand(Command::FormOpen);
            ImGui::Separator();
            if (CommandItem(ICON_FA_SAVE "  Save Form", Command::FormSave))
                RunCommand(Command::FormSave);
            if (CommandItem(ICON_FA_SAVE "  Save Form As...", Command::FormSaveAs))
                RunCommand(Command::FormSaveAs);
            if (CommandItem(m_oneFile ? ICON_FA_FILE_EXPORT "  Export Standalone Script..." : ICON_FA_FILE_EXPORT "  Export Script...", Command::FormExport))
                RunCommand(Command::FormExport);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_SHAPES "  Save as Template..."))
                SaveAsTemplate();
            ImGui::Separator();
            if (m_oneFile)
            {
                if (ImGui::MenuItem(ICON_FA_FILE_CODE "  Open in Script Window", nullptr, false, !m_path.empty()))
                    OpenLinked();
                ItemTooltip("Opens the form's script in a script window, with the form's table written out.");
                ImGui::Separator();
            }
            else
            {
                if (ImGui::MenuItem(ICON_FA_COMPRESS_ALT "  Convert to One File..."))
                    ConvertToOneFile();
                ItemTooltip("Moves the form into its script, so form and handlers live in one .lua file. The .form.lua file stays as it is.");
                ImGui::Separator();
            }
            if (!m_oneFile)
            {
                const std::filesystem::path script = LinkedScript();
                if (ImGui::MenuItem(ICON_FA_LINK "  Link Script..."))
                    LinkScript();
                ItemTooltip("Links the form to the Lua script that holds its handlers, so the handler buttons open their functions there.");
                if (ImGui::MenuItem(ICON_FA_FILE_CODE "  Open Linked Script", nullptr, false, !script.empty()))
                    OpenLinked();
                if (ImGui::MenuItem(ICON_FA_UNLINK "  Unlink Script", nullptr, false, !script.empty()))
                {
                    Remember();
                    m_form.Script.clear();
                    Changed();
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Import Cheat Engine Form..."))
                ImportFile();
            ItemTooltip("Reads a form from a .FRM file or a cheat table (.CT) and turns it into a manifold.ui form.");
            const std::vector<std::string>& extra = m_engine.Table().ExtraXml;
            const bool tableForms = std::any_of(extra.begin(), extra.end(), [](const std::string& xml) { return xml.rfind("<Forms", 0) == 0; });
            if (ImGui::MenuItem(ICON_FA_TABLE "  Import Form from the Open Table...", nullptr, false, tableForms))
                ImportTable();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            if (CommandItem(ICON_FA_UNDO "  Undo", Command::FormUndo, false, !m_undo.empty()))
                Undo();
            if (CommandItem(ICON_FA_REDO "  Redo", Command::FormRedo, false, !m_redo.empty()))
                Redo();
            ImGui::Separator();
            const UiNode* chosen = m_form.Model.Find(m_selected);
            const bool widget = chosen && chosen->Kind != UiKind::Window;
            const bool deletable = widget || Lua::FormIsModal(m_form, m_selected);
            if (CommandItem(ICON_FA_CUT "  Cut", Command::FormCut, false, widget))
                Cut();
            if (CommandItem(ICON_FA_COPY "  Copy", Command::FormCopy))
                Copy();
            if (CommandItem(ICON_FA_PASTE "  Paste", Command::FormPaste))
                Paste();
            ImGui::Separator();
            if (CommandItem(ICON_FA_CLONE "  Duplicate", Command::FormDuplicate, false, widget))
                Duplicate();
            if (CommandItem(ICON_FA_ARROW_UP "  Move Up", Command::FormMoveUp, false, widget))
                Move(-1);
            if (CommandItem(ICON_FA_ARROW_DOWN "  Move Down", Command::FormMoveDown, false, widget))
                Move(1);
            ImGui::Separator();
            if (DangerCommandItem(ICON_FA_TRASH "  Delete", Command::FormDelete, deletable))
                Delete();
            ImGui::EndMenu();
        }
        SnippetMenu();
        if (ImGui::BeginMenu("View"))
        {
            if (CommandItem(ICON_FA_MOUSE_POINTER "  Try the Form", Command::FormTry, m_trying))
                ToggleTry();
            if (ImGui::MenuItem(ICON_FA_CODE "  Show Code", nullptr, m_showCode))
                m_showCode = !m_showCode;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run"))
        {
            if (CommandItem(ICON_FA_PLAY "  Run Script", Command::FormRun))
                RunScript();
            if (ImGui::MenuItem(ICON_FA_CODE "  Open as Lua Script"))
                m_workspace.OpenScript(Lua::FormScript(m_form, Lua::FormScriptStyle::Standalone), kNoRecord, true);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void FormEditorWindow::PaletteTile(UiKind kind, float width)
    {
        const std::string title(Lua::FormKindTitle(kind));
        ImGui::PushID(static_cast<int>(kind));
        const ImVec2 size(width, ImGui::GetFrameHeight() + Px(4.0f));
        const bool clicked = ImGui::InvisibleButton("##tile", size);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec4 family = FamilyColor(FamilyOf(kind));
        draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered || held ? ManifoldTheme::WithAlpha(family, held ? 0.28f : 0.16f) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)), Px(4.0f));
        if (hovered)
            draw->AddRect(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(family, 0.55f)), Px(4.0f));
        draw->AddRectFilled(ImVec2(min.x, min.y + Px(4.0f)), ImVec2(min.x + Px(3.0f), max.y - Px(4.0f)), ImGui::GetColorU32(family), Px(1.5f));
        const float y = min.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
        draw->AddText(ImVec2(min.x + Px(10.0f), y), ImGui::GetColorU32(family), KindIcon(kind));
        {
            const float limit = std::max(min.x + Px(30.0f), max.x - Px(6.0f));
            ImGui::RenderTextEllipsis(draw, ImVec2(min.x + Px(30.0f), y), ImVec2(limit, y + ImGui::GetTextLineHeight()), limit, title.c_str(), nullptr, nullptr);
        }
        if (clicked)
            Add(kind);
        if (ImGui::BeginDragDropSource())
        {
            const int value = static_cast<int>(kind);
            ImGui::SetDragDropPayload(kKindPayload, &value, sizeof(value));
            ImGui::TextColored(family, "%s", KindIcon(kind));
            ImGui::SameLine();
            ImGui::TextUnformatted(title.c_str());
            ImGui::EndDragDropSource();
        }
        ItemTooltip((title + ": " + std::string(Lua::FormKindHelp(kind))).append("\nClick adds it to the selected container, or drag it into the outline or the preview.").c_str());
        ImGui::PopID();
    }

    void FormEditorWindow::Palette()
    {
        SectionLabel("Add");
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputTextWithHint("##palettefilter", ICON_FA_SEARCH "  Search widgets", m_paletteFilter);
        ImGui::BeginChild("##palettetiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float width = std::floor((ImGui::GetContentRegionAvail().x - spacing) * 0.5f);
        bool any = false;
        for (const Family family : { Family::Layout, Family::Input, Family::Text, Family::Data })
        {
            std::vector<UiKind> kinds;
            for (const UiKind kind : Lua::FormKinds())
            {
                if (FamilyOf(kind) == family && (Matches(Lua::FormKindTitle(kind), m_paletteFilter) || Matches(Lua::UiKindName(kind), m_paletteFilter)))
                    kinds.push_back(kind);
            }
            if (kinds.empty())
                continue;
            any = true;
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::WithAlpha(FamilyColor(family), 0.9f));
            ImGui::TextUnformatted(FamilyTitle(family));
            ImGui::PopStyleColor();
            for (std::size_t i = 0; i < kinds.size(); ++i)
            {
                if (i % 2 == 1)
                    ImGui::SameLine(0.0f, spacing);
                PaletteTile(kinds[i], width);
            }
            ImGui::Spacing();
        }
        if (!any)
            Muted("No widget matches.");
        ImGui::EndChild();
    }

    void FormEditorWindow::Outline()
    {
        SectionLabel("Outline");
        // ∑ Find: rows whose name, kind or caption hold the text, with the
        // containers above them (dimmed); Enter selects the first one, Escape
        // empties the field
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_focusOutlineFilter)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusOutlineFilter = false;
        }
        InputTextWithHint("##outlinefilter", ICON_FA_SEARCH "  Find a widget (Ctrl+F)", m_outlineFilter);
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape))
            m_outlineFilter.clear();
        const bool pickFirst = ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        m_outlineShown.clear();
        m_outlineMatches.clear();
        if (!m_outlineFilter.empty())
        {
            std::function<bool(int)> mark = [&](int id) {
                const UiNode* node = m_form.Model.Find(id);
                if (!node)
                    return false;
                const auto meta = m_form.Meta.find(id);
                const std::string text = (meta != m_form.Meta.end() ? meta->second.Name + " " : std::string()) + Caption(m_form, *node);
                bool any = ContainsNoCase(text, m_outlineFilter);
                if (any)
                    m_outlineMatches.insert(id);
                for (const int child : node->Children)
                    any = mark(child) || any;
                if (any)
                    m_outlineShown.insert(id);
                return any;
            };
            for (const int window : Lua::FormWindows(m_form))
                mark(window);
            if (pickFirst)
            {
                for (const int id : m_outlineOrder)
                {
                    if (m_outlineMatches.count(id))
                    {
                        Select(id);
                        break;
                    }
                }
            }
        }
        m_outlineOrder = std::move(m_outlineDrawn);
        m_outlineDrawn.clear();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Surface);
        // the rows take the arrow keys themselves (no nav highlight jumping between them)
        ImGui::BeginChild("##outline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
        ImGui::PopStyleColor();
        if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl && !m_outlineOrder.empty())
        {
            // ∑ Up and Down walk the rows, Left folds or goes up, Right
            // unfolds or goes to the first widget inside, Enter shows a handler
            const auto at = std::find(m_outlineOrder.begin(), m_outlineOrder.end(), m_selected);
            const std::ptrdiff_t index = at == m_outlineOrder.end() ? -1 : at - m_outlineOrder.begin();
            const UiNode* selected = m_form.Model.Find(m_selected);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                Select(m_outlineOrder[static_cast<std::size_t>(std::min<std::ptrdiff_t>(index + 1, static_cast<std::ptrdiff_t>(m_outlineOrder.size()) - 1))]);
            else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                Select(m_outlineOrder[static_cast<std::size_t>(std::max<std::ptrdiff_t>(index - 1, 0))]);
            else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && selected)
            {
                if (!selected->Children.empty() && !m_collapsed.count(m_selected) && m_outlineFilter.empty())
                    m_collapsed.insert(m_selected);
                else
                    SelectParent();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && selected && !selected->Children.empty())
            {
                if (m_collapsed.count(m_selected))
                    m_collapsed.erase(m_selected);
                else
                    Select(selected->Children.front());
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && selected)
                OpenFirstHandler(m_selected);
        }
        // ∑ Own rows instead of ImGui tree nodes: full-width hover and
        // selection, indent guides per level (the selected container's guide
        // lit), a chevron that only folds, and the markers at the right edge.
        // Deep trees scroll sideways; a row is as wide as its text.
        m_menuSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        m_outlineX = ImGui::GetCursorScreenPos().x;
        m_outlineWidth = ImGui::GetContentRegionAvail().x;
        m_outlineRows = 0;
        std::vector<int> path;
        OutlineNode(m_form.Window, 0, path);
        for (const int modal : m_form.Modals)
        {
            ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
            OutlineNode(modal, 0, path);
        }
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(8.0f));
        ImGui::PushStyleColor(ImGuiCol_TextLink, ManifoldTheme::Muted);
        if (ImGui::TextLink(ICON_FA_PLUS "  Modal Form"))
            AddModal();
        ImGui::PopStyleColor();
        ItemTooltip("Adds a modal form to this file: a dialog that opens in front of the program. A button opens it with Opens, the script with modals.<name>.show().");
        if (m_form.Model.Find(m_form.Window) && m_form.Model.Find(m_form.Window)->Children.empty())
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(10.0f));
            ImGui::PushTextWrapPos(0.0f);
            Muted("Drag widgets from Add onto the window, or onto the preview.");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        m_scrollOutline = false;
    }

    void FormEditorWindow::OutlineNode(int id, int depth, std::vector<int>& path)
    {
        UiNode* node = m_form.Model.Find(id);
        if (!node)
            return;
        const bool filtering = !m_outlineFilter.empty();
        if (filtering && !m_outlineShown.count(id))
            return;
        m_outlineDrawn.push_back(id);
        ImGui::PushID(id);
        const bool container = Lua::UiIsContainer(node->Kind);
        const bool hasChildren = !node->Children.empty();
        if (m_scrollOutline && hasChildren && m_collapsed.count(id))
        {
            // the selection moved inside a folded container: open the way to it
            for (const UiNode* walk = m_form.Model.Find(m_selected); walk; walk = m_form.Model.Find(walk->Parent))
            {
                if (walk->Parent == id)
                {
                    m_collapsed.erase(id);
                    break;
                }
            }
        }
        const bool open = hasChildren && (filtering || !m_collapsed.count(id));
        const float indent = Px(15.0f);
        const float rowHeight = ImGui::GetFrameHeight() + Px(1.0f);
        const float pad = Px(6.0f);
        const float x0 = m_outlineX;
        const float y0 = ImGui::GetCursorScreenPos().y;
        const float chevronX = x0 + pad + indent * static_cast<float>(depth);
        const float iconX = chevronX + indent;
        const float alpha = (node->Visible ? 1.0f : 0.45f) * (filtering && !m_outlineMatches.count(id) ? 0.55f : 1.0f);

        const auto meta = m_form.Meta.find(id);
        const std::string name = meta == m_form.Meta.end() ? std::string() : meta->second.Name;
        const bool modalRoot = Lua::FormIsModal(m_form, id);
        std::string detail = Caption(m_form, *node);
        {
            const std::string title(Lua::FormKindTitle(node->Kind));
            detail = detail.size() > title.size() ? Trim(std::string_view(detail).substr(title.size())) : std::string();
        }
        const std::string kind = modalRoot ? std::string("Modal") : std::string(Lua::FormKindTitle(node->Kind));
        if (!name.empty() && detail.rfind(name, 0) == 0)
            detail = Trim(std::string_view(detail).substr(name.size()));
        const std::string first = name.empty() ? kind : name;
        const std::string second = name.empty() ? detail : (detail.empty() ? kind : kind + "  " + detail);
        const float textX = iconX + Px(21.0f);
        const float firstWidth = ImGui::CalcTextSize(first.c_str()).x;
        const float textWidth = firstWidth + (second.empty() ? 0.0f : Px(8.0f) + ImGui::CalcTextSize(second.c_str()).x);
        // room only for the markers this row can show: the eye (hover or
        // hidden), a problem, handlers; long rows push them past the text
        const float slot = rowHeight * 0.85f;
        const bool hasHandlers = meta != m_form.Meta.end() && !meta->second.Handlers.empty();
        const float markers = (node->Kind != UiKind::Window ? rowHeight : 0.0f) + (m_problemNodes.count(id) ? slot : 0.0f) + (hasHandlers ? slot : 0.0f);
        const float visibleRight = ImGui::GetCurrentWindow()->InnerClipRect.Max.x;
        const float right = std::max(visibleRight - pad, textX + firstWidth + Px(10.0f) + markers);
        const float rowWidth = std::max(m_outlineWidth, right + pad - x0);

        ImGui::SetCursorScreenPos(ImVec2(x0, y0));
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##row", ImVec2(rowWidth, rowHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 eyeMin(right - rowHeight, rowMin.y);
        const ImVec2 eyeMax(right, rowMax.y);
        const bool overEye = node->Kind != UiKind::Window && rowHovered && mouse.x >= eyeMin.x && mouse.x < eyeMax.x;
        const bool overChevron = hasChildren && rowHovered && mouse.x >= chevronX - Px(2.0f) && mouse.x < chevronX + indent;
        if (rowHovered)
            m_hovered = id;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (overChevron)
            {
                if (open)
                    m_collapsed.insert(id);
                else
                    m_collapsed.erase(id);
            }
            else if (!overEye)
                Select(id);
        }
        if (rowHovered && hasChildren && !overChevron && !overEye && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (open)
                m_collapsed.insert(id);
            else
                m_collapsed.erase(id);
        }
        else if (rowHovered && !hasChildren && !overEye && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            OpenFirstHandler(id);
        if (id == m_selected && m_scrollOutline && !ImGui::IsRectVisible(rowMin, rowMax))
            ImGui::SetScrollHereY(0.4f);
        // ∑ the rows run with no item spacing, and a popup takes the spacing
        // that holds when it begins: without the normal one, a menu entry's
        // keys sit right against its text ("Delete Modal FormDel")
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, m_menuSpacing);
        if (ImGui::BeginPopupContextItem("##menu"))
        {
            Select(id);
            NodeMenu(id);
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        const std::string text = std::string(KindIcon(node->Kind)) + "  " + Caption(m_form, *node);
        if (node->Kind != UiKind::Window && ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(kNodePayload, &id, sizeof(id));
            ImGui::TextUnformatted(text.c_str());
            ImGui::EndDragDropSource();
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (ImGui::BeginDragDropTarget())
        {
            const ImRect rect(ImVec2(textX - Px(24.0f), rowMin.y), ImVec2(right + pad - Px(2.0f), rowMax.y));
            const float y = mouse.y;
            const float band = rect.GetHeight() * 0.3f;
            Drop where = Drop::Inside;
            if (node->Kind != UiKind::Window)
            {
                if (y < rect.Min.y + band)
                    where = Drop::Before;
                else if (y > rect.Max.y - band || !container)
                    where = Drop::After;
            }
            const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
            if (where == Drop::Inside)
                draw->AddRect(rect.Min, rect.Max, accent, Px(4.0f), 0, Px(2.0f));
            else
            {
                const float line = where == Drop::Before ? rect.Min.y : rect.Max.y;
                draw->AddLine(ImVec2(rect.Min.x, line), ImVec2(rect.Max.x, line), accent, Px(2.0f));
                draw->AddCircleFilled(ImVec2(rect.Min.x, line), Px(3.0f), accent);
            }
            int parent = 0;
            int index = -1;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kKindPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                const UiKind kindDropped = static_cast<UiKind>(*static_cast<const int*>(payload->Data));
                if (DropAt(id, where, parent, index))
                    AddInto(kindDropped, parent, index);
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kRecordPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect); payload && payload->DataSize == sizeof(int))
            {
                if (DropAt(id, where, parent, index))
                    AddRecordAt(*static_cast<const int*>(payload->Data), parent, index);
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                const int moving = *static_cast<const int*>(payload->Data);
                if (moving != id && DropAt(id, where, parent, index))
                {
                    FormDocument before = m_form;
                    if (Lua::FormMove(m_form, moving, parent, index))
                    {
                        m_undo.push_back(std::move(before));
                        m_redo.clear();
                        m_lastSource = 0;
                        Changed();
                        Select(moving);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        // background: selection with an accent bar, hover a soft fill
        const bool selected = id == m_selected;
        const ImVec2 fillMin(rowMin.x + Px(3.0f), rowMin.y + Px(1.0f));
        const ImVec2 fillMax(right + pad - Px(3.0f), rowMax.y - Px(1.0f));
        if (selected)
        {
            draw->AddRectFilled(fillMin, fillMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.16f)), Px(4.0f));
            draw->AddRectFilled(ImVec2(fillMin.x, fillMin.y + Px(3.0f)), ImVec2(fillMin.x + Px(3.0f), fillMax.y - Px(3.0f)), ImGui::GetColorU32(ManifoldTheme::Accent), Px(1.5f));
        }
        else if (rowHovered || id == m_hovered)
            draw->AddRectFilled(fillMin, fillMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f)), Px(4.0f));

        // indent guides: one per level, the one of the selected container lit
        for (std::size_t level = 0; level < path.size(); ++level)
        {
            const float gx = std::floor(x0 + pad + indent * static_cast<float>(level) + indent * 0.5f) + 0.5f;
            const bool lit = path[level] == m_selected;
            draw->AddLine(ImVec2(gx, rowMin.y), ImVec2(gx, rowMax.y),
                ImGui::GetColorU32(lit ? ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.7f) : ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.22f)), 1.0f);
        }

        const float y = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
        if (hasChildren)
        {
            const char* chevron = open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
            const float scale = 0.72f;
            const float size = ImGui::GetFontSize() * scale;
            const ImVec2 glyph = ImGui::CalcTextSize(chevron);
            draw->AddText(ImGui::GetFont(), size, ImVec2(chevronX + (indent - glyph.x * scale) * 0.5f, rowMin.y + (rowHeight - size) * 0.5f),
                ImGui::GetColorU32(overChevron ? ManifoldTheme::Text : ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.9f)), chevron);
        }
        draw->AddText(ImVec2(iconX, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(modalRoot ? ManifoldTheme::CodeLabel : FamilyColor(FamilyOf(node->Kind)), alpha)),
            modalRoot ? ICON_FA_WINDOW_RESTORE : KindIcon(node->Kind));
        const float textRight = right - markers;
        // ∑ the name always fits (the row grows and the outline scrolls
        // sideways); the kind and caption after it end in "..." where the
        // markers begin instead of being cut through a letter
        draw->AddText(ImVec2(textX, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(selected ? ManifoldTheme::Text : ManifoldTheme::Label, alpha)), first.c_str());
        const float secondX = textX + firstWidth + Px(8.0f);
        if (!second.empty() && secondX + ImGui::CalcTextSize("...").x < textRight)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::WithAlpha(ManifoldTheme::Muted, alpha * 0.9f));
            ImGui::RenderTextEllipsis(draw, ImVec2(secondX, y), ImVec2(textRight, y + ImGui::GetTextLineHeight()), textRight, second.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        }
        if (rowHovered && !overEye && !overChevron && textX + textWidth > textRight && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            ImGui::SetTooltip("%s%s%s", first.c_str(), second.empty() ? "" : "  ", second.c_str());

        // markers, right to left: eye (on hover or when hidden), problem, handlers
        float marker = node->Kind != UiKind::Window ? eyeMin.x : right;
        if (node->Kind != UiKind::Window)
        {
            if (!node->Visible || rowHovered)
            {
                const char* glyph = node->Visible ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
                const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
                draw->AddText(ImVec2(eyeMin.x + (rowHeight - glyphSize.x) * 0.5f, y), ImGui::GetColorU32(overEye ? ManifoldTheme::Text : ManifoldTheme::Muted), glyph);
            }
            if (overEye)
            {
                ImGui::SetTooltip("%s", node->Visible ? "Hide it at the start (visible = false)" : "Show it at the start");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    Remember();
                    node->Visible = !node->Visible;
                    Changed();
                }
            }
        }
        if (m_problemNodes.count(id))
        {
            marker -= slot;
            draw->AddText(ImVec2(marker, y), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_EXCLAMATION_TRIANGLE);
        }
        if (hasHandlers)
        {
            marker -= slot;
            const bool lit = !m_hoveredChip.empty() && [&] {
                for (const auto& [key, handler] : meta->second.Handlers)
                {
                    if (handler == m_hoveredChip)
                        return true;
                }
                return false;
            }();
            draw->AddText(ImVec2(marker, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, lit ? 1.0f : 0.7f)), ICON_FA_BOLT);
        }
        ++m_outlineRows;
        ImGui::SetCursorScreenPos(ImVec2(x0, rowMax.y));
        if (open)
        {
            path.push_back(id);
            for (const int child : std::vector<int>(node->Children))
                OutlineNode(child, depth + 1, path);
            path.pop_back();
        }
        ImGui::PopID();
    }

    void FormEditorWindow::NodeMenu(int id)
    {
        const UiNode* node = m_form.Model.Find(id);
        if (!node)
            return;
        const bool container = Lua::UiIsContainer(node->Kind);
        if (container && ImGui::BeginMenu(ICON_FA_PLUS "  Add Inside"))
        {
            for (const UiKind kind : Lua::FormKinds())
            {
                if (!Lua::FormCanHold(node->Kind, kind))
                    continue;
                const std::string label = std::string(KindIcon(kind)) + "  " + std::string(Lua::FormKindTitle(kind));
                if (ImGui::MenuItem(label.c_str()))
                    AddInto(kind, id, -1);
            }
            ImGui::EndMenu();
        }
        if (node->Kind == UiKind::Window)
        {
            if (CommandItem(ICON_FA_PASTE "  Paste", Command::FormPaste))
                Paste();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Add Modal Form"))
                AddModal();
            if (Lua::FormIsModal(m_form, id) && DangerCommandItem(ICON_FA_TRASH "  Delete Modal Form", Command::FormDelete))
                Delete();
            return;
        }
        if (CommandItem(ICON_FA_LEVEL_UP_ALT "  Select Parent", Command::FormSelectParent))
            Select(node->Parent);
        if (!Lua::FormHandlers(node->Kind).empty() && ImGui::MenuItem(ICON_FA_BOLT "  Show Handler"))
            OpenFirstHandler(id);
        ImGui::Separator();
        if (node->Kind != UiKind::Tab && ImGui::BeginMenu(ICON_FA_LAYER_GROUP "  Wrap In"))
        {
            for (const UiKind kind : { UiKind::Row, UiKind::Column, UiKind::Grid, UiKind::Section })
            {
                const std::string label = std::string(KindIcon(kind)) + "  " + std::string(Lua::FormKindTitle(kind));
                if (ImGui::MenuItem(label.c_str()))
                    Wrap(id, kind);
            }
            ImGui::EndMenu();
        }
        if (container && node->Kind != UiKind::Tab && ImGui::MenuItem(ICON_FA_EXPAND_ARROWS_ALT "  Unwrap", nullptr, false, !node->Children.empty()))
            Unwrap(id);
        ItemTooltip("Moves the widgets inside out to where this container is and removes it.");
        if (const std::vector<UiKind> kinds = Lua::FormKindChoices(node->Kind); !kinds.empty() && ImGui::BeginMenu(ICON_FA_EXCHANGE_ALT "  Change Kind"))
        {
            for (const UiKind kind : kinds)
            {
                const std::string label = std::string(KindIcon(kind)) + "  " + std::string(Lua::FormKindTitle(kind));
                if (ImGui::MenuItem(label.c_str()))
                    ChangeKind(id, kind);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (CommandItem(ICON_FA_CUT "  Cut", Command::FormCut))
            Cut();
        if (CommandItem(ICON_FA_COPY "  Copy", Command::FormCopy))
            Copy();
        if (CommandItem(ICON_FA_PASTE "  Paste", Command::FormPaste))
            Paste();
        if (CommandItem(ICON_FA_CLONE "  Duplicate", Command::FormDuplicate))
            Duplicate();
        if (node->Kind == UiKind::Button && ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Open a New Modal Form"))
            AddModal(id);
        if (CommandItem(ICON_FA_ARROW_UP "  Move Up", Command::FormMoveUp))
            Move(-1);
        if (CommandItem(ICON_FA_ARROW_DOWN "  Move Down", Command::FormMoveDown))
            Move(1);
        ImGui::Separator();
        if (DangerCommandItem(ICON_FA_TRASH "  Delete", Command::FormDelete))
            Delete();
    }

    void FormEditorWindow::Toolbar()
    {
        // ∑ what does not fit beside the last item goes to the next line
        // instead of being cut off at the pane's edge
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const auto next = [&](float width) {
            if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right)
                ImGui::SameLine();
        };
        const auto buttonWidth = [](const char* text) { return ImGui::CalcTextSize(text, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f; };
        int mode = m_trying ? 1 : 0;
        if (Segmented("##mode", &mode, { ICON_FA_MOUSE_POINTER "  Design", ICON_FA_HAND_POINTER "  Try" }, Px(190.0f)) && (mode == 1) != m_trying)
            ToggleTry();
        next(buttonWidth(ICON_FA_PLAY "  Run Script"));
        if (ImGui::Button(ICON_FA_PLAY "  Run Script"))
            RunScript();
        ItemTooltip("Runs the script in Manifold's Lua engine, so the real window opens. Running it again replaces that window.");
        next(ImGui::GetFrameHeight());
        if (IconButton(ICON_FA_CROSSHAIRS, "Bring the form back to the top left of the work surface"))
            m_pan = ImVec2(Px(-m_form.CanvasX + 40.0f), Px(-m_form.CanvasY + 30.0f));
        if (const std::filesystem::path script = LinkedScript(); !script.empty() && !m_oneFile)
        {
            std::error_code ignored;
            const bool exists = std::filesystem::exists(script, ignored);
            const std::string label = std::string(ICON_FA_FILE_CODE "  ") + WideToUtf8(script.filename().wstring()) + (exists ? "" : "  (missing)") + "##linked";
            next(ImGui::CalcTextSize(label.c_str(), nullptr, true).x);
            ImGui::AlignTextToFramePadding();
            if (ImGui::TextLink(label.c_str()) && exists)
                OpenLinked();
            ItemTooltip(("Linked script: " + WideToUtf8(script.wstring()) + "\nClick to open it.").c_str());
        }
        if (!m_path.empty())
        {
            if (const int live = m_engine.Lua().Ui().Watching(WideToUtf8(m_path.wstring())); live > 0)
            {
                const bool pending = m_dirty || (m_oneFile && ScriptDirty());
                const std::string text = std::string(ICON_FA_SYNC "  Live in ") + std::to_string(live) + (live == 1 ? " Window" : " Windows") + (pending ? " (save to update)" : "");
                next(ImGui::CalcTextSize(text.c_str()).x);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(pending ? ManifoldTheme::Warning : ManifoldTheme::Success, "%s", text.c_str());
                ItemTooltip(m_oneFile ? "This script is running and built its window from the form. Saving rebuilds that window right away; widgets that keep their name keep their value. Changed handler code takes effect when the script runs again."
                                      : "A running script loaded this file with manifold.ui.form.load. Saving it rebuilds that window right away, with the script's handlers; widgets that keep their name keep their value.");
            }
        }
        const std::vector<Lua::FormProblem>& problems = m_problems;
        if (!problems.empty())
        {
            const std::string label = std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + std::to_string(problems.size()) + (problems.size() == 1 ? " Problem" : " Problems");
            next(buttonWidth(label.c_str()));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            if (ImGui::Button(label.c_str()))
                m_problemsDialog.Open();
            ImGui::PopStyleColor();
        }
        if (m_trying)
        {
            next(ImGui::CalcTextSize("Clicks reach the widgets; handlers are not called.").x);
            ImGui::AlignTextToFramePadding();
            ImGui::PushTextWrapPos(0.0f);
            Muted("Clicks reach the widgets; handlers are not called.");
            ImGui::PopTextWrapPos();
        }
        Breadcrumb();
    }

    void FormEditorWindow::Breadcrumb()
    {
        std::vector<const UiNode*> path;
        for (const UiNode* node = m_form.Model.Find(m_selected); node; node = m_form.Model.Find(node->Parent))
            path.insert(path.begin(), node);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Px(4.0f), ImGui::GetStyle().ItemSpacing.y));
        std::vector<std::string> labels;
        std::vector<float> widths;
        const float spacing = Px(4.0f);
        const float separator = ImGui::CalcTextSize(ICON_FA_ANGLE_RIGHT).x + spacing * 2.0f;
        for (const UiNode* node : path)
        {
            const auto meta = m_form.Meta.find(node->Id);
            labels.push_back(std::string(KindIcon(node->Kind)) + " " + (meta != m_form.Meta.end() && !meta->second.Name.empty() ? meta->second.Name : std::string(Lua::FormKindTitle(node->Kind))));
            widths.push_back(ImGui::CalcTextSize(labels.back().c_str()).x);
        }
        // ∑ A path wider than the bar keeps its first and last steps and folds
        // the ones in between into a "..." menu, dropping from the root side.
        const float avail = ImGui::GetContentRegionAvail().x;
        const float ellipsis = ImGui::CalcTextSize("...").x + separator;
        float total = 0.0f;
        for (std::size_t i = 0; i < path.size(); ++i)
            total += widths[i] + (i ? separator : 0.0f);
        std::size_t hiddenEnd = 1;
        while (total > avail && hiddenEnd + 1 < path.size())
        {
            total -= widths[hiddenEnd] + separator;
            if (hiddenEnd == 1)
                total += ellipsis;
            ++hiddenEnd;
        }
        const bool folded = hiddenEnd > 1;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            if (folded && i >= 1 && i < hiddenEnd)
            {
                if (i == 1)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ManifoldTheme::Muted, ICON_FA_ANGLE_RIGHT);
                    ImGui::SameLine();
                    if (ImGui::TextLink("...##crumbs"))
                        ImGui::OpenPopup("##crumbmenu");
                    ItemTooltip("Show the folded steps of the path");
                    if (ImGui::BeginPopup("##crumbmenu"))
                    {
                        for (std::size_t j = 1; j < hiddenEnd; ++j)
                        {
                            ImGui::PushID(path[j]->Id);
                            ImGui::Indent(Px(10.0f) * static_cast<float>(j - 1));
                            if (ImGui::MenuItem(labels[j].c_str()))
                                Select(path[j]->Id);
                            ImGui::Unindent(Px(10.0f) * static_cast<float>(j - 1));
                            ImGui::PopID();
                        }
                        ImGui::EndPopup();
                    }
                }
                continue;
            }
            const UiNode& node = *path[i];
            if (i)
            {
                ImGui::SameLine();
                ImGui::TextColored(ManifoldTheme::Muted, ICON_FA_ANGLE_RIGHT);
                ImGui::SameLine();
            }
            ImGui::PushID(node.Id);
            if (i + 1 == path.size())
                ImGui::TextUnformatted(labels[i].c_str());
            else if (ImGui::TextLink(labels[i].c_str()))
                Select(node.Id);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }

    void FormEditorWindow::Preview()
    {
        Toolbar();
        FormDocument& form = m_trying ? m_try : m_form;
        if (!form.Model.Find(form.Window))
            return;
        if (m_trying)
            Lua::FormApplyRules(m_try); // Enabled When plays along while trying the form
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Page);
        ImGui::BeginChild("##previewarea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 areaMin = ImGui::GetWindowPos();
        const ImVec2 areaMax(areaMin.x + ImGui::GetWindowWidth(), areaMin.y + ImGui::GetWindowHeight());
        m_areaSize = ImVec2(areaMax.x - areaMin.x, areaMax.y - areaMin.y);
        {
            // ∑ The work surface: dragging empty space (or the middle button
            // anywhere, or the wheel) moves the view; the forms and the
            // function chips sit on it at their own places.
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetCursorScreenPos(areaMin);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##canvas", m_areaSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
            const bool dragging = ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f));
            const bool middle = !ImGui::IsItemActive() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f);
            if (dragging || middle)
            {
                m_pan.x += io.MouseDelta.x;
                m_pan.y += io.MouseDelta.y;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
            if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f && !io.KeyCtrl)
            {
                if (io.KeyShift)
                    m_pan.x += io.MouseWheel * Px(60.0f);
                else
                    m_pan.y += io.MouseWheel * Px(60.0f);
            }
            if (ImGui::IsWindowHovered() && io.MouseWheelH != 0.0f)
                m_pan.x += io.MouseWheelH * Px(60.0f);
        }
        const ImVec2 origin(areaMin.x + m_pan.x, areaMin.y + m_pan.y);
        {
            const float step = Px(18.0f);
            const float startX = areaMin.x + std::fmod(std::fmod(m_pan.x, step) + step, step);
            const float startY = areaMin.y + std::fmod(std::fmod(m_pan.y, step) + step, step);
            const ImU32 dot = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.28f));
            const float radius = std::max(1.0f, Px(1.0f));
            for (float y = startY; y < areaMax.y; y += step)
                for (float x = startX; x < areaMax.x; x += step)
                    draw->AddRectFilled(ImVec2(x - radius * 0.5f, y - radius * 0.5f), ImVec2(x + radius * 0.5f, y + radius * 0.5f), dot);
        }
        // ∑ Each window is a frame of its own layer (see PreviewFrame); the
        // one that holds the selection is drawn last, so it lies on top of
        // the others with everything it has. Links and chips lie below the
        // frames, on the work surface itself.
        m_frames.clear();
        m_frameMarks.clear();
        m_frameClips.clear();
        m_grips.clear();
        m_design.Rects.clear();
        m_previewMenu = ImGui::GetID("##previewmenu");
        SyncImages(form);
        std::vector<int> windows = Lua::FormWindows(form);
        const int front = Lua::FormRootWindow(form, m_trying && m_flashWindow && ImGui::GetTime() < m_flashUntil ? m_flashWindow : m_selected);
        if (const auto at = std::find(windows.begin(), windows.end(), front); at != windows.end())
            std::rotate(at, at + 1, windows.end());
        for (const int window : windows)
            PreviewFrame(form, window, origin, areaMin, areaMax);
        if (m_trying)
            TryEvents(areaMin, areaMax);
        else
        {
            JumpLinks(areaMin, areaMax);
            FunctionChips(origin, areaMin, areaMax);
        }
        if (ImGui::BeginPopup("##previewmenu"))
        {
            NodeMenu(m_selected);
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    ImDrawList* FormEditorWindow::Marks() const
    {
        return m_marks ? m_marks : ImGui::GetWindowDrawList();
    }

    ImVec2 FormEditorWindow::FramePlace(int window) const
    {
        if (window == m_form.Window || !Lua::FormIsModal(m_form, window))
            return ImVec2(m_form.CanvasX, m_form.CanvasY);
        const std::string name = Lua::FormModalName(m_form, window);
        if (const auto stored = m_form.ModalNodes.find(name); stored != m_form.ModalNodes.end())
            return ImVec2(stored->second.X, stored->second.Y);
        // ∑ a modal without a place stacks below the main form
        const auto height = [&](int id) {
            const UiNode* node = m_form.Model.Find(id);
            return node && node->Height > 0.0f ? node->Height : 200.0f;
        };
        float y = m_form.CanvasY + height(m_form.Window) + 70.0f;
        for (const int modal : m_form.Modals)
        {
            if (modal == window)
                break;
            const auto stored = m_form.ModalNodes.find(Lua::FormModalName(m_form, modal));
            if (stored == m_form.ModalNodes.end())
                y += height(modal) + 60.0f;
        }
        return ImVec2(m_form.CanvasX, y);
    }

    void FormEditorWindow::SetFramePlace(int window, ImVec2 place)
    {
        if (window == m_form.Window)
        {
            m_form.CanvasX = place.x;
            m_form.CanvasY = place.y;
        }
        else
        {
            const std::string name = Lua::FormModalName(m_form, window);
            if (name.empty())
                return;
            m_form.ModalNodes[name] = Lua::FormNode{ place.x, place.y, false };
        }
        if (m_trying)
        {
            m_try.CanvasX = m_form.CanvasX;
            m_try.CanvasY = m_form.CanvasY;
            m_try.ModalNodes = m_form.ModalNodes;
        }
    }

    void FormEditorWindow::PreviewFrame(FormDocument& form, int id, ImVec2 origin, ImVec2 areaMin, ImVec2 areaMax)
    {
        const UiNode* window = form.Model.Find(id);
        if (!window)
            return;
        const bool modal = Lua::FormIsModal(form, id);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto limit = [](float value, float low, float high) {
            if (high > 0.0f)
                value = std::min(value, high);
            return std::max(value, low);
        };
        const float pointsWidth = limit(window->Width > 0.0f ? window->Width : 420.0f, window->MinWidth > 0.0f ? window->MinWidth : 160.0f, window->MaxWidth);
        const float pointsHeight = limit(window->Height > 0.0f ? window->Height : 320.0f, window->MinHeight > 0.0f ? window->MinHeight : 100.0f, window->MaxHeight);
        const ImVec2 size(Px(pointsWidth), Px(pointsHeight));
        const ImVec2 place = FramePlace(id);
        const ImVec2 frameMin(origin.x + Px(place.x), origin.y + Px(place.y));
        const float titleHeight = ImGui::GetFrameHeight() + Px(4.0f);
        const float bodyHeight = std::max(Px(40.0f), size.y - titleHeight);
        const ImVec2 frameMax(frameMin.x + size.x, frameMin.y + titleHeight + bodyHeight);
        m_frames[id] = Frame{ frameMin, frameMax, titleHeight };
        ImGui::PushID(id);
        // ∑ The frame is a layer: an input-less child over the frame and its
        // shadow holds the chrome, the title bar and the body are children of
        // it, and its marks (outlines, tags, guides, grip) go into one more
        // input-less child that is begun first, so its draw list lives through
        // the frame, and ordered last, so it lies above the body. A frame begun
        // later lies above all of it; clicks in the margin reach the surface.
        ImGuiWindow* area = ImGui::GetCurrentWindow();
        const ImVec2 cursorMax = area->DC.CursorMaxPos;
        const float margin = Px(18.0f);
        const ImVec2 outerMin(frameMin.x - margin, frameMin.y - margin);
        const ImVec2 outerSize(frameMax.x - frameMin.x + margin * 2.0f, frameMax.y - frameMin.y + margin * 2.0f);
        const ImGuiWindowFlags layer = ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::SetCursorScreenPos(outerMin);
        ImGui::BeginChild("##frame", outerSize, ImGuiChildFlags_None, layer);
        ImGuiWindow* frameWindow = ImGui::GetCurrentWindow();
        ImGui::SetCursorScreenPos(outerMin);
        ImGui::BeginChild("##framemarks", outerSize, ImGuiChildFlags_None, layer);
        ImGuiWindow* marksWindow = ImGui::GetCurrentWindow();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        const ImVec2 clipMin(std::max(outerMin.x, area->InnerClipRect.Min.x), std::max(outerMin.y, area->InnerClipRect.Min.y));
        const ImVec2 clipMax(std::min(outerMin.x + outerSize.x, area->InnerClipRect.Max.x), std::min(outerMin.y + outerSize.y, area->InnerClipRect.Max.y));
        m_marks = marksWindow->DrawList;
        m_marks->PushClipRect(clipMin, ImVec2(std::max(clipMin.x, clipMax.x), std::max(clipMin.y, clipMax.y)), false);
        m_frameMarks[id] = m_marks;
        m_frameClips[id] = { clipMin, clipMax };
        m_surfaceMin = area->InnerClipRect.Min;
        m_surfaceMax = area->InnerClipRect.Max;
        draw = frameWindow->DrawList;
        for (int i = 1; i <= 6; ++i)
        {
            const float grow = Px(static_cast<float>(i) * 2.0f);
            draw->AddRectFilled(ImVec2(frameMin.x - grow, frameMin.y - grow + Px(3.0f)), ImVec2(frameMax.x + grow, frameMax.y + grow + Px(3.0f)),
                ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Shadow, 0.10f)), Px(8.0f) + grow);
        }
        draw->AddRectFilled(frameMin, ImVec2(frameMax.x, frameMin.y + titleHeight), ImGui::GetColorU32(ManifoldTheme::TitleBar), Px(6.0f), ImDrawFlags_RoundCornersTop);
        if (modal)
            draw->AddRectFilled(frameMin, ImVec2(frameMax.x, frameMin.y + Px(2.0f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::CodeLabel, 0.9f)), Px(6.0f), ImDrawFlags_RoundCornersTop);
        const std::string title = UiHost::IconText(window->Icon, window->Title.empty() ? std::string("Script") : window->Title);
        const float textY = frameMin.y + (titleHeight - ImGui::GetTextLineHeight()) * 0.5f;
        // a long title in a narrow form ends in "..." before the close cross
        const float titleLimit = std::max(frameMin.x + Px(10.0f), frameMax.x - Px(20.0f) - ImGui::CalcTextSize(ICON_FA_TIMES).x);
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Text);
        ImGui::RenderTextEllipsis(draw, ImVec2(frameMin.x + Px(10.0f), textY), ImVec2(titleLimit, textY + ImGui::GetTextLineHeight()), titleLimit, title.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        float titleRight = std::min(titleLimit, frameMin.x + Px(10.0f) + ImGui::CalcTextSize(title.c_str()).x);
        if (modal)
        {
            // a pill with the modal's name: what modals.<name> and opens use
            const std::string pill = std::string(ICON_FA_WINDOW_RESTORE " ") + Lua::FormModalName(form, id);
            const ImVec2 pillSize = ImGui::CalcTextSize(pill.c_str());
            const ImVec2 pillMin(titleRight + Px(10.0f), frameMin.y + (titleHeight - pillSize.y) * 0.5f - Px(1.0f));
            const ImVec2 pillMax(pillMin.x + pillSize.x + Px(12.0f), pillMin.y + pillSize.y + Px(2.0f));
            draw->AddRectFilled(pillMin, pillMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::CodeLabel, 0.18f)), pillSize.y);
            draw->AddText(ImVec2(pillMin.x + Px(6.0f), pillMin.y + Px(1.0f)), ImGui::GetColorU32(ManifoldTheme::CodeLabel), pill.c_str());
            titleRight = pillMax.x;
        }
        {
            const ImVec2 closeSize = ImGui::CalcTextSize(ICON_FA_TIMES);
            const float closeX = frameMax.x - Px(12.0f) - closeSize.x;
            draw->AddText(ImVec2(closeX, textY), ImGui::GetColorU32(ManifoldTheme::Muted), ICON_FA_TIMES);
            char dimensions[48];
            std::snprintf(dimensions, sizeof(dimensions), "%d x %d%s", static_cast<int>(pointsWidth), static_cast<int>(pointsHeight), window->Resizable ? "" : "  fixed");
            const float dimensionsWidth = ImGui::CalcTextSize(dimensions).x;
            if (closeX - Px(14.0f) - dimensionsWidth > titleRight + Px(8.0f))
                draw->AddText(ImVec2(closeX - Px(14.0f) - dimensionsWidth, textY), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.8f)), dimensions);
        }
        {
            ImGui::SetCursorScreenPos(frameMin);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("##titlebar", ImVec2(size.x, titleHeight), ImGuiChildFlags_None,
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            ImGui::SetCursorScreenPos(frameMin);
            ImGui::InvisibleButton("##formtitle", ImVec2(size.x, titleHeight));
            if (ImGui::IsItemActivated())
            {
                m_frameMoved = false;
                if (!m_trying)
                    Select(id);
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (ImGui::IsItemActive() && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            {
                // an undo step only for a real move, not for a click that selects
                if (!m_frameMoved)
                    Remember();
                m_frameMoved = true;
                SetFramePlace(id, ImVec2(place.x + ImGui::GetIO().MouseDelta.x / Px(1.0f), place.y + ImGui::GetIO().MouseDelta.y / Px(1.0f)));
                Changed();
            }
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("%s", modal ? "A modal form: it opens in front of the program and blocks it until it is closed.\nDrag the title to move it on the work surface; click it to select the modal."
                                              : "Drag the title to move the form on the work surface; click it to select the window.");
            ImGui::EndChild();
        }
        ImGui::SetCursorScreenPos(ImVec2(frameMin.x, frameMin.y + titleHeight));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::Border);
        ImGui::BeginChild("##previewbody", ImVec2(size.x, bodyHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor(2);
        const ImVec2 bodyMin = ImGui::GetWindowPos();
        const ImVec2 bodyMax(bodyMin.x + ImGui::GetWindowWidth(), bodyMin.y + ImGui::GetWindowHeight());
        if (m_trying)
            m_tryHost->DrawContents(id, nullptr);
        else
        {
            // ∑ widgets that follow another one (Enabled When) are drawn as
            // the window starts: dimmed while the rule does not hold. Only for
            // the drawing; the saved enabled option stays as it is.
            std::vector<std::pair<int, bool>> held;
            for (const auto& [node, meta] : m_form.Meta)
            {
                const std::optional<Lua::FormRule> rule = meta.EnabledWhen.empty() ? std::nullopt : Lua::FormParseWhen(meta.EnabledWhen);
                UiNode* target = rule ? m_form.Model.Find(node) : nullptr;
                if (target && target->Enabled && !Lua::FormRuleHolds(m_form, *rule))
                {
                    held.push_back({ node, target->Enabled });
                    target->Enabled = false;
                }
            }
            UiDesign pass;
            pass.Picking = true;
            pass.ForceTab = m_design.ForceTab;
            m_host->DrawContents(id, &pass);
            for (const auto& [node, enabled] : held)
            {
                if (UiNode* target = m_form.Model.Find(node))
                    target->Enabled = enabled;
            }
            m_design.ForceTab = pass.ForceTab;
            m_design.Rects.insert(m_design.Rects.end(), pass.Rects.begin(), pass.Rects.end());
        }
        const bool grippable = !m_trying && window->Resizable && !window->AutoSize;
        const float grip = Px(16.0f);
        const ImVec2 gripMin = ImVec2(frameMax.x - grip, frameMax.y - grip);
        if (grippable)
            m_grips.push_back({ gripMin, frameMax });
        ImGui::EndChild();
        const bool mine = m_gripping && m_gripWindow == id;
        if (grippable || mine)
        {
            // ∑ No item: an item at the body's corner would grow the body's
            // content and give it a scroll bar right under the grip. The grip
            // tests the mouse itself and holds an active id while it drags, so
            // the tool window does not move along.
            const ImGuiID gripId = ImGui::GetID("##sizegrip");
            const ImGuiIO& io = ImGui::GetIO();
            const bool over = grippable && ImGui::IsMouseHoveringRect(gripMin, frameMax) &&
                              ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (over && !m_gripping && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload())
            {
                m_gripping = true;
                m_gripWindow = id;
                m_frameMoved = false;
                ImGui::SetActiveID(gripId, ImGui::GetCurrentWindow());
            }
            if (m_gripping && m_gripWindow == id)
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    ImGui::KeepAliveID(gripId);
                    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
                    {
                        if (!m_frameMoved)
                            Remember(); // only when the size really changes
                        m_frameMoved = true;
                        if (UiNode* edited = m_form.Model.Find(id))
                        {
                            const float scale = Px(1.0f);
                            edited->Width = limit(std::round(pointsWidth + io.MouseDelta.x / scale), edited->MinWidth > 0.0f ? edited->MinWidth : 160.0f, edited->MaxWidth);
                            edited->Height = limit(std::round(pointsHeight + io.MouseDelta.y / scale), edited->MinHeight > 0.0f ? edited->MinHeight : 100.0f, edited->MaxHeight);
                            Changed();
                        }
                    }
                }
                else
                {
                    m_gripping = false;
                    m_gripWindow = 0;
                    if (ImGui::GetActiveID() == gripId)
                        ImGui::ClearActiveID();
                }
            }
            const bool held = m_gripping && m_gripWindow == id;
            if (over || held)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            if (over && !held)
                ImGui::SetTooltip("Drag to try other sizes; the window keeps to its minimum and maximum.");
            if (grippable)
            {
                const ImU32 color = ImGui::GetColorU32(held ? ManifoldTheme::Accent : over ? ManifoldTheme::Text : ManifoldTheme::Muted);
                ImDrawList* marks = Marks();
                marks->PushClipRect(areaMin, areaMax, true);
                for (int i = 1; i <= 3; ++i)
                {
                    const float inset = Px(4.0f) * static_cast<float>(i);
                    marks->AddLine(ImVec2(frameMax.x - inset, frameMax.y - 2.0f), ImVec2(frameMax.x - 2.0f, frameMax.y - inset), color, 1.5f);
                }
                marks->PopClipRect();
            }
        }
        if (!m_trying)
        {
            PreviewOverlay(bodyMin, bodyMax, id);
            if (window->MinWidth > 0.0f || window->MinHeight > 0.0f)
            {
                // the minimum size, above the body (it used to lie under it)
                ImDrawList* marks = Marks();
                const ImVec2 least(frameMin.x + Px(window->MinWidth > 0.0f ? window->MinWidth : 160.0f), frameMin.y + Px(window->MinHeight > 0.0f ? window->MinHeight : 100.0f));
                if (least.x < frameMax.x - 1.0f || least.y < frameMax.y - 1.0f)
                {
                    const ImU32 guide = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.7f));
                    DashedRect(marks, frameMin, least, guide, 1.0f, Px(5.0f), Px(4.0f));
                    const char* text = "min";
                    const ImVec2 textSize = ImGui::CalcTextSize(text);
                    const ImVec2 tagMin(least.x - textSize.x - Px(8.0f), least.y - textSize.y - Px(3.0f));
                    marks->AddRectFilled(tagMin, ImVec2(least.x - Px(1.0f), least.y - Px(1.0f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Page, 0.85f)), Px(3.0f));
                    marks->AddText(ImVec2(tagMin.x + Px(4.0f), tagMin.y + Px(1.0f)), guide, text);
                }
            }
        }
        // the selected window, or the one a click in Try just opened
        const bool flash = m_flashWindow == id && ImGui::GetTime() < m_flashUntil;
        if ((!m_trying && m_selected == id) || flash)
        {
            ImDrawList* marks = Marks();
            marks->PushClipRect(areaMin, areaMax, true);
            const float alpha = flash ? 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 12.0f) : 0.9f;
            marks->AddRect(ImVec2(frameMin.x - Px(3.0f), frameMin.y - Px(3.0f)), ImVec2(frameMax.x + Px(3.0f), frameMax.y + Px(3.0f)),
                ImGui::GetColorU32(ManifoldTheme::WithAlpha(modal ? ManifoldTheme::CodeLabel : ManifoldTheme::Accent, alpha)), Px(8.0f), 0, Px(2.0f));
            marks->PopClipRect();
        }
        marksWindow->BeginOrderWithinParent = SHRT_MAX;
        m_marks = nullptr;
        ImGui::EndChild();
        area->DC.CursorMaxPos = cursorMax;
        ImGui::PopID();
    }

    // ∑ A button's opens = "<modal>" is drawn as an arrow from the button's
    // port on its form's edge to the modal's title; the side follows where
    // the modal sits (beside or below).
    void FormEditorWindow::JumpLinks(ImVec2 areaMin, ImVec2 areaMax)
    {
        // on the surface, below the frames; the dashed part inside the form on its own layer
        ImDrawList* links = ImGui::GetWindowDrawList();
        for (const auto& [id, meta] : m_form.Meta)
        {
            if (meta.Opens.empty())
                continue;
            int target = 0;
            for (const int modal : m_form.Modals)
            {
                if (Lua::FormModalName(m_form, modal) == meta.Opens)
                    target = modal;
            }
            const int root = Lua::FormRootWindow(m_form, id);
            const auto from = m_frames.find(root);
            const auto to = m_frames.find(target);
            const UiDesign::Rect* rect = RectOf(id);
            if (!target || target == root || from == m_frames.end() || to == m_frames.end() || !rect)
                continue;
            const Frame& a = from->second;
            const Frame& b = to->second;
            const ImVec2 ca((a.Min.x + a.Max.x) * 0.5f, (a.Min.y + a.Max.y) * 0.5f);
            const ImVec2 cb((b.Min.x + b.Max.x) * 0.5f, (b.Min.y + b.Max.y) * 0.5f);
            const bool beside = b.Min.x > a.Max.x || b.Max.x < a.Min.x;
            const float bodyTop = a.Min.y + a.Title;
            ImVec2 port;
            ImVec2 end;
            ImVec2 outA;
            ImVec2 outB;
            const float widgetX = std::clamp((rect->Min.x + rect->Max.x) * 0.5f, a.Min.x + Px(10.0f), a.Max.x - Px(10.0f));
            const float widgetY = std::clamp((rect->Min.y + rect->Max.y) * 0.5f, bodyTop + Px(8.0f), std::max(bodyTop + Px(8.0f), a.Max.y - Px(8.0f)));
            if (beside)
            {
                const bool right = cb.x > ca.x;
                port = ImVec2(right ? a.Max.x : a.Min.x, widgetY);
                end = ImVec2(right ? b.Min.x : b.Max.x, b.Min.y + b.Title * 0.5f);
                const float bend = std::max(Px(40.0f), std::abs(end.x - port.x) * 0.45f);
                outA = ImVec2(port.x + (right ? bend : -bend), port.y);
                outB = ImVec2(end.x + (right ? -bend : bend), end.y);
            }
            else
            {
                const bool down = cb.y > ca.y;
                port = ImVec2(widgetX, down ? a.Max.y : a.Min.y);
                end = ImVec2(std::clamp(widgetX, b.Min.x + Px(20.0f), b.Max.x - Px(20.0f)), down ? b.Min.y : b.Max.y);
                const float bend = std::max(Px(30.0f), std::abs(end.y - port.y) * 0.45f);
                outA = ImVec2(port.x, port.y + (down ? bend : -bend));
                outB = ImVec2(end.x, end.y + (down ? -bend : bend));
            }
            const bool strong = id == m_selected || id == m_hovered || target == m_selected;
            const ImVec4 tone = ManifoldTheme::CodeLabel;
            const ImU32 color = ImGui::GetColorU32(ManifoldTheme::WithAlpha(tone, strong ? 0.95f : 0.45f));
            links->PushClipRect(areaMin, areaMax, true);
            links->AddBezierCubic(port, outA, outB, end, color, strong ? Px(2.0f) : Px(1.4f));
            links->AddCircleFilled(port, strong ? Px(3.5f) : Px(2.5f), color);
            {
                // arrow head along the curve's last tangent
                ImVec2 dir(end.x - outB.x, end.y - outB.y);
                const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                if (length > 0.0f)
                {
                    dir = ImVec2(dir.x / length, dir.y / length);
                    const float head = Px(strong ? 9.0f : 7.0f);
                    const ImVec2 side(-dir.y, dir.x);
                    links->AddTriangleFilled(end, ImVec2(end.x - dir.x * head + side.x * head * 0.5f, end.y - dir.y * head + side.y * head * 0.5f),
                        ImVec2(end.x - dir.x * head - side.x * head * 0.5f, end.y - dir.y * head - side.y * head * 0.5f), color);
                }
            }
            if (strong)
            {
                const std::string label = std::string(ICON_FA_WINDOW_RESTORE "  opens ") + meta.Opens;
                const ImVec2 at((port.x + end.x) * 0.5f, (port.y + end.y) * 0.5f);
                const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
                const ImVec2 labelMin(at.x - labelSize.x * 0.5f - Px(6.0f), at.y - labelSize.y * 0.5f - Px(2.0f));
                links->AddRectFilled(labelMin, ImVec2(labelMin.x + labelSize.x + Px(12.0f), labelMin.y + labelSize.y + Px(4.0f)), ImGui::GetColorU32(ManifoldTheme::Page), Px(4.0f));
                links->AddRect(labelMin, ImVec2(labelMin.x + labelSize.x + Px(12.0f), labelMin.y + labelSize.y + Px(4.0f)), color, Px(4.0f));
                links->AddText(ImVec2(labelMin.x + Px(6.0f), labelMin.y + Px(2.0f)), color, label.c_str());
            }
            links->PopClipRect();
            ImDrawList* inner = m_frameMarks.count(root) ? m_frameMarks[root] : nullptr;
            if (strong && inner)
            {
                const ImVec2 innerMin(std::max(a.Min.x, areaMin.x), std::max(bodyTop, areaMin.y));
                const ImVec2 innerMax(std::min(a.Max.x, areaMax.x), std::min(a.Max.y, areaMax.y));
                if (innerMax.x > innerMin.x && innerMax.y > innerMin.y)
                {
                    const ImVec2 from2 = beside ? ImVec2(port.x > widgetX ? rect->Max.x + Px(3.0f) : rect->Min.x - Px(3.0f), widgetY)
                                                : ImVec2(widgetX, port.y > widgetY ? rect->Max.y + Px(3.0f) : rect->Min.y - Px(3.0f));
                    inner->PushClipRect(innerMin, innerMax, true);
                    DashedLine(inner, from2, port, ImGui::GetColorU32(ManifoldTheme::WithAlpha(tone, 0.6f)), 1.0f, Px(4.0f), Px(3.0f));
                    inner->PopClipRect();
                }
            }
        }
    }

    // ∑ Try has no script behind it, so the editor plays what a form file
    // does itself: a click on a button that opens a modal flashes that
    // modal's frame and brings it into view.
    void FormEditorWindow::TryEvents(ImVec2 areaMin, ImVec2 areaMax)
    {
        for (const Lua::UiEvent& event : m_try.Model.TakeEvents())
        {
            if (event.Name != "click")
                continue;
            const auto meta = m_try.Meta.find(event.Target);
            if (meta == m_try.Meta.end())
                continue;
            int target = 0;
            if (!meta->second.Opens.empty())
            {
                for (const int modal : m_try.Modals)
                {
                    if (Lua::FormModalName(m_try, modal) == meta->second.Opens)
                        target = modal;
                }
            }
            else if (meta->second.Closes)
                target = Lua::FormRootWindow(m_try, event.Target);
            const auto frame = m_frames.find(target);
            if (!target || frame == m_frames.end())
                continue;
            m_flashWindow = target;
            m_flashUntil = ImGui::GetTime() + (meta->second.Opens.empty() ? 0.6 : 1.4);
            const Frame& f = frame->second;
            if (!meta->second.Opens.empty() && (f.Min.x < areaMin.x || f.Min.y < areaMin.y || f.Max.x > areaMax.x || f.Max.y > areaMax.y))
            {
                m_pan.x += (areaMin.x + areaMax.x) * 0.5f - (f.Min.x + f.Max.x) * 0.5f;
                m_pan.y += (areaMin.y + areaMax.y) * 0.5f - (f.Min.y + f.Max.y) * 0.5f;
            }
        }
    }

    void FormEditorWindow::AddModal(int opener)
    {
        Remember();
        const int id = Lua::FormAddModal(m_form);
        if (!id)
        {
            m_undo.pop_back();
            return;
        }
        if (const UiNode* button = m_form.Model.Find(opener); button && button->Kind == UiKind::Button)
            m_form.Meta[opener].Opens = Lua::FormModalName(m_form, id);
        Changed();
        Select(id);
        if (const auto frame = m_frames.find(m_form.Window); frame != m_frames.end())
        {
            // bring the new modal into view
            const ImVec2 place = FramePlace(id);
            m_pan.y = std::min(m_pan.y, -Px(place.y) + Px(30.0f));
        }
    }

    const UiDesign::Rect* FormEditorWindow::RectOf(int id) const
    {
        for (const UiDesign::Rect& rect : m_design.Rects)
        {
            if (rect.Node == id)
                return &rect;
        }
        return nullptr;
    }

    bool FormEditorWindow::Within(int node, int ancestor) const
    {
        for (const UiNode* up = m_form.Model.Find(node); up; up = m_form.Model.Find(up->Parent))
        {
            if (up->Id == ancestor)
                return true;
        }
        return false;
    }

    FormEditorWindow::DropSpot FormEditorWindow::SpotAt(ImVec2 mouse, int moving) const
    {
        const UiDesign::Rect* best = nullptr;
        for (const UiDesign::Rect& rect : m_design.Rects)
        {
            if (mouse.x < rect.Min.x || mouse.y < rect.Min.y || mouse.x >= rect.Max.x || mouse.y >= rect.Max.y)
                continue;
            if (moving && Within(rect.Node, moving))
                continue;
            if (m_overlayWindow && Lua::FormRootWindow(m_form, rect.Node) != m_overlayWindow)
                continue;
            if (!best || rect.Depth > best->Depth || (rect.Depth == best->Depth && (rect.Max.x - rect.Min.x) * (rect.Max.y - rect.Min.y) < (best->Max.x - best->Min.x) * (best->Max.y - best->Min.y)))
                best = &rect;
        }
        const int window = m_overlayWindow ? m_overlayWindow : m_form.Window;
        DropSpot spot{ window, Drop::Inside };
        if (!best)
            return spot;
        const UiNode* node = m_form.Model.Find(best->Node);
        const UiNode* parent = node ? m_form.Model.Find(node->Parent) : nullptr;
        if (!node || !parent)
            return spot;
        spot.Target = node->Id;
        const bool across = parent->Kind == UiKind::Row || parent->Kind == UiKind::Grid;
        // ∑ the left or right edge of a widget in a column, section, tab
        // or window: dropped there, the two go side by side into a new row
        if (!across && node->Kind != UiKind::Tab && parent->Kind != UiKind::Split && parent->Kind != UiKind::Tabs && Lua::FormCanHold(parent->Kind, UiKind::Row))
        {
            const float side = std::min(Px(28.0f), (best->Max.x - best->Min.x) * 0.2f);
            if (mouse.x < best->Min.x + side || mouse.x > best->Max.x - side)
            {
                spot.Where = mouse.x < best->Min.x + side ? Drop::Left : Drop::Right;
                return spot;
            }
        }
        const float low = across ? best->Min.x : best->Min.y;
        const float high = across ? best->Max.x : best->Max.y;
        const float at = across ? mouse.x : mouse.y;
        if (Lua::UiIsContainer(node->Kind) && node->Kind != UiKind::Tab)
        {
            const float edge = std::min(Px(10.0f), (high - low) * 0.25f);
            spot.Where = at < low + edge ? Drop::Before : at > high - edge ? Drop::After : Drop::Inside;
        }
        else if (node->Kind == UiKind::Tab)
            spot.Where = Drop::Inside;
        else
            spot.Where = at < (low + high) * 0.5f ? Drop::Before : Drop::After;
        return spot;
    }

    void FormEditorWindow::DrawSpot(ImDrawList* draw, const DropSpot& spot) const
    {
        const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
        const UiDesign::Rect* rect = RectOf(spot.Target);
        if (!rect)
            return;
        if (spot.Where == Drop::Inside)
        {
            draw->AddRectFilled(rect->Min, rect->Max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.12f)), Px(3.0f));
            draw->AddRect(rect->Min, rect->Max, accent, Px(3.0f), 0, Px(2.0f));
            return;
        }
        const UiNode* node = m_form.Model.Find(spot.Target);
        const UiNode* parent = node ? m_form.Model.Find(node->Parent) : nullptr;
        const bool beside = spot.Where == Drop::Left || spot.Where == Drop::Right;
        const bool across = beside || (parent && (parent->Kind == UiKind::Row || parent->Kind == UiKind::Grid));
        const float thick = Px(3.0f);
        ImVec2 a;
        ImVec2 b;
        if (beside)
        {
            // the target's place is shared: its half lit, a bar on the side the new widget takes
            const float x = spot.Where == Drop::Left ? rect->Min.x - thick : rect->Max.x + thick;
            draw->AddRect(rect->Min, rect->Max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.45f)), Px(3.0f), 0, Px(1.0f));
            a = ImVec2(x, rect->Min.y);
            b = ImVec2(x, rect->Max.y);
            const char* hint = ICON_FA_COLUMNS " side by side";
            const ImVec2 size = ImGui::CalcTextSize(hint);
            const ImVec2 at(spot.Where == Drop::Left ? rect->Min.x + Px(4.0f) : rect->Max.x - size.x - Px(10.0f), rect->Min.y - size.y - Px(4.0f));
            draw->AddRectFilled(ImVec2(at.x - Px(4.0f), at.y - Px(1.0f)), ImVec2(at.x + size.x + Px(4.0f), at.y + size.y + Px(1.0f)), accent, Px(3.0f));
            draw->AddText(at, ImGui::GetColorU32(ManifoldTheme::Page), hint);
        }
        else if (across)
        {
            const float x = spot.Where == Drop::Before ? rect->Min.x - thick : rect->Max.x + thick;
            a = ImVec2(x, rect->Min.y);
            b = ImVec2(x, rect->Max.y);
        }
        else
        {
            const float y = spot.Where == Drop::Before ? rect->Min.y - thick : rect->Max.y + thick;
            a = ImVec2(rect->Min.x, y);
            b = ImVec2(rect->Max.x, y);
        }
        draw->AddLine(a, b, accent, thick);
        draw->AddCircleFilled(a, thick * 1.4f, accent);
        draw->AddCircleFilled(b, thick * 1.4f, accent);
    }

    // ∑ A record from the address list becomes the widget that fits it,
    // bound to it: a switch for a script or group, a combo for a value with a
    // drop-down list, a field for text, a number otherwise.
    int FormEditorWindow::AddRecordWidget(int parent, int index, int recordId)
    {
        const Table::MemoryRecord* record = m_engine.Table().FindById(recordId);
        const UiNode* holder = m_form.Model.Find(parent);
        if (!record || !holder)
            return 0;
        UiKind kind = UiKind::Number;
        if (record->Kind != Table::RecordKind::Value)
            kind = UiKind::Checkbox;
        else if (!record->DropDown.empty())
            kind = UiKind::Combo;
        else if (record->Type == Scan::ValueType::String || record->Type == Scan::ValueType::ByteArray)
            kind = UiKind::Input;
        if (!Lua::FormCanHold(holder->Kind, kind))
            return 0;
        const int id = Lua::FormAdd(m_form, kind, parent, index);
        UiNode* node = m_form.Model.Find(id);
        if (!node)
            return 0;
        const std::string label = record->Description.empty() ? std::string("Record ") + std::to_string(record->Id) : record->Description;
        node->Label = label;
        if (kind == UiKind::Checkbox)
            node->Style = "switch";
        else if (kind == UiKind::Combo)
        {
            node->Items.clear();
            for (const Table::DropDownEntry& entry : record->DropDown)
                node->Items.push_back(entry.Text.empty() ? entry.Value : entry.Text);
            node->Selected = 0;
        }
        else if (kind == UiKind::Number)
        {
            node->Integer = record->Type != Scan::ValueType::Float && record->Type != Scan::ValueType::Double;
            node->Hex = record->ShowHex && node->Integer;
        }
        Lua::FormMeta& meta = m_form.Meta[id];
        meta.Name = Lua::FormNameFromText(m_form, label);
        const bool unique = !record->Description.empty() && m_engine.Table().CountByDescription(record->Description) == 1;
        meta.Record = unique ? record->Description : std::to_string(record->Id);
        meta.RecordById = !unique;
        m_form.Model.Touch(*node);
        return id;
    }

    void FormEditorWindow::AddRecordAt(int record, int parent, int index)
    {
        Remember();
        const int id = AddRecordWidget(parent, index, record);
        if (!id)
        {
            m_undo.pop_back();
            m_engine.Notify(LogLevel::Warning, "That record cannot go there.");
            return;
        }
        Changed();
        Select(id);
    }

    void FormEditorWindow::DropBeside(const ImGuiPayload* payload, const DropSpot& spot)
    {
        const UiNode* target = m_form.Model.Find(spot.Target);
        if (!target)
            return;
        const int moving = payload->IsDataType(kNodePayload) ? *static_cast<const int*>(payload->Data) : 0;
        if (moving && (moving == spot.Target || Within(spot.Target, moving)))
            return;
        FormDocument before = m_form;
        const int row = Lua::FormAdd(m_form, UiKind::Row, target->Parent, Lua::FormIndexOf(m_form, spot.Target));
        int placed = 0;
        if (row && Lua::FormMove(m_form, spot.Target, row, 0))
        {
            const int index = spot.Where == Drop::Left ? 0 : 1;
            if (payload->IsDataType(kKindPayload))
                placed = Lua::FormAdd(m_form, static_cast<UiKind>(*static_cast<const int*>(payload->Data)), row, index);
            else if (payload->IsDataType(kRecordPayload) && payload->DataSize == sizeof(int))
                placed = AddRecordWidget(row, index, *static_cast<const int*>(payload->Data));
            else if (moving && Lua::FormMove(m_form, moving, row, index))
                placed = moving;
        }
        if (!placed)
        {
            m_form = std::move(before);
            return;
        }
        m_undo.push_back(std::move(before));
        if (m_undo.size() > kUndoLimit)
            m_undo.pop_front();
        m_redo.clear();
        m_lastSource = 0;
        Changed();
        Select(placed);
    }

    void FormEditorWindow::DropPayload(const ImGuiPayload* payload, const DropSpot& spot)
    {
        if (spot.Where == Drop::Left || spot.Where == Drop::Right)
        {
            DropBeside(payload, spot);
            return;
        }
        int parent = 0;
        int index = -1;
        if (!DropAt(spot.Target, spot.Where, parent, index))
            return;
        if (payload->IsDataType(kKindPayload))
        {
            AddInto(static_cast<UiKind>(*static_cast<const int*>(payload->Data)), parent, index);
            return;
        }
        if (payload->IsDataType(kRecordPayload))
        {
            if (payload->DataSize == sizeof(int))
                AddRecordAt(*static_cast<const int*>(payload->Data), parent, index);
            return;
        }
        const int moving = *static_cast<const int*>(payload->Data);
        if (moving == spot.Target || Within(parent, moving))
            return;
        FormDocument before = m_form;
        if (Lua::FormMove(m_form, moving, parent, index))
        {
            m_undo.push_back(std::move(before));
            if (m_undo.size() > kUndoLimit)
                m_undo.pop_front();
            m_redo.clear();
            m_lastSource = 0;
            Changed();
            Select(moving);
        }
    }

    void FormEditorWindow::PreviewOverlay(ImVec2 min, ImVec2 max, int window)
    {
        m_overlayWindow = window;
        const auto mine = [&](int node) { return node && Lua::FormRootWindow(m_form, node) == window; };
        // ∑ The form can be panned partly out of the work surface; the overlay
        // (marks layer) must stop at the surface's edge like the body does.
        const auto clip = m_frameClips.count(window) ? m_frameClips[window] : std::pair<ImVec2, ImVec2>(min, max);
        {
            min = ImVec2(std::max(min.x, clip.first.x), std::max(min.y, clip.first.y));
            max = ImVec2(std::min(max.x, clip.second.x), std::min(max.y, clip.second.y));
            max = ImVec2(std::max(max.x, min.x), std::max(max.y, min.y));
        }
        const ImGuiIO& io = ImGui::GetIO();
        const ImGuiID pick = ImGui::GetID("##previewpick");
        // ∑ called inside this frame's layer: the hovered window is one of its
        // children only when no other frame lies on top of the mouse
        const bool overMe = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool inside = ImGui::IsMouseHoveringRect(min, max) && overMe;
        int pointed = 0;
        if (inside)
        {
            int depth = -1;
            for (const UiDesign::Rect& rect : m_design.Rects)
            {
                if (io.MousePos.x >= rect.Min.x && io.MousePos.y >= rect.Min.y && io.MousePos.x < rect.Max.x && io.MousePos.y < rect.Max.y && rect.Depth > depth && mine(rect.Node))
                {
                    pointed = rect.Node;
                    depth = rect.Depth;
                }
            }
            bool onGrip = false;
            for (const auto& [gripMin, gripMax] : m_grips)
                onGrip = onGrip || (io.MousePos.x >= gripMin.x && io.MousePos.y >= gripMin.y && io.MousePos.x < gripMax.x && io.MousePos.y < gripMax.y);
            if (pointed && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload() && !onGrip)
                OpenFirstHandler(pointed);
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload() && !onGrip)
            {
                Select(pointed ? pointed : window);
                m_pressNode = pointed;
                m_pressing = true;
                ImGui::SetActiveID(pick, ImGui::GetCurrentWindow());
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                Select(pointed ? pointed : window);
                ImGui::OpenPopupEx(m_previewMenu); // the menu lives on the surface, not in this layer's id scope
            }
            if (pointed)
                m_hovered = pointed;
        }
        if (m_pressing)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                ImGui::KeepAliveID(pick);
            else
            {
                m_pressing = false;
                m_pressNode = 0;
                if (ImGui::GetActiveID() == pick)
                    ImGui::ClearActiveID();
            }
        }
        if (m_pressing && m_pressNode && mine(m_pressNode) && m_pressNode != window && ImGui::IsMouseDragging(ImGuiMouseButton_Left, Px(6.0f)))
        {
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
            {
                const int node = m_pressNode;
                ImGui::SetDragDropPayload(kNodePayload, &node, sizeof(node));
                if (const UiNode* item = m_form.Model.Find(node))
                    ImGui::Text("%s  %s", KindIcon(item->Kind), Caption(m_form, *item).c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImDrawList* draw = Marks();
        draw->PushClipRect(min, max, true);
        const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
        const ImU32 soft = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.45f));
        const auto outline = [&](int id, ImU32 color, float thickness) {
            if (const UiDesign::Rect* rect = RectOf(id))
                draw->AddRect(ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), ImVec2(rect->Max.x + 2.0f, rect->Max.y + 2.0f), color, Px(3.0f), 0, thickness);
        };
        const auto label = [&](int id) {
            const UiNode* node = m_form.Model.Find(id);
            if (!node)
                return std::string();
            const auto meta = m_form.Meta.find(id);
            std::string text = std::string(KindIcon(node->Kind)) + " " + std::string(Lua::FormKindTitle(node->Kind));
            if (meta != m_form.Meta.end() && !meta->second.Name.empty())
                text += "  " + meta->second.Name;
            if (meta != m_form.Meta.end() && !meta->second.EnabledWhen.empty())
                text += "  \xC2\xB7  " ICON_FA_TOGGLE_ON " " + meta->second.EnabledWhen;
            if (meta != m_form.Meta.end() && !meta->second.Record.empty())
                text += "  \xC2\xB7  " ICON_FA_LINK " " + (meta->second.RecordById ? "#" + meta->second.Record : meta->second.Record);
            return text;
        };
        {
            const ImU32 structure = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.55f));
            const ImU32 cell = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.22f));
            const ImU32 hatch = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.35f));
            const float dash = Px(4.0f);
            const float gap = Px(3.0f);
            for (const UiDesign::Rect& rect : m_design.Rects)
            {
                const UiNode* node = m_form.Model.Find(rect.Node);
                if (!node || !mine(rect.Node))
                    continue;
                const UiNode* parent = m_form.Model.Find(node->Parent);
                if (Lua::UiIsContainer(node->Kind))
                {
                    DashedRect(draw, ImVec2(rect.Min.x - 1.0f, rect.Min.y - 1.0f), ImVec2(rect.Max.x + 1.0f, rect.Max.y + 1.0f), structure, 1.0f, dash, gap);
                    if (node->Children.empty())
                    {
                        const char* hint = node->Kind == UiKind::Tabs ? "Drop a tab here" : "Drop widgets here";
                        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
                        draw->AddText(ImVec2((rect.Min.x + rect.Max.x - hintSize.x) * 0.5f, (rect.Min.y + rect.Max.y - hintSize.y) * 0.5f),
                            ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.85f)), hint);
                    }
                }
                else if (parent && parent->Kind == UiKind::Grid)
                    DashedRect(draw, rect.Min, rect.Max, cell, 1.0f, dash, gap);
                if (!node->Visible)
                    Hatch(draw, rect.Min, rect.Max, hatch, Px(7.0f));
            }
        }
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        const bool dragging = payload && (payload->IsDataType(kKindPayload) || payload->IsDataType(kNodePayload) || payload->IsDataType(kRecordPayload));
        const ImU32 onAccent = ImGui::GetColorU32(ManifoldTheme::Page);
        const ImRect area(clip.first, clip.second);
        struct PendingTag
        {
            ImVec2 Min;
            std::string Text;
            ImU32 Background;
            float Right;
            int Node = 0;
        };
        std::vector<PendingTag> tags;
        if (!dragging)
        {
            if (m_hovered && m_hovered != m_selected && mine(m_hovered))
            {
                if (const UiDesign::Rect* rect = RectOf(m_hovered))
                {
                    DashedRect(draw, ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), ImVec2(rect->Max.x + 2.0f, rect->Max.y + 2.0f), soft, Px(1.5f), Px(5.0f), Px(3.0f));
                    tags.push_back({ ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), label(m_hovered), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.55f)), rect->Max.x + 2.0f, m_hovered });
                }
            }
            if (m_selected != window && mine(m_selected))
            {
                outline(m_selected, accent, Px(2.0f));
                if (const UiDesign::Rect* rect = RectOf(m_selected))
                    tags.push_back({ ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), label(m_selected), accent, 0.0f, m_selected });
            }
        }
        else if (ImGui::IsMouseHoveringRect(min, max) && overMe)
        {
            const int moving = payload->IsDataType(kNodePayload) ? *static_cast<const int*>(payload->Data) : 0;
            const DropSpot spot = SpotAt(io.MousePos, moving);
            if (moving)
            {
                if (const UiDesign::Rect* rect = RectOf(moving))
                {
                    draw->AddRectFilled(rect->Min, rect->Max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Page, 0.6f)));
                    DashedRect(draw, rect->Min, rect->Max, soft, Px(1.5f), Px(5.0f), Px(3.0f));
                }
            }
            if (spot.Target == window)
                draw->AddRect(min, max, accent, 0.0f, 0, Px(2.0f));
            else
                DrawSpot(draw, spot);
            if (ImGui::BeginDragDropTargetCustom(ImRect(min, max), ImGui::GetID(("##previewdrop" + std::to_string(window)).c_str())))
            {
                if (const ImGuiPayload* kind = ImGui::AcceptDragDropPayload(kKindPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                    DropPayload(kind, spot);
                if (const ImGuiPayload* moved = ImGui::AcceptDragDropPayload(kNodePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                    DropPayload(moved, spot);
                if (const ImGuiPayload* record = ImGui::AcceptDragDropPayload(kRecordPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                    DropPayload(record, spot);
                ImGui::EndDragDropTarget();
            }
        }
        if (!dragging)
        {
            // ∑ Enabled When: a dashed elbow from the widget another one
            // follows, while either of them is hovered or selected
            const ImU32 ruleColor = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Success, 0.9f));
            for (const auto& [id, meta] : m_form.Meta)
            {
                if (meta.EnabledWhen.empty() || !mine(id))
                    continue;
                const std::optional<Lua::FormRule> rule = Lua::FormParseWhen(meta.EnabledWhen);
                const int source = rule ? Lua::FormFindNamed(m_form, rule->Source) : 0;
                if (!source || !mine(source) || !(id == m_selected || id == m_hovered || source == m_selected || source == m_hovered))
                    continue;
                const UiDesign::Rect* a = RectOf(source);
                const UiDesign::Rect* b = RectOf(id);
                if (!a || !b)
                    continue;
                const float x = std::max(std::min(std::max(a->Max.x, b->Max.x) + Px(12.0f), max.x - Px(5.0f)), std::max(a->Max.x, b->Max.x) - Px(6.0f));
                const ImVec2 from(std::min(a->Max.x + Px(2.0f), x), (a->Min.y + a->Max.y) * 0.5f);
                const ImVec2 to(std::min(b->Max.x + Px(2.0f), x), (b->Min.y + b->Max.y) * 0.5f);
                DashedLine(draw, from, ImVec2(x, from.y), ruleColor, Px(1.5f), Px(4.0f), Px(3.0f));
                DashedLine(draw, ImVec2(x, from.y), ImVec2(x, to.y), ruleColor, Px(1.5f), Px(4.0f), Px(3.0f));
                DashedLine(draw, ImVec2(x, to.y), to, ruleColor, Px(1.5f), Px(4.0f), Px(3.0f));
                draw->AddCircleFilled(from, Px(3.0f), ruleColor);
                draw->AddTriangleFilled(to, ImVec2(to.x + Px(7.0f), to.y - Px(4.5f)), ImVec2(to.x + Px(7.0f), to.y + Px(4.5f)), ruleColor);
            }
        }
        draw->PopClipRect();
        // ∑ A tag never covers another widget: it goes above or below its
        // widget where that space is free, else into the margin beside the
        // form (a short line leads to the frame), and only when none of that
        // fits over the content, faint. It fades while the mouse is on it, so
        // what lies under it can still be seen and clicked.
        const ImRect surface(m_surfaceMin, m_surfaceMax);
        draw->PushClipRect(surface.Min, surface.Max, false);
        const auto within = [&](int node, int outer) {
            for (const UiNode* item = m_form.Model.Find(node); item && item->Parent; item = m_form.Model.Find(item->Parent))
            {
                if (item->Parent == outer)
                    return true;
            }
            return false;
        };
        std::vector<ImRect> placed;
        const auto blocked = [&](const ImRect& box, int owner) {
            for (const ImRect& other : placed)
            {
                if (other.Overlaps(box))
                    return true;
            }
            for (const UiDesign::Rect& rect : m_design.Rects)
            {
                if (rect.Node == owner || !mine(rect.Node) || within(owner, rect.Node) || within(rect.Node, owner))
                    continue;
                if (ImRect(rect.Min, rect.Max).Overlaps(box))
                    return true;
            }
            return false;
        };
        const auto faded = [](ImU32 color, float factor) {
            ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
            value.w *= factor;
            return ImGui::GetColorU32(value);
        };
        for (const PendingTag& tag : tags)
        {
            const UiDesign::Rect* rect = RectOf(tag.Node);
            if (!rect)
                continue;
            const ImVec2 textSize = ImGui::CalcTextSize(tag.Text.c_str());
            const ImVec2 padding(Px(5.0f), Px(1.0f));
            const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
            const ImVec2 selMin(rect->Min.x - 2.0f, rect->Min.y - 2.0f);
            const ImVec2 selMax(rect->Max.x + 2.0f, rect->Max.y + 2.0f);
            const float gap = Px(2.0f);
            const float leftX = std::max(min.x + gap, std::min(selMin.x, max.x - size.x - gap));
            const float rightX = std::max(min.x + gap, std::min(selMax.x - size.x, max.x - size.x - gap));
            std::vector<ImVec2> inside = {
                ImVec2(leftX, selMin.y - size.y - gap), ImVec2(rightX, selMin.y - size.y - gap),
                ImVec2(leftX, selMax.y + gap), ImVec2(rightX, selMax.y + gap),
            };
            if (tag.Right > 0.0f)
                inside = { inside[1], inside[0], inside[3], inside[2] };
            std::optional<ImRect> box;
            for (const ImVec2& at : inside)
            {
                const ImRect candidate(at, ImVec2(at.x + size.x, at.y + size.y));
                if (candidate.Min.y >= min.y && candidate.Max.y <= max.y && candidate.Min.x >= min.x && candidate.Max.x <= max.x && !blocked(candidate, tag.Node))
                {
                    box = candidate;
                    break;
                }
            }
            float leader = 0.0f; // the frame edge a tag in the margin points at
            if (!box)
            {
                const float y = std::clamp(selMin.y, min.y, std::max(min.y, max.y - size.y));
                const float margin = Px(12.0f);
                const ImRect left(ImVec2(min.x - margin - size.x, y), ImVec2(min.x - margin, y + size.y));
                const ImRect right(ImVec2(max.x + margin, y), ImVec2(max.x + margin + size.x, y + size.y));
                const auto free = [&](const ImRect& candidate) {
                    if (candidate.Min.x < surface.Min.x + gap || candidate.Max.x > surface.Max.x - gap)
                        return false;
                    for (const ImRect& other : placed)
                    {
                        if (other.Overlaps(candidate))
                            return false;
                    }
                    return true;
                };
                if (free(left))
                {
                    box = left;
                    leader = min.x;
                }
                else if (free(right))
                {
                    box = right;
                    leader = max.x;
                }
            }
            float alpha = 1.0f;
            if (!box)
            {
                const ImVec2 at(leftX, selMin.y - size.y - gap >= min.y ? selMin.y - size.y - gap : selMin.y + gap);
                box = ImRect(at, ImVec2(at.x + size.x, at.y + size.y));
                alpha = 0.6f;
            }
            if (box->Contains(io.MousePos))
                alpha = 0.15f;
            placed.push_back(*box);
            const ImU32 background = faded(tag.Background, alpha);
            if (leader != 0.0f)
            {
                const float mid = (box->Min.y + box->Max.y) * 0.5f;
                const float from = leader < box->Min.x ? box->Min.x : box->Max.x;
                draw->AddLine(ImVec2(from, mid), ImVec2(leader, mid), background, Px(1.5f));
                draw->AddCircleFilled(ImVec2(leader, mid), Px(2.5f), background);
            }
            draw->AddRectFilled(box->Min, box->Max, background, Px(3.0f));
            draw->AddText(ImVec2(box->Min.x + padding.x, box->Min.y + padding.y), faded(onAccent, alpha), tag.Text.c_str());
        }
        draw->PopClipRect();
    }

    void FormEditorWindow::NameRow(UiNode& node)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if (node.Kind == UiKind::Window && Lua::FormIsModal(m_form, node.Id))
        {
            ImGui::TextUnformatted("Name");
            ItemTooltip("The modal's name: modals.<name> in the script, and what a button's Opens names.");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            std::string name = Lua::FormModalName(m_form, node.Id);
            const std::string before = name;
            if (InputTextWithHint("##modalname", "name", name, ImGuiInputTextFlags_CharsNoBlank) && name != before && Lua::FormIsIdentifier(name))
            {
                Remember(ImGui::GetItemID());
                Lua::FormRenameModal(m_form, node.Id, name);
                Changed();
            }
            if (!name.empty() && !Lua::FormIsIdentifier(name))
                TextWithError(NameError(name), ManifoldTheme::Error);
            return;
        }
        if (node.Kind == UiKind::Window)
        {
            ImGui::TextUnformatted("Key");
            ItemTooltip("key: a window created with the same key replaces the earlier one, so running the script again does not open a second window.");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (m_focusName)
            {
                ImGui::SetKeyboardFocusHere();
                m_focusName = false;
            }
            std::string key = m_form.Key;
            if (InputTextWithHint("##key", "none", key))
            {
                Remember(ImGui::GetItemID());
                m_form.Key = key;
                Changed();
            }
            return;
        }
        ImGui::TextUnformatted("Name");
        ItemTooltip("The widget's name in the script: w.<name>. Widgets the script reads or changes need one. F2 jumps here; handlers named after the widget follow a new name.");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_focusName)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusName = false;
        }
        std::string name = m_form.Meta.count(node.Id) ? m_form.Meta[node.Id].Name : std::string();
        if (InputTextWithHint("##name", "no name", name, ImGuiInputTextFlags_CharsNoBlank))
        {
            Remember(ImGui::GetItemID());
            m_form.Meta[node.Id].Name = name;
            Changed();
        }
        // ∑ widgets that follow this one (Enabled When) follow the new name once
        // typing ends: renaming on every keystroke lost the link when the field
        // was emptied first, and passed through other widgets' names on the way
        if (ImGui::IsItemActivated())
        {
            m_renameNode = node.Id;
            m_renameFrom = name;
        }
        if (ImGui::IsItemDeactivated() && m_renameNode == node.Id)
        {
            if (Lua::FormIsIdentifier(name) && name != m_renameFrom && Lua::FormFindNamed(m_form, m_renameFrom) == 0)
            {
                Lua::FormRenameReferences(m_form, m_renameFrom, name);
                RenameFunctions(node.Id, m_renameFrom, name);
                Changed();
            }
            m_renameNode = 0;
        }
        if (!name.empty() && !Lua::FormIsIdentifier(name))
            TextWithError(NameError(name), ManifoldTheme::Error);
    }

    void FormEditorWindow::PropertyRow(UiNode& node, const Lua::FormProp& prop)
    {
        ImGui::PushID(prop.Key.data(), prop.Key.data() + prop.Key.size());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        const bool changed = !Lua::FormPropIsDefault(node, prop.Key);
        const bool resettable = changed && !(node.Kind == UiKind::Window && prop.Key == "title");
        const float cellRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (changed)
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + Px(3.0f), at.y + ImGui::GetTextLineHeight() * 0.5f), Px(2.5f), ImGui::GetColorU32(ManifoldTheme::Accent));
            ImGui::SetCursorScreenPos(ImVec2(at.x + Px(10.0f), at.y));
        }
        // ∑ the name ends in "..." before the reset button instead of running
        // under it; the tooltip has the whole name
        const float side = ImGui::GetFrameHeight() * 0.8f;
        {
            const std::string title = TitleCase(prop.Key);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float limit = std::max(at.x, at.x + (cellRight - ImGui::GetCursorPosX()) - (resettable ? side + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f));
            const ImVec2 size = ImGui::CalcTextSize(title.c_str());
            ImGui::Dummy(ImVec2(std::min(size.x, limit - at.x), size.y));
            ImGui::PushStyleColor(ImGuiCol_Text, changed ? ManifoldTheme::Text : ManifoldTheme::Label);
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, ImVec2(limit, at.y + size.y), limit, title.c_str(), nullptr, &size);
            ImGui::PopStyleColor();
        }
        ItemTooltip((TitleCase(prop.Key) + " (" + std::string(prop.Key) + "): " + std::string(prop.Help) + (changed ? "\nChanged from the default." : "")).c_str());
        if (resettable)
        {
            ImGui::SameLine(std::max(0.0f, cellRight - side));
            if (IconButton(ICON_FA_UNDO, "Back to the default", false, side))
            {
                Remember();
                Lua::FormSetProp(node, prop.Key, Lua::FormPropDefault(node.Kind, prop.Key));
                if (prop.Type == FormPropType::File)
                    m_badImages.clear();
                Changed();
            }
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string before = Lua::FormPropValue(node, prop.Key);
        std::string value = before;
        bool edited = false;
        const bool choice = (node.Kind == UiKind::Combo || node.Kind == UiKind::Radio) && prop.Key == "value";
        if (choice && node.Kind == UiKind::Combo && node.Multi)
        {
            // ∑ the chosen items by name, ticked in a list that stays open
            std::string preview;
            for (const int item : node.Chosen)
            {
                if (item >= 1 && item <= static_cast<int>(node.Items.size()))
                    preview += (preview.empty() ? "" : ", ") + node.Items[static_cast<std::size_t>(item - 1)];
            }
            if (ImGui::BeginCombo("##v", preview.empty() ? "None" : preview.c_str()))
            {
                std::vector<int> chosen = node.Chosen;
                for (int i = 0; i < static_cast<int>(node.Items.size()); ++i)
                {
                    ImGui::PushID(i);
                    bool on = std::find(chosen.begin(), chosen.end(), i + 1) != chosen.end();
                    if (ImGui::Checkbox(node.Items[static_cast<std::size_t>(i)].c_str(), &on))
                    {
                        if (on)
                            chosen.push_back(i + 1);
                        else
                            std::erase(chosen, i + 1);
                        std::sort(chosen.begin(), chosen.end());
                        value.clear();
                        for (const int item : chosen)
                            value += (value.empty() ? "" : ", ") + std::to_string(item);
                        edited = true;
                    }
                    ImGui::PopID();
                }
                if (node.Items.empty())
                    Muted("Add items first.");
                ImGui::EndCombo();
            }
        }
        else if (choice)
        {
            // ∑ the chosen item by name instead of its number
            const int selected = node.Selected;
            const char* preview = selected >= 1 && selected <= static_cast<int>(node.Items.size()) ? node.Items[static_cast<std::size_t>(selected - 1)].c_str() : "None";
            if (ImGui::BeginCombo("##v", preview))
            {
                if (ImGui::Selectable("None", selected == 0))
                {
                    value = "0";
                    edited = true;
                }
                for (int i = 0; i < static_cast<int>(node.Items.size()); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(node.Items[static_cast<std::size_t>(i)].c_str(), selected == i + 1))
                    {
                        value = std::to_string(i + 1);
                        edited = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        else
        switch (prop.Type)
        {
        case FormPropType::Bool:
        {
            bool flag = value == "true";
            if (ToggleSwitch("##v", &flag))
            {
                value = flag ? "true" : "false";
                edited = true;
            }
            break;
        }
        case FormPropType::Integer:
        {
            int number = std::atoi(value.c_str());
            if (ImGui::InputInt("##v", &number))
            {
                value = std::to_string(number);
                edited = true;
            }
            break;
        }
        case FormPropType::Choice:
        {
            if (ImGui::BeginCombo("##v", value.c_str()))
            {
                std::string_view choices = prop.Choices;
                while (!choices.empty())
                {
                    const std::size_t bar = choices.find('|');
                    const std::string choice(choices.substr(0, bar));
                    if (ImGui::Selectable(choice.c_str(), choice == value))
                    {
                        value = choice;
                        edited = true;
                    }
                    choices = bar == std::string_view::npos ? std::string_view() : choices.substr(bar + 1);
                }
                ImGui::EndCombo();
            }
            break;
        }
        case FormPropType::Text:
        case FormPropType::Items:
        case FormPropType::Columns:
        case FormPropType::Rows:
        case FormPropType::Tree:
        {
            const int lines = prop.Type == FormPropType::Text ? 3 : 5;
            edited = InputMultiline("##v", value, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * lines + ImGui::GetStyle().FramePadding.y * 2.0f));
            if (prop.Type != FormPropType::Text)
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                ImGui::TextUnformatted(prop.Help.data(), prop.Help.data() + prop.Help.size());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
            break;
        }
        case FormPropType::Color:
        {
            const float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
            edited = InputTextWithHint("##v", "theme colour or #RRGGBB", value);
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            const ImVec4 swatch = UiHost::Color(value, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            if (ImGui::ColorButton("##swatch", swatch, ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(button, button)))
                ImGui::OpenPopup("##colors");
            if (ImGui::BeginPopup("##colors"))
            {
                SectionLabel("Theme colours follow the user's theme");
                if (ImGui::BeginChild("##themecolors", ImVec2(Px(220.0f), Px(220.0f))))
                {
                    for (const ManifoldTheme::PaletteColor& entry : ManifoldTheme::kPalette)
                    {
                        const std::string name = ThemeName(entry.Name);
                        ImGui::ColorButton(("##c" + name).c_str(), *entry.Value, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
                        ImGui::SameLine();
                        if (ImGui::Selectable(name.c_str(), EqualsNoCase(name, value)))
                        {
                            value = name;
                            edited = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                float channels[4] = { swatch.x, swatch.y, swatch.z, swatch.w > 0.0f ? swatch.w : 1.0f };
                if (ImGui::ColorPicker4("##pick", channels, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
                {
                    const auto byte = [](float c) { return static_cast<unsigned>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    char text[16];
                    if (byte(channels[3]) == 255)
                        std::snprintf(text, sizeof(text), "#%02X%02X%02X", byte(channels[0]), byte(channels[1]), byte(channels[2]));
                    else
                        std::snprintf(text, sizeof(text), "#%02X%02X%02X%02X", byte(channels[0]), byte(channels[1]), byte(channels[2]), byte(channels[3]));
                    value = text;
                    edited = true;
                }
                ImGui::EndPopup();
            }
            break;
        }
        case FormPropType::Icon:
        {
            const float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
            edited = InputTextWithHint("##v", "icon name", value);
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            const std::string_view glyph = IconGlyph(value);
            const std::string face = (glyph.empty() ? std::string(ICON_FA_ICONS) : std::string(glyph)) + "##pickicon";
            if (ImGui::Button(face.c_str(), ImVec2(button, button)))
            {
                m_iconFilter.clear();
                ImGui::OpenPopup("##icons");
            }
            if (ImGui::BeginPopup("##icons"))
            {
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                ImGui::SetNextItemWidth(Px(260.0f));
                InputTextWithHint("##filter", "Filter", m_iconFilter);
                const std::string filter = Lower(m_iconFilter);
                std::vector<const IconName*> shown;
                for (const IconName& icon : IconNames())
                {
                    if (filter.empty() || icon.Name.find(filter) != std::string_view::npos)
                        shown.push_back(&icon);
                }
                if (ImGui::Selectable("(none)", value.empty()))
                {
                    value.clear();
                    edited = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::BeginChild("##iconlist", ImVec2(Px(260.0f), Px(260.0f)));
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(shown.size()));
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        const IconName& icon = *shown[static_cast<std::size_t>(i)];
                        const std::string label = std::string(icon.Glyph) + "  " + std::string(icon.Name);
                        if (ImGui::Selectable(label.c_str(), icon.Name == value))
                        {
                            value = std::string(icon.Name);
                            edited = true;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
            break;
        }
        case FormPropType::Number:
            if ((prop.Key == "width" || prop.Key == "height") && node.Kind != UiKind::Window)
                edited = SizeControl(node, prop.Key, value);
            else
                edited = InputTextWithHint("##v", prop.Key == "min" || prop.Key == "max" ? "none" : "0", value, ImGuiInputTextFlags_CharsScientific);
            break;
        case FormPropType::File:
        {
            const float button = ImGui::GetFrameHeight();
            ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
            edited = InputTextWithHint("##v", "picture file", value);
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (IconButton(ICON_FA_FOLDER_OPEN, "Choose a picture", false, button))
            {
                const std::filesystem::path folder = m_path.empty() ? Paths::ProgramFolder() : m_path.parent_path();
                if (const auto chosen = m_workspace.OpenFileDialog({ { L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif)", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif" }, { L"All files", L"*.*" } }, folder))
                {
                    std::error_code ignored;
                    std::filesystem::path relative = std::filesystem::relative(*chosen, folder, ignored);
                    const std::wstring text = relative.wstring();
                    value = WideToUtf8(!ignored && !relative.empty() && text.rfind(L"..", 0) != 0 ? relative.generic_wstring() : chosen->wstring());
                    edited = true;
                }
            }
            if (!value.empty() && !std::filesystem::exists(ResolveImage(value)))
                TextWithError("Not found next to the form file or the program", ManifoldTheme::Warning);
            break;
        }
        case FormPropType::String:
            edited = InputText("##v", value);
            break;
        }
        if (edited && value != before)
        {
            Remember(ImGui::GetID("##v"));
            Lua::FormSetProp(node, prop.Key, value);
            if (prop.Type == FormPropType::File)
                m_badImages.clear();
            Changed();
        }
        ImGui::PopID();
    }

    // ∑ Width and height as a choice instead of a number to know: Auto
    // (the usual size), Fill (the rest of the line or of the height) and
    // Fixed, which starts from the size the widget has in the preview.
    bool FormEditorWindow::SizeControl(UiNode& node, std::string_view key, std::string& value)
    {
        const bool height = key == "height";
        static constexpr UiKind kFillHeight[] = { UiKind::Table, UiKind::Tree, UiKind::Canvas, UiKind::Column, UiKind::Tab, UiKind::Section, UiKind::Input };
        const bool canFill = !height || std::find(std::begin(kFillHeight), std::end(kFillHeight), node.Kind) != std::end(kFillHeight);
        const double number = std::atof(value.c_str());
        enum { Auto, Fill, Fixed };
        const int mode = number > 0.0 ? Fixed : number < 0.0 && canFill ? Fill : Auto;
        std::vector<const char*> options = { "Auto" };
        if (canFill)
            options.push_back("Fill");
        options.push_back("Fixed");
        int shown = mode == Fixed ? static_cast<int>(options.size()) - 1 : mode == Fill ? 1 : 0;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float segment = mode == Fixed ? std::max(Px(120.0f), avail * 0.6f) : avail;
        bool edited = false;
        if (Segmented("##size", &shown, options, std::min(segment, avail)))
        {
            const bool fixed = shown == static_cast<int>(options.size()) - 1;
            if (fixed && mode != Fixed)
            {
                const UiDesign::Rect* rect = RectOf(node.Id);
                const float pixels = rect ? (height ? rect->Max.y - rect->Min.y : rect->Max.x - rect->Min.x) : 0.0f;
                const int points = static_cast<int>(std::lround(pixels / std::max(Px(1.0f), 0.01f)));
                value = std::to_string(points > 0 ? points : 100);
            }
            else if (!fixed)
                value = shown == 1 && canFill ? "-1" : "0";
            edited = true;
        }
        ItemTooltip(height ? "Auto: the usual height. Fill: the height that is left in the window (lists, tables, canvases, scrolling columns, multi-line fields). Fixed: this many points."
                           : "Auto: the usual width (fields, bars and lists take the rest of the line, buttons fit their text). Fill: the rest of the line, or the widget's share of a row. Fixed: this many points.");
        if (mode == Fixed)
        {
            // beside the choice where there is room, else below it
            if (avail - segment > Px(40.0f))
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(-FLT_MIN);
            edited = InputTextWithHint("##v", "points", value, ImGuiInputTextFlags_CharsScientific) || edited;
        }
        return edited;
    }

    void FormEditorWindow::HandlerRow(UiNode& node, const Lua::FormHandler& handler)
    {
        ImGui::PushID(handler.Key.data(), handler.Key.data() + handler.Key.size());
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(handler.Key.data(), handler.Key.data() + handler.Key.size());
        ItemTooltip((std::string(handler.Help) + "\nThe function gets (" + std::string(handler.Params) + ").").c_str());
        ImGui::TableSetColumnIndex(1);
        const float button = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(-(button * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f));
        const std::string key(handler.Key);
        std::string value;
        if (const auto meta = m_form.Meta.find(node.Id); meta != m_form.Meta.end())
        {
            if (const auto found = meta->second.Handlers.find(key); found != meta->second.Handlers.end())
                value = found->second;
        }
        const std::string before = value;
        bool edited = InputTextWithHint("##fn", "function name", value, ImGuiInputTextFlags_CharsNoBlank);
        const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (IconButton(ICON_FA_MAGIC, "Name the handler after the widget", false, button))
        {
            std::string base = m_form.Meta.count(node.Id) ? m_form.Meta[node.Id].Name : std::string();
            if (base.empty())
                base = node.Kind == UiKind::Window ? std::string("window") : std::string(Lua::UiKindName(node.Kind));
            value = base + key.substr(2);
            edited = true;
        }
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        const std::filesystem::path script = LinkedScript();
        const std::string goTip = script.empty() ? std::string("Write the function: the editor asks where the script goes, links it to the form and opens it there")
                                                 : "Open the function in " + WideToUtf8(script.filename().wstring()) + "; a missing one is added first (also on a double click on the name)";
        const bool open = IconButton(ICON_FA_EXTERNAL_LINK_ALT, goTip.c_str(), false, button);
        if (edited && value != before)
        {
            Remember(ImGui::GetID("##fn"));
            if (value.empty())
                m_form.Meta[node.Id].Handlers.erase(key);
            else
                m_form.Meta[node.Id].Handlers[key] = value;
            Changed();
        }
        if (open || doubleClicked)
            OpenHandler(node, handler);
        ImGui::PopID();
    }

    void FormEditorWindow::Properties()
    {
        UiNode* node = m_form.Model.Find(m_selected);
        if (!node)
        {
            m_selected = m_form.Window;
            node = m_form.Model.Find(m_selected);
            if (!node)
                return;
        }
        const bool modalWindow = Lua::FormIsModal(m_form, node->Id);
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        if (modalWindow)
            ImGui::TextUnformatted(ICON_FA_WINDOW_RESTORE "  Modal Form");
        else
            ImGui::Text("%s  %s", KindIcon(node->Kind), std::string(Lua::FormKindTitle(node->Kind)).c_str());
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        Muted(modalWindow ? std::string_view("A dialog in front of the program that blocks it until it is closed. A button opens it with Opens; the script with modals.<name>.show().")
                          : Lua::FormKindHelp(node->Kind));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::BeginChild("##props", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        ImGui::PushID(node->Id);
        // ∑ the name column is as wide as this widget's longest option name
        // plus the reset button, within 45 % of the panel
        float labels = Px(118.0f);
        {
            float longest = 0.0f;
            for (const Lua::FormProp& prop : Lua::FormProps(node->Kind))
                longest = std::max(longest, ImGui::CalcTextSize(TitleCase(prop.Key).c_str()).x);
            for (const Lua::FormHandler& handler : Lua::FormHandlers(node->Kind))
                longest = std::max(longest, ImGui::CalcTextSize(std::string(handler.Key).c_str()).x);
            const float wanted = longest + Px(10.0f) + ImGui::GetFrameHeight() * 0.8f + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::GetStyle().CellPadding.x * 2.0f;
            labels = std::clamp(wanted, labels, std::max(labels, ImGui::GetContentRegionAvail().x * 0.45f));
        }
        const auto beginTable = [&](const char* id) {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
                return false;
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labels);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        };
        if (beginTable("##identity"))
        {
            NameRow(*node);
            ImGui::EndTable();
        }
        for (const int group : { 0, 4, 1, 2, 3 })
        {
            std::vector<const Lua::FormProp*> props;
            int changed = 0;
            for (const Lua::FormProp& prop : Lua::FormProps(node->Kind))
            {
                if (GroupOf(prop.Key, node->Kind) != group)
                    continue;
                if (modalWindow && (prop.Key == "dock" || prop.Key == "topmost" || prop.Key == "position" || prop.Key == "open"))
                    continue;
                props.push_back(&prop);
                if (!Lua::FormPropIsDefault(*node, prop.Key))
                    ++changed;
            }
            if (props.empty())
                continue;
            ImGui::Spacing();
            const std::string header = std::string(kGroupTitles[group]) + (changed ? "  \xC2\xB7  " + std::to_string(changed) + " changed" : std::string()) + "###group" + std::to_string(group);
            if (!GroupHeader(header.c_str(), group != 3))
                continue;
            if (beginTable("##groupprops"))
            {
                for (const Lua::FormProp* prop : props)
                    PropertyRow(*node, *prop);
                ImGui::EndTable();
            }
        }
        const std::span<const Lua::FormHandler> handlers = Lua::FormHandlers(node->Kind);
        if (!handlers.empty())
        {
            ImGui::Spacing();
            if (GroupHeader(ICON_FA_BOLT "  Handlers###handlers", true) && ImGui::BeginTable("##handlertable", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
            {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labels);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                for (const Lua::FormHandler& handler : handlers)
                    HandlerRow(*node, handler);
                ImGui::EndTable();
            }
        }
        if (node->Kind == UiKind::Button)
        {
            ImGui::Spacing();
            if (GroupHeader(ICON_FA_WINDOW_RESTORE "  Opens and Closes###jumps", true) && beginTable("##jumptable"))
            {
                Lua::FormMeta& meta = m_form.Meta[node->Id];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Opens");
                ItemTooltip("opens: the modal form this button shows when clicked (before its onClick).");
                ImGui::TableSetColumnIndex(1);
                const float button = ImGui::GetFrameHeight();
                ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
                if (ImGui::BeginCombo("##opens", meta.Opens.empty() ? "nothing" : meta.Opens.c_str()))
                {
                    if (ImGui::Selectable("nothing", meta.Opens.empty()) && !meta.Opens.empty())
                    {
                        Remember();
                        m_form.Meta[node->Id].Opens.clear();
                        Changed();
                    }
                    for (const int modal : m_form.Modals)
                    {
                        const std::string name = Lua::FormModalName(m_form, modal);
                        const UiNode* window = m_form.Model.Find(modal);
                        const std::string label = name + (window && !window->Title.empty() ? "  \xC2\xB7  " + window->Title : std::string());
                        if (ImGui::Selectable(label.c_str(), meta.Opens == name) && meta.Opens != name)
                        {
                            Remember();
                            m_form.Meta[node->Id].Opens = name;
                            Changed();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                if (IconButton(ICON_FA_PLUS "##newmodal", "A new modal form that this button opens", false, button))
                    AddModal(node->Id);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Closes");
                ItemTooltip("closes: hides the window this button is in, like OK or Cancel in a dialog.");
                ImGui::TableSetColumnIndex(1);
                bool closes = m_form.Meta[node->Id].Closes;
                if (ToggleSwitch("##closes", &closes))
                {
                    Remember();
                    m_form.Meta[node->Id].Closes = closes;
                    Changed();
                }
                ImGui::EndTable();
            }
        }
        if (Lua::FormCanBind(node->Kind) && !(node->Kind == UiKind::Combo && node->Multi))
        {
            // ∑ record = ...: the widget shows and changes a memory record of the table
            ImGui::Spacing();
            const auto found = m_form.Meta.find(node->Id);
            const std::string current = found == m_form.Meta.end() ? std::string() : found->second.Record;
            const bool byId = found != m_form.Meta.end() && found->second.RecordById;
            const std::string header = std::string(ICON_FA_LINK "  Memory Record") + (current.empty() ? "" : "  \xC2\xB7  " + (byId ? "#" + current : current)) + "###memoryrecord";
            const auto store = [&](const std::string& record, bool id) {
                if (record == current && id == byId)
                    return;
                Remember();
                m_form.Meta[node->Id].Record = record;
                m_form.Meta[node->Id].RecordById = id && !record.empty();
                Changed();
            };
            if (GroupHeader(header.c_str(), !current.empty()) && beginTable("##recordtable"))
            {
                const bool wantsValue = node->Kind != UiKind::Checkbox && node->Kind != UiKind::Button;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Record");
                ItemTooltip(node->Kind == UiKind::Checkbox ? "record: the box shows whether this memory record is on and switches it, like its box in the address list. No code needed."
                            : node->Kind == UiKind::Button   ? "record: a click switches this memory record on or off. No code needed."
                            : node->Kind == UiKind::Text     ? "record: the text shows this memory record's value, as the address list shows it."
                            : node->Kind == UiKind::Input    ? "record: the field shows this memory record's value and writes it when Enter is pressed."
                            : node->Kind == UiKind::Combo || node->Kind == UiKind::Radio ? "record: the chosen item's text is written to this memory record (a drop-down entry of it), and the item that matches its value is chosen."
                                                             : "record: the widget shows this memory record's value and writes it when it is changed.");
                ImGui::TableSetColumnIndex(1);
                const float button = ImGui::GetFrameHeight();
                ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
                std::string text = byId ? "#" + current : current;
                if (InputTextWithHint("##record", "description, or #id", text))
                {
                    const bool id = text.size() > 1 && text[0] == '#' && text.find_first_not_of("0123456789", 1) == std::string::npos;
                    store(id ? text.substr(1) : text, id);
                }
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                if (IconButton(ICON_FA_LIST "##pickrecord", "Pick a record of the open table", false, button))
                {
                    m_recordFilter.clear();
                    ImGui::OpenPopup("##records");
                }
                if (ImGui::BeginPopup("##records"))
                {
                    ImGui::SetNextItemWidth(Px(280.0f));
                    if (ImGui::IsWindowAppearing())
                        ImGui::SetKeyboardFocusHere();
                    InputTextWithHint("##recordfilter", ICON_FA_SEARCH " Filter", m_recordFilter);
                    ImGui::BeginChild("##recordlist", ImVec2(Px(280.0f), Px(260.0f)));
                    int shown = 0;
                    for (Table::MemoryRecord* record : m_engine.Table().Flatten())
                    {
                        if (wantsValue && record->Kind != Table::RecordKind::Value)
                            continue;
                        if (!m_recordFilter.empty() && !ContainsNoCase(record->Description, m_recordFilter))
                            continue;
                        int depth = 0;
                        for (const Table::MemoryRecord* up = record->Parent; up; up = up->Parent)
                            ++depth;
                        const char* icon = record->Kind == Table::RecordKind::Script ? ICON_FA_CODE : record->Kind == Table::RecordKind::Group ? ICON_FA_FOLDER : ICON_FA_HASHTAG;
                        // ∑ a description binds only when it names exactly one record;
                        // a shared or empty one binds by id instead
                        const int twins = record->Description.empty() ? 0 : m_engine.Table().CountByDescription(record->Description);
                        const bool unique = twins == 1;
                        ImGui::PushID(record->Id);
                        ImGui::Indent(Px(12.0f) * static_cast<float>(depth));
                        const std::string label = std::string(icon) + "  " + (record->Description.empty() ? "(no description)" : record->Description) +
                                                  (unique ? std::string() : "  \xC2\xB7  #" + std::to_string(record->Id));
                        const bool chosen = byId ? current == std::to_string(record->Id) : unique && record->Description == current;
                        if (ImGui::Selectable(label.c_str(), chosen))
                        {
                            if (unique)
                                store(record->Description, false);
                            else
                                store(std::to_string(record->Id), true);
                            ImGui::CloseCurrentPopup();
                        }
                        if (!unique && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                            ImGui::SetTooltip("%s", twins > 1 ? (std::to_string(twins) + " records have this description, so this one is bound by its id.").c_str()
                                                              : "Without a description the record is bound by its id.");
                        ImGui::Unindent(Px(12.0f) * static_cast<float>(depth));
                        ImGui::PopID();
                        ++shown;
                    }
                    if (shown == 0)
                        Muted(m_engine.Table().Count() == 0 ? "No table is open, or it has no records." : wantsValue ? "No value record matches." : "No record matches.");
                    ImGui::EndChild();
                    ImGui::EndPopup();
                }
                if (!current.empty())
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(1);
                    Table::MemoryRecord* record = byId ? m_engine.Table().FindById(std::atoi(current.c_str())) : m_engine.Table().FindByDescription(current);
                    const int twins = byId || !record ? 0 : m_engine.Table().CountByDescription(current);
                    if (twins > 1)
                    {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextColored(ManifoldTheme::Warning, "%s", (std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + std::to_string(twins) +
                            " records have this description, so it does not say which one is meant: the widget stays disabled. Pick the record from the list to bind it by its id, or give the records distinct descriptions.").c_str());
                        ImGui::PopTextWrapPos();
                    }
                    else if (record)
                        Muted(std::string(record->Kind == Table::RecordKind::Script ? "A script" : record->Kind == Table::RecordKind::Group ? "A group" : "A value") + " of the open table" +
                              (wantsValue && record->Kind != Table::RecordKind::Value ? ": it has no value to show" : ""));
                    else
                        Muted(m_engine.Table().Count() == 0 ? "No table is open; the record is looked up when the form runs." : "Not in the open table; the widget stays disabled until it is.");
                    ImGui::SameLine();
                    if (ImGui::SmallButton(ICON_FA_TIMES "##unbind"))
                        store({}, false);
                    ItemTooltip("Unbind the widget");
                }
                ImGui::EndTable();
            }
        }
        if (node->Kind != UiKind::Window)
        {
            // ∑ enabledWhen without typing: the widget to follow, then what of it
            ImGui::Spacing();
            const auto found = m_form.Meta.find(node->Id);
            const std::string current = found == m_form.Meta.end() ? std::string() : found->second.EnabledWhen;
            Lua::FormRule rule = Lua::FormParseWhen(current).value_or(Lua::FormRule{});
            const UiNode* source = m_form.Model.Find(Lua::FormFindNamed(m_form, rule.Source));
            const std::string header = std::string(ICON_FA_TOGGLE_ON "  Enabled When") + (current.empty() ? "" : "  \xC2\xB7  " + current) + "###enabledwhen";
            const auto store = [&](const Lua::FormRule& changed) {
                const std::string text = Lua::FormWhenText(changed);
                if (text == current)
                    return;
                Remember();
                m_form.Meta[node->Id].EnabledWhen = text;
                Changed();
            };
            if (GroupHeader(header.c_str(), !current.empty()) && beginTable("##whentable"))
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Follows");
                ItemTooltip("enabledWhen: the widget is enabled only while this checkbox, switch, combo or radio says so; otherwise it is drawn dimmed and takes no input. No code needed.");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                const std::string preview = rule.Source.empty() ? std::string("nothing (always enabled)") : rule.Source;
                if (ImGui::BeginCombo("##whensource", preview.c_str()))
                {
                    if (ImGui::Selectable("nothing (always enabled)", rule.Source.empty()))
                        store(Lua::FormRule{});
                    for (const auto& [id, meta] : m_form.Meta)
                    {
                        const UiNode* item = m_form.Model.Find(id);
                        if (!item || id == node->Id || meta.Name.empty() ||
                            (item->Kind != UiKind::Checkbox && item->Kind != UiKind::Combo && item->Kind != UiKind::Radio))
                            continue;
                        const std::string label = std::string(KindIcon(item->Kind)) + "  " + meta.Name;
                        if (ImGui::Selectable(label.c_str(), meta.Name == rule.Source))
                            store(Lua::FormRule{ meta.Name, false, {} });
                    }
                    ImGui::EndCombo();
                }
                if (source && source->Kind == UiKind::Checkbox)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("While");
                    ImGui::TableSetColumnIndex(1);
                    int state = rule.Negate ? 1 : 0;
                    if (Segmented("##whenstate", &state, { "On", "Off" }, -1.0f))
                    {
                        Lua::FormRule changed = rule;
                        changed.Negate = state == 1;
                        store(changed);
                    }
                }
                else if (source)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Chosen");
                    ItemTooltip("Enabled while one of the ticked items is chosen; with none ticked, while any item is chosen.");
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string chosen;
                    for (const std::string& value : rule.Values)
                        chosen += (chosen.empty() ? "" : ", ") + value;
                    if (ImGui::BeginCombo("##whenvalues", chosen.empty() ? "any item" : chosen.c_str()))
                    {
                        for (std::size_t i = 0; i < source->Items.size(); ++i)
                        {
                            ImGui::PushID(static_cast<int>(i));
                            const std::string& item = source->Items[i];
                            bool on = std::find(rule.Values.begin(), rule.Values.end(), item) != rule.Values.end();
                            if (ImGui::Checkbox(item.c_str(), &on))
                            {
                                Lua::FormRule changed = rule;
                                changed.Negate = false;
                                std::erase(changed.Values, item);
                                if (on)
                                {
                                    // keep the items' own order
                                    std::vector<std::string> ordered;
                                    for (const std::string& each : source->Items)
                                    {
                                        if (each == item || std::find(changed.Values.begin(), changed.Values.end(), each) != changed.Values.end())
                                            ordered.push_back(each);
                                    }
                                    changed.Values = ordered;
                                }
                                store(changed);
                            }
                            ImGui::PopID();
                        }
                        if (source->Items.empty())
                            Muted("That widget has no items yet.");
                        ImGui::EndCombo();
                    }
                }
                ImGui::EndTable();
            }
        }
        bool any = false;
        const std::vector<Lua::FormProblem> problems = m_problems; // a fix below changes the form
        const int nodeId = node->Id;
        for (const Lua::FormProblem& problem : problems)
        {
            if (problem.Node != nodeId)
                continue;
            if (!any)
            {
                ImGui::Spacing();
                SectionLabel("Problems");
                any = true;
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            ImGui::TextUnformatted((std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + problem.Text).c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::PushID(&problem);
            FixLink(problem);
            ImGui::PopID();
        }
        if (!ImGui::IsAnyItemActive())
            m_lastSource = 0;
        ImGui::PopID();
        ImGui::EndChild();
    }

    void FormEditorWindow::CodePane(float height)
    {
        ImGui::BeginChild("##code", ImVec2(0.0f, height), ImGuiChildFlags_None);
        const std::filesystem::path script = LinkedScript();
        const std::string scriptTab = std::string(ICON_FA_FILE_CODE "  ") + (script.empty() ? std::string("Script") : WideToUtf8(script.filename().wstring())) + (ScriptDirty() ? " *" : "");
        const std::string first = scriptTab;
        Segmented("##codetab", &m_codeTab, { first.c_str(), ICON_FA_CODE "  Generated" }, Px(330.0f));
        if (m_codeTab == 0)
            ScriptPane();
        else
        {
            ImGui::SameLine();
            Segmented("##style", &m_style, { "Standalone", "Script + Form File", "One File" }, Px(360.0f));
            ItemTooltip("Standalone: one script that builds the window with manifold.ui calls.\nScript + Form File: the layout in a .form.lua file that the script loads with manifold.ui.form.load (the older way).\nOne File: the script holds the form as a table and builds it with manifold.ui.form.build; this is how the Form Editor saves.");
            ImGui::SameLine();
            if (IconButton(ICON_FA_COPY, "Copy the generated code"))
                ImGui::SetClipboardText(Code().c_str());
            ImGui::SameLine();
            if (IconButton(ICON_FA_FILE_EXPORT, m_oneFile ? "Export Standalone Script... (plain manifold.ui calls, no form table)" : "Export Script... (writes or updates a script file)"))
                Export();
            std::string code = Code();
            CodeEditor("##formcode", code, Language::Lua, ImVec2(0.0f, 0.0f), m_codeEditor);
        }
        ImGui::EndChild();
    }

    void FormEditorWindow::ScriptPane()
    {
        const std::filesystem::path script = LinkedScript();
        if (script.empty() && !m_oneFile)
        {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            Muted("The handlers' code lives in a Lua script linked to this form. Create one (it loads the form and holds a function for every handler), or link a script you already have.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (AccentButton(ICON_FA_FILE_EXPORT "  Create Script..."))
            {
                m_style = 1;
                Export();
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_LINK "  Link Script..."))
                LinkScript();
            return;
        }
        EnsureScript();
        ImGui::SameLine();
        if (IconButton(ICON_FA_PLUS, "Add a function for every handler the script does not have yet"))
            AddFunctions();
        ImGui::SameLine();
        if (IconButton(ICON_FA_EXTERNAL_LINK_ALT, "Open the script in a script window (saves it first)"))
        {
            int line = m_scriptEditor.CursorLine;
            if (m_oneFile)
            {
                // the window shows the form's table written out, the pane one line for it
                const std::size_t standIn = m_form.ScriptText.find(Lua::kFormStandIn);
                const int at = standIn == std::string::npos ? 0 : 1 + static_cast<int>(std::count(m_form.ScriptText.begin(), m_form.ScriptText.begin() + static_cast<std::ptrdiff_t>(standIn), '\n'));
                if (at > 0 && line > at)
                {
                    const std::string block = Lua::FormBlockText(m_form, "form");
                    line += static_cast<int>(std::count(block.begin(), block.end(), '\n')) - 1;
                }
            }
            else
                SaveScript();
            OpenLinked(line);
        }
        if (m_scriptConflict)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_EXCLAMATION_TRIANGLE "  Changed on disk too; saving keeps this version");
        }
        else if (m_oneFile && ImGui::GetTime() < m_lockNoteUntil)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_LOCK "  The form's line stays");
            ItemTooltip("The line \"local form = { ... }\" stands for the form on the canvas, so it cannot be removed here. Change the form on the canvas; saving writes its table into the script.");
        }
        else if (m_oneFile)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            Muted(ICON_FA_INFO_CIRCLE "  One file");
            ItemTooltip("Form and handlers live in this one script. The line \"local form = { ... }\" stands for the form on the canvas; saving writes the form's table there, and manifold.ui.form.build makes the window from it.");
        }
        std::string text = m_form.ScriptText;
        if (CodeEditor("##scriptcode", text, Language::Lua, ImVec2(0.0f, 0.0f), m_scriptEditor) && text != m_form.ScriptText)
        {
            // ∑ one file: the form's line is locked; an edit that removes or
            // doubles it is dropped (the editor shows the old text again)
            if (m_oneFile && !Lua::FormViewIntact(text))
                m_lockNoteUntil = ImGui::GetTime() + 4.0;
            else
            {
                Remember(ImGui::GetID("##scriptcode"));
                m_form.ScriptText = std::move(text);
            }
        }
    }

    void FormEditorWindow::Popups()
    {
        if (m_unsavedDialog.Begin())
        {
            ImGui::TextUnformatted("Save the changes to the form?");
            ImGui::Spacing();
            int choice = 0;
            if (AccentButton("Save", ImVec2(Px(90.0f), 0.0f)))
                choice = 1;
            ImGui::SameLine();
            if (ImGui::Button("Discard", ImVec2(Px(90.0f), 0.0f)))
                choice = 2;
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(Px(90.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                choice = 3;
            if (choice)
                m_unsavedDialog.Close();
            m_unsavedDialog.End();
            if (choice == 1 && Save(false))
                DoPending();
            else if (choice == 2)
            {
                m_dirty = false;
                m_form.ScriptLoaded = false;
                DoPending();
            }
            else if (choice == 3)
                m_pending = Pending::None;
        }
        if (m_importDialog.Begin())
        {
            ImGui::TextWrapped("%s holds %d forms. Which one?", m_importSource.c_str(), static_cast<int>(m_importChoices.size()));
            ImGui::BeginChild("##importlist", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - Px(8.0f)), ImGuiChildFlags_Borders);
            for (int i = 0; i < static_cast<int>(m_importChoices.size()); ++i)
            {
                const Table::CeFormEntry& entry = m_importChoices[static_cast<std::size_t>(i)];
                std::string label = entry.Name;
                if (entry.Root)
                    label += "  \"" + entry.Root->Text("Caption") + "\"  " + std::to_string(entry.Root->Children.size()) + " controls";
                else
                    label += "  (" + entry.Error + ")";
                ImGui::PushID(i);
                ImGui::BeginDisabled(!entry.Root);
                if (ImGui::Selectable(label.c_str(), m_importChoice == i, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_importChoice = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        const Table::CeFormEntry chosen = entry;
                        m_importDialog.Close();
                        ImportEntry(chosen);
                    }
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndChild();
            const bool valid = m_importChoice >= 0 && m_importChoice < static_cast<int>(m_importChoices.size()) && m_importChoices[static_cast<std::size_t>(m_importChoice)].Root;
            const DialogChoice choice = m_importDialog.Buttons("Import", valid);
            if (choice == DialogChoice::Ok && valid)
            {
                const Table::CeFormEntry chosen = m_importChoices[static_cast<std::size_t>(m_importChoice)];
                m_importDialog.Close();
                ImportEntry(chosen);
            }
            else if (choice == DialogChoice::Cancel)
                m_importDialog.Close();
            m_importDialog.End();
        }
        if (m_notesDialog.Begin())
        {
            ImGui::TextWrapped("%d controls were converted. Pixel positions became rows, grids and splits, fonts and colours follow the theme, "
                               "and event names became handler names (Export Script writes their stubs). Cheat Engine scripts reach a control as "
                               "Form_Control or Form.Control; a script that loads this form reaches it as w.<name>.", m_importConverted);
            ImGui::Spacing();
            ImGui::BeginChild("##notes", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - Px(8.0f)), ImGuiChildFlags_Borders);
            if (m_importNotes.empty())
                Muted("Nothing was left out.");
            for (const std::string& note : m_importNotes)
            {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", note.c_str());
            }
            ImGui::EndChild();
            if (m_notesDialog.Buttons("Close", true, true, nullptr) != DialogChoice::None)
                m_notesDialog.Close();
            m_notesDialog.End();
        }
        if (m_problemsDialog.Begin())
        {
            // the same list the toolbar counts (the editor adds checks against the open table)
            const std::vector<Lua::FormProblem> problems = m_problems;
            ImGui::BeginChild("##problemlist", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - Px(8.0f)), ImGuiChildFlags_Borders);
            if (problems.empty())
                Muted("No problems.");
            bool picked = false;
            for (std::size_t i = 0; i < problems.size(); ++i)
            {
                // wrapped, so a long text is read in full; a click shows the widget
                ImGui::PushID(static_cast<int>(i));
                const std::string text = std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + problems[i].Text;
                const float width = ImGui::GetContentRegionAvail().x;
                const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, width);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##problem", false, 0, ImVec2(width, size.y)))
                {
                    Select(problems[i].Node);
                    picked = true;
                }
                ItemTooltip("Click to select the widget");
                ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, ImGui::GetColorU32(ManifoldTheme::Warning), text.c_str(), nullptr, width);
                FixLink(problems[i]);
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (picked)
                m_problemsDialog.Close();
            if (m_problemsDialog.Buttons("Close", true, true, nullptr) != DialogChoice::None)
                m_problemsDialog.Close();
            m_problemsDialog.End();
        }
    }

    std::filesystem::path FormEditorWindow::ResolveImage(const std::string& source) const
    {
        std::filesystem::path file(Utf8ToWide(source));
        if (file.is_relative())
            file = (m_path.empty() ? Paths::ProgramFolder() : m_path.parent_path()) / file;
        return file;
    }

    void FormEditorWindow::SyncImages(Lua::FormDocument& form)
    {
        std::function<void(int)> visit = [&](int id) {
            UiNode* node = form.Model.Find(id);
            if (!node)
                return;
            if (node->Kind == UiKind::Image)
            {
                const std::string resolved = node->Source.empty() ? std::string() : WideToUtf8(ResolveImage(node->Source).wstring());
                const Lua::UiImage* have = form.Model.FindImage(node->ImageId);
                if (resolved.empty())
                    node->ImageId = 0;
                else if (!have || have->Path != resolved)
                {
                    node->ImageId = 0;
                    if (!m_badImages.count(resolved))
                    {
                        std::ifstream file(ResolveImage(node->Source), std::ios::binary);
                        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                        std::optional<DecodedImage> decoded = file ? DecodeImage(bytes) : std::nullopt;
                        if (decoded && decoded->Width > 0 && decoded->Height > 0)
                        {
                            Lua::UiImage image;
                            image.Width = decoded->Width;
                            image.Height = decoded->Height;
                            image.Rgba = std::move(decoded->Rgba);
                            image.Path = resolved;
                            node->ImageId = form.Model.AddImage(std::move(image));
                        }
                        else
                            m_badImages.insert(resolved);
                    }
                }
            }
            for (const int child : std::vector<int>(node->Children))
                visit(child);
        };
        for (const int window : Lua::FormWindows(form))
            visit(window);
    }

    void FormEditorWindow::Copy()
    {
        const UiNode* node = m_form.Model.Find(m_selected);
        std::string text;
        if (m_selected == m_form.Window || !node)
            text = Lua::FormSave(m_form);
        else if (node->Kind == UiKind::Window)
            text = Lua::FormFragment(m_form, node->Children);
        else
            text = Lua::FormFragment(m_form, { m_selected });
        ImGui::SetClipboardText(text.c_str());
    }

    void FormEditorWindow::Cut()
    {
        if (!m_form.Model.Find(m_selected) || m_form.Model.Find(m_selected)->Kind == UiKind::Window)
            return;
        Copy();
        Delete();
    }

    void FormEditorWindow::Paste()
    {
        const char* text = ImGui::GetClipboardText();
        PasteText(text ? std::string(text) : std::string());
    }

    void FormEditorWindow::PasteText(const std::string& text)
    {
        const UiNode* selected = m_form.Model.Find(m_selected);
        if (!selected)
            selected = m_form.Model.Find(m_form.Window);
        if (!selected)
            return;
        Remember();
        std::string error;
        std::vector<int> made;
        if (Lua::UiIsContainer(selected->Kind))
            made = Lua::FormPaste(m_form, text, selected->Id, -1, error);
        if (made.empty() && selected->Id != m_form.Window)
        {
            std::string again;
            made = Lua::FormPaste(m_form, text, selected->Parent, Lua::FormIndexOf(m_form, selected->Id) + 1, again);
            if (!made.empty() || error.empty())
                error = again;
        }
        if (made.empty())
        {
            m_undo.pop_back();
            m_engine.Notify(LogLevel::Warning, error.empty() ? std::string("The clipboard holds no form widgets.") : "Nothing pasted: " + error);
            return;
        }
        m_lastSource = 0;
        Changed();
        Select(made.front());
        if (!error.empty())
            m_engine.Notify(LogLevel::Warning, "Some widgets were left out: " + error);
    }

    std::filesystem::path FormEditorWindow::TemplateFolder() const
    {
        return Paths::ProgramFolder() / L"forms" / L"templates";
    }

    void FormEditorWindow::TemplateMenu()
    {
        if (!ImGui::BeginMenu(ICON_FA_SHAPES "  New from Template"))
            return;
        for (const Lua::FormTemplate& entry : Lua::FormTemplates())
        {
            if (entry.Snippet)
                continue;
            if (ImGui::MenuItem(std::string(entry.Title).c_str()))
            {
                m_pendingTemplate = std::string(entry.Text);
                AskThen(Pending::Template);
            }
            ItemTooltip(std::string(entry.Help).c_str());
        }
        std::vector<std::filesystem::path> own;
        std::error_code ignored;
        for (std::filesystem::directory_iterator it(TemplateFolder(), ignored), end; !ignored && it != end; it.increment(ignored))
        {
            if (it->is_regular_file(ignored) && FormStem(it->path()) + ".form.lua" == WideToUtf8(it->path().filename().wstring()))
                own.push_back(it->path());
        }
        std::sort(own.begin(), own.end());
        if (!own.empty())
            ImGui::Separator();
        for (const std::filesystem::path& path : own)
        {
            if (ImGui::MenuItem(FormStem(path).c_str()))
            {
                std::string text;
                if (ReadFile(path, text))
                {
                    m_pendingTemplate = text;
                    AskThen(Pending::Template);
                }
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FOLDER "  Open Templates Folder"))
        {
            std::filesystem::create_directories(TemplateFolder(), ignored);
            ::ShellExecuteW(nullptr, L"open", TemplateFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::EndMenu();
    }

    void FormEditorWindow::SnippetMenu()
    {
        if (!ImGui::BeginMenu("Insert"))
            return;
        for (const Lua::FormTemplate& entry : Lua::FormTemplates())
        {
            if (!entry.Snippet)
                continue;
            if (ImGui::MenuItem(std::string(entry.Title).c_str()))
                PasteText(std::string(entry.Text));
            ItemTooltip(std::string(entry.Help).append("\nIt goes into the selected container, or after the selected widget.").c_str());
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Widget"))
        {
            for (const UiKind kind : Lua::FormKinds())
            {
                const std::string label = std::string(KindIcon(kind)) + "  " + std::string(Lua::FormKindTitle(kind));
                if (ImGui::MenuItem(label.c_str()))
                    Add(kind);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_WINDOW_RESTORE "  Modal Form"))
            AddModal();
        ItemTooltip("A dialog in the same form file that opens in front of the program; a button opens it with Opens.");
        ImGui::EndMenu();
    }

    void FormEditorWindow::SaveAsTemplate()
    {
        std::error_code ignored;
        std::filesystem::create_directories(TemplateFolder(), ignored);
        const UiNode* window = m_form.Model.Find(m_form.Window);
        const std::string name = SafeFileName(window ? window->Title : std::string()) + ".form.lua";
        const auto chosen = m_workspace.SaveFileDialog({ { L"Manifold forms (*.form.lua)", L"*.form.lua" } }, WideToUtf8((TemplateFolder() / Utf8ToWide(name)).wstring()), L"lua");
        if (!chosen)
            return;
        std::filesystem::path path = *chosen;
        if (FormStem(path) + ".form.lua" != WideToUtf8(path.filename().wstring()))
            path.replace_filename(Utf8ToWide(FormStem(path) + ".form.lua"));
        if (!WriteFile(path, Lua::FormSave(m_form)))
        {
            m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(path.wstring()));
            return;
        }
        m_engine.Notify(LogLevel::Success, "Saved the template " + FormStem(path) + "; File > New from Template lists it.");
    }

    std::filesystem::path FormEditorWindow::LinkedScript() const
    {
        if (m_oneFile)
            return m_path.empty() ? std::filesystem::path() : m_path.lexically_normal();
        if (m_form.Script.empty())
            return {};
        std::filesystem::path script(Utf8ToWide(m_form.Script));
        if (script.is_relative())
            script = (m_path.empty() ? Paths::ProgramFolder() : m_path.parent_path()) / script;
        return script.lexically_normal();
    }

    void FormEditorWindow::SetLink(const std::filesystem::path& script)
    {
        std::string stored = WideToUtf8(script.wstring());
        if (!m_path.empty())
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::relative(script, m_path.parent_path(), error);
            if (!error && !relative.empty() && relative.wstring().rfind(L"..", 0) != 0)
                stored = WideToUtf8(relative.generic_wstring());
        }
        if (stored == m_form.Script)
            return;
        Remember();
        m_form.Script = stored;
        Changed();
    }

    void FormEditorWindow::LinkScript()
    {
        const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
        if (const auto chosen = m_workspace.OpenFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" }, { L"All files", L"*.*" } }, folder))
            SetLink(*chosen);
    }

    void FormEditorWindow::OpenLinked(int line)
    {
        if (m_oneFile && (m_path.empty() || Dirty()) && !Save(false))
            return;
        const std::filesystem::path script = LinkedScript();
        if (script.empty())
            return;
        if (ScriptDirty())
            SaveScript();
        if (!m_workspace.OpenScriptFile(script, line))
            m_engine.Notify(LogLevel::Error, "Could not open " + WideToUtf8(script.wstring()));
    }

    void FormEditorWindow::OpenHandler(UiNode& node, const Lua::FormHandler& handler)
    {
        const std::string key(handler.Key);
        std::string name;
        if (const auto meta = m_form.Meta.find(node.Id); meta != m_form.Meta.end())
        {
            if (const auto found = meta->second.Handlers.find(key); found != meta->second.Handlers.end())
                name = found->second;
        }
        if (name.empty())
        {
            std::string base = m_form.Meta.count(node.Id) ? m_form.Meta[node.Id].Name : std::string();
            if (base.empty())
                base = node.Kind == UiKind::Window ? std::string("window") : std::string(Lua::UiKindName(node.Kind));
            name = base + key.substr(2);
            Remember();
            m_form.Meta[node.Id].Handlers[key] = name;
            Changed();
        }
        if (!Lua::FormIsIdentifier(name))
        {
            m_engine.Notify(LogLevel::Warning, name + " is no Lua function name.");
            return;
        }
        ShowFunction(name);
    }

    void FormEditorWindow::EnsureScript()
    {
        if (m_oneFile)
        {
            // ∑ the file changed elsewhere (a script window, another editor):
            // without own edits here, take its form and code; else warn
            if (m_path.empty())
                return;
            const double now = ImGui::GetTime();
            if (now < m_nextScriptPoll)
                return;
            m_nextScriptPoll = now + 0.5;
            const std::uint64_t stamp = Lua::UiModel::FileStamp(WideToUtf8(m_path.wstring()));
            if (stamp == 0 || stamp == m_scriptStamp)
                return;
            m_scriptStamp = stamp;
            std::string text;
            if (!ReadFile(m_path, text) || text == m_fileDisk)
                return;
            Lua::FormDocument form;
            std::string error;
            const FileText plain = PlainText(text);
            const std::string view = Lua::FormScriptView(plain.Text);
            if (Dirty() || !Lua::FormLoadScript(plain.Text, form, error) || !Lua::FormViewIntact(view))
            {
                m_scriptConflict = true;
                return;
            }
            Remember();
            m_crlf = plain.Crlf;
            m_bom = plain.Bom;
            form.ScriptText = view;
            form.ScriptLoaded = true;
            form.CanvasX = m_form.CanvasX;
            form.CanvasY = m_form.CanvasY;
            m_form = std::move(form);
            m_scriptDisk = m_form.ScriptText;
            m_fileDisk = text;
            m_scriptConflict = false;
            m_scriptEditor.KeepCursor = true;
            if (!m_form.Model.Find(m_selected))
                m_selected = m_form.Window;
            ++m_revision;
            return;
        }
        const std::filesystem::path script = LinkedScript();
        if (script.empty())
        {
            m_form.ScriptLoaded = false;
            m_scriptPath.clear();
            return;
        }
        const double now = ImGui::GetTime();
        if (m_form.ScriptLoaded && m_scriptPath == script)
        {
            if (now < m_nextScriptPoll)
                return;
            m_nextScriptPoll = now + 0.5;
            const std::uint64_t stamp = Lua::UiModel::FileStamp(WideToUtf8(script.wstring()));
            if (stamp == 0 || stamp == m_scriptStamp)
                return;
            m_scriptStamp = stamp;
            std::string text;
            if (!ReadFile(script, text) || text == m_scriptDisk)
                return;
            if (m_form.ScriptText == m_scriptDisk)
            {
                m_form.ScriptText = text;
                m_scriptDisk = text;
                m_scriptEditor.KeepCursor = true;
            }
            else
                m_scriptConflict = true;
            return;
        }
        std::string text;
        if (ScriptWindow* window = m_workspace.ScriptFile(script); window && window->FileChanged())
        {
            ReadFile(script, m_scriptDisk);
            text = window->Text();
        }
        else
        {
            ReadFile(script, text);
            m_scriptDisk = text;
        }
        m_form.ScriptText = text;
        m_form.ScriptLoaded = true;
        m_scriptPath = script;
        m_scriptStamp = Lua::UiModel::FileStamp(WideToUtf8(script.wstring()));
        m_nextScriptPoll = now + 0.5;
        m_scriptConflict = false;
    }

    bool FormEditorWindow::SaveScript()
    {
        if (m_oneFile)
        {
            if (m_path.empty())
                return true;
            const std::optional<std::string> file = Lua::FormComposeScript(m_form.ScriptText, m_form);
            if (!file)
            {
                m_engine.Notify(LogLevel::Error, "The script lost the form's line (local form = { ... }), so it was not written.");
                return false;
            }
            const std::string bytes = FileBytes(*file, m_crlf, m_bom);
            if (bytes == m_fileDisk)
            {
                m_dirty = false;
                m_scriptDisk = m_form.ScriptText;
                return true;
            }
            ScriptWindow* window = m_workspace.ScriptFile(m_path);
            if (window && window->FileChanged() && PlainText(window->Text()).Text != *file)
            {
                m_engine.Notify(LogLevel::Warning, WideToUtf8(m_path.filename().wstring()) + " is open in a script window with other unsaved changes, so the form editor did not write it. Save or close that window first.");
                return false;
            }
            if (!WriteFile(m_path, bytes))
            {
                m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(m_path.wstring()));
                return false;
            }
            if (window)
            {
                window->ReplaceText(*file);
                window->SetFile(m_path);
            }
            m_fileDisk = bytes;
            m_scriptDisk = m_form.ScriptText;
            m_scriptPath = m_path;
            m_scriptStamp = Lua::UiModel::FileStamp(WideToUtf8(m_path.wstring()));
            m_scriptConflict = false;
            m_dirty = false;
            return true;
        }
        if (!m_form.ScriptLoaded || m_scriptPath.empty() || m_form.ScriptText == m_scriptDisk)
            return true;
        ScriptWindow* window = m_workspace.ScriptFile(m_scriptPath);
        if (window && window->FileChanged() && window->Text() != m_form.ScriptText)
        {
            m_engine.Notify(LogLevel::Warning, WideToUtf8(m_scriptPath.filename().wstring()) + " is open in a script window with other unsaved changes, so the form editor did not write it. Save or close that window first.");
            return false;
        }
        if (!WriteFile(m_scriptPath, m_form.ScriptText))
        {
            m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(m_scriptPath.wstring()));
            return false;
        }
        if (window)
        {
            window->ReplaceText(m_form.ScriptText);
            window->SetFile(m_scriptPath);
        }
        m_scriptDisk = m_form.ScriptText;
        m_scriptStamp = Lua::UiModel::FileStamp(WideToUtf8(m_scriptPath.wstring()));
        m_scriptConflict = false;
        return true;
    }

    bool FormEditorWindow::AddFunctions(const std::string& only)
    {
        EnsureScript();
        if (!m_form.ScriptLoaded)
            return false;
        Lua::FormMerge merge;
        if (only.empty())
            merge = Lua::FormMergeScript(m_form.ScriptText, m_form);
        else
        {
            // ∑ A chip click adds just its own function, not every missing one.
            Lua::FormDocument one = m_form;
            for (auto& [id, meta] : one.Meta)
                std::erase_if(meta.Handlers, [&](const auto& entry) { return entry.second != only; });
            merge = Lua::FormMergeScript(m_form.ScriptText, one);
        }
        if (!merge.Anchored)
        {
            m_engine.Notify(LogLevel::Warning, (m_scriptPath.empty() ? std::string("The script") : WideToUtf8(m_scriptPath.filename().wstring())) + " neither builds the form nor creates its window, so no function was added; write them yourself.");
            return false;
        }
        if (merge.Added.empty())
            return true;
        Remember();
        m_form.ScriptText = merge.Text;
        m_scriptEditor.KeepCursor = true;
        return true;
    }

    void FormEditorWindow::ShowFunction(const std::string& name)
    {
        std::filesystem::path script = LinkedScript();
        std::error_code missing;
        if (!m_oneFile && (script.empty() || !std::filesystem::exists(script, missing)))
        {
            m_style = 1;
            Export();
            script = LinkedScript();
            if (script.empty() || !std::filesystem::exists(script, missing))
                return;
        }
        EnsureScript();
        if (Lua::FormFunctionLine(m_form.ScriptText, name) == 0)
            AddFunctions(name);
        const int line = Lua::FormFunctionLine(m_form.ScriptText, name);
        m_codeTab = 0;
        m_showCode = true;
        if (line > 0)
        {
            m_scriptEditor.GotoLine = line + 1;
            m_scriptEditor.Focus = true;
        }
        m_activeChip = name;
    }

    void FormEditorWindow::FunctionChips(ImVec2 origin, ImVec2 areaMin, ImVec2 areaMax)
    {
        const std::vector<std::pair<std::string, std::string>> functions = Lua::FormHandlerList(m_form);
        if (functions.empty())
            return;
        EnsureScript();
        const bool linked = m_form.ScriptLoaded;
        const ImGuiIO& io = ImGui::GetIO();
        const float height = ImGui::GetFrameHeight() + Px(2.0f);
        const float gapY = Px(10.0f);
        // ∑ a chip without a place sits right of the form (or modal) whose
        // widget names it, one below the other
        std::map<std::string, int> owner;
        for (const auto& [id, meta] : m_form.Meta)
        {
            for (const auto& [key, handler] : meta.Handlers)
            {
                if (!owner.count(handler))
                    owner[handler] = Lua::FormRootWindow(m_form, id);
            }
        }
        std::map<int, float> autoRows;
        std::map<std::string, std::pair<ImVec2, ImVec2>> rects;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        // chips above their links: both on the surface, below the frames
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        std::string hovered;
        for (const auto& [name, params] : functions)
        {
            ImVec2 place;
            if (const auto stored = m_form.Nodes.find(name); stored != m_form.Nodes.end())
                place = ImVec2(stored->second.X, stored->second.Y);
            else
            {
                const int root = owner.count(name) ? owner[name] : m_form.Window;
                const auto frame = m_frames.find(root);
                const ImVec2 at = FramePlace(root);
                const float width = frame == m_frames.end() ? 420.0f : (frame->second.Max.x - frame->second.Min.x) / Px(1.0f);
                float& row = autoRows[root];
                place = ImVec2(at.x + width + 56.0f, at.y + row);
                row += (height + gapY) / Px(1.0f);
            }
            m_chipPlaces[name] = place;
            const bool present = linked && Lua::FormFunctionLine(m_form.ScriptText, name) > 0;
            const std::string label = std::string(ICON_FA_BOLT "  ") + name + "(" + params + ")";
            const float width = ImGui::CalcTextSize(label.c_str()).x + Px(28.0f) + (present ? 0.0f : ImGui::CalcTextSize(ICON_FA_PLUS).x + Px(8.0f));
            const ImVec2 min(origin.x + Px(place.x), origin.y + Px(place.y));
            const ImVec2 max(min.x + width, min.y + height);
            rects[name] = { min, max };
            ImGui::PushID(name.c_str());
            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton("##chip", ImVec2(width, height));
            const bool over = ImGui::IsItemHovered();
            if (over)
                hovered = name;
            if (ImGui::IsItemActivated())
                m_chipDragged = false;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f))
            {
                if (!m_chipDragged)
                    Remember();
                m_chipDragged = true;
                Lua::FormNode& node = m_form.Nodes[name];
                node.X = place.x + io.MouseDelta.x / Px(1.0f);
                node.Y = place.y + io.MouseDelta.y / Px(1.0f);
                if (m_form.Nodes.size() == 1 || true)
                    Changed();
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
            if (ImGui::IsItemDeactivated() && !m_chipDragged)
                ShowFunction(name);
            if (over && !ImGui::IsItemActive())
                ImGui::SetTooltip("%s", present ? "Click to show the function in the script below; drag to move the chip."
                                                : linked ? "Not in the script yet: click to add it." : "Click to create the script that holds the handlers.");
            ImGui::PopID();
            const bool active = name == m_activeChip;
            const ImVec4 family = ManifoldTheme::CodeKeyword;
            draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(max.x + 2.0f, max.y + 3.0f), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Shadow, 0.25f)), height * 0.5f);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(over ? ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.18f) : ManifoldTheme::Card), height * 0.5f);
            if (present)
                draw->AddRect(min, max, ImGui::GetColorU32(active ? ManifoldTheme::Accent : ManifoldTheme::WithAlpha(family, over ? 0.9f : 0.5f)), height * 0.5f, 0, active ? Px(2.0f) : 1.0f);
            else
                DashedRect(draw, min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.8f)), 1.0f, Px(4.0f), Px(3.0f));
            const float textY = min.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
            const float boltWidth = ImGui::CalcTextSize(ICON_FA_BOLT "  ").x;
            draw->AddText(ImVec2(min.x + Px(12.0f), textY), ImGui::GetColorU32(present ? ManifoldTheme::Accent : ManifoldTheme::Muted), ICON_FA_BOLT);
            const std::string call = name + "(" + params + ")";
            draw->AddText(ImVec2(min.x + Px(12.0f) + boltWidth, textY), ImGui::GetColorU32(present ? ManifoldTheme::Text : ManifoldTheme::Muted), call.c_str());
            if (!present)
                draw->AddText(ImVec2(max.x - Px(12.0f) - ImGui::CalcTextSize(ICON_FA_PLUS).x, textY), ImGui::GetColorU32(ManifoldTheme::Muted), ICON_FA_PLUS);
        }
        m_hoveredChip = hovered;

        // ∑ Links never cross the form: each widget gets a port on the form's
        // edge at its own height (on the side its chip is on), and only the
        // part outside the form is drawn. A hovered or selected widget or chip
        // adds a faint dashed guide from the widget to its port.
        draw->ChannelsSetCurrent(0);
        ImDrawList* links = draw;
        for (const auto& [id, meta] : m_form.Meta)
        {
            ImDrawList* inner = m_frameMarks.count(Lua::FormRootWindow(m_form, id)) ? m_frameMarks[Lua::FormRootWindow(m_form, id)] : nullptr;
            const auto frame = m_frames.find(Lua::FormRootWindow(m_form, id));
            if (frame == m_frames.end())
                continue;
            const ImVec2 frameMin = frame->second.Min;
            const ImVec2 frameMax = frame->second.Max;
            const float titleHeight = frame->second.Title;
            const float bodyTop = frameMin.y + titleHeight;
            const ImVec2 innerMin(std::max(frameMin.x, areaMin.x), std::max(bodyTop, areaMin.y));
            const ImVec2 innerMax(std::min(frameMax.x, areaMax.x), std::min(frameMax.y, areaMax.y));
            const float formCenter = (frameMin.x + frameMax.x) * 0.5f;
            const bool isWindow = m_form.Model.Find(id) && m_form.Model.Find(id)->Kind == UiKind::Window;
            for (const auto& [key, name] : meta.Handlers)
            {
                const auto chip = rects.find(name);
                if (chip == rects.end())
                    continue;
                const ImVec2 chipMin = chip->second.first;
                const ImVec2 chipMax = chip->second.second;
                const bool right = (chipMin.x + chipMax.x) * 0.5f >= formCenter;
                const float edge = right ? frameMax.x : frameMin.x;
                const float out = right ? 1.0f : -1.0f;
                const UiDesign::Rect* rect = nullptr;
                float y = frameMin.y + titleHeight * 0.5f;
                if (!isWindow)
                {
                    rect = m_trying ? nullptr : RectOf(id);
                    if (!rect)
                        continue;
                    y = std::clamp((rect->Min.y + rect->Max.y) * 0.5f, bodyTop + Px(8.0f), std::max(bodyTop + Px(8.0f), frameMax.y - Px(8.0f)));
                }
                const ImVec2 port(edge, y);
                const ImVec2 to(right ? chipMin.x : chipMax.x, (chipMin.y + chipMax.y) * 0.5f);
                const bool strong = name == hovered || name == m_activeChip || id == m_selected || id == m_hovered;
                const ImU32 color = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, strong ? 0.95f : 0.35f));
                const float bend = std::max(Px(30.0f), std::abs(to.x - port.x) * 0.45f);
                links->PushClipRect(areaMin, areaMax, true);
                links->AddBezierCubic(port, ImVec2(port.x + bend * out, port.y), ImVec2(to.x - bend * out, to.y), to, color, strong ? Px(2.0f) : Px(1.2f));
                links->AddCircleFilled(port, strong ? Px(3.5f) : Px(2.5f), color);
                links->AddCircleFilled(to, strong ? Px(3.5f) : Px(2.5f), color);
                links->PopClipRect();
                if (strong && rect && inner && innerMax.x > innerMin.x && innerMax.y > innerMin.y)
                {
                    const float from = right ? rect->Max.x + Px(3.0f) : rect->Min.x - Px(3.0f);
                    if ((port.x - from) * out > Px(4.0f))
                    {
                        inner->PushClipRect(innerMin, innerMax, true);
                        DashedLine(inner, ImVec2(from, y), port, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.55f)), 1.0f, Px(4.0f), Px(3.0f));
                        inner->PopClipRect();
                    }
                }
            }
        }
        draw->ChannelsMerge();
        if (!hovered.empty())
        {
            for (const auto& [id, meta] : m_form.Meta)
            {
                for (const auto& [key, name] : meta.Handlers)
                {
                    if (name == hovered && m_form.Model.Find(id) && m_form.Model.Find(id)->Kind != UiKind::Window)
                        m_hovered = id;
                }
            }
        }
    }

    void FormEditorWindow::ImportFile()
    {
        const std::filesystem::path folder = m_path.empty() ? Paths::ProgramFolder() : m_path.parent_path();
        const auto chosen = m_workspace.OpenFileDialog({ { L"Cheat Engine forms and tables (*.frm;*.ct;*.xml)", L"*.frm;*.ct;*.xml" }, { L"All files", L"*.*" } }, folder);
        if (!chosen)
            return;
        std::string text;
        if (!ReadFile(*chosen, text))
        {
            m_engine.Notify(LogLevel::Error, "Could not read " + WideToUtf8(chosen->wstring()));
            return;
        }
        std::string error;
        std::vector<Table::CeFormEntry> entries = Table::ReadCeFormFile(text, error);
        if (entries.empty())
        {
            m_engine.Notify(LogLevel::Error, WideToUtf8(chosen->filename().wstring()) + ": " + error);
            return;
        }
        ImportChoices(std::move(entries), WideToUtf8(chosen->filename().wstring()));
    }

    void FormEditorWindow::ImportTable()
    {
        std::vector<Table::CeFormEntry> entries = Table::ReadCeTableForms(m_engine.Table().ExtraXml);
        if (entries.empty())
        {
            m_engine.Notify(LogLevel::Warning, "The open table holds no forms.");
            return;
        }
        ImportChoices(std::move(entries), "the open table");
    }

    void FormEditorWindow::ImportChoices(std::vector<Table::CeFormEntry> entries, std::string source)
    {
        if (entries.size() == 1)
        {
            ImportEntry(entries.front());
            return;
        }
        m_importChoices = std::move(entries);
        m_importSource = std::move(source);
        m_importChoice = 0;
        m_importDialog.Open();
    }

    void FormEditorWindow::ImportEntry(const Table::CeFormEntry& entry)
    {
        if (!entry.Root)
        {
            m_engine.Notify(LogLevel::Error, "The form " + entry.Name + " could not be read: " + entry.Error);
            return;
        }
        Lua::FormImport imported = Lua::FormFromCe(*entry.Root);
        m_importNotes = std::move(imported.Notes);
        m_importConverted = imported.Converted;
        m_pendingImport = std::move(imported.Form);
        AskThen(Pending::Import);
    }

    void FormEditorWindow::Draw()
    {
        if (!m_open)
            return;
        const std::string title = Title();
        if (!BeginWindow(title.c_str(), ImVec2(1240.0f, 760.0f), ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(820.0f, 460.0f)))
            return;
        if (!m_open && Dirty())
        {
            m_open = true;
            AskThen(Pending::Close);
        }
        Menu();
        Shortcuts();
        if (m_oneFile && !m_trying)
            EnsureScript(); // the script is the form's file: watch it even with the code pane hidden
        if (!m_form.Model.Find(m_selected))
            m_selected = m_form.Window;
        m_hovered = 0;
        m_problems = Lua::FormProblems(m_form);
        {
            // theme colour names: the palette the program draws with
            std::vector<int> stack = Lua::FormWindows(m_form);
            while (!stack.empty())
            {
                const UiNode* node = m_form.Model.Find(stack.back());
                stack.pop_back();
                if (!node)
                    continue;
                stack.insert(stack.end(), node->Children.begin(), node->Children.end());
                for (const std::string* colour : { &node->Color, &node->Background })
                {
                    if (colour->empty() || colour->front() == '#')
                        continue;
                    const bool known = std::any_of(std::begin(ManifoldTheme::kPalette), std::end(ManifoldTheme::kPalette),
                        [&](const ManifoldTheme::PaletteColor& entry) { return EqualsNoCase(entry.Name, *colour); });
                    if (!known)
                        m_problems.push_back({ node->Id, std::string(Lua::FormKindTitle(node->Kind)) + ": \"" + *colour + "\" is no theme colour; pick one with the swatch, or write #RRGGBB" });
                }
            }
        }
        // bindings that the open table cannot resolve to one record
        for (const auto& [id, meta] : m_form.Meta)
        {
            if (meta.Record.empty() || meta.RecordById || !m_form.Model.Find(id))
                continue;
            if (const int twins = m_engine.Table().CountByDescription(meta.Record); twins > 1)
                m_problems.push_back({ id, std::string(Lua::FormKindTitle(m_form.Model.Find(id)->Kind)) + ": " + std::to_string(twins) + " records of the open table are named \"" + meta.Record +
                                               "\"; bind it by id (pick it in Memory Record)" });
        }
        m_problemNodes.clear();
        for (const Lua::FormProblem& problem : m_problems)
            m_problemNodes.insert(problem.Node);

        const float gap = Px(8.0f);
        const float minSide = Px(220.0f);
        const float minCenter = Px(320.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        if (m_leftWidth <= 0.0f)
            m_leftWidth = Px(270.0f);
        if (m_rightWidth <= 0.0f)
            m_rightWidth = Px(330.0f);
        const float room = available.x - minCenter - gap * 2.0f;
        m_leftWidth = std::clamp(m_leftWidth, minSide, std::max(minSide, room - minSide));
        m_rightWidth = std::clamp(m_rightWidth, minSide, std::max(minSide, room - m_leftWidth));
        const float centerWidth = std::max(Px(120.0f), available.x - m_leftWidth - m_rightWidth - gap * 2.0f);

        ImGui::SetCursorPos(start);
        ImGui::BeginChild("##left", ImVec2(m_leftWidth, available.y), ImGuiChildFlags_None);
        {
            const ImVec2 inner = ImGui::GetContentRegionAvail();
            const float minPalette = Px(110.0f);
            const float minOutline = Px(110.0f);
            if (m_paletteHeight <= 0.0f)
                m_paletteHeight = inner.y * 0.44f;
            m_paletteHeight = std::clamp(m_paletteHeight, minPalette, std::max(minPalette, inner.y - minOutline - gap));
            const ImVec2 top = ImGui::GetCursorPos();
            ImGui::BeginChild("##palette", ImVec2(0.0f, m_paletteHeight), ImGuiChildFlags_None);
            Palette();
            ImGui::EndChild();
            float first = m_paletteHeight;
            float second = inner.y - m_paletteHeight - gap;
            ImGui::SetCursorPos(top);
            if (Splitter("##palettesplit", false, gap, &first, &second, minPalette, minOutline, inner.x))
                m_paletteHeight = first;
            ImGui::SetCursorPos(ImVec2(top.x, top.y + m_paletteHeight + gap));
            Outline();
        }
        ImGui::EndChild();
        {
            float first = m_leftWidth;
            float second = centerWidth;
            ImGui::SetCursorPos(start);
            Splitter("##leftsplit", true, gap, &first, &second, minSide, minCenter, available.y);
            m_leftWidth = first;
        }

        const float centerX = start.x + m_leftWidth + gap;
        ImGui::SetCursorPos(ImVec2(centerX, start.y));
        ImGui::BeginChild("##center", ImVec2(centerWidth, available.y), ImGuiChildFlags_None);
        {
            const ImVec2 inner = ImGui::GetContentRegionAvail();
            if (m_codeHeight <= 0.0f)
                m_codeHeight = inner.y * 0.36f;
            const float minPreview = Px(160.0f);
            const float minCode = Px(110.0f);
            if (m_showCode)
                m_codeHeight = std::clamp(m_codeHeight, minCode, std::max(minCode, inner.y - minPreview - gap));
            const float previewHeight = m_showCode ? inner.y - m_codeHeight - gap : inner.y;
            const ImVec2 top = ImGui::GetCursorPos();
            ImGui::BeginChild("##previewpane", ImVec2(0.0f, previewHeight), ImGuiChildFlags_None);
            Preview();
            ImGui::EndChild();
            if (m_showCode)
            {
                float first = previewHeight;
                float second = m_codeHeight;
                ImGui::SetCursorPos(top);
                if (Splitter("##codesplit", false, gap, &first, &second, minPreview, minCode, inner.x))
                    m_codeHeight = second;
                ImGui::SetCursorPos(ImVec2(top.x, top.y + previewHeight + gap));
                CodePane(m_codeHeight);
            }
        }
        ImGui::EndChild();
        {
            float first = centerWidth;
            float second = m_rightWidth;
            ImGui::SetCursorPos(ImVec2(centerX, start.y));
            Splitter("##rightsplit", true, gap, &first, &second, minCenter, minSide, available.y);
            m_rightWidth = second;
        }
        ImGui::SetCursorPos(ImVec2(centerX + centerWidth + gap, start.y));
        ImGui::BeginChild("##right", ImVec2(m_rightWidth, available.y), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        Properties();
        ImGui::EndChild();
        Popups();
        EndToolWindow();
    }
}
