#include "UI/Windows.h"

#include "App/Sounds.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        ImVec4 OutputColor(Lua::OutputKind kind)
        {
            switch (kind)
            {
            case Lua::OutputKind::Error: return ManifoldTheme::Error;
            case Lua::OutputKind::Warning: return ManifoldTheme::Warning;
            case Lua::OutputKind::Success: return ManifoldTheme::Accent;
            case Lua::OutputKind::Debug: return ManifoldTheme::Muted;
            case Lua::OutputKind::Info:
            case Lua::OutputKind::Print: break;
            }
            return ManifoldTheme::Text;
        }

        ImVec4 OutputTagColor(Lua::OutputKind kind)
        {
            return kind == Lua::OutputKind::Info ? ManifoldTheme::AddressText : OutputColor(kind);
        }
    }

    ProcessPicker::ProcessPicker(Workspace& workspace) : m_workspace(workspace)
    {
        Refresh();
    }

    void ProcessPicker::Show()
    {
        m_open = true;
        m_focusFilter = true;
        Refresh();
        RequestWindowRaise(ICON_FA_DESKTOP "  Processes###processes");
    }

    void ProcessPicker::Refresh()
    {
        m_list = Target::ListProcesses(true);
        std::sort(m_list.begin(), m_list.end(), [](const Target::ProcessEntry& a, const Target::ProcessEntry& b)
            {
                const bool aWindow = !a.WindowTitle.empty();
                const bool bWindow = !b.WindowTitle.empty();
                if (aWindow != bWindow)
                    return aWindow;
                return Lower(a.Name) < Lower(b.Name);
            });
    }

    void ProcessPicker::Draw()
    {
        if (!BeginToolWindow(ICON_FA_DESKTOP "  Processes###processes", &m_open, ImVec2(560.0f, 520.0f), 0, ImVec2(520.0f, 320.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        if (m_focusFilter)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusFilter = false;
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(250.0f));
        InputTextWithHint("##filter", ICON_FA_SEARCH " Name, title or pid", m_filter);
        ImGui::SameLine();
        ImGui::Checkbox("With window", &m_windowsOnly);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SYNC "  Refresh"))
            Refresh();
        if (!Target::IsElevated())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            ImGui::TextUnformatted(ICON_FA_SHIELD_ALT "  Without administrator rights some processes cannot be opened.");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Restart as administrator"))
                m_workspace.Window().RestartElevated();
        }

        const auto open = [&](std::uint32_t id)
            {
                const Status status = engine.OpenProcess(id);
                m_workspace.Report(status);
                if (status)
                    m_open = false;
            };
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##list", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing())))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, Px(60.0f));
            ImGui::TableSetupColumn("Process", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Window", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Bits", ImGuiTableColumnFlags_WidthFixed, Px(40.0f));
            ImGui::TableHeadersRow();
            const std::uint32_t current = engine.HasProcess() ? engine.Process()->Id() : 0;
            for (const Target::ProcessEntry& entry : m_list)
            {
                if (m_windowsOnly && entry.WindowTitle.empty())
                    continue;
                if (!m_filter.empty() && !ContainsNoCase(entry.Name, m_filter) && !ContainsNoCase(entry.WindowTitle, m_filter) && std::to_string(entry.Id).find(m_filter) == std::string::npos)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(entry.Id));
                const bool selected = m_selected == entry.Id;
                if (ImGui::Selectable(std::to_string(entry.Id).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = entry.Id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        open(entry.Id);
                }
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, !entry.Accessible ? ManifoldTheme::Muted : entry.Id == current ? ManifoldTheme::Accent : ManifoldTheme::Text);
                ImGui::TextUnformatted(entry.Name.c_str());
                ImGui::PopStyleColor();
                if (!entry.Path.empty())
                    ItemTooltip(WideToUtf8(entry.Path).c_str());
                ImGui::TableNextColumn();
                Muted(entry.WindowTitle);
                ImGui::TableNextColumn();
                Muted(entry.Is64Bit ? "64" : "32");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::BeginDisabled(m_selected == 0);
        if (AccentButton(ICON_FA_PLUG "  Open", Px(110.0f, 0.0f)) || (m_selected && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)))
            open(m_selected);
        ImGui::EndDisabled();
        ImGui::SameLine();
        Muted(std::to_string(m_list.size()) + " processes");
        EndToolWindow();
    }

    LuaWindow::LuaWindow(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core())
    {
        m_text = "-- Lua 5.4 with the Manifold API. Ctrl+Enter runs the script.\n"
                 "print(\"Process: \" .. getOpenedProcessID())\n";
        m_assist.Complete = [this](const std::string& chain, const std::string& prefix, bool method)
            {
                std::vector<CodeSuggestion> result;
                for (Lua::LuaCompletion& item : m_engine.Lua().Complete(chain, prefix, method))
                    result.push_back({ std::move(item.Name), std::move(item.Kind), std::move(item.Signature), std::move(item.Text) });
                return result;
            };
        m_assist.Signatures = [this](const std::string& chain, bool method) { return m_engine.Lua().CallSignatures(chain, method); };
        m_assist.Choices = [this](const std::string& chain, bool method, int argument) { return m_engine.Lua().CallArgumentChoices(chain, method, argument); };
        m_editor.Assist = &m_assist;
        m_editor.Focus = true;
    }

    void LuaWindow::FunctionList()
    {
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##functionfilter", ICON_FA_SEARCH " Filter", m_functionFilter);
        const double now = ImGui::GetTime();
        if (m_listedAt < 0.0 || now - m_listedAt > 2.0 || m_listedFilter != m_functionFilter)
        {
            m_listedAt = now;
            m_listedFilter = m_functionFilter;
            m_listed.clear();
            for (Lua::LuaCompletion& item : m_engine.Lua().Complete("", "", false))
            {
                if (item.Kind == "function" && (m_functionFilter.empty() || ContainsNoCase(item.Name, m_functionFilter)))
                    m_listed.push_back({ std::move(item.Name), std::move(item.Kind), std::move(item.Signature), std::move(item.Text) });
            }
        }
        Muted(std::to_string(m_listed.size()) + (m_listed.size() == 1 ? " function" : " functions"));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(4.0f, 3.0f));
        ImGui::BeginChild("##functionrows", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        PushMono();
        const float line = ImGui::GetTextLineHeight();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_listed.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const CodeSuggestion& item = m_listed[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                if (ImGui::Selectable("##function", false))
                {
                    m_editor.Insert = item.Name + "(";
                    m_editor.Focus = true;
                }
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float right = pos.x + width;
                const float nameWidth = ImGui::CalcTextSize(item.Name.c_str()).x;
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Text);
                ImGui::RenderTextEllipsis(draw, pos, ImVec2(right, pos.y + line), right, item.Name.c_str(), item.Name.c_str() + item.Name.size(), nullptr);
                ImGui::PopStyleColor();
                if (!item.Signature.empty() && pos.x + nameWidth < right - Px(12.0f))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                    ImGui::RenderTextEllipsis(draw, ImVec2(pos.x + nameWidth, pos.y), ImVec2(right, pos.y + line), right, item.Signature.c_str(),
                        item.Signature.c_str() + item.Signature.size(), nullptr);
                    ImGui::PopStyleColor();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                {
                    const std::vector<std::string> signatures = m_engine.Lua().CallSignatures(item.Name, false);
                    ShowTooltip([&]()
                        {
                            PushMono();
                            for (const std::string& signature : signatures.empty() ? std::vector<std::string>{ item.Name + item.Signature } : signatures)
                                ImGui::TextColored(ManifoldTheme::CodeRegister, "%s", signature.c_str());
                            PopMono();
                            if (!item.Text.empty())
                            {
                                ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
                                ImGui::TextUnformatted(item.Text.c_str());
                            }
                            Muted("Click to insert at the cursor");
                        });
                }
                ImGui::PopID();
            }
        }
        PopMono();
        ImGui::EndChild();
    }

    void LuaWindow::Execute()
    {
        m_editor.ErrorLine = 0;
        m_followOutput = true;
        Lua::LuaEngine& lua = m_engine.Lua();
        if (!m_engine.Async())
        {
            MarkError(lua.Run(m_text, "console"));
            return;
        }
        if (m_task && lua.TaskAlive(m_task))
            return;
        m_stopped = false;
        const std::weak_ptr<int> alive = m_alive;
        m_task = lua.StartTask(m_text, "console", [this, alive](const Status& status, const std::string&)
            {
                if (alive.expired())
                    return;
                m_task = 0;
                MarkError(status);
            });
    }

    void LuaWindow::MarkError(const Status& status)
    {
        if (!status)
        {
            if (!std::exchange(m_stopped, false))
                Sounds::Play(Sounds::Cue::Error);
            const std::size_t colon = status.Text.find("console:");
            if (colon != std::string::npos)
            {
                const auto line = ParseDecimal(status.Text.substr(colon + 8, status.Text.find(':', colon + 8) - colon - 8));
                if (line)
                    m_editor.ErrorLine = static_cast<int>(*line);
            }
        }
    }

    void LuaWindow::RememberShowOnPrint(bool show)
    {
        m_engine.Lua().RememberShowOnPrint(show);
        m_workspace.Report(m_engine.Options().Save(Paths::SettingsFile()));
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
                m_path = *path;
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
                m_engine.Notify(LogLevel::Success, "Saved " + WideToUtf8(path->filename().wstring()));
            }
            else
                m_engine.Notify(LogLevel::Error, "Could not write " + WideToUtf8(path->wstring()));
            break;
        }
        case Command::LuaExecute: Execute(); break;
        case Command::LuaClearOutput: m_engine.Lua().ClearOutput(); break;
        case Command::LuaFunctions: m_showFunctions = !m_showFunctions; break;
        default: break;
        }
    }

    void LuaWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_MOON "  Lua Engine###lua", &m_open, ImVec2(780.0f, 620.0f), ImGuiWindowFlags_MenuBar, ImVec2(640.0f, 420.0f)))
        {
            EndToolWindow();
            return;
        }
        if (ImGui::BeginMenuBar())
        {
            const auto item = [&](const char* label, Command command, bool selected = false)
                {
                    if (CommandItem(label, command, selected))
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
                item(ICON_FA_BOOK "  Functions", Command::LuaFunctions, m_showFunctions);
                item(ICON_FA_ERASER "  Clear output", Command::LuaClearOutput);
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_EYE "  Show on print", nullptr, m_engine.Lua().ShowOnPrint()))
                    RememberShowOnPrint(!m_engine.Lua().ShowOnPrint());
                ItemTooltip("Opens this window when a script prints or logs while it is closed.");
                ImGui::EndMenu();
            }
            item(ICON_FA_PLAY "  Execute", Command::LuaExecute);
            ImGui::EndMenuBar();
        }
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::LuaEngine && Pressed(info.Id))
                RunCommand(info.Id);
        }

        const float total = ImGui::GetContentRegionAvail().x;
        const float gap = Px(8.0f);
        float side = 0.0f;
        if (m_showFunctions)
            side = std::clamp(m_functionsWidth > 0.0f ? m_functionsWidth : Px(340.0f), Px(220.0f), std::max(Px(220.0f), total - gap - Px(300.0f)));
        const float functionsWidth = m_showFunctions ? side + gap : 0.0f;
        const ImVec2 origin = ImGui::GetCursorPos();
        ImGui::BeginGroup();
        const float available = ImGui::GetContentRegionAvail().y;
        float output = std::floor(available * m_outputShare);
        float editor = available - output - Px(6.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        BeginCard("##output", nullptr, ImVec2(ImGui::GetContentRegionAvail().x - functionsWidth, output));
        Lua::LuaEngine& lua = m_engine.Lua();
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeightWithSpacing();
        const std::uint64_t generation = lua.OutputGeneration();
        if (const std::uint64_t dropped = lua.OutputDropped())
            Muted(Grouped(dropped) + (dropped == 1 ? " earlier line is" : " earlier lines are") + " no longer kept");
        PushMono();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(lua.OutputCount()));
        while (clipper.Step())
        {
            lua.VisitOutput(static_cast<std::size_t>(clipper.DisplayStart), static_cast<std::size_t>(clipper.DisplayEnd), [](std::size_t, const Lua::OutputLine& line)
                {
                    const char* text = line.Text.data();
                    const char* end = text + line.Text.size();
                    const ImVec4 message = OutputColor(line.Kind);
                    if (line.HeadEnd == 0 || line.HeadEnd > line.Text.size())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, message);
                        ImGui::TextUnformatted(text, end);
                        ImGui::PopStyleColor();
                        return;
                    }
                    const auto segment = [](const char* from, const char* to, const ImVec4& color, bool more)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, color);
                            ImGui::TextUnformatted(from, to);
                            ImGui::PopStyleColor();
                            if (more)
                                ImGui::SameLine(0.0f, 0.0f);
                        };
                    const std::size_t timeEnd = std::min<std::size_t>(line.TimeEnd, line.HeadEnd);
                    const std::size_t tagEnd = std::clamp<std::size_t>(line.TagEnd, timeEnd, line.HeadEnd);
                    segment(text, text + timeEnd, ManifoldTheme::Muted, true);
                    segment(text + timeEnd, text + tagEnd, OutputTagColor(line.Kind), true);
                    segment(text + tagEnd, text + line.HeadEnd, ManifoldTheme::Label, true);
                    segment(text + line.HeadEnd, end, message, false);
                });
        }
        if (m_seenOutput != generation)
        {
            m_seenOutput = generation;
            if (atBottom || m_followOutput)
                ImGui::SetScrollHereY(1.0f);
            m_followOutput = false;
        }
        PopMono();
        if (ImGui::BeginPopupContextWindow())
        {
            if (ImGui::MenuItem("Clear"))
                lua.ClearOutput();
            if (ImGui::MenuItem("Copy all"))
            {
                std::string text;
                lua.VisitOutput(0, lua.OutputCount(), [&](std::size_t, const Lua::OutputLine& line)
                    {
                        text += line.Text;
                        text += '\n';
                    });
                ImGui::SetClipboardText(text.c_str());
            }
            ImGui::EndPopup();
        }
        EndCard();
        ImGui::SetCursorPos(start);
        if (Splitter("##luasplit", false, Px(6.0f), &output, &editor, Px(60.0f), Px(80.0f)))
            m_outputShare = output / available;
        ImGui::SetCursorPos(ImVec2(start.x, start.y + output + Px(6.0f)));
        CodeEditor("##lua", m_text, Language::Lua, ImVec2(ImGui::GetContentRegionAvail().x - functionsWidth, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()), m_editor);
        const bool running = m_task && m_engine.Lua().TaskAlive(m_task);
        if (running)
        {
            if (DangerButton(ICON_FA_STOP "  Stop", Px(110.0f, 0.0f)))
            {
                m_stopped = true;
                m_engine.Lua().StopTask(m_task);
            }
            ImGui::SameLine();
            Spinner(ImGui::GetTextLineHeight(), ManifoldTheme::Accent);
            ImGui::SameLine();
        }
        else if (AccentButton(ICON_FA_PLAY "  Execute", Px(110.0f, 0.0f)))
        {
            Execute();
        }
        if (!running)
            ImGui::SameLine();
        const std::string keys = ShortcutLabel(Command::LuaExecute);
        Muted((keys.empty() ? std::string() : keys + "  ·  ") + "line " + std::to_string(m_editor.CursorLine) + ", column " + std::to_string(m_editor.CursorColumn));
        {
            bool show = m_engine.Lua().ShowOnPrint();
            const ImGuiStyle& style = ImGui::GetStyle();
            const float width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Show on print").x;
            ImGui::SameLine();
            const float right = origin.x + total - functionsWidth - width;
            if (right > ImGui::GetCursorPosX())
                ImGui::SetCursorPosX(right);
            if (ImGui::Checkbox("Show on print", &show))
                RememberShowOnPrint(show);
            ItemTooltip("Opens this window when a script prints or logs while it is closed. Saved with the settings.");
        }
        ImGui::EndGroup();

        if (m_showFunctions)
        {
            float left = total - side - gap;
            ImGui::SetCursorPos(origin);
            if (Splitter("##functionsplit", true, gap, &left, &side, Px(300.0f), Px(220.0f)))
                m_functionsWidth = side;
            ImGui::SetCursorPos(ImVec2(origin.x + total - side, origin.y));
            BeginCard("##functions", nullptr, ImVec2(side, ImGui::GetContentRegionAvail().y));
            FunctionList();
            EndCard();
        }
        EndToolWindow();
    }

    LogWindow::LogWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void LogWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_STREAM "  Log###log", &m_open, ImVec2(860.0f, 480.0f), 0, ImVec2(640.0f, 260.0f)))
        {
            EndToolWindow();
            return;
        }
        if (ImGui::BeginTabBar("##logtabs"))
        {
            if (ImGui::BeginTabItem("Log"))
            {
                const char* levels[] = { "Debug", "Info", "Success", "Warnings", "Errors" };
                ImGui::SetNextItemWidth(Px(110.0f));
                ImGui::Combo("##level", &m_level, levels, static_cast<int>(std::size(levels)));
                ImGui::SameLine();
                const ImGuiStyle& style = ImGui::GetStyle();
                const float boxes = (ImGui::GetFrameHeight() + style.ItemInnerSpacing.x) * 2.0f + ImGui::CalcTextSize("Follow").x + ImGui::CalcTextSize("Debug messages").x;
                const float buttons = ImGui::CalcTextSize(ICON_FA_ERASER "  Clear").x + ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN "  Log file").x + style.FramePadding.x * 4.0f;
                const float rest = boxes + buttons + style.ItemSpacing.x * 5.0f;
                ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - rest, Px(120.0f), Px(220.0f)));
                InputTextWithHint("##search", ICON_FA_SEARCH " Search", m_search);
                ImGui::SameLine();
                ImGui::Checkbox("Follow", &m_follow);
                ImGui::SameLine();
                bool verbose = RuntimeLog::Verbose();
                if (ImGui::Checkbox("Debug messages", &verbose))
                {
                    RuntimeLog::SetVerbose(verbose);
                    m_workspace.Core().Options().VerboseLog = verbose;
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_ERASER "  Clear"))
                    RuntimeLog::Clear();
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Log file"))
                {
                    const std::wstring folder = RuntimeLog::FilePath().parent_path().wstring();
                    ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                if (m_generation != RuntimeLog::Generation())
                {
                    m_generation = RuntimeLog::Generation();
                    m_entries = RuntimeLog::Entries();
                }
                std::vector<const LogEntry*> shown;
                for (const LogEntry& entry : m_entries)
                {
                    if (RuntimeLog::EntryMatches(entry, static_cast<LogLevel>(m_level), m_search))
                        shown.push_back(&entry);
                }
                const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("##entries", 4, flags))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Px(64.0f));
                    ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthFixed, Px(110.0f));
                    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 4.0f);
                    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                    ImGui::TableHeadersRow();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(shown.size()));
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const LogEntry& entry = *shown[static_cast<std::size_t>(i)];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            Muted(entry.Time);
                            ImGui::TableNextColumn();
                            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.Level));
                            ImGui::Text("%s %s", LevelIcon(entry.Level), entry.Area.c_str());
                            ImGui::PopStyleColor();
                            ImGui::TableNextColumn();
                            ImGui::PushID(i);
                            ImGui::PushStyleColor(ImGuiCol_Text, entry.Level == LogLevel::Error ? ManifoldTheme::Error : entry.Level == LogLevel::Warning ? ManifoldTheme::Warning : ManifoldTheme::Text);
                            ImGui::Selectable(entry.Message.c_str(), false);
                            ImGui::PopStyleColor();
                            ErrorTooltip(entry.Message);
                            if (ImGui::BeginPopupContextItem("##copy"))
                            {
                                if (ImGui::MenuItem("Copy message"))
                                    ImGui::SetClipboardText(entry.Message.c_str());
                                if (ImGui::MenuItem("Copy line"))
                                    ImGui::SetClipboardText(("[" + entry.Time + "] [" + RuntimeLog::LevelName(entry.Level) + "] [" + entry.Area + "] [" + entry.Trace + "] " + entry.Message).c_str());
                                ImGui::EndPopup();
                            }
                            ImGui::PopID();
                            ImGui::TableNextColumn();
                            Muted(entry.Trace);
                        }
                    }
                    if (m_follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - Px(40.0f))
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Error codes"))
            {
                const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("##codes", 4, flags))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed, Px(80.0f));
                    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableSetupColumn("Cause", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                    ImGui::TableSetupColumn("Help", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                    ImGui::TableHeadersRow();
                    for (const ErrorInfo& info : ErrorTable())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextColored(ManifoldTheme::Accent, "%s", ErrorName(info.Code).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%.*s", static_cast<int>(info.Message.size()), info.Message.data());
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%.*s", static_cast<int>(info.Cause.size()), info.Cause.data());
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
                        ImGui::TextWrapped("%.*s", static_cast<int>(info.Help.size()), info.Help.data());
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        EndToolWindow();
    }

    SymbolsWindow::SymbolsWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void SymbolsWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_TAGS "  Symbols and modules###symbols", &m_open, ImVec2(640.0f, 520.0f), 0, ImVec2(480.0f, 320.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        {
            const bool hasProcess = engine.HasProcess();
            ImGui::BeginDisabled(!hasProcess);
            if (ImGui::Button(ICON_FA_DOWNLOAD "  Load symbols"))
                engine.LoadSymbols(false);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_GLOBE "  Load Windows symbols"))
                engine.LoadSymbols(true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (engine.SymbolsActive() && !engine.SymbolsLoaded())
                Muted("Loading " + std::to_string(engine.SymbolsModulesDone()) + "/" + std::to_string(engine.SymbolsModulesTotal()) + " modules...");
            else if (engine.SymbolsLoaded())
                Muted("Symbols loaded");
            else
                Muted("PDB symbols not loaded");
            ImGui::Separator();
        }
        if (ImGui::BeginTabBar("##symboltabs"))
        {
            if (ImGui::BeginTabItem("Registered symbols"))
            {
                ImGui::SetNextItemWidth(Px(160.0f));
                InputTextWithHint("##name", "name", m_name);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Px(220.0f));
                InputTextWithHint("##address", "address", m_address);
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_PLUS "  Register"))
                {
                    Status error;
                    if (const auto value = engine.Evaluate(m_address, &error))
                    {
                        error = engine.Symbols().Register(m_name, *value, "user");
                        if (error)
                        {
                            m_name.clear();
                            m_address.clear();
                        }
                    }
                    m_error = error.Ok() ? "" : error.Text;
                }
                if (!m_error.empty())
                    TextWithError(m_error, ManifoldTheme::Error);
                const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
                if (ImGui::BeginTable("##registered", 4, flags))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Address");
                    ImGui::TableSetupColumn("Owner");
                    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, Px(30.0f));
                    ImGui::TableHeadersRow();
                    for (const Target::UserSymbol& symbol : engine.Symbols().Registered())
                    {
                        ImGui::TableNextRow();
                        ImGui::PushID(symbol.Name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(symbol.Name.c_str());
                        ImGui::TableNextColumn();
                        PushMono();
                        if (ImGui::Selectable(Hex(symbol.Address).c_str()))
                            m_workspace.OpenMemoryViewer(symbol.Address);
                        PopMono();
                        ImGui::TableNextColumn();
                        Muted(symbol.Owner);
                        ImGui::TableNextColumn();
                        if (ImGui::SmallButton(ICON_FA_TIMES))
                            engine.Symbols().Unregister(symbol.Name);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Modules"))
            {
                if (!engine.HasProcess())
                    m_workspace.NoProcessState(ICON_FA_CUBES, "Open a process to list the modules it has loaded.");
                else
                {
                    if (ImGui::Button(ICON_FA_SYNC "  Refresh"))
                        engine.Process()->RefreshModules();
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(Px(220.0f));
                    InputTextWithHint("##filter", ICON_FA_SEARCH " Filter", m_filter);
                    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
                    if (ImGui::BeginTable("##modules", 3, flags))
                    {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Module");
                        ImGui::TableSetupColumn("Base");
                        ImGui::TableSetupColumn("Size");
                        ImGui::TableHeadersRow();
                        for (const Target::ModuleInfo& module : *engine.Process()->Modules())
                        {
                            if (!m_filter.empty() && !ContainsNoCase(module.Name, m_filter))
                                continue;
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::PushID(module.Name.c_str());
                            if (ImGui::Selectable(module.Name.c_str(), m_module == module.Name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                            {
                                m_module = module.Name;
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                                    m_workspace.OpenMemoryViewer(module.Base, true);
                            }
                            ItemTooltip(WideToUtf8(module.Path).c_str());
                            ImGui::PopID();
                            ImGui::TableNextColumn();
                            PushMono();
                            ImGui::TextUnformatted(Hex(module.Base).c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(Hex(module.Size).c_str());
                            PopMono();
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Exports"))
            {
                if (!engine.HasProcess())
                    m_workspace.NoProcessState(ICON_FA_SITEMAP, "Open a process to browse the functions its modules export.");
                else
                {
                    ImGui::SetNextItemWidth(Px(200.0f));
                    if (ImGui::BeginCombo("##module", m_module.empty() ? "Pick a module" : m_module.c_str(), ImGuiComboFlags_HeightLarge))
                    {
                        for (const Target::ModuleInfo& module : *engine.Process()->Modules())
                        {
                            if (ImGui::Selectable(module.Name.c_str(), m_module == module.Name))
                                m_module = module.Name;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(Px(220.0f));
                    InputTextWithHint("##exportfilter", ICON_FA_SEARCH " Filter", m_filter);
                    if (const auto module = engine.Process()->FindModule(m_module))
                    {
                        const auto exports = engine.Symbols().ExportsOf(*module);
                        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
                        if (ImGui::BeginTable("##exports", 2, flags))
                        {
                            ImGui::TableSetupScrollFreeze(0, 1);
                            ImGui::TableSetupColumn("Export");
                            ImGui::TableSetupColumn("Address");
                            ImGui::TableHeadersRow();
                            for (const Target::ExportSymbol& symbol : exports)
                            {
                                if (!m_filter.empty() && !ContainsNoCase(symbol.Name, m_filter))
                                    continue;
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::PushID(symbol.Name.c_str());
                                if (ImGui::Selectable(symbol.Name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                                    m_workspace.OpenMemoryViewer(symbol.Address);
                                ImGui::PopID();
                                ImGui::TableNextColumn();
                                PushMono();
                                ImGui::TextUnformatted(Hex(symbol.Address).c_str());
                                PopMono();
                            }
                            ImGui::EndTable();
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        EndToolWindow();
    }

    RegionsWindow::RegionsWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void RegionsWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_TH_LIST "  Memory regions###regions", &m_open, ImVec2(760.0f, 540.0f), 0, ImVec2(560.0f, 300.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        if (!engine.HasProcess())
        {
            m_workspace.NoProcessState(ICON_FA_TH_LIST, "Open a process to list its memory regions and their protection.");
            EndToolWindow();
            return;
        }
        if (ImGui::Button(ICON_FA_SYNC "  Refresh") || m_dirty)
        {
            m_regions = engine.Process()->Regions(0, engine.Process()->HighestAddress());
            m_dirty = false;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Committed only", &m_committedOnly);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(220.0f));
        InputTextWithHint("##filter", ICON_FA_SEARCH " Module or protection", m_filter);
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        PushMono();
        if (ImGui::BeginTable("##regions", 6, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Base");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Protect");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Allocation");
            ImGui::TableSetupColumn("Module");
            ImGui::TableHeadersRow();
            for (const Target::RegionInfo& region : m_regions)
            {
                if (m_committedOnly && !region.Committed())
                    continue;
                const auto module = engine.Process()->ModuleAt(region.Base);
                const std::string protect = region.Committed() ? Target::ProtectText(region.Protect) : region.State == 0x2000 ? "reserved" : "free";
                if (!m_filter.empty() && !(module && ContainsNoCase(module->Name, m_filter)) && !ContainsNoCase(protect, m_filter) &&
                    !ContainsNoCase(Target::RegionTypeText(region.Type), m_filter))
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(reinterpret_cast<void*>(static_cast<std::uintptr_t>(region.Base)));
                if (ImGui::Selectable(Hex(region.Base, 12).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    m_workspace.OpenMemoryViewer(region.Base, !region.Executable());
                if (ImGui::BeginPopupContextItem("##regionmenu"))
                {
                    if (ImGui::MenuItem("Show in hex view"))
                        m_workspace.OpenMemoryViewer(region.Base, true);
                    if (ImGui::MenuItem("Disassemble"))
                        m_workspace.OpenMemoryViewer(region.Base);
                    if (ImGui::MenuItem("Copy base"))
                        ImGui::SetClipboardText(Hex(region.Base).c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ByteSize(region.Size).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(protect.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Target::RegionTypeText(region.Type).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Hex(region.AllocationBase).c_str());
                ImGui::TableNextColumn();
                Muted(module ? module->Name : "");
            }
            ImGui::EndTable();
        }
        PopMono();
        EndToolWindow();
    }
}
