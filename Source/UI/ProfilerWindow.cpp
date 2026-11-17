#include "UI/ProfilerWindow.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Errors.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        const char* const kTabNames[] = { ICON_FA_CHART_BAR "  Overview", ICON_FA_STREAM "  Breakdown", ICON_FA_LAYER_GROUP "  Threads", ICON_FA_STETHOSCOPE "  Checks",
            ICON_FA_INFO_CIRCLE "  App info" };

        struct ZoneHelp
        {
            const char* Name;
            const char* Text;
        };

        const ZoneHelp kZoneHelp[] = {
            { "Whole frame", "From the start of one frame to the start of the next, including the time the program waits." },
            { "Window messages", "Messages Windows sends to the program's windows: mouse, keyboard, focus and resizing." },
            { "Waiting for input", "The program sleeps until input arrives or the next idle tick is due. Idle time, not work." },
            { "Engine tick while waiting", "While the window idles, the engine keeps reading values, writing frozen values, checking hotkeys and running Lua timers every 15 ms." },
            { "Engine tick", "Reads values, writes frozen values, checks hotkeys, runs Lua timers and tasks, and picks up finished background work." },
            { "Input and new frame", "Starts the frame: reads mouse and keyboard and prepares Dear ImGui." },
            { "Build the interface", "Lays out every window and widget of this frame. Usually the biggest part of the work." },
            { "Finish the draw lists", "Dear ImGui finishes and sorts the draw lists of every window." },
            { "Draw the main window", "Hands the main window's draw lists to the graphics card." },
            { "Draw the tool windows", "Draws every separate tool window into its own swap chain." },
            { "Present the tool windows", "Hands the tool windows to the display. It can wait for a refresh when the display is still busy with the last frame, so it counts as waiting, not work." },
            { "Present (waits for the display)", "Shows the frame and waits for the next refresh of the display. Waiting, not work." },
            { "Finished background jobs", "Results of work that ran in the background, such as symbol loading and searches." },
            { "Scanner", "Picks up progress and results of a running scan." },
            { "Process check", "Checks once in a while whether the opened process still runs." },
            { "Open processes automatically", "Looks for the processes from Settings > Scripts > Open processes automatically." },
            { "Frozen values", "Writes frozen values back into the process." },
            { "Hotkeys", "Checks the hotkeys of the records." },
            { "Address list values", "Reads the values of the records that are visible in the address list." },
            { "Lua timers", "Runs the Lua timers that are due. Slow timer callbacks show up here." },
            { "Work for the main thread", "Lua parts of scripts that run on the main thread while the Auto Assembler works in the background." },
            { "Lua tasks", "Continues Lua work that runs in slices: the table script, activations and long callbacks." },
            { "Script activations", "Turns queued scripts on and off; the Auto Assembler itself runs in the background." },
            { "Debugger", "Checks for a break and runs breakpoint callbacks." },
            { "Requests and shortcuts", "Requests from scripts, such as opening a window or selecting an address, and the global shortcuts." },
            { "Main window", "The main window: title bar, process bar, scanner, found list and address list." },
            { "Tool windows", "Every open tool window, each listed below." },
            { "Dialogs and notices", "Confirmations, prompts and the notices in the corner." },
            { "Script forms", "Windows that Lua scripts build with createForm." },
        };

        const char* HelpFor(const char* name)
        {
            for (const ZoneHelp& help : kZoneHelp)
            {
                if (std::string_view(help.Name) == name)
                    return help.Text;
            }
            return nullptr;
        }

        ImVec4 Palette(std::size_t index)
        {
            const ImVec4 colors[] = { ManifoldTheme::Accent, ManifoldTheme::CodeRegister, ManifoldTheme::CodeKeyword, ManifoldTheme::CodeNumber, ManifoldTheme::CodeString,
                ManifoldTheme::FlowJump, ManifoldTheme::FlowReturn, ManifoldTheme::CodeLabel, ManifoldTheme::AddressText, ManifoldTheme::CodeSection };
            return colors[index % std::size(colors)];
        }

        std::string Fixed(double value, int digits)
        {
            char text[64];
            std::snprintf(text, sizeof(text), "%.*f", digits, value);
            return text;
        }

        ImVec4 LevelTint(Diagnostics::Level level)
        {
            switch (level)
            {
            case Diagnostics::Level::Ok: return ManifoldTheme::Accent;
            case Diagnostics::Level::Info: return ManifoldTheme::AddressText;
            case Diagnostics::Level::Warning: return ManifoldTheme::Warning;
            case Diagnostics::Level::Problem: return ManifoldTheme::Error;
            }
            return ManifoldTheme::Muted;
        }

        const char* LevelGlyph(Diagnostics::Level level)
        {
            switch (level)
            {
            case Diagnostics::Level::Ok: return ICON_FA_CHECK_CIRCLE;
            case Diagnostics::Level::Info: return ICON_FA_INFO_CIRCLE;
            case Diagnostics::Level::Warning: return ICON_FA_EXCLAMATION_TRIANGLE;
            case Diagnostics::Level::Problem: return ICON_FA_TIMES_CIRCLE;
            }
            return "";
        }

        void OpenFolder(const std::filesystem::path& folder)
        {
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }

        const char* PriorityName(int priority)
        {
            switch (priority)
            {
            case THREAD_PRIORITY_IDLE: return "Idle";
            case THREAD_PRIORITY_LOWEST: return "Lowest";
            case THREAD_PRIORITY_BELOW_NORMAL: return "Below normal";
            case THREAD_PRIORITY_NORMAL: return "Normal";
            case THREAD_PRIORITY_ABOVE_NORMAL: return "Above normal";
            case THREAD_PRIORITY_HIGHEST: return "Highest";
            case THREAD_PRIORITY_TIME_CRITICAL: return "Time critical";
            default: return "Other";
            }
        }

        struct Tile
        {
            const char* Label;
            std::string Value;
            std::string Sub;
            ImVec4 Color;
            std::string Tooltip;
        };

        bool DrawTile(const char* id, const Tile& tile, float width, float height, bool clickable)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 max(pos.x + width, pos.y + height);
            ImGui::InvisibleButton(id, ImVec2(width, height));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = clickable && ImGui::IsItemClicked();
            if (!tile.Tooltip.empty())
                ItemTooltip(tile.Tooltip.c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float rounding = Px(8.0f);
            draw->AddRectFilled(pos, max, ImGui::GetColorU32(ManifoldTheme::Card), rounding);
            draw->AddRect(pos, max, ImGui::GetColorU32(hovered && clickable ? ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.45f) : ManifoldTheme::CardBorder), rounding);
            const float pad = std::round(Px(12.0f));
            const float line = ImGui::GetTextLineHeight();
            draw->AddText(ImVec2(pos.x + pad, pos.y + pad - Px(2.0f)), ImGui::GetColorU32(ManifoldTheme::Muted), tile.Label);
            ImFont* bold = Fonts::Bold();
            const float big = ImGui::GetFontSize() * 1.5f;
            const ImVec2 clipMin(pos.x + pad, pos.y);
            const ImVec4 clip(clipMin.x, clipMin.y, max.x - pad, max.y);
            draw->AddText(bold, big, ImVec2(pos.x + pad, pos.y + pad + line), ImGui::GetColorU32(tile.Color), tile.Value.c_str(), nullptr, 0.0f, &clip);
            draw->AddText(nullptr, 0.0f, ImVec2(pos.x + pad, max.y - pad - line + Px(2.0f)), ImGui::GetColorU32(ManifoldTheme::Muted), tile.Sub.c_str(), nullptr, 0.0f, &clip);
            return clicked;
        }

        void Sparkline(ImDrawList* draw, ImVec2 min, ImVec2 max, const std::vector<float>& values, ImVec4 color, float top)
        {
            if (values.size() < 2 || top <= 0.0f)
                return;
            const int points = std::clamp(static_cast<int>(max.x - min.x), 2, static_cast<int>(values.size()));
            ImVec2 line[Profiler::kHistory];
            const float height = max.y - min.y;
            for (int i = 0; i < points; ++i)
            {
                const std::size_t from = static_cast<std::size_t>(i) * values.size() / static_cast<std::size_t>(points);
                const std::size_t to = std::max(from + 1, static_cast<std::size_t>(i + 1) * values.size() / static_cast<std::size_t>(points));
                float most = 0.0f;
                for (std::size_t at = from; at < to && at < values.size(); ++at)
                    most = std::max(most, values[at]);
                line[i] = ImVec2(min.x + (max.x - min.x) * static_cast<float>(i) / static_cast<float>(points - 1), max.y - height * std::min(most / top, 1.0f));
            }
            draw->AddPolyline(line, points, ImGui::GetColorU32(color), ImDrawFlags_None, Px(1.2f));
        }

        float LocalWrap(float screenX)
        {
            const ImGuiWindow* window = ImGui::GetCurrentWindow();
            return screenX - window->Pos.x + window->Scroll.x;
        }

        double NiceTop(double value)
        {
            const double steps[] = { 1.0, 2.0, 2.5, 5.0, 10.0 };
            const double magnitude = std::pow(10.0, std::floor(std::log10(std::max(value, 0.001))));
            for (const double step : steps)
            {
                if (step * magnitude >= value)
                    return step * magnitude;
            }
            return 10.0 * magnitude;
        }
    }

    std::vector<Diagnostics::InfoGroup> InterfaceInfo()
    {
        Diagnostics::InfoGroup group{ "Interface", {} };
        if (!ImGui::GetCurrentContext())
            return { group };
        const ImGuiIO& io = ImGui::GetIO();
        const ImGuiContext& context = *ImGui::GetCurrentContext();
        int shown = 0;
        for (const ImGuiWindow* window : context.Windows)
        {
            if (window->WasActive && !window->ParentWindow)
                ++shown;
        }
        group.Rows.push_back({ "Frame rate (ImGui)", Fixed(io.Framerate, 1) + " frames per second" });
        group.Rows.push_back({ "Windows", std::to_string(shown) + " shown, " + std::to_string(context.Windows.Size) + " known to Dear ImGui" });
        group.Rows.push_back({ "OS windows", std::to_string(ImGui::GetPlatformIO().Viewports.Size) });
        if (io.Fonts && io.Fonts->TexData)
            group.Rows.push_back({ "Font atlas", std::to_string(io.Fonts->TexData->Width) + " x " + std::to_string(io.Fonts->TexData->Height) + ", " + std::to_string(io.Fonts->Fonts.Size) + " fonts" });
        group.Rows.push_back({ "Interface scale", std::to_string(static_cast<int>(std::lround(Scale() * 100.0f))) + " %" });
        group.Rows.push_back({ "Layout file", io.IniFilename ? io.IniFilename : "none" });
        return { group };
    }

    std::string DiagnosticsReport(Workspace& workspace)
    {
        return Diagnostics::Report(workspace.Core(), InterfaceInfo());
    }

    bool FrameReadout(std::string& text)
    {
        static double at = -100.0;
        static std::string cached;
        if (!Profiler::Enabled() || Profiler::Frames() < 2)
            return false;
        const double now = ImGui::GetTime();
        if (now - at > 0.5 || cached.empty())
        {
            at = now;
            const Profiler::FrameSummary summary = Profiler::Summary();
            cached = ICON_FA_TACHOMETER_ALT "  " + Diagnostics::Milliseconds(summary.WorkMs) + " per frame  ·  " + Fixed(summary.FramesPerSecond, 0) + " fps";
        }
        text = cached;
        return true;
    }

    ProfilerWindow::ProfilerWindow(Workspace& workspace) : m_workspace(workspace)
    {
    }

    void ProfilerWindow::Show(int tab)
    {
        m_open = true;
        m_bringToFront = true;
        if (tab >= 0 && tab < TabCount)
            m_requestedTab = tab;
    }

    int ProfilerWindow::SelectedAgo() const
    {
        if (!m_pinned)
            return -1;
        const std::uint64_t count = Profiler::FrameCount();
        if (count == 0 || m_pinnedFrame >= count)
            return -1;
        const std::uint64_t ago = count - 1 - m_pinnedFrame;
        return ago < static_cast<std::uint64_t>(Profiler::Frames()) ? static_cast<int>(ago) : -1;
    }

    ImVec4 ProfilerWindow::ZoneColor(int node) const
    {
        if (Profiler::NodeWaits(node))
            return ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.55f);
        const int top = node >= 0 && node < static_cast<int>(m_topOf.size()) ? m_topOf[static_cast<std::size_t>(node)] : -1;
        if (top < 0)
            return ManifoldTheme::Text;
        if (Profiler::NodeWaits(top))
            return ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.55f);
        const auto found = std::find(m_topLevel.begin(), m_topLevel.end(), top);
        return Palette(static_cast<std::size_t>(found - m_topLevel.begin()));
    }

    void ProfilerWindow::RefreshTree()
    {
        m_tree = Profiler::Tree();
        m_topLevel = Profiler::TopLevel();
        int most = 0;
        for (const Profiler::ZoneView& zone : m_tree)
            most = std::max(most, zone.Node);
        m_topOf.assign(static_cast<std::size_t>(most + 1), -1);
        for (const Profiler::ZoneView& zone : m_tree)
        {
            if (zone.Depth == 1)
                m_topOf[static_cast<std::size_t>(zone.Node)] = zone.Node;
            else if (zone.Depth > 1 && zone.Parent >= 0)
                m_topOf[static_cast<std::size_t>(zone.Node)] = m_topOf[static_cast<std::size_t>(zone.Parent)];
        }
    }

    void ProfilerWindow::RefreshChecks(bool force)
    {
        const double now = ImGui::GetTime();
        if (!force && !m_checks.empty() && now - m_checkedAt < 10.0)
            return;
        m_checkedAt = now;
        m_checks = Diagnostics::RunChecks(m_workspace.Core());
    }

    void ProfilerWindow::RunAction(Diagnostics::Action action)
    {
        switch (action)
        {
        case Diagnostics::Action::None: break;
        case Diagnostics::Action::RestartElevated: m_workspace.Window().RestartElevated(); break;
        case Diagnostics::Action::OpenCrashFolder: OpenFolder(Paths::CrashFolder()); break;
        case Diagnostics::Action::OpenSettings: m_workspace.OpenSettings(); break;
        case Diagnostics::Action::OpenLog: m_workspace.OpenLog(); break;
        case Diagnostics::Action::TurnOnProfiler:
        {
            Settings& settings = m_workspace.Core().Options();
            settings.ProfilerEnabled = true;
            Profiler::SetEnabled(true);
            m_workspace.Report(settings.Save(Paths::SettingsFile()));
            m_checkedAt = -100.0;
            break;
        }
        }
    }

    void ProfilerWindow::SaveReport()
    {
        const auto path = m_workspace.SaveFileDialog({ { L"Text files (*.txt)", L"*.txt" }, { L"Markdown (*.md)", L"*.md" }, { L"All files", L"*.*" } }, "Manifold-Engine diagnostics.txt", L"txt");
        if (!path)
            return;
        const std::string report = DiagnosticsReport(m_workspace);
        std::ofstream file(*path, std::ios::binary | std::ios::trunc);
        file.write(report.data(), static_cast<std::streamsize>(report.size()));
        file.close();
        if (!file)
        {
            m_workspace.Report(Status::Fail(ErrorCode::ReportNotSaved, WideToUtf8(path->wstring())));
            return;
        }
        m_workspace.Core().Notify(LogLevel::Success, "Saved the diagnostics report to " + WideToUtf8(path->filename().wstring()));
    }

    void ProfilerWindow::Draw()
    {
        if (m_bringToFront)
        {
            ImGui::SetNextWindowFocus();
            m_bringToFront = false;
        }
        if (!BeginToolWindow(ICON_FA_TACHOMETER_ALT "  Profiler and diagnostics###profiler", &m_open, ImVec2(940.0f, 660.0f), 0, ImVec2(640.0f, 440.0f)))
        {
            EndToolWindow();
            return;
        }
        const double now = ImGui::GetTime();
        if (now - m_sampledAt > 1.0)
        {
            PROFILE_ZONE("Profiler: process sample");
            m_sampledAt = now;
            const bool threads = m_tab == ThreadsTab;
            Diagnostics::Sample(threads);
            if (threads)
                m_threadsAt = now;
        }
        {
            PROFILE_ZONE("Profiler: zone tree");
            RefreshTree();
        }
        {
            PROFILE_ZONE("Profiler: checks");
            RefreshChecks(false);
        }
        if (m_pinned && SelectedAgo() < 0)
            m_pinned = false;

        Toolbar();
        int issues = 0;
        for (const Diagnostics::Check& check : m_checks)
            issues += check.Severity == Diagnostics::Level::Warning || check.Severity == Diagnostics::Level::Problem;
        if (ImGui::BeginTabBar("##profilertabs"))
        {
            for (int tab = 0; tab < TabCount; ++tab)
            {
                std::string label = kTabNames[tab];
                if (tab == ChecksTab && issues > 0)
                    label += "  " + std::to_string(issues);
                label += "###profilertab" + std::to_string(tab);
                const ImGuiTabItemFlags flags = m_requestedTab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (!ImGui::BeginTabItem(label.c_str(), nullptr, flags))
                    continue;
                if (m_tab != tab && tab == ThreadsTab)
                    m_threadsAt = -100.0;
                m_tab = tab;
                ImGui::BeginChild("##profilerbody", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, tab == BreakdownTab || tab == ThreadsTab ? ImGuiWindowFlags_NoScrollbar : 0);
                switch (tab)
                {
                case OverviewTab: Overview(); break;
                case BreakdownTab: Breakdown(); break;
                case ThreadsTab: ThreadList(); break;
                case ChecksTab: ChecksList(); break;
                default: AppInfo(); break;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            m_requestedTab = -1;
            ImGui::EndTabBar();
        }
        EndToolWindow();
    }

    void ProfilerWindow::Toolbar()
    {
        const bool on = Profiler::Enabled();
        const bool paused = Profiler::Paused();
        ImGui::AlignTextToFramePadding();
        if (on)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, paused ? ManifoldTheme::Warning : ManifoldTheme::Accent);
            ImGui::TextUnformatted(paused ? ICON_FA_PAUSE : ICON_FA_HEARTBEAT);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::TextUnformatted(paused ? "Paused" : "Measuring");
            ImGui::SameLine(0.0f, Px(10.0f));
            Muted(std::to_string(Profiler::Frames()) + " of " + std::to_string(Profiler::kHistory) + " frames  ·  " + std::to_string(Profiler::FrameCount()) + " in total");
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
            ImGui::TextUnformatted(ICON_FA_PAUSE "  The profiler is off");
            ImGui::PopStyleColor();
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        const char* pauseLabel = paused ? ICON_FA_PLAY "  Resume" : ICON_FA_PAUSE "  Pause";
        const char* resetLabel = ICON_FA_UNDO "  Reset";
        const bool copied = ImGui::GetTime() - m_copiedAt < 1.5;
        const char* copyLabel = copied ? ICON_FA_CHECK_CIRCLE "  Copied###copyreport" : ICON_FA_COPY "  Copy report###copyreport";
        const char* saveLabel = ICON_FA_SAVE "  Save report...";
        const auto width = [&](const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f; };
        const float total = width(pauseLabel) + width(resetLabel) + width(ICON_FA_CHECK_CIRCLE "  Copy report") + width(saveLabel) + style.ItemSpacing.x * 3.0f + Px(10.0f);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + Px(10.0f), ImGui::GetWindowContentRegionMax().x - total));
        ImGui::BeginDisabled(!on);
        if (ImGui::Button(pauseLabel))
            Profiler::SetPaused(!paused);
        ItemTooltip("Freeze the measured frames to look at them; new frames are not recorded meanwhile");
        ImGui::SameLine();
        if (ImGui::Button(resetLabel))
        {
            Profiler::Reset();
            m_pinned = false;
            m_graphTop = {};
            m_sparkTops.clear();
        }
        ItemTooltip("Forget the measured frames and start again");
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, style.ItemSpacing.x + Px(10.0f));
        if (ImGui::Button(copyLabel))
        {
            ImGui::SetClipboardText(DiagnosticsReport(m_workspace).c_str());
            m_copiedAt = ImGui::GetTime();
        }
        ItemTooltip("Copy checks, app info, frame times and threads as text, for a bug report");
        ImGui::SameLine();
        if (ImGui::Button(saveLabel))
            SaveReport();
        ImGui::Spacing();
    }

    bool ProfilerWindow::ProfilerOffCard()
    {
        std::vector<EmptyAction> actions = { { ICON_FA_TACHOMETER_ALT "  Turn on the profiler", nullptr, true } };
        const int clicked = EmptyState(ICON_FA_STOPWATCH, "The profiler is off",
            "Turned on, it measures every frame: the engine, Lua, every window, drawing and waiting, so slow parts and stutters show up. It costs a few microseconds per frame. "
            "Checks, threads and app info below work without it.",
            actions, true, "Settings > General > Diagnostics turns it on and off as well");
        if (clicked == 0)
        {
            RunAction(Diagnostics::Action::TurnOnProfiler);
            return true;
        }
        return false;
    }

    void ProfilerWindow::Tiles()
    {
        const bool on = Profiler::Enabled();
        const Profiler::FrameSummary frames = Profiler::Summary();
        const Diagnostics::ProcessSample& sample = Diagnostics::LastSample();
        double drawCalls = 0.0;
        double vertices = 0.0;
        double windows = 0.0;
        for (const Profiler::CounterView& counter : Profiler::Counters())
        {
            if (std::string_view(counter.Name) == "Draw calls")
                drawCalls = counter.Last;
            else if (std::string_view(counter.Name) == "Vertices")
                vertices = counter.Last;
            else if (std::string_view(counter.Name) == "Viewports drawn")
                windows = counter.Last;
        }
        int warnings = 0;
        int problems = 0;
        for (const Diagnostics::Check& check : m_checks)
        {
            warnings += check.Severity == Diagnostics::Level::Warning;
            problems += check.Severity == Diagnostics::Level::Problem;
        }
        const bool measured = on && frames.Frames > 1;
        std::vector<Tile> tiles;
        tiles.push_back({ "Work per frame", measured ? Diagnostics::Milliseconds(frames.WorkMs) : std::string("-"),
            measured ? "max " + Diagnostics::Milliseconds(frames.WorkMax) : std::string("profiler off"), measured && frames.WorkP95 > 16.0 ? ManifoldTheme::Warning : ManifoldTheme::Text,
            "Time the program spends on a frame, without waiting for input or the display." +
                (measured ? "\nMedian " + Diagnostics::Milliseconds(frames.WorkP50) + ", 95 % under " + Diagnostics::Milliseconds(frames.WorkP95) + ", 99 % under " + Diagnostics::Milliseconds(frames.WorkP99) + "." : std::string()) });
        tiles.push_back({ "Frame rate", measured ? Fixed(frames.FramesPerSecond, frames.FramesPerSecond < 20.0 ? 1 : 0) + " fps" : std::string("-"),
            measured ? (frames.Hitches ? std::to_string(frames.Hitches) + (frames.Hitches == 1 ? " stutter" : " stutters") : std::string("no stutters")) : std::string("profiler off"),
            measured && frames.Hitches ? ManifoldTheme::Warning : ManifoldTheme::Text,
            "About 10 frames per second while nothing happens, up to the display's refresh rate while you work.\nA stutter is a frame with more than 50 ms of work." });
        tiles.push_back({ "Processor", sample.Valid ? Fixed(sample.CpuOfOneCore, sample.CpuOfOneCore < 10.0 ? 1 : 0) + " %" : std::string("-"),
            sample.Valid ? std::string("of one core") : std::string("measuring..."), sample.Valid && sample.CpuOfOneCore > 30.0 ? ManifoldTheme::Warning : ManifoldTheme::Text,
            "Processor time of the whole program over the last second; 100 % is one full core." + (sample.Valid ? "\n" + Fixed(sample.CpuOfSystem, 1) + " % of all processor threads." : std::string()) });
        tiles.push_back({ "Memory", Diagnostics::Bytes(sample.PrivateBytes), "peak " + Diagnostics::Bytes(sample.PeakPrivateBytes), ManifoldTheme::Text,
            "Private memory of the program; about 55 MB of it is the graphics driver.\nWorking set " + Diagnostics::Bytes(sample.WorkingSet) + ", " + std::to_string(sample.Threads) + " threads, " +
                std::to_string(sample.Handles) + " handles." });
        tiles.push_back({ "Drawing", measured ? Fixed(drawCalls, 0) + " calls" : std::string("-"), measured ? Fixed(vertices / 1000.0, 1) + "k vertices" : std::string("profiler off"), ManifoldTheme::Text,
            "Draw calls and vertices of the last frame, over all " + Fixed(windows, 0) + " OS windows that were drawn." });
        tiles.push_back({ "Checks", problems ? std::to_string(problems) + (problems == 1 ? " problem" : " problems") : warnings ? std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings") : std::string("All fine"),
            problems && warnings ? std::to_string(warnings) + (warnings == 1 ? " warning too" : " warnings too") : std::to_string(m_checks.size()) + " checks",
            problems ? ManifoldTheme::Error : warnings ? ManifoldTheme::Warning : ManifoldTheme::Accent, "Checks of the setup and the environment. Click to see them." });

        const float available = ImGui::GetContentRegionAvail().x;
        const float gap = std::round(Px(8.0f));
        const int columns = std::clamp(static_cast<int>((available + gap) / (Px(140.0f) + gap)), 2, 6);
        const float width = std::floor((available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
        const float height = std::round(Px(80.0f));
        for (std::size_t i = 0; i < tiles.size(); ++i)
        {
            if (i % static_cast<std::size_t>(columns) != 0)
                ImGui::SameLine(0.0f, gap);
            else if (i > 0)
                ImGui::Dummy(ImVec2(0.0f, gap - ImGui::GetStyle().ItemSpacing.y));
            const bool checks = i + 1 == tiles.size();
            if (DrawTile(("##tile" + std::to_string(i)).c_str(), tiles[i], width, height, checks) && checks)
                m_requestedTab = ChecksTab;
        }
    }

    void ProfilerWindow::Overview()
    {
        const bool on = Profiler::Enabled();
        if (!on && ProfilerOffCard())
            return;
        if (!on)
            ImGui::Spacing();
        Tiles();
        if (!on)
            return;
        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        ImGui::TextUnformatted("Frames");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, Px(10.0f));
        Muted(m_wholeFrame ? "the whole frame period, including waiting" : "work per frame, without waiting for input and the display");
        const float choice = std::round(Px(210.0f));
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - choice));
        int mode = m_wholeFrame ? 1 : 0;
        if (Segmented("##graphmode", &mode, { "Work", "Whole frame" }, choice))
            m_wholeFrame = mode == 1;
        const float legend = ImGui::GetTextLineHeightWithSpacing() * 2.0f + Px(16.0f);
        FrameGraph(std::max(Px(150.0f), ImGui::GetContentRegionAvail().y - legend));
        Legend();
    }

    double ProfilerWindow::EasedTop::Follow(double needed, double now)
    {
        const double elapsed = now - SeenAt;
        SeenAt = now;
        if (Shown <= 0.0 || elapsed < 0.0 || elapsed > 0.5)
        {
            Shown = Target = Previous = needed;
            ChangedAt = -100.0;
            LowerSince = -1.0;
            return Shown;
        }
        const auto retarget = [&]() {
            if (Fade(now) >= 0.5f)
                Previous = Target;
            Target = needed;
            ChangedAt = now;
            LowerSince = -1.0;
        };
        if (needed > Target)
            retarget();
        else if (needed < Target)
        {
            if (LowerSince < 0.0)
                LowerSince = now;
            else if (now - LowerSince >= 0.5)
                retarget();
        }
        else
            LowerSince = -1.0;
        const double rate = Target > Shown ? 12.0 : 5.0;
        Shown *= std::exp(std::log(Target / Shown) * (1.0 - std::exp(-rate * elapsed)));
        if (std::abs(std::log(Target / Shown)) < 0.002)
            Shown = Target;
        return Shown;
    }

    float ProfilerWindow::EasedTop::Fade(double now) const
    {
        return static_cast<float>(std::clamp((now - ChangedAt) / 0.3, 0.0, 1.0));
    }

    void ProfilerWindow::FrameGraph(float height)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 max(pos.x + width, pos.y + height);
        ImGui::InvisibleButton("##framegraph", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float rounding = Px(8.0f);
        draw->AddRectFilled(pos, max, ImGui::GetColorU32(ManifoldTheme::Card), rounding);
        draw->AddRect(pos, max, ImGui::GetColorU32(ManifoldTheme::CardBorder), rounding);

        const int frames = Profiler::Frames();
        if (frames == 0)
        {
            const char* text = Profiler::Paused() ? "Paused before the first frame." : "Waiting for the first frames...";
            const ImVec2 size = ImGui::CalcTextSize(text);
            draw->AddText(ImVec2(pos.x + (width - size.x) * 0.5f, pos.y + (height - size.y) * 0.5f), ImGui::GetColorU32(ManifoldTheme::Muted), text);
            return;
        }
        m_scratch.resize(static_cast<std::size_t>(frames));
        for (int ago = 0; ago < frames; ++ago)
            m_scratch[static_cast<std::size_t>(ago)] = static_cast<float>(m_wholeFrame ? Profiler::FrameMs(ago) : Profiler::WorkMs(ago));
        std::vector<float> sorted = m_scratch;
        std::sort(sorted.begin(), sorted.end());
        const double p97 = sorted[static_cast<std::size_t>(std::min<double>(sorted.size() - 1, 0.97 * static_cast<double>(sorted.size() - 1) + 0.5))];
        const double now = ImGui::GetTime();
        const double top = m_graphTop.Follow(NiceTop(std::max(p97 * 1.2, m_wholeFrame ? 20.0 : 2.0)), now);
        const float fade = m_graphTop.Fade(now);
        const bool crossfade = fade < 1.0f && m_graphTop.Previous != m_graphTop.Target;

        const float line = ImGui::GetTextLineHeight();
        const float left = std::round(pos.x + Px(54.0f));
        const float right = std::round(max.x - Px(10.0f));
        const float upper = std::round(pos.y + Px(12.0f));
        const float bottom = std::round(max.y - Px(10.0f));
        const float span = bottom - upper;
        const auto yOf = [&](double ms) { return bottom - static_cast<float>(std::min(ms, top) / top) * span; };

        const auto onTick = [](double ms, double ofTop) {
            const double at = ms / (ofTop / 4.0);
            return at <= 4.0001 && std::abs(at - std::round(at)) < 0.0001;
        };
        const auto ticks = [&](double ofTop, double otherTop, float alpha, bool owner) {
            for (int step = 0; step <= 4; ++step)
            {
                const double ms = ofTop * step / 4.0;
                if (ms > top * 1.0005)
                    break;
                float shown = alpha;
                if (crossfade && onTick(ms, otherTop))
                {
                    if (!owner)
                        continue;
                    shown = 1.0f;
                }
                if (shown <= 0.01f)
                    continue;
                const float y = yOf(ms);
                draw->AddLine(ImVec2(left, y), ImVec2(right, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.08f * shown)), Px(1.0f));
                const std::string text = Diagnostics::Milliseconds(ms);
                const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                draw->AddText(ImVec2(left - Px(8.0f) - size.x, y - line * 0.5f), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, ManifoldTheme::Muted.w * shown)), text.c_str());
            }
        };
        ticks(m_graphTop.Target, m_graphTop.Previous, crossfade ? fade : 1.0f, true);
        if (crossfade)
            ticks(m_graphTop.Previous, m_graphTop.Target, 1.0f - fade, false);
        const struct
        {
            double Ms;
            const char* Label;
        } targets[] = { { 1000.0 / 60.0, "60 fps" }, { 1000.0 / 30.0, "30 fps" }, { 100.0, "10 fps" } };
        for (const auto& target : targets)
        {
            if (target.Ms >= top)
                continue;
            const float y = yOf(target.Ms);
            const float edge = std::clamp((y - upper) / Px(14.0f), 0.0f, 1.0f);
            for (float x = left; x < right; x += Px(8.0f))
                draw->AddLine(ImVec2(x, y), ImVec2(std::min(right, x + Px(4.0f)), y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.45f * edge)), Px(1.0f));
            draw->AddText(ImVec2(right - ImGui::CalcTextSize(target.Label).x - Px(4.0f), y - line - Px(1.0f)), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Warning, 0.8f * edge)), target.Label);
        }

        const float slot = (right - left) / static_cast<float>(Profiler::kHistory);
        const float gap = slot >= 3.0f ? Px(1.0f) : 0.0f;
        const int selected = SelectedAgo();
        int hoveredAgo = -1;
        if (hovered)
        {
            const float mouse = ImGui::GetIO().MousePos.x;
            if (mouse >= left && mouse <= right)
            {
                const int ago = static_cast<int>((right - mouse) / slot);
                if (ago >= 0 && ago < frames)
                    hoveredAgo = ago;
            }
        }
        draw->PushClipRect(ImVec2(left, upper - Px(2.0f)), ImVec2(right, bottom + Px(1.0f)), true);
        for (int ago = 0; ago < frames; ++ago)
        {
            const float x1 = right - static_cast<float>(ago) * slot;
            const float x0 = x1 - slot + gap;
            if (ago == selected || ago == hoveredAgo)
                draw->AddRectFilled(ImVec2(x0 - gap, upper - Px(2.0f)), ImVec2(x1, bottom), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ago == selected ? ManifoldTheme::Accent : ManifoldTheme::Text, 0.10f)));
            float y = bottom;
            double sum = 0.0;
            for (const int node : m_topLevel)
            {
                if (!m_wholeFrame && Profiler::NodeWaits(node))
                    continue;
                const double ms = Profiler::ZoneMs(node, ago);
                if (ms <= 0.0)
                    continue;
                const float y0 = std::max(upper, yOf(sum + ms));
                if (y0 < y)
                    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y), ImGui::GetColorU32(ZoneColor(node)));
                y = y0;
                sum += ms;
            }
            const double total = m_scratch[static_cast<std::size_t>(ago)];
            if (total > sum)
            {
                const float y0 = std::max(upper, yOf(total));
                if (y0 < y)
                    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.35f)));
            }
            if (total > top)
                draw->AddRectFilled(ImVec2(x0, upper - Px(2.0f)), ImVec2(x1, upper + Px(1.0f)), ImGui::GetColorU32(ManifoldTheme::Error));
        }
        draw->PopClipRect();

        if (hoveredAgo >= 0)
        {
            const std::uint64_t number = Profiler::FrameCount() - 1 - static_cast<std::uint64_t>(hoveredAgo);
            ShowTooltip([&]() {
                ImGui::PushFont(Fonts::Bold(), 0.0f);
                ImGui::Text("Frame %llu", static_cast<unsigned long long>(number));
                ImGui::PopFont();
                Muted("Work " + Diagnostics::Milliseconds(Profiler::WorkMs(hoveredAgo)) + "  ·  whole frame " + Diagnostics::Milliseconds(Profiler::FrameMs(hoveredAgo)));
                ImGui::Separator();
                for (const int node : m_topLevel)
                {
                    const double ms = Profiler::ZoneMs(node, hoveredAgo);
                    if (ms < 0.005)
                        continue;
                    ImGui::PushStyleColor(ImGuiCol_Text, ZoneColor(node));
                    ImGui::TextUnformatted(ICON_FA_SQUARE);
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextUnformatted(Profiler::NodeName(node));
                    ImGui::SameLine(Px(260.0f));
                    ImGui::TextUnformatted(Diagnostics::Milliseconds(ms).c_str());
                }
                ImGui::Spacing();
                Muted("Click to pin this frame, double-click to break it down");
            });
            if (clicked)
            {
                m_pinned = true;
                m_pinnedFrame = number;
            }
            if (doubleClicked)
            {
                m_pinned = true;
                m_pinnedFrame = number;
                m_requestedTab = BreakdownTab;
            }
        }
    }

    void ProfilerWindow::Legend()
    {
        const int ago = SelectedAgo();
        ImGui::Dummy(ImVec2(0.0f, Px(4.0f)));
        if (ago >= 0)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
            ImGui::TextUnformatted(ICON_FA_THUMBTACK);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("Frame %llu: work %s, whole %s", static_cast<unsigned long long>(m_pinnedFrame), Diagnostics::Milliseconds(Profiler::WorkMs(ago)).c_str(),
                Diagnostics::Milliseconds(Profiler::FrameMs(ago)).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Break it down"))
                m_requestedTab = BreakdownTab;
            ImGui::SameLine();
            if (ImGui::SmallButton("Unpin"))
                m_pinned = false;
        }
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        float x = ImGui::GetCursorScreenPos().x;
        float y = ImGui::GetCursorScreenPos().y;
        const float start = x;
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const auto entry = [&](ImVec4 color, const std::string& text) {
            const float dot = std::round(Px(9.0f));
            const float width = dot + Px(6.0f) + ImGui::CalcTextSize(text.c_str()).x + Px(16.0f);
            if (x > start && x + width > right)
            {
                x = start;
                y += lineHeight;
            }
            const float middle = y + ImGui::GetTextLineHeight() * 0.5f;
            draw->AddRectFilled(ImVec2(x, middle - dot * 0.5f), ImVec2(x + dot, middle + dot * 0.5f), ImGui::GetColorU32(color), Px(2.0f));
            draw->AddText(ImVec2(x + dot + Px(6.0f), y), ImGui::GetColorU32(ManifoldTheme::Label), text.c_str());
            x += width;
        };
        for (const int node : m_topLevel)
        {
            if (!m_wholeFrame && Profiler::NodeWaits(node))
                continue;
            double ms = 0.0;
            double most = 0.0;
            for (const Profiler::ZoneView& zone : m_tree)
            {
                if (zone.Node == node)
                {
                    ms = zone.AverageMs;
                    most = zone.MaxMs;
                }
            }
            if (most <= 0.0)
                continue;
            if (ago >= 0)
                ms = Profiler::ZoneMs(node, ago);
            entry(ZoneColor(node), std::string(Profiler::NodeName(node)) + "  " + Diagnostics::Milliseconds(ms));
        }
        entry(ManifoldTheme::WithAlpha(ManifoldTheme::Muted, 0.35f), "Between zones");
        ImGui::SetCursorScreenPos(ImVec2(start, y));
        ImGui::Dummy(ImVec2(right - start, lineHeight));
    }

    void ProfilerWindow::Breakdown()
    {
        if (!Profiler::Enabled() && ProfilerOffCard())
            return;
        if (!Profiler::Enabled())
            return;
        const int ago = SelectedAgo();
        const Profiler::FrameSummary summary = Profiler::Summary();
        ImGui::AlignTextToFramePadding();
        if (ago >= 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
            ImGui::TextUnformatted(ICON_FA_THUMBTACK);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("Frame %llu: work %s, whole frame %s", static_cast<unsigned long long>(m_pinnedFrame), Diagnostics::Milliseconds(Profiler::WorkMs(ago)).c_str(),
                Diagnostics::Milliseconds(Profiler::FrameMs(ago)).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Back to the average"))
                m_pinned = false;
        }
        else
        {
            Muted("Average of the last " + std::to_string(summary.Frames) + " frames. Click a bar in the frame graph to look at a single frame.");
        }
        ImGui::SameLine();
        const char* tinyLabel = "Hide zones under 10 us";
        const float tinyWidth = ImGui::CalcTextSize(tinyLabel).x + ToggleSwitchWidth() + Px(10.0f);
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + Px(10.0f), ImGui::GetWindowContentRegionMax().x - tinyWidth));
        ImGui::TextUnformatted(tinyLabel);
        ImGui::SameLine(0.0f, Px(8.0f));
        ToggleSwitch("##hidetiny", &m_hideTiny);

        std::vector<double> value(m_topOf.size(), 0.0);
        std::vector<double> children(m_topOf.size(), 0.0);
        std::vector<double> calls(m_topOf.size(), 0.0);
        for (const Profiler::ZoneView& zone : m_tree)
        {
            const std::size_t node = static_cast<std::size_t>(zone.Node);
            value[node] = ago >= 0 ? Profiler::ZoneMs(zone.Node, ago) : zone.AverageMs;
            calls[node] = ago >= 0 ? Profiler::ZoneCalls(zone.Node, ago) : zone.CallsPerFrame;
        }
        for (const Profiler::ZoneView& zone : m_tree)
        {
            if (zone.Parent >= 0)
                children[static_cast<std::size_t>(zone.Parent)] += value[static_cast<std::size_t>(zone.Node)];
        }
        const double work = ago >= 0 ? Profiler::WorkMs(ago) : summary.WorkMs;

        const std::vector<Profiler::CounterView> counters = Profiler::Counters();
        const float countersHeight = counters.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * (static_cast<float>(counters.size()) + 1.0f) + Px(26.0f);
        const float tableHeight = std::max(Px(160.0f), ImGui::GetContentRegionAvail().y - countersHeight);
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##zones", 7, flags, ImVec2(0.0f, tableHeight)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 3.0f);
            ImGui::TableSetupColumn(ago >= 0 ? "Time" : "Average", ImGuiTableColumnFlags_WidthFixed, Px(78.0f));
            ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed, Px(78.0f));
            ImGui::TableSetupColumn("Most", ImGuiTableColumnFlags_WidthFixed, Px(78.0f));
            ImGui::TableSetupColumn("Share of work", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, Px(54.0f));
            ImGui::TableSetupColumn("Last frames", ImGuiTableColumnFlags_WidthFixed, Px(150.0f));
            ImGui::TableHeadersRow();

            std::vector<int> open;
            int skipBelow = -1;
            for (const Profiler::ZoneView& zone : m_tree)
            {
                while (!open.empty() && open.back() >= zone.Depth)
                {
                    ImGui::TreePop();
                    open.pop_back();
                }
                if (skipBelow >= 0 && zone.Depth > skipBelow)
                    continue;
                skipBelow = -1;
                const std::size_t node = static_cast<std::size_t>(zone.Node);
                if (m_hideTiny && zone.Depth > 0 && zone.AverageMs < 0.01 && zone.MaxMs < 0.05 && value[node] < 0.01)
                {
                    if (zone.HasChildren)
                        skipBelow = zone.Depth;
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;
                if (!zone.HasChildren)
                    treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                if (zone.Depth <= 1)
                    treeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
                const ImVec4 color = zone.Depth == 0 ? ManifoldTheme::Text : ZoneColor(zone.Node);
                const float rowX = ImGui::GetCursorScreenPos().x;
                ImGui::PushStyleColor(ImGuiCol_Text, zone.Wait ? ManifoldTheme::Muted : ManifoldTheme::Text);
                const bool expanded = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::intptr_t>(zone.Node + 1)), treeFlags, "%s", zone.Name);
                ImGui::PopStyleColor();
                if (const char* help = HelpFor(zone.Name))
                    ItemTooltip(help);
                if (zone.Depth > 0)
                {
                    const ImVec2 min = ImGui::GetItemRectMin();
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(rowX - Px(6.0f), min.y + Px(5.0f)), ImVec2(rowX - Px(3.0f), ImGui::GetItemRectMax().y - Px(5.0f)), ImGui::GetColorU32(color), Px(1.0f));
                }
                if (zone.HasChildren)
                {
                    if (expanded)
                        open.push_back(zone.Depth);
                    else
                        skipBelow = zone.Depth;
                }
                const double own = std::max(0.0, value[node] - children[node]);
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(Diagnostics::Milliseconds(value[node]).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                if (zone.HasChildren)
                    Muted(Diagnostics::Milliseconds(own));
                else
                    Muted("-");
                ImGui::TableSetColumnIndex(3);
                ImGui::AlignTextToFramePadding();
                Muted(Diagnostics::Milliseconds(zone.MaxMs));
                ImGui::TableSetColumnIndex(4);
                ImGui::AlignTextToFramePadding();
                if (zone.Depth > 0 && !zone.Wait && !Profiler::NodeWaits(m_topOf[node] >= 0 ? m_topOf[node] : zone.Node) && work > 0.0)
                {
                    const double share = std::min(1.0, value[node] / work);
                    const ImVec2 cell = ImGui::GetCursorScreenPos();
                    const float barWidth = ImGui::GetContentRegionAvail().x;
                    const float barHeight = ImGui::GetFrameHeight() - Px(8.0f);
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    draw->AddRectFilled(ImVec2(cell.x, cell.y + Px(4.0f)), ImVec2(cell.x + barWidth, cell.y + Px(4.0f) + barHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.05f)), Px(3.0f));
                    draw->AddRectFilled(ImVec2(cell.x, cell.y + Px(4.0f)), ImVec2(cell.x + barWidth * static_cast<float>(share), cell.y + Px(4.0f) + barHeight), ImGui::GetColorU32(ManifoldTheme::WithAlpha(color, 0.45f)), Px(3.0f));
                    ImGui::SetCursorScreenPos(ImVec2(cell.x + Px(6.0f), cell.y));
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted((Fixed(share * 100.0, share < 0.1 ? 1 : 0) + " %").c_str());
                }
                else
                {
                    Muted(zone.Wait ? "waiting" : "");
                }
                ImGui::TableSetColumnIndex(5);
                ImGui::AlignTextToFramePadding();
                Muted(Fixed(calls[node], calls[node] == std::floor(calls[node]) ? 0 : 1));
                ImGui::TableSetColumnIndex(6);
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                const float sparkWidth = ImGui::GetContentRegionAvail().x;
                Profiler::History(zone.Node, m_scratch);
                float most = 0.0f;
                for (const float ms : m_scratch)
                    most = std::max(most, ms);
                if (node >= m_sparkTops.size())
                    m_sparkTops.resize(node + 1);
                EasedTop& eased = m_sparkTops[node];
                if (most > 0.0f)
                    Sparkline(ImGui::GetWindowDrawList(), ImVec2(cell.x, cell.y + Px(4.0f)), ImVec2(cell.x + sparkWidth, cell.y + ImGui::GetFrameHeight() - Px(4.0f)), m_scratch, color,
                        static_cast<float>(eased.Follow(most, ImGui::GetTime())));
                else
                    eased = {};
                ImGui::Dummy(ImVec2(sparkWidth, ImGui::GetFrameHeight()));
            }
            while (!open.empty())
            {
                ImGui::TreePop();
                open.pop_back();
            }
            ImGui::EndTable();
        }

        if (counters.empty())
            return;
        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));
        if (ImGui::BeginTable("##counters", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Last frame", ImGuiTableColumnFlags_WidthFixed, Px(100.0f));
            ImGui::TableSetupColumn("Average", ImGuiTableColumnFlags_WidthFixed, Px(100.0f));
            ImGui::TableSetupColumn("Most", ImGuiTableColumnFlags_WidthFixed, Px(100.0f));
            ImGui::TableHeadersRow();
            for (const Profiler::CounterView& counter : counters)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(counter.Name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(Fixed(counter.Last, 0).c_str());
                ImGui::TableSetColumnIndex(2);
                Muted(Fixed(counter.Average, 1));
                ImGui::TableSetColumnIndex(3);
                Muted(Fixed(counter.Max, 0));
            }
            ImGui::EndTable();
        }
    }

    void ProfilerWindow::ThreadList()
    {
        const double now = ImGui::GetTime();
        if (now - m_threadsAt > 1.0)
        {
            Diagnostics::Sample(true);
            m_threadsAt = now;
            m_sampledAt = now;
        }
        const std::vector<Diagnostics::ThreadSample>& threads = Diagnostics::LastThreads();
        Muted(std::to_string(threads.size()) + " threads. Processor use is measured over the last second; 100 % is one full core. Most threads belong to the graphics driver and Windows.");
        ImGui::Spacing();
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##threads", 6, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
            return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, Px(70.0f));
        ImGui::TableSetupColumn("Starts in", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Processor", ImGuiTableColumnFlags_WidthFixed, Px(130.0f));
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, Px(80.0f));
        ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, Px(96.0f));
        ImGui::TableHeadersRow();
        ClippedRows(static_cast<int>(threads.size()), [&](int index) {
            const Diagnostics::ThreadSample& thread = threads[static_cast<std::size_t>(index)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            PushMono();
            ImGui::PushStyleColor(ImGuiCol_Text, thread.Main ? ManifoldTheme::Accent : ManifoldTheme::AddressText);
            ImGui::Text("%lu", thread.Id);
            ImGui::PopStyleColor();
            PopMono();
            ImGui::TableSetColumnIndex(1);
            PushMono();
            ImGui::TextUnformatted(thread.Start.c_str());
            PopMono();
            ImGui::TableSetColumnIndex(2);
            Muted(thread.Description);
            ImGui::TableSetColumnIndex(3);
            const ImVec2 cell = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = ImGui::GetTextLineHeight();
            const double share = std::clamp(thread.CpuOfOneCore / 100.0, 0.0, 1.0);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(cell, ImVec2(cell.x + width, cell.y + height), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.05f)), Px(3.0f));
            if (share > 0.0)
                draw->AddRectFilled(cell, ImVec2(cell.x + std::max(Px(2.0f), width * static_cast<float>(share)), cell.y + height),
                    ImGui::GetColorU32(ManifoldTheme::WithAlpha(thread.CpuOfOneCore > 50.0 ? ManifoldTheme::Warning : ManifoldTheme::Accent, 0.5f)), Px(3.0f));
            ImGui::SetCursorScreenPos(ImVec2(cell.x + Px(6.0f), cell.y));
            ImGui::TextUnformatted((Fixed(thread.CpuOfOneCore, 1) + " %").c_str());
            ImGui::TableSetColumnIndex(4);
            Muted(Fixed(thread.CpuSeconds, 2) + " s");
            ImGui::TableSetColumnIndex(5);
            Muted(PriorityName(thread.Priority));
        });
        ImGui::EndTable();
    }

    void ProfilerWindow::ChecksList()
    {
        int counts[4] = {};
        for (const Diagnostics::Check& check : m_checks)
            ++counts[static_cast<int>(check.Severity)];
        ImGui::AlignTextToFramePadding();
        const Diagnostics::Level levels[] = { Diagnostics::Level::Problem, Diagnostics::Level::Warning, Diagnostics::Level::Info, Diagnostics::Level::Ok };
        const char* names[] = { "problems", "warnings", "notes", "fine" };
        const char* single[] = { "problem", "warning", "note", "fine" };
        for (int i = 0; i < 4; ++i)
        {
            if (i > 0)
                ImGui::SameLine(0.0f, Px(14.0f));
            const int count = counts[static_cast<int>(levels[i])];
            ImGui::PushStyleColor(ImGuiCol_Text, count ? LevelTint(levels[i]) : ManifoldTheme::Muted);
            ImGui::Text("%s  %d %s", LevelGlyph(levels[i]), count, count == 1 ? single[i] : names[i]);
            ImGui::PopStyleColor();
        }
        const char* againLabel = ICON_FA_SYNC "  Check again";
        const char* issuesLabel = "Only problems and warnings";
        const float widthNeeded = ImGui::CalcTextSize(againLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::CalcTextSize(issuesLabel).x + ToggleSwitchWidth() + Px(30.0f);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + Px(10.0f), ImGui::GetWindowContentRegionMax().x - widthNeeded));
        ImGui::TextUnformatted(issuesLabel);
        ImGui::SameLine(0.0f, Px(8.0f));
        ToggleSwitch("##onlyissues", &m_onlyIssues);
        ImGui::SameLine(0.0f, Px(14.0f));
        if (ImGui::Button(againLabel))
            RefreshChecks(true);
        ImGui::Dummy(ImVec2(0.0f, Px(6.0f)));

        const float width = ImGui::GetContentRegionAvail().x;
        const float pad = std::round(Px(14.0f));
        const float gap = std::round(Px(8.0f));
        Diagnostics::Action clicked = Diagnostics::Action::None;
        int shown = 0;
        for (std::size_t i = 0; i < m_checks.size(); ++i)
        {
            const Diagnostics::Check& check = m_checks[i];
            if (m_onlyIssues && check.Severity != Diagnostics::Level::Warning && check.Severity != Diagnostics::Level::Problem)
                continue;
            ++shown;
            ImGui::PushID(static_cast<int>(i));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            draw->ChannelsSplit(2);
            draw->ChannelsSetCurrent(1);
            const ImVec4 tint = LevelTint(check.Severity);
            ImGui::SetCursorScreenPos(ImVec2(start.x + pad + Px(4.0f), start.y + pad));
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, tint);
            ImGui::TextUnformatted(LevelGlyph(check.Severity));
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, Px(8.0f));
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(check.Title.c_str());
            ImGui::PopFont();
            ImGui::SameLine(0.0f, Px(10.0f));
            Muted(check.Area);
            ImGui::PushTextWrapPos(LocalWrap(start.x + width - pad));
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
            ImGui::TextUnformatted(check.Detail.c_str());
            ImGui::PopStyleColor();
            if (!check.Fix.empty() && check.Severity != Diagnostics::Level::Ok)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                ImGui::TextUnformatted((std::string(ICON_FA_WRENCH "  ") + check.Fix).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            const char* actionLabel = nullptr;
            switch (check.Act)
            {
            case Diagnostics::Action::RestartElevated: actionLabel = ICON_FA_SHIELD_ALT "  Restart as administrator"; break;
            case Diagnostics::Action::OpenCrashFolder: actionLabel = ICON_FA_FOLDER_OPEN "  Open the folder"; break;
            case Diagnostics::Action::OpenSettings: actionLabel = ICON_FA_COGS "  Open the settings"; break;
            case Diagnostics::Action::OpenLog: actionLabel = ICON_FA_STREAM "  Open the log"; break;
            case Diagnostics::Action::TurnOnProfiler: actionLabel = ICON_FA_TACHOMETER_ALT "  Turn on the profiler"; break;
            case Diagnostics::Action::None: break;
            }
            if (actionLabel)
            {
                ImGui::Dummy(ImVec2(0.0f, Px(2.0f)));
                if (ImGui::SmallButton(actionLabel))
                    clicked = check.Act;
            }
            ImGui::EndGroup();
            const float bottom = ImGui::GetItemRectMax().y + pad;
            draw->ChannelsSetCurrent(0);
            const float rounding = Px(8.0f);
            draw->AddRectFilled(start, ImVec2(start.x + width, bottom), ImGui::GetColorU32(ManifoldTheme::Card), rounding);
            draw->AddRect(start, ImVec2(start.x + width, bottom), ImGui::GetColorU32(ManifoldTheme::CardBorder), rounding);
            draw->AddRectFilled(ImVec2(start.x, start.y), ImVec2(start.x + Px(3.0f), bottom), ImGui::GetColorU32(ManifoldTheme::WithAlpha(tint, 0.8f)), rounding, ImDrawFlags_RoundCornersLeft);
            draw->ChannelsMerge();
            ImGui::SetCursorScreenPos(ImVec2(start.x, bottom + gap));
            ImGui::Dummy(ImVec2(width, 0.0f));
            ImGui::PopID();
        }
        if (shown == 0)
            EmptyState(ICON_FA_CHECK_CIRCLE, "Nothing to fix", "No check found a problem or a warning.", {}, true);
        if (clicked != Diagnostics::Action::None)
        {
            RunAction(clicked);
            RefreshChecks(true);
        }
    }

    void ProfilerWindow::AppInfo()
    {
        const double now = ImGui::GetTime();
        if (now - m_infoAt > 1.0 || m_info.empty())
        {
            m_infoAt = now;
            m_info = Diagnostics::CollectInfo(m_workspace.Core());
            for (Diagnostics::InfoGroup& group : InterfaceInfo())
                m_info.push_back(std::move(group));
        }
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = available >= Px(780.0f) ? 2 : 1;
        const float gap = std::round(Px(10.0f));
        const float cardWidth = std::floor((available - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
        const float pad = std::round(Px(12.0f));
        std::vector<float> columnY(static_cast<std::size_t>(columns), ImGui::GetCursorScreenPos().y);
        const float startX = ImGui::GetCursorScreenPos().x;
        for (std::size_t i = 0; i < m_info.size(); ++i)
        {
            const Diagnostics::InfoGroup& group = m_info[i];
            const std::size_t column = static_cast<std::size_t>(std::min_element(columnY.begin(), columnY.end()) - columnY.begin());
            ImGui::SetCursorScreenPos(ImVec2(startX + static_cast<float>(column) * (cardWidth + gap), columnY[column]));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::CardBorder);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
            ImGui::BeginChild(("##info" + std::to_string(i)).c_str(), ImVec2(cardWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(group.Title.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, Px(4.0f) - ImGui::GetStyle().ItemSpacing.y));
            if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, std::min(Px(150.0f), cardWidth * 0.36f));
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                for (const Diagnostics::InfoRow& row : group.Rows)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    Muted(row.Key);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(row.Value.c_str());
                    if (ImGui::IsItemHovered() && ImGui::CalcTextSize(row.Value.c_str()).x > ImGui::GetContentRegionAvail().x)
                        ItemTooltip(row.Value.c_str());
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    {
                        ImGui::SetClipboardText(row.Value.c_str());
                        m_workspace.Core().Notify(LogLevel::Info, "Copied " + row.Key);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            columnY[column] += ImGui::GetItemRectSize().y + gap;
        }
        const float bottom = *std::max_element(columnY.begin(), columnY.end());
        ImGui::SetCursorScreenPos(ImVec2(startX, bottom));
        Muted("Right-click a value to copy it.");
    }
}
