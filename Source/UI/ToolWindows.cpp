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
#include <climits>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
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

        const auto open = [&](ProcessId id)
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
            const ProcessId current = engine.HasProcess() ? engine.Process()->Id() : ProcessId::None;
            for (const Target::ProcessEntry& entry : m_list)
            {
                if (m_windowsOnly && entry.WindowTitle.empty())
                    continue;
                if (!m_filter.empty() && !ContainsNoCase(entry.Name, m_filter) && !ContainsNoCase(entry.WindowTitle, m_filter) && ToText(entry.Id).find(m_filter) == std::string::npos)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(Raw(entry.Id)));
                const bool selected = m_selected == Raw(entry.Id);
                if (ImGui::Selectable(ToText(entry.Id).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_selected = Raw(entry.Id);
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
            open(MakeProcessId(m_selected));
        ImGui::EndDisabled();
        ImGui::SameLine();
        Muted(std::to_string(m_list.size()) + " processes");
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
