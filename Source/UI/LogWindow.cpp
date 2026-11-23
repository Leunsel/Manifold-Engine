#include "UI/LogWindow.h"

#include "App/Engine.h"
#include "Core/Errors.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Shortcuts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <map>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr int kWarningLevel = static_cast<int>(LogLevel::Warning);
        constexpr int kErrorLevel = static_cast<int>(LogLevel::Error);
        constexpr const char* kLevels[] = { "Debug", "Info", "Success", "Warnings", "Errors" };

        bool SameText(const LogEntry& a, const LogEntry& b)
        {
            return a.Level == b.Level && a.Area == b.Area && a.Trace == b.Trace && a.Text() == b.Text();
        }

        std::string Clock(const LogEntry& entry)
        {
            return entry.Time + Format(".%03d", entry.Milliseconds);
        }

        std::string Duration(double seconds)
        {
            if (seconds < 0.001)
                return Format("%.0f µs", seconds * 1000000.0);
            if (seconds < 1.0)
                return Format("%.1f ms", seconds * 1000.0);
            if (seconds < 120.0)
                return Format("%.2f s", seconds);
            return Format("%.1f min", seconds / 60.0);
        }

        std::string_view FirstLine(std::string_view text, int& more)
        {
            more = 0;
            const std::size_t cut = text.find('\n');
            if (cut == std::string_view::npos)
                return text;
            more = static_cast<int>(std::count(text.begin() + static_cast<std::ptrdiff_t>(cut), text.end(), '\n'));
            if (!text.empty() && text.back() == '\n')
                --more;
            std::string_view line = text.substr(0, cut);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            return line;
        }

        // ∑ An entry with details of its own (the Auto Assembler's step table) shows only its
        //   summary in the list; the details belong in the details pane. Shown in the list as well,
        //   the table came out twice, and in the list's proportional font its columns went astray.
        const std::string& ListText(const LogEntry& entry)
        {
            return entry.Detailed ? entry.Message : entry.Text();
        }

        // Text of several lines is a table, a trace or code: the fixed-width font keeps it in line,
        // in the list as in the details pane.
        ImFont* ListFont(std::string_view text)
        {
            return text.find('\n') != std::string_view::npos ? Fonts::Mono() : ImGui::GetFont();
        }
    }

    void LogWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_STREAM "  Log###log", ImVec2(980.0f, 560.0f), ImGuiWindowFlags_MenuBar, ImVec2(720.0f, 300.0f)))
            return;
        Refresh();
        MenuBar();
        bool logTab = false;
        if (ImGui::BeginTabBar("##logtabs"))
        {
            if (ImGui::BeginTabItem("Log"))
            {
                logTab = true;
                const bool find = Pressed(Command::LogFind);
                const bool copy = Pressed(Command::LogCopy);
                const bool all = Pressed(Command::LogSelectAll);
                const bool typing = ImGui::GetIO().WantTextInput;
                if (find)
                    m_focusSearch = true;
                Refresh();
                if (copy && !typing)
                    Copy(SelectedRows());
                if (all && !typing)
                    SelectAll();
                Toolbar();
                Refresh();

                const float gap = Px(6.0f);
                const float status = ImGui::GetTextLineHeightWithSpacing();
                const float available = ImGui::GetContentRegionAvail().y - status;
                if (m_details)
                {
                    const float minTable = Px(90.0f);
                    const float minDetails = Px(90.0f);
                    m_detailHeight = std::clamp(m_detailHeight, minDetails, std::max(minDetails, available - minTable - gap));
                    float top = available - m_detailHeight - gap;
                    float bottom = m_detailHeight;
                    const ImVec2 start = ImGui::GetCursorPos();
                    Table(top);
                    ImGui::SetCursorPos(start);
                    if (Splitter("##logsplit", false, gap, &top, &bottom, minTable, minDetails))
                        m_detailHeight = bottom;
                    ImGui::SetCursorPos(ImVec2(start.x, start.y + top + gap));
                    ImGui::BeginChild("##logdetails", ImVec2(0.0f, m_detailHeight), ImGuiChildFlags_Borders);
                    Details();
                    ImGui::EndChild();
                }
                else
                {
                    Table(available);
                }
                const std::size_t selected = m_selected.size();
                std::string line = Format("%zu of %zu entries", m_rows.size(), m_entries.size());
                if (m_warnings)
                    line += Format("  ·  %d %s", m_warnings, m_warnings == 1 ? "warning" : "warnings");
                if (m_errors)
                    line += Format("  ·  %d %s", m_errors, m_errors == 1 ? "error" : "errors");
                if (selected > 1)
                    line += Format("  ·  %zu selected (%s copies them)", selected, ShortcutText(Command::LogCopy) ? ShortcutText(Command::LogCopy) : "Copy");
                if (!m_hiddenAreas.empty())
                    line += Format("  ·  %zu %s hidden", m_hiddenAreas.size(), m_hiddenAreas.size() == 1 ? "area" : "areas");
                if (m_level != 1)
                    line += m_level == kErrorLevel ? std::string("  ·  errors only") : Format("  ·  %s and up", kLevels[m_level]);
                Muted(line);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Error Codes"))
            {
                ErrorsTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        m_logTab = logTab;
        EndToolWindow();
    }

    void LogWindow::Refresh()
    {
        if (m_generation != RuntimeLog::Generation())
        {
            m_generation = RuntimeLog::Generation();
            m_entries = RuntimeLog::Entries();
            m_dirty = true;
        }
        if (m_search != m_appliedSearch || m_level != m_appliedLevel)
            m_dirty = true;
        if (m_dirty)
            Rebuild();
    }

    void LogWindow::Rebuild()
    {
        m_dirty = false;
        m_appliedSearch = m_search;
        m_appliedLevel = m_level;
        m_rows.clear();
        m_warnings = 0;
        m_errors = 0;
        std::map<std::string, int> areas;
        const std::string search = Trim(m_search);
        for (std::size_t i = 0; i < m_entries.size(); ++i)
        {
            const LogEntry& entry = m_entries[i];
            if (entry.Level == LogLevel::Warning)
                ++m_warnings;
            else if (entry.Level == LogLevel::Error)
                ++m_errors;
            ++areas[entry.Area];
            if (m_hiddenAreas.count(entry.Area) || !RuntimeLog::EntryMatches(entry, static_cast<LogLevel>(m_level), search))
                continue;
            if (m_group && !m_rows.empty() && SameText(m_entries[m_rows.back().Last], entry))
            {
                m_rows.back().Last = i;
                ++m_rows.back().Repeat;
                continue;
            }
            m_rows.push_back({ i, i, 1, 0.0f, 0.0f });
        }
        m_areas.assign(areas.begin(), areas.end());
        m_layoutWidth = -1.0f;
    }

    void LogWindow::Layout(float width)
    {
        m_layoutWidth = width;
        m_layoutWrap = m_wrap;
        const float size = ImGui::GetFontSize();
        const float line = ImGui::GetTextLineHeight();
        const float padding = ImGui::GetStyle().CellPadding.y * 2.0f;
        float top = 0.0f;
        for (Row& row : m_rows)
        {
            float height = line;
            if (m_wrap)
            {
                const std::string& text = ListText(m_entries[row.First]);
                height = std::max(line, ListFont(text)->CalcTextSizeA(size, FLT_MAX, std::max(width, Px(40.0f)), text.data(), text.data() + text.size()).y);
            }
            row.Top = top;
            row.Height = std::ceil(height + padding);
            top += row.Height;
        }
        m_total = top;
    }

    void LogWindow::MenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;
        if (ImGui::BeginMenu("Log"))
        {
            const std::vector<std::size_t> selected = SelectedRows();
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Selected Entries", ShortcutText(Command::LogCopy), false, m_logTab && !selected.empty()))
                Copy(selected);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Shown Entries", nullptr, false, !m_rows.empty()))
            {
                std::vector<std::size_t> rows(m_rows.size());
                for (std::size_t i = 0; i < rows.size(); ++i)
                    rows[i] = i;
                Copy(rows);
            }
            ItemTooltip("Every entry the list shows with the current filters, as text.");
            if (CommandItem(ICON_FA_CHECK_DOUBLE "  Select All", Command::LogSelectAll, false, m_logTab && !m_rows.empty()))
                SelectAll();
            if (CommandItem(ICON_FA_SEARCH "  Find", Command::LogFind, false, m_logTab))
                m_focusSearch = true;
            ImGui::Separator();
            bool verbose = RuntimeLog::Verbose();
            if (ImGui::MenuItem(ICON_FA_BUG "  Record Debug Messages", nullptr, &verbose))
            {
                RuntimeLog::SetVerbose(verbose);
                m_workspace.Core().Options().VerboseLog = verbose;
            }
            ItemTooltip("Also record the debug level. Saved with the settings.");
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open the Log Folder"))
            {
                const std::wstring folder = RuntimeLog::FilePath().parent_path().wstring();
                ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::Separator();
            if (DangerMenuItem(ICON_FA_ERASER "  Clear the Log", nullptr, !m_entries.empty()))
            {
                RuntimeLog::Clear();
                m_selected.clear();
                m_current = 0;
            }
            ItemTooltip("Empties the list. The log file keeps everything.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View", m_logTab))
        {
            if (ImGui::BeginMenu(ICON_FA_SIGNAL "  Lowest Level"))
            {
                for (int i = 0; i < static_cast<int>(std::size(kLevels)); ++i)
                {
                    if (ImGui::MenuItem(kLevels[i], nullptr, m_level == i))
                        m_level = i;
                }
                if (!RuntimeLog::Verbose())
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("Debug messages are only recorded\nwith Log > Record Debug Messages.");
                }
                ImGui::EndMenu();
            }
            const std::string areas = m_hiddenAreas.empty() ? std::string(ICON_FA_FILTER "  Areas") : Format(ICON_FA_FILTER "  Areas (%zu hidden)", m_hiddenAreas.size());
            if (ImGui::BeginMenu((areas + "###areas").c_str()))
            {
                AreaMenu();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Follow New Entries", nullptr, &m_follow);
            ItemTooltip("Scroll along with new entries while the list is at its end.");
            if (ImGui::MenuItem(ICON_FA_ALIGN_LEFT "  Wrap Long Messages", nullptr, &m_wrap))
                m_layoutWidth = -1.0f;
            ItemTooltip("Wrap long and multi-line messages in the list. Off shows their first line only.");
            if (ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Group Repeats", nullptr, &m_group))
                m_dirty = true;
            ItemTooltip("Show repeats of the same message in a row as one entry with a count.");
            ImGui::MenuItem(ICON_FA_COLUMNS "  Details Pane", nullptr, &m_details);
            ItemTooltip("Details below the list: the whole message, time to the millisecond, thread and source.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void LogWindow::Toolbar()
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float counters = (m_warnings ? ImGui::CalcTextSize(Format(ICON_FA_EXCLAMATION_TRIANGLE " %d", m_warnings).c_str()).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f) +
                               (m_errors ? ImGui::CalcTextSize(Format(ICON_FA_TIMES_CIRCLE " %d", m_errors).c_str()).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f);
        if (m_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSearch = false;
        }
        ImGui::SetNextItemWidth(std::max(Px(110.0f), ImGui::GetContentRegionAvail().x - counters));
        InputTextWithHint("##search", ICON_FA_SEARCH "  Search messages, areas and sources", m_search);
        const GhostButtons ghost;
        const auto counter = [&](const std::string& label, const ImVec4& color, int level, const char* tooltip)
        {
            ImGui::SameLine();
            const bool active = m_level == level;
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(color, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Button(label.c_str()))
                m_level = active ? 1 : level;
            ImGui::PopStyleColor(active ? 2 : 1);
            ItemTooltip(tooltip);
        };
        if (m_warnings)
            counter(Format(ICON_FA_EXCLAMATION_TRIANGLE " %d##warnings", m_warnings), ManifoldTheme::Warning, kWarningLevel,
                    "Warnings in the log. Click to show warnings and errors only, click again for everything.");
        if (m_errors)
            counter(Format(ICON_FA_TIMES_CIRCLE " %d##errors", m_errors), ManifoldTheme::Error, kErrorLevel, "Errors in the log. Click to show errors only, click again for everything.");
    }

    void LogWindow::AreaMenu()
    {
        if (ImGui::MenuItem(ICON_FA_EYE "  Show All", nullptr, false, !m_hiddenAreas.empty()))
        {
            m_hiddenAreas.clear();
            m_dirty = true;
        }
        if (ImGui::MenuItem(ICON_FA_EYE_SLASH "  Hide All", nullptr, false, !m_areas.empty()))
        {
            for (const auto& [area, count] : m_areas)
                m_hiddenAreas.insert(area);
            m_dirty = true;
        }
        ImGui::Separator();
        // Picking several areas in a row is the common case, so the menu stays open.
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
        for (const auto& [area, count] : m_areas)
        {
            const bool shown = !m_hiddenAreas.count(area);
            if (ImGui::MenuItem((area + "##area").c_str(), std::to_string(count).c_str(), shown))
            {
                if (shown)
                    m_hiddenAreas.insert(area);
                else
                    m_hiddenAreas.erase(area);
                m_dirty = true;
            }
        }
        ImGui::PopItemFlag();
        if (m_areas.empty())
            ImGui::TextDisabled("Nothing logged yet");
    }

    void LogWindow::Table(float height)
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##entries", 4, flags, ImVec2(0.0f, height)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, Px(88.0f));
        ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthFixed, Px(128.0f));
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 4.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();

        const float width = m_messageWidth > 0.0f ? m_messageWidth : ImGui::GetContentRegionAvail().x * 0.6f;
        if (m_layoutWidth < 0.0f || m_layoutWrap != m_wrap || (m_wrap && std::abs(width - m_layoutWidth) > 0.5f))
            Layout(width);
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - Px(4.0f);
        const float header = ImGui::TableGetHeaderRowHeight();
        const float view = ImGui::GetWindowHeight();
        const float from = ImGui::GetScrollY() - header;
        const float to = from + view;
        const auto first = std::upper_bound(m_rows.begin(), m_rows.end(), from, [](float value, const Row& row) { return value < row.Top + row.Height; });
        const auto last = std::lower_bound(first, m_rows.end(), to, [](const Row& row, float value) { return row.Top < value; });
        const std::size_t begin = static_cast<std::size_t>(first - m_rows.begin());
        const std::size_t end = static_cast<std::size_t>(last - m_rows.begin());
        if (begin > 0 && begin <= m_rows.size())
            ImGui::TableNextRow(ImGuiTableRowFlags_None, m_rows[std::min(begin, m_rows.size() - 1)].Top);
        const float padding = ImGui::GetStyle().CellPadding.y * 2.0f;
        for (std::size_t index = begin; index < end; ++index)
        {
            const Row& row = m_rows[index];
            const LogEntry& entry = m_entries[row.First];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row.Height);
            ImGui::PushID(static_cast<int>(entry.Serial));
            ImGui::TableNextColumn();
            const bool selected = m_selected.count(entry.Serial) > 0;
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            const bool clicked = ImGui::Selectable((Clock(entry) + "##row").c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                ImVec2(0.0f, row.Height - padding));
            ImGui::PopStyleColor();
            const bool hovered = ImGui::IsItemHovered();
            if (clicked)
            {
                const ImGuiIO& io = ImGui::GetIO();
                Select(index, io.KeyCtrl, io.KeyShift);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
                Select(index, false, false);
            if (ImGui::BeginPopupContextItem("##rowmenu"))
            {
                RowMenu(index);
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.Level));
            ImGui::TextUnformatted((std::string(LevelIcon(entry.Level)) + " " + entry.Area).c_str());
            ImGui::PopStyleColor();
            if (row.Repeat > 1)
            {
                ImGui::SameLine();
                ImGui::TextColored(ManifoldTheme::Accent, "x%d", row.Repeat);
            }

            ImGui::TableNextColumn();
            m_messageWidth = ImGui::GetContentRegionAvail().x;
            const ImVec4 color = entry.Level == LogLevel::Error ? ManifoldTheme::Error : entry.Level == LogLevel::Warning ? ManifoldTheme::Warning : ManifoldTheme::Text;
            const std::string& text = ListText(entry);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            int more = 0;
            if (m_wrap)
            {
                ImGui::PushFont(ListFont(text), 0.0f);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
                ImGui::PopTextWrapPos();
                ImGui::PopFont();
            }
            else
            {
                const std::string_view line = FirstLine(text, more);
                ImGui::TextUnformatted(line.data(), line.data() + line.size());
            }
            ImGui::PopStyleColor();
            if (more > 0)
            {
                ImGui::SameLine();
                Muted(Format("+%d %s", more, more == 1 ? "line" : "lines"));
            }
            if (!ErrorTooltip(entry.Message) && hovered && !m_wrap && (ImGui::CalcTextSize(text.c_str()).x > m_messageWidth || text.find('\n') != std::string::npos))
            {
                ShowTooltip([&]()
                    {
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                        ImGui::TextUnformatted(text.data(), text.data() + text.size());
                        ImGui::PopTextWrapPos();
                    });
            }

            ImGui::TableNextColumn();
            Muted(entry.Trace);
            ImGui::PopID();
        }
        if (end < m_rows.size())
            ImGui::TableNextRow(ImGuiTableRowFlags_None, m_total - m_rows[end].Top);
        if (m_follow && atEnd)
            ImGui::SetScrollY(1.0e9f);
        ImGui::EndTable();
    }

    void LogWindow::RowMenu(std::size_t index)
    {
        const Row& row = m_rows[index];
        const LogEntry& entry = m_entries[row.First];
        const std::vector<std::size_t> rows = SelectedRows();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Message"))
            ImGui::SetClipboardText(entry.Text().c_str());
        if (ImGui::MenuItem(rows.size() > 1 ? ICON_FA_COPY "  Copy the Selected Entries" : ICON_FA_COPY "  Copy the Entry", ShortcutText(Command::LogCopy)))
            Copy(rows.empty() ? std::vector<std::size_t>{ index } : rows);
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Source", entry.Trace.c_str()))
            ImGui::SetClipboardText(entry.Trace.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem((ICON_FA_FILTER "  Show Only " + entry.Area).c_str()))
        {
            m_hiddenAreas.clear();
            for (const auto& [area, count] : m_areas)
            {
                if (area != entry.Area)
                    m_hiddenAreas.insert(area);
            }
            m_dirty = true;
        }
        if (ImGui::MenuItem((ICON_FA_EYE_SLASH "  Hide " + entry.Area).c_str()))
        {
            m_hiddenAreas.insert(entry.Area);
            m_dirty = true;
        }
        if (!m_hiddenAreas.empty() && ImGui::MenuItem(ICON_FA_EYE "  Show All Areas"))
        {
            m_hiddenAreas.clear();
            m_dirty = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CHECK_DOUBLE "  Select All", ShortcutText(Command::LogSelectAll)))
            SelectAll();
    }

    void LogWindow::Select(std::size_t index, bool toggle, bool range)
    {
        const std::uint64_t serial = m_entries[m_rows[index].First].Serial;
        if (range && m_anchor)
        {
            std::size_t from = index;
            for (std::size_t i = 0; i < m_rows.size(); ++i)
            {
                if (m_entries[m_rows[i].First].Serial == m_anchor)
                    from = i;
            }
            if (!toggle)
                m_selected.clear();
            for (std::size_t i = std::min(from, index); i <= std::max(from, index); ++i)
                m_selected.insert(m_entries[m_rows[i].First].Serial);
        }
        else if (toggle)
        {
            if (!m_selected.erase(serial))
                m_selected.insert(serial);
            m_anchor = serial;
        }
        else
        {
            m_selected = { serial };
            m_anchor = serial;
        }
        m_current = serial;
    }

    void LogWindow::SelectAll()
    {
        m_selected.clear();
        for (const Row& row : m_rows)
            m_selected.insert(m_entries[row.First].Serial);
    }

    std::vector<std::size_t> LogWindow::SelectedRows() const
    {
        std::vector<std::size_t> rows;
        for (std::size_t i = 0; i < m_rows.size(); ++i)
        {
            if (m_selected.count(m_entries[m_rows[i].First].Serial))
                rows.push_back(i);
        }
        return rows;
    }

    std::string LogWindow::LineText(const Row& row) const
    {
        const LogEntry& entry = m_entries[row.First];
        std::string text = "[" + Clock(entry) + "] [" + RuntimeLog::LevelName(entry.Level) + "] [" + entry.Area + "] [" + entry.Trace + "] " + entry.Text();
        if (row.Repeat > 1)
            text += Format("  (x%d, last at %s)", row.Repeat, Clock(m_entries[row.Last]).c_str());
        return text;
    }

    void LogWindow::Copy(const std::vector<std::size_t>& rows) const
    {
        if (rows.empty())
            return;
        std::string text;
        for (const std::size_t row : rows)
            text += LineText(m_rows[row]) + "\n";
        ImGui::SetClipboardText(text.c_str());
        m_workspace.Core().Notify(LogLevel::Info, Format("Copied %zu log %s", rows.size(), rows.size() == 1 ? "entry" : "entries"));
    }

    const LogWindow::Row* LogWindow::CurrentRow() const
    {
        for (const Row& row : m_rows)
        {
            if (m_entries[row.First].Serial == m_current)
                return &row;
        }
        return nullptr;
    }

    void LogWindow::Details()
    {
        const Row* row = CurrentRow();
        if (!row)
        {
            ImGui::PushTextWrapPos(0.0f);
            Muted("Pick an entry to see all of it here: the whole message with every line, the time to the millisecond, how long after the entry before it, the thread, the source, "
                  "and what an ME- error code means. Ctrl or Shift picks several to copy.");
            ImGui::PopTextWrapPos();
            return;
        }
        const LogEntry& entry = m_entries[row->First];
        if (m_detailSerial != entry.Serial)
        {
            m_detailSerial = entry.Serial;
            m_detailText = entry.Text();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(entry.Level));
        ImGui::TextUnformatted((std::string(LevelIcon(entry.Level)) + "  " + RuntimeLog::LevelName(entry.Level) + "  " + entry.Area).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::string facts = Clock(entry);
        if (row->First > 0)
            facts += "  ·  " + Duration(entry.Clock - m_entries[row->First - 1].Clock) + " after the entry before";
        facts += "  ·  " + (entry.MainThread ? std::string("main thread") : Format("thread %u", entry.Thread));
        if (row->Repeat > 1)
            facts += Format("  ·  %d times in a row, the last at %s", row->Repeat, Clock(m_entries[row->Last]).c_str());
        Muted(facts);
        ImGui::AlignTextToFramePadding();
        TextFitted(entry.Trace, ImGui::GetContentRegionAvail().x - Px(250.0f), ManifoldTheme::Label);
        {
            const GhostButtons ghost;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - Px(246.0f)));
            if (ImGui::SmallButton(ICON_FA_COPY "  Message"))
                ImGui::SetClipboardText(entry.Text().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_COPY "  Entry"))
                ImGui::SetClipboardText(LineText(*row).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_COPY "  Source"))
                ImGui::SetClipboardText(entry.Trace.c_str());
        }
        if (const ErrorInfo* info = ErrorOf(entry.Text()))
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ManifoldTheme::Accent, "%s  %s", ErrorName(info->Code).c_str(), std::string(ErrorArea(info->Code)).c_str());
            ImGui::SameLine();
            Muted(std::string(info->Cause));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            ImGui::TextUnformatted(info->Help.data(), info->Help.data() + info->Help.size());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        // ∑ details of several lines are tables, traces or code (the Auto Assembler's step table): a
        //   fixed-width font keeps their columns in line
        const bool table = m_detailText.find('\n') != std::string::npos;
        if (table)
            PushMono();
        InputMultiline("##logtext", m_detailText, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        if (table)
            PopMono();
    }

    void LogWindow::ErrorsTab()
    {
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##codes", 4, flags))
            return;
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
}
