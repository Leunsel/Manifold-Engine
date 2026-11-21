#include "UI/FormEditor.h"

#include "App/Engine.h"
#include "Core/IconNames.h"
#include "Core/ImageDecode.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
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
        m_problemsDialog("formproblems", ICON_FA_EXCLAMATION_TRIANGLE, "Form Problems", ImVec2(560.0f, 320.0f))
    {
        m_host = std::make_unique<UiHost>(m_engine, workspace.Window(), &m_form.Model);
        m_tryHost = std::make_unique<UiHost>(m_engine, workspace.Window(), &m_try.Model);
        m_codeEditor.ReadOnly = true;
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
        ++m_revision;
    }

    std::string FormEditorWindow::Title() const
    {
        std::string title = ICON_FA_PENCIL_RULER "  Form Editor";
        if (!m_path.empty())
            title += "  -  " + WideToUtf8(m_path.filename().wstring());
        if (m_dirty)
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
        const std::string code = Lua::FormScript(m_form, Lua::FormScriptStyle::Standalone);
        const Status status = m_engine.Lua().Run(code, kRunChunk);
        if (!status)
            m_engine.Notify(status);
    }

    void FormEditorWindow::AskThen(Pending pending)
    {
        if (!m_dirty)
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
        m_engine.Notify(LogLevel::Success, "Saved " + WideToUtf8(path.filename().wstring()));
        return true;
    }

    void FormEditorWindow::Export()
    {
        const bool withFile = m_style == 1;
        if (withFile && (m_path.empty() || m_dirty) && !Save(false))
            return;
        std::error_code ignored;
        if (m_path.empty())
            std::filesystem::create_directories(Paths::AutorunFolder(), ignored);
        const std::filesystem::path folder = m_path.empty() ? Paths::AutorunFolder() : m_path.parent_path();
        const auto chosen = m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } }, WideToUtf8((folder / Utf8ToWide(ScriptFileName())).wstring()), L"lua");
        if (!chosen)
            return;
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

    void FormEditorWindow::Palette()
    {
        SectionLabel("Add");
        const float indent = ImGui::GetCursorPosX();
        bool first = true;
        for (const UiKind kind : Lua::FormKinds())
        {
            const std::string label = std::string(KindIcon(kind)) + "  " + std::string(Lua::FormKindTitle(kind)) + "##add" + std::string(Lua::UiKindName(kind));
            const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (!first)
                FlowNext(width, indent);
            first = false;
            if (ImGui::SmallButton(label.c_str()))
                Add(kind);
            if (ImGui::BeginDragDropSource())
            {
                const int value = static_cast<int>(kind);
                ImGui::SetDragDropPayload(kKindPayload, &value, sizeof(value));
                ImGui::TextUnformatted(label.c_str(), label.c_str() + label.find("##"));
                ImGui::EndDragDropSource();
            }
            ItemTooltip(std::string(Lua::FormKindHelp(kind)).append("\nClick adds it to the selected container, or drag it into the outline or the preview.").c_str());
        }
    }

    void FormEditorWindow::Outline()
    {
        SectionLabel("Outline");
        ImGui::BeginChild("##outline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
        OutlineNode(m_form.Window, 0);
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
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding;
        if (node->Children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (id == m_selected)
            flags |= ImGuiTreeNodeFlags_Selected;
        const std::string text = std::string(KindIcon(node->Kind)) + "  " + Caption(m_form, *node);
        if (!node->Visible)
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
        const bool open = ImGui::TreeNodeEx("##node", flags, "%s", text.c_str());
        if (!node->Visible)
            ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
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
        ItemTooltip("Runs the script in Manifold's Lua engine, so the real window opens. Running it again replaces that window.");
        const std::vector<Lua::FormProblem> problems = Lua::FormProblems(m_form);
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
    }

    void FormEditorWindow::Preview()
    {
        Toolbar();
        FormDocument& form = m_trying ? m_try : m_form;
        const UiNode* window = form.Model.Find(form.Window);
        if (!window)
            return;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Page);
        ImGui::BeginChild("##previewarea", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleColor();
        const ImVec2 size(Px(window->Width > 0.0f ? window->Width : 420.0f), Px(window->Height > 0.0f ? window->Height : 320.0f));
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - size.x) * 0.5f), ImGui::GetCursorPosY() + Px(4.0f)));
        const ImVec2 frameMin = ImGui::GetCursorScreenPos();
        const float titleHeight = ImGui::GetFrameHeight() + Px(4.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(frameMin, ImVec2(frameMin.x + size.x, frameMin.y + titleHeight), ImGui::GetColorU32(ManifoldTheme::TitleBar), Px(6.0f), ImDrawFlags_RoundCornersTop);
        const std::string title = UiHost::IconText(window->Icon, window->Title.empty() ? std::string("Script") : window->Title);
        draw->AddText(ImVec2(frameMin.x + Px(10.0f), frameMin.y + (titleHeight - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ManifoldTheme::Text), title.c_str());
        ImGui::Dummy(ImVec2(size.x, titleHeight));
        ImGui::SetCursorScreenPos(ImVec2(frameMin.x, frameMin.y + titleHeight));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::Border);
        ImGui::BeginChild("##previewbody", ImVec2(size.x, std::max(Px(40.0f), size.y - titleHeight)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
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
        ImGui::EndChild();
        if (!m_trying)
            PreviewOverlay(bodyMin, bodyMax);
        ImGui::Dummy(ImVec2(0.0f, Px(8.0f)));
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
        if (across)
        {
            const float x = spot.Where == Drop::Before ? rect->Min.x - thick : rect->Max.x + thick;
            draw->AddLine(ImVec2(x, rect->Min.y), ImVec2(x, rect->Max.y), accent, thick);
        }
        else
        {
            const float y = spot.Where == Drop::Before ? rect->Min.y - thick : rect->Max.y + thick;
            draw->AddLine(ImVec2(rect->Min.x, y), ImVec2(rect->Max.x, y), accent, thick);
        }
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
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload())
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
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        const bool dragging = payload && (payload->IsDataType(kKindPayload) || payload->IsDataType(kNodePayload));
        if (!dragging)
        {
            if (m_hovered && m_hovered != m_selected)
                outline(m_hovered, soft, Px(1.0f));
            if (m_selected != m_form.Window)
                outline(m_selected, accent, Px(2.0f));
        }
        else if (ImGui::IsMouseHoveringRect(min, max))
        {
            const int moving = payload->IsDataType(kNodePayload) ? *static_cast<const int*>(payload->Data) : 0;
            const DropSpot spot = SpotAt(io.MousePos, moving);
            if (moving)
                outline(moving, soft, Px(1.0f));
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
        if (changed)
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Text);
        else
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
        ImGui::TextUnformatted(TitleCase(prop.Key).c_str());
        ImGui::PopStyleColor();
        ItemTooltip((std::string(prop.Key) + ": " + std::string(prop.Help)).c_str());
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
            if (ImGui::Checkbox("##v", &flag))
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
        ImGui::SetNextItemWidth(-(button + ImGui::GetStyle().ItemInnerSpacing.x));
        const std::string key(handler.Key);
        std::string value;
        if (const auto meta = m_form.Meta.find(node.Id); meta != m_form.Meta.end())
        {
            if (const auto found = meta->second.Handlers.find(key); found != meta->second.Handlers.end())
                value = found->second;
        }
        const std::string before = value;
        bool edited = InputTextWithHint("##fn", "function name", value, ImGuiInputTextFlags_CharsNoBlank);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (IconButton(ICON_FA_MAGIC, "Name the handler after the widget", false, button))
        {
            std::string base = m_form.Meta.count(node.Id) ? m_form.Meta[node.Id].Name : std::string();
            if (base.empty())
                base = node.Kind == UiKind::Window ? std::string("window") : std::string(Lua::UiKindName(node.Kind));
            value = base + key.substr(2);
            edited = true;
        }
        if (edited && value != before)
        {
            Remember(ImGui::GetID("##fn"));
            if (value.empty())
                m_form.Meta[node.Id].Handlers.erase(key);
            else
                m_form.Meta[node.Id].Handlers[key] = value;
            Changed();
        }
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
        const float labels = Px(104.0f);
        if (ImGui::BeginTable("##proptable", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labels);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            NameRow(*node);
            for (const Lua::FormProp& prop : Lua::FormProps(node->Kind))
                PropertyRow(*node, prop);
            ImGui::EndTable();
        }
        const std::span<const Lua::FormHandler> handlers = Lua::FormHandlers(node->Kind);
        if (!handlers.empty())
        {
            ImGui::Spacing();
            SectionLabel("Handlers");
            if (ImGui::BeginTable("##handlertable", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
            {
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labels);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                for (const Lua::FormHandler& handler : handlers)
                    HandlerRow(*node, handler);
                ImGui::EndTable();
            }
        }
        bool any = false;
        for (const Lua::FormProblem& problem : Lua::FormProblems(m_form))
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
        Segmented("##style", &m_style, { "Standalone Script", "Script + Form File" }, Px(300.0f));
        ItemTooltip("Standalone: one script that builds the window with manifold.ui calls.\nScript + Form File: the layout stays in the .form.lua file, the script loads it with manifold.ui.form.load, so the layout can be changed here again later without touching the code.");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY "  Copy"))
            ImGui::SetClipboardText(Code().c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_EXPORT "  Export..."))
            Export();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CODE "  Open as Script"))
            m_workspace.OpenScript(Code(), kNoRecord, true);
        std::string code = Code();
        CodeEditor("##formcode", code, Language::Lua, ImVec2(0.0f, 0.0f), m_codeEditor);
        ImGui::EndChild();
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
                DoPending();
            }
            else if (choice == 3)
                m_pending = Pending::None;
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

    void FormEditorWindow::Draw()
    {
        if (!m_open)
            return;
        const std::string title = Title();
        if (!BeginWindow(title.c_str(), ImVec2(1240.0f, 760.0f), ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, ImVec2(820.0f, 460.0f)))
            return;
        if (!m_open && m_dirty)
        {
            m_open = true;
            AskThen(Pending::Close);
        }
        Menu();
        Shortcuts();
        if (!m_form.Model.Find(m_selected))
            m_selected = m_form.Window;
        m_hovered = 0;

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
        Palette();
        ImGui::Spacing();
        Outline();
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
