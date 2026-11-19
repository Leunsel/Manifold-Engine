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

        bool Toggle(const char* icon, const char* tooltip, bool& value)
        {
            if (value)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.22f));
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
            }
            const bool clicked = IconButton(icon, tooltip);
            if (value)
                ImGui::PopStyleColor(2);
            if (clicked)
                value = !value;
            return clicked;
        }
    }

    void LogWindow::Draw()
    {
        if (!BeginWindow(ICON_FA_STREAM "  Log###log", ImVec2(980.0f, 560.0f), 0, ImVec2(720.0f, 300.0f)))
            return;
        if (ImGui::BeginTabBar("##logtabs"))
        {
            if (ImGui::BeginTabItem("Log"))
            {
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
                    ImGui::SetCursorPos(ImVec2(start.x, start.y + top));
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
                Muted(line);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Error codes"))
            {
                ErrorsTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
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
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const float line = ImGui::GetTextLineHeight();
        const float padding = ImGui::GetStyle().CellPadding.y * 2.0f;
        float top = 0.0f;
        for (Row& row : m_rows)
        {
            float height = line;
            if (m_wrap)
            {
                const std::string& text = m_entries[row.First].Text();
                height = std::max(line, font->CalcTextSizeA(size, FLT_MAX, std::max(width, Px(40.0f)), text.data(), text.data() + text.size()).y);
            }
            row.Top = top;
            row.Height = std::ceil(height + padding);
            top += row.Height;
        }
        m_total = top;
    }

    void LogWindow::Toolbar()
    {
        const char* levels[] = { "Debug", "Info", "Success", "Warnings", "Errors" };
        ImGui::SetNextItemWidth(Px(104.0f));
        ImGui::Combo("##level", &m_level, levels, static_cast<int>(std::size(levels)));
        ItemTooltip("The lowest level that is shown. Debug lines are only recorded while the bug button is on.");
        ImGui::SameLine();
        AreaFilter();
        ImGui::SameLine();
        const ImGuiStyle& style = ImGui::GetStyle();
        const float iconButton = ImGui::GetFrameHeight();
        const float counters = (m_warnings ? ImGui::CalcTextSize(Format(ICON_FA_EXCLAMATION_TRIANGLE " %d", m_warnings).c_str()).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f) +
                               (m_errors ? ImGui::CalcTextSize(Format(ICON_FA_TIMES_CIRCLE " %d", m_errors).c_str()).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x : 0.0f);
        const float right = iconButton * 8.0f + style.ItemSpacing.x * 9.0f + counters;
        if (m_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSearch = false;
        }
        ImGui::SetNextItemWidth(std::max(Px(110.0f), ImGui::GetContentRegionAvail().x - right));
        InputTextWithHint("##search", ICON_FA_SEARCH "  Search messages, areas and sources", m_search);
        const GhostButtons ghost;
        if (m_warnings)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Warning);
            if (ImGui::Button(Format(ICON_FA_EXCLAMATION_TRIANGLE " %d##warnings", m_warnings).c_str()))
                m_level = m_level == kWarningLevel ? 1 : kWarningLevel;
            ImGui::PopStyleColor();
            ItemTooltip("Warnings in the log. Click to show warnings and errors only, click again for everything.");
        }
        if (m_errors)
        {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Error);
            if (ImGui::Button(Format(ICON_FA_TIMES_CIRCLE " %d##errors", m_errors).c_str()))
                m_level = m_level == kErrorLevel ? 1 : kErrorLevel;
            ImGui::PopStyleColor();
            ItemTooltip("Errors in the log. Click to show errors only, click again for everything.");
        }
        ImGui::SameLine();
        Toggle(ICON_FA_ARROW_DOWN "##follow", "Follow: scroll along with new entries while the list is at its end", m_follow);
        ImGui::SameLine();
        if (Toggle(ICON_FA_ALIGN_LEFT "##wrap", "Wrap long and multi-line messages in the list", m_wrap))
            m_layoutWidth = -1.0f;
        ImGui::SameLine();
        if (Toggle(ICON_FA_LAYER_GROUP "##group", "Show repeats of the same message in a row as one entry with a count", m_group))
            m_dirty = true;
        ImGui::SameLine();
        Toggle(ICON_FA_COLUMNS "##details", "Details below the list: the whole message, time to the millisecond, thread and source", m_details);
        ImGui::SameLine();
        bool verbose = RuntimeLog::Verbose();
        if (Toggle(ICON_FA_BUG "##debug", "Record debug messages too (saved with the settings)", verbose))
        {
            RuntimeLog::SetVerbose(verbose);
            m_workspace.Core().Options().VerboseLog = verbose;
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_COPY "##copyshown", "Copy the shown entries as text"))
        {
            std::vector<std::size_t> rows(m_rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i)
                rows[i] = i;
            Copy(rows);
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_ERASER "##clear", "Clear the log (the log file keeps everything)"))
        {
            RuntimeLog::Clear();
            m_selected.clear();
            m_current = 0;
        }
        ImGui::SameLine();
        if (IconButton(ICON_FA_FOLDER_OPEN "##file", "Open the folder with the log file"))
        {
            const std::wstring folder = RuntimeLog::FilePath().parent_path().wstring();
            ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void LogWindow::AreaFilter()
    {
        const std::string label = m_hiddenAreas.empty() ? std::string(ICON_FA_FILTER "  Areas") : Format(ICON_FA_FILTER "  Areas (%zu hidden)", m_hiddenAreas.size());
        if (ImGui::Button((label + "##areas").c_str()))
            ImGui::OpenPopup("##areafilter");
        ItemTooltip("Which areas the list shows");
        if (!ImGui::BeginPopup("##areafilter"))
            return;
        {
            const GhostButtons ghost;
            if (ImGui::SmallButton("Show all"))
            {
                m_hiddenAreas.clear();
                m_dirty = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Hide all"))
            {
                for (const auto& [area, count] : m_areas)
                    m_hiddenAreas.insert(area);
                m_dirty = true;
            }
        }
        ImGui::Separator();
        for (const auto& [area, count] : m_areas)
        {
            bool shown = !m_hiddenAreas.count(area);
            if (ImGui::Checkbox((area + "##area").c_str(), &shown))
            {
                if (shown)
                    m_hiddenAreas.erase(area);
                else
                    m_hiddenAreas.insert(area);
                m_dirty = true;
            }
            ImGui::SameLine();
            Muted(std::to_string(count));
        }
        if (m_areas.empty())
            Muted("Nothing logged yet");
        ImGui::EndPopup();
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
            const std::string& text = entry.Text();
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (m_wrap)
            {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(text.data(), text.data() + text.size());
                ImGui::PopTextWrapPos();
            }
            else
            {
                int more = 0;
                const std::string_view line = FirstLine(text, more);
                ImGui::TextUnformatted(line.data(), line.data() + line.size());
                if (more > 0)
                {
                    ImGui::SameLine();
                    Muted(Format("+%d %s", more, more == 1 ? "line" : "lines"));
                }
            }
            ImGui::PopStyleColor();
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
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the message"))
            ImGui::SetClipboardText(entry.Text().c_str());
        if (ImGui::MenuItem(rows.size() > 1 ? ICON_FA_COPY "  Copy the selected entries" : ICON_FA_COPY "  Copy the entry", ShortcutText(Command::LogCopy)))
            Copy(rows.empty() ? std::vector<std::size_t>{ index } : rows);
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the source", entry.Trace.c_str()))
            ImGui::SetClipboardText(entry.Trace.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem((ICON_FA_FILTER "  Show only " + entry.Area).c_str()))
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
        if (!m_hiddenAreas.empty() && ImGui::MenuItem(ICON_FA_EYE "  Show all areas"))
        {
            m_hiddenAreas.clear();
            m_dirty = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CHECK_DOUBLE "  Select all", ShortcutText(Command::LogSelectAll)))
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
        PushMono();
        TextFitted(entry.Trace, ImGui::GetContentRegionAvail().x - Px(250.0f), ManifoldTheme::Label);
        PopMono();
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
        PushMono();
        InputMultiline("##logtext", m_detailText, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
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
