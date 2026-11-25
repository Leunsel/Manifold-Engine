#include "UI/CommandPalette.h"

#include "App/Commands.h"
#include "App/Engine.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Layout/Layout.h"
#include "Table/AddressList.h"
#include "UI/SettingsWindow.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Workspace.h"

#include <algorithm>
#include <filesystem>

namespace Manifold::UI
{
    namespace
    {
        constexpr int kRows = 11;
        constexpr std::size_t kLimit = 300;
        constexpr const char* kPopup = "##commandpalette";

        struct NamedWindow
        {
            const char* Name;
            const char* Title;
            const char* Detail;
            bool Release;
        };

        // The tool windows that have no command of their own (those come from the command table).
        constexpr NamedWindow kWindows[] = {
            { "symbols", "Symbols and Modules", "", false },
            { "regions", "Memory Regions", "", false },
            { "patches", "Scan for Patches", "", false },
            { "dissect", "Code Dissect", "referenced functions, strings and data", false },
            { "asmsearch", "Find Assembly", "", false },
            { "bytes", "Find Bytes", "an array of bytes or a signature", false },
            { "threadlist", "Thread List", "", false },
            { "breakpoints", "Breakpoint List", "", false },
            { "writehistory", "Write History", "every change to the game's memory, with undo", false },
            { "comments", "Comment List", "", false },
            { "flowgraph", "Flow Graph", "", false },
            { "templates", "Template Manager", "Auto Assembler templates", false },
            { "modules", "Autorun Modules", "", false },
            { "about", "About Manifold Engine", "", true },
            { "credits", "Credits", "who made and helped make Manifold Engine", true },
        };

        const char* KindIcon(PaletteKind kind)
        {
            switch (kind)
            {
            case PaletteKind::Command: return ICON_FA_TERMINAL;
            case PaletteKind::Window: return ICON_FA_WINDOW_MAXIMIZE;
            case PaletteKind::Setting: return ICON_FA_SLIDERS_H;
            case PaletteKind::Table: return ICON_FA_FILE_ALT;
            case PaletteKind::Record: return ICON_FA_LIST;
            }
            return "";
        }

        std::string RecordDetail(const Table::MemoryRecord& record)
        {
            switch (record.Kind)
            {
            case Table::RecordKind::Script: return record.Active ? "Script, on" : "Script, off";
            case Table::RecordKind::Group: return "Group";
            case Table::RecordKind::Value:
            {
                std::string text = "Value";
                if (!record.ValueText.empty() && record.ValueText != "??")
                    text += " " + record.ValueText;
                if (record.Active)
                    text += ", frozen";
                return text;
            }
            }
            return {};
        }
    }

