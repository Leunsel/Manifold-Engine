#include "UI/Windows.h"

#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>

#include <windows.h>
#include <shellapi.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr float kTitleHeight = 36.0f;
        constexpr float kProcessBarHeight = 40.0f;
        constexpr float kStatusHeight = 24.0f;
        constexpr const char* kRecordDrag = "MANIFOLD_RECORD";

        const Scan::Compare kCompares[] = { Scan::Compare::Exact, Scan::Compare::Bigger, Scan::Compare::Smaller, Scan::Compare::Between, Scan::Compare::Unknown,
            Scan::Compare::Increased, Scan::Compare::IncreasedBy, Scan::Compare::Decreased, Scan::Compare::DecreasedBy, Scan::Compare::Changed,
            Scan::Compare::Unchanged, Scan::Compare::SameAsFirst };

        Scan::Tristate ToTristate(int state)
        {
            return state == 1 ? Scan::Tristate::Yes : state == 0 ? Scan::Tristate::No : Scan::Tristate::Any;
        }

        std::string TypeText(const Table::MemoryRecord& record)
        {
            if (record.Kind == Table::RecordKind::Script)
                return "<script>";
            if (record.Kind == Table::RecordKind::Group)
                return "";
            if (record.Type == Scan::ValueType::String)
                return std::string(record.Unicode ? "Unicode[" : "String[") + std::to_string(record.Length) + "]";
            if (record.Type == Scan::ValueType::ByteArray)
                return "AOB[" + std::to_string(record.Length) + "]";
            if (record.Type == Scan::ValueType::Binary)
                return "Binary " + std::to_string(record.BitStart) + ":" + std::to_string(record.BitLength);
            return Scan::TypeName(record.Type);
        }

        std::size_t FindNoCase(std::string_view text, std::string_view part)
        {
            if (part.empty() || part.size() > text.size())
                return std::string_view::npos;
            const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            for (std::size_t i = 0; i + part.size() <= text.size(); ++i)
            {
                bool same = true;
                for (std::size_t j = 0; j < part.size() && same; ++j)
                    same = lower(text[i + j]) == lower(part[j]);
                if (same)
                    return i;
            }
            return std::string_view::npos;
        }

        int ArrowSteps(ImGuiInputTextCallbackData* data)
        {
            if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory)
                *static_cast<int*>(data->UserData) += data->EventKey == ImGuiKey_UpArrow ? -1 : 1;
            return 0;
        }

        int FindEntry(const std::vector<Table::DropDownEntry>& entries, std::string_view text)
        {
            const std::string_view wanted = TrimView(text);
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                if (EqualsNoCase(entries[i].Value, wanted))
                    return static_cast<int>(i);
            }
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                if (EqualsNoCase(entries[i].Text, wanted))
                    return static_cast<int>(i);
            }
            return -1;
        }

        bool CanToggleChildren(const Table::MemoryRecord& record)
        {
            return !record.Children.empty() && !(record.Options.HideChildren && !record.Active) && !record.Options.AlwaysHideChildren;
        }

        bool TableHandlesScriptEdit(Lua::LuaEngine& lua, Table::MemoryRecord& record)
        {
            if (!lua.AddressListEvent("onautoassembleredit", &record))
                return false;
            LOG_INFO("Table", "The table's Lua script handles editing of \"" + record.Description + "\", so the editor stays closed.");
            return true;
        }

        bool WindowButton(const char* id, const char* icon, float width, float height, bool danger)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (hovered)
                drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), danger ? IM_COL32(196, 43, 28, 255) : IM_COL32(255, 255, 255, 18));
            const ImVec2 size = ImGui::CalcTextSize(icon);
            drawList->AddText(ImVec2(std::round(pos.x + (width - size.x) * 0.5f), std::round(pos.y + (height - size.y) * 0.5f)),
                hovered ? IM_COL32(240, 240, 240, 255) : IM_COL32(150, 154, 160, 255), icon);
            return clicked;
        }
    }

    MainWindow::MainWindow(Workspace& workspace) : m_workspace(workspace), m_engine(workspace.Core())
    {
        const Settings& settings = m_engine.Options();
        m_fastScan = settings.FastScan;
        m_pause = settings.PauseWhileScanning;
        m_private = settings.ScanPrivate;
        m_image = settings.ScanImage;
        m_mapped = settings.ScanMapped;
    }

    void MainWindow::Draw()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float rounding = m_workspace.Window().CornerRadius();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ManifoldTheme::Page), rounding);
        drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + Px(kTitleHeight)), ImGui::GetColorU32(ManifoldTheme::Sidebar), rounding, ImDrawFlags_RoundCornersTop);
        drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ManifoldTheme::Border), rounding);

        TitleBar();
        ImGui::SetCursorPos(ImVec2(Px(10.0f), Px(kTitleHeight) + Px(8.0f)));
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, PxWhole(8.0f, 8.0f));
        const float width = size.x - Px(20.0f);
        ImGui::BeginChild("##body", ImVec2(width, size.y - Px(kTitleHeight) - Px(16.0f) - Px(kStatusHeight)), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        m_release = m_engine.Options().ReleaseMode;
        ProcessBar();
        if (m_engine.Options().CompactMode)
        {
            AddressListCard(ImGui::GetContentRegionAvail().y);
        }
        else
        {
            const float available = ImGui::GetContentRegionAvail().y;
            if (m_topHeight <= 0.0f)
                m_topHeight = std::floor(available * 0.52f);
            m_topHeight = std::clamp(m_topHeight, Px(200.0f), std::max(Px(200.0f), available - Px(140.0f)));
            float bottom = available - m_topHeight - Px(6.0f);
            Splitter("##split", false, Px(6.0f), &m_topHeight, &bottom, Px(200.0f), Px(120.0f));
            ScanArea(m_topHeight);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(2.0f));
            AddressListCard(ImGui::GetContentRegionAvail().y);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        StatusBar();

        ValueDialog();
        AddAddressDialog();
        DescriptionDialog();
        ImGui::End();
    }

    void MainWindow::TitleBar()
    {
        const float height = Px(kTitleHeight);
        const ImVec2 origin = ImGui::GetWindowPos();
        const float width = ImGui::GetWindowWidth();
        ImGui::SetCursorPos(ImVec2(Px(14.0f), 0.0f));
        ImGui::BeginGroup();
        const float textY = std::round((height - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::SetCursorPosY(textY);
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
        ImGui::TextUnformatted(ICON_FA_MICROCHIP);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, Px(8.0f));
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        ImGui::TextUnformatted("Manifold");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, Px(4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
        ImGui::TextUnformatted("Engine");
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, Px(18.0f));
        const float menuHeight = ImGui::GetFontSize() + Px(5.0f) * 2.0f;
        ImGui::SetCursorPosY(std::round((height - menuHeight) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Px(4.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(8.0f, 5.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        const float menusStart = ImGui::GetCursorPosX();
        ImGui::BeginChild("##menus", ImVec2(Px(440.0f), menuHeight), ImGuiChildFlags_None, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        float menusEnd = origin.x + menusStart;

        Engine& engine = m_engine;
        Workspace& workspace = m_workspace;
        const bool hasProcess = engine.HasProcess();
        ImGui::BeginMenuBar();
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem(ICON_FA_FILE "  New table", "Ctrl+N"))
                workspace.NewTable();
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open table...", "Ctrl+O"))
            {
                if (const auto path = workspace.OpenFileDialog({ { L"Cheat tables (*.CT)", L"*.CT;*.ct" }, { L"All files", L"*.*" } }, Paths::TablesFolder()))
                    workspace.OpenTable(*path, false);
            }
            if (!m_release && ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Merge table..."))
            {
                if (const auto path = workspace.OpenFileDialog({ { L"Cheat tables (*.CT)", L"*.CT;*.ct" } }, Paths::TablesFolder()))
                    workspace.OpenTable(*path, true);
            }
            if (ImGui::BeginMenu(ICON_FA_HISTORY "  Recent tables", !engine.Options().RecentTables.empty()))
            {
                const std::vector<std::string> recent = engine.Options().RecentTables;
                for (const std::string& path : recent)
                {
                    if (ImGui::MenuItem(path.c_str()))
                        workspace.OpenTable(Utf8ToWide(path), false);
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(ICON_FA_SAVE "  Save", "Ctrl+S"))
                workspace.SaveTable(false);
            if (ImGui::MenuItem(ICON_FA_SAVE "  Save as...", "Ctrl+Shift+S"))
                workspace.SaveTable(true);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_DESKTOP "  Open process...", "Ctrl+P"))
                workspace.OpenProcessPicker();
            if (ImGui::MenuItem(ICON_FA_TIMES "  Close process", nullptr, false, hasProcess))
                engine.CloseProcess();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COG "  Settings"))
                workspace.OpenSettings();
            if (ImGui::MenuItem(ICON_FA_SIGN_OUT_ALT "  Exit"))
                workspace.RequestQuit();
            ImGui::EndMenu();
        }
        if (!m_release && ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem(ICON_FA_UNDO "  Undo last scan", nullptr, false, engine.Scanner().CanUndo()))
                engine.Scanner().Undo();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy records", "Ctrl+C", false, !engine.Table().Selection().empty()))
                workspace.CopyRecords(engine.Table().Selection());
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste records", "Ctrl+V"))
                workspace.PasteRecords(0);
            if (ImGui::MenuItem(ICON_FA_CHECK_DOUBLE "  Select all records", "Ctrl+A"))
            {
                engine.Table().ClearSelection();
                engine.Table().ForEach([&](Table::MemoryRecord& record) { engine.Table().Select(record.Id, true); });
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Table"))
        {
            if (!m_release)
            {
                if (ImGui::MenuItem(ICON_FA_PLUS "  Add address manually"))
                {
                    m_addDialog = true;
                    m_addAddress.clear();
                    m_addDescription = "No description";
                }
                if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  Add group"))
                    engine.Table().AddGroup("Group");
                if (ImGui::MenuItem(ICON_FA_CODE "  Add Auto Assembler script"))
                {
                    Table::MemoryRecord& record = engine.Table().AddScript("Auto Assemble script", Asm::CheatTableTemplate());
                    workspace.OpenScript(record.Script, record.Id);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_SCROLL "  Table Lua script"))
                    workspace.OpenScript(engine.Table().TableLuaScript, -1, true);
                if (ImGui::MenuItem(ICON_FA_FILE_ARCHIVE "  Table files", nullptr, false, true))
                    workspace.OpenTableFiles();
            }
            if (ImGui::MenuItem(ICON_FA_TOGGLE_OFF "  Deactivate all records"))
                workspace.TurnOffEverything();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            Settings& options = engine.Options();
            if (ImGui::MenuItem(ICON_FA_COMPRESS_ALT "  Compact mode", nullptr, options.CompactMode))
            {
                options.CompactMode = !options.CompactMode;
                SaveOptions();
            }
            ItemTooltip("Only the address list, without the scanner");
            if (ImGui::MenuItem(ICON_FA_GAMEPAD "  Release mode", nullptr, options.ReleaseMode))
            {
                options.ReleaseMode = !options.ReleaseMode;
                SaveOptions();
            }
            ItemTooltip("Hides addresses, types and the developer tools, so a table is simple to use. Turn it off here again.");
            ImGui::EndMenu();
        }
        if (!m_release && ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem(ICON_FA_MEMORY "  Memory viewer", "Ctrl+M"))
                workspace.OpenMemoryViewer(0);
            if (ImGui::MenuItem(ICON_FA_CLONE "  New memory viewer window"))
                workspace.OpenMemoryViewer(0, false, true);
            if (ImGui::MenuItem(ICON_FA_SITEMAP "  Structure dissect"))
                workspace.OpenStructure(0);
            if (ImGui::MenuItem(ICON_FA_CODE "  Auto Assembler"))
                workspace.OpenScript(Asm::CheatTableTemplate());
            if (ImGui::MenuItem(ICON_FA_MOON "  Lua engine", "Ctrl+L"))
                workspace.OpenLua();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TAGS "  Symbols and modules"))
                workspace.OpenSymbols();
            if (ImGui::MenuItem(ICON_FA_TH_LIST "  Memory regions", nullptr, false, hasProcess))
                workspace.OpenRegions();
            if (ImGui::MenuItem(ICON_FA_BAND_AID "  Scan for patches", nullptr, false, hasProcess))
                workspace.OpenPatchScan();
            if (ImGui::MenuItem(ICON_FA_PROJECT_DIAGRAM "  Dissect code", nullptr, false, hasProcess))
                workspace.OpenCodeDissect(CodeDissectWindow::Functions);
            if (ImGui::MenuItem(ICON_FA_QUOTE_LEFT "  Referenced strings", nullptr, false, hasProcess))
                workspace.OpenCodeDissect(CodeDissectWindow::Strings);
            if (ImGui::MenuItem(ICON_FA_SEARCH_LOCATION "  Find assembly code", nullptr, false, hasProcess))
                workspace.OpenAsmSearch();
            if (ImGui::MenuItem(ICON_FA_STREAM "  Log"))
                workspace.OpenLog();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PLAY "  Run autorun scripts again"))
                workspace.Report(engine.Lua().RunAutorun(Paths::AutorunFolder()));
            if (ImGui::MenuItem(ICON_FA_FOLDER "  Open autorun folder"))
            {
                std::error_code ignored;
                std::filesystem::create_directories(Paths::AutorunFolder(), ignored);
                ::ShellExecuteW(nullptr, L"open", Paths::AutorunFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::EndMenu();
        }
        if (!m_release && ImGui::BeginMenu("Debug"))
        {
            Debug::Debugger& debugger = engine.Debugger();
            if (ImGui::MenuItem(ICON_FA_BUG "  Attach debugger", nullptr, false, hasProcess && !debugger.Attached()))
                workspace.Report(engine.EnsureDebugger());
            if (ImGui::MenuItem(ICON_FA_UNLINK "  Detach debugger", nullptr, false, debugger.Attached()))
                debugger.Detach();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PAUSE "  Break", nullptr, false, debugger.Attached() && !debugger.Broken()))
                workspace.Report(debugger.BreakNow());
            if (ImGui::MenuItem(ICON_FA_PLAY "  Run", "F9", false, debugger.Broken()))
                debugger.Resume(Debug::ResumeMode::Continue);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem(ICON_FA_INFO_CIRCLE "  About"))
                workspace.OpenAbout();
            if (!m_release && ImGui::MenuItem(ICON_FA_LIST_OL "  Error codes"))
                workspace.OpenLog();
            if (!Target::IsElevated() && ImGui::MenuItem(ICON_FA_SHIELD_ALT "  Restart as administrator"))
                workspace.Window().RestartElevated();
            ImGui::EndMenu();
        }
        menusEnd = ImGui::GetItemRectMax().x;
        ImGui::EndMenuBar();
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        const float buttonWidth = Px(46.0f);
        const float buttonsStart = width - buttonWidth * 3.0f;
        workspace.Window().AddDragArea(menusEnd - origin.x + Px(8.0f), 0.0f, buttonsStart, height);
        ImGui::SetCursorPos(ImVec2(buttonsStart, 0.0f));
        if (WindowButton("##minimise", ICON_FA_WINDOW_MINIMIZE, buttonWidth, height, false))
            workspace.Window().Minimise();
        ImGui::SameLine(0.0f, 0.0f);
        if (WindowButton("##maximise", workspace.Window().Maximised() ? ICON_FA_WINDOW_RESTORE : ICON_FA_WINDOW_MAXIMIZE, buttonWidth, height, false))
            workspace.Window().ToggleMaximised();
        ImGui::SameLine(0.0f, 0.0f);
        if (WindowButton("##close", ICON_FA_TIMES, buttonWidth, height, true))
            workspace.RequestQuit();
    }

    void MainWindow::ProcessBar()
    {
        Engine& engine = m_engine;
        const float height = Px(kProcessBarHeight);
        BeginCard("##process", nullptr, ImVec2(0.0f, height), ImGuiWindowFlags_NoScrollbar);
        const float y = std::round((height - ImGui::GetFrameHeight()) * 0.5f) - Px(1.0f);
        ImGui::SetCursorPosY(y);
        const bool hasProcess = engine.HasProcess();
        if (hasProcess ? ImGui::Button(ICON_FA_DESKTOP "  Process") : AccentButton(ICON_FA_DESKTOP "  Open process"))
            m_workspace.OpenProcessPicker();
        ItemTooltip("Pick the process to work on (Ctrl+P)");
        ImGui::SameLine(0.0f, Px(12.0f));
        if (hasProcess)
        {
            const auto& process = *engine.Process();
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::TextUnformatted(process.Name().c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            if (m_release)
                Muted(process.Alive() ? "Opened" : "Closed");
            else
                Muted(Format("pid %u  ·  %s  ·  %zu modules", process.Id(), process.Is64Bit() ? "64-bit" : "32-bit", process.Modules()->size()));
        }
        else
        {
            Muted(m_release ? "No game opened yet." : "No process opened. Pick one to scan, view memory or run scripts.");
        }
        if (m_release)
        {
            EndCard();
            return;
        }

        const float right = ImGui::GetWindowContentRegionMax().x;
        Debug::Debugger& debugger = engine.Debugger();
        std::string debugText = debugger.Attached() ? (debugger.Broken() ? ICON_FA_PAUSE_CIRCLE "  Broken" : ICON_FA_BUG "  Debugger attached") : "";
        float x = right;
        x -= ImGui::CalcTextSize(ICON_FA_MEMORY "  Memory view").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(x, y));
        if (ImGui::Button(ICON_FA_MEMORY "  Memory view"))
            m_workspace.OpenMemoryViewer(0);
        ItemTooltip("Disassembler and hex view of the process (Ctrl+M)");
        if (!debugText.empty())
        {
            const float textWidth = ImGui::CalcTextSize(debugText.c_str()).x;
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(x - textWidth - Px(16.0f), y));
            ImGui::PushStyleColor(ImGuiCol_Text, debugger.Broken() ? ManifoldTheme::Warning : ManifoldTheme::Accent);
            ImGui::TextUnformatted(debugText.c_str());
            ImGui::PopStyleColor();
            if (debugger.Broken() && ImGui::IsItemClicked())
                m_workspace.OpenMemoryViewer(debugger.Break().Address);
            ItemTooltip(debugger.Broken() ? "The process is stopped at a breakpoint. Click to show it." : "The debugger is attached to the process.");
        }
        EndCard();
    }

    void MainWindow::ScanArea(float height)
    {
        const float total = ImGui::GetContentRegionAvail().x;
        float left = std::floor(total * m_resultsShare);
        float right = total - left - Px(8.0f);
        const ImVec2 start = ImGui::GetCursorPos();
        ResultsCard(left, height);
        ImGui::SetCursorPos(start);
        if (Splitter("##scansplit", true, Px(8.0f), &left, &right, Px(260.0f), Px(300.0f)))
            m_resultsShare = left / total;
        ImGui::SetCursorPos(ImVec2(start.x + left + Px(8.0f), start.y));
        ScanCard(right, height);
        ImGui::SetCursorPos(ImVec2(start.x, start.y + height + Px(6.0f)));
    }

    void MainWindow::ResultsCard(float width, float height)
    {
        Engine& engine = m_engine;
        Scan::Scanner& scanner = engine.Scanner();
        BeginCard("##results", nullptr, ImVec2(width, height), ImGuiWindowFlags_NoScrollbar);
        const std::uint64_t count = scanner.Count();
        CardTitle(ICON_FA_SEARCH, "Found", scanner.HasResults() ? Grouped(count) + (scanner.ScanNumber() > 0 ? Format("  ·  scan %d  ·  %.2f s", scanner.ScanNumber(), scanner.LastSeconds()) : "") : "no scan yet");
        if (!m_selectedResults.empty())
        {
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(ICON_FA_ARROW_DOWN "  Add 0000 selected").x - Px(16.0f));
            if (ImGui::SmallButton((ICON_FA_ARROW_DOWN "  Add " + std::to_string(m_selectedResults.size()) + " selected").c_str()))
            {
                for (std::uint64_t index : m_selectedResults)
                {
                    Scan::ScanRecord record;
                    if (scanner.Record(index, record))
                        m_workspace.AddAddress(record.Address, scanner.Type());
                }
            }
        }

        if (m_liveScan != scanner.ScanNumber())
        {
            m_liveScan = scanner.ScanNumber();
            m_liveValues.clear();
            m_selectedResults.clear();
        }
        const double now = engine.Now();
        const bool refresh = now - m_liveAt > engine.Options().ResultValueRefreshMs / 1000.0;
        if (refresh)
        {
            m_liveAt = now;
            m_liveValues.clear();
        }

        if (!scanner.HasResults() && !scanner.Busy())
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const std::vector<SkeletonColumn> columns = { { 1.4f, ManifoldTheme::AddressText, 0.10f }, { 1.0f, ManifoldTheme::Text, 0.06f },
                { 1.0f, ManifoldTheme::Text, 0.05f }, { 1.0f, ManifoldTheme::Muted, 0.08f } };
            TableSkeleton(origin, ImVec2(origin.x + available.x, origin.y + available.y), columns, 3, ManifoldTheme::Card);
            if (!engine.HasProcess())
                m_workspace.NoProcessState(ICON_FA_SEARCH, m_release ? "Open the game, then search its memory for the value you want to change." : "Open a process, then search its memory for the value you want to change.");
            else
                EmptyState(ICON_FA_SEARCH, "Find a value", "Enter the value on the right and press First scan. Change it in the game, then Next scan narrows the results down.", {}, false);
            EndCard();
            return;
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        PushMono();
        if (ImGui::BeginTable("##found", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("First", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            const int shown = static_cast<int>(std::min<std::uint64_t>(count, 50'000'000ull));
            const Scan::ValueType type = scanner.Type();
            const Scan::ValueFormat format = scanner.Format();
            ImGuiListClipper clipper;
            clipper.Begin(shown);
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    Scan::ScanRecord record;
                    if (!scanner.Record(static_cast<std::uint64_t>(row), record))
                        continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool selected = m_selectedResults.count(static_cast<std::uint64_t>(row)) > 0;
                    ImGui::PushID(row);
                    const std::string address = engine.Symbols().ModuleOffset(record.Address);
                    const bool isStatic = address.front() == '"';
                    if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        const std::uint64_t index = static_cast<std::uint64_t>(row);
                        if (ImGui::GetIO().KeyShift && !m_selectedResults.empty())
                        {
                            const std::uint64_t from = std::min(m_lastClickedResult, index);
                            const std::uint64_t to = std::max(m_lastClickedResult, index);
                            for (std::uint64_t i = from; i <= to && i - from < 100000; ++i)
                                m_selectedResults.insert(i);
                        }
                        else if (ImGui::GetIO().KeyCtrl)
                        {
                            if (selected)
                                m_selectedResults.erase(index);
                            else
                                m_selectedResults.insert(index);
                        }
                        else
                        {
                            m_selectedResults.clear();
                            m_selectedResults.insert(index);
                        }
                        m_lastClickedResult = index;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            m_workspace.AddAddress(record.Address, type);
                    }
                    if (ImGui::BeginPopupContextItem("##resultmenu"))
                    {
                        if (!selected)
                        {
                            m_selectedResults.clear();
                            m_selectedResults.insert(static_cast<std::uint64_t>(row));
                        }
                        if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Add selected to the address list"))
                        {
                            for (std::uint64_t index : m_selectedResults)
                            {
                                Scan::ScanRecord selectedRecord;
                                if (scanner.Record(index, selectedRecord))
                                    m_workspace.AddAddress(selectedRecord.Address, type);
                            }
                        }
                        if (ImGui::MenuItem(ICON_FA_MEMORY "  Browse this memory region"))
                            m_workspace.OpenMemoryViewer(record.Address, true);
                        if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect data at this address"))
                            m_workspace.OpenStructure(record.Address);
                        ImGui::Separator();
                        const int size = Scan::ValueSize(type, format);
                        if (ImGui::MenuItem(ICON_FA_EYE "  Find out what accesses this address"))
                            m_workspace.StartWatch(Debug::WatchKind::Accesses, record.Address, size);
                        if (ImGui::MenuItem(ICON_FA_PEN "  Find out what writes to this address"))
                            m_workspace.StartWatch(Debug::WatchKind::Writes, record.Address, size);
                        ImGui::Separator();
                        if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                            ImGui::SetClipboardText(Hex(record.Address).c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, isStatic ? ManifoldTheme::Accent : ManifoldTheme::Text);
                    ImGui::TextUnformatted(isStatic ? address.c_str() : Hex(record.Address, engine.Process() && engine.Process()->Is64Bit() ? 12 : 8).c_str());
                    ImGui::PopStyleColor();
                    if (isStatic)
                        ItemTooltip("Static address inside a module: it stays the same after the game restarts.");
                    ImGui::PopID();

                    auto live = m_liveValues.find(static_cast<std::uint64_t>(row));
                    if (live == m_liveValues.end())
                    {
                        std::string value = "??";
                        if (engine.HasProcess())
                        {
                            std::vector<std::uint8_t> bytes(record.Previous.size());
                            if (!bytes.empty() && engine.Process()->ReadExact(record.Address, bytes.data(), bytes.size()))
                                value = Scan::FormatValue(type, bytes, format);
                        }
                        live = m_liveValues.emplace(static_cast<std::uint64_t>(row), value).first;
                    }
                    const std::string previous = Scan::FormatValue(type, record.Previous, format);
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, live->second != previous ? ManifoldTheme::Changed : ManifoldTheme::Text);
                    ImGui::TextUnformatted(live->second.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(previous.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                    ImGui::TextUnformatted(Scan::FormatValue(type, record.First, format).c_str());
                    ImGui::PopStyleColor();
                }
            }
            if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !m_selectedResults.empty())
            {
                for (std::uint64_t index : m_selectedResults)
                {
                    Scan::ScanRecord record;
                    if (scanner.Record(index, record))
                        m_workspace.AddAddress(record.Address, type);
                }
            }
            ImGui::EndTable();
        }
        PopMono();
        EndCard();
    }

    Scan::ScanOptions MainWindow::CurrentOptions() const
    {
        Scan::ScanOptions options;
        options.Type = Scan::kScanTypes[m_typeIndex];
        options.Mode = kCompares[m_compareIndex];
        options.Value = Trim(m_value);
        options.Value2 = Trim(m_value2);
        options.Hex = m_hex;
        options.Unicode = m_unicode;
        options.CaseSensitive = m_caseSensitive;
        options.Floats = static_cast<Scan::FloatMode>(m_floatMode);
        options.Start = ParseHex(m_start).value_or(0);
        options.Stop = ParseHex(m_stop).value_or(0x7FFFFFFFFFFFull);
        options.Writable = ToTristate(m_writable);
        options.Executable = ToTristate(m_executable);
        options.CopyOnWrite = ToTristate(m_copyOnWrite);
        options.Private = m_private;
        options.Image = m_image;
        options.Mapped = m_mapped;
        options.Alignment = m_fastScan ? std::max(1, m_alignment) : 1;
        options.PauseProcess = m_pause;
        options.Threads = m_engine.Options().ScanThreads;
        return options;
    }

    void MainWindow::DoFirstScan()
    {
        if (!m_engine.HasProcess())
        {
            m_workspace.Report(Status::Fail(ErrorCode::NoProcess));
            return;
        }
        if (!Scan::CompareAllowedFirst(kCompares[m_compareIndex]))
            m_compareIndex = 0;
        const Status status = m_engine.Scanner().Start(m_engine.Process(), CurrentOptions(), true);
        m_workspace.Report(status);
    }

    void MainWindow::DoNextScan()
    {
        if (!m_engine.HasProcess())
        {
            m_workspace.Report(Status::Fail(ErrorCode::NoProcess));
            return;
        }
        Scan::ScanOptions options = CurrentOptions();
        options.Type = m_engine.Scanner().Type();
        const Status status = m_engine.Scanner().Start(m_engine.Process(), options, false);
        m_workspace.Report(status);
    }

    void MainWindow::ScanCard(float width, float height)
    {
        Engine& engine = m_engine;
        Scan::Scanner& scanner = engine.Scanner();
        BeginCard("##scan", nullptr, ImVec2(width, height));
        CardTitle(ICON_FA_CROSSHAIRS, "Scan");
        const bool busy = scanner.Busy();
        const bool scanned = scanner.HasResults();
        const float buttonWidth = std::floor((ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f);

        ImGui::BeginDisabled(busy);
        if (!scanned)
        {
            if (AccentButton(ICON_FA_SEARCH "  First scan", ImVec2(buttonWidth, 0.0f)))
                DoFirstScan();
        }
        else if (ImGui::Button(ICON_FA_REDO "  New scan", ImVec2(buttonWidth, 0.0f)))
        {
            scanner.Reset();
            m_selectedResults.clear();
            if (!Scan::CompareAllowedFirst(kCompares[m_compareIndex]))
                m_compareIndex = 0;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!scanned);
        if ((scanned ? AccentButton(ICON_FA_FILTER "  Next scan", ImVec2(buttonWidth, 0.0f)) : ImGui::Button(ICON_FA_FILTER "  Next scan", ImVec2(buttonWidth, 0.0f))))
            DoNextScan();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!scanner.CanUndo());
        if (ImGui::Button(ICON_FA_UNDO "  Undo scan", ImVec2(buttonWidth, 0.0f)))
            scanner.Undo();
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        if (busy)
        {
            ImGui::ProgressBar(static_cast<float>(scanner.Progress()), ImVec2(ImGui::GetContentRegionAvail().x - Px(80.0f), 0.0f));
            ImGui::SameLine();
            if (DangerButton("Cancel", ImVec2(-1.0f, 0.0f)))
                scanner.Cancel();
        }
        else if (!scanner.LastError().Ok())
        {
            TextWithError(scanner.LastError().Text, ManifoldTheme::Error);
        }

        ImGui::Spacing();
        const Scan::Compare compare = kCompares[m_compareIndex];
        const bool needsValue = Scan::CompareNeedsValue(compare);
        const float labelWidth = Px(84.0f);
        ImGui::BeginDisabled(busy);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Value");
        ImGui::SameLine(labelWidth);
        ImGui::BeginDisabled(!needsValue);
        if (m_focusValue)
        {
            ImGui::SetKeyboardFocusHere();
            m_focusValue = false;
        }
        const float hexWidth = ImGui::CalcTextSize("Hex").x + ImGui::GetFrameHeight() + Px(12.0f);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - hexWidth);
        if (InputTextWithHint("##value", Scan::CompareNeedsTwoValues(compare) ? "from" : "value", m_value, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (!scanned)
                DoFirstScan();
            else
                DoNextScan();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Hex", &m_hex);
        if (Scan::CompareNeedsTwoValues(compare))
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth - ImGui::GetStyle().WindowPadding.x + Px(2.0f));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - hexWidth);
            InputTextWithHint("##value2", "to", m_value2);
        }
        ImGui::EndDisabled();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Scan type");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##compare", Scan::CompareName(compare)))
        {
            for (int i = 0; i < static_cast<int>(std::size(kCompares)); ++i)
            {
                const bool allowed = scanned ? Scan::CompareAllowedNext(kCompares[i]) : Scan::CompareAllowedFirst(kCompares[i]);
                if (!allowed)
                    continue;
                if (ImGui::Selectable(Scan::CompareName(kCompares[i]), i == m_compareIndex))
                    m_compareIndex = i;
            }
            ImGui::EndCombo();
        }
        if (!scanned && !Scan::CompareAllowedFirst(compare))
            m_compareIndex = 0;
        if (scanned && !Scan::CompareAllowedNext(compare))
            m_compareIndex = 0;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Value type");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::BeginDisabled(scanned);
        if (ImGui::BeginCombo("##type", Scan::TypeName(Scan::kScanTypes[m_typeIndex])))
        {
            for (int i = 0; i < static_cast<int>(std::size(Scan::kScanTypes)); ++i)
            {
                if (ImGui::Selectable(Scan::TypeName(Scan::kScanTypes[i]), i == m_typeIndex))
                {
                    m_typeIndex = i;
                    m_alignment = Scan::DefaultAlignment(Scan::kScanTypes[i]);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        const Scan::ValueType type = scanned ? scanner.Type() : Scan::kScanTypes[m_typeIndex];
        if (Scan::IsFloating(type))
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth - ImGui::GetStyle().WindowPadding.x + Px(2.0f));
            ImGui::RadioButton("Rounded", &m_floatMode, 0);
            ItemTooltip("100.5 finds values from 100.45 to 100.55");
            ImGui::SameLine();
            ImGui::RadioButton("Truncated", &m_floatMode, 1);
            ItemTooltip("100.5 finds values from 100.5 to 100.6");
            ImGui::SameLine();
            ImGui::RadioButton("Exact", &m_floatMode, 2);
        }
        if (type == Scan::ValueType::String)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelWidth - ImGui::GetStyle().WindowPadding.x + Px(2.0f));
            ImGui::Checkbox("UTF-16", &m_unicode);
            ImGui::SameLine();
            ImGui::Checkbox("Case sensitive", &m_caseSensitive);
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Memory scan options"))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Start");
            ImGui::SameLine(labelWidth);
            ImGui::SetNextItemWidth(Px(150.0f));
            InputText("##start", m_start, ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::SameLine();
            ImGui::TextUnformatted("Stop");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Px(150.0f));
            InputText("##stop", m_stop, ImGuiInputTextFlags_CharsHexadecimal);
            if (engine.HasProcess() && ImGui::BeginCombo("##module", "Limit to a module...", ImGuiComboFlags_HeightLarge))
            {
                if (ImGui::Selectable("All memory"))
                {
                    m_start = "0";
                    m_stop = "7FFFFFFFFFFF";
                }
                for (const Target::ModuleInfo& module : *engine.Process()->Modules())
                {
                    if (ImGui::Selectable(module.Name.c_str()))
                    {
                        m_start = Hex(module.Base);
                        m_stop = Hex(module.Base + module.Size);
                    }
                }
                ImGui::EndCombo();
            }
            TriStateCheckbox("Writable", m_writable);
            ItemTooltip("Checked: only writable memory. Empty: only read-only memory. Mixed: both.");
            ImGui::SameLine();
            TriStateCheckbox("Executable", m_executable);
            ImGui::SameLine();
            TriStateCheckbox("Copy on write", m_copyOnWrite);
            ImGui::Checkbox("Private", &m_private);
            ImGui::SameLine();
            ImGui::Checkbox("Image", &m_image);
            ImGui::SameLine();
            ImGui::Checkbox("Mapped", &m_mapped);
            ImGui::Checkbox("Fast scan", &m_fastScan);
            ItemTooltip("Only look at addresses aligned to this many bytes. Values in games are nearly always aligned.");
            ImGui::SameLine();
            ImGui::BeginDisabled(!m_fastScan);
            ImGui::SetNextItemWidth(Px(70.0f));
            ImGui::InputInt("Alignment", &m_alignment, 1, 4);
            m_alignment = std::clamp(m_alignment, 1, 64);
            ImGui::EndDisabled();
            ImGui::Checkbox("Pause the process while scanning", &m_pause);
        }
        ImGui::EndDisabled();
        EndCard();
    }

    std::vector<Table::MemoryRecord*> MainWindow::SelectedRecords()
    {
        std::vector<Table::MemoryRecord*> records;
        for (int id : m_engine.Table().Selection())
        {
            if (Table::MemoryRecord* record = m_engine.Table().FindById(id))
                records.push_back(record);
        }
        return records;
    }

    void MainWindow::AddressListCard(float height)
    {
        Engine& engine = m_engine;
        Table::AddressList& list = engine.Table();
        BeginCard("##addresslist", nullptr, ImVec2(0.0f, height), ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();
        CardTitle(ICON_FA_LIST, "Address list", list.Count() ? std::to_string(list.Count()) + (list.Count() == 1 ? " record" : " records") : "");
        if (!m_release)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.11f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.16f));
            ImGui::SameLine(0.0f, Px(16.0f));
            if (ImGui::Button(ICON_FA_PLUS "  Address"))
            {
                m_addDialog = true;
                m_addAddress.clear();
                m_addDescription = "No description";
            }
            ItemTooltip("Add an address by hand");
            ImGui::SameLine(0.0f, Px(4.0f));
            if (ImGui::Button(ICON_FA_FOLDER_PLUS "  Group"))
                list.AddGroup("Group");
            ItemTooltip("Add an empty group");
            ImGui::SameLine(0.0f, Px(4.0f));
            if (ImGui::Button(ICON_FA_CODE "  Script"))
            {
                Table::MemoryRecord& record = list.AddScript("Auto Assemble script", Asm::CheatTableTemplate());
                m_workspace.OpenScript(record.Script, record.Id);
            }
            ItemTooltip("Add an Auto Assembler script");
            ImGui::PopStyleColor(3);
        }
        const float filterWidth = Px(180.0f);
        const float iconWidth = ImGui::GetFrameHeight();
        const float gap = Px(4.0f);
        const float right = ImGui::GetWindowContentRegionMax().x;
        const float filterX = right - filterWidth - iconWidth - gap;
        const float groupsX = filterX - (iconWidth + gap) * 2.0f - Px(6.0f);
        ProgressLine(groupsX - Px(12.0f));
        ImGui::SameLine();
        ImGui::SetCursorPosX(groupsX);
        if (IconButton(ICON_FA_EXPAND_ALT, "Expand every group"))
            list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = true; });
        ImGui::SameLine(0.0f, gap);
        if (IconButton(ICON_FA_COMPRESS_ALT, "Collapse every group"))
            list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = false; });
        ImGui::SameLine();
        ImGui::SetCursorPosX(filterX);
        ImGui::SetNextItemWidth(filterWidth);
        InputTextWithHint("##tablefilter", ICON_FA_SEARCH " Filter", m_tableFilter);
        ImGui::SameLine(0.0f, gap);
        const bool filterOptions = m_filterActiveOnly || m_filterKind != 0;
        if (filterOptions)
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
        if (IconButton(ICON_FA_FILTER, filterOptions ? "Filter options (some are on)" : "Filter options"))
            ImGui::OpenPopup("##filteroptions");
        if (filterOptions)
            ImGui::PopStyleColor();
        if (ImGui::BeginPopup("##filteroptions"))
        {
            ImGui::Checkbox("Only active records", &m_filterActiveOnly);
            ImGui::Spacing();
            Muted("Show");
            Segmented("##filterkind", &m_filterKind, { "All", "Values", "Scripts", "Groups" }, Px(280.0f));
            ImGui::Spacing();
            ImGui::BeginDisabled(!filterOptions);
            if (ImGui::SmallButton(ICON_FA_UNDO "  Reset"))
            {
                m_filterActiveOnly = false;
                m_filterKind = 0;
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        BuildRows();

        const auto listMenu = [&](bool open) {
            if (!open)
                return;
            if (ImGui::MenuItem(ICON_FA_PLUS "  Add address manually"))
            {
                m_addDialog = true;
                m_addAddress.clear();
                m_addDescription = "No description";
            }
            if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  Add group"))
                list.AddGroup("Group");
            if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V"))
                m_workspace.PasteRecords(0);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_EXPAND_ALT "  Expand every group"))
                list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = true; });
            if (ImGui::MenuItem(ICON_FA_COMPRESS_ALT "  Collapse every group"))
                list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = false; });
            ImGui::EndPopup();
        };

        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
        if (list.Count() == 0)
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const ImVec2 available = ImGui::GetContentRegionAvail();
            std::vector<SkeletonColumn> columns = { { 0.35f, ManifoldTheme::Muted, 0.22f, true }, { 2.4f, ManifoldTheme::Text, 0.07f } };
            if (!m_release)
            {
                columns.push_back({ 1.3f, ManifoldTheme::AddressText, 0.09f });
                columns.push_back({ 0.8f, ManifoldTheme::Muted, 0.09f });
            }
            columns.push_back({ 1.3f, ManifoldTheme::Text, 0.06f });
            TableSkeleton(origin, ImVec2(origin.x + available.x, origin.y + available.y), columns, 11, ManifoldTheme::Card);

            const std::vector<std::string>& recent = engine.Options().RecentTables;
            const std::string last = recent.empty() ? std::string() : recent.front();
            if (last != m_recentChecked || engine.Now() - m_recentCheckedAt > 2.0)
            {
                std::error_code error;
                m_recentChecked = last;
                m_recentCheckedAt = engine.Now();
                m_recentExists = !last.empty() && std::filesystem::exists(Utf8ToWide(last), error);
            }
            std::vector<EmptyAction> actions;
            actions.push_back({ ICON_FA_FOLDER_OPEN "  Open table", "Ctrl+O" });
            if (!m_release)
                actions.push_back({ ICON_FA_PLUS "  Add address" });
            if (m_recentExists)
                actions.push_back({ ICON_FA_HISTORY "  Open " + WideToUtf8(std::filesystem::path(Utf8ToWide(last)).filename().wstring()), nullptr, false, true, last.c_str() });
            const int clicked = EmptyState(ICON_FA_LIST, m_release ? "No table loaded" : "No entries yet",
                m_release ? "Open a table to see its cheats here." : "Add results from a scan, add an address by hand, or open a cheat table.", actions, false);
            if (clicked == 0)
            {
                if (const auto path = m_workspace.OpenFileDialog({ { L"Cheat tables (*.CT)", L"*.CT;*.ct" }, { L"All files", L"*.*" } }, Paths::TablesFolder()))
                    m_workspace.OpenTable(*path, false);
            }
            else if (clicked >= 0 && actions[static_cast<std::size_t>(clicked)].NewRow)
            {
                m_workspace.OpenTable(Utf8ToWide(last), false);
            }
            else if (clicked == 1)
            {
                m_addDialog = true;
                m_addAddress.clear();
                m_addDescription = "No description";
            }
            if (!m_release)
                listMenu(ImGui::BeginPopupContextWindow("##listmenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems));
        }
        else if (ImGui::BeginTable(m_release ? "##recordsrelease" : "##records", m_release ? 3 : 5, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Active", ImGuiTableColumnFlags_WidthFixed, Px(46.0f));
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            if (!m_release)
            {
                ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.3f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            }
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableHeadersRow();
            int row = 0;
            m_spots.clear();
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            const std::vector<ListRow> rows = m_rows;
            for (const ListRow& entry : rows)
            {
                if (Table::MemoryRecord* record = list.FindById(entry.Id))
                    RecordRow(*record, entry.Depth, row, entry.Match);
            }
            ImGui::PopItemFlag();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Dummy(ImVec2(0.0f, Px(30.0f)));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kRecordDrag))
                {
                    if (Table::MemoryRecord* dragged = list.FindById(*static_cast<const int*>(payload->Data)))
                        list.Move(*dragged, nullptr, -1);
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
                list.ClearSelection();
            if (!m_release)
                listMenu(ImGui::BeginPopupContextWindow("##listmenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems));
            const std::function<void()> paintStickyHeader = StickyHeader();
            ImGui::EndTable();
            if (paintStickyHeader)
                paintStickyHeader();
        }

        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput;
        if (focused)
        {
            auto selected = SelectedRecords();
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !selected.empty())
            {
                const bool target = !selected.front()->Active;
                for (Table::MemoryRecord* record : selected)
                    engine.RequestActive(*record, target);
            }
            ListKeys();
            if (!m_release && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selected.empty())
            {
                std::vector<int> ids = list.Selection();
                m_workspace.Confirm("Delete records", "Delete " + std::to_string(ids.size()) + (ids.size() == 1 ? " record" : " records") + " and their children?", [this, ids]()
                    {
                        for (int id : ids)
                        {
                            if (Table::MemoryRecord* record = m_engine.Table().FindById(id))
                            {
                                if (record->Active && record->Kind == Table::RecordKind::Script)
                                    m_workspace.Report(m_engine.SetActive(*record, false));
                                m_engine.Table().Remove(*record);
                            }
                        }
                    }, "Delete");
            }
            if (!m_release && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
                m_workspace.CopyRecords(list.Selection());
            if (!m_release && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                m_workspace.PasteRecords(0);
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            {
                list.ClearSelection();
                list.ForEach([&](Table::MemoryRecord& record) { list.Select(record.Id, true); });
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !selected.empty() && !engine.Lua().AddressListEvent("onvaluechange", selected.front()))
            {
                m_valueTargets = list.Selection();
                m_valueText = selected.front()->ValueText;
                m_valueDialog = true;
            }
        }
        EndCard();
    }

    void MainWindow::RecordRow(Table::MemoryRecord& record, int depth, int& row, bool match)
    {
        Engine& engine = m_engine;
        Table::AddressList& list = engine.Table();
        ImGui::TableNextRow();
        ImGui::PushID(record.Id);
        ++row;

        ImGui::TableNextColumn();
        m_spots.push_back({ record.Id, ImGui::GetCursorScreenPos().y });
        bool active = record.Active;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(3.0f, 1.0f));
        {
            const bool arrow = record.Active && record.Kind == Table::RecordKind::Value && record.Freeze != Table::FreezeMode::Normal;
            const float used = ImGui::GetFrameHeight() + (arrow ? Px(2.0f) + ImGui::CalcTextSize(ICON_FA_ARROW_UP).x : 0.0f);
            const float spare = ImGui::GetContentRegionAvail().x - used;
            if (spare > 0.0f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor(spare * 0.5f));
        }
        if (engine.ActivationPending(record.Id))
        {
            Spinner(ImGui::GetFrameHeight(), ManifoldTheme::Accent);
            ImGui::PopStyleVar();
            ItemTooltip(record.Active ? "Switching off, waiting in line" : "Switching on, waiting in line");
        }
        else
        {
            if (ImGui::Checkbox("##active", &active))
                engine.RequestActive(record, active);
            ImGui::PopStyleVar();
            if (record.Kind == Table::RecordKind::Value)
                ItemTooltip(record.Active ? "Frozen: the value is written back all the time" : "Freeze the value");
            else if (record.Kind == Table::RecordKind::Script)
                ItemTooltip(record.Active ? "Disable the script" : "Enable the script");
        }
        if (record.Active && record.Kind == Table::RecordKind::Value && record.Freeze != Table::FreezeMode::Normal)
        {
            ImGui::SameLine(0.0f, Px(2.0f));
            ImGui::TextColored(ManifoldTheme::Warning, "%s", record.Freeze == Table::FreezeMode::AllowIncrease ? ICON_FA_ARROW_UP : ICON_FA_ARROW_DOWN);
        }

        ImGui::TableNextColumn();
        const bool selected = list.Selected(record.Id);
        const float indent = Px(16.0f) * static_cast<float>(depth);
        if (indent > 0.0f)
            ImGui::Indent(indent);
        const bool hasChildren = !record.Children.empty();
        if (hasChildren)
        {
            const bool canToggle = !(record.Options.HideChildren && !record.Active) && !record.Options.AlwaysHideChildren;
            ImGui::PushStyleColor(ImGuiCol_Text, canToggle ? ManifoldTheme::Label : ManifoldTheme::Muted);
            ImGui::TextUnformatted(record.ChildrenVisible() ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
            ImGui::PopStyleColor();
            if (canToggle && ImGui::IsItemClicked())
                record.Expanded = !record.Expanded;
            ImGui::SameLine(0.0f, Px(5.0f));
        }
        else if (depth > 0)
        {
            ImGui::Dummy(ImVec2(Px(8.0f), 0.0f));
            ImGui::SameLine(0.0f, Px(5.0f));
        }
        const char* icon = record.Kind == Table::RecordKind::Script ? ICON_FA_CODE " " : record.Kind == Table::RecordKind::Group ? ICON_FA_FOLDER " " : "";
        const std::string label = std::string(icon) + record.Description + "##record";
        if (match && !m_tableFilter.empty())
        {
            const std::size_t at = FindNoCase(record.Description, m_tableFilter);
            if (at != std::string_view::npos)
            {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float before = ImGui::CalcTextSize((std::string(icon) + record.Description.substr(0, at)).c_str()).x;
                const float width = ImGui::CalcTextSize(record.Description.substr(at, m_tableFilter.size()).c_str()).x;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + before - Px(1.0f), pos.y), ImVec2(pos.x + before + width + Px(1.0f), pos.y + ImGui::GetTextLineHeight()),
                    ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.30f)), Px(2.0f));
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, !match ? ManifoldTheme::Muted : record.HasColor ? RecordColor(record.Color) : record.Kind == Table::RecordKind::Group ? ManifoldTheme::Label : ManifoldTheme::Text);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
        {
            if (ImGui::GetIO().KeyCtrl)
            {
                if (selected)
                    list.Selection().erase(std::remove(list.Selection().begin(), list.Selection().end(), record.Id), list.Selection().end());
                else
                    list.Select(record.Id, true);
            }
            else if (ImGui::GetIO().KeyShift && (m_anchorRecord >= 0 || m_lastClickedRecord >= 0))
            {
                if (m_anchorRecord < 0)
                    m_anchorRecord = m_lastClickedRecord;
                for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
                {
                    if (m_rows[static_cast<std::size_t>(i)].Id == record.Id)
                    {
                        const int previous = m_scrollToRecord;
                        SelectRow(i, true);
                        m_scrollToRecord = previous;
                    }
                }
            }
            else
            {
                list.Select(record.Id, false);
                m_anchorRecord = record.Id;
            }
            m_lastClickedRecord = record.Id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && m_release)
            {
                if (record.Kind == Table::RecordKind::Value)
                    StartInlineEdit(record);
                else
                    engine.RequestActive(record, !record.Active);
            }
            else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                const int column = ImGui::TableGetHoveredColumn();
                Lua::LuaEngine& lua = engine.Lua();
                if (record.Kind == Table::RecordKind::Script && column != 1)
                {
                    if (!TableHandlesScriptEdit(lua, record))
                        m_workspace.OpenScript(record.Script, record.Id);
                }
                else if (column == 1 || record.Kind == Table::RecordKind::Group)
                {
                    if (!lua.AddressListEvent("ondescriptionchange", &record))
                    {
                        m_descriptionDialog = true;
                        m_descriptionTarget = record.Id;
                        m_descriptionText = record.Description;
                    }
                }
                else if ((column == 2 && lua.AddressListEvent("onaddresschange", &record)) || (column == 3 && lua.AddressListEvent("ontypechange", &record)))
                {
                }
                else if (column == 4 && record.Kind == Table::RecordKind::Value)
                {
                    StartInlineEdit(record);
                }
                else
                    m_workspace.OpenRecordEditor(record.Id);
            }
        }
        ImGui::PopStyleColor();
        if (m_scrollToRecord == record.Id)
        {
            ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            m_scrollToRecord = -1;
        }
        if (!m_release && ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload(kRecordDrag, &record.Id, sizeof(int));
            const std::size_t count = selected ? list.Selection().size() : 1;
            ImGui::TextUnformatted(count > 1 ? (std::to_string(count) + " records").c_str() : record.Description.c_str());
            Muted("Top or bottom edge: place before or after  \u00b7  middle: put inside");
            ImGui::EndDragDropSource();
        }
        if (!m_release && ImGui::BeginDragDropTarget())
        {
            DropOnRecord(record, depth);
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##recordmenu"))
        {
            if (!selected)
                list.Select(record.Id, false);
            RecordMenu(record);
            ImGui::EndPopup();
        }
        if (!record.Hotkeys.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ManifoldTheme::Muted, ICON_FA_KEYBOARD);
            if (ImGui::IsItemHovered())
            {
                ShowTooltip([&]()
                    {
                        for (const Table::Hotkey& hotkey : record.Hotkeys)
                            ImGui::Text("%s: %s %s", Table::KeysText(hotkey.Keys).c_str(), Table::HotkeyActionName(hotkey.Action), hotkey.Value.c_str());
                    });
            }
        }
        if (!record.LastError.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ManifoldTheme::Error, ICON_FA_EXCLAMATION_TRIANGLE);
            ErrorTooltip(record.LastError);
            if (ImGui::IsItemHovered() && !ErrorOf(record.LastError))
                ItemTooltip(record.LastError.c_str());
        }
        if (indent > 0.0f)
            ImGui::Unindent(indent);

        PushMono();
        if (!m_release)
            ImGui::TableNextColumn();
        if (!m_release && record.Kind == Table::RecordKind::Value)
        {
            if (record.IsPointer())
            {
                ImGui::TextUnformatted(record.AddressOk ? ("P->" + Hex(record.ResolvedAddress)).c_str() : "P->????????");
                ItemTooltip(("[" + record.Address + "] + " + Join(record.Offsets, " + ")).c_str());
            }
            else
            {
                ImGui::TextUnformatted(record.Address.c_str());
                if (!record.AddressOk && !record.AddressError.empty())
                    ErrorTooltip(record.AddressError);
                else if (record.AddressOk)
                    ItemTooltip(Hex(record.ResolvedAddress).c_str());
            }
        }
        if (!m_release)
        {
            ImGui::TableNextColumn();
            Muted(TypeText(record));
        }
        ImGui::TableNextColumn();
        if (record.Kind == Table::RecordKind::Value && m_inlineEdit == record.Id)
        {
            InlineValue(record);
        }
        else if (record.Kind == Table::RecordKind::Value)
        {
            const bool fresh = engine.Now() - record.ChangedAt < 0.6;
            ImGui::PushStyleColor(ImGuiCol_Text, record.ValueText == "??" ? ManifoldTheme::Muted : fresh ? ManifoldTheme::Changed : ManifoldTheme::Text);
            ImGui::TextUnformatted(record.ValueText.c_str());
            ImGui::PopStyleColor();
        }
        else if (record.Kind == Table::RecordKind::Script && !m_release)
        {
            Muted(record.Active ? "<enabled>" : "<script>");
        }
        PopMono();
        ImGui::PopID();
    }

    bool MainWindow::RowMatches(const Table::MemoryRecord& record) const
    {
        if (!m_tableFilter.empty() && FindNoCase(record.Description, m_tableFilter) == std::string_view::npos)
            return false;
        if (m_filterActiveOnly && !record.Active)
            return false;
        switch (m_filterKind)
        {
        case 1: return record.Kind == Table::RecordKind::Value;
        case 2: return record.Kind == Table::RecordKind::Script;
        case 3: return record.Kind == Table::RecordKind::Group || !record.Children.empty();
        default: return true;
        }
    }

    void MainWindow::BuildRows()
    {
        m_rows.clear();
        Table::AddressList& list = m_engine.Table();
        const bool filtering = !m_tableFilter.empty() || m_filterActiveOnly || m_filterKind != 0;
        if (!filtering)
        {
            list.ForEachVisible([&](Table::MemoryRecord& record, int depth) { m_rows.push_back({ record.Id, depth, true }); });
            return;
        }
        const std::function<bool(Table::MemoryRecord&, int)> walk = [&](Table::MemoryRecord& record, int depth) -> bool
            {
                const std::size_t at = m_rows.size();
                const bool match = RowMatches(record);
                m_rows.push_back({ record.Id, depth, match });
                bool any = match;
                for (const auto& child : record.Children)
                    any = walk(*child, depth + 1) || any;
                if (!any)
                    m_rows.resize(at);
                return any;
            };
        for (const auto& root : list.Roots())
            walk(*root, 0);
    }

    void MainWindow::SelectRow(int index, bool extend)
    {
        if (index < 0 || index >= static_cast<int>(m_rows.size()))
            return;
        Table::AddressList& list = m_engine.Table();
        const int id = m_rows[static_cast<std::size_t>(index)].Id;
        if (extend)
        {
            int anchor = index;
            for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
            {
                if (m_rows[static_cast<std::size_t>(i)].Id == m_anchorRecord)
                    anchor = i;
            }
            list.ClearSelection();
            for (int i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
                list.Select(m_rows[static_cast<std::size_t>(i)].Id, true);
        }
        else
        {
            list.Select(id, false);
            m_anchorRecord = id;
        }
        m_lastClickedRecord = id;
        m_scrollToRecord = id;
    }

    void MainWindow::ListKeys()
    {
        if (m_rows.empty())
            return;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl || io.KeyAlt)
            return;
        Table::AddressList& list = m_engine.Table();
        int current = -1;
        for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
        {
            if (m_rows[static_cast<std::size_t>(i)].Id == m_lastClickedRecord)
                current = i;
        }
        const int last = static_cast<int>(m_rows.size()) - 1;
        int target = current;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            target = current < 0 ? 0 : std::min(current + 1, last);
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            target = current < 0 ? 0 : std::max(current - 1, 0);
        else if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
            target = current < 0 ? 0 : std::min(current + 10, last);
        else if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            target = current < 0 ? 0 : std::max(current - 10, 0);
        else if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
            target = 0;
        else if (ImGui::IsKeyPressed(ImGuiKey_End, false))
            target = last;
        else if (current >= 0 && ImGui::IsKeyPressed(ImGuiKey_F2, false))
        {
            if (Table::MemoryRecord* record = list.FindById(m_rows[static_cast<std::size_t>(current)].Id))
                StartInlineEdit(*record);
            return;
        }
        else if (current >= 0 && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)))
        {
            Table::MemoryRecord* record = list.FindById(m_rows[static_cast<std::size_t>(current)].Id);
            if (!record)
                return;
            const bool filtering = !m_tableFilter.empty() || m_filterActiveOnly || m_filterKind != 0;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            {
                if (!filtering && CanToggleChildren(*record) && record->ChildrenVisible())
                    record->Expanded = false;
                else if (record->Parent)
                {
                    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i)
                    {
                        if (m_rows[static_cast<std::size_t>(i)].Id == record->Parent->Id)
                            target = i;
                    }
                }
            }
            else if (!filtering && CanToggleChildren(*record) && !record->ChildrenVisible())
                record->Expanded = true;
            else if (current < last && m_rows[static_cast<std::size_t>(current + 1)].Depth > m_rows[static_cast<std::size_t>(current)].Depth)
                target = current + 1;
        }
        if (target != current && target >= 0)
            SelectRow(target, io.KeyShift);
    }

    void MainWindow::StartInlineEdit(Table::MemoryRecord& record)
    {
        if (record.Kind != Table::RecordKind::Value)
            return;
        if (m_engine.Lua().AddressListEvent("onvaluechange", &record))
            return;
        if (!m_engine.DropDownOf(record).empty())
        {
            OpenValueDialog(record);
            return;
        }
        Table::AddressList& list = m_engine.Table();
        if (!list.Selected(record.Id))
            list.Select(record.Id, false);
        m_inlineEdit = record.Id;
        m_inlineText = record.ValueText == "??" ? "" : record.ValueText;
        m_inlineFocus = true;
    }

    void MainWindow::InlineValue(Table::MemoryRecord& record)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(4.0f), 0.0f));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_inlineFocus)
        {
            ImGui::SetKeyboardFocusHere();
            m_inlineFocus = false;
        }
        const bool enter = InputText("##inlinevalue", m_inlineText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool deactivated = ImGui::IsItemDeactivated();
        ImGui::PopStyleVar();
        if (enter)
        {
            Table::AddressList& list = m_engine.Table();
            const std::vector<int> targets = list.Selected(record.Id) ? list.Selection() : std::vector<int>{ record.Id };
            for (int id : targets)
            {
                Table::MemoryRecord* target = list.FindById(id);
                if (target && target->Kind == Table::RecordKind::Value)
                    m_workspace.Report(m_engine.WriteRecord(*target, m_inlineText));
            }
            m_engine.RefreshValues(true);
            m_inlineEdit = -1;
        }
        else if (deactivated)
        {
            m_inlineEdit = -1;
        }
    }

    void MainWindow::ProgressLine(float right)
    {
        const ActivationProgress progress = m_engine.Progress();
        if (progress.Total == 0 && !progress.TableScript)
            return;
        const float left = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + Px(14.0f);
        std::string text;
        const char* button = nullptr;
        if (progress.TableScript)
        {
            text = Format("Table script running  ·  %.1f s", progress.TableScriptSeconds);
            button = ICON_FA_STOP "  Stop";
        }
        else
        {
            text = Format("Switching %d of %d", std::min(progress.Done + 1, progress.Total), progress.Total);
            button = "Cancel";
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const float spinner = ImGui::GetTextLineHeight();
        const float buttonWidth = ImGui::CalcTextSize(button).x + style.FramePadding.x * 2.0f;
        const float fixed = spinner + Px(6.0f) + Px(8.0f) + buttonWidth;
        std::string full = text;
        if (!progress.TableScript && !progress.Current.empty())
        {
            std::string name = progress.Current;
            const float room = right - left - fixed - ImGui::CalcTextSize((text + "  ·  ").c_str()).x;
            while (!name.empty() && ImGui::CalcTextSize((name + "...").c_str()).x > room)
                name.pop_back();
            if (!name.empty())
                full += "  ·  " + (name.size() < progress.Current.size() ? name + "..." : name);
        }
        const float width = fixed + ImGui::CalcTextSize(full.c_str()).x;
        if (right - width < left)
            return;
        ImGui::SameLine();
        ImGui::SetCursorPosX(right - width);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.FramePadding.y);
        Spinner(spinner, ManifoldTheme::Accent);
        ImGui::SameLine(0.0f, Px(6.0f));
        ImGui::TextColored(ManifoldTheme::Label, "%s", full.c_str());
        ImGui::SameLine(0.0f, Px(8.0f));
        if (ImGui::Button(button))
        {
            if (progress.TableScript)
                m_engine.StopTableScript();
            else
                m_engine.CancelActivations();
        }
        ItemTooltip(progress.TableScript ? "Stop the table's Lua script; records you switched meanwhile follow afterwards" : "Drop the records still waiting; the one switching now finishes");
    }

    std::function<void()> MainWindow::StickyHeader()
    {
        ImGuiTable* table = ImGui::GetCurrentTable();
        if (!table || m_spots.size() < 2)
            return {};
        const float rowHeight = m_spots[1].Top - m_spots[0].Top;
        if (rowHeight <= 0.0f)
            return {};
        const float bodyTop = table->InnerClipRect.Min.y + ImGui::TableGetHeaderRowHeight();
        Table::AddressList& list = m_engine.Table();
        const RowSpot* first = nullptr;
        for (const RowSpot& spot : m_spots)
        {
            if (spot.Top + rowHeight * 0.5f > bodyTop)
            {
                first = &spot;
                break;
            }
        }
        if (!first)
            return {};
        const Table::MemoryRecord* record = list.FindById(first->Id);
        if (!record)
            return {};
        const Table::MemoryRecord* pinned = nullptr;
        float pinnedTop = 0.0f;
        for (const Table::MemoryRecord* parent = record->Parent; parent && !pinned; parent = parent->Parent)
        {
            const auto spot = std::find_if(m_spots.begin(), m_spots.end(), [&](const RowSpot& entry) { return entry.Id == parent->Id; });
            if (spot == m_spots.end())
                continue;
            if (spot->Top + rowHeight * 0.5f > bodyTop)
                return {};
            pinned = parent;
            pinnedTop = spot->Top;
        }
        if (!pinned)
            return {};

        struct Piece
        {
            ImVec2 Pos;
            ImU32 Color = 0;
            std::string Text;
        };
        const ImVec2 min(table->InnerClipRect.Min.x, bodyTop);
        const ImVec2 max(table->InnerClipRect.Max.x, bodyTop + rowHeight);
        const ImVec4 card = ManifoldTheme::Card;
        const ImVec4 fill(card.x + (ManifoldTheme::Text.x - card.x) * 0.05f, card.y + (ManifoldTheme::Text.y - card.y) * 0.05f, card.z + (ManifoldTheme::Text.z - card.z) * 0.05f, 1.0f);

        std::vector<const Table::MemoryRecord*> chain;
        for (const Table::MemoryRecord* at = pinned; at; at = at->Parent)
            chain.push_back(at);
        std::reverse(chain.begin(), chain.end());
        const float textY = std::round(min.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
        float x = table->Columns[1].MinX + ImGui::GetStyle().CellPadding.x;
        std::vector<Piece> pieces;
        pieces.push_back({ ImVec2(x, textY), ImGui::GetColorU32(ManifoldTheme::Accent), ICON_FA_FOLDER_OPEN });
        x += ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + Px(8.0f);
        for (std::size_t i = 0; i < chain.size(); ++i)
        {
            const bool inner = i + 1 == chain.size();
            pieces.push_back({ ImVec2(x, textY), ImGui::GetColorU32(inner ? ManifoldTheme::Text : ManifoldTheme::Muted), chain[i]->Description });
            x += ImGui::CalcTextSize(chain[i]->Description.c_str()).x;
            if (!inner)
            {
                x += Px(8.0f);
                pieces.push_back({ ImVec2(x, textY), ImGui::GetColorU32(ManifoldTheme::Muted), ICON_FA_ANGLE_RIGHT });
                x += ImGui::CalcTextSize(ICON_FA_ANGLE_RIGHT).x + Px(8.0f);
            }
        }

        ImGui::PushClipRect(min, max, false);
        ImGui::SetCursorScreenPos(min);
        if (ImGui::InvisibleButton("##stickygroup", ImVec2(max.x - min.x, max.y - min.y)))
            ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - (bodyTop - pinnedTop) - ImGui::GetStyle().CellPadding.y));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ItemTooltip("Scroll up to this group");
        }
        ImGui::PopClipRect();

        return [draw = ImGui::GetWindowDrawList(), clip = table->InnerClipRect, min, max, fillColor = ImGui::GetColorU32(fill),
                   borderColor = ImGui::GetColorU32(ManifoldTheme::Border), line = Px(1.0f), shadow = Px(6.0f), pieces = std::move(pieces)]()
            {
                draw->PushClipRect(clip.Min, clip.Max, false);
                draw->AddRectFilledMultiColor(ImVec2(min.x, max.y), ImVec2(max.x, max.y + shadow), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
                draw->AddRectFilled(min, max, fillColor);
                draw->AddLine(ImVec2(min.x, max.y - 0.5f), ImVec2(max.x, max.y - 0.5f), borderColor, line);
                for (const Piece& piece : pieces)
                    draw->AddText(piece.Pos, piece.Color, piece.Text.c_str());
                draw->PopClipRect();
            };
    }

    std::vector<Table::MemoryRecord*> MainWindow::DraggedRecords(int draggedId)
    {
        Table::AddressList& list = m_engine.Table();
        std::vector<Table::MemoryRecord*> moving;
        Table::MemoryRecord* dragged = list.FindById(draggedId);
        if (!dragged)
            return moving;
        if (!list.Selected(draggedId) || list.Selection().size() < 2)
        {
            moving.push_back(dragged);
            return moving;
        }
        for (Table::MemoryRecord* record : list.Flatten())
        {
            if (!list.Selected(record->Id))
                continue;
            bool covered = false;
            for (const Table::MemoryRecord* parent = record->Parent; parent && !covered; parent = parent->Parent)
                covered = list.Selected(parent->Id);
            if (!covered)
                moving.push_back(record);
        }
        return moving;
    }

    void MainWindow::DropOnRecord(Table::MemoryRecord& record, int depth)
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kRecordDrag, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (!payload)
            return;
        Table::AddressList& list = m_engine.Table();
        std::vector<Table::MemoryRecord*> moving = DraggedRecords(*static_cast<const int*>(payload->Data));
        moving.erase(std::remove_if(moving.begin(), moving.end(), [&](Table::MemoryRecord* candidate) { return candidate == &record || record.IsDescendantOf(candidate); }), moving.end());
        if (moving.empty())
            return;

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float where = (ImGui::GetMousePos().y - min.y) / std::max(1.0f, max.y - min.y);
        const bool expanded = !record.Children.empty() && record.ChildrenVisible();
        enum class Zone { Before, Into, After, FirstChild };
        Zone zone = where < 0.3f ? Zone::Before : (where > 0.7f ? (expanded ? Zone::FirstChild : Zone::After) : Zone::Into);

        ImGuiTable* table = ImGui::GetCurrentTable();
        if (table)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(table->InnerClipRect.Min, table->InnerClipRect.Max, false);
            const ImU32 accent = ImGui::GetColorU32(ManifoldTheme::Accent);
            const float indent = Px(16.0f) * static_cast<float>(depth + (zone == Zone::FirstChild ? 1 : 0));
            const float left = table->Columns[1].MinX + ImGui::GetStyle().CellPadding.x + indent;
            const float right = table->InnerClipRect.Max.x - Px(4.0f);
            if (zone == Zone::Into)
            {
                const ImVec2 boxMin(table->Columns[1].MinX + Px(2.0f), min.y + Px(1.0f));
                const ImVec2 boxMax(right, max.y - Px(1.0f));
                draw->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Accent, 0.14f)), Px(4.0f));
                draw->AddRect(boxMin, boxMax, accent, Px(4.0f), 0, Px(1.5f));
            }
            else
            {
                const float y = std::round(zone == Zone::Before ? min.y : max.y);
                draw->AddLine(ImVec2(left, y), ImVec2(right, y), accent, Px(2.0f));
                draw->AddCircleFilled(ImVec2(left, y), Px(3.5f), accent, 12);
            }
            draw->PopClipRect();
        }
        if (!payload->IsDelivery())
            return;

        switch (zone)
        {
        case Zone::Before:
            for (Table::MemoryRecord* item : moving)
                list.Move(*item, record.Parent, list.IndexOf(record));
            break;
        case Zone::After:
        {
            Table::MemoryRecord* anchor = &record;
            for (Table::MemoryRecord* item : moving)
            {
                list.Move(*item, anchor->Parent, list.IndexOf(*anchor) + 1);
                anchor = item;
            }
            break;
        }
        case Zone::FirstChild:
            for (std::size_t i = 0; i < moving.size(); ++i)
                list.Move(*moving[i], &record, static_cast<int>(i));
            break;
        case Zone::Into:
            for (Table::MemoryRecord* item : moving)
                list.Move(*item, &record, -1);
            record.Expanded = true;
            break;
        }
        list.ClearSelection();
        for (Table::MemoryRecord* item : moving)
            list.Select(item->Id, true);
        m_lastClickedRecord = moving.front()->Id;
        m_anchorRecord = moving.front()->Id;
    }

    void MainWindow::RecordMenu(Table::MemoryRecord& record)
    {
        Engine& engine = m_engine;
        Table::AddressList& list = engine.Table();
        auto selected = SelectedRecords();
        ImGui::TextDisabled("%s", record.Description.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(record.Active ? ICON_FA_TOGGLE_ON "  Deactivate" : ICON_FA_TOGGLE_OFF "  Activate", "Space"))
        {
            for (Table::MemoryRecord* target : selected)
                engine.RequestActive(*target, !record.Active);
        }
        if (m_release && record.Kind != Table::RecordKind::Value)
            return;
        if (record.Kind == Table::RecordKind::Value)
        {
            if (ImGui::MenuItem(ICON_FA_EDIT "  Change value", "Enter") && !engine.Lua().AddressListEvent("onvaluechange", &record))
            {
                m_valueTargets = list.Selection();
                m_valueText = record.ValueText == "??" ? "" : record.ValueText;
                m_valueDialog = true;
            }
            if (ImGui::BeginMenu(ICON_FA_SNOWFLAKE "  Freeze mode"))
            {
                if (ImGui::MenuItem("Normal", nullptr, record.Freeze == Table::FreezeMode::Normal))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::Normal;
                if (ImGui::MenuItem("Allow increase", nullptr, record.Freeze == Table::FreezeMode::AllowIncrease))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::AllowIncrease;
                if (ImGui::MenuItem("Allow decrease", nullptr, record.Freeze == Table::FreezeMode::AllowDecrease))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::AllowDecrease;
                ImGui::EndMenu();
            }
            if (m_release)
                return;
            if (ImGui::MenuItem(ICON_FA_HASHTAG "  Show as hexadecimal", nullptr, record.ShowHex))
            {
                for (auto* target : selected)
                    target->ShowHex = !record.ShowHex;
                engine.RefreshValues(true);
            }
            if (ImGui::MenuItem(ICON_FA_MINUS_CIRCLE "  Show as signed", nullptr, record.ShowSigned))
            {
                for (auto* target : selected)
                    target->ShowSigned = !record.ShowSigned;
                engine.RefreshValues(true);
            }
            if (ImGui::BeginMenu(ICON_FA_EXCHANGE_ALT "  Change type"))
            {
                for (Scan::ValueType type : Scan::kRecordTypes)
                {
                    if (ImGui::MenuItem(Scan::TypeName(type), nullptr, record.Type == type) && !engine.Lua().AddressListEvent("ontypechange", &record))
                    {
                        for (auto* target : selected)
                            target->Type = type;
                        engine.RefreshValues(true);
                    }
                }
                ImGui::EndMenu();
            }
        }
        if (record.Kind == Table::RecordKind::Script && ImGui::MenuItem(ICON_FA_CODE "  Edit script") && !TableHandlesScriptEdit(engine.Lua(), record))
            m_workspace.OpenScript(record.Script, record.Id);
        if (ImGui::MenuItem(ICON_FA_SLIDERS_H "  Edit record..."))
            m_workspace.OpenRecordEditor(record.Id);
        if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Change description") && !engine.Lua().AddressListEvent("ondescriptionchange", &record))
        {
            m_descriptionDialog = true;
            m_descriptionTarget = record.Id;
            m_descriptionText = record.Description;
        }
        if (ImGui::BeginMenu(ICON_FA_SITEMAP "  Group options"))
        {
            ImGui::MenuItem("Hide children when deactivated", nullptr, &record.Options.HideChildren);
            ImGui::MenuItem("Activate children as well", nullptr, &record.Options.ActivateChildren);
            ImGui::MenuItem("Deactivate children as well", nullptr, &record.Options.DeactivateChildren);
            ImGui::MenuItem("Set value of children too", nullptr, &record.Options.RecursiveSetValue);
            ImGui::MenuItem("Always hide children", nullptr, &record.Options.AlwaysHideChildren);
            ImGui::MenuItem("Allow manual collapse and expand", nullptr, &record.Options.AllowManualExpand);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  Put selected into a new group"))
        {
            Table::MemoryRecord* parent = record.Parent;
            const int index = list.IndexOf(record);
            auto group = std::make_unique<Table::MemoryRecord>();
            group->Kind = Table::RecordKind::Group;
            group->Description = "Group";
            Table::MemoryRecord& added = list.Add(std::move(group), parent, index);
            for (Table::MemoryRecord* target : selected)
            {
                if (target != &added && !added.IsDescendantOf(target))
                    list.Move(*target, &added, -1);
            }
        }
        ImGui::Separator();
        if (record.Kind == Table::RecordKind::Value)
        {
            engine.ResolveRecord(record);
            ImGui::BeginDisabled(!record.AddressOk);
            if (ImGui::MenuItem(ICON_FA_MEMORY "  Browse this memory region"))
                m_workspace.OpenMemoryViewer(record.ResolvedAddress, true);
            if (ImGui::MenuItem(ICON_FA_SITEMAP "  Dissect data at this address"))
                m_workspace.OpenStructure(record.ResolvedAddress);
            const int size = record.Size(engine.Process() ? engine.Process()->PointerSize() : 8);
            if (ImGui::MenuItem(ICON_FA_EYE "  Find out what accesses this address", "F5"))
                m_workspace.StartWatch(Debug::WatchKind::Accesses, record.ResolvedAddress, size);
            if (ImGui::MenuItem(ICON_FA_PEN "  Find out what writes to this address", "F6"))
                m_workspace.StartWatch(Debug::WatchKind::Writes, record.ResolvedAddress, size);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                ImGui::SetClipboardText(Hex(record.ResolvedAddress).c_str());
            ImGui::EndDisabled();
            ImGui::Separator();
        }
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C"))
            m_workspace.CopyRecords(list.Selection());
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste into this record"))
            m_workspace.PasteRecords(record.Id);
        if (DangerMenuItem(ICON_FA_TRASH "  Delete", "Del"))
        {
            std::vector<int> ids = list.Selection();
            m_workspace.Confirm("Delete records", "Delete " + std::to_string(ids.size()) + (ids.size() == 1 ? " record" : " records") + " and their children?", [this, ids]()
                {
                    for (int id : ids)
                    {
                        if (Table::MemoryRecord* target = m_engine.Table().FindById(id))
                        {
                            if (target->Active && target->Kind == Table::RecordKind::Script)
                                m_workspace.Report(m_engine.SetActive(*target, false));
                            m_engine.Table().Remove(*target);
                        }
                    }
                }, "Delete");
        }
    }

    void MainWindow::ValueDialog()
    {
        if (m_valueDialog)
        {
            ImGui::OpenPopup("Change value##main");
            m_valueDialog = false;
        }
        Table::MemoryRecord* first = m_valueTargets.empty() ? nullptr : m_engine.Table().FindById(m_valueTargets.front());
        const std::vector<Table::DropDownEntry> noEntries;
        const std::vector<Table::DropDownEntry>& entries = first ? m_engine.DropDownOf(*first) : noEntries;
        if (!BeginModal("Change value##main", ICON_FA_EDIT, "Change value", ImVec2(entries.empty() ? 380.0f : 440.0f, 0.0f)))
            return;
        Muted(m_valueTargets.size() == 1 && first ? first->Description : std::to_string(m_valueTargets.size()) + " records");
        bool apply = false;
        if (!entries.empty())
            apply = ValueChoices(entries);
        if (!first || !first->DropDownOnly || entries.empty())
        {
            if (!entries.empty())
            {
                ImGui::AlignTextToFramePadding();
                Muted("Value");
                ImGui::SameLine(0.0f, Px(10.0f));
            }
            else if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1.0f);
            apply = InputText("##newvalue", m_valueText, ImGuiInputTextFlags_EnterReturnsTrue) || apply;
            if (ImGui::IsItemEdited() && !entries.empty())
            {
                m_valueCursor = FindEntry(entries, m_valueText);
                m_valueScroll = m_valueCursor >= 0 ? 2 : 0;
            }
        }
        ImGui::Spacing();
        if (AccentButton("OK", Px(90.0f, 0.0f)) || apply)
        {
            for (int id : m_valueTargets)
            {
                if (Table::MemoryRecord* record = m_engine.Table().FindById(id))
                    m_workspace.Report(m_engine.WriteRecord(*record, m_valueText));
            }
            m_engine.RefreshValues(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        EndModal();
    }

    bool MainWindow::ValueChoices(const std::vector<Table::DropDownEntry>& entries)
    {
        if (m_valueEntries != entries.size() || m_valueFiltered != m_valueSearch)
        {
            const bool searched = m_valueEntries == entries.size();
            m_valueVisible.clear();
            const std::string_view search = TrimView(m_valueSearch);
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                if (search.empty() || StartsWithNoCase(entries[i].Value, search) || ContainsNoCase(entries[i].Text, search))
                    m_valueVisible.push_back(static_cast<int>(i));
            }
            if (!searched)
            {
                PushMono();
                m_valueColumn = 0.0f;
                for (const Table::DropDownEntry& entry : entries)
                    m_valueColumn = std::max(m_valueColumn, ImGui::CalcTextSize(entry.Value.c_str()).x);
                PopMono();
            }
            m_valueEntries = entries.size();
            m_valueFiltered = m_valueSearch;
            if (!search.empty() && !m_valueVisible.empty() && std::find(m_valueVisible.begin(), m_valueVisible.end(), m_valueCursor) == m_valueVisible.end())
            {
                m_valueCursor = m_valueVisible.front();
                m_valueText = entries[static_cast<std::size_t>(m_valueCursor)].Value;
                m_valueScroll = 2;
            }
        }

        int steps = 0;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string hint = ICON_FA_SEARCH "  Search " + std::to_string(entries.size()) + (entries.size() == 1 ? " entry" : " entries");
        const bool enter = InputTextWithHint("##valuesearch", hint.c_str(), m_valueSearch, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, &ArrowSteps, &steps);

        const auto row = [&](int index) -> int
        {
            const auto at = std::find(m_valueVisible.begin(), m_valueVisible.end(), index);
            return at == m_valueVisible.end() ? -1 : static_cast<int>(at - m_valueVisible.begin());
        };
        if (steps != 0 && !m_valueVisible.empty())
        {
            const int count = static_cast<int>(m_valueVisible.size());
            int at = row(m_valueCursor);
            at = at < 0 ? (steps > 0 ? 0 : count - 1) : std::clamp(at + steps, 0, count - 1);
            m_valueCursor = m_valueVisible[static_cast<std::size_t>(at)];
            m_valueText = entries[static_cast<std::size_t>(m_valueCursor)].Value;
            m_valueScroll = 2;
        }
        bool picked = enter && row(m_valueCursor) >= 0;

        const ImGuiStyle& style = ImGui::GetStyle();
        const float rowHeight = ImGui::GetFrameHeight();
        const int shown = std::clamp(static_cast<int>(entries.size()), 3, 12);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(4.0f), Px(4.0f)));
        const bool open = ImGui::BeginChild("##valuelist", ImVec2(-FLT_MIN, rowHeight * static_cast<float>(shown) + Px(8.0f)), ImGuiChildFlags_FrameStyle);
        ImGui::PopStyleVar();
        if (open)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
            const int cursorRow = row(m_valueCursor);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_valueVisible.size()), rowHeight);
            if (m_valueScroll != 0 && cursorRow >= 0)
                clipper.IncludeItemByIndex(cursorRow);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            while (clipper.Step())
            {
                for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line)
                {
                    const int index = m_valueVisible[static_cast<std::size_t>(line)];
                    const Table::DropDownEntry& entry = entries[static_cast<std::size_t>(index)];
                    const bool current = EqualsNoCase(entry.Value, m_valueCurrent);
                    ImGui::PushID(index);
                    const ImVec2 start = ImGui::GetCursorScreenPos();
                    const float right = start.x + ImGui::GetContentRegionAvail().x;
                    if (ImGui::Selectable("##entry", index == m_valueCursor, ImGuiSelectableFlags_None, ImVec2(0.0f, rowHeight)))
                    {
                        m_valueCursor = index;
                        m_valueText = entry.Value;
                        picked = true;
                    }
                    if (line == cursorRow && m_valueScroll != 0)
                    {
                        if (m_valueScroll == 1)
                            ImGui::SetScrollHereY(0.5f);
                        else
                            ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                        m_valueScroll = 0;
                    }
                    PushMono();
                    const ImVec2 valueSize = ImGui::CalcTextSize(entry.Value.c_str());
                    draw->AddText(ImVec2(start.x + Px(6.0f) + m_valueColumn - valueSize.x, start.y + std::floor((rowHeight - valueSize.y) * 0.5f)),
                        ImGui::GetColorU32(current ? ManifoldTheme::Accent : ManifoldTheme::Muted), entry.Value.c_str());
                    PopMono();
                    const float textX = start.x + Px(6.0f) + m_valueColumn + Px(14.0f);
                    const float markWidth = current ? ImGui::CalcTextSize(ICON_FA_CHECK).x + Px(8.0f) : 0.0f;
                    const float textY = start.y + std::floor((rowHeight - ImGui::GetTextLineHeight()) * 0.5f);
                    ImGui::PushStyleColor(ImGuiCol_Text, current ? ManifoldTheme::Accent : ManifoldTheme::Text);
                    ImGui::RenderTextEllipsis(draw, ImVec2(textX, textY), ImVec2(right - markWidth, textY + ImGui::GetTextLineHeight()), right - markWidth,
                        entry.Text.c_str(), entry.Text.c_str() + entry.Text.size(), nullptr);
                    ImGui::PopStyleColor();
                    if (current)
                        draw->AddText(ImVec2(right - markWidth + Px(2.0f), textY), ImGui::GetColorU32(ManifoldTheme::Accent), ICON_FA_CHECK);
                    ImGui::PopID();
                }
            }
            if (cursorRow < 0)
                m_valueScroll = 0;
            ImGui::PopStyleVar();
            if (m_valueVisible.empty())
            {
                ImGui::SetCursorPos(ImVec2(Px(8.0f), Px(8.0f)));
                Muted("No entry matches \"" + m_valueSearch + "\"");
            }
        }
        ImGui::EndChild();
        return picked;
    }

    void MainWindow::AddAddressDialog()
    {
        if (m_addDialog)
        {
            ImGui::OpenPopup("Add address##main");
            m_addDialog = false;
        }
        if (!BeginModal("Add address##main", ICON_FA_PLUS, "Add address", ImVec2(420.0f, 0.0f)))
            return;
        ImGui::TextUnformatted("Address");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        InputTextWithHint("##address", "game.exe+1234 or [[base]+10]+8", m_addAddress);
        if (!m_addAddress.empty())
        {
            Status error;
            if (const auto value = m_engine.Evaluate(m_addAddress, &error))
                Muted("= " + Hex(*value) + "  " + m_engine.Describe(*value));
            else
                TextWithError(error.Text, ManifoldTheme::Error);
        }
        ImGui::TextUnformatted("Description");
        ImGui::SetNextItemWidth(-1.0f);
        InputText("##description", m_addDescription);
        ImGui::TextUnformatted("Type");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##addtype", Scan::TypeName(Scan::kRecordTypes[m_addType])))
        {
            for (int i = 0; i < static_cast<int>(std::size(Scan::kRecordTypes)); ++i)
            {
                if (ImGui::Selectable(Scan::TypeName(Scan::kRecordTypes[i]), i == m_addType))
                    m_addType = i;
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        if (AccentButton("Add", Px(90.0f, 0.0f)) || (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !m_addAddress.empty()))
        {
            m_engine.Table().AddValue(m_addDescription, Trim(m_addAddress), Scan::kRecordTypes[m_addType]);
            m_engine.RefreshValues(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        EndModal();
    }

    void MainWindow::DescriptionDialog()
    {
        if (m_descriptionDialog)
        {
            ImGui::OpenPopup("Description##main");
            m_descriptionDialog = false;
        }
        if (!BeginModal("Description##main", ICON_FA_I_CURSOR, "Change description", ImVec2(380.0f, 0.0f)))
            return;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = InputText("##text", m_descriptionText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (AccentButton("OK", Px(90.0f, 0.0f)) || enter)
        {
            if (Table::MemoryRecord* record = m_engine.Table().FindById(m_descriptionTarget))
            {
                record->Description = m_descriptionText;
                m_engine.Table().Touch();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        EndModal();
    }

    void MainWindow::SaveOptions()
    {
        m_workspace.Report(m_engine.Options().Save(Paths::SettingsFile()));
    }

    void MainWindow::OpenValueDialog(Table::MemoryRecord& record)
    {
        Table::AddressList& list = m_engine.Table();
        if (!list.Selected(record.Id))
            list.Select(record.Id, false);
        m_valueTargets = list.Selection();
        m_valueText = record.ValueText == "??" ? "" : record.ValueText;
        m_valueSearch.clear();
        m_valueEntries = static_cast<std::size_t>(-1);
        m_valueCursor = -1;
        m_valueScroll = 0;
        const std::vector<Table::DropDownEntry>& entries = m_engine.DropDownOf(record);
        if (!entries.empty())
        {
            const std::size_t colon = m_valueText.find(" : ");
            if (colon != std::string::npos)
                m_valueText.resize(colon);
            m_valueCursor = FindEntry(entries, m_valueText);
            if (m_valueCursor >= 0)
            {
                m_valueText = entries[static_cast<std::size_t>(m_valueCursor)].Value;
                m_valueScroll = 1;
            }
        }
        m_valueCurrent = m_valueText;
        m_valueDialog = true;
    }

    void MainWindow::StatusBar()
    {
        Engine& engine = m_engine;
        const float height = Px(kStatusHeight);
        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        ImGui::SetCursorPos(ImVec2(Px(14.0f), windowSize.y - height + std::round((height - ImGui::GetTextLineHeight()) * 0.5f) - Px(1.0f)));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(windowPos.x, windowPos.y + windowSize.y - height), ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y - height),
            ImGui::GetColorU32(ManifoldTheme::RowSeparator));
        const std::string& path = engine.Table().FilePath;
        std::string table = path.empty() ? "Untitled table" : WideToUtf8(std::filesystem::path(Utf8ToWide(path)).filename().wstring());
        if (engine.Table().Dirty())
            table += " *";
        Muted(ICON_FA_FILE_ALT "  " + table);
        ImGui::SameLine(0.0f, Px(18.0f));
        Muted(Target::IsElevated() ? ICON_FA_SHIELD_ALT "  Administrator" : ICON_FA_USER "  No administrator rights");

        if (const std::optional<LogEntry> newest = RuntimeLog::Last())
        {
            const LogEntry& last = *newest;
            const std::string text = std::string(LevelIcon(last.Level)) + "  " + (m_release ? Workspace::WithoutCodes(last.Message) : last.Message);
            const float textWidth = std::min(ImGui::CalcTextSize(text.c_str()).x, windowSize.x * 0.55f);
            ImGui::SameLine();
            ImGui::SetCursorPosX(windowSize.x - textWidth - Px(14.0f));
            ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(windowPos.x + windowSize.x - Px(10.0f), windowPos.y + windowSize.y), true);
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::WithAlpha(LevelColor(last.Level), 0.85f));
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopClipRect();
            if (!m_release)
            {
                if (!ErrorTooltip(last.Message) && ImGui::IsItemClicked())
                    m_workspace.OpenLog();
                ItemTooltip("Click to open the log");
            }
        }
    }
}
