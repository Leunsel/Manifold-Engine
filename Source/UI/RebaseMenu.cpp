#include "UI/RebaseMenu.h"

#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

namespace Manifold::UI
{
    std::string RebaseNote(Engine& engine, const Target::ModuleInfo& module)
    {
        const auto& process = engine.Process();
        if (!process)
            return {};
        const auto base = engine.Symbols().Rebase().ShownBase(*process, module);
        return base ? module.Name + " shown at " + Hex(*base) : std::string();
    }

    void RebasePrompt::MenuItems(Engine& engine, const Target::ModuleInfo& module)
    {
        if (!ImGui::BeginMenu(ICON_FA_MAP_PIN "  Rebase Module"))
            return;
        const Target::Rebases& rebases = engine.Symbols().Rebase();
        const auto rule = rebases.Rule(module.Name);
        const auto imageBase = engine.Process() ? rebases.ImageBase(*engine.Process(), module) : std::nullopt;
        Layout::Muted(module.Name + ", loaded at " + Hex(module.Base));
        ImGui::Separator();
        const std::string imageLabel = imageBase ? "Show at Its Image Base (" + Hex(*imageBase) + ")" : std::string("Show at Its Image Base");
        if (ImGui::MenuItem(imageLabel.c_str(), nullptr, rule && rule->ImageBase, imageBase.has_value()))
            engine.SetRebase(module.Name, Target::RebaseRule{ true, 0 });
        ItemTooltip(imageBase ? "The base its PE header asks for, as IDA and Ghidra show the file: addresses match theirs, whatever ASLR picked this time."
                              : "Its PE header cannot be read.");
        if (ImGui::MenuItem("Show as RVAs (Base 0)", nullptr, rule && !rule->ImageBase && rule->Base == 0))
            engine.SetRebase(module.Name, Target::RebaseRule{ false, 0 });
        ItemTooltip("Every address is its offset from the start of the module.");
        if (ImGui::MenuItem("Show at a Chosen Base...", nullptr, rule && !rule->ImageBase && rule->Base != 0))
        {
            m_module = module.Name;
            m_text = rule && !rule->ImageBase ? Hex(rule->Base) : imageBase ? Hex(*imageBase) : std::string();
            m_error.clear();
            m_open = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Show Where It Is Loaded", nullptr, !rule, rule.has_value()))
            engine.SetRebase(module.Name, std::nullopt);
        ItemTooltip("Remove the rebase: the module shows its real addresses again.");
        ImGui::EndMenu();
    }

    void RebasePrompt::Draw(Engine& engine)
    {
        if (m_open)
        {
            ImGui::OpenPopup("##rebasebase");
            m_open = false;
        }
        if (!ImGui::BeginPopup("##rebasebase"))
            return;
        Layout::Text("Show " + m_module + " as if it were loaded at");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        Layout::FieldOptions field;
        field.Placement = Layout::LabelPlacement::None;
        field.Width = Px(240.0f);
        Layout::Field("Base", field);
        const bool enter = InputText("##rebasebasetext", m_text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsHexadecimal);
        Layout::Muted("Hexadecimal. Only the shown addresses change, never the process.");
        if (!m_error.empty())
            TextWithError(m_error, ManifoldTheme::Error);
        const DialogChoice choice = DialogButtons("Rebase", !Trim(m_text).empty(), false);
        if (enter || choice == DialogChoice::Ok)
        {
            const auto rule = Target::Rebases::ParseRule(m_text);
            if (!rule || rule->ImageBase)
                m_error = "Not a hexadecimal number.";
            else
            {
                engine.SetRebase(m_module, rule);
                ImGui::CloseCurrentPopup();
            }
        }
        else if (choice == DialogChoice::Cancel)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
