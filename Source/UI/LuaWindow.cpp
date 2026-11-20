#include "UI/LuaWindow.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "App/Sounds.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "Scan/ValueType.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace Manifold::UI
{
    namespace
    {
        constexpr const char* kScriptChunk = "script";

        std::filesystem::path HistoryFile()
        {
            return Paths::ConfigFolder() / L"Manifold-Engine.lua-history.txt";
        }
    }

    CodeAssist MakeLuaAssist(Engine& engine)
    {
        const auto convert = [](std::vector<Lua::LuaCompletion> items)
            {
                std::vector<CodeSuggestion> result;
                result.reserve(items.size());
                for (Lua::LuaCompletion& item : items)
                    result.push_back({ std::move(item.Name), std::move(item.Kind), std::move(item.Signature), std::move(item.Text), std::move(item.Doc) });
                return result;
            };
        Engine* owner = &engine;
        CodeAssist assist;
        assist.Complete = [owner, convert](const std::string& chain, const std::string& prefix, bool method) { return convert(owner->Lua().Complete(chain, prefix, method)); };
        assist.Signatures = [owner](const std::string& chain, bool method) { return owner->Lua().CallSignatures(chain, method); };
        assist.Choices = [owner](const std::string& chain, bool method, int argument) { return owner->Lua().CallArgumentChoices(chain, method, argument); };
        assist.Describe = [owner](const std::string& chain, bool method) { return owner->Lua().Describe(chain, method); };
        assist.ResolveType = [owner](const std::string& type, const std::string& scope) { return owner->Lua().ResolveDocType(type, scope); };
        assist.CompleteClass = [owner, convert](const std::string& className, const std::string& prefix, bool method) { return convert(owner->Lua().CompleteClass(className, prefix, method)); };
        assist.ClassMember = [owner](const std::string& className, const std::string& member) { return owner->Lua().ClassMember(className, member); };
        return assist;
    }

    LuaWindow::LuaWindow(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core()), m_history(HistoryFile())
    {
        m_text = "-- Lua 5.4 with the Manifold API. Ctrl+Enter runs the script.\n"
                 "print(\"Process: \" .. getOpenedProcessID())\n";
        m_savedText = m_text;
        m_assist = MakeLuaAssist(m_engine);
        m_editor.Assist = &m_assist;
        m_inputEditor.Assist = &m_assist;
        m_inputEditor.Console = true;
        m_inputEditor.Placeholder = "Lua: Enter runs it, Shift+Enter adds a line, \xE2\x86\x91 brings back earlier input";
        m_inputEditor.Focus = true;
        m_history.Load();
        const Settings& options = m_engine.Options();
        m_filter.Wrap = options.LuaWrapOutput;
        m_filter.Time = options.LuaConsoleTime;
        m_filter.Hex = options.LuaConsoleHex;
    }

    bool LuaWindow::Running()
    {
        const Lua::LuaEngine& lua = m_engine.Lua();
        return (m_task && lua.TaskAlive(m_task)) || (m_inputTask && lua.TaskAlive(m_inputTask));
    }

    void LuaWindow::SaveViewSettings()
    {
        Settings& options = m_engine.Options();
        options.LuaWrapOutput = m_filter.Wrap;
        options.LuaConsoleTime = m_filter.Time;
        options.LuaConsoleHex = m_filter.Hex;
        m_engine.SaveOptionsSoon();
    }

    void LuaWindow::RememberShowOnPrint(bool show)
    {
        m_engine.Lua().RememberShowOnPrint(show);
        m_engine.SaveOptionsSoon();
    }

    void LuaWindow::SelectTab(Tab tab)
    {
        m_tab = tab;
        m_selectTab = true;
        if (tab == Tab::Console)
            m_inputEditor.Focus = true;
        else
            m_editor.Focus = true;
    }

    void LuaWindow::ExecuteScript()
    {
        m_editor.ErrorLine = 0;
        m_scriptView.Follow();
        m_view.Follow();
        Lua::LuaEngine& lua = m_engine.Lua();
        lua.RememberChunk(kScriptChunk, m_text);
        if (!m_engine.Async())
        {
            MarkError(lua.Run(m_text, kScriptChunk));
            return;
        }
        if (m_task && lua.TaskAlive(m_task))
            return;
        m_stopped = false;
        const std::weak_ptr<int> alive = m_alive;
        m_task = lua.StartTask(m_text, kScriptChunk, [this, alive](const Status& status, const std::string&)
            {
                if (alive.expired())
                    return;
                m_task = 0;
                MarkError(status);
            });
    }

    void LuaWindow::MarkError(const Status& status)
    {
        if (status)
            return;
        if (!std::exchange(m_stopped, false))
            Sounds::Play(Sounds::Cue::Error);
        const std::string prefix = std::string(kScriptChunk) + ":";
        const std::size_t at = status.Text.find(prefix);
        if (at != std::string::npos)
        {
            const auto line = ParseDecimal(status.Text.substr(at + prefix.size(), status.Text.find(':', at + prefix.size()) - at - prefix.size()));
            if (line)
                m_editor.ErrorLine = static_cast<int>(*line);
        }
    }

    void LuaWindow::Submit()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (TrimView(m_input).empty())
            return;
        if (Lua::LuaEngine::Incomplete(lua.State(), m_input))
        {
            m_inputEditor.Insert = "\n";
            m_inputEditor.Focus = true;
            return;
        }
        m_history.Add(m_input);
        m_history.Save();
        m_view.Follow();
        m_scriptView.Follow();
        m_stopped = false;
        int task = 0;
        const std::weak_ptr<int> alive = m_alive;
        lua.Evaluate(m_input, [this, alive](const Status& status)
            {
                if (alive.expired())
                    return;
                m_inputTask = 0;
                if (!status && !std::exchange(m_stopped, false))
                    Sounds::Play(Sounds::Cue::Error);
            }, &task);
        if (task)
            m_inputTask = task;
        m_input.clear();
        m_inputEditor.Focus = true;
    }

    void LuaWindow::Stop()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (m_task && lua.TaskAlive(m_task))
        {
            m_stopped = true;
            lua.StopTask(m_task);
        }
        if (m_inputTask && lua.TaskAlive(m_inputTask))
        {
            m_stopped = true;
            lua.StopTask(m_inputTask);
        }
    }

    ConsoleActions LuaWindow::Actions()
    {
        ConsoleActions actions;
        actions.OpenMemory = [this](std::uint64_t address) { m_workspace.OpenMemoryViewer(address); };
        actions.Dissect = [this](std::uint64_t address) { m_workspace.OpenStructure(address); };
        actions.AddAddress = [this](std::uint64_t address) { m_workspace.AddAddress(address, Scan::ValueType::FourBytes, "From the Lua console"); };
        actions.Recall = [this](const std::string& code)
        {
            m_input = code;
            SelectTab(Tab::Console);
        };
        actions.GotoSource = [this](const std::string& chunk, int line)
        {
            if (chunk != kScriptChunk)
                return;
            m_editor.GotoLine = line;
            m_editor.ErrorLine = line;
            SelectTab(Tab::Script);
        };
        actions.Clear = [this]() { m_engine.Lua().ClearOutput(); };
        return actions;
    }

    void LuaWindow::RunCommand(Command command)
    {
        switch (command)
        {
        case Command::LuaOpen:
            if (const auto path = m_workspace.OpenFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" }, { L"All files", L"*.*" } }, Paths::AutorunFolder()))
            {
                std::ifstream file(*path, std::ios::binary);
                std::stringstream content;
                content << file.rdbuf();
                m_text = content.str();
                m_savedText = m_text;
                m_path = *path;
                SelectTab(Tab::Script);
            }
            break;
        case Command::LuaSave:
        case Command::LuaSaveAs:
        {
            std::optional<std::filesystem::path> path;
            if (command == Command::LuaSave && !m_path.empty())
                path = m_path;
            else
                path = m_workspace.SaveFileDialog({ { L"Lua scripts (*.lua)", L"*.lua" } }, m_path.empty() ? "script.lua" : WideToUtf8(m_path.filename().wstring()), L"lua");
            if (!path)
                break;
            std::ofstream file(*path, std::ios::binary | std::ios::trunc);
            file.write(m_text.data(), static_cast<std::streamsize>(m_text.size()));
            if (file)
            {
                m_path = *path;
                m_savedText = m_text;
                m_engine.Notify(LogLevel::Success, "Saved " + WideToUtf8(path->filename().wstring()));
            }
            else
            {
                m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(path->wstring()));
            }
            break;
        }
        case Command::LuaExecute:
            if (m_tab == Tab::Console)
                Submit();
            else
                ExecuteScript();
            break;
        case Command::LuaClearOutput: m_engine.Lua().ClearOutput(); break;
        case Command::LuaFunctions:
            m_engine.Options().LuaDocsPanel = !m_engine.Options().LuaDocsPanel;
            if (m_engine.Options().LuaDocsPanel)
                m_docs.FocusSearch();
            m_engine.SaveOptionsSoon();
            break;
        case Command::LuaStop: Stop(); break;
        case Command::LuaConsoleTab: SelectTab(Tab::Console); break;
        case Command::LuaScriptTab: SelectTab(Tab::Script); break;
        case Command::LuaFind:
            SelectTab(Tab::Console);
            m_view.FocusSearch();
            break;
        default: break;
        }
    }

    void LuaWindow::DrawMenu()
    {
        if (!ImGui::BeginMenuBar())
            return;
        const auto item = [&](const char* label, Command command, bool selected = false, bool enabled = true)
        {
            if (CommandItem(label, command, selected, enabled))
                RunCommand(command);
        };
        if (ImGui::BeginMenu("File"))
        {
            item(ICON_FA_FOLDER_OPEN "  Open script...", Command::LuaOpen);
            item(ICON_FA_SAVE "  Save script", Command::LuaSave);
            item(ICON_FA_SAVE "  Save script as...", Command::LuaSaveAs);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            item(ICON_FA_TERMINAL "  Console", Command::LuaConsoleTab, m_tab == Tab::Console);
            item(ICON_FA_FILE_CODE "  Script", Command::LuaScriptTab, m_tab == Tab::Script);
            item(ICON_FA_BOOK "  Documentation", Command::LuaFunctions, m_engine.Options().LuaDocsPanel);
            ImGui::Separator();
            item(ICON_FA_SEARCH "  Search the console", Command::LuaFind);
            item(ICON_FA_ERASER "  Clear the console", Command::LuaClearOutput);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_EYE "  Show on print", nullptr, m_engine.Lua().ShowOnPrint()))
                RememberShowOnPrint(!m_engine.Lua().ShowOnPrint());
            ItemTooltip("Opens this window when a script prints or logs while it is closed.");
            bool changed = false;
            if (ImGui::MenuItem(ICON_FA_ALIGN_LEFT "  Wrap lines", nullptr, m_filter.Wrap))
            {
                m_filter.Wrap = !m_filter.Wrap;
                changed = true;
            }
            if (ImGui::MenuItem(ICON_FA_CLOCK "  Show the time", nullptr, m_filter.Time))
            {
                m_filter.Time = !m_filter.Time;
                changed = true;
            }
            if (ImGui::MenuItem(ICON_FA_HASHTAG "  Integers in hex", nullptr, m_filter.Hex))
            {
                m_filter.Hex = !m_filter.Hex;
                changed = true;
            }
            if (changed)
                SaveViewSettings();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run"))
        {
            item(ICON_FA_PLAY "  Run", Command::LuaExecute);
            item(ICON_FA_STOP "  Stop", Command::LuaStop, false, Running());
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void LuaWindow::DrawFooter(bool script)
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        const float total = ImGui::GetContentRegionAvail().x;
        const float origin = ImGui::GetCursorPosX();
        const bool running = Running();
        if (running)
        {
            if (DangerButton(ICON_FA_STOP "  Stop", Px(96.0f, 0.0f)))
                Stop();
            ImGui::SameLine();
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine();
        }
        else if (script)
        {
            if (AccentButton(ICON_FA_PLAY "  Run", Px(96.0f, 0.0f)))
                ExecuteScript();
            ImGui::SameLine();
        }
        std::string status;
        if (script)
        {
            const std::string keys = ShortcutLabel(Command::LuaExecute);
            status = (keys.empty() ? std::string() : keys + "  \xC2\xB7  ") + "line " + std::to_string(m_editor.CursorLine) + ", column " + std::to_string(m_editor.CursorColumn);
        }
        else
        {
            const Lua::ConsoleCounts counts = lua.Console().Counts();
            const std::size_t errors = counts.Levels[static_cast<std::size_t>(LogLevel::Error)];
            const std::size_t warnings = counts.Levels[static_cast<std::size_t>(LogLevel::Warning)];
            status = Grouped(lua.Console().Count()) + (lua.Console().Count() == 1 ? " entry" : " entries");
            if (errors)
                status += "  \xC2\xB7  " + Grouped(errors) + (errors == 1 ? " error" : " errors");
            if (warnings)
                status += "  \xC2\xB7  " + Grouped(warnings) + (warnings == 1 ? " warning" : " warnings");
        }
        bool show = lua.ShowOnPrint();
        const float checkWidth = CheckboxWidth("Show on print");
        const float right = origin + total - checkWidth;
        TextFitted(status, std::max(Px(40.0f), right - ImGui::GetCursorPosX() - Px(8.0f)), ManifoldTheme::Muted);
        ImGui::SameLine();
        if (right > ImGui::GetCursorPosX())
            ImGui::SetCursorPosX(right);
        if (ImGui::Checkbox("Show on print", &show))
            RememberShowOnPrint(show);
        ItemTooltip("Opens this window when a script prints or logs while it is closed. Saved with the settings.");
    }

    void LuaWindow::DrawConsole()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        bool changed = false;
        m_view.DrawFilterBar(lua.Console(), m_filter, changed, [this]() { m_engine.Lua().ClearOutput(); });
        if (changed && (m_filter.Wrap != m_engine.Options().LuaWrapOutput || m_filter.Time != m_engine.Options().LuaConsoleTime || m_filter.Hex != m_engine.Options().LuaConsoleHex))
            SaveViewSettings();
        ImGui::Spacing();
        const float footer = ImGui::GetFrameHeightWithSpacing();
        PushMono();
        const float row = std::round(ImGui::GetTextLineHeight() + Px(3.0f));
        PopMono();
        const int inputLines = std::clamp(static_cast<int>(std::count(m_input.begin(), m_input.end(), '\n')) + 1, 1, m_inputEditor.ConsoleMaxLines);
        const float inputHeight = static_cast<float>(inputLines) * row + Px(8.0f) + ImGui::GetStyle().ItemSpacing.y;
        const float logHeight = std::max(Px(60.0f), ImGui::GetContentRegionAvail().y - footer - inputHeight - ImGui::GetStyle().ItemSpacing.y);
        m_view.Draw(lua.Console(), m_filter, ImVec2(0.0f, logHeight), Actions());

        const float runWidth = Px(70.0f);
        const float inputWidth = ImGui::GetContentRegionAvail().x - runWidth - ImGui::GetStyle().ItemSpacing.x;
        CodeEditor("##console", m_input, Language::Lua, ImVec2(inputWidth, 0.0f), m_inputEditor);
        if (std::exchange(m_inputEditor.Submitted, false))
            Submit();
        if (const int step = std::exchange(m_inputEditor.HistoryStep, 0); step != 0)
        {
            const std::optional<std::string> recalled = step < 0 ? m_history.Previous(m_input) : m_history.Next();
            if (recalled)
                m_input = *recalled;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(TrimView(m_input).empty());
        if (AccentButton(ICON_FA_PLAY "##submit", ImVec2(runWidth, ImGui::GetFrameHeight())))
            Submit();
        ImGui::EndDisabled();
        ItemTooltip("Runs the input (Enter). Expressions show their value.");
        DrawFooter(false);
    }

    void LuaWindow::DrawScript()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        const std::string name = m_path.empty() ? std::string("Untitled script") : WideToUtf8(m_path.filename().wstring());
        const bool dirty = m_text != m_savedText;
        ImGui::TextColored(ManifoldTheme::Label, "%s %s%s", ICON_FA_FILE_CODE, name.c_str(), dirty ? "  \xE2\x80\xA2" : "");
        if (!m_path.empty())
            ItemTooltip(WideToUtf8(m_path.wstring()).c_str());
        const float available = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        float editor = std::floor(available * m_scriptShare);
        float output = available - editor - Px(6.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        CodeEditor("##lua", m_text, Language::Lua, ImVec2(0.0f, editor), m_editor);
        ImGui::SetCursorPos(start);
        const ImVec2 screen = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (Splitter("##scriptsplit", false, Px(6.0f), &editor, &output, Px(80.0f), Px(60.0f)))
            m_scriptShare = editor / std::max(1.0f, available);
        const ImVec2 grip(std::floor(screen.x + width * 0.5f - Px(16.0f)), std::floor(screen.y + editor + Px(2.0f)));
        ImGui::GetWindowDrawList()->AddRectFilled(grip, ImVec2(grip.x + Px(32.0f), grip.y + Px(2.0f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.45f)), Px(1.0f));
        ImGui::SetCursorPos(ImVec2(start.x, start.y + editor + Px(6.0f)));
        m_scriptView.Draw(lua.Console(), m_filter, ImVec2(0.0f, std::max(Px(40.0f), output)), Actions());
        DrawFooter(true);
    }

    void LuaWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_MOON "  Lua Engine###lua", ImVec2(860.0f, 640.0f), ImGuiWindowFlags_MenuBar, ImVec2(420.0f, 320.0f)))
            return;
        DrawMenu();
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::LuaEngine && Pressed(info.Id))
                RunCommand(info.Id);
        }

        const bool docs = m_engine.Options().LuaDocsPanel;
        const float total = ImGui::GetContentRegionAvail().x;
        const float gap = Px(8.0f);
        float side = 0.0f;
        if (docs)
            side = std::clamp(m_docsWidth > 0.0f ? m_docsWidth : Px(340.0f), Px(220.0f), std::max(Px(220.0f), total - gap - Px(300.0f)));
        const float mainWidth = docs ? total - side - gap : total;
        const ImVec2 origin = ImGui::GetCursorPos();
        ImGui::BeginChild("##luamain", ImVec2(mainWidth, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("##luatabs"))
        {
            const ImGuiTabItemFlags consoleFlags = m_selectTab && m_tab == Tab::Console ? ImGuiTabItemFlags_SetSelected : 0;
            const ImGuiTabItemFlags scriptFlags = m_selectTab && m_tab == Tab::Script ? ImGuiTabItemFlags_SetSelected : 0;
            m_selectTab = false;
            if (ImGui::BeginTabItem(ICON_FA_TERMINAL "  Console", nullptr, consoleFlags))
            {
                if (m_tab != Tab::Console)
                    m_inputEditor.Focus = true;
                m_tab = Tab::Console;
                DrawConsole();
                ImGui::EndTabItem();
            }
            const std::string scriptLabel = std::string(ICON_FA_FILE_CODE "  Script") + (m_text != m_savedText ? " \xE2\x80\xA2" : "") + "###script";
            if (ImGui::BeginTabItem(scriptLabel.c_str(), nullptr, scriptFlags))
            {
                m_tab = Tab::Script;
                DrawScript();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        if (docs)
        {
            float left = mainWidth;
            ImGui::SetCursorPos(origin);
            if (Splitter("##docsside", true, gap, &left, &side, Px(300.0f), Px(220.0f)))
                m_docsWidth = side;
            ImGui::SetCursorPos(ImVec2(origin.x + total - side, origin.y));
            BeginCard("##docs", nullptr, ImVec2(side, ImGui::GetContentRegionAvail().y));
            m_docs.Draw(m_engine.Lua().Docs(), ImVec2(0.0f, 0.0f), [this](const std::string& text)
                {
                    if (m_tab == Tab::Console)
                    {
                        m_inputEditor.Insert = text;
                        m_inputEditor.Focus = true;
                    }
                    else
                    {
                        m_editor.Insert = text;
                        m_editor.Focus = true;
                    }
                });
            EndCard();
        }
        EndToolWindow();
    }
}
