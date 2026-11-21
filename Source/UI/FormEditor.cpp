#include "UI/FormEditor.h"

#include "App/Engine.h"
#include "Core/IconNames.h"
#include "Core/ImageDecode.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
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

        // ∑ Tags are drawn over the whole preview area, not clipped to the
        // previewed window, so a widget at the window's edge still shows its
        // full tag; one that would leave the area is moved back into it.
        void Tag(ImDrawList* draw, ImVec2 min, ImVec2 areaMin, ImVec2 areaMax, const std::string& text, ImU32 background, ImU32 foreground, float right = 0.0f)
        {
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const ImVec2 padding(Px(5.0f), Px(1.0f));
            const float width = size.x + padding.x * 2.0f;
            const float height = size.y + padding.y * 2.0f;
            ImVec2 at(right > 0.0f ? std::max(min.x, right - width) : min.x, min.y - height - Px(2.0f));
            if (at.y < areaMin.y)
                at.y = min.y + Px(2.0f);
            at.x = std::max(areaMin.x + Px(2.0f), std::min(at.x, areaMax.x - width - Px(2.0f)));
            draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), background, Px(3.0f));
            draw->AddText(ImVec2(at.x + padding.x, at.y + padding.y), foreground, text.c_str());
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
                "topmost", "position", "dock", "opacity", "inactiveOpacity", "interval" };
            if (kind == UiKind::Window)
                for (std::string_view entry : kWindowKeys)
                    if (entry == key)
                        return 4;
            static constexpr std::string_view kContent[] = { "text", "title", "label", "icon", "badge", "hint", "items", "menu", "rows", "image" };
            static constexpr std::string_view kValue[] = { "value", "min", "max", "step", "integer", "hex", "format", "alpha" };
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
                        const std::string second = Trim(std::string_view(head).substr(comma + 1));
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
                if (owner == "*widgets" && member.empty())
                {
                    std::vector<CodeSuggestion> names;
                    for (const auto& [id, meta] : m_form.Meta)
                    {
                        if (meta.Name.empty() || id == m_form.Window || meta.Name.compare(0, prefix.size(), prefix) != 0)
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
                        return m_engine.Lua().ClassMember(owner, chain.substr(dot + 1));
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
        Reset(std::move(form), {});
    }

    FormEditorWindow::~FormEditorWindow() = default;

    void FormEditorWindow::Reset(Lua::FormDocument form, std::filesystem::path path)
    {
        m_form = std::move(form);
        m_path = std::move(path);
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
        if (!m_path.empty())
            return FormStem(m_path) + ".lua";
        const UiNode* window = m_form.Model.Find(m_form.Window);
        return SafeFileName(window ? window->Title : std::string()) + ".lua";
    }

    const std::string& FormEditorWindow::Code()
    {
        if (m_codeRevision != m_revision || m_codeStyle != m_style)
        {
            const std::string file = m_path.empty() ? FormStem(ScriptFileName()) + ".form.lua" : WideToUtf8(m_path.filename().wstring());
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
        if (!Lua::FormLoad(text, form, error, &warnings))
        {
            m_engine.Notify(LogLevel::Error, WideToUtf8(path.filename().wstring()) + ": " + error);
            return false;
        }
        Reset(std::move(form), path);
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

    void FormEditorWindow::Delete()
    {
        if (m_selected == m_form.Window || !m_form.Model.Find(m_selected))
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
        if (m_selected == m_form.Window)
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
        const std::string code = Lua::FormScript(m_form, Lua::FormScriptStyle::Standalone);
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
            Reset(Lua::NewForm("New Form"), {});
            break;
        case Pending::Open:
            Open();
            break;
        case Pending::Close:
            m_dirty = false;
            m_open = false;
            break;
        case Pending::Import:
            Reset(std::move(m_pendingImport), {});
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
                Reset(std::move(form), {});
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
        if (const auto path = m_workspace.OpenFileDialog({ { L"Manifold forms (*.form.lua)", L"*.form.lua" }, { L"Lua files (*.lua)", L"*.lua" }, { L"All files", L"*.*" } }, folder))
            OpenFile(*path);
    }

    bool FormEditorWindow::Save(bool as)
    {
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

    void FormEditorWindow::Export()
    {
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
        for (const Command command : { Command::FormUndo, Command::FormRedo, Command::FormDuplicate, Command::FormDelete, Command::FormMoveUp, Command::FormMoveDown, Command::FormCopy, Command::FormCut, Command::FormPaste })
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
            if (CommandItem(ICON_FA_FILE_EXPORT "  Export Script...", Command::FormExport))
                RunCommand(Command::FormExport);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_SHAPES "  Save as Template..."))
                SaveAsTemplate();
            ImGui::Separator();
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
            const bool widget = m_selected != m_form.Window;
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
            if (DangerCommandItem(ICON_FA_TRASH "  Delete", Command::FormDelete, widget))
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
        draw->PushClipRect(min, max, true);
        draw->AddText(ImVec2(min.x + Px(30.0f), y), ImGui::GetColorU32(ManifoldTheme::Text), title.c_str());
        draw->PopClipRect();
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
        ItemTooltip(std::string(Lua::FormKindHelp(kind)).append("\nClick adds it to the selected container, or drag it into the outline or the preview.").c_str());
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
        ImGui::BeginChild("##outline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        // ∑ Deep trees: a narrower indent, and rows as wide as their text so
        // the list scrolls sideways instead of cutting the names off.
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, Px(13.0f));
        OutlineNode(m_form.Window, 0);
        ImGui::PopStyleVar();
        ImGui::EndChild();
        m_scrollOutline = false;
    }

    void FormEditorWindow::OutlineNode(int id, int depth)
    {
        UiNode* node = m_form.Model.Find(id);
        if (!node)
            return;
        ImGui::PushID(id);
        const bool container = Lua::UiIsContainer(node->Kind);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding |
                                   ImGuiTreeNodeFlags_DrawLinesToNodes;
        if (node->Children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (id == m_selected)
            flags |= ImGuiTreeNodeFlags_Selected;
        const std::string text = std::string(KindIcon(node->Kind)) + "  " + Caption(m_form, *node);
        ImGui::PushStyleColor(ImGuiCol_TreeLines, ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.45f));
        const bool open = ImGui::TreeNodeEx("##node", flags);
        ImGui::PopStyleColor();
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const bool rowHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
        const float eyeSide = rowMax.y - rowMin.y;
        const ImVec2 eyeMin(rowMax.x - eyeSide, rowMin.y);
        const bool overEye = node->Kind != UiKind::Window && rowHovered && ImGui::IsMouseHoveringRect(eyeMin, rowMax);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() && !overEye)
            Select(id);
        if (id == m_selected && m_scrollOutline)
            ImGui::SetScrollHereY(0.5f);
        if (ImGui::IsItemHovered())
            m_hovered = id;
        if (ImGui::BeginPopupContextItem("##menu"))
        {
            Select(id);
            NodeMenu(id);
            ImGui::EndPopup();
        }
        if (node->Kind != UiKind::Window && ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(kNodePayload, &id, sizeof(id));
            ImGui::TextUnformatted(text.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            const float y = ImGui::GetIO().MousePos.y;
            const float band = rect.GetHeight() * 0.3f;
            Drop where = Drop::Inside;
            if (node->Kind != UiKind::Window)
            {
                if (y < rect.Min.y + band)
                    where = Drop::Before;
                else if (y > rect.Max.y - band || !container)
                    where = Drop::After;
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
            if (where == Drop::Inside)
                draw->AddRect(rect.Min, rect.Max, accent, Px(3.0f), 0, Px(2.0f));
            else
            {
                const float line = where == Drop::Before ? rect.Min.y : rect.Max.y;
                draw->AddLine(ImVec2(rect.Min.x, line), ImVec2(rect.Max.x, line), accent, Px(2.0f));
            }
            int parent = 0;
            int index = -1;
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kKindPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
            {
                const UiKind kind = static_cast<UiKind>(*static_cast<const int*>(payload->Data));
                if (DropAt(id, where, parent, index))
                    AddInto(kind, parent, index);
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
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float x = rowMin.x + ImGui::GetTreeNodeToLabelSpacing();
            const float y = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f;
            const float alpha = node->Visible ? 1.0f : 0.45f;
            const auto meta = m_form.Meta.find(id);
            const std::string name = meta == m_form.Meta.end() ? std::string() : meta->second.Name;
            const std::string kind(Lua::FormKindTitle(node->Kind));
            std::string detail = Caption(m_form, *node);
            detail = detail.size() > kind.size() ? Trim(std::string_view(detail).substr(kind.size())) : std::string();
            if (!name.empty() && detail.rfind(name, 0) == 0)
                detail = Trim(std::string_view(detail).substr(name.size()));
            const std::string first = name.empty() ? kind : name;
            const std::string second = name.empty() ? detail : (detail.empty() ? kind : kind + "  " + detail);
            float at = x;
            draw->AddText(ImVec2(at, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(FamilyColor(FamilyOf(node->Kind)), alpha)), KindIcon(node->Kind));
            at += Px(20.0f);
            const float right = rowMax.x - ImGui::GetFrameHeight() * 2.6f;
            {
                const float textWidth = ImGui::CalcTextSize(first.c_str()).x + (second.empty() ? 0.0f : Px(8.0f) + ImGui::CalcTextSize(second.c_str()).x);
                const float need = at + textWidth + ImGui::GetFrameHeight() * 2.6f + ImGui::GetStyle().FramePadding.x;
                ImGuiWindow* outline = ImGui::GetCurrentWindow();
                outline->DC.CursorMaxPos.x = std::max(outline->DC.CursorMaxPos.x, need);
                if (rowHovered && !overEye && at + textWidth > right && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                    ImGui::SetTooltip("%s%s%s", first.c_str(), second.empty() ? "" : "  ", second.c_str());
            }
            draw->PushClipRect(ImVec2(at, rowMin.y), ImVec2(std::max(at, right), rowMax.y), true);
            draw->AddText(ImVec2(at, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, alpha)), first.c_str());
            at += ImGui::CalcTextSize(first.c_str()).x + Px(8.0f);
            if (!second.empty())
                draw->AddText(ImVec2(at, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, alpha)), second.c_str());
            draw->PopClipRect();
            const float side = eyeSide;
            float marker = eyeMin.x;
            if (node->Kind != UiKind::Window)
            {
                if (!node->Visible || rowHovered)
                {
                    const char* glyph = node->Visible ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
                    const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
                    draw->AddText(ImVec2(marker + (side - glyphSize.x) * 0.5f, y), ImGui::GetColorU32(overEye ? ManifoldTheme::Text : ManifoldTheme::Muted), glyph);
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
            marker -= side * 0.8f;
            if (m_problemNodes.count(id))
            {
                draw->AddText(ImVec2(marker, y), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_EXCLAMATION_TRIANGLE);
                marker -= side * 0.8f;
            }
            if (meta != m_form.Meta.end() && !meta->second.Handlers.empty())
                draw->AddText(ImVec2(marker, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.8f)), ICON_FA_BOLT);
        }
        if (open)
        {
            for (const int child : std::vector<int>(node->Children))
                OutlineNode(child, depth + 1);
            ImGui::TreePop();
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
            return;
        }
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
        if (CommandItem(ICON_FA_CUT "  Cut", Command::FormCut))
            Cut();
        if (CommandItem(ICON_FA_COPY "  Copy", Command::FormCopy))
            Copy();
        if (CommandItem(ICON_FA_PASTE "  Paste", Command::FormPaste))
            Paste();
        if (CommandItem(ICON_FA_CLONE "  Duplicate", Command::FormDuplicate))
            Duplicate();
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
        int mode = m_trying ? 1 : 0;
        if (Segmented("##mode", &mode, { ICON_FA_MOUSE_POINTER "  Design", ICON_FA_HAND_POINTER "  Try" }, Px(190.0f)) && (mode == 1) != m_trying)
            ToggleTry();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLAY "  Run Script"))
            RunScript();
        ImGui::SameLine();
        if (IconButton(ICON_FA_CROSSHAIRS, "Bring the form back to the top left of the work surface"))
            m_pan = ImVec2(Px(-m_form.CanvasX + 40.0f), Px(-m_form.CanvasY + 30.0f));
        ItemTooltip("Runs the script in Manifold's Lua engine, so the real window opens. Running it again replaces that window.");
        if (const std::filesystem::path script = LinkedScript(); !script.empty())
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            std::error_code ignored;
            const bool exists = std::filesystem::exists(script, ignored);
            const std::string label = std::string(ICON_FA_FILE_CODE "  ") + WideToUtf8(script.filename().wstring()) + (exists ? "" : "  (missing)") + "##linked";
            if (ImGui::TextLink(label.c_str()) && exists)
                OpenLinked();
            ItemTooltip(("Linked script: " + WideToUtf8(script.wstring()) + "\nClick to open it.").c_str());
        }
        if (!m_path.empty())
        {
            if (const int live = m_engine.Lua().Ui().Watching(WideToUtf8(m_path.wstring())); live > 0)
            {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                const std::string text = std::string(ICON_FA_SYNC "  Live in ") + std::to_string(live) + (live == 1 ? " Window" : " Windows") + (m_dirty ? " (save to update)" : "");
                ImGui::TextColored(m_dirty ? ManifoldTheme::Warning : ManifoldTheme::Success, "%s", text.c_str());
                ItemTooltip("A running script loaded this file with manifold.ui.form.load. Saving it rebuilds that window right away, with the script's handlers; widgets that keep their name keep their value.");
            }
        }
        const std::vector<Lua::FormProblem>& problems = m_problems;
        if (!problems.empty())
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            const std::string label = std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + std::to_string(problems.size()) + (problems.size() == 1 ? " Problem" : " Problems");
            if (ImGui::Button(label.c_str()))
                m_problemsDialog.Open();
            ImGui::PopStyleColor();
        }
        if (m_trying)
        {
            ImGui::SameLine();
            Muted("Clicks reach the widgets; handlers are not called.");
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
        const UiNode* window = form.Model.Find(form.Window);
        if (!window)
            return;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Page);
        ImGui::BeginChild("##previewarea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 areaMin = ImGui::GetWindowPos();
        const ImVec2 areaMax(areaMin.x + ImGui::GetWindowWidth(), areaMin.y + ImGui::GetWindowHeight());
        m_areaSize = ImVec2(areaMax.x - areaMin.x, areaMax.y - areaMin.y);
        {
            // ∑ The work surface: dragging empty space (or the middle button
            // anywhere, or the wheel) moves the view; the form and the function
            // chips sit on it at their own places.
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
        const auto limit = [](float value, float low, float high) {
            if (high > 0.0f)
                value = std::min(value, high);
            return std::max(value, low);
        };
        const float pointsWidth = limit(window->Width > 0.0f ? window->Width : 420.0f, window->MinWidth > 0.0f ? window->MinWidth : 160.0f, window->MaxWidth);
        const float pointsHeight = limit(window->Height > 0.0f ? window->Height : 320.0f, window->MinHeight > 0.0f ? window->MinHeight : 100.0f, window->MaxHeight);
        const ImVec2 size(Px(pointsWidth), Px(pointsHeight));
        const ImVec2 frameMin(origin.x + Px(m_form.CanvasX), origin.y + Px(m_form.CanvasY));
        const float titleHeight = ImGui::GetFrameHeight() + Px(4.0f);
        const float bodyHeight = std::max(Px(40.0f), size.y - titleHeight);
        for (int i = 1; i <= 6; ++i)
        {
            const float grow = Px(static_cast<float>(i) * 2.0f);
            draw->AddRectFilled(ImVec2(frameMin.x - grow, frameMin.y - grow + Px(3.0f)), ImVec2(frameMin.x + size.x + grow, frameMin.y + titleHeight + bodyHeight + grow + Px(3.0f)),
                ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Shadow, 0.10f)), Px(8.0f) + grow);
        }
        draw->AddRectFilled(frameMin, ImVec2(frameMin.x + size.x, frameMin.y + titleHeight), ImGui::GetColorU32(ManifoldTheme::TitleBar), Px(6.0f), ImDrawFlags_RoundCornersTop);
        const std::string title = UiHost::IconText(window->Icon, window->Title.empty() ? std::string("Script") : window->Title);
        const float textY = frameMin.y + (titleHeight - ImGui::GetTextLineHeight()) * 0.5f;
        draw->AddText(ImVec2(frameMin.x + Px(10.0f), textY), ImGui::GetColorU32(ManifoldTheme::Text), title.c_str());
        {
            const ImVec2 closeSize = ImGui::CalcTextSize(ICON_FA_TIMES);
            const float closeX = frameMin.x + size.x - Px(12.0f) - closeSize.x;
            draw->AddText(ImVec2(closeX, textY), ImGui::GetColorU32(ManifoldTheme::Muted), ICON_FA_TIMES);
            char dimensions[48];
            std::snprintf(dimensions, sizeof(dimensions), "%d x %d%s", static_cast<int>(pointsWidth), static_cast<int>(pointsHeight), window->Resizable ? "" : "  fixed");
            const float dimensionsWidth = ImGui::CalcTextSize(dimensions).x;
            draw->AddText(ImVec2(closeX - Px(14.0f) - dimensionsWidth, textY), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.8f)), dimensions);
        }
        {
            ImGui::SetCursorScreenPos(frameMin);
            ImGui::InvisibleButton("##formtitle", ImVec2(size.x, titleHeight));
            if (ImGui::IsItemActivated())
                Remember();
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            if (ImGui::IsItemActive() && (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f))
            {
                m_form.CanvasX += ImGui::GetIO().MouseDelta.x / Px(1.0f);
                m_form.CanvasY += ImGui::GetIO().MouseDelta.y / Px(1.0f);
                if (m_trying)
                {
                    m_try.CanvasX = m_form.CanvasX;
                    m_try.CanvasY = m_form.CanvasY;
                }
                Changed();
            }
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("Drag the title to move the form on the work surface.");
        }
        ImGui::SetCursorScreenPos(ImVec2(frameMin.x, frameMin.y + titleHeight));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::Border);
        ImGui::BeginChild("##previewbody", ImVec2(size.x, bodyHeight), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleColor(2);
        const ImVec2 bodyMin = ImGui::GetWindowPos();
        const ImVec2 bodyMax(bodyMin.x + ImGui::GetWindowWidth(), bodyMin.y + ImGui::GetWindowHeight());
        SyncImages(form);
        if (m_trying)
        {
            m_tryHost->DrawContents(m_try.Window, nullptr);
            m_try.Model.TakeEvents();
        }
        else
        {
            m_design.Picking = true;
            m_host->DrawContents(m_form.Window, &m_design);
        }
        const ImVec2 frameMax(frameMin.x + size.x, frameMin.y + titleHeight + bodyHeight);
        const bool grippable = !m_trying && window->Resizable && !window->AutoSize;
        const float grip = Px(16.0f);
        m_gripMin = grippable ? ImVec2(frameMax.x - grip, frameMax.y - grip) : ImVec2(0.0f, 0.0f);
        m_gripMax = grippable ? frameMax : ImVec2(0.0f, 0.0f);
        ImGui::EndChild();
        if (grippable || m_gripping)
        {
            // ∑ No item: an item at the body's corner would grow the body's
            // content and give it a scroll bar right under the grip. The grip
            // tests the mouse itself and holds an active id while it drags, so
            // the tool window does not move along.
            const ImGuiID gripId = ImGui::GetID("##sizegrip");
            const ImGuiIO& io = ImGui::GetIO();
            const bool over = grippable && ImGui::IsMouseHoveringRect(m_gripMin, m_gripMax) &&
                              ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload())
            {
                m_gripping = true;
                Remember();
                ImGui::SetActiveID(gripId, ImGui::GetCurrentWindow());
            }
            if (m_gripping)
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    ImGui::KeepAliveID(gripId);
                    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
                    {
                        UiNode* edited = m_form.Model.Find(m_form.Window);
                        const float scale = Px(1.0f);
                        edited->Width = limit(std::round(pointsWidth + io.MouseDelta.x / scale), edited->MinWidth > 0.0f ? edited->MinWidth : 160.0f, edited->MaxWidth);
                        edited->Height = limit(std::round(pointsHeight + io.MouseDelta.y / scale), edited->MinHeight > 0.0f ? edited->MinHeight : 100.0f, edited->MaxHeight);
                        Changed();
                    }
                }
                else
                {
                    m_gripping = false;
                    if (ImGui::GetActiveID() == gripId)
                        ImGui::ClearActiveID();
                }
            }
            if (over || m_gripping)
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            if (over && !m_gripping)
                ImGui::SetTooltip("Drag to try other sizes; the window keeps to its minimum and maximum.");
            if (grippable)
            {
                const ImU32 color = ImGui::GetColorU32(m_gripping ? ManifoldTheme::Accent : over ? ManifoldTheme::Text : ManifoldTheme::Muted);
                ImDrawList* foreground = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
                foreground->PushClipRect(areaMin, areaMax, true);
                for (int i = 1; i <= 3; ++i)
                {
                    const float inset = Px(4.0f) * static_cast<float>(i);
                    foreground->AddLine(ImVec2(frameMax.x - inset, frameMax.y - 2.0f), ImVec2(frameMax.x - 2.0f, frameMax.y - inset), color, 1.5f);
                }
                foreground->PopClipRect();
            }
        }
        if (!m_trying)
        {
            PreviewOverlay(bodyMin, bodyMax);
            if (window->MinWidth > 0.0f || window->MinHeight > 0.0f)
            {
                const ImVec2 least(frameMin.x + Px(window->MinWidth > 0.0f ? window->MinWidth : 160.0f), frameMin.y + Px(window->MinHeight > 0.0f ? window->MinHeight : 100.0f));
                if (least.x < frameMax.x - 1.0f || least.y < frameMax.y - 1.0f)
                {
                    DashedRect(ImGui::GetWindowDrawList(), frameMin, least, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.55f)), 1.0f, Px(5.0f), Px(4.0f));
                    ImGui::GetWindowDrawList()->AddText(ImVec2(least.x - ImGui::CalcTextSize("min").x - Px(3.0f), least.y - ImGui::GetTextLineHeight() - Px(1.0f)),
                        ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.8f)), "min");
                }
            }
            FunctionChips(origin, areaMin, areaMax, frameMin, frameMax, titleHeight);
        }
        ImGui::EndChild();
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
            if (!best || rect.Depth > best->Depth || (rect.Depth == best->Depth && (rect.Max.x - rect.Min.x) * (rect.Max.y - rect.Min.y) < (best->Max.x - best->Min.x) * (best->Max.y - best->Min.y)))
                best = &rect;
        }
        DropSpot spot{ m_form.Window, Drop::Inside };
        if (!best)
            return spot;
        const UiNode* node = m_form.Model.Find(best->Node);
        const UiNode* parent = node ? m_form.Model.Find(node->Parent) : nullptr;
        if (!node || !parent)
            return spot;
        spot.Target = node->Id;
        const bool across = parent->Kind == UiKind::Row || parent->Kind == UiKind::Grid;
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
        const bool across = parent && (parent->Kind == UiKind::Row || parent->Kind == UiKind::Grid);
        const float thick = Px(3.0f);
        ImVec2 a;
        ImVec2 b;
        if (across)
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

    void FormEditorWindow::DropPayload(const ImGuiPayload* payload, const DropSpot& spot)
    {
        int parent = 0;
        int index = -1;
        if (!DropAt(spot.Target, spot.Where, parent, index))
            return;
        if (payload->IsDataType(kKindPayload))
        {
            AddInto(static_cast<UiKind>(*static_cast<const int*>(payload->Data)), parent, index);
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

    void FormEditorWindow::PreviewOverlay(ImVec2 min, ImVec2 max)
    {
        // ∑ The form can be panned partly out of the work surface; the overlay
        // (foreground list) must stop at the surface's edge like the body does.
        {
            const ImRect surface = ImGui::GetCurrentWindow()->InnerClipRect;
            min = ImVec2(std::max(min.x, surface.Min.x), std::max(min.y, surface.Min.y));
            max = ImVec2(std::min(max.x, surface.Max.x), std::min(max.y, surface.Max.y));
            max = ImVec2(std::max(max.x, min.x), std::max(max.y, min.y));
        }
        const ImGuiIO& io = ImGui::GetIO();
        const ImGuiID pick = ImGui::GetID("##previewpick");
        const bool inside = ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        int pointed = 0;
        if (inside)
        {
            int depth = -1;
            for (const UiDesign::Rect& rect : m_design.Rects)
            {
                if (io.MousePos.x >= rect.Min.x && io.MousePos.y >= rect.Min.y && io.MousePos.x < rect.Max.x && io.MousePos.y < rect.Max.y && rect.Depth > depth)
                {
                    pointed = rect.Node;
                    depth = rect.Depth;
                }
            }
            const bool onGrip = io.MousePos.x >= m_gripMin.x && io.MousePos.y >= m_gripMin.y && io.MousePos.x < m_gripMax.x && io.MousePos.y < m_gripMax.y;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload() && !onGrip)
            {
                Select(pointed ? pointed : m_form.Window);
                m_pressNode = pointed;
                m_pressing = true;
                ImGui::SetActiveID(pick, ImGui::GetCurrentWindow());
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                Select(pointed ? pointed : m_form.Window);
                ImGui::OpenPopup("##previewmenu");
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
        if (m_pressing && m_pressNode && m_pressNode != m_form.Window && ImGui::IsMouseDragging(ImGuiMouseButton_Left, Px(6.0f)))
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
        if (ImGui::BeginPopup("##previewmenu"))
        {
            NodeMenu(m_selected);
            ImGui::EndPopup();
        }
        ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
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
                if (!node)
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
        const bool dragging = payload && (payload->IsDataType(kKindPayload) || payload->IsDataType(kNodePayload));
        const ImU32 onAccent = ImGui::GetColorU32(ManifoldTheme::Page);
        const ImRect area = ImGui::GetCurrentWindow()->InnerClipRect;
        struct PendingTag
        {
            ImVec2 Min;
            std::string Text;
            ImU32 Background;
            float Right;
        };
        std::vector<PendingTag> tags;
        if (!dragging)
        {
            if (m_hovered && m_hovered != m_selected)
            {
                if (const UiDesign::Rect* rect = RectOf(m_hovered))
                {
                    DashedRect(draw, ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), ImVec2(rect->Max.x + 2.0f, rect->Max.y + 2.0f), soft, Px(1.5f), Px(5.0f), Px(3.0f));
                    tags.push_back({ ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), label(m_hovered), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.55f)), rect->Max.x + 2.0f });
                }
            }
            if (m_selected != m_form.Window)
            {
                outline(m_selected, accent, Px(2.0f));
                if (const UiDesign::Rect* rect = RectOf(m_selected))
                    tags.push_back({ ImVec2(rect->Min.x - 2.0f, rect->Min.y - 2.0f), label(m_selected), accent, 0.0f });
            }
        }
        else if (ImGui::IsMouseHoveringRect(min, max))
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
            if (spot.Target == m_form.Window)
                draw->AddRect(min, max, accent, 0.0f, 0, Px(2.0f));
            else
                DrawSpot(draw, spot);
            if (ImGui::BeginDragDropTargetCustom(ImRect(min, max), ImGui::GetID("##previewdrop")))
            {
                if (const ImGuiPayload* kind = ImGui::AcceptDragDropPayload(kKindPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                    DropPayload(kind, spot);
                if (const ImGuiPayload* moved = ImGui::AcceptDragDropPayload(kNodePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
                    DropPayload(moved, spot);
                ImGui::EndDragDropTarget();
            }
        }
        draw->PopClipRect();
        draw->PushClipRect(area.Min, area.Max, true);
        for (const PendingTag& tag : tags)
            Tag(draw, tag.Min, area.Min, area.Max, tag.Text, tag.Background, onAccent, tag.Right);
        draw->PopClipRect();
    }

    void FormEditorWindow::NameRow(UiNode& node)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if (node.Kind == UiKind::Window)
        {
            ImGui::TextUnformatted("Key");
            ItemTooltip("key: a window created with the same key replaces the earlier one, so running the script again does not open a second window.");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
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
        ItemTooltip("The widget's name in the script: w.<name>. Widgets the script reads or changes need one.");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        std::string name = m_form.Meta.count(node.Id) ? m_form.Meta[node.Id].Name : std::string();
        if (InputTextWithHint("##name", "no name", name, ImGuiInputTextFlags_CharsNoBlank))
        {
            Remember(ImGui::GetItemID());
            m_form.Meta[node.Id].Name = name;
            Changed();
        }
        if (!name.empty() && !Lua::FormIsIdentifier(name))
            TextWithError("Letters, digits and _ only, not starting with a digit", ManifoldTheme::Error);
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
        ImGui::PushStyleColor(ImGuiCol_Text, changed ? ManifoldTheme::Text : ManifoldTheme::Label);
        ImGui::TextUnformatted(TitleCase(prop.Key).c_str());
        ImGui::PopStyleColor();
        ItemTooltip((std::string(prop.Key) + ": " + std::string(prop.Help) + (changed ? "\nChanged from the default." : "")).c_str());
        if (resettable)
        {
            const float side = ImGui::GetFrameHeight() * 0.8f;
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
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        ImGui::Text("%s  %s", KindIcon(node->Kind), std::string(Lua::FormKindTitle(node->Kind)).c_str());
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        Muted(Lua::FormKindHelp(node->Kind));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::BeginChild("##props", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        ImGui::PushID(node->Id);
        const float labels = Px(118.0f);
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
        bool any = false;
        for (const Lua::FormProblem& problem : m_problems)
        {
            if (problem.Node != node->Id)
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
            Segmented("##style", &m_style, { "Standalone", "Script + Form File" }, Px(240.0f));
            ItemTooltip("Standalone: one script that builds the window with manifold.ui calls.\nScript + Form File: the layout stays in the .form.lua file, the script loads it with manifold.ui.form.load, so the layout can be changed here again later without touching the code.");
            ImGui::SameLine();
            if (IconButton(ICON_FA_COPY, "Copy the generated code"))
                ImGui::SetClipboardText(Code().c_str());
            ImGui::SameLine();
            if (IconButton(ICON_FA_FILE_EXPORT, "Export Script... (writes or updates a script file)"))
                Export();
            std::string code = Code();
            CodeEditor("##formcode", code, Language::Lua, ImVec2(0.0f, 0.0f), m_codeEditor);
        }
        ImGui::EndChild();
    }

    void FormEditorWindow::ScriptPane()
    {
        const std::filesystem::path script = LinkedScript();
        if (script.empty())
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
            SaveScript();
            OpenLinked(m_scriptEditor.CursorLine);
        }
        if (m_scriptConflict)
        {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ManifoldTheme::Warning, ICON_FA_EXCLAMATION_TRIANGLE "  Changed on disk too; saving keeps this version");
        }
        std::string text = m_form.ScriptText;
        if (CodeEditor("##scriptcode", text, Language::Lua, ImVec2(0.0f, 0.0f), m_scriptEditor) && text != m_form.ScriptText)
        {
            Remember(ImGui::GetID("##scriptcode"));
            m_form.ScriptText = std::move(text);
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
            const std::vector<Lua::FormProblem> problems = Lua::FormProblems(m_form);
            ImGui::BeginChild("##problemlist", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - Px(8.0f)), ImGuiChildFlags_Borders);
            if (problems.empty())
                Muted("No problems.");
            for (std::size_t i = 0; i < problems.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable((std::string(ICON_FA_EXCLAMATION_TRIANGLE "  ") + problems[i].Text).c_str()))
                    Select(problems[i].Node);
                ImGui::PopID();
            }
            ImGui::EndChild();
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
        visit(form.Window);
    }

    void FormEditorWindow::Copy()
    {
        const std::string text = m_selected == m_form.Window ? Lua::FormSave(m_form) : Lua::FormFragment(m_form, { m_selected });
        ImGui::SetClipboardText(text.c_str());
    }

    void FormEditorWindow::Cut()
    {
        if (m_selected == m_form.Window)
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
            m_engine.Notify(LogLevel::Warning, WideToUtf8(m_scriptPath.filename().wstring()) + " neither loads the form nor creates its window, so no function was added; write them yourself.");
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
        if (script.empty() || !std::filesystem::exists(script, missing))
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

    void FormEditorWindow::FunctionChips(ImVec2 origin, ImVec2 areaMin, ImVec2 areaMax, ImVec2 frameMin, ImVec2 frameMax, float titleHeight)
    {
        const std::vector<std::pair<std::string, std::string>> functions = Lua::FormHandlerList(m_form);
        if (functions.empty())
            return;
        EnsureScript();
        const bool linked = m_form.ScriptLoaded;
        const ImGuiIO& io = ImGui::GetIO();
        const float height = ImGui::GetFrameHeight() + Px(2.0f);
        const float gapY = Px(10.0f);
        float autoY = m_form.CanvasY;
        const float autoX = m_form.CanvasX + (frameMax.x - frameMin.x) / Px(1.0f) + 56.0f;
        std::map<std::string, std::pair<ImVec2, ImVec2>> rects;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        std::string hovered;
        for (const auto& [name, params] : functions)
        {
            ImVec2 place;
            if (const auto stored = m_form.Nodes.find(name); stored != m_form.Nodes.end())
                place = ImVec2(stored->second.X, stored->second.Y);
            else
            {
                place = ImVec2(autoX, autoY);
                autoY += (height + gapY) / Px(1.0f);
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
        ImDrawList* links = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
        const float bodyTop = frameMin.y + titleHeight;
        const ImVec2 innerMin(std::max(frameMin.x, areaMin.x), std::max(bodyTop, areaMin.y));
        const ImVec2 innerMax(std::min(frameMax.x, areaMax.x), std::min(frameMax.y, areaMax.y));
        const float formCenter = (frameMin.x + frameMax.x) * 0.5f;
        for (const auto& [id, meta] : m_form.Meta)
        {
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
                if (id != m_form.Window)
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
                if (strong && rect && innerMax.x > innerMin.x && innerMax.y > innerMin.y)
                {
                    const float from = right ? rect->Max.x + Px(3.0f) : rect->Min.x - Px(3.0f);
                    if ((port.x - from) * out > Px(4.0f))
                    {
                        links->PushClipRect(innerMin, innerMax, true);
                        DashedLine(links, ImVec2(from, y), port, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.55f)), 1.0f, Px(4.0f), Px(3.0f));
                        links->PopClipRect();
                    }
                }
            }
        }
        if (!hovered.empty())
        {
            for (const auto& [id, meta] : m_form.Meta)
            {
                for (const auto& [key, name] : meta.Handlers)
                {
                    if (name == hovered && id != m_form.Window)
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
        if (!m_form.Model.Find(m_selected))
            m_selected = m_form.Window;
        m_hovered = 0;
        m_problems = Lua::FormProblems(m_form);
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
