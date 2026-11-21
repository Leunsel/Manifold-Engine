#include "UI/ModulesWindow.h"

#include "App/Engine.h"
#include "Core/Paths.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include <algorithm>
#include <system_error>

#include <windows.h>

#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        using Autorun::Module;
        using Autorun::State;

        ImVec4 StateColor(const Module& module)
        {
            switch (module.State)
            {
            case State::Loaded: return module.Changed || module.HeldNext ? ManifoldTheme::Warning : ManifoldTheme::Success;
            case State::Failed:
            case State::Blocked: return ManifoldTheme::Error;
            case State::Held: return ManifoldTheme::Warning;
            case State::Disabled: return module.ActiveUntilRestart ? ManifoldTheme::Warning : ManifoldTheme::Muted;
            default: return ManifoldTheme::Muted;
            }
        }

        const char* OriginText(const Module& module)
        {
            if (module.Shape == Autorun::Shape::Loose)
                return module.Origin == Autorun::Origin::Shipped ? "shipped, loose" : "loose";
            if (module.Origin == Autorun::Origin::Shipped)
                return module.Unmodified ? "shipped" : "shipped, edited";
            return "user";
        }

        bool Switchable(const Module& module)
        {
            return module.State != State::Superseded && module.State != State::BuiltIn && !module.Duplicate;
        }

        void OpenInExplorer(const std::filesystem::path& folder)
        {
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        void Field(const char* label, const std::string& value)
        {
            if (value.empty())
                return;
            ImGui::TextColored(ManifoldTheme::Muted, "%s", label);
            ImGui::SameLine(Px(110.0f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(value.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    void ModulesWindow::Ask(Action action, const std::string& name)
    {
        m_action = action;
        m_actionName = name;
    }

    void ModulesWindow::Perform()
    {
        const Action action = m_action;
        m_action = Action::None;
        if (action == Action::None)
            return;
        Engine& engine = m_workspace.Core();
        Lua::LuaEngine& lua = engine.Lua();
        Status status;
        switch (action)
        {
        case Action::Enable: status = lua.SetModuleEnabled(m_actionName, true); break;
        case Action::Disable: status = lua.SetModuleEnabled(m_actionName, false); break;
        case Action::Reload: status = lua.ReloadModule(m_actionName); break;
        case Action::Allow: status = lua.AllowModule(m_actionName); break;
        default: status = engine.RunAutorun(); break;
        }
        m_workspace.Report(status);
    }

    void ModulesWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_PUZZLE_PIECE "  Modules###modules", ImVec2(860.0f, 540.0f), ImGuiWindowFlags_MenuBar, ImVec2(560.0f, 320.0f)))
            return;
        Engine& engine = m_workspace.Core();
        const Lua::LuaEngine& lua = engine.Lua();
        const std::vector<Module>& modules = lua.Modules();
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Modules"))
            {
                if (ImGui::MenuItem(ICON_FA_SYNC "  Reload all modules"))
                    Ask(Action::ReloadAll, {});
                ItemTooltip("Unloads every module, reads the autorun folder again and runs what is switched on.");
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open the autorun folder"))
                    OpenInExplorer(Paths::AutorunFolder());
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open the modules folder"))
                    OpenInExplorer(Paths::AutorunFolder() / L"modules");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }
        if (modules.empty())
        {
            const int chosen = EmptyState(ICON_FA_PUZZLE_PIECE, "No autorun modules",
                engine.Options().AutorunEnabled ? "Lua scripts in the autorun folder run at start. A file in its root runs as it is; modules with their own folder go into autorun\\modules."
                                                : "Running the autorun scripts at start is switched off in Settings, under Scripts. Reload all modules runs them now.",
                { { ICON_FA_SYNC "  Reload all modules" }, { ICON_FA_FOLDER_OPEN "  Open the autorun folder" } }, false);
            if (chosen == 0)
                Ask(Action::ReloadAll, {});
            else if (chosen == 1)
                OpenInExplorer(Paths::AutorunFolder());
            EndToolWindow();
            Perform();
            return;
        }

        int loaded = 0;
        int attention = 0;
        for (const Module& module : modules)
        {
            loaded += module.State == State::Loaded ? 1 : 0;
            attention += module.State == State::Held || module.State == State::Failed || module.State == State::Blocked || module.HeldNext ? 1 : 0;
        }
        std::string summary = Format("%d of %zu loaded", loaded, modules.size());
        if (attention > 0)
            summary += Format(", %d need%s a look", attention, attention == 1 ? "s" : "");
        Muted(summary);

        const Module* selected = Autorun::Find(modules, m_selected);
        if (!selected)
        {
            selected = &modules.front();
            m_selected = selected->Key;
        }
        const float gap = Px(6.0f);
        const float minTable = Px(90.0f);
        const float minDetails = Px(110.0f);
        const float available = ImGui::GetContentRegionAvail().y;
        if (m_detailHeight <= 0.0f)
            m_detailHeight = std::clamp(available * 0.42f, Px(150.0f), Px(300.0f));
        m_detailHeight = std::clamp(m_detailHeight, minDetails, std::max(minDetails, available - minTable - gap));
        float top = available - m_detailHeight - gap;
        float bottom = m_detailHeight;
        const ImVec2 start = ImGui::GetCursorPos();
        if (BeginDataTable("##modules", { { "", 1.0f, 44.0f }, { "Name", 2.2f }, { "Category", 1.0f }, { "Version", 1.0f, 70.0f }, { "Origin", 1.0f, 104.0f }, { "State", 1.6f }, { "Load", 1.0f, 64.0f } },
                ImVec2(0.0f, top)))
        {
            for (const Module& module : modules)
                Row(module);
            ImGui::EndTable();
        }
        ImGui::SetCursorPos(ImVec2(start.x, start.y + top));
        if (Splitter("##modulesplit", false, gap, &top, &bottom, minTable, minDetails))
            m_detailHeight = bottom;
        ImGui::SetCursorPos(ImVec2(start.x, start.y + top + gap));
        ImGui::BeginChild("##moduledetail", ImVec2(0.0f, m_detailHeight), ImGuiChildFlags_Borders);
        selected = Autorun::Find(modules, m_selected);
        if (selected)
            Details(*selected);
        ImGui::EndChild();
        EndToolWindow();
        Perform();
    }

    void ModulesWindow::Row(const Module& module)
    {
        ImGui::PushID(&module);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool enabled = module.Enabled && Switchable(module);
        ImGui::BeginDisabled(!Switchable(module));
        if (ToggleSwitch("##on", &enabled))
            Ask(enabled ? Action::Enable : Action::Disable, module.Key);
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        const bool isSelected = m_selected == module.Key && module.State != State::Superseded;
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Selectable(module.Name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetFrameHeight())))
            m_selected = module.Key;
        ImGui::PopStyleVar();
        if (ImGui::BeginPopupContextItem("##modulemenu"))
        {
            m_selected = module.Key;
            if (ImGui::MenuItem(ICON_FA_SYNC "  Reload", nullptr, false, Switchable(module) && module.State != State::Disabled && module.State != State::Held))
                Ask(Action::Reload, module.Key);
            if (ImGui::MenuItem(ICON_FA_CHECK "  Allow", nullptr, false, module.State == State::Held || module.HeldNext))
                Ask(Action::Allow, module.Key);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open its folder"))
                OpenInExplorer(module.Shape == Autorun::Shape::Folder ? module.Root : module.Entry.parent_path());
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the path"))
                ImGui::SetClipboardText(WideToUtf8(module.Entry.wstring()).c_str());
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ManifoldTheme::Muted, "%s", module.Category.c_str());
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(module.Header.Version.c_str());
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ManifoldTheme::Muted, "%s", OriginText(module));
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(StateColor(module), "%s", module.HeldNext && module.State == State::Loaded ? "Loaded, held next time" : Autorun::StateText(module));
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (module.State == State::Loaded)
            ImGui::TextColored(ManifoldTheme::Muted, "%.0f ms", module.LoadMs);
        ImGui::PopID();
    }

    void ModulesWindow::Details(const Module& module)
    {
        Engine& engine = m_workspace.Core();
        ImGui::TextUnformatted((module.Header.Version.empty() ? module.Name : module.Name + "  " + module.Header.Version).c_str());
        ImGui::SameLine();
        ImGui::TextColored(StateColor(module), "%s", module.HeldNext && module.State == State::Loaded ? "Loaded, held next time" : Autorun::StateText(module));
        if (!module.Header.Description.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ManifoldTheme::Muted, "%s", module.Header.Description.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        const bool held = module.State == State::Held || module.HeldNext;
        if (held)
        {
            if (AccentButton(ICON_FA_CHECK "  Allow"))
                Ask(Action::Allow, module.Key);
            ItemTooltip("Runs the module and remembers its code as it is now. Look at the script first when you did not put it there.");
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!Switchable(module) || module.State == State::Disabled || module.State == State::Held);
        if (ImGui::Button(ICON_FA_SYNC "  Reload"))
            Ask(Action::Reload, module.Key);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Folder"))
            OpenInExplorer(module.Shape == Autorun::Shape::Folder ? module.Root : module.Entry.parent_path());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DATABASE "  Data folder"))
            OpenInExplorer(engine.Lua().ModuleDataFolder(module));
        ItemTooltip("Where the module keeps its settings and other files: autorun\\data, under its name.");
        ImGui::Spacing();

        if (!module.Reason.empty())
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(StateColor(module), "%s", module.Reason.c_str());
            ImGui::PopTextWrapPos();
        }
        if (module.HeldNext && module.State == State::Loaded)
            ImGui::TextColored(ManifoldTheme::Warning, "A script wrote to its files in this session. It is held at its next load until it is allowed.");
        else if (module.Changed)
            ImGui::TextColored(ManifoldTheme::Warning, "Its code changed since it last ran.");
        if (module.ActiveUntilRestart)
            ImGui::TextColored(ManifoldTheme::Warning, "What it added to menus, forms, timers or custom types stays until the program restarts.");
        Field("Path", module.Where);
        Field("Author", module.Header.Author);
        std::string needs;
        for (const Autorun::Requirement& requirement : module.Header.Requires)
            needs += (needs.empty() ? "" : ", ") + requirement.Name + (requirement.MinVersion.empty() ? "" : " >= " + requirement.MinVersion);
        Field("Requires", needs);
        if (module.Header.Api > 0)
            Field("manifold API", std::to_string(module.Header.Api));
        Field("First seen", module.FirstSeen);
        for (const auto& [key, value] : module.Header.Other)
            Field(key.c_str(), value);
        if (!module.Traceback.empty())
        {
            ImGui::Spacing();
            PushMono();
            ImGui::TextColored(ManifoldTheme::Muted, "%s", module.Traceback.c_str());
            PopMono();
        }
    }
}
