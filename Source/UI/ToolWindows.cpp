#include "UI/ToolWindows.h"

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

    namespace
    {
        struct ProcessGroup
        {
            std::string Key;
            std::vector<std::size_t> Members;
            std::size_t Primary = 0;
            bool App = false;
            bool Matched = false;
        };

        enum class RowKind
        {
            AppsHeader,
            BackgroundHeader,
            Group,
            Member
        };

        struct PickerRow
        {
            RowKind Kind = RowKind::Group;
            std::size_t Group = 0;
            std::size_t Entry = 0;
            std::string Id;
        };

        void Pill(ImDrawList* draw, float& right, float centerY, const char* text, ImVec4 color)
        {
            const ImVec2 size = ImGui::CalcTextSize(text);
            const float padX = Px(6.0f);
            const float height = std::round(size.y + Px(3.0f));
            const ImVec2 max(right, std::round(centerY + height * 0.5f));
            const ImVec2 min(right - size.x - padX * 2.0f, max.y - height);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(color, 0.14f)), height * 0.5f);
            draw->AddText(ImVec2(min.x + padX, std::round(min.y + (height - size.y) * 0.5f)), ImGui::GetColorU32(color), text);
            right = min.x - Px(6.0f);
        }
    }

    void ProcessPicker::Draw()
    {
        if (!BeginToolWindow(ICON_FA_DESKTOP "  Processes###processes", &m_open, ImVec2(620.0f, 580.0f), 0, ImVec2(460.0f, 340.0f)))
        {
            EndToolWindow();
            return;
        }
        Engine& engine = m_workspace.Core();
        const ImGuiStyle& style = ImGui::GetStyle();

        const float refreshSide = ImGui::GetFrameHeight();
        const float segmentWidth = std::round(Px(124.0f));
        if (m_focusFilter)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusFilter = false;
        }
        ImGui::SetNextItemWidth(std::max(Px(120.0f), ImGui::GetContentRegionAvail().x - segmentWidth - refreshSide - style.ItemSpacing.x * 2.0f));
        InputTextWithHint("##filter", ICON_FA_SEARCH "  Name, window title or pid", m_filter);
        ImGui::SameLine();
        int mode = m_windowsOnly ? 0 : 1;
        if (Segmented("##mode", &mode, { "Apps", "All" }, segmentWidth))
            m_windowsOnly = mode == 0;
        ItemTooltip("Apps are programs with a window. All adds the background processes.");
        ImGui::SameLine();
        {
            const GhostButtons ghost;
            if (IconButton(ICON_FA_SYNC "##refresh", "Read the process list again"))
                Refresh();
        }

        if (!Target::IsElevated())
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = ImGui::GetFrameHeight() + Px(6.0f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.09f)), Px(6.0f));
            draw->AddRect(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.30f)), Px(6.0f));
            const char* restart = ICON_FA_SHIELD_ALT "  Restart as administrator";
            const float button = ImGui::CalcTextSize(restart).x + style.FramePadding.x * 2.0f;
            ImGui::SetCursorScreenPos(ImVec2(at.x + Px(10.0f), at.y + Px(3.0f)));
            ImGui::AlignTextToFramePadding();
            TextFitted("Some processes can only be opened with administrator rights.", std::max(Px(40.0f), width - button - Px(30.0f)), ManifoldTheme::Warning);
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(at.x + width - button - Px(3.0f), at.y + Px(3.0f)));
            if (ImGui::Button(restart))
                m_workspace.Window().RestartElevated();
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height + style.ItemSpacing.y));
        }

        const std::string filter = Trim(m_filter);
        const bool pidFilter = !filter.empty() && std::all_of(filter.begin(), filter.end(), [](char c) { return c >= '0' && c <= '9'; });
        const ProcessId current = engine.HasProcess() ? engine.Process()->Id() : ProcessId::None;
        const std::string last = Lower(engine.Options().LastProcess);
        std::vector<ProcessGroup> groups;
        std::map<std::string, std::size_t> byKey;
        for (std::size_t i = 0; i < m_list.size(); ++i)
        {
            const Target::ProcessEntry& entry = m_list[i];
            const std::string key = Lower(entry.Name);
            auto found = byKey.find(key);
            if (found == byKey.end())
            {
                found = byKey.emplace(key, groups.size()).first;
                groups.push_back({ key, {}, i, false, false });
            }
            ProcessGroup& group = groups[found->second];
            group.Members.push_back(i);
            if (!entry.WindowTitle.empty() && !group.App)
            {
                group.App = true;
                group.Primary = i;
            }
            if (entry.Id == current && !group.App)
                group.Primary = i;
            if (filter.empty() || ContainsNoCase(entry.Name, filter) || ContainsNoCase(entry.WindowTitle, filter) || ToText(entry.Id).find(filter) != std::string::npos)
                group.Matched = true;
        }
        std::vector<std::size_t> apps;
        std::vector<std::size_t> background;
        for (std::size_t g = 0; g < groups.size(); ++g)
        {
            if (!groups[g].Matched)
                continue;
            if (groups[g].App)
                apps.push_back(g);
            else if (!m_windowsOnly)
                background.push_back(g);
        }
        const auto ordered = [&](std::size_t a, std::size_t b)
            {
                const bool lastA = !last.empty() && groups[a].Key == last;
                const bool lastB = !last.empty() && groups[b].Key == last;
                if (lastA != lastB)
                    return lastA;
                return groups[a].Key < groups[b].Key;
            };
        std::sort(apps.begin(), apps.end(), ordered);
        std::sort(background.begin(), background.end(), ordered);

        std::vector<PickerRow> rows;
        std::vector<std::size_t> selectable;
        const auto addGroups = [&](const std::vector<std::size_t>& list)
            {
                for (const std::size_t g : list)
                {
                    const ProcessGroup& group = groups[g];
                    selectable.push_back(rows.size());
                    rows.push_back({ RowKind::Group, g, group.Primary, "g:" + group.Key });
                    const bool expanded = group.Members.size() > 1 && (m_expanded.count(group.Key) > 0 || pidFilter);
                    if (!expanded)
                        continue;
                    for (const std::size_t member : group.Members)
                    {
                        selectable.push_back(rows.size());
                        rows.push_back({ RowKind::Member, g, member, "m:" + ToText(m_list[member].Id) });
                    }
                }
            };
        if (!apps.empty())
        {
            rows.push_back({ RowKind::AppsHeader, 0, apps.size(), {} });
            addGroups(apps);
        }
        if (!background.empty())
        {
            rows.push_back({ RowKind::BackgroundHeader, 0, background.size(), {} });
            addGroups(background);
        }

        const auto select = [&](const PickerRow& row)
            {
                m_selectedRow = row.Id;
                m_selected = Raw(m_list[row.Entry].Id);
            };
        const auto selectedIndex = [&]() -> int
            {
                for (std::size_t s = 0; s < selectable.size(); ++s)
                {
                    if (rows[selectable[s]].Id == m_selectedRow)
                        return static_cast<int>(s);
                }
                return -1;
            };
        if (filter != m_lastFilter || m_windowsOnly != m_lastMode)
        {
            m_lastFilter = filter;
            m_lastMode = m_windowsOnly;
            if (!filter.empty() && !selectable.empty())
            {
                select(rows[selectable.front()]);
                m_scrollToSelected = true;
            }
            else if (filter.empty() && selectedIndex() < 0 && !last.empty())
            {
                for (const std::size_t index : selectable)
                {
                    if (rows[index].Kind == RowKind::Group && groups[rows[index].Group].Key == last)
                    {
                        select(rows[index]);
                        m_scrollToSelected = true;
                        break;
                    }
                }
            }
        }
        if (selectedIndex() < 0 && !m_selectedRow.empty())
        {
            m_selectedRow.clear();
            m_selected = 0;
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !selectable.empty())
        {
            const int step = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
            if (step != 0)
            {
                const int at = selectedIndex();
                const int next = std::clamp(at < 0 ? (step > 0 ? 0 : static_cast<int>(selectable.size()) - 1) : at + step, 0, static_cast<int>(selectable.size()) - 1);
                select(rows[selectable[static_cast<std::size_t>(next)]]);
                m_scrollToSelected = true;
            }
        }

        const auto open = [&](ProcessId id)
            {
                const Status status = engine.OpenProcess(id);
                m_workspace.Report(status);
                if (status)
                    m_open = false;
            };
        std::optional<ProcessId> openNow;
        const float footer = ImGui::GetFrameHeight() + style.ItemSpacing.y + Px(2.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Card);
        ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(6.0f, 6.0f));
        ImGui::BeginChild("##processlist", ImVec2(0.0f, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (rows.empty())
        {
            EmptyState(ICON_FA_SEARCH, "No process matches", filter.empty() ? std::string("No process is running that Manifold can list.") : "Nothing is called \"" + filter + "\".", {}, false);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, Px(2.0f)));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const float line = ImGui::GetTextLineHeight();
        for (std::size_t r = 0; r < rows.size(); ++r)
        {
            const PickerRow& row = rows[r];
            if (row.Kind == RowKind::AppsHeader || row.Kind == RowKind::BackgroundHeader)
            {
                if (r > 0)
                    ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(6.0f));
                ImGui::PushFont(Fonts::Bold(), 0.0f);
                ImGui::TextColored(ManifoldTheme::Label, "%s", row.Kind == RowKind::AppsHeader ? "Apps" : "Background processes");
                ImGui::PopFont();
                ImGui::SameLine();
                Muted(std::to_string(row.Entry));
                ImGui::Dummy(ImVec2(0.0f, Px(1.0f)));
                continue;
            }
            const Target::ProcessEntry& entry = m_list[row.Entry];
            const ProcessGroup& group = groups[row.Group];
            const bool member = row.Kind == RowKind::Member;
            const bool tall = !member && group.App;
            const float height = std::round(tall ? Px(42.0f) : Px(28.0f));
            ImGui::PushID(row.Id.c_str());
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + rowWidth, min.y + height);
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowWidth, height));
            const bool hovered = ImGui::IsItemHovered();
            const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const bool selected = row.Id == m_selectedRow;
            if (selected && m_scrollToSelected)
            {
                ImGui::SetScrollHereY(0.5f);
                m_scrollToSelected = false;
            }
            const bool visible = ImGui::IsRectVisible(min, max);
            if (visible)
            {
                const float rounding = Px(6.0f);
                if (selected)
                    draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, hovered ? 0.22f : 0.17f)), rounding);
                else if (hovered)
                    draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.05f)), rounding);
                if (entry.Id == current)
                    draw->AddRectFilled(ImVec2(min.x, min.y + Px(7.0f)), ImVec2(min.x + Px(3.0f), max.y - Px(7.0f)), ImGui::GetColorU32(ManifoldTheme::Accent), Px(2.0f));
            }

            float right = max.x - Px(10.0f);
            const float centerY = (min.y + max.y) * 0.5f;
            bool caretHit = false;
            if (!member && group.Members.size() > 1)
            {
                const bool expanded = m_expanded.count(group.Key) > 0 || pidFilter;
                const char* caret = expanded ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT;
                const float caretSize = std::round(ImGui::GetFontSize() * 0.72f);
                const float caretWidth = ImGui::GetFont()->CalcTextSizeA(caretSize, FLT_MAX, 0.0f, caret).x;
                const ImRect caretRect(ImVec2(right - caretWidth - Px(8.0f), min.y), ImVec2(max.x, max.y));
                caretHit = caretRect.Contains(ImGui::GetIO().MousePos);
                if (visible)
                    draw->AddText(ImGui::GetFont(), caretSize, ImVec2(right - caretWidth, std::round(centerY - caretSize * 0.5f)),
                        ImGui::GetColorU32(caretHit && hovered ? ManifoldTheme::Accent : ManifoldTheme::Muted), caret);
                right -= caretWidth + Px(8.0f);
                if (visible)
                    Pill(draw, right, centerY, std::to_string(group.Members.size()).c_str(), ManifoldTheme::Label);
            }
            if (visible)
            {
                if (!member || group.Members.size() == 1)
                {
                    const std::string pid = ToText(entry.Id);
                    const float pidWidth = ImGui::CalcTextSize(pid.c_str()).x;
                    draw->AddText(ImVec2(right - pidWidth, std::round(centerY - line * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Muted), pid.c_str());
                    right -= pidWidth + Px(10.0f);
                }
                if (entry.Id == current)
                    Pill(draw, right, centerY, "opened", ManifoldTheme::Accent);
                else if (!member && !last.empty() && group.Key == last)
                    Pill(draw, right, centerY, "last", ManifoldTheme::Label);
                if (!entry.Is64Bit)
                    Pill(draw, right, centerY, "32-bit", ManifoldTheme::Label);
                if (!entry.Accessible)
                {
                    const float lockWidth = ImGui::CalcTextSize(ICON_FA_LOCK).x;
                    draw->AddText(ImVec2(right - lockWidth, std::round(centerY - line * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Warning), ICON_FA_LOCK);
                    right -= lockWidth + Px(8.0f);
                }

                float x = min.x + Px(12.0f);
                if (member)
                {
                    x += Px(26.0f);
                    draw->AddCircleFilled(ImVec2(x - Px(10.0f), centerY), Px(2.0f), ImGui::GetColorU32(ManifoldTheme::Muted));
                    const std::string pid = "pid " + ToText(entry.Id);
                    PushMono();
                    draw->AddText(ImVec2(x, std::round(centerY - line * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Label), pid.c_str());
                    const float pidWidth = ImGui::CalcTextSize(pid.c_str()).x;
                    PopMono();
                    x += pidWidth + Px(12.0f);
                    const std::string title = entry.WindowTitle.empty() ? std::string("no window") : entry.WindowTitle;
                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                    ImGui::RenderTextEllipsis(draw, ImVec2(x, std::round(centerY - line * 0.5f)), ImVec2(right, std::round(centerY + line * 0.5f)), right, title.c_str(), nullptr, nullptr);
                    ImGui::PopStyleColor();
                }
                else
                {
                    const float icon = std::round(tall ? Px(22.0f) : line);
                    const ImVec2 iconAt(x, std::round(centerY - icon * 0.5f));
                    void* texture = m_icons.Texture(m_workspace.Window(), entry.Path, static_cast<int>(icon), 4);
                    if (texture)
                        draw->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))), iconAt, ImVec2(iconAt.x + icon, iconAt.y + icon));
                    else
                    {
                        const char* glyph = group.App ? ICON_FA_WINDOW_MAXIMIZE : ICON_FA_COG;
                        const ImVec2 glyphSize = ImGui::CalcTextSize(glyph);
                        draw->AddText(ImVec2(std::round(iconAt.x + (icon - glyphSize.x) * 0.5f), std::round(centerY - glyphSize.y * 0.5f)), ImGui::GetColorU32(ManifoldTheme::Muted), glyph);
                    }
                    x += icon + Px(10.0f);
                    const float nameY = tall ? std::round(centerY - line - Px(1.0f)) : std::round(centerY - line * 0.5f);
                    ImGui::PushFont(Fonts::Bold(), 0.0f);
                    ImGui::PushStyleColor(ImGuiCol_Text, !entry.Accessible ? ManifoldTheme::Muted : entry.Id == current ? ManifoldTheme::Accent : ManifoldTheme::Text);
                    ImGui::RenderTextEllipsis(draw, ImVec2(x, nameY), ImVec2(right, nameY + line), right, entry.Name.c_str(), nullptr, nullptr);
                    ImGui::PopStyleColor();
                    ImGui::PopFont();
                    if (tall)
                    {
                        const float titleY = std::round(centerY + Px(1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                        ImGui::RenderTextEllipsis(draw, ImVec2(x, titleY), ImVec2(right, titleY + line), right, entry.WindowTitle.c_str(), nullptr, nullptr);
                        ImGui::PopStyleColor();
                    }
                }
            }

            if (clicked)
            {
                if (caretHit)
                {
                    if (!m_expanded.erase(group.Key))
                        m_expanded.insert(group.Key);
                }
                else
                    select(row);
            }
            if (doubleClicked && !caretHit)
                openNow = entry.Id;
            if (hovered && !caretHit)
            {
                std::string tip = entry.Name + "  ·  pid " + ToText(entry.Id) + "  ·  " + (entry.Is64Bit ? "64-bit" : "32-bit");
                if (!entry.WindowTitle.empty())
                    tip += "\n" + entry.WindowTitle;
                if (!entry.Path.empty())
                    tip += "\n" + WideToUtf8(entry.Path);
                if (!entry.Accessible)
                    tip += "\n\nNeeds administrator rights to open.";
                if (!member && group.Members.size() > 1)
                    tip += "\n" + std::to_string(group.Members.size()) + " processes with this name; the arrow lists them.";
                ItemTooltip(tip.c_str());
            }
            else if (hovered && caretHit)
                ItemTooltip(m_expanded.count(group.Key) ? "Hide the processes of this name" : "Show every process of this name");
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();

        ImGui::BeginDisabled(m_selected == 0);
        if (AccentButton(ICON_FA_PLUG "  Open", Px(110.0f, 0.0f)) || (m_selected && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)))
            openNow = MakeProcessId(m_selected);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        std::size_t appCount = 0;
        for (const ProcessGroup& group : groups)
            appCount += group.App ? 1 : 0;
        Muted(Format("%zu processes  ·  %zu apps", m_list.size(), appCount));
        if (openNow)
            open(*openNow);
        EndToolWindow();
    }

    SymbolsWindow::SymbolsWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void SymbolsWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_TAGS "  Symbols and modules###symbols", ImVec2(640.0f, 520.0f), 0, ImVec2(480.0f, 320.0f)))
            return;
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
                            ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(module.Base)));
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
                            ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(module.Base)));
                            if (ImGui::Selectable(module.Name.c_str(), m_module == module.Name))
                                m_module = module.Name;
                            ImGui::PopID();
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
        if (!BeginWindow(ICON_FA_TH_LIST "  Memory regions###regions", ImVec2(760.0f, 540.0f), 0, ImVec2(560.0f, 300.0f)))
            return;
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