    CommandPalette::CommandPalette(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void CommandPalette::Toggle()
    {
        if (m_open)
            Close();
        else
            Open();
    }

    void CommandPalette::Open(std::string query)
    {
        m_open = true;
        m_appearing = true;
        m_query = std::move(query);
        m_selected = 0;
        m_scroll = 0;
        m_mouseY = ImGui::GetIO().MousePos.y;
        Build();
        m_ranked = "\x01";
        Rank();
    }

    // ∑ The popup is closed from inside it, where ImGui expects that: from
    //   outside (the shortcut again) the next frame submits it once to close it.
    void CommandPalette::Close()
    {
        m_open = false;
        m_closing = true;
    }

    // ∑ Built once when the palette opens: the list reflects that moment (a
    //   command that cannot run now is left out), and typing only ranks.
    void CommandPalette::Build()
    {
        Engine& engine = m_workspace.Core();
        m_items.clear();
        m_entries.clear();
        const auto add = [&](PaletteItem item, std::function<void()> main, std::function<void()> alternate = {})
            {
                if (!*item.Icon)
                    item.Icon = KindIcon(item.Kind);
                item.Order = static_cast<int>(m_items.size());
                m_items.push_back(std::move(item));
                m_entries.push_back({ std::move(main), std::move(alternate) });
            };

        for (const CommandInfo& info : AllCommands())
        {
            if ((info.Scope != CommandScope::Global && info.Scope != CommandScope::Main) || info.Id == Command::CommandPalette || !m_workspace.CanRunAnywhere(info.Id))
                continue;
            PaletteItem item;
            item.Kind = PaletteKind::Command;
            item.Key = std::string("command:") + info.Key;
            item.Title = info.Name;
            item.Shortcut = ShortcutLabel(info.Id);
            const Command command = info.Id;
            add(std::move(item), [this, command] { m_workspace.RunAnywhere(command); });
        }

        const bool release = engine.Options().ReleaseMode;
        for (const NamedWindow& window : kWindows)
        {
            if (release && !window.Release)
                continue;
            PaletteItem item;
            item.Kind = PaletteKind::Window;
            item.Key = std::string("window:") + window.Name;
            item.Title = window.Title;
            item.Detail = window.Detail;
            const std::string name = window.Name;
            add(std::move(item), [this, name] { m_workspace.OpenNamed(name); });
        }

        for (int page = 0; page < SettingsWindow::PageCount; ++page)
        {
            PaletteItem item;
            item.Kind = PaletteKind::Setting;
            item.Key = "settings:" + std::to_string(page);
            item.Title = std::string("Settings: ") + SettingsWindow::PageTitle(page);
            item.Detail = SettingsWindow::PageSubtitle(page);
            item.Icon = SettingsWindow::PageIcon(page);
            add(std::move(item), [this, page] { m_workspace.OpenSettings(page); });
        }

        for (const std::string& path : engine.Options().RecentTables)
        {
            const std::filesystem::path file = Utf8ToWide(path);
            PaletteItem item;
            item.Kind = PaletteKind::Table;
            item.Key = "table:" + path;
            item.Title = WideToUtf8(file.filename().wstring());
            item.Detail = WideToUtf8(file.parent_path().wstring());
            add(std::move(item), [this, path] { m_workspace.OpenTable(Utf8ToWide(path), false); });
        }

        engine.Table().ForEach([&](Table::MemoryRecord& record)
            {
                PaletteItem item;
                item.Kind = PaletteKind::Record;
                item.Key = "record:" + std::to_string(record.Id);
                item.Title = record.Description.empty() ? std::string("(no description)") : record.Description;
                item.Detail = RecordDetail(record);
                item.Icon = record.Kind == Table::RecordKind::Script ? (record.Active ? ICON_FA_TOGGLE_ON : ICON_FA_TOGGLE_OFF)
                            : record.Kind == Table::RecordKind::Group ? ICON_FA_FOLDER
                            : record.Active ? ICON_FA_SNOWFLAKE : ICON_FA_LIST;
                const int id = record.Id;
                const bool switches = record.Kind != Table::RecordKind::Group;
                const auto reveal = [this, id] { m_workspace.RevealRecord(id); };
                const auto flip = [this, id]
                    {
                        Engine& core = m_workspace.Core();
                        if (Table::MemoryRecord* found = core.Table().FindById(id))
                            core.RequestActive(*found, !found->Active);
                    };
                if (switches)
                    add(std::move(item), flip, reveal);
                else
                    add(std::move(item), reveal, reveal);
            });
    }

    void CommandPalette::Rank()
    {
        if (m_ranked == m_query)
            return;
        m_ranked = m_query;
        const std::vector<std::string> recent = SplitPaletteRecent(m_workspace.Core().Options().PaletteRecent);
        m_matches = RankPalette(m_items, m_query, recent, kLimit);
        m_selected = 0;
        m_scroll = 0;
    }

    void CommandPalette::Choose(std::size_t match, bool alternate)
    {
        if (match >= m_matches.size())
            return;
        const std::size_t index = m_matches[match].Index;
        Settings& options = m_workspace.Core().Options();
        std::vector<std::string> recent = SplitPaletteRecent(options.PaletteRecent);
        RememberPaletteKey(recent, m_items[index].Key);
        options.PaletteRecent = JoinPaletteRecent(recent);
        m_workspace.Core().SaveOptionsSoon();
        const Entry& entry = m_entries[index];
        m_pending = alternate && entry.Alternate ? entry.Alternate : entry.Main;
        m_open = false;
        if (m_inPopup)
            ImGui::CloseCurrentPopup();
        else
            m_closing = true;
    }

    // ∑ The chosen action runs after the palette's popup ended: a command that
    //   opens a popup of its own (Open Process) would otherwise open it inside
    //   the palette's, and closing the palette would close it too.
    void CommandPalette::RunPending()
    {
        std::function<void()> action = std::move(m_pending);
        m_pending = nullptr;
        if (action)
            action();
    }

    bool CommandPalette::Run(const std::string& key, bool alternate)
    {
        if (m_items.empty())
            Build();
        m_ranked = "\x01";
        m_query.clear();
        Rank();
        const auto item = std::find_if(m_items.begin(), m_items.end(), [&](const PaletteItem& candidate) { return candidate.Key == key; });
        if (item == m_items.end())
            return false;
        const std::size_t index = static_cast<std::size_t>(item - m_items.begin());
        m_matches.insert(m_matches.begin(), PaletteMatch{ index, 0, {} });
        Choose(0, alternate);
        RunPending();
        return true;
    }

    void CommandPalette::Row(std::size_t row, float width, float height)
    {
        const PaletteMatch& match = m_matches[row];
        const PaletteItem& item = m_items[match.Index];
        ImGui::PushID(static_cast<int>(row));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##paletterow", ImVec2(width, height));
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemHovered() && io.MousePos.y != m_mouseY)
        {
            m_selected = static_cast<int>(row);
            m_mouseY = io.MousePos.y;
        }
        ImGui::PopID();
        const ImVec2 max(min.x + width, min.y + height);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const bool selected = static_cast<int>(row) == m_selected;
        if (selected)
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.18f)), Px(5.0f));
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const float textY = min.y + (height - size) * 0.5f;
        const float pad = Px(10.0f);
        const ImU32 text = ImGui::GetColorU32(ManifoldTheme::Text);
        const ImU32 muted = ImGui::GetColorU32(ManifoldTheme::Muted);
        const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
        draw->AddText(font, size, ImVec2(min.x + pad, textY), selected ? accent : muted, item.Icon);

        float right = max.x - pad;
        if (!item.Shortcut.empty())
        {
            const ImVec2 keys = font->CalcTextSizeA(size * 0.9f, FLT_MAX, 0.0f, item.Shortcut.c_str());
            const ImVec2 capMin(right - keys.x - Px(10.0f), min.y + Px(5.0f));
            const ImVec2 capMax(right, max.y - Px(5.0f));
            draw->AddRect(capMin, capMax, ImGui::GetColorU32(ManifoldTheme::Border), Px(4.0f));
            draw->AddText(font, size * 0.9f, ImVec2(capMin.x + Px(5.0f), min.y + (height - size * 0.9f) * 0.5f), muted, item.Shortcut.c_str());
            right = capMin.x - Px(8.0f);
        }
        if (item.Kind != PaletteKind::Command)
        {
            const char* tag = PaletteKindName(item.Kind);
            const float tagWidth = font->CalcTextSizeA(size * 0.85f, FLT_MAX, 0.0f, tag).x;
            draw->AddText(font, size * 0.85f, ImVec2(right - tagWidth, min.y + (height - size * 0.85f) * 0.5f), muted, tag);
            right -= tagWidth + Px(10.0f);
        }

        const ImVec4 clip(min.x, min.y, std::max(min.x, right), max.y);
        float x = min.x + pad + size * 1.8f;
        const std::string& title = item.Title;
        std::size_t at = 0;
        std::size_t next = 0;
        while (at < title.size())
        {
            const bool lit = next < match.Highlight.size() && static_cast<std::size_t>(match.Highlight[next]) == at;
            std::size_t end = at + 1;
            if (lit)
            {
                ++next;
                while (end < title.size() && next < match.Highlight.size() && static_cast<std::size_t>(match.Highlight[next]) == end)
                {
                    ++end;
                    ++next;
                }
            }
            else
            {
                while (end < title.size() && !(next < match.Highlight.size() && static_cast<std::size_t>(match.Highlight[next]) == end))
                    ++end;
            }
            const char* begin = title.c_str() + at;
            const char* stop = title.c_str() + end;
            draw->AddText(font, size, ImVec2(x, textY), lit ? accent : text, begin, stop, 0.0f, &clip);
            if (lit)  // a second pass one pixel over makes the matched letters stand out like bold
                draw->AddText(font, size, ImVec2(x + 1.0f, textY), accent, begin, stop, 0.0f, &clip);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, stop).x;
            at = end;
        }
        if (!item.Detail.empty() && x + Px(14.0f) < clip.z)
            draw->AddText(font, size, ImVec2(x + Px(14.0f), textY), muted, item.Detail.c_str(), nullptr, 0.0f, &clip);
        if (clicked)
            Choose(row, ImGui::GetIO().KeyAlt);
    }

    void CommandPalette::Draw()
    {
        if (!m_open)
        {
            if (m_closing && ImGui::BeginPopup(kPopup))
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            m_closing = false;
            return;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(Px(660.0f), viewport->WorkSize.x - Px(32.0f));
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.12f));
        ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
        ImGui::SetNextWindowViewport(viewport->ID);
        if (m_appearing && !ImGui::IsPopupOpen(kPopup))
            ImGui::OpenPopup(kPopup);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoDocking;
        if (!ImGui::BeginPopup(kPopup, flags))
        {
            m_open = false;
            return;
        }
        m_closing = false;
        m_inPopup = true;
        Contents();
        m_inPopup = false;
        ImGui::EndPopup();
        RunPending();
    }

    void CommandPalette::Contents()
    {
        Layout::FieldOptions field;
        field.Placement = Layout::LabelPlacement::None;
        Layout::Field("Search", field);
        if (m_appearing)
        {
            ImGui::SetKeyboardFocusHere();
            m_appearing = false;
        }
        const bool enter = InputTextWithHint("##palettequery", ICON_FA_SEARCH "  A command, a window, a table or a record", m_query, ImGuiInputTextFlags_EnterReturnsTrue);
        Rank();
        const ImGuiIO& io = ImGui::GetIO();
        const int count = static_cast<int>(m_matches.size());
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            m_open = false;
            ImGui::CloseCurrentPopup();
            return;
        }
        if (count > 0)
        {
            const int before = m_selected;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
                m_selected = std::min(count - 1, m_selected + 1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
                m_selected = std::max(0, m_selected - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
                m_selected = std::min(count - 1, m_selected + kRows);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
                m_selected = std::max(0, m_selected - kRows);
            // the keys keep the choice in view; the wheel only moves the view
            if (m_selected != before)
            {
                if (m_selected < m_scroll)
                    m_scroll = m_selected;
                if (m_selected >= m_scroll + kRows)
                    m_scroll = m_selected - kRows + 1;
            }
            if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f)
                m_scroll -= static_cast<int>(io.MouseWheel * 3.0f);
            m_scroll = std::clamp(m_scroll, 0, std::max(0, count - kRows));
            m_selected = std::clamp(m_selected, 0, count - 1);
        }
        if (enter && count > 0)
        {
            Choose(static_cast<std::size_t>(m_selected), io.KeyAlt);
            return;
        }
        Layout::Spacing(Layout::Space::S);
        const float rowHeight = std::round(ImGui::GetFontSize() * 1.95f);
        const float listWidth = ImGui::GetContentRegionAvail().x;
        if (count == 0)
        {
            Layout::Muted(m_query.empty() ? "Nothing to offer: no table is open." : "Nothing matches. > commands only, @ records, # tables.");
        }
        else
        {
            const int last = std::min(count, m_scroll + kRows);
            for (int row = m_scroll; row < last && m_open; ++row)
                Row(static_cast<std::size_t>(row), listWidth, rowHeight);
        }
        if (m_open)
        {
            Layout::Spacing(Layout::Space::S);
            const PaletteItem* current = count > 0 && m_selected < count ? &m_items[m_matches[static_cast<std::size_t>(m_selected)].Index] : nullptr;
            std::string hint = Format("%d of %zu", count, m_items.size()) + "  " ICON_FA_ARROWS_ALT_V " choose  ·  Enter ";
            if (current && current->Kind == PaletteKind::Record)
                hint += current->Detail == "Group" ? "show it in the list" : "switch it  ·  Alt+Enter show it in the list";
            else
                hint += "run";
            hint += "  ·  Esc close  ·  > commands  @ records  # tables";
            Layout::Muted(hint);
        }
    }
}
