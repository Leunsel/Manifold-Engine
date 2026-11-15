#include "UI/TemplateManager.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "Target/Process.h"
#include "Templates/TemplateGenerator.h"
#include "UI/Fonts.h"
#include "UI/Templates.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <limits>
#include <memory>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kDragType = "MANIFOLD_TEMPLATE";

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
    }

    TemplateManager::TemplateManager(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core())
    {
    }

    Templates::Library& TemplateManager::Library()
    {
        return TemplateLibrary(m_engine);
    }

    std::filesystem::path TemplateManager::Root() const
    {
        return TemplateFolder();
    }

    void TemplateManager::Message(std::string text)
    {
        m_message = std::move(text);
        m_openMessage = true;
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
        m_scriptEditor = CodeEditorState{};
        m_rawEditor = CodeEditorState{};
        m_scriptEditor.ReadOnly = info.BuiltIn;
        m_rawEditor.ReadOnly = info.BuiltIn;
        m_previewInputs.clear();
        for (const Templates::InputDefinition& input : info.Inputs)
            m_previewInputs[input.Name] = Templates::InputText(input.Default);
        m_previewSignature.clear();
        m_seenSignature.clear();
    }

    void TemplateManager::RequestSelect(const std::string& id, bool fresh, const std::string& script)
    {
        m_pendingId = id;
        m_pendingNew = fresh;
        m_pendingScript = script;
        if (Dirty())
        {
            m_askUnsaved = true;
            return;
        }
        if (m_pendingNew)
        {
            Draft draft;
            draft.IsNew = true;
            draft.Form.SchemaVersion = 2;
            draft.Form.Caption = "New template";
            draft.Form.Id = Templates::UniqueId(Library(), draft.Form.Caption);
            draft.Form.Category = m_draft && !m_draft->BuiltIn ? m_draft->Form.Category : "Templates";
            draft.Script = m_pendingScript.empty() ? "[ENABLE]\n\n[DISABLE]\n" : m_pendingScript;
            draft.SettingsText = Templates::WriteSettings(draft.Form);
            draft.OriginalCategory = draft.Form.Category;
            m_draft = std::move(draft);
            m_scriptEditor = CodeEditorState{};
            m_rawEditor = CodeEditorState{};
            m_previewInputs.clear();
            m_previewSignature.clear();
            m_seenSignature.clear();
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
        m_draft.reset();
        if (const Templates::TemplateInfo* saved = Library().Find(id))
            Load(*saved);
        return true;
    }

    void TemplateManager::Toolbar()
    {
        if (IconButton(ICON_FA_PLUS, "New template"))
            RequestSelect({}, true);
        ImGui::SameLine();
        const Templates::TemplateInfo* current = m_draft && !m_draft->IsNew ? Library().Find(m_draft->Id) : nullptr;
        ImGui::BeginDisabled(!current);
        if (IconButton(ICON_FA_COPY, "Duplicate as a new template") && current)
        {
            std::string id;
            const Status status = Templates::DuplicateTemplate(Library(), *current, Root(), false, &id);
            if (!status)
                Message(status.Text);
            else
            {
                Reload();
                RequestSelect(id, false);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(current && current->BuiltIn);
        if (IconButton(ICON_FA_TRASH, "Move to the recycle bin", true))
            m_askDelete = true;
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (IconButton(ICON_FA_CHECK_DOUBLE, "Check all templates"))
        {
            m_report = Templates::CheckAll(m_engine.Lua().State(), Library());
            m_openReport = true;
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_FOLDER_OPEN, "Open the template folder"))
        {
            std::error_code ignored;
            std::filesystem::create_directories(Root(), ignored);
            ::ShellExecuteW(nullptr, L"open", Root().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_FILE_IMPORT, "Import templates from a folder"))
        {
            if (const auto folder = m_workspace.FolderDialog())
            {
                int imported = 0;
                const Status status = Templates::ImportTemplates(*folder, Root(), &imported);
                Reload();
                Message(status ? std::to_string(imported) + (imported == 1 ? " template imported." : " templates imported.") + " Files that already exist were left alone." : status.Text);
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        InputTextWithHint("##filter", "Search", m_filter);
    }

    void TemplateManager::Tree()
    {
        Templates::Library& library = Library();
        Node root;
        const std::string filter = Lower(m_filter);
        for (const Templates::TemplateInfo& info : library.Templates())
        {
            if (!filter.empty() && Lower(info.Caption + " " + info.Id + " " + info.Category).find(filter) == std::string::npos)
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
                const bool open = ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s %s", ICON_FA_FOLDER, child->Label.c_str());
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
                const std::string label = info->Caption + (info->BuiltIn ? "  (built-in)" : "");
                if (ImGui::Selectable(label.c_str(), selected))
                    RequestSelect(info->Id, false);
                if (ImGui::IsItemHovered() && !info->Description.empty())
                    ImGui::SetTooltip("%s", info->Description.c_str());
                if (!info->BuiltIn && ImGui::BeginDragDropSource())
                {
                    ImGui::SetDragDropPayload(kDragType, info->Id.data(), info->Id.size());
                    ImGui::TextUnformatted(info->Caption.c_str());
                    Muted("Drop on a category to move it there");
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
        };

        ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        draw(draw, root);
        if (!library.Problems().empty())
        {
            const std::string label = "Skipped (" + std::to_string(library.Problems().size()) + ")";
            if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                for (const Templates::LibraryProblem& problem : library.Problems())
                {
                    ImGui::TextColored(ManifoldTheme::Error, "%s", std::filesystem::path(Utf8ToWide(problem.File)).filename().string().c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", problem.Message.c_str());
                }
                ImGui::TreePop();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, Px(24.0f)));
        Muted("Drop here: top level");
        drop("Templates");
        ImGui::EndChild();

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
        InputTextWithHint("##valuefilter", "Find a value", m_valueFilter);
        const std::string filter = Lower(m_valueFilter);
        auto entry = [&](const std::string& name, const std::string& tip, const std::string& insert)
        {
            if (!filter.empty() && Lower(name).find(filter) == std::string::npos)
                return;
            if (ImGui::Selectable(name.c_str()) && !m_scriptEditor.ReadOnly)
            {
                m_scriptEditor.Insert = insert;
                m_scriptEditor.Focus = true;
                m_scriptEditor.KeepCursor = true;
            }
            if (ImGui::IsItemHovered() && !tip.empty())
                ImGui::SetTooltip("%s", tip.c_str());
        };
        std::string provider;
        for (const Templates::VariableInfo& variable : Templates::Variables())
        {
            if (provider != variable.Provider && filter.empty())
            {
                provider = variable.Provider;
                ImGui::Spacing();
                Muted(provider);
            }
            entry(variable.Name, variable.Description, std::string("<< ") + variable.Name + " >>");
        }
        if (m_draft && !m_draft->Form.Inputs.empty())
        {
            if (filter.empty())
            {
                ImGui::Spacing();
                Muted("Inputs");
            }
            for (const Templates::InputDefinition& input : m_draft->Form.Inputs)
                entry(input.Name, input.Caption, "<< " + input.Name + " >>");
        }
        if (filter.empty())
        {
            ImGui::Spacing();
            Muted("Helpers");
        }
        for (const std::string& helper : Templates::HelperNames())
            entry(helper + "()", helper == "include" ? "Renders a template from Partials" : "Helper function", "<< " + helper + "() >>");
        ImGui::EndChild();
    }

    void TemplateManager::ScriptTab()
    {
        Draft& draft = *m_draft;
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float values = Px(190.0f);
        CodeEditor("##templatescript", draft.Script, Language::AutoAssembler, ImVec2(available.x - values - ImGui::GetStyle().ItemSpacing.x, available.y), m_scriptEditor);
        ImGui::SameLine();
        ValuesPanel(values, available.y);
    }

    void TemplateManager::SettingsTab()
    {
        Draft& draft = *m_draft;
        Templates::SettingsForm& form = draft.Form;
        ImGui::BeginChild("##settings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        ImGui::BeginDisabled(draft.BuiltIn);
        bool changed = false;
        if (draft.SettingsHadComments && !draft.RawEdited)
            TextWithError("The settings file has comments. Saving from this form writes it anew without them; edit the Settings file tab to keep them.", ManifoldTheme::Warning);
        if (!draft.SettingsError.empty())
            TextWithError(draft.SettingsError, ManifoldTheme::Error);
        if (ImGui::BeginTable("##form", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Px(170.0f));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            Label("Caption");
            changed |= InputText("##caption", form.Caption);
            Label("Id");
            changed |= InputText("##id", form.Id);
            Label("Description");
            changed |= InputText("##description", form.Description);
            Label("Category");
            changed |= InputText("##category", form.Category);
            Label("");
            changed |= ImGui::Checkbox("Keep the category in the settings file", &form.ExplicitCategory);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off: the template file lives in the folder of its category.\nOn: the category is written into the settings, wherever the file lives.");
            Label("Show at the top of the menu");
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
            Label("Category order");
            if (InputText("##categoryorder", categoryOrder))
            {
                form.CategoryOrder = ParseNumber(categoryOrder);
                changed = true;
            }
            Label("Author");
            changed |= InputText("##author", form.Author);
            Label("Version");
            changed |= InputText("##version", form.Version);
            Label("Tags");
            changed |= ListField("##tags", form.Tags);
            Label("Requires");
            changed |= ListField("##requires", form.Requires);
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
            Label("Program Lua functions");
            changed |= ImGui::Checkbox("##globals", &form.AllowUnsafeGlobals);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Lets the template code call the program's Lua functions.");
            Label("Ask for the address");
            changed |= TriState("##askaddress", form.Memory.AskForInjectionAddress);
            Label("Ask for the hook name");
            changed |= TriState("##askhook", form.Memory.AskForHookName);
            Label("Default hook name");
            changed |= OptionalText("defaulthook", form.Memory.DefaultHookName);
            Label("Hook name suffix");
            changed |= OptionalText("suffix", form.Memory.AppendToHookName);
            Label("Allocation size");
            changed |= OptionalText("allocation", form.Memory.AllocationSize);
            Label("Allocate near the hook");
            changed |= TriState("##near", form.Memory.AllocationNear);
            ImGui::EndTable();
        }

        ImGui::SeparatorText("Inputs");
        int removed = -1;
        if (!form.Inputs.empty() && ImGui::BeginTable("##inputs", 7, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH))
        {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, Px(90.0f));
            ImGui::TableSetupColumn("Caption");
            ImGui::TableSetupColumn("Default");
            ImGui::TableSetupColumn("Values or range");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(1.0f));
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(28.0f));
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
                ImGui::TableNextColumn();
                if (IconButton(ICON_FA_TIMES, "Remove this input", true))
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
        if (ImGui::Button(ICON_FA_PLUS "  Add input"))
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
            ImGui::SeparatorText("Other fields");
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

    void TemplateManager::Preview()
    {
        Draft& draft = *m_draft;
        const bool live = m_engine.HasProcess() && m_workspace.LastCodeAddress != 0;
        ImGui::TextUnformatted("Preview");
        ImGui::SameLine();
        if (live)
            Muted("for " + m_engine.Symbols().ModuleOffset(m_workspace.LastCodeAddress) + ", the instruction selected last");
        else
            Muted("with sample values; open a process and select an instruction for real ones");
        ImGui::Checkbox("14-byte jumps", &m_preview14);
        for (const Templates::InputDefinition& input : draft.Form.Inputs)
        {
            if (!m_previewInputs.count(input.Name))
                m_previewInputs[input.Name] = Templates::InputText(input.Default);
            std::string& text = m_previewInputs[input.Name];
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(110.0f));
            InputTextWithHint(("##preview" + input.Name).c_str(), input.Caption.c_str(), text);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s (%s)", input.Caption.c_str(), input.Type.c_str());
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
        if (m_seenSignature != m_previewSignature && (m_previewSignature.empty() || now - m_changedAt >= 0.3))
        {
            m_previewSignature = m_seenSignature;
            m_previewText.clear();
            m_previewError.clear();
            m_previewWarnings.clear();
            m_scriptEditor.ErrorLine = 0;
            lua_State* state = m_engine.Lua().State();
            std::string error;
            auto parsed = Templates::ReadSettings(state, draft.SettingsText, "=settings", draft.Form.Caption, error);
            if (!parsed)
                m_previewError = "The settings are not valid: " + error;
            else
            {
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
                    std::string ignored;
                    if (const auto value = Templates::ParseInput(input, found->second, ignored))
                        request.Inputs[input.Name] = *value;
                }
                const Templates::GenerationResult result = Templates::Generate(m_engine, Library(), *parsed, request);
                if (result.Text)
                    m_previewText = *result.Text;
                else
                {
                    m_previewError = Templates::Describe(result.Failure, parsed->Caption);
                    if (result.Failure.Source == parsed->SourceName && result.Failure.Line > 0)
                        m_scriptEditor.ErrorLine = result.Failure.Line;
                }
                m_previewWarnings = result.Warnings;
                const Templates::CheckEntry check = Templates::CheckTemplate(state, Library(), *parsed, &draft.Script);
                for (const std::string& text : check.Errors)
                    m_previewWarnings.push_back(text);
                for (const std::string& text : check.Warnings)
                    m_previewWarnings.push_back(text);
            }
        }

        for (const std::string& warning : m_previewWarnings)
            TextWithError(warning, ManifoldTheme::Warning);
        if (!m_previewError.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            TextWithError(m_previewError, ManifoldTheme::Error);
            ImGui::PopTextWrapPos();
            return;
        }
        ImGui::PushFont(Fonts::Mono());
        ImGui::InputTextMultiline("##previewtext", m_previewText.data(), m_previewText.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
        ImGui::PopFont();
    }

    void TemplateManager::Editor()
    {
        if (!m_draft)
        {
            Muted("Pick a template on the left, or start a new one with " ICON_FA_PLUS ".");
            return;
        }
        Draft& draft = *m_draft;
        ImGui::PushFont(Fonts::Bold());
        ImGui::TextUnformatted(draft.Form.Caption.empty() ? "Untitled" : draft.Form.Caption.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (draft.BuiltIn)
            Muted("built-in, read only");
        else if (draft.IsNew)
            Muted("new, not saved yet");
        else if (Dirty())
            Muted("changed");
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (draft.BuiltIn)
        {
            ImGui::SameLine();
            if (AccentButton(ICON_FA_EDIT "  Edit a copy"))
            {
                if (const Templates::TemplateInfo* info = Library().Find(draft.Id))
                {
                    const Status status = Templates::DuplicateTemplate(Library(), *info, Root(), true);
                    if (!status)
                        Message(status.Text);
                    else
                    {
                        const std::string id = draft.Id;
                        Reload();
                        m_draft.reset();
                        RequestSelect(id, false);
                        return;
                    }
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Saves a copy in your template folder. It replaces the built-in template until you delete it.");
        }
        else
        {
            ImGui::SameLine();
            ImGui::BeginDisabled(!Dirty());
            if (AccentButton(ICON_FA_SAVE "  Save") || (focused && Dirty() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)))
                Save();
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_UNDO "  Revert"))
            {
                if (draft.IsNew)
                    m_draft.reset();
                else if (const Templates::TemplateInfo* info = Library().Find(draft.Id))
                    Load(*info);
                return;
            }
            ImGui::EndDisabled();
        }
        if (!m_draft)
            return;

        const ImVec2 available = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##edit", ImVec2(available.x * 0.56f, 0.0f), ImGuiChildFlags_None);
        if (ImGui::BeginTabBar("##tabs"))
        {
            if (ImGui::BeginTabItem("Script"))
            {
                ScriptTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings"))
            {
                SettingsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings file"))
            {
                RawTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        Preview();
        ImGui::EndChild();
    }

    void TemplateManager::Popups()
    {
        if (m_askUnsaved)
        {
            ImGui::OpenPopup("Unsaved Changes###templateunsaved");
            m_askUnsaved = false;
        }
        if (ImGui::BeginPopupModal("Unsaved Changes###templateunsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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
            if (ImGui::Button("Cancel", ImVec2(Px(90.0f), 0.0f)))
                choice = 3;
            if (choice)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
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

        if (m_askDelete)
        {
            ImGui::OpenPopup("Delete Template###templatedelete");
            m_askDelete = false;
        }
        if (ImGui::BeginPopupModal("Delete Template###templatedelete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Move \"%s\" to the recycle bin?", m_draft ? m_draft->Form.Caption.c_str() : "");
            ImGui::Spacing();
            if (AccentButton("Delete", ImVec2(Px(90.0f), 0.0f)))
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
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(Px(90.0f), 0.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (m_openReport)
        {
            ImGui::OpenPopup("Template Check###templatereport");
            m_openReport = false;
        }
        ImGui::SetNextWindowSize(ImVec2(Px(720.0f), Px(460.0f)), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Template Check###templatereport", nullptr))
        {
            int failing = 0;
            for (const Templates::CheckEntry& entry : m_report)
                failing += entry.Errors.empty() ? 0 : 1;
            ImGui::Text("%d templates, %d with errors", static_cast<int>(m_report.size()), failing);
            const float footer = ImGui::GetFrameHeightWithSpacing() + Px(4.0f);
            ImGui::BeginChild("##report", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
            for (const Templates::CheckEntry& entry : m_report)
            {
                if (entry.Errors.empty() && entry.Warnings.empty())
                    continue;
                ImGui::PushFont(Fonts::Bold());
                ImGui::TextUnformatted(entry.Caption.c_str());
                ImGui::PopFont();
                for (const std::string& text : entry.Errors)
                    TextWithError(text, ManifoldTheme::Error);
                for (const std::string& text : entry.Warnings)
                    TextWithError(text, ManifoldTheme::Warning);
                ImGui::Spacing();
            }
            if (std::all_of(m_report.begin(), m_report.end(), [](const Templates::CheckEntry& entry) { return entry.Errors.empty() && entry.Warnings.empty(); }))
                Muted("No problems found.");
            ImGui::EndChild();
            if (ImGui::Button("Close", ImVec2(Px(90.0f), 0.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (m_openMessage)
        {
            ImGui::OpenPopup("Templates###templatemessage");
            m_openMessage = false;
        }
        if (ImGui::BeginPopupModal("Templates###templatemessage", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::PushTextWrapPos(Px(560.0f));
            ImGui::TextUnformatted(m_message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(Px(90.0f), 0.0f)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void TemplateManager::Draw()
    {
        if (!m_open)
            return;
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_SHAPES "  Templates###templatemanager", &m_open, ImVec2(1220.0f, 740.0f), 0, ImVec2(820.0f, 420.0f)))
        {
            EndToolWindow();
            return;
        }
        if (!m_open && Dirty())
        {
            m_open = true;
            m_closing = true;
            m_askUnsaved = true;
        }
        ImGui::BeginChild("##side", ImVec2(Px(280.0f), 0.0f), ImGuiChildFlags_Borders);
        Toolbar();
        Tree();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        Editor();
        ImGui::EndChild();
        Popups();
        EndToolWindow();
    }
}
