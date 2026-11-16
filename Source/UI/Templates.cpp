#include "UI/Templates.h"

#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/InputBox.h"
#include "Lua/LuaEngine.h"
#include "Target/Process.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include "imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace Manifold::UI
{
    namespace
    {
        bool g_use14ByteJump = false;
        bool g_previewFirst = false;

        struct CategoryNode
        {
            std::string Label;
            double Order = std::numeric_limits<double>::infinity();
            std::vector<std::unique_ptr<CategoryNode>> Children;
            std::vector<const Templates::TemplateInfo*> Items;

            CategoryNode& Child(const std::string& label)
            {
                for (const auto& child : Children)
                {
                    if (child->Label == label)
                        return *child;
                }
                Children.push_back(std::make_unique<CategoryNode>());
                Children.back()->Label = label;
                return *Children.back();
            }
        };

        std::filesystem::path Folder()
        {
            return Paths::ProgramFolder() / L"templates";
        }

        Templates::Library& SharedLibrary()
        {
            static Templates::Library library(Folder());
            return library;
        }
    }

    std::filesystem::path TemplateFolder()
    {
        return Folder();
    }

    TemplateRunner::TemplateRunner(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()),
        m_inputs("templateinputs", ICON_FA_MAGIC, "Template inputs"),
        m_previewDialog("templatepreview", ICON_FA_EYE, "Template preview", ImVec2(760.0f, 540.0f)),
        m_failureDialog("templatefailure", ICON_FA_EXCLAMATION_TRIANGLE, "Template generation failed")
    {
    }

    namespace
    {

        bool Supported(Engine& engine, const Templates::TemplateInfo& info, std::string& reason)
        {
            if (info.Architectures.empty() || !engine.HasProcess())
                return true;
            const std::string target = engine.Process()->Is64Bit() ? "x64" : "x86";
            if (std::find(info.Architectures.begin(), info.Architectures.end(), target) != info.Architectures.end())
                return true;
            reason = "Only for ";
            for (std::size_t index = 0; index < info.Architectures.size(); ++index)
                reason += (index ? "/" : "") + info.Architectures[index];
            reason += " processes";
            return false;
        }

        std::vector<std::string> Segments(const std::string& category)
        {
            std::vector<std::string> segments;
            std::size_t start = 0;
            while (start <= category.size())
            {
                const std::size_t end = category.find('>', start);
                const std::string segment = Trim(category.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (!segment.empty())
                    segments.push_back(segment);
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            if (segments.empty())
                segments.push_back("Templates");
            return segments;
        }
    }

    Templates::Library& TemplateLibrary(Engine& engine)
    {
        static double lastCheck = -10.0;
        Templates::Library& library = SharedLibrary();
        const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : lastCheck + 10.0;
        if (now - lastCheck >= 1.0)
        {
            lastCheck = now;
            if (library.ReloadIfChanged(engine.Lua().State()))
            {
                for (const Templates::LibraryProblem& problem : library.Problems())
                    LOG_WARN("Templates", "Skipped " + problem.File + ": " + problem.Message);
            }
        }
        return library;
    }

    void TemplateRunner::MenuItems(std::uint64_t address, const std::string* script)
    {
        Templates::Library& library = TemplateLibrary(m_engine);
        auto choose = [&](const Templates::TemplateInfo& info)
        {
            m_pending = info.Id;
            m_address = address;
        };
        auto item = [&](const Templates::TemplateInfo& info)
        {
            std::string reason;
            const bool supported = Supported(m_engine, info, reason);
            if (ImGui::MenuItem(info.Caption.c_str(), info.Shortcut.empty() ? nullptr : info.Shortcut.c_str(), false, supported))
                choose(info);
            const std::string tip = supported ? info.Description : reason;
            if (!tip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tip.c_str());
        };

        CategoryNode root;
        bool topLevel = false;
        for (const Templates::TemplateInfo& info : library.Templates())
        {
            if (!info.InSubMenu)
            {
                item(info);
                topLevel = true;
                continue;
            }
            CategoryNode* node = &root;
            for (const std::string& segment : Segments(info.Category))
            {
                std::optional<double> prefix;
                const std::string label = Templates::CategoryLabel(segment, &prefix);
                node = &node->Child(label.empty() ? segment : label);
                if (prefix)
                    node->Order = std::min(node->Order, *prefix);
                if (info.CategoryOrder)
                    node->Order = std::min(node->Order, *info.CategoryOrder);
            }
            node->Items.push_back(&info);
        }

        auto draw = [&](auto& self, const CategoryNode& node) -> void
        {
            std::vector<const CategoryNode*> children;
            for (const auto& child : node.Children)
                children.push_back(child.get());
            std::stable_sort(children.begin(), children.end(), [](const CategoryNode* a, const CategoryNode* b)
                {
                    if (a->Order != b->Order)
                        return a->Order < b->Order;
                    return Lower(a->Label) < Lower(b->Label);
                });
            for (const CategoryNode* child : children)
            {
                if (ImGui::BeginMenu(child->Label.c_str()))
                {
                    self(self, *child);
                    ImGui::EndMenu();
                }
            }
            for (const Templates::TemplateInfo* info : node.Items)
                item(*info);
        };
        if (topLevel && !root.Children.empty())
            ImGui::Separator();
        draw(draw, root);

        ImGui::Separator();
        ImGui::MenuItem("Use 14-byte jumps", nullptr, &g_use14ByteJump);
        ImGui::MenuItem("Preview before inserting", nullptr, &g_previewFirst);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_SHAPES "  Manage templates..."))
            m_workspace.OpenTemplateManager();
        if (script && ImGui::MenuItem(ICON_FA_FILE_EXPORT "  Save script as template...", nullptr, false, !Trim(*script).empty()))
            m_workspace.OpenTemplateManager({}, *script);
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open template folder"))
        {
            std::error_code ignored;
            std::filesystem::create_directories(Folder(), ignored);
            ::ShellExecuteW(nullptr, L"open", Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem(ICON_FA_SYNC "  Reload templates"))
            library.Load(m_engine.Lua().State());
        if (!library.Problems().empty())
        {
            const std::string label = std::to_string(library.Problems().size()) + (library.Problems().size() == 1 ? " template skipped" : " templates skipped");
            if (ImGui::BeginMenu(label.c_str()))
            {
                for (const Templates::LibraryProblem& problem : library.Problems())
                {
                    const std::string text = std::filesystem::path(Utf8ToWide(problem.File)).filename().string() + ": " + problem.Message;
                    ImGui::MenuItem(text.c_str(), nullptr, false, false);
                }
                ImGui::EndMenu();
            }
        }
    }

    bool TemplateRunner::Choose(const std::string& id, std::uint64_t address)
    {
        const Templates::TemplateInfo* info = TemplateLibrary(m_engine).Find(id);
        if (!info)
            return false;
        std::string reason;
        if (!Supported(m_engine, *info, reason))
            return false;
        m_pending = info->Id;
        m_address = address;
        return true;
    }

    void TemplateRunner::Start(const Templates::TemplateInfo& info)
    {
        if (info.Inputs.empty())
        {
            Run(info, {});
            return;
        }
        m_running = info.Id;
        m_inputTexts.clear();
        m_inputFlags.clear();
        m_inputChoices.clear();
        m_inputError.clear();
        for (const Templates::InputDefinition& input : info.Inputs)
        {
            const std::string text = Templates::InputText(input.Default);
            m_inputTexts.push_back(text);
            m_inputFlags.push_back(std::holds_alternative<bool>(input.Default) && std::get<bool>(input.Default));
            const auto found = std::find(input.Values.begin(), input.Values.end(), text);
            m_inputChoices.push_back(found == input.Values.end() ? 0 : static_cast<int>(found - input.Values.begin()));
        }
        m_inputs.Open();
    }

    void TemplateRunner::Run(const Templates::TemplateInfo& info, std::map<std::string, Templates::Value> inputs)
    {
        Templates::GenerationRequest request;
        request.Address = m_address;
        request.Use14ByteJump = g_use14ByteJump;
        request.Inputs = std::move(inputs);
        request.Ask.Ask = [](const std::string& title, const std::string& message, const std::string& value) -> std::optional<std::string>
        {
            std::string text = value;
            if (!Lua::AskText(title, message, text))
                return std::nullopt;
            return text;
        };
        const Templates::GenerationResult result = Templates::Generate(m_engine, SharedLibrary(), info, request);
        for (const std::string& warning : result.Warnings)
            LOG_WARN("Templates", info.Caption + ": " + warning);
        if (!result.Ok())
        {
            if (result.Failure.Cancelled)
                return;
            m_failure = Templates::Describe(result.Failure, info.Caption);
            LOG_ERROR("Templates", ErrorMessage(result.Failure.Code, result.Failure.Message));
            m_failureDialog.Open();
            return;
        }
        if (g_previewFirst)
        {
            m_preview = *result.Text;
            m_previewCaption = info.Caption;
            m_previewDialog.Open();
            return;
        }
        m_ready = *result.Text;
    }

    void TemplateRunner::InputsPopup()
    {
        if (!m_inputs.Begin())
            return;
        Templates::Library& library = SharedLibrary();
        const Templates::TemplateInfo* info = library.Find(m_running);
        if (!info || info->Inputs.size() != m_inputTexts.size())
        {
            m_inputs.Close();
            m_inputs.End();
            return;
        }
        ImGui::TextUnformatted(info->Caption.c_str());
        if (!info->Description.empty())
            Muted(info->Description);
        ImGui::Spacing();
        if (ImGui::BeginTable("##inputs", 2, ImGuiTableFlags_SizingFixedFit))
        {
            for (std::size_t index = 0; index < info->Inputs.size(); ++index)
            {
                const Templates::InputDefinition& input = info->Inputs[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(input.Caption.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(index));
                ImGui::SetNextItemWidth(Px(260.0f));
                if (input.Type == "boolean")
                {
                    bool value = m_inputFlags[index];
                    if (ImGui::Checkbox("##value", &value))
                        m_inputFlags[index] = value;
                }
                else if (input.Type == "enum")
                {
                    int& choice = m_inputChoices[index];
                    if (ImGui::BeginCombo("##value", input.Values[static_cast<std::size_t>(choice)].c_str()))
                    {
                        for (std::size_t option = 0; option < input.Values.size(); ++option)
                        {
                            if (ImGui::Selectable(input.Values[option].c_str(), choice == static_cast<int>(option)))
                                choice = static_cast<int>(option);
                        }
                        ImGui::EndCombo();
                    }
                }
                else
                    InputText("##value", m_inputTexts[index]);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!m_inputError.empty())
            TextWithError(m_inputError, ManifoldTheme::Error);
        bool run = false;
        std::map<std::string, Templates::Value> values;
        if (m_inputs.Buttons("Generate") == DialogChoice::Ok)
        {
            m_inputError.clear();
            for (std::size_t index = 0; index < info->Inputs.size() && m_inputError.empty(); ++index)
            {
                const Templates::InputDefinition& input = info->Inputs[index];
                const std::string text = input.Type == "boolean" ? (m_inputFlags[index] ? "true" : "false")
                    : input.Type == "enum" ? input.Values[static_cast<std::size_t>(m_inputChoices[index])] : m_inputTexts[index];
                std::string error;
                if (const auto value = Templates::ParseInput(input, text, error))
                    values[input.Name] = *value;
                else
                    m_inputError = error;
            }
            if (m_inputError.empty())
            {
                run = true;
                m_inputs.Close();
            }
        }
        m_inputs.End();
        if (run)
            Run(*info, std::move(values));
    }

    void TemplateRunner::PreviewPopup()
    {
        if (!m_previewDialog.Begin())
            return;
        ImGui::TextUnformatted(m_previewCaption.c_str());
        const float footer = ImGui::GetFrameHeightWithSpacing() + Px(6.0f);
        ImGui::InputTextMultiline("##preview", m_preview.data(), m_preview.size() + 1, ImVec2(-FLT_MIN, -footer), ImGuiInputTextFlags_ReadOnly);
        if (m_previewDialog.Buttons("Insert", true, false) == DialogChoice::Ok)
        {
            m_ready = m_preview;
            m_previewDialog.Close();
        }
        m_previewDialog.End();
    }

    void TemplateRunner::FailurePopup()
    {
        if (!m_failureDialog.Begin())
            return;
        ImGui::PushTextWrapPos(Px(620.0f));
        TextWithError(m_failure, ManifoldTheme::Error);
        ImGui::PopTextWrapPos();
        if (m_failureDialog.Buttons("Close", true, true, nullptr) == DialogChoice::Ok)
            m_failureDialog.Close();
        m_failureDialog.End();
    }

    void TemplateRunner::Draw()
    {
        if (!m_pending.empty())
        {
            const std::string id = std::exchange(m_pending, std::string());
            if (const Templates::TemplateInfo* info = SharedLibrary().Find(id))
                Start(*info);
        }
        InputsPopup();
        PreviewPopup();
        FailurePopup();
    }

    std::optional<std::string> TemplateRunner::TakeText()
    {
        return std::exchange(m_ready, std::nullopt);
    }
}
