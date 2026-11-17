#include "UI/ConsoleView.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Text/Highlight.h"
#include "UI/EditorShared.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Manifold::UI
{
    namespace
    {
        constexpr std::uint32_t kTracebackKey = 1;
        constexpr std::uint32_t kValuesKey = 3;
        constexpr std::uint8_t kFieldsValue = 255;
        constexpr std::uint32_t kMoreFlag = 0x80000000u;

        struct Display
        {
            std::string Text;
            std::size_t CodeEnd = 0;
            std::size_t ChannelEnd = 0;
            std::size_t MessageEnd = 0;
        };

        Display DisplayOf(const Lua::ConsoleEntry& entry)
        {
            Display display;
            if (!entry.Code.empty())
                display.Text += entry.Code + " ";
            display.CodeEnd = display.Text.size();
            if (!entry.Channel.empty())
                display.Text += "[" + entry.Channel + "] ";
            display.ChannelEnd = display.Text.size();
            display.Text += entry.Text;
            display.MessageEnd = display.Text.size();
            if (!entry.FieldsText.empty())
                display.Text += "  " + entry.FieldsText;
            return display;
        }

        std::string SourceLabel(const Lua::ConsoleEntry& entry)
        {
            std::string label;
            if (!entry.Source.Chunk.empty() && entry.Kind != Lua::EntryKind::Input && entry.Kind != Lua::EntryKind::Result)
                label = entry.Source.Chunk + (entry.Source.Line > 0 ? ":" + std::to_string(entry.Source.Line) : std::string());
            if (entry.Kind == Lua::EntryKind::Result && entry.Elapsed >= 0.0)
            {
                char text[32] = {};
                std::snprintf(text, sizeof(text), entry.Elapsed < 1.0 ? "%.1f ms" : "%.2f s", entry.Elapsed < 1.0 ? entry.Elapsed * 1000.0 : entry.Elapsed);
                label = text;
            }
            if (entry.Repeat > 1)
                label = "\xC3\x97" + std::to_string(entry.Repeat) + (label.empty() ? std::string() : "  " + label);
            return label;
        }

        std::uint32_t Columns(std::string_view text)
        {
            std::uint32_t columns = 0;
            for (const char c : text)
            {
                if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
                    ++columns;
            }
            return columns;
        }

        std::size_t ByteAfter(std::string_view text, std::size_t from, std::uint32_t columns)
        {
            std::size_t at = from;
            while (at < text.size() && columns > 0)
            {
                ++at;
                while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
                    ++at;
                --columns;
            }
            return at;
        }

        std::uint32_t NodeKey(std::uint8_t value, std::uint32_t node)
        {
            return (2u << 24) | (static_cast<std::uint32_t>(value) << 16) | (node & 0xFFFF);
        }

        ImVec4 EntryColor(const Lua::ConsoleEntry& entry)
        {
            switch (entry.Level)
            {
            case LogLevel::Error: return ManifoldTheme::Error;
            case LogLevel::Warning: return ManifoldTheme::Warning;
            case LogLevel::Success: return ManifoldTheme::Accent;
            case LogLevel::Debug: return ManifoldTheme::Muted;
            case LogLevel::Info: break;
            }
            return ManifoldTheme::Text;
        }

        ImVec4 NodeColor(const Lua::SnapshotNode& node)
        {
            if (node.IsAddress)
                return ManifoldTheme::AddressText;
            if (node.Type == "number")
                return ManifoldTheme::CodeNumber;
            if (node.Type == "string")
                return ManifoldTheme::CodeString;
            if (node.Type == "boolean")
                return ManifoldTheme::CodeKeyword;
            if (node.Type == "function")
                return ManifoldTheme::CodeRegister;
            if (node.Type == "nil")
                return ManifoldTheme::Muted;
            if (node.Type == "table" || node.Type == "userdata")
                return ManifoldTheme::Label;
            return ManifoldTheme::Text;
        }

        void Segment(ImDrawList* draw, float& x, float y, std::string_view text, ImU32 color)
        {
            if (text.empty())
                return;
            draw->AddText(ImVec2(x, y), color, text.data(), text.data() + text.size());
            x += ImGui::CalcTextSize(text.data(), text.data() + text.size()).x;
        }
    }

    bool ConsoleView::Expanded(std::uint64_t id, std::uint32_t key) const
    {
        return m_expanded.count({ id, key }) > 0;
    }

    void ConsoleView::Toggle(std::uint64_t id, std::uint32_t key)
    {
        const auto at = m_expanded.find({ id, key });
        if (at == m_expanded.end())
            m_expanded.insert({ id, key });
        else
            m_expanded.erase(at);
        m_built = false;
    }

    bool ConsoleView::Passes(const Lua::ConsoleEntry& entry, const ConsoleFilter& filter) const
    {
        if (!filter.Levels[static_cast<std::size_t>(entry.Level)])
            return false;
        if (!filter.Channel.empty() && entry.Channel != filter.Channel && entry.Channel.rfind(filter.Channel + "/", 0) != 0)
            return false;
        if (!filter.Search.empty())
        {
            const auto has = [&](const std::string& text) { return ContainsNoCase(text, filter.Search); };
            if (!has(entry.Text) && !has(entry.FieldsText) && !has(entry.Channel) && !has(entry.Code) && !has(entry.Source.Chunk) && !has(entry.Traceback))
                return false;
        }
        return true;
    }

    float ConsoleView::MessageOffset(bool time) const
    {
        return std::round(Px(4.0f) + m_glyph * (3.0f + (time ? 9.0f : 0.0f)));
    }

    const Lua::ValueSnapshot* ConsoleView::ValueOf(const Lua::ConsoleEntry& entry, std::uint8_t value) const
    {
        if (value == kFieldsValue)
            return entry.Fields.get();
        return value < entry.Values.size() ? entry.Values[value].get() : nullptr;
    }

    void ConsoleView::WrapText(std::uint64_t id, RowKind kind, std::uint8_t value, std::string_view text, std::uint32_t columns, bool head)
    {
        columns = std::max<std::uint32_t>(columns, 8);
        std::size_t start = 0;
        bool first = true;
        while (start <= text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
                end = text.size();
            std::size_t at = start;
            do
            {
                const std::size_t stop = std::min(end, ByteAfter(text, at, columns));
                Row row;
                row.Id = id;
                row.Kind = first && head ? RowKind::Head : kind;
                row.Value = value;
                row.Start = static_cast<std::uint32_t>(at);
                row.End = static_cast<std::uint32_t>(stop);
                m_rows.push_back(row);
                first = false;
                at = stop;
            } while (at < end);
            if (end >= text.size())
                break;
            start = end + 1;
        }
    }

    void ConsoleView::AddTree(std::uint64_t id, std::uint8_t value, const Lua::ValueSnapshot& snapshot, bool includeRoot)
    {
        const auto& nodes = snapshot.Nodes;
        std::size_t i = includeRoot ? 0 : 1;
        while (i < nodes.size())
        {
            const Lua::SnapshotNode& node = nodes[i];
            Row row;
            row.Id = id;
            row.Kind = RowKind::Node;
            row.Value = value;
            row.Start = static_cast<std::uint32_t>(i);
            m_rows.push_back(row);
            const bool open = node.Subtree > 0 && Expanded(id, NodeKey(value, static_cast<std::uint32_t>(i)));
            if (node.Subtree > 0 && !open)
            {
                i += node.Subtree + 1;
                continue;
            }
            if (open && node.Total > node.Shown)
            {
                Row more = row;
                more.Start = static_cast<std::uint32_t>(i) | kMoreFlag;
                const std::size_t after = i + node.Subtree + 1;
                ++i;
                while (i < after && i < nodes.size())
                {
                    const Lua::SnapshotNode& child = nodes[i];
                    Row childRow = row;
                    childRow.Start = static_cast<std::uint32_t>(i);
                    m_rows.push_back(childRow);
                    const bool childOpen = child.Subtree > 0 && Expanded(id, NodeKey(value, static_cast<std::uint32_t>(i)));
                    i += childOpen || child.Subtree == 0 ? 1 : child.Subtree + 1;
                }
                m_rows.push_back(more);
                continue;
            }
            ++i;
        }
    }

    void ConsoleView::Append(const Lua::ConsoleEntry& entry, const ConsoleFilter& filter)
    {
        if (!Passes(entry, filter))
            return;
        switch (entry.Kind)
        {
        case Lua::EntryKind::Input:
        {
            std::size_t start = 0;
            bool first = true;
            while (start <= entry.Text.size())
            {
                std::size_t end = entry.Text.find('\n', start);
                if (end == std::string::npos)
                    end = entry.Text.size();
                Row row;
                row.Id = entry.Id;
                row.Kind = first ? RowKind::Head : RowKind::Code;
                m_maxColumns = std::max(m_maxColumns, Columns(std::string_view(entry.Text).substr(start, end - start)));
                row.Start = static_cast<std::uint32_t>(start);
                row.End = static_cast<std::uint32_t>(end);
                m_rows.push_back(row);
                first = false;
                if (end >= entry.Text.size())
                    break;
                start = end + 1;
            }
            return;
        }
        case Lua::EntryKind::Result:
            for (std::size_t v = 0; v < entry.Values.size() && v < 250; ++v)
            {
                if (entry.Values[v])
                    AddTree(entry.Id, static_cast<std::uint8_t>(v), *entry.Values[v], true);
            }
            return;
        default:
            break;
        }
        const Display display = DisplayOf(entry);
        for (std::size_t start = 0; start <= display.Text.size();)
        {
            const std::size_t end = std::min(display.Text.find('\n', start), display.Text.size());
            m_maxColumns = std::max(m_maxColumns, Columns(std::string_view(display.Text).substr(start, end - start)));
            start = end + 1;
        }
        const std::uint32_t reserve = std::min<std::uint32_t>(Columns(SourceLabel(entry)) + 2, m_columns / 3);
        const std::size_t firstBreak = display.Text.find('\n');
        const std::string_view firstLine = std::string_view(display.Text).substr(0, firstBreak);
        const std::uint32_t headColumns = m_columns > reserve + 8 ? m_columns - reserve : m_columns;
        const std::size_t headEnd = std::min(firstLine.size(), ByteAfter(firstLine, 0, headColumns));
        Row head;
        head.Id = entry.Id;
        head.Kind = RowKind::Head;
        head.Start = 0;
        head.End = static_cast<std::uint32_t>(headEnd);
        m_rows.push_back(head);
        if (headEnd < display.Text.size())
        {
            std::size_t rest = headEnd;
            if (rest < display.Text.size() && display.Text[rest] == '\n')
                ++rest;
            const std::size_t before = m_rows.size();
            WrapText(entry.Id, RowKind::Text, 0, std::string_view(display.Text).substr(rest), m_columns, false);
            for (std::size_t i = before; i < m_rows.size(); ++i)
            {
                m_rows[i].Start += static_cast<std::uint32_t>(rest);
                m_rows[i].End += static_cast<std::uint32_t>(rest);
            }
            if (m_rows.size() > before && m_rows[before].Start == m_rows[before].End && headEnd == firstLine.size())
                m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(before));
        }
        if (entry.Fields && entry.Fields->Nodes.size() > 1 && Expanded(entry.Id, NodeKey(kFieldsValue, 0)))
            AddTree(entry.Id, kFieldsValue, *entry.Fields, false);
        if (!entry.Values.empty() && Expanded(entry.Id, kValuesKey))
        {
            for (std::size_t v = 0; v < entry.Values.size() && v < 250; ++v)
            {
                if (entry.Values[v] && entry.Values[v]->Nodes.size() > 1)
                    AddTree(entry.Id, static_cast<std::uint8_t>(v), *entry.Values[v], true);
            }
        }
        if (!entry.Traceback.empty())
        {
            Row toggle;
            toggle.Id = entry.Id;
            toggle.Kind = RowKind::Toggle;
            m_rows.push_back(toggle);
            if (Expanded(entry.Id, kTracebackKey))
            {
                std::size_t start = 0;
                while (start < entry.Traceback.size())
                {
                    std::size_t end = entry.Traceback.find('\n', start);
                    if (end == std::string::npos)
                        end = entry.Traceback.size();
                    Row line;
                    line.Id = entry.Id;
                    line.Kind = RowKind::Traceback;
                    line.Start = static_cast<std::uint32_t>(start);
                    line.End = static_cast<std::uint32_t>(end);
                    m_rows.push_back(line);
                    start = end + 1;
                }
            }
        }
    }

    void ConsoleView::Rebuild(const Lua::ConsoleLog& log, const ConsoleFilter& filter)
    {
        m_rows.clear();
        m_maxColumns = 0;
        m_lastId = 0;
        m_lastRepeat = 0;
        log.Visit(0, log.Count(), [&](std::size_t, const Lua::ConsoleEntry& entry)
            {
                Append(entry, filter);
                m_lastId = entry.Id;
            });
        m_built = true;
        m_builtFilter = filter;
        m_epoch = log.Epoch();
        m_dropped = log.Dropped();
    }

    void ConsoleView::DrawFilterBar(const Lua::ConsoleLog& log, ConsoleFilter& filter, bool& changed)
    {
        const Lua::ConsoleCounts counts = log.Counts();
        constexpr LogLevel kLevels[] = { LogLevel::Debug, LogLevel::Info, LogLevel::Success, LogLevel::Warning, LogLevel::Error };
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, PxWhole(6.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Px(10.0f));
        const float start = ImGui::GetCursorPosX();
        for (std::size_t i = 0; i < std::size(kLevels); ++i)
        {
            const bool on = filter.Levels[i];
            const ImVec4 color = LevelColor(kLevels[i]);
            const std::string label = std::string(LevelIcon(kLevels[i])) + " " + Grouped(counts.Levels[i]) + "##level" + std::to_string(i);
            const float width = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            if (i > 0)
                FlowNext(width, start);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ManifoldTheme::WithAlpha(color, 0.18f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(color, 0.28f));
            ImGui::PushStyleColor(ImGuiCol_Text, on ? color : ManifoldTheme::Muted);
            if (ImGui::Button(label.c_str()))
            {
                filter.Levels[i] = !filter.Levels[i];
                changed = true;
            }
            ImGui::PopStyleColor(3);
            static constexpr const char* kNames[] = { "debug", "info and print", "success", "warning", "error" };
            ItemTooltip((std::string(on ? "Hide " : "Show ") + kNames[i] + " entries. Right-click shows only these.").c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                for (std::size_t k = 0; k < filter.Levels.size(); ++k)
                    filter.Levels[k] = k == i;
                changed = true;
            }
        }
        ImGui::PopStyleVar(2);

        const std::vector<std::string> channels = log.Channels();
        const float comboWidth = Px(150.0f);
        FlowNext(comboWidth, start);
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##channel", filter.Channel.empty() ? "All channels" : filter.Channel.c_str()))
        {
            if (ImGui::Selectable("All channels", filter.Channel.empty()))
            {
                filter.Channel.clear();
                changed = true;
            }
            for (const std::string& channel : channels)
            {
                const auto depth = static_cast<float>(std::count(channel.begin(), channel.end(), '/'));
                const std::size_t slash = channel.rfind('/');
                const std::string label = std::string(static_cast<std::size_t>(depth) * 2, ' ') + (slash == std::string::npos ? channel : channel.substr(slash + 1)) + "##" + channel;
                if (ImGui::Selectable(label.c_str(), filter.Channel == channel))
                {
                    filter.Channel = channel;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ItemTooltip("Shows one channel and its sub-channels.");

        const float searchWidth = Px(170.0f);
        FlowNext(searchWidth, start);
        ImGui::SetNextItemWidth(searchWidth);
        if (m_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusSearch = false;
        }
        if (InputTextWithHint("##consolesearch", ICON_FA_SEARCH " Search", filter.Search))
            changed = true;

        const auto toggle = [&](const char* icon, bool& value, const char* tip)
        {
            const float side = ImGui::GetFrameHeight();
            FlowNext(side, start);
            ImGui::PushStyleColor(ImGuiCol_Button, value ? ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.22f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, value ? ManifoldTheme::Accent : ManifoldTheme::Muted);
            ImGui::PushID(icon);
            if (ImGui::Button(icon, ImVec2(side, side)))
            {
                value = !value;
                changed = true;
            }
            ImGui::PopID();
            ImGui::PopStyleColor(2);
            ItemTooltip(tip);
        };
        toggle(ICON_FA_ALIGN_LEFT, filter.Wrap, "Wrap long lines at the window's edge");
        toggle(ICON_FA_CLOCK, filter.Time, "Show the time of each entry");
        toggle(ICON_FA_HASHTAG, filter.Hex, "Show integers in hex");
        const float side = ImGui::GetFrameHeight();
        FlowNext(side, start);
        bool follow = m_follow;
        toggle(ICON_FA_ARROW_DOWN, follow, "Follow new entries");
        m_follow = follow;
    }

    void ConsoleView::Draw(const Lua::ConsoleLog& log, const ConsoleFilter& filter, ImVec2 size, const ConsoleActions& actions)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(4.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::WithAlpha(ManifoldTheme::Sidebar, 0.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(4.0f));
        if (!filter.Wrap)
            ImGui::SetNextWindowContentSize(ImVec2(MessageOffset(filter.Time) + static_cast<float>(m_maxColumns + 24) * m_glyph, 0.0f));
        const bool visible = ImGui::BeginChild("##consolerows", size, ImGuiChildFlags_AlwaysUseWindowPadding, filter.Wrap ? 0 : ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (!visible)
        {
            ImGui::EndChild();
            return;
        }
        PushMono();
        m_glyph = std::max(1.0f, ImGui::CalcTextSize("M").x);
        const float width = ImGui::GetContentRegionAvail().x;
        const float messageX = MessageOffset(filter.Time);
        const std::uint32_t columns = filter.Wrap ? static_cast<std::uint32_t>(std::max(16.0f, std::floor((width - messageX - Px(6.0f)) / m_glyph))) : 4096u;
        const bool filterChanged = filter.Levels != m_builtFilter.Levels || filter.Channel != m_builtFilter.Channel || filter.Search != m_builtFilter.Search ||
                                   filter.Wrap != m_builtFilter.Wrap || filter.Time != m_builtFilter.Time;
        const std::uint64_t first = log.FirstId();
        const std::size_t before = m_rows.size();
        if (!m_built || filterChanged || columns != m_columns || log.Epoch() != m_epoch)
        {
            m_columns = columns;
            Rebuild(log, filter);
        }
        else if (log.Generation() != m_generation)
        {
            if (log.Dropped() != m_dropped)
            {
                const auto keep = std::find_if(m_rows.begin(), m_rows.end(), [first](const Row& row) { return row.Id >= first; });
                m_rows.erase(m_rows.begin(), keep);
                m_dropped = log.Dropped();
            }
            for (std::uint64_t id = std::max(m_lastId + 1, first); log.VisitId(id, [&](const Lua::ConsoleEntry& entry) { Append(entry, filter); }); ++id)
                m_lastId = id;
        }
        const bool grew = m_rows.size() != before || log.Generation() != m_generation;
        m_generation = log.Generation();

        const float rowHeight = std::round(ImGui::GetTextLineHeight() + Px(3.0f));
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - rowHeight;
        if (const std::uint64_t dropped = log.Dropped())
            Muted(Grouped(dropped) + (dropped == 1 ? " earlier entry is" : " earlier entries are") + " no longer kept");
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()), rowHeight);
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Row& row = m_rows[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                if (!log.VisitId(row.Id, [&](const Lua::ConsoleEntry& entry) { DrawRow(row, entry, filter, actions, width); }))
                    ImGui::Dummy(ImVec2(width, rowHeight));
                ImGui::PopID();
            }
        }
        if (grew && (m_follow || atBottom))
            ImGui::SetScrollHereY(1.0f);

        if (m_menuId != 0)
        {
            ImGui::OpenPopup("##entrymenu");
            m_selected = m_menuId;
        }
        PopMono();
        if (ImGui::BeginPopup("##entrymenu"))
        {
            const std::uint64_t id = m_selected;
            log.VisitId(id, [&](const Lua::ConsoleEntry& entry)
                {
                    if (m_menuHasAddress)
                    {
                        const std::string address = "0x" + Hex(m_menuAddress);
                        ImGui::TextColored(ManifoldTheme::AddressText, "%s", address.c_str());
                        if (actions.OpenMemory && ImGui::MenuItem(ICON_FA_MEMORY "  Open in the memory viewer"))
                            actions.OpenMemory(m_menuAddress);
                        if (actions.Dissect && ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect the structure here"))
                            actions.Dissect(m_menuAddress);
                        if (actions.AddAddress && ImGui::MenuItem(ICON_FA_PLUS "  Add to the address list"))
                            actions.AddAddress(m_menuAddress);
                        if (ImGui::MenuItem(ICON_FA_COPY "  Copy the address"))
                            ImGui::SetClipboardText(address.c_str());
                        ImGui::Separator();
                    }
                    if (!m_menuValue.empty() && ImGui::MenuItem(ICON_FA_COPY "  Copy the value"))
                        ImGui::SetClipboardText(m_menuValue.c_str());
                    if (ImGui::MenuItem(ICON_FA_COPY "  Copy the entry"))
                        ImGui::SetClipboardText(entry.PlainText().c_str());
                    if (!entry.Traceback.empty() && ImGui::MenuItem(ICON_FA_LIST "  Copy the traceback"))
                        ImGui::SetClipboardText(entry.Traceback.c_str());
                    if (entry.Kind == Lua::EntryKind::Input && actions.Recall && ImGui::MenuItem(ICON_FA_REDO "  Put back into the input"))
                        actions.Recall(entry.Text);
                    if (!entry.Source.Chunk.empty() && entry.Source.Line > 0 && actions.GotoSource && ImGui::MenuItem(ICON_FA_CODE "  Go to the line"))
                        actions.GotoSource(entry.Source.Chunk, entry.Source.Line);
                });
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy everything shown"))
            {
                std::string text;
                std::uint64_t last = 0;
                for (const Row& row : m_rows)
                {
                    if (row.Id == last)
                        continue;
                    last = row.Id;
                    log.VisitId(row.Id, [&](const Lua::ConsoleEntry& entry) { text += entry.PlainText() + "\n"; });
                }
                ImGui::SetClipboardText(text.c_str());
            }
            ImGui::EndPopup();
        }
        m_menuId = 0;
        if (ImGui::IsWindowFocused() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) && m_selected != 0)
            log.VisitId(m_selected, [](const Lua::ConsoleEntry& entry) { ImGui::SetClipboardText(entry.PlainText().c_str()); });
        ImGui::EndChild();
    }

    void ConsoleView::DrawRow(const Row& row, const Lua::ConsoleEntry& entry, const ConsoleFilter& filter, const ConsoleActions& actions, float width)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float rowHeight = std::round(ImGui::GetTextLineHeight() + Px(3.0f));
        const float textY = origin.y + std::round((rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
        const float messageX = origin.x + MessageOffset(filter.Time);
        const ImVec2 max(origin.x + std::max(width, ImGui::GetContentRegionAvail().x), origin.y + rowHeight);
        ImGui::InvisibleButton("##row", ImVec2(std::max(1.0f, max.x - origin.x), rowHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (entry.Kind == Lua::EntryKind::Error || entry.Level == LogLevel::Error)
            draw->AddRectFilled(origin, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.07f)));
        else if (entry.Level == LogLevel::Warning)
            draw->AddRectFilled(origin, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.05f)));
        else if (entry.Kind == Lua::EntryKind::Input)
            draw->AddRectFilled(origin, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.05f)));
        if (m_selected == entry.Id)
            draw->AddRectFilled(origin, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.14f)));
        else if (hovered)
            draw->AddRectFilled(origin, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.04f)));
        if (clicked)
            m_selected = entry.Id;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        {
            m_menuId = entry.Id;
            m_menuHasAddress = false;
            m_menuValue.clear();
        }

        const bool head = row.Kind == RowKind::Head || (row.Kind == RowKind::Node && (row.Start & ~kMoreFlag) == 0 && row.Value != kFieldsValue && entry.Kind == Lua::EntryKind::Result);
        if (head)
        {
            float x = origin.x + Px(4.0f);
            const char* icon = nullptr;
            ImVec4 color = ManifoldTheme::Muted;
            switch (entry.Kind)
            {
            case Lua::EntryKind::Input:
                icon = "\xE2\x80\xBA";
                color = ManifoldTheme::Accent;
                break;
            case Lua::EntryKind::Result:
                icon = "\xE2\x86\x90";
                color = ManifoldTheme::Accent;
                break;
            case Lua::EntryKind::Log:
            case Lua::EntryKind::Error:
                icon = LevelIcon(entry.Level);
                color = LevelColor(entry.Level);
                break;
            case Lua::EntryKind::System:
                icon = ICON_FA_INFO_CIRCLE;
                break;
            case Lua::EntryKind::Print:
                break;
            }
            if (icon && (row.Kind == RowKind::Head || row.Value == 0))
                draw->AddText(ImVec2(x, textY), ImGui::GetColorU32(color), icon);
            x += m_glyph * 2.0f;
            if (filter.Time && (row.Kind == RowKind::Head || row.Value == 0))
                draw->AddText(ImVec2(x, textY), ImGui::GetColorU32(ManifoldTheme::Muted), Lua::ConsoleTime(entry.TimeMs, false).c_str());
            if (row.Kind == RowKind::Head || row.Value == 0)
            {
                const std::string source = SourceLabel(entry);
                if (!source.empty())
                {
                    const float sourceWidth = ImGui::CalcTextSize(source.c_str()).x;
                    const float right = std::min(origin.x + width, ImGui::GetCurrentWindow()->InnerRect.Max.x);
                    const float sx = std::max(messageX, right - sourceWidth - Px(6.0f));
                    draw->AddRectFilled(ImVec2(sx - Px(6.0f), origin.y), ImVec2(right, origin.y + rowHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Sidebar, 0.85f)));
                    draw->AddText(ImVec2(sx, textY), ImGui::GetColorU32(ManifoldTheme::Muted), source.c_str());
                    if (hovered && entry.Source.Line > 0)
                        ImGui::SetItemTooltip("%s", (entry.Source.Chunk + ":" + std::to_string(entry.Source.Line) + (entry.Source.Function.empty() ? std::string() : " in " + entry.Source.Function)).c_str());
                }
            }
        }

        switch (row.Kind)
        {
        case RowKind::Head:
        case RowKind::Text:
        {
            if (entry.Kind == Lua::EntryKind::Input)
            {
                std::vector<Text::TokenSpan> spans;
                Text::LineState state = Text::InitialState(Text::Language::Lua);
                const std::string_view line = std::string_view(entry.Text).substr(row.Start, row.End - row.Start);
                Text::HighlightLine(Text::Language::Lua, line, 0, state, spans);
                float x = messageX;
                std::size_t drawn = 0;
                for (const Text::TokenSpan& span : spans)
                {
                    Segment(draw, x, textY, line.substr(drawn, span.Start - drawn), ImGui::GetColorU32(ManifoldTheme::Text));
                    Segment(draw, x, textY, line.substr(span.Start, span.End - span.Start), EditorShared::TokenColor(span.Kind));
                    drawn = span.End;
                }
                Segment(draw, x, textY, line.substr(drawn), ImGui::GetColorU32(ManifoldTheme::Text));
                break;
            }
            const Display display = DisplayOf(entry);
            float x = messageX;
            const auto part = [&](std::size_t from, std::size_t to, const ImVec4& color)
            {
                from = std::max<std::size_t>(from, row.Start);
                to = std::min<std::size_t>(to, row.End);
                if (from < to)
                    Segment(draw, x, textY, std::string_view(display.Text).substr(from, to - from), ImGui::GetColorU32(color));
            };
            if (row.Start < display.CodeEnd)
            {
                const float before = x;
                part(0, display.CodeEnd, ManifoldTheme::Error);
                draw->AddRect(ImVec2(before - Px(2.0f), origin.y + Px(1.0f)), ImVec2(x - m_glyph + Px(2.0f), origin.y + rowHeight - Px(1.0f)),
                    ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.6f)), Px(3.0f));
            }
            part(display.CodeEnd, display.ChannelEnd, ManifoldTheme::Label);
            part(display.ChannelEnd, display.MessageEnd, EntryColor(entry));
            part(display.MessageEnd, display.Text.size(), ManifoldTheme::Muted);
            if (row.Kind == RowKind::Head)
            {
                const bool fields = entry.Fields && entry.Fields->Nodes.size() > 1;
                bool values = false;
                for (const auto& value : entry.Values)
                    values = values || (value && value->Nodes.size() > 1);
                if (fields || values)
                {
                    const std::uint32_t key = fields ? NodeKey(kFieldsValue, 0) : kValuesKey;
                    const bool open = Expanded(entry.Id, key);
                    draw->AddText(ImVec2(messageX - m_glyph * 1.4f, textY), ImGui::GetColorU32(ManifoldTheme::Accent), open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
                    if (clicked)
                        Toggle(entry.Id, key);
                }
            }
            break;
        }
        case RowKind::Code:
        {
            std::vector<Text::TokenSpan> spans;
            Text::LineState state = Text::InitialState(Text::Language::Lua);
            const std::string_view line = std::string_view(entry.Text).substr(row.Start, row.End - row.Start);
            Text::HighlightLine(Text::Language::Lua, line, 0, state, spans);
            float x = messageX;
            std::size_t drawn = 0;
            for (const Text::TokenSpan& span : spans)
            {
                Segment(draw, x, textY, line.substr(drawn, span.Start - drawn), ImGui::GetColorU32(ManifoldTheme::Text));
                Segment(draw, x, textY, line.substr(span.Start, span.End - span.Start), EditorShared::TokenColor(span.Kind));
                drawn = span.End;
            }
            Segment(draw, x, textY, line.substr(drawn), ImGui::GetColorU32(ManifoldTheme::Text));
            break;
        }
        case RowKind::Toggle:
        {
            const bool open = Expanded(entry.Id, kTracebackKey);
            const std::size_t lines = static_cast<std::size_t>(std::count(entry.Traceback.begin(), entry.Traceback.end(), '\n'));
            const std::string label = std::string(open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT) + " traceback (" + std::to_string(lines) + (lines == 1 ? " frame)" : " frames)");
            draw->AddText(ImVec2(messageX, textY), ImGui::GetColorU32(hovered ? ManifoldTheme::Text : ManifoldTheme::Muted), label.c_str());
            if (clicked)
                Toggle(entry.Id, kTracebackKey);
            break;
        }
        case RowKind::Traceback:
        {
            const std::string_view line = std::string_view(entry.Traceback).substr(row.Start, row.End - row.Start);
            float x = messageX + m_glyph * 2.0f;
            std::string clean(line);
            std::replace(clean.begin(), clean.end(), '\t', ' ');
            Segment(draw, x, textY, clean, ImGui::GetColorU32(ManifoldTheme::Muted));
            break;
        }
        case RowKind::Node:
        {
            const Lua::ValueSnapshot* snapshot = ValueOf(entry, row.Value);
            if (!snapshot)
                break;
            const bool more = (row.Start & kMoreFlag) != 0;
            const std::size_t index = row.Start & ~kMoreFlag;
            if (index >= snapshot->Nodes.size())
                break;
            const Lua::SnapshotNode& node = snapshot->Nodes[index];
            const int depth = row.Value == kFieldsValue ? std::max(0, static_cast<int>(node.Depth) - 1) : node.Depth;
            float x = messageX + m_glyph * 2.0f * static_cast<float>(depth + (more ? 1 : 0));
            if (more)
            {
                Segment(draw, x, textY, "\xE2\x80\xA6 " + Grouped(node.Total - node.Shown) + " more", ImGui::GetColorU32(ManifoldTheme::Muted));
                break;
            }
            const std::uint32_t key = NodeKey(row.Value, static_cast<std::uint32_t>(index));
            if (node.Subtree > 0 || (node.Total > 0 && node.Shown > 0))
            {
                const bool open = Expanded(entry.Id, key);
                draw->AddText(ImVec2(x - m_glyph * 1.4f, textY), ImGui::GetColorU32(ManifoldTheme::Accent), open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
            }
            if (!node.Key.empty())
            {
                Segment(draw, x, textY, node.Key, ImGui::GetColorU32(ManifoldTheme::Label));
                Segment(draw, x, textY, " = ", ImGui::GetColorU32(ManifoldTheme::Muted));
            }
            const std::string value = Lua::NodeValueText(node, filter.Hex);
            const float valueX = x;
            Segment(draw, x, textY, value, ImGui::GetColorU32(NodeColor(node)));
            if (node.Type == "table" && node.Label.empty() == false && node.Total > 0)
                Segment(draw, x, textY, "  {" + Grouped(node.Total) + "}", ImGui::GetColorU32(ManifoldTheme::Muted));
            if (node.IsInteger && node.IsAddress && !filter.Hex)
                Segment(draw, x, textY, "  " + node.Value, ImGui::GetColorU32(ManifoldTheme::Muted));
            const bool overValue = hovered && ImGui::GetIO().MousePos.x >= valueX && ImGui::GetIO().MousePos.x <= valueX + ImGui::CalcTextSize(value.c_str()).x;
            if (node.IsAddress && overValue)
            {
                draw->AddLine(ImVec2(valueX, textY + ImGui::GetTextLineHeight()), ImVec2(valueX + ImGui::CalcTextSize(value.c_str()).x, textY + ImGui::GetTextLineHeight()),
                    ImGui::GetColorU32(ManifoldTheme::AddressText), Px(1.0f));
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (clicked && node.IsAddress && overValue))
            {
                m_menuId = entry.Id;
                m_menuHasAddress = node.IsAddress;
                m_menuAddress = static_cast<std::uint64_t>(node.Integer);
                m_menuValue = value;
            }
            else if (clicked && (node.Subtree > 0 || node.Shown > 0))
            {
                Toggle(entry.Id, key);
            }
            break;
        }
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && entry.Kind == Lua::EntryKind::Input && actions.Recall)
            actions.Recall(entry.Text);
    }
}
