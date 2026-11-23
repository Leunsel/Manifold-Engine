#include "UI/TemplateManager.h"

#include "App/Commands.h"
#include "Asm/AsmAssist.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Lua/DocRecord.h"
#include "Lua/LuaEngine.h"
#include "Target/Process.h"
#include "Templates/TemplateEngine.h"
#include "Templates/TemplateGenerator.h"
#include "UI/Fonts.h"
#include "UI/LuaWindow.h"
#include "UI/ScriptWindow.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <optional>
#include <limits>
#include <memory>
#include <regex>
#include <set>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kDragType = "MANIFOLD_TEMPLATE";
        constexpr int kPreviewLimitMs = 2000;

        struct Node
        {
            std::string Label;
            std::string Path;
            double Order = std::numeric_limits<double>::infinity();
            std::vector<std::unique_ptr<Node>> Children;
            std::vector<const Templates::TemplateInfo*> Items;

            Node& Child(const std::string& label, const std::string& path)
            {
                for (const auto& child : Children)
                {
                    if (child->Path == path)
                        return *child;
                }
                Children.push_back(std::make_unique<Node>());
                Children.back()->Label = label;
                Children.back()->Path = path;
                return *Children.back();
            }
        };

        std::vector<std::string> SplitList(const std::string& text, char separator)
        {
            std::vector<std::string> parts;
            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t end = text.find(separator, start);
                const std::string part = Trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (!part.empty())
                    parts.push_back(part);
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            return parts;
        }

        std::string JoinList(const std::vector<std::string>& items)
        {
            std::string text;
            for (const std::string& item : items)
                text += (text.empty() ? "" : ", ") + item;
            return text;
        }

        std::string NumberText(const std::optional<double>& value)
        {
            if (!value)
                return {};
            return Templates::InputText(*value == static_cast<double>(static_cast<long long>(*value)) ? Templates::Value(static_cast<std::int64_t>(*value)) : Templates::Value(*value));
        }

        std::optional<double> ParseNumber(const std::string& text)
        {
            const std::string trimmed = Trim(text);
            if (trimmed.empty())
                return std::nullopt;
            char* end = nullptr;
            const double value = std::strtod(trimmed.c_str(), &end);
            if (!end || *end != '\0')
                return std::nullopt;
            return value;
        }

        bool ListField(const char* label, std::vector<std::string>& items)
        {
            std::string text = JoinList(items);
            if (InputText(label, text))
            {
                items = SplitList(text, ',');
                return true;
            }
            return false;
        }

        bool TriState(const char* label, std::optional<bool>& value)
        {
            static const char* kChoices[] = { "Default", "Yes", "No" };
            int current = !value ? 0 : (*value ? 1 : 2);
            bool changed = false;
            ImGui::SetNextItemWidth(Px(120.0f));
            if (ImGui::BeginCombo(label, kChoices[current]))
            {
                for (int index = 0; index < 3; ++index)
                {
                    if (ImGui::Selectable(kChoices[index], index == current))
                    {
                        value = index == 0 ? std::optional<bool>() : std::optional<bool>(index == 1);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        bool OptionalText(const char* label, std::optional<std::string>& value)
        {
            bool set = value.has_value();
            bool changed = false;
            ImGui::PushID(label);
            if (ImGui::Checkbox("##set", &set))
            {
                value = set ? std::optional<std::string>(std::string()) : std::nullopt;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!set);
            std::string text = value.value_or("");
            ImGui::SetNextItemWidth(Px(180.0f));
            if (InputText("##text", text) && set)
            {
                value = text;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            return changed;
        }

        void Label(const char* text)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
        }

        bool BeginFormSection(const char* title)
        {
            ImGui::SeparatorText(title);
            if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp))
                return false;
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Px(190.0f));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            return true;
        }

        // Check and generator messages name their line in running text, as
        // "(line 12)" or "on line 12".
        int MessageLine(const std::string& text)
        {
            static const std::regex pattern(R"([Ll]ine (\d+))");
            std::smatch match;
            if (!std::regex_search(text, match, pattern))
                return 0;
            return static_cast<int>(ParseDecimal(match[1].str()).value_or(0));
        }

        std::shared_ptr<const Lua::DocRecord> ValueRecord(const std::string& name, Lua::DocItemKind kind, const std::string& summary, const std::string& provider)
        {
            auto record = std::make_shared<Lua::DocRecord>();
            record->Name = name;
            record->Path = name;
            record->Kind = kind;
            record->Origin = Lua::DocOrigin::Manifold;
            record->Summary = summary;
            record->Text = summary + "\n\nProvided by " + provider + ".";
            return record;
        }

        std::shared_ptr<const Lua::DocRecord> HelperRecord(const Templates::HelperInfo& helper)
        {
            auto record = std::make_shared<Lua::DocRecord>();
            record->Name = helper.Name;
            record->Path = helper.Name;
            record->Kind = Lua::DocItemKind::Function;
            record->Origin = Lua::DocOrigin::Manifold;
            record->Summary = helper.Text;
            record->Text = helper.Text;
            Lua::DocSignature signature;
            for (const std::string& param : SplitList(helper.Params, ','))
            {
                Lua::DocParam entry;
                entry.Name = param;
                entry.Variadic = param == "...";
                signature.Params.push_back(std::move(entry));
            }
            record->Signatures.push_back(std::move(signature));
            return record;
        }

        std::vector<CodeSuggestion> Convert(std::vector<Lua::LuaCompletion> items)
        {
            std::vector<CodeSuggestion> result;
            result.reserve(items.size());
            for (Lua::LuaCompletion& item : items)
                result.push_back({ std::move(item.Name), std::move(item.Kind), std::move(item.Signature), std::move(item.Text), std::move(item.Doc) });
            return result;
        }
    }

    TemplateManager::TemplateManager(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()),
        m_unsavedDialog("templateunsaved", ICON_FA_SAVE, "Unsaved Changes"),
        m_deleteDialog("templatedelete", ICON_FA_TRASH, "Delete Template"),
        m_reportDialog("templatereport", ICON_FA_CHECK_DOUBLE, "Template Check", ImVec2(720.0f, 460.0f)),
        m_messageDialog("templatemessage", ICON_FA_SHAPES, "Templates")
    {
        // ∑ Inside a tag the code is the template's Lua, which sees the values
        // (HookName, AoBOffset), the template's inputs, the helpers and a few
        // standard libraries; the program's own Lua functions only when the
        // template's settings allow them. Around the tags it is Auto Assembler.
        Engine* engine = &m_engine;
        m_valueAssist.Complete = [this, engine](const std::string& chain, const std::string& prefix, bool method)
            {
                if (!chain.empty())
                    return Convert(engine->Lua().Complete(chain, prefix, method));
                std::vector<CodeSuggestion> items;
                std::set<std::string> seen;
                const auto add = [&](CodeSuggestion item)
                    {
                        if (seen.insert(item.Name).second)
                            items.push_back(std::move(item));
                    };
                if (m_draft)
                {
                    for (const Templates::InputDefinition& input : m_draft->Form.Inputs)
                        add({ input.Name, "property", "input, " + input.Type, input.Caption, ValueRecord(input.Name, Lua::DocItemKind::Property, input.Caption, "Input") });
                }
                for (const Templates::VariableInfo& variable : Templates::Variables())
                    add({ variable.Name, "property", variable.Provider, variable.Description, ValueRecord(variable.Name, Lua::DocItemKind::Property, variable.Description, variable.Provider) });
                for (const Templates::HelperInfo& helper : Templates::Helpers())
                    add({ helper.Name, "function", std::string("(") + helper.Params + ")", helper.Text, HelperRecord(helper) });
                for (const std::string& name : Templates::LuaBaseNames())
                    add({ name, name == "string" || name == "math" || name == "table" || name == "TemplateSettings" ? "table" : "function", {}, {}, engine->Lua().Describe(name, false) });
                for (const char* keyword : { "if", "then", "else", "elseif", "end", "for", "in", "do", "local", "not", "and", "or", "nil", "true", "false", "function", "return" })
                    add({ keyword, "keyword", {}, {}, nullptr });
                if (m_draft && m_draft->Form.AllowUnsafeGlobals)
                {
                    for (CodeSuggestion& item : Convert(engine->Lua().Complete({}, prefix, false)))
                        add(std::move(item));
                }
                return items;
            };
        m_valueAssist.Describe = [this, engine](const std::string& chain, bool method) -> std::shared_ptr<const Lua::DocRecord>
            {
                if (chain.find_first_of(".:") == std::string::npos)
                {
                    if (m_draft)
                    {
                        for (const Templates::InputDefinition& input : m_draft->Form.Inputs)
                        {
                            if (input.Name == chain)
                                return ValueRecord(input.Name, Lua::DocItemKind::Property, input.Caption + " (input, " + input.Type + ")", "Input");
                        }
                    }
                    for (const Templates::VariableInfo& variable : Templates::Variables())
                    {
                        if (chain == variable.Name)
                            return ValueRecord(variable.Name, Lua::DocItemKind::Property, variable.Description, variable.Provider);
                    }
                    for (const Templates::HelperInfo& helper : Templates::Helpers())
                    {
                        if (chain == helper.Name)
                            return HelperRecord(helper);
                    }
                }
                return engine->Lua().Describe(chain, method);
            };
        m_valueAssist.Signatures = [engine](const std::string& chain, bool method) { return engine->Lua().CallSignatures(chain, method); };
        m_valueAssist.Choices = [engine](const std::string& chain, bool method, int argument) { return engine->Lua().CallArgumentChoices(chain, method, argument); };
        m_valueAssist.ResolveType = [engine](const std::string& type, const std::string& scope) { return engine->Lua().ResolveDocType(type, scope); };
        m_valueAssist.CompleteClass = [engine](const std::string& className, const std::string& prefix, bool method) { return Convert(engine->Lua().CompleteClass(className, prefix, method)); };
        m_valueAssist.ClassMember = [engine](const std::string& className, const std::string& member) { return engine->Lua().ClassMember(className, member); };

        m_asmAssist = MakeAsmAssist(m_engine);
        m_asmAssist.Embedded = &m_valueAssist;
        m_asmAssist.UseEmbedded = [](const std::string& text, std::size_t cursor) { return Templates::InTag(text, cursor) || Asm::InLuaBlock(text, cursor); };
        ResetEditors(false);
    }

    Templates::Library& TemplateManager::Library()
    {
        return TemplateLibrary(m_engine);
    }

    std::filesystem::path TemplateManager::Root() const
    {
        return TemplateFolder();
    }

    const Templates::TemplateInfo* TemplateManager::Current()
    {
        return m_draft && !m_draft->IsNew ? Library().Find(m_draft->Id) : nullptr;
    }

    void TemplateManager::Message(std::string text)
    {
        m_message = std::move(text);
        m_messageDialog.Open();
    }

    void TemplateManager::Reload()
    {
        Library().Load(m_engine.Lua().State());
    }

    void TemplateManager::Select(const std::string& id)
    {
        RequestSelect(id, false);
    }

    void TemplateManager::NewTemplate(const std::string& script)
    {
        RequestSelect({}, true, script);
    }

    void TemplateManager::ResetEditors(bool readOnly)
    {
        m_scriptEditor = CodeEditorState{};
        m_rawEditor = CodeEditorState{};
        m_previewEditor = CodeEditorState{};
        m_scriptEditor.ReadOnly = readOnly;
        m_rawEditor.ReadOnly = readOnly;
        m_previewEditor.ReadOnly = true;
        m_scriptEditor.Assist = &m_asmAssist;
        m_previewInputs.clear();
        m_previewSignature.clear();
        m_seenSignature.clear();
        m_previewText.clear();
        m_previewFailed = false;
        m_problems.clear();
    }

    void TemplateManager::Load(const Templates::TemplateInfo& info)
    {
        Draft draft;
        draft.Id = info.Id;
        draft.BuiltIn = info.BuiltIn;
        if (const auto script = Library().Script(info))
            draft.Script = script->Text;
        draft.SavedScript = draft.Script;
        draft.Form = Templates::FormOf(info);
        draft.SettingsText = info.HasSettings ? info.SettingsText : Templates::WriteSettings(draft.Form);
        draft.SavedSettingsText = draft.SettingsText;
        draft.OriginalCategory = info.Category;
        draft.SettingsHadComments = info.SettingsHasComments;
        m_draft = std::move(draft);
        ResetEditors(info.BuiltIn);
        for (const Templates::InputDefinition& input : info.Inputs)
            m_previewInputs[input.Name] = Templates::InputText(input.Default);
    }

    void TemplateManager::RequestSelect(const std::string& id, bool fresh, const std::string& script)
    {
        m_pendingId = id;
        m_pendingNew = fresh;
        m_pendingScript = script;
        if (Dirty())
        {
            m_unsavedDialog.Open();
            return;
        }
        if (m_pendingNew)
        {
            Draft draft;
            draft.IsNew = true;
            draft.Form.SchemaVersion = 2;
            draft.Form.Caption = "New Template";
            draft.Form.Id = Templates::UniqueId(Library(), draft.Form.Caption);
            draft.Form.Category = m_draft && !m_draft->BuiltIn ? m_draft->Form.Category : "Templates";
            draft.Script = m_pendingScript.empty() ? "[ENABLE]\n\n[DISABLE]\n" : m_pendingScript;
            draft.SettingsText = Templates::WriteSettings(draft.Form);
            draft.OriginalCategory = draft.Form.Category;
            m_draft = std::move(draft);
            ResetEditors(false);
            m_requestTab = SettingsTabIndex;
        }
        else if (const Templates::TemplateInfo* info = Library().Find(m_pendingId))
            Load(*info);
        m_pendingId.clear();
        m_pendingNew = false;
        m_pendingScript.clear();
    }

    void TemplateManager::FormChanged()
    {
        m_draft->SettingsText = Templates::WriteSettings(m_draft->Form);
        m_draft->RawEdited = false;
        m_draft->SettingsError.clear();
    }

    bool TemplateManager::Dirty() const
    {
        if (!m_draft || m_draft->BuiltIn)
            return false;
        return m_draft->IsNew || m_draft->Script != m_draft->SavedScript || m_draft->SettingsText != m_draft->SavedSettingsText;
    }

    bool TemplateManager::Save()
    {
        if (!m_draft || m_draft->BuiltIn)
            return false;
        Draft& draft = *m_draft;
        lua_State* state = m_engine.Lua().State();
        std::string error;
        const auto parsed = Templates::ReadSettings(state, draft.SettingsText, "=settings", draft.Form.Caption, error);
        if (!parsed)
        {
            Message("The settings are not valid:\n\n" + error);
            return false;
        }
        const Templates::TemplateInfo* other = Library().Find(parsed->Id);
        if (other && !other->BuiltIn && (draft.IsNew || parsed->Id != draft.Id))
        {
            Message("The Id " + parsed->Id + " is already used by \"" + other->Caption + "\".");
            return false;
        }
        Status status = Status::Success();
        if (draft.IsNew)
        {
            Templates::SettingsForm form = draft.Form;
            if (!draft.RawEdited)
                status = Templates::CreateTemplate(Root(), form, draft.Script);
            else
            {
                std::filesystem::path created;
                status = Templates::CreateTemplate(Root(), form, draft.Script, &created);
                if (status)
                {
                    Templates::TemplateInfo fresh;
                    fresh.ScriptPath = created;
                    fresh.Name = WideToUtf8(created.stem().wstring());
                    status = Templates::SaveSettingsText(fresh, draft.SettingsText);
                }
            }
        }
        else
        {
            const Templates::TemplateInfo* info = Library().Find(draft.Id);
            if (!info)
            {
                Message("The template is gone from the folder. Save it again as a new template.");
                return false;
            }
            if (draft.Script != draft.SavedScript)
                status = Templates::SaveScript(*info, draft.Script);
            if (status && draft.SettingsText != draft.SavedSettingsText)
                status = Templates::SaveSettingsText(*info, draft.SettingsText);
            if (status && !draft.Form.ExplicitCategory && draft.Form.Category != draft.OriginalCategory)
            {
                Reload();
                if (const Templates::TemplateInfo* saved = Library().Find(parsed->Id))
                    status = Templates::MoveTemplate(*saved, Root(), draft.Form.Category);
            }
        }
        if (!status)
        {
            Message(status.Text);
            return false;
        }
        Reload();
        const std::string id = parsed->Id;
        // Saving keeps the place in the script and the preview's inputs.
        const int line = m_scriptEditor.CursorLine;
        const std::map<std::string, std::string> inputs = m_previewInputs;
        m_draft.reset();
        if (const Templates::TemplateInfo* saved = Library().Find(id))
        {
            Load(*saved);
            for (const auto& [name, value] : inputs)
                m_previewInputs[name] = value;
            m_scriptEditor.GotoLine = line;
        }
        return true;
    }

    void TemplateManager::Revert()
    {
        if (!m_draft)
            return;
        if (m_draft->IsNew)
            m_draft.reset();
        else if (const Templates::TemplateInfo* info = Library().Find(m_draft->Id))
            Load(*info);
    }

    void TemplateManager::Duplicate(const Templates::TemplateInfo& info)
    {
        std::string id;
        const Status status = Templates::DuplicateTemplate(Library(), info, Root(), false, &id);
        if (!status)
        {
            Message(status.Text);
            return;
        }
        Reload();
        RequestSelect(id, false);
    }

    void TemplateManager::EditCopy()
    {
        const Templates::TemplateInfo* info = Current();
        if (!info || !info->BuiltIn)
            return;
        const Status status = Templates::DuplicateTemplate(Library(), *info, Root(), true);
        if (!status)
        {
            Message(status.Text);
            return;
        }
        const std::string id = m_draft->Id;
        Reload();
        m_draft.reset();
        RequestSelect(id, false);
    }

    void TemplateManager::ImportFolder()
    {
        const auto folder = m_workspace.FolderDialog();
        if (!folder)
            return;
        int imported = 0;
        const Status status = Templates::ImportTemplates(*folder, Root(), &imported);
        Reload();
        Message(status ? std::to_string(imported) + (imported == 1 ? " template imported." : " templates imported.") + " Files that already exist were left alone." : status.Text);
    }

    void TemplateManager::OpenFolder()
    {
        std::error_code ignored;
        std::filesystem::create_directories(Root(), ignored);
        ::ShellExecuteW(nullptr, L"open", Root().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void TemplateManager::CheckAll()
    {
        m_report = Templates::CheckAll(m_engine.Lua().State(), Library());
        m_reportDialog.Open();
    }

    void TemplateManager::Menu()
    {
        if (!ImGui::BeginMenuBar())
            return;
        const Templates::TemplateInfo* current = Current();
        const bool builtIn = m_draft && m_draft->BuiltIn;
        if (ImGui::BeginMenu("Template"))
        {
            if (ImGui::MenuItem(ICON_FA_PLUS "  New Template"))
                RequestSelect({}, true);
            if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate as a New Template", nullptr, false, current != nullptr) && current)
                Duplicate(*current);
            if (builtIn && ImGui::MenuItem(ICON_FA_EDIT "  Edit a Copy"))
                EditCopy();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_SAVE "  Save", "Ctrl+S", false, Dirty()))
                Save();
            if (ImGui::MenuItem(ICON_FA_UNDO "  Revert Changes", nullptr, false, Dirty()))
                Revert();
            ImGui::Separator();
            if (DangerMenuItem(ICON_FA_TRASH "  Move to the Recycle Bin", nullptr, current && !current->BuiltIn))
                m_deleteDialog.Open();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CHECK_DOUBLE "  Check All Templates"))
                CheckAll();
            if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Import Templates from a Folder..."))
                ImportFolder();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open the Template Folder"))
                OpenFolder();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem(ICON_FA_LIST "  Library", nullptr, &m_showLibrary);
            ImGui::MenuItem(ICON_FA_TAGS "  Values", nullptr, &m_showValues);
            ImGui::MenuItem(ICON_FA_EYE "  Preview", nullptr, &m_showPreview);
            ImGui::Separator();
            ImGui::MenuItem("14-Byte Jumps in the Preview", nullptr, &m_preview14);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void TemplateManager::Shortcuts()
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            return;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_S, false) && Dirty())
            Save();
    }

    void TemplateManager::LibraryPane()
    {
        const float side = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(-(side + ImGui::GetStyle().ItemSpacing.x));
        InputTextWithHint("##filter", ICON_FA_SEARCH "  Search", m_filter);
        ImGui::SameLine();
        if (IconButton(ICON_FA_PLUS, "New Template"))
            RequestSelect({}, true);
        Tree();
    }

    void TemplateManager::Tree()
    {
        Templates::Library& library = Library();
        Node root;
        const std::string filter = Lower(m_filter);
        for (const Templates::TemplateInfo& info : library.Templates())
        {
            if (!filter.empty() && Lower(info.Caption + " " + info.Id + " " + info.Category + " " + info.Description).find(filter) == std::string::npos)
                continue;
            Node* node = &root;
            std::string path;
            for (const std::string& segment : SplitList(info.Category.empty() ? std::string("Templates") : info.Category, '>'))
            {
                std::optional<double> order;
                const std::string label = Templates::CategoryLabel(segment, &order);
                path += (path.empty() ? "" : " > ") + segment;
                node = &node->Child(label.empty() ? segment : label, path);
                if (order)
                    node->Order = std::min(node->Order, *order);
                if (info.CategoryOrder)
                    node->Order = std::min(node->Order, *info.CategoryOrder);
            }
            node->Items.push_back(&info);
        }

        std::string moveId;
        std::string moveTo;
        const Templates::TemplateInfo* duplicate = nullptr;
        auto drop = [&](const std::string& path)
        {
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kDragType))
                {
                    moveId.assign(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize));
                    moveTo = path;
                }
                ImGui::EndDragDropTarget();
            }
        };
        auto draw = [&](auto& self, const Node& node) -> void
        {
            std::vector<const Node*> children;
            for (const auto& child : node.Children)
                children.push_back(child.get());
            std::stable_sort(children.begin(), children.end(), [](const Node* a, const Node* b)
                {
                    if (a->Order != b->Order)
                        return a->Order < b->Order;
                    return Lower(a->Label) < Lower(b->Label);
                });
            for (const Node* child : children)
            {
                ImGui::PushID(child->Path.c_str());
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (!filter.empty())
                    ImGui::SetNextItemOpen(true);
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
                const bool open = Layout::TreeNode("##node", ICON_FA_FOLDER "  " + child->Label, flags);
                ImGui::PopStyleColor();
                drop(child->Path);
                if (open)
                {
                    self(self, *child);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            for (const Templates::TemplateInfo* info : node.Items)
            {
                ImGui::PushID(info->Id.c_str());
                const bool selected = m_draft && !m_draft->IsNew && m_draft->Id == info->Id;
                const bool changed = selected && Dirty();
                const float x = ImGui::GetCursorPosX();
                if (ImGui::Selectable("##item", selected))
                {
                    if (!selected)
                        RequestSelect(info->Id, false);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                {
                    ShowTooltip([&]()
                        {
                            ImGui::PushFont(Fonts::Bold());
                            ImGui::TextUnformatted(info->Caption.c_str());
                            ImGui::PopFont();
                            ImGui::PushTextWrapPos(Px(380.0f));
                            if (!info->Description.empty())
                                ImGui::TextUnformatted(info->Description.c_str());
                            ImGui::PopTextWrapPos();
                            Muted(info->BuiltIn ? "Built in, read only" : WideToUtf8(info->ScriptPath.filename().wstring()));
                        });
                }
                if (!info->BuiltIn && ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload(kDragType, info->Id.data(), info->Id.size());
                    ImGui::TextUnformatted(info->Caption.c_str());
                    Muted("Drop on a category to move it there");
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("##itemmenu"))
                {
                    if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate as a New Template"))
                        duplicate = info;
                    if (!info->BuiltIn && ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in Explorer"))
                    {
                        const std::wstring arguments = L"/select,\"" + info->ScriptPath.wstring() + L"\"";
                        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine(x);
                // ∑ the caption gives way to the marker after it; its tooltip is the item's own (caption and description)
                const char* marker = changed ? "\xE2\x97\x8F" : info->BuiltIn ? "built-in" : nullptr;
                const float markerWidth = marker ? ImGui::CalcTextSize(marker).x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
                Layout::TextStyle caption;
                caption.MaxWidth = std::max(1.0f, Layout::Avail() - markerWidth);
                caption.Tooltip = false;
                Layout::Text(info->Caption, caption);
                if (marker)
                {
                    ImGui::SameLine();
                    if (changed)
                        Layout::Colored(marker, ManifoldTheme::Warning, Layout::Overflow::Visible);
                    else
                        Layout::Muted(marker, Layout::Overflow::Visible);
                }
                ImGui::PopID();
            }
        };

        ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        draw(draw, root);
        if (root.Children.empty() && root.Items.empty())
            Muted(filter.empty() ? "No templates yet." : "No template matches the search.");
        if (!library.Problems().empty())
        {
            ImGui::Spacing();
            const std::string label = ICON_FA_EXCLAMATION_TRIANGLE "  Skipped (" + std::to_string(library.Problems().size()) + ")";
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            const bool open = Layout::TreeNode("##skipped", label, ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            if (open)
            {
                for (const Templates::LibraryProblem& problem : library.Problems())
                {
                    Layout::TextStyle style;
                    style.Color = ManifoldTheme::Error;
                    style.Tooltip = false;
                    Layout::Text(WideToUtf8(std::filesystem::path(Utf8ToWide(problem.File)).filename().wstring()), style);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                        Layout::TextTooltip(problem.File + "\n" + problem.Message);
                }
                ImGui::TreePop();
            }
        }
        // The rest of the pane takes drops for the top level.
        const ImVec2 rest = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, Px(24.0f))));
        drop("Templates");
        ImGui::EndChild();

        if (duplicate)
            Duplicate(*duplicate);
        if (!moveId.empty())
        {
            if (const Templates::TemplateInfo* info = library.Find(moveId))
            {
                if (m_draft && m_draft->Id == moveId && Dirty())
                    Message("Save or revert the changes to \"" + info->Caption + "\" before moving it.");
                else
                {
                    const Status status = Templates::MoveTemplate(*info, Root(), moveTo);
                    if (!status)
                        Message(status.Text);
                    Reload();
                    if (m_draft && m_draft->Id == moveId)
                    {
                        m_draft.reset();
                        RequestSelect(moveId, false);
                    }
                }
            }
        }
    }

    void TemplateManager::ValuesPanel(float width, float height)
    {
        ImGui::BeginChild("##values", ImVec2(width, height), ImGuiChildFlags_Borders);
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputTextWithHint("##valuefilter", ICON_FA_SEARCH "  Find a Value", m_valueFilter);
        const std::string filter = Lower(m_valueFilter);
        const bool editable = !m_scriptEditor.ReadOnly;
        // A click puts the name into the script: bare inside a tag, as << Name >> anywhere else.
        auto entry = [&](const std::string& name, const std::string& tip, bool call)
        {
            if (!filter.empty() && Lower(name).find(filter) == std::string::npos)
                return;
            ImGui::PushStyleColor(ImGuiCol_Text, call ? ManifoldTheme::CodeRegister : ManifoldTheme::AddressText);
            const bool clicked = ImGui::Selectable((name + (call ? "()" : "")).c_str());
            ImGui::PopStyleColor();
            if (clicked && editable && m_draft)
            {
                const bool inside = Templates::InTag(m_draft->Script, static_cast<std::size_t>(std::max(0, m_scriptEditor.CursorOffset)));
                const std::string text = call ? name + "()" : name;
                m_scriptEditor.Insert = inside ? text : "<< " + text + " >>";
                m_scriptEditor.Focus = true;
                m_scriptEditor.KeepCursor = true;
            }
            if (ImGui::IsItemHovered() && !tip.empty())
            {
                ShowTooltip([&]()
                    {
                        ImGui::PushTextWrapPos(Px(340.0f));
                        ImGui::TextUnformatted(tip.c_str());
                        ImGui::PopTextWrapPos();
                        Muted(editable ? "Click to put it into the script" : "Built-in templates are read only");
                    });
            }
        };
        auto group = [&](const std::string& title, const std::function<void()>& body)
        {
            if (!filter.empty())
            {
                body();
                return;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            const bool open = Layout::TreeNode(title.c_str(), title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            ImGui::PopStyleColor();
            if (open)
                body();
        };

        ImGui::BeginChild("##valuelist", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        if (m_draft && !m_draft->Form.Inputs.empty())
        {
            group("Inputs", [&]()
                {
                    for (const Templates::InputDefinition& input : m_draft->Form.Inputs)
                        entry(input.Name, input.Caption + " (" + input.Type + ")", false);
                });
        }
        std::vector<std::string> providers;
        for (const Templates::VariableInfo& variable : Templates::Variables())
        {
            if (std::find(providers.begin(), providers.end(), variable.Provider) == providers.end())
                providers.push_back(variable.Provider);
        }
        for (const std::string& provider : providers)
        {
            group(provider, [&]()
                {
                    for (const Templates::VariableInfo& variable : Templates::Variables())
                    {
                        if (provider == variable.Provider)
                            entry(variable.Name, variable.Description, false);
                    }
                });
        }
        group("Helpers", [&]()
            {
                for (const Templates::HelperInfo& helper : Templates::Helpers())
                    entry(helper.Name, std::string(helper.Name) + "(" + helper.Params + ")\n" + helper.Text, true);
            });
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void TemplateManager::ScriptTab()
    {
        Draft& draft = *m_draft;
        const float footer = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 start = ImGui::GetCursorPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float height = std::max(available.y - footer, Px(80.0f));
        float editorWidth = available.x;
        if (m_showValues)
        {
            const float gap = Px(6.0f);
            const float minValues = Px(150.0f);
            const float minEditor = Px(240.0f);
            if (m_valuesWidth <= 0.0f)
                m_valuesWidth = Px(210.0f);
            m_valuesWidth = std::clamp(m_valuesWidth, minValues, std::max(minValues, available.x - minEditor - gap));
            editorWidth = available.x - m_valuesWidth - gap;
            CodeEditor("##templatescript", draft.Script, Language::Template, ImVec2(editorWidth, height), m_scriptEditor);
            float first = editorWidth;
            float second = m_valuesWidth;
            ImGui::SetCursorPos(start);
            if (Splitter("##valuesplit", true, gap, &first, &second, minEditor, minValues, height))
                m_valuesWidth = second;
            ImGui::SetCursorPos(ImVec2(start.x + editorWidth + gap, start.y));
            ValuesPanel(m_valuesWidth, height);
        }
        else
            CodeEditor("##templatescript", draft.Script, Language::Template, ImVec2(editorWidth, height), m_scriptEditor);

        ImGui::SetCursorPos(ImVec2(start.x, start.y + height + ImGui::GetStyle().ItemSpacing.y));
        std::string status = "Line " + std::to_string(m_scriptEditor.CursorLine) + ", Column " + std::to_string(m_scriptEditor.CursorColumn);
        if (!draft.BuiltIn)
        {
            const char* complete = ShortcutText(Command::EditorComplete);
            status += std::string("   \xC2\xB7   ") + (complete && *complete ? complete : "Ctrl+Space") + " suggests values, inputs and helpers inside << >> and <% %>";
        }
        TextFitted(status, available.x, ManifoldTheme::Muted);
    }

    void TemplateManager::SettingsTab()
    {
        Draft& draft = *m_draft;
        Templates::SettingsForm& form = draft.Form;
        ImGui::BeginChild("##settings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        ImGui::BeginDisabled(draft.BuiltIn);
        bool changed = false;
        if (draft.SettingsHadComments && !draft.RawEdited)
            TextWithError("The settings file has comments. Saving from this form writes it anew without them; edit the Settings File tab to keep them.", ManifoldTheme::Warning);
        if (!draft.SettingsError.empty())
            TextWithError(draft.SettingsError, ManifoldTheme::Error);

        if (BeginFormSection("General"))
        {
            Label("Caption");
            changed |= InputText("##caption", form.Caption);
            Label("Id");
            changed |= InputText("##id", form.Id);
            Label("Description");
            changed |= InputText("##description", form.Description);
            Label("Category");
            changed |= InputText("##category", form.Category);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Levels separated by >, like x86/x64 > Pointer Hooks.");
            Label("");
            changed |= ImGui::Checkbox("Keep the Category in the Settings File", &form.ExplicitCategory);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off: the template file lives in the folder of its category.\nOn: the category is written into the settings, wherever the file lives.");
            Label("Author");
            changed |= InputText("##author", form.Author);
            Label("Version");
            changed |= InputText("##version", form.Version);
            Label("Tags");
            changed |= ListField("##tags", form.Tags);
            ImGui::EndTable();
        }

        if (BeginFormSection("Menu"))
        {
            Label("Show at the Top of the Menu");
            bool top = !form.InSubMenu;
            if (ImGui::Checkbox("##top", &top))
            {
                form.InSubMenu = !top;
                changed = true;
            }
            std::string order = NumberText(form.Order);
            Label("Order");
            if (InputText("##order", order))
            {
                form.Order = ParseNumber(order);
                changed = true;
            }
            std::string categoryOrder = NumberText(form.CategoryOrder);
            Label("Category Order");
            if (InputText("##categoryorder", categoryOrder))
            {
                form.CategoryOrder = ParseNumber(categoryOrder);
                changed = true;
            }
            ImGui::EndTable();
        }

        if (BeginFormSection("Requirements"))
        {
            Label("Requires");
            changed |= ListField("##requires", form.Requires);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Values the template cannot do without, separated by commas.");
            Label("Optional");
            changed |= ListField("##optional", form.Optional);
            Label("Architectures");
            for (const char* architecture : { "x86", "x64" })
            {
                bool on = std::find(form.Architectures.begin(), form.Architectures.end(), architecture) != form.Architectures.end();
                if (ImGui::Checkbox(architecture, &on))
                {
                    if (on)
                        form.Architectures.push_back(architecture);
                    else
                        form.Architectures.erase(std::remove(form.Architectures.begin(), form.Architectures.end(), architecture), form.Architectures.end());
                    changed = true;
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
            Label("Needs");
            for (const char* capability : { "Manifold.AssemblerCommands", "Manifold.Trampolines" })
            {
                bool on = std::find(form.Capabilities.begin(), form.Capabilities.end(), capability) != form.Capabilities.end();
                if (ImGui::Checkbox(capability, &on))
                {
                    if (on)
                        form.Capabilities.push_back(capability);
                    else
                        form.Capabilities.erase(std::remove(form.Capabilities.begin(), form.Capabilities.end(), capability), form.Capabilities.end());
                    changed = true;
                }
            }
            Label("Program Lua Functions");
            changed |= ImGui::Checkbox("##globals", &form.AllowUnsafeGlobals);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Lets the template code call the program's Lua functions.");
            ImGui::EndTable();
        }

        if (BeginFormSection("Hook"))
        {
            Label("Ask for the Address");
            changed |= TriState("##askaddress", form.Memory.AskForInjectionAddress);
            Label("Ask for the Hook Name");
            changed |= TriState("##askhook", form.Memory.AskForHookName);
            Label("Default Hook Name");
            changed |= OptionalText("defaulthook", form.Memory.DefaultHookName);
            Label("Hook Name Suffix");
            changed |= OptionalText("suffix", form.Memory.AppendToHookName);
            Label("Allocation Size");
            changed |= OptionalText("allocation", form.Memory.AllocationSize);
            Label("Allocate Near the Hook");
            changed |= TriState("##near", form.Memory.AllocationNear);
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Inputs");
        Muted("Asked for when the template runs; the script reads each one by its name.");
        int removed = -1;
        if (!form.Inputs.empty() && ImGui::BeginTable("##inputs", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(90.0f));
            ImGui::TableSetupColumn("Caption");
            ImGui::TableSetupColumn("Default");
            ImGui::TableSetupColumn("Values or Range");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < form.Inputs.size(); ++index)
            {
                Templates::InputDefinition& input = form.Inputs[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= InputText("##name", input.Name);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##type", input.Type.c_str()))
                {
                    for (const char* type : { "string", "boolean", "integer", "number", "enum" })
                    {
                        if (ImGui::Selectable(type, input.Type == type) && input.Type != type)
                        {
                            input.Type = type;
                            if (input.Type == "boolean")
                                input.Default = false;
                            else if (input.Type == "integer")
                                input.Default = std::int64_t(0);
                            else if (input.Type == "number")
                                input.Default = 0.0;
                            else if (input.Type == "enum")
                            {
                                if (input.Values.empty())
                                    input.Values = { "A", "B" };
                                input.Default = input.Values.front();
                            }
                            else
                                input.Default = std::string();
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                changed |= InputText("##caption", input.Caption);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (input.Type == "boolean")
                {
                    bool value = std::holds_alternative<bool>(input.Default) && std::get<bool>(input.Default);
                    if (ImGui::Checkbox("##default", &value))
                    {
                        input.Default = value;
                        changed = true;
                    }
                }
                else if (input.Type == "enum")
                {
                    const std::string current = Templates::InputText(input.Default);
                    if (ImGui::BeginCombo("##default", current.c_str()))
                    {
                        for (const std::string& value : input.Values)
                        {
                            if (ImGui::Selectable(value.c_str(), value == current))
                            {
                                input.Default = value;
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }
                else
                {
                    std::string text = Templates::InputText(input.Default);
                    if (InputText("##default", text))
                    {
                        Templates::InputDefinition loose = input;
                        loose.Min.reset();
                        loose.Max.reset();
                        std::string ignored;
                        if (const auto value = Templates::ParseInput(loose, text, ignored))
                            input.Default = *value;
                        else if (input.Type == "string")
                            input.Default = text;
                        changed = true;
                    }
                }
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (input.Type == "enum")
                {
                    if (ListField("##values", input.Values))
                    {
                        if (input.Values.empty())
                            input.Values = { "A" };
                        if (std::find(input.Values.begin(), input.Values.end(), Templates::InputText(input.Default)) == input.Values.end())
                            input.Default = input.Values.front();
                        changed = true;
                    }
                }
                else if (input.Type == "integer" || input.Type == "number")
                {
                    std::string range = NumberText(input.Min) + ".." + NumberText(input.Max);
                    if (InputText("##range", range))
                    {
                        const std::size_t dots = range.find("..");
                        input.Min = ParseNumber(dots == std::string::npos ? range : range.substr(0, dots));
                        input.Max = dots == std::string::npos ? std::nullopt : ParseNumber(range.substr(dots + 2));
                        changed = true;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Smallest and largest value, like 1..16. Leave a side empty for no limit.");
                }
                ImGui::TableNextColumn();
                if (IconButton(ICON_FA_TIMES, "Remove This Input", true))
                    removed = static_cast<int>(index);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (removed >= 0)
        {
            form.Inputs.erase(form.Inputs.begin() + removed);
            changed = true;
        }
        if (ImGui::Button(ICON_FA_PLUS "  Add Input"))
        {
            Templates::InputDefinition input;
            input.Name = "Input" + std::to_string(form.Inputs.size() + 1);
            input.Caption = input.Name;
            input.Default = std::string();
            form.Inputs.push_back(input);
            changed = true;
        }
        if (!form.CustomFields.empty())
        {
            ImGui::SeparatorText("Other Fields");
            Muted("Kept as they are; the template reads them as TemplateSettings.<name>.");
            for (const auto& [key, value] : form.CustomFields)
                ImGui::Text("%s = %s", key.c_str(), value.c_str());
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        if (changed)
            FormChanged();
    }

    void TemplateManager::RawTab()
    {
        Draft& draft = *m_draft;
        if (!draft.SettingsError.empty())
            TextWithError(draft.SettingsError, ManifoldTheme::Error);
        const std::string before = draft.SettingsText;
        CodeEditor("##templatesettings", draft.SettingsText, Language::Lua, ImGui::GetContentRegionAvail(), m_rawEditor);
        if (draft.SettingsText == before)
            return;
        draft.RawEdited = true;
        std::string error;
        const auto parsed = Templates::ReadSettings(m_engine.Lua().State(), draft.SettingsText, "=settings", draft.Form.Caption, error);
        if (!parsed)
        {
            draft.SettingsError = error;
            return;
        }
        draft.SettingsError.clear();
        Templates::SettingsForm form = Templates::FormOf(*parsed);
        if (!parsed->ExplicitCategory)
            form.Category = draft.Form.Category;
        draft.Form = std::move(form);
    }

    // ∑ Generating runs the template's Lua, so it waits until typing pauses
    // for 0.3 s; it runs even while the preview is hidden, because the
    // problems and the error line in the script come from it too.
    void TemplateManager::RefreshPreview()
    {
        if (!m_draft)
            return;
        Draft& draft = *m_draft;
        const bool live = m_engine.HasProcess() && m_workspace.LastCodeAddress != 0;
        for (const Templates::InputDefinition& input : draft.Form.Inputs)
        {
            if (!m_previewInputs.count(input.Name))
                m_previewInputs[input.Name] = Templates::InputText(input.Default);
        }
        std::string signature = draft.Script + '\x1f' + draft.SettingsText + '\x1f' + (live ? Hex(m_workspace.LastCodeAddress) : "sample") + (m_preview14 ? "w" : "n");
        for (const auto& [name, value] : m_previewInputs)
            signature += '\x1f' + name + '=' + value;
        const double now = ImGui::GetTime();
        if (signature != m_seenSignature)
        {
            m_seenSignature = signature;
            m_changedAt = now;
        }
        if (m_seenSignature == m_previewSignature || (!m_previewSignature.empty() && now - m_changedAt < 0.3))
            return;
        m_previewSignature = m_seenSignature;
        m_problems.clear();
        m_scriptEditor.ErrorLine = 0;
        std::set<std::string> seen;
        const auto add = [&](std::string text, bool error, int line, int tab, std::string detail = {})
            {
                if (seen.insert(text).second)
                    m_problems.push_back({ std::move(text), std::move(detail), error, line, tab });
            };

        lua_State* state = m_engine.Lua().State();
        std::string error;
        auto parsed = Templates::ReadSettings(state, draft.SettingsText, "=settings", draft.Form.Caption, error);
        if (!parsed)
        {
            m_previewFailed = true;
            add("The settings are not valid: " + error, true, 0, SettingsFileTabIndex);
            return;
        }
        const Templates::TemplateInfo* saved = draft.IsNew ? nullptr : Library().Find(draft.Id);
        parsed->SourceName = saved ? saved->SourceName : "new template";
        parsed->Name = saved ? saved->Name : draft.Form.Caption;
        Templates::GenerationRequest request;
        request.SampleValues = true;
        request.Address = live ? m_workspace.LastCodeAddress : 0;
        request.Use14ByteJump = m_preview14;
        request.Script = draft.Script;
        for (const Templates::InputDefinition& input : parsed->Inputs)
        {
            const auto found = m_previewInputs.find(input.Name);
            if (found == m_previewInputs.end())
                continue;
            std::string inputError;
            if (const auto value = Templates::ParseInput(input, found->second, inputError))
                request.Inputs[input.Name] = *value;
            else
                add("Preview input " + input.Name + ": " + inputError, false, 0, ScriptTabIndex);
        }
        // ∑ The preview runs the template's Lua on the UI thread as the user types; a loop in it must not hang the program.
        std::optional<Lua::LuaEngine::TimeLimit> limit(std::in_place, m_engine.Lua(), kPreviewLimitMs, "the preview stops templates that run longer; Generate runs them without a limit");
        const Templates::GenerationResult result = Templates::Generate(m_engine, Library(), *parsed, request);
        limit.reset();
        m_previewFailed = !result.Text;
        int failureLine = 0;
        if (result.Text)
            m_previewText = *result.Text;
        else
        {
            failureLine = result.Failure.Source == parsed->SourceName ? result.Failure.Line : 0;
            if (failureLine > 0)
                m_scriptEditor.ErrorLine = failureLine;
            const std::string summary = (result.Failure.Variable.empty() ? std::string() : result.Failure.Variable + ": ") + result.Failure.Message;
            add(summary.empty() ? std::string("The template failed") : summary, true, failureLine, ScriptTabIndex, Templates::Describe(result.Failure, parsed->Caption));
        }
        // The check compiles the template again and finds the same syntax
        // error the generator stopped at; one entry per line is enough.
        const Templates::CheckEntry check = Templates::CheckTemplate(state, Library(), *parsed, &draft.Script);
        for (const std::string& text : check.Errors)
        {
            const int line = MessageLine(text);
            if (line == 0 || line != failureLine)
                add(text, true, line, ScriptTabIndex);
        }
        for (const std::string& text : result.Warnings)
            add(text, false, MessageLine(text), ScriptTabIndex);
        for (const std::string& text : check.Warnings)
            add(text, false, MessageLine(text), ScriptTabIndex);
    }

    void TemplateManager::JumpTo(const Problem& problem)
    {
        m_requestTab = problem.Tab;
        CodeEditorState& editor = problem.Tab == SettingsFileTabIndex ? m_rawEditor : m_scriptEditor;
        if (problem.Line > 0)
            editor.GotoLine = problem.Line;
        editor.Focus = true;
    }

    void TemplateManager::PreviewInputs()
    {
        Draft& draft = *m_draft;
        if (draft.Form.Inputs.empty())
            return;
        if (!ImGui::BeginTable("##previewinputs", 2, ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Px(130.0f));
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (const Templates::InputDefinition& input : draft.Form.Inputs)
        {
            std::string& text = m_previewInputs[input.Name];
            ImGui::PushID(input.Name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            TextFitted(input.Caption.empty() ? input.Name : input.Caption, ImGui::GetContentRegionAvail().x, ManifoldTheme::Label);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s (%s)", input.Name.c_str(), input.Type.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (input.Type == "boolean")
            {
                bool value = Lower(Trim(text)) == "true";
                if (ImGui::Checkbox("##value", &value))
                    text = value ? "true" : "false";
            }
            else if (input.Type == "enum" && !input.Values.empty())
            {
                if (ImGui::BeginCombo("##value", text.c_str()))
                {
                    for (const std::string& value : input.Values)
                    {
                        if (ImGui::Selectable(value.c_str(), value == text))
                            text = value;
                    }
                    ImGui::EndCombo();
                }
            }
            else
                InputText("##value", text);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void TemplateManager::ProblemList(float height)
    {
        ImGui::BeginChild("##problems", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        int errors = 0;
        for (const Problem& problem : m_problems)
            errors += problem.Error ? 1 : 0;
        const int warnings = static_cast<int>(m_problems.size()) - errors;
        std::string title = "Problems";
        if (errors)
            title += "   " ICON_FA_TIMES_CIRCLE " " + std::to_string(errors);
        if (warnings)
            title += "   " ICON_FA_EXCLAMATION_TRIANGLE " " + std::to_string(warnings);
        Muted(title);
        const float lineWidth = ImGui::CalcTextSize("Line 0000").x;
        for (std::size_t index = 0; index < m_problems.size(); ++index)
        {
            const Problem& problem = m_problems[index];
            ImGui::PushID(static_cast<int>(index));
            const float x = ImGui::GetCursorPosX();
            const float width = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable("##problem", false))
                JumpTo(problem);
            if (ImGui::IsItemHovered())
            {
                ShowTooltip([&]()
                    {
                        ImGui::PushTextWrapPos(Px(460.0f));
                        ImGui::TextUnformatted(problem.Detail.empty() ? problem.Text.c_str() : problem.Detail.c_str());
                        ImGui::PopTextWrapPos();
                        Muted(problem.Line > 0 ? "Click to go to line " + std::to_string(problem.Line) : problem.Tab == SettingsFileTabIndex ? "Click to open the settings file" : "Click to go to the script");
                    });
            }
            ImGui::SameLine(x);
            ImGui::TextColored(problem.Error ? ManifoldTheme::Error : ManifoldTheme::Warning, "%s", problem.Error ? ICON_FA_TIMES_CIRCLE : ICON_FA_EXCLAMATION_TRIANGLE);
            ImGui::SameLine();
            const float textWidth = width - (ImGui::GetCursorPosX() - x) - (problem.Line > 0 ? lineWidth : 0.0f);
            const std::size_t newline = problem.Text.find('\n');
            TextFitted(newline == std::string::npos ? problem.Text : problem.Text.substr(0, newline), textWidth, ManifoldTheme::Text);
            if (problem.Line > 0)
            {
                ImGui::SameLine(x + width - lineWidth);
                Muted("Line " + std::to_string(problem.Line));
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    void TemplateManager::PreviewPane()
    {
        const bool live = m_engine.HasProcess() && m_workspace.LastCodeAddress != 0;
        const float side = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(Fonts::Bold());
        ImGui::TextUnformatted("Preview");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - side * 3.0f - spacing * 2.0f);
        ImGui::BeginDisabled(m_previewText.empty());
        if (IconButton(ICON_FA_CLIPBOARD, "Copy the Generated Script"))
            ImGui::SetClipboardText(m_previewText.c_str());
        ImGui::SameLine();
        if (IconButton(ICON_FA_EXTERNAL_LINK_ALT, "Open It in an Auto Assembler Window"))
            m_workspace.OpenScript(m_previewText);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (IconButton(ICON_FA_CHEVRON_RIGHT, "Hide the Preview"))
            m_showPreview = false;
        TextFitted(live ? "For " + m_engine.Symbols().ModuleOffset(m_workspace.LastCodeAddress) + ", the instruction selected last"
                        : "With sample values; select an instruction in a process for real ones",
            ImGui::GetContentRegionAvail().x, ManifoldTheme::Muted);
        PreviewInputs();
        if (m_previewFailed && !m_previewText.empty())
            TextWithError("Shows the last script that generated; the template has errors now.", ManifoldTheme::Warning);

        const ImVec2 start = ImGui::GetCursorPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float gap = Px(6.0f);
        float previewHeight = available.y;
        if (!m_problems.empty())
        {
            const float minProblems = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().WindowPadding.y * 2.0f;
            const float minPreview = Px(100.0f);
            if (m_problemsHeight <= 0.0f)
                m_problemsHeight = Px(120.0f);
            m_problemsHeight = std::clamp(m_problemsHeight, minProblems, std::max(minProblems, available.y - minPreview - gap));
            previewHeight = available.y - m_problemsHeight - gap;
        }
        if (m_previewText.empty())
        {
            ImGui::BeginChild("##nopreview", ImVec2(0.0f, previewHeight), ImGuiChildFlags_None);
            EmptyState(ICON_FA_EYE, m_previewFailed ? "Nothing Generated" : "Generating",
                m_previewFailed ? "The template does not generate a script yet; the problems below say why." : "The script shows here once the template has run.", {}, false);
            ImGui::EndChild();
        }
        else
            CodeEditor("##templatepreview", m_previewText, Language::AutoAssembler, ImVec2(0.0f, previewHeight), m_previewEditor);
        if (m_problems.empty())
            return;
        float first = previewHeight;
        float second = m_problemsHeight;
        ImGui::SetCursorPos(start);
        if (Splitter("##problemsplit", false, gap, &first, &second, Px(100.0f), Px(40.0f)))
            m_problemsHeight = second;
        ImGui::SetCursorPos(ImVec2(start.x, start.y + previewHeight + gap));
        ProblemList(m_problemsHeight);
    }

    void TemplateManager::EditorHeader()
    {
        Draft& draft = *m_draft;
        const float side = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(Fonts::Bold());
        ImGui::TextUnformatted(draft.Form.Caption.empty() ? "Untitled" : draft.Form.Caption.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (draft.BuiltIn)
            Muted("built-in, read only");
        else if (draft.IsNew)
            ImGui::TextColored(ManifoldTheme::Warning, "new, not saved yet");
        else if (Dirty())
            ImGui::TextColored(ManifoldTheme::Warning, "\xE2\x97\x8F changed");
        ImGui::SameLine();
        if (draft.BuiltIn)
        {
            if (AccentButton(ICON_FA_EDIT "  Edit a Copy"))
            {
                EditCopy();
                return;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Saves a copy in your template folder. It replaces the built-in template until you delete it.");
        }
        else
        {
            ImGui::BeginDisabled(!Dirty());
            if (AccentButton(ICON_FA_SAVE "  Save"))
                Save();
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_UNDO "  Revert"))
            {
                Revert();
                ImGui::EndDisabled();
                return;
            }
            ImGui::EndDisabled();
        }

        // Pane switches at the right edge; the lit ones are shown.
        const auto toggle = [&](const char* icon, const char* show, const char* hide, bool& value)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, value ? ManifoldTheme::Accent : ManifoldTheme::Muted);
                if (IconButton(icon, value ? hide : show))
                    value = !value;
                ImGui::PopStyleColor();
            };
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - side * 3.0f - spacing * 2.0f));
        toggle(ICON_FA_LIST, "Show the Library", "Hide the Library", m_showLibrary);
        ImGui::SameLine();
        toggle(ICON_FA_TAGS, "Show the Values", "Hide the Values", m_showValues);
        ImGui::SameLine();
        toggle(ICON_FA_EYE, "Show the Preview", "Hide the Preview", m_showPreview);
    }

    void TemplateManager::EditorPane()
    {
        if (!m_draft)
        {
            if (EmptyState(ICON_FA_SHAPES, "Pick a Template",
                    "Templates turn the instruction you selected into a ready Auto Assembler script. Pick one in the library to edit it, "
                    "or start a new one; built-in templates open read only and can be edited as a copy.",
                    { { ICON_FA_PLUS "  New Template", nullptr, true } }, false) == 0)
                RequestSelect({}, true);
            return;
        }
        EditorHeader();
        if (!m_draft)
            return;
        if (ImGui::BeginTabBar("##tabs"))
        {
            const auto tab = [&](const char* label, int index)
                {
                    ImGuiTabItemFlags flags = 0;
                    if (m_requestTab == index)
                        flags |= ImGuiTabItemFlags_SetSelected;
                    if (!ImGui::BeginTabItem(label, nullptr, flags))
                        return false;
                    if (m_requestTab == index)
                        m_requestTab = -1;
                    m_tab = index;
                    return true;
                };
            if (tab(ICON_FA_CODE "  Script", ScriptTabIndex))
            {
                ScriptTab();
                ImGui::EndTabItem();
            }
            if (tab(ICON_FA_SLIDERS_H "  Settings", SettingsTabIndex))
            {
                SettingsTab();
                ImGui::EndTabItem();
            }
            if (tab(ICON_FA_FILE_CODE "  Settings File", SettingsFileTabIndex))
            {
                RawTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void TemplateManager::Popups()
    {
        if (m_unsavedDialog.Begin())
        {
            ImGui::Text("Save the changes to \"%s\"?", m_draft ? m_draft->Form.Caption.c_str() : "");
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
            if (choice == 1 && !Save())
                choice = 3;
            if (choice == 1 || choice == 2)
            {
                if (choice == 2)
                    m_draft.reset();
                if (m_closing)
                {
                    m_open = false;
                    m_closing = false;
                }
                else
                    RequestSelect(m_pendingId, m_pendingNew, m_pendingScript);
            }
            else if (choice == 3)
            {
                m_closing = false;
                m_pendingId.clear();
                m_pendingNew = false;
                m_pendingScript.clear();
            }
        }

        if (m_deleteDialog.Begin())
        {
            ImGui::Text("Move \"%s\" to the recycle bin?", m_draft ? m_draft->Form.Caption.c_str() : "");
            if (m_deleteDialog.Buttons("Delete") == DialogChoice::Ok)
            {
                if (m_draft)
                {
                    if (const Templates::TemplateInfo* info = Library().Find(m_draft->Id))
                    {
                        const Status status = Templates::DeleteTemplate(*info, true);
                        if (!status)
                            Message(status.Text);
                    }
                    m_draft.reset();
                    Reload();
                }
                m_deleteDialog.Close();
            }
            m_deleteDialog.End();
        }

        if (m_reportDialog.Begin())
        {
            int failing = 0;
            for (const Templates::CheckEntry& entry : m_report)
                failing += entry.Errors.empty() ? 0 : 1;
            ImGui::Text("%d templates, %d with errors", static_cast<int>(m_report.size()), failing);
            const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + Px(4.0f);
            ImGui::BeginChild("##report", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
            std::string open;
            for (const Templates::CheckEntry& entry : m_report)
            {
                if (entry.Errors.empty() && entry.Warnings.empty())
                    continue;
                ImGui::PushID(entry.Id.c_str());
                ImGui::PushFont(Fonts::Bold());
                if (ImGui::Selectable(entry.Caption.c_str()))
                    open = entry.Id;
                ImGui::PopFont();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Open this template");
                for (const std::string& text : entry.Errors)
                    TextWithError(text, ManifoldTheme::Error);
                for (const std::string& text : entry.Warnings)
                    TextWithError(text, ManifoldTheme::Warning);
                ImGui::Spacing();
                ImGui::PopID();
            }
            if (std::all_of(m_report.begin(), m_report.end(), [](const Templates::CheckEntry& entry) { return entry.Errors.empty() && entry.Warnings.empty(); }))
                Muted("No problems found.");
            ImGui::EndChild();
            const bool close = m_reportDialog.Buttons("Close", true, true, nullptr) == DialogChoice::Ok;
            if (close || !open.empty())
                m_reportDialog.Close();
            m_reportDialog.End();
            if (!open.empty())
                RequestSelect(open, false);
        }

        if (m_messageDialog.Begin())
        {
            ImGui::PushTextWrapPos(Px(560.0f));
            ImGui::TextUnformatted(m_message.c_str());
            ImGui::PopTextWrapPos();
            if (m_messageDialog.Buttons("OK", true, true, nullptr) == DialogChoice::Ok)
                m_messageDialog.Close();
            m_messageDialog.End();
        }
    }

    void TemplateManager::Draw()
    {
        if (!m_open)
            return;
        if (!BeginWindow(ICON_FA_SHAPES "  Templates###templatemanager", ImVec2(1320.0f, 780.0f), ImGuiWindowFlags_MenuBar, ImVec2(720.0f, 420.0f)))
            return;
        if (!m_open && Dirty())
        {
            m_open = true;
            m_closing = true;
            m_unsavedDialog.Open();
        }
        Menu();
        Shortcuts();
        RefreshPreview();

        // ∑ Three panes side by side, each split by a draggable gap: the
        // library and the preview keep their widths, the editor takes the rest
        // and never drops below its own minimum. The window's minimum width
        // fits the editor with one side pane.
        const float gap = Px(8.0f);
        const float minLibrary = Px(190.0f);
        const float minEditor = Px(360.0f);
        const float minPreview = Px(280.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const bool library = m_showLibrary;
        const bool preview = m_showPreview && m_draft;
        if (m_libraryWidth <= 0.0f)
            m_libraryWidth = Px(270.0f);
        if (m_previewWidth <= 0.0f)
            m_previewWidth = std::max(minPreview, available.x * 0.36f);
        const float room = available.x - minEditor - (library ? gap : 0.0f) - (preview ? gap : 0.0f);
        if (library)
            m_libraryWidth = std::clamp(m_libraryWidth, minLibrary, std::max(minLibrary, room - (preview ? minPreview : 0.0f)));
        if (preview)
            m_previewWidth = std::clamp(m_previewWidth, minPreview, std::max(minPreview, room - (library ? m_libraryWidth : 0.0f)));
        const float editorWidth = std::max(Px(120.0f), available.x - (library ? m_libraryWidth + gap : 0.0f) - (preview ? m_previewWidth + gap : 0.0f));

        float x = start.x;
        if (library)
        {
            ImGui::SetCursorPos(ImVec2(x, start.y));
            ImGui::BeginChild("##library", ImVec2(m_libraryWidth, available.y), ImGuiChildFlags_Borders);
            LibraryPane();
            ImGui::EndChild();
            float first = m_libraryWidth;
            float second = editorWidth;
            ImGui::SetCursorPos(ImVec2(x, start.y));
            Splitter("##librarysplit", true, gap, &first, &second, minLibrary, minEditor, available.y);
            x += m_libraryWidth + gap;
            m_libraryWidth = first;
        }
        ImGui::SetCursorPos(ImVec2(x, start.y));
        ImGui::BeginChild("##editor", ImVec2(editorWidth, available.y), ImGuiChildFlags_None);
        EditorPane();
        ImGui::EndChild();
        if (preview && m_draft)
        {
            float first = editorWidth;
            float second = m_previewWidth;
            ImGui::SetCursorPos(ImVec2(x, start.y));
            Splitter("##previewsplit", true, gap, &first, &second, minEditor, minPreview, available.y);
            x += editorWidth + gap;
            ImGui::SetCursorPos(ImVec2(x, start.y));
            ImGui::BeginChild("##preview", ImVec2(m_previewWidth, available.y), ImGuiChildFlags_Borders);
            PreviewPane();
            ImGui::EndChild();
            m_previewWidth = second;
        }
        Popups();
        EndToolWindow();
    }
}
