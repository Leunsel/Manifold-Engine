#include "UI/Windows.h"

#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/ProfilerWindow.h"
#include "UI/ScriptUi.h"
#include "UI/Shortcuts.h"
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
        constexpr std::size_t kUnclippedRows = 250;

        const Scan::Compare kCompares[] = { Scan::Compare::Exact, Scan::Compare::Bigger, Scan::Compare::Smaller, Scan::Compare::Between, Scan::Compare::Unknown,
            Scan::Compare::Increased, Scan::Compare::IncreasedBy, Scan::Compare::Decreased, Scan::Compare::DecreasedBy, Scan::Compare::Changed,
            Scan::Compare::Unchanged, Scan::Compare::SameAsFirst };

        // Text of one status bar part in a slot of fixed width, ending in "..." when it
        // does not fit. Returns true when the text was cut.
        bool StatusText(float x, float y, std::string_view text, float width, const ImVec4& color)
        {
            const char* begin = text.data();
            const char* end = begin + text.size();
            const ImVec2 size = ImGui::CalcTextSize(begin, end);
            ImGui::SetCursorPos(ImVec2(x, y));
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImRect rect(pos, ImVec2(pos.x + std::max(0.0f, width), pos.y + size.y));
            ImGui::ItemSize(rect.GetSize());
            if (ImGui::ItemAdd(rect, 0))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), rect.Min, rect.Max, rect.Max.x, begin, end, &size);
                ImGui::PopStyleColor();
            }
            return size.x > width + 0.5f;
        }

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
        SetLegacyCodeEditor(m_engine.Options().LegacyCodeEditor);
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

        {
            PROFILE_ZONE("Title bar and menus");
            TitleBar();
        }
        ImGui::SetCursorPos(ImVec2(Px(10.0f), Px(kTitleHeight) + Px(8.0f)));
        const float width = size.x - Px(20.0f);
        const float height = size.y - Px(kTitleHeight) - Px(16.0f) - Px(kStatusHeight);
        const ImVec2 bodyPos = ImGui::GetCursorScreenPos();
        ImVec2 innerPos = bodyPos;
        ImVec2 innerSize(width, height);
        {
            PROFILE_ZONE("Docked tool windows");
            const ImGuiID dock = ImGui::GetID("##maindock");
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(Px(380.0f), Px(300.0f)));
            ImGui::DockSpace(dock, ImVec2(width, height), ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode);
            ImGui::PopStyleVar();
            if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock))
            {
                const float gap = Px(6.0f);
                const float left = central->Pos.x > bodyPos.x + 0.5f ? gap : 0.0f;
                const float top = central->Pos.y > bodyPos.y + 0.5f ? gap : 0.0f;
                const float right = central->Pos.x + central->Size.x < bodyPos.x + width - 0.5f ? gap : 0.0f;
                const float bottom = central->Pos.y + central->Size.y < bodyPos.y + height - 0.5f ? gap : 0.0f;
                innerPos = ImVec2(central->Pos.x + left, central->Pos.y + top);
                innerSize = ImVec2(std::max(Px(40.0f), central->Size.x - left - right), std::max(Px(40.0f), central->Size.y - top - bottom));
            }
        }
        ImGui::SetCursorScreenPos(innerPos);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, PxWhole(8.0f, 8.0f));
        ImGui::BeginChild("##body", innerSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        m_release = m_engine.Options().ReleaseMode;
        {
            PROFILE_ZONE("Process bar");
            ProcessBar();
        }
        if (m_engine.Options().CompactMode)
        {
            PROFILE_ZONE("Address list");
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
            {
                PROFILE_ZONE("Scanner and found list");
                ScanArea(m_topHeight);
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(2.0f));
            PROFILE_ZONE("Address list");
            AddressListCard(ImGui::GetContentRegionAvail().y);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        {
            PROFILE_ZONE("Status bar");
            StatusBar();
        }
        MainShortcuts();

        ValueDialog();
        ResultValueDialog();
        SaveResultsDialog();
        AddAddressDialog();
        DescriptionDialog();
        ImGui::End();
    }

    void MainWindow::MainShortcuts()
    {
        Engine& engine = m_engine;
        Scan::Scanner& scanner = engine.Scanner();
        const bool scannerShown = !engine.Options().CompactMode;
        if (Pressed(Command::NewTable))
            m_workspace.NewTable();
        if (Pressed(Command::OpenLastProcess))
            m_workspace.OpenLastProcess();
        if (Pressed(Command::DeactivateAll))
            m_workspace.TurnOffEverything();
        if (Pressed(Command::CompactMode))
            ToggleCompactMode();
        if (Pressed(Command::ReleaseMode))
            ToggleReleaseMode();
        if (Pressed(Command::FocusScanValue) && scannerShown)
            m_focusValue = true;
        if (Pressed(Command::FirstScan) && scannerShown && !scanner.Busy())
        {
            if (scanner.HasResults())
                DoNextScan();
            else
                DoFirstScan();
        }
        if (Pressed(Command::NewScan) && scannerShown && !scanner.Busy() && scanner.HasResults())
            NewScan();
        if (Pressed(Command::UndoScan) && scannerShown && !scanner.Busy() && scanner.CanUndo())
            scanner.Undo();
        ScriptMenuShortcuts();
        if (m_release)
            return;
        if (Pressed(Command::TableLuaScript))
            m_workspace.OpenScript(engine.Table().TableLuaScript, kTableLuaScript, true);
        if (Pressed(Command::TableFiles))
            m_workspace.OpenTableFiles();
        if (Pressed(Command::AddAddress))
            OpenAddDialog();
        if (Pressed(Command::AddGroup))
            engine.Table().AddGroup("Group");
        if (Pressed(Command::AddScript))
            AddScriptRecord();
    }

    void MainWindow::ToggleCompactMode()
    {
        Settings& options = m_engine.Options();
        options.CompactMode = !options.CompactMode;
        SaveOptions();
    }

    void MainWindow::ToggleReleaseMode()
    {
        Settings& options = m_engine.Options();
        options.ReleaseMode = !options.ReleaseMode;
        SaveOptions();
    }

    void MainWindow::OpenAddDialog()
    {
        m_addDialog = true;
        m_addAddress.clear();
        m_addDescription = "No description";
    }

    void MainWindow::AddScriptRecord()
    {
        Table::MemoryRecord& record = m_engine.Table().AddScript("Auto Assemble script", Asm::CheatTableTemplate());
        m_workspace.OpenScript(record.Script, record.Id);
    }

    void MainWindow::NewScan()
    {
        m_engine.Scanner().Reset();
        m_selectedResults.clear();
        m_compareTo = Scan::CompareTo::Previous;
        m_savedName.clear();
        if (!Scan::CompareAllowedFirst(kCompares[m_compareIndex]))
            m_compareIndex = 0;
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
        RefreshScriptMenu();
        DetectMono();
        float scriptMenusWidth = 0.0f;
        for (const Lua::ScriptMenuItem& root : m_scriptMenu)
        {
            if (!ScriptMenuMerged(root) && root.Caption != "-")
                scriptMenusWidth += ImGui::CalcTextSize(root.Caption.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        }
        if (m_monoDetected && !m_release)
            scriptMenusWidth += ImGui::CalcTextSize("Mono").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::BeginChild("##menus", ImVec2(Px(540.0f) + scriptMenusWidth, menuHeight), ImGuiChildFlags_None, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        float menusEnd = origin.x + menusStart;

        Engine& engine = m_engine;
        Workspace& workspace = m_workspace;
        const bool hasProcess = engine.HasProcess();
        Table::AddressList& list = engine.Table();
        int scriptOpened = 0;
        int scriptClicked = 0;
        ImGui::BeginMenuBar();
        if (ImGui::BeginMenu("File"))
        {
            if (CommandItem(ICON_FA_FILE "  New table", Command::NewTable))
                workspace.NewTable();
            if (CommandItem(ICON_FA_FOLDER_OPEN "  Open table...", Command::OpenTable))
                workspace.OpenTableDialog(false);
            if (!m_release && ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Merge table..."))
                workspace.OpenTableDialog(true);
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
            if (CommandItem(ICON_FA_SAVE "  Save", Command::SaveTable))
                workspace.SaveTable(false);
            if (CommandItem(ICON_FA_SAVE "  Save as...", Command::SaveTableAs))
                workspace.SaveTable(true);
            ScriptMenuInto("file", scriptOpened, scriptClicked);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_SIGN_OUT_ALT "  Exit"))
                workspace.RequestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit"))
        {
            if (!m_release)
            {
                if (CommandItem(ICON_FA_UNDO "  Undo last scan", Command::UndoScan, false, engine.Scanner().CanUndo() && !engine.Scanner().Busy()))
                    engine.Scanner().Undo();
                if (CommandItem(ICON_FA_UNDO_ALT "  Undo last value change", Command::UndoRecordValue, false, !m_valueUndo.empty()))
                    UndoValueChange();
                ImGui::Separator();
                const bool selection = !list.Selection().empty();
                if (CommandItem(ICON_FA_CUT "  Cut records", Command::CutRecords, false, selection))
                    RunRecordCommand(Command::CutRecords, nullptr);
                if (CommandItem(ICON_FA_COPY "  Copy records", Command::CopyRecords, false, selection))
                    RunRecordCommand(Command::CopyRecords, nullptr);
                if (CommandItem(ICON_FA_PASTE "  Paste records", Command::PasteRecords))
                    RunRecordCommand(Command::PasteRecords, nullptr);
                if (CommandItem(ICON_FA_CHECK_DOUBLE "  Select all records", Command::SelectAllRecords))
                    RunRecordCommand(Command::SelectAllRecords, nullptr);
                if (DangerCommandItem(ICON_FA_TRASH "  Delete records", Command::DeleteRecords, selection))
                    RunRecordCommand(Command::DeleteRecords, nullptr);
                ImGui::Separator();
            }
            if (CommandItem(ICON_FA_COG "  Settings", Command::OpenSettings))
                workspace.OpenSettings();
            ScriptMenuInto("edit", scriptOpened, scriptClicked);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Process"))
        {
            if (CommandItem(ICON_FA_DESKTOP "  Open process...", Command::OpenProcess))
                workspace.OpenProcessPicker();
            const std::string& last = engine.Options().LastProcess;
            const std::string lastLabel = ICON_FA_HISTORY "  Open " + (last.empty() ? std::string("the last process") : last);
            if (CommandItem(lastLabel.c_str(), Command::OpenLastProcess, false, workspace.CanOpenLastProcess()))
                workspace.OpenLastProcess();
            if (ImGui::MenuItem(ICON_FA_TIMES "  Close process", nullptr, false, hasProcess))
                engine.CloseProcess();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Table"))
        {
            if (!m_release)
            {
                if (CommandItem(ICON_FA_PLUS "  Add address manually", Command::AddAddress))
                    OpenAddDialog();
                if (CommandItem(ICON_FA_FOLDER_PLUS "  Add group", Command::AddGroup))
                    list.AddGroup("Group");
                if (CommandItem(ICON_FA_CODE "  Add Auto Assembler script", Command::AddScript))
                    AddScriptRecord();
                ImGui::Separator();
                if (CommandItem(ICON_FA_SCROLL "  Table Lua script", Command::TableLuaScript))
                    workspace.OpenScript(list.TableLuaScript, kTableLuaScript, true);
                if (CommandItem(ICON_FA_FILE_ARCHIVE "  Table files", Command::TableFiles))
                    workspace.OpenTableFiles();
                ImGui::Separator();
            }
            if (CommandItem(ICON_FA_TOGGLE_OFF "  Deactivate all records", Command::DeactivateAll))
                workspace.TurnOffEverything();
            ScriptMenuInto("table", scriptOpened, scriptClicked);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            Settings& options = engine.Options();
            if (CommandItem(ICON_FA_COMPRESS_ALT "  Compact mode", Command::CompactMode, options.CompactMode))
                ToggleCompactMode();
            ItemTooltip("Only the address list, without the scanner");
            if (CommandItem(ICON_FA_GAMEPAD "  Release mode", Command::ReleaseMode, options.ReleaseMode))
                ToggleReleaseMode();
            ItemTooltip("Hides addresses, types and the developer tools, so a table is simple to use. Turn it off here again.");
            ImGui::EndMenu();
        }
        if (!m_release && ImGui::BeginMenu("Tools"))
        {
            if (CommandItem(ICON_FA_MEMORY "  Memory viewer", Command::OpenMemoryViewer))
                workspace.OpenMemoryViewer(0);
            if (ImGui::MenuItem(ICON_FA_CLONE "  New memory viewer window"))
                workspace.OpenMemoryViewer(0, false, true);
            if (CommandItem(ICON_FA_SITEMAP "  Structure dissect", Command::OpenStructureDissect))
                workspace.OpenStructure(0);
            if (CommandItem(ICON_FA_CODE "  Auto Assembler", Command::OpenAutoAssembler))
                workspace.OpenScript(Asm::CheatTableTemplate());
            if (CommandItem(ICON_FA_MOON "  Lua engine", Command::OpenLuaEngine))
                workspace.OpenLua();
            if (CommandItem(ICON_FA_DICE_D6 "  Mono / .NET dissector", Command::OpenMono))
                workspace.OpenMono();
            if (CommandItem(ICON_FA_STREAM "  Log", Command::OpenLog))
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
            ImGui::Separator();
            SectionLabel("Code, memory and debugger tools are in the memory viewer's menus");
            ImGui::EndMenu();
        }
        if (m_monoDetected && !m_release)
            MonoMenu();
        for (const Lua::ScriptMenuItem& root : m_scriptMenu)
        {
            if (ScriptMenuMerged(root) || root.Caption == "-")
                continue;
            const std::string label = root.Caption + "##mainscript" + std::to_string(root.Id);
            if (root.Children.empty())
            {
                if (ImGui::MenuItem(label.c_str(), nullptr, root.Checked, root.Enabled))
                    scriptClicked = root.Id;
                continue;
            }
            if (ImGui::BeginMenu(label.c_str(), root.Enabled))
            {
                if (ImGui::IsWindowAppearing())
                    scriptOpened = root.Id;
                if (const int inner = DrawScriptMenuItems(root.Children))
                    scriptClicked = inner;
                ImGui::EndMenu();
            }
        }
        if (ImGui::BeginMenu("Help"))
        {
            if (CommandItem(ICON_FA_KEYBOARD "  Keyboard shortcuts", Command::OpenShortcuts))
                workspace.OpenShortcuts();
            if (!m_release && ImGui::MenuItem(ICON_FA_LIST_OL "  Error codes"))
                workspace.OpenLog();
            if (engine.Options().ProfilerEnabled && CommandItem(ICON_FA_TACHOMETER_ALT "  Profiler and diagnostics", Command::OpenProfiler))
                workspace.OpenProfiler();
            if (!Target::IsElevated() && ImGui::MenuItem(ICON_FA_SHIELD_ALT "  Restart as administrator"))
                workspace.Window().RestartElevated();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_INFO_CIRCLE "  About"))
                workspace.OpenAbout();
            ScriptMenuInto("help", scriptOpened, scriptClicked);
            ImGui::EndMenu();
        }
        menusEnd = ImGui::GetItemRectMax().x;
        ImGui::EndMenuBar();
        ImGui::EndChild();
        if (scriptOpened)
            engine.Lua().ClickMenuItem(scriptOpened);
        if (scriptClicked)
            engine.Lua().ClickMenuItem(scriptClicked);
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
        ItemTooltip(WithKeys("Pick the process to work on", Command::OpenProcess).c_str());
        ImGui::SameLine(0.0f, Px(12.0f));

        const float right = ImGui::GetWindowContentRegionMax().x;
        Debug::Debugger& debugger = engine.Debugger();
        std::string debugText = !m_release && debugger.Attached() ? (debugger.Broken() ? ICON_FA_PAUSE_CIRCLE "  Broken" : ICON_FA_BUG "  Debugger attached") : "";
        float x = right;
        if (!m_release)
            x -= ImGui::CalcTextSize(ICON_FA_MEMORY "  Memory view").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float debugWidth = debugText.empty() ? 0.0f : ImGui::CalcTextSize(debugText.c_str()).x + Px(16.0f);
        const float textRoom = x - debugWidth - Px(12.0f) - ImGui::GetCursorPosX();
        if (hasProcess)
        {
            const auto& process = *engine.Process();
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            const float nameWidth = std::min(ImGui::CalcTextSize(process.Name().c_str()).x, std::max(Px(60.0f), textRoom));
            TextFitted(process.Name(), nameWidth, ManifoldTheme::Text);
            ImGui::PopFont();
            const float detailRoom = textRoom - nameWidth - ImGui::GetStyle().ItemSpacing.x;
            if (detailRoom > Px(40.0f))
            {
                ImGui::SameLine();
                if (m_release)
                    TextFitted(process.Alive() ? "Opened" : "Closed", detailRoom, ManifoldTheme::Muted);
                else
                    TextFitted(Format("pid %u  ·  %s  ·  %zu modules", process.Id(), process.Is64Bit() ? "64-bit" : "32-bit", process.Modules()->size()), detailRoom, ManifoldTheme::Muted);
            }
        }
        else
        {
            TextFitted(m_release ? "No game opened yet." : "No process opened. Pick one to scan, view memory or run scripts.", textRoom, ManifoldTheme::Muted);
        }
        if (m_release)
        {
            EndCard();
            return;
        }
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(x, y));
        if (ImGui::Button(ICON_FA_MEMORY "  Memory view"))
            m_workspace.OpenMemoryViewer(0);
        ItemTooltip(WithKeys("Disassembler and hex view of the process", Command::OpenMemoryViewer).c_str());
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
        const ImVec2 start = ImGui::GetCursorPos();
        const float gap = Px(8.0f);
        if (total < Px(260.0f) + Px(300.0f) + gap)
        {
            float top = std::floor(height * m_stackShare);
            float bottom = height - top - gap;
            ResultsCard(total, top);
            ImGui::SetCursorPos(start);
            if (Splitter("##scanstack", false, gap, &top, &bottom, Px(90.0f), Px(150.0f)))
                m_stackShare = top / height;
            ImGui::SetCursorPos(ImVec2(start.x, start.y + top + gap));
            ScanCard(total, bottom);
            ImGui::SetCursorPos(ImVec2(start.x, start.y + height + Px(6.0f)));
            return;
        }
        float left = std::floor(total * m_resultsShare);
        float right = total - left - gap;
        ResultsCard(left, height);
        ImGui::SetCursorPos(start);
        if (Splitter("##scansplit", true, Px(8.0f), &left, &right, Px(260.0f), Px(300.0f), height))
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
                        AddResult(record);
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
        const bool showType = scanner.Kind() == Scan::ScanKind::All;
        const bool wideValues = scanner.Kind() == Scan::ScanKind::Grouped;
        if (ImGui::BeginTable(showType ? "##foundall" : "##found", showType ? 5 : 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            if (showType)
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, wideValues ? 2.0f : 1.0f);
            ImGui::TableSetupColumn("Previous", ImGuiTableColumnFlags_WidthStretch, wideValues ? 2.0f : 1.0f);
            ImGui::TableSetupColumn("First", ImGuiTableColumnFlags_WidthStretch, wideValues ? 2.0f : 1.0f);
            ImGui::TableHeadersRow();
            const int shown = static_cast<int>(std::min<std::uint64_t>(count, 50'000'000ull));
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
                            AddResult(record);
                    }
                    if (ImGui::BeginPopupContextItem("##resultmenu"))
                    {
                        if (!selected)
                        {
                            m_selectedResults.clear();
                            m_selectedResults.insert(static_cast<std::uint64_t>(row));
                        }
                        PopMono();
                        ResultMenu(static_cast<std::uint64_t>(row));
                        PushMono();
                        ImGui::EndPopup();
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, isStatic ? ManifoldTheme::Accent : ManifoldTheme::Text);
                    ImGui::TextUnformatted(isStatic ? address.c_str() : Hex(record.Address, engine.Process() && engine.Process()->Is64Bit() ? 12 : 8).c_str());
                    ImGui::PopStyleColor();
                    if (isStatic)
                        ItemTooltip("Static address inside a module: it stays the same after the game restarts.");
                    ImGui::PopID();
                    if (showType)
                    {
                        ImGui::TableNextColumn();
                        ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                        ImGui::TextUnformatted(Scan::TypeName(record.Type));
                        ImGui::PopStyleColor();
                    }

                    auto live = m_liveValues.find(static_cast<std::uint64_t>(row));
                    if (live == m_liveValues.end())
                    {
                        std::string value = "??";
                        if (engine.HasProcess())
                        {
                            std::vector<std::uint8_t> bytes(record.Previous.size());
                            if (!bytes.empty() && engine.Process()->ReadExact(record.Address, bytes.data(), bytes.size()))
                                value = ResultText(record, bytes);
                        }
                        live = m_liveValues.emplace(static_cast<std::uint64_t>(row), value).first;
                    }
                    const std::string previous = ResultText(record, record.Previous);
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, live->second != previous ? ManifoldTheme::Changed : ManifoldTheme::Text);
                    ImGui::TextUnformatted(live->second.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(previous.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Muted);
                    ImGui::TextUnformatted(ResultText(record, record.First).c_str());
                    ImGui::PopStyleColor();
                }
            }
            for (const CommandInfo& info : AllCommands())
            {
                if (info.Scope == CommandScope::ScanResults && Pressed(info.Id) && !m_selectedResults.empty())
                    RunResultCommand(info.Id, m_selectedResults.count(m_lastClickedResult) ? m_lastClickedResult : *m_selectedResults.begin());
            }
            ImGui::EndTable();
        }
        PopMono();
        EndCard();
    }

    void MainWindow::ResultMenu(std::uint64_t index)
    {
        if (CommandItem(ICON_FA_ARROW_DOWN "  Add selected to the address list", Command::AddResults))
            RunResultCommand(Command::AddResults, index);
        if (!m_release && CommandItem(ICON_FA_EDIT "  Change the value of the selected results", Command::ChangeResultValue))
            RunResultCommand(Command::ChangeResultValue, index);
        if (m_release)
            return;
        ImGui::Separator();
        if (CommandItem(ICON_FA_MEMORY "  Browse this memory region", Command::BrowseResult))
            RunResultCommand(Command::BrowseResult, index);
        if (CommandItem(ICON_FA_CODE "  Disassemble this memory region", Command::DisassembleResult))
            RunResultCommand(Command::DisassembleResult, index);
        if (CommandItem(ICON_FA_SITEMAP "  Dissect data at this address", Command::DissectResult))
            RunResultCommand(Command::DissectResult, index);
        ImGui::Separator();
        if (CommandItem(ICON_FA_EYE "  Find out what accesses this address", Command::ResultAccesses))
            RunResultCommand(Command::ResultAccesses, index);
        if (CommandItem(ICON_FA_PEN "  Find out what writes to this address", Command::ResultWrites))
            RunResultCommand(Command::ResultWrites, index);
        ImGui::Separator();
        if (CommandItem(ICON_FA_COPY "  Copy the selected addresses", Command::CopyResults))
            RunResultCommand(Command::CopyResults, index);
        if (CommandItem(ICON_FA_CHECK_DOUBLE "  Select all results", Command::SelectAllResults))
            RunResultCommand(Command::SelectAllResults, index);
    }

    void MainWindow::RunResultCommand(Command command, std::uint64_t index)
    {
        Scan::Scanner& scanner = m_engine.Scanner();
        Scan::ScanRecord focus;
        if (!scanner.Record(index, focus))
            return;
        const auto selection = [&]()
            {
                std::vector<Scan::ScanRecord> found;
                for (std::uint64_t selected : m_selectedResults)
                {
                    Scan::ScanRecord record;
                    if (scanner.Record(selected, record))
                        found.push_back(std::move(record));
                }
                return found;
            };
        const auto addresses = [&]()
            {
                std::vector<std::uint64_t> found;
                for (const Scan::ScanRecord& record : selection())
                    found.push_back(record.Address);
                return found;
            };
        int size = scanner.Kind() == Scan::ScanKind::All ? Scan::ValueSize(focus.Type, scanner.Format()) : static_cast<int>(focus.Previous.size());
        if (scanner.Kind() == Scan::ScanKind::Single)
            size = Scan::ValueSize(scanner.Type(), scanner.Format());
        if (const auto group = scanner.Group(); group && scanner.Kind() == Scan::ScanKind::Grouped && !group->Elements.empty())
            size = group->Elements.front().Size;
        size = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
        if (m_release && command != Command::AddResults && command != Command::SelectAllResults)
            return;
        switch (command)
        {
        case Command::AddResults:
            for (const Scan::ScanRecord& record : selection())
                AddResult(record);
            break;
        case Command::ChangeResultValue:
            if (scanner.Kind() == Scan::ScanKind::Grouped)
            {
                m_engine.Notify(LogLevel::Info, "Grouped results hold several values. Add them to the address list to change one of them.");
                break;
            }
            m_resultValueTargets.clear();
            for (const Scan::ScanRecord& record : selection())
                m_resultValueTargets.push_back({ record.Address, record.Type });
            m_resultValueText = m_liveValues.count(index) && m_liveValues[index] != "??" ? m_liveValues[index] : "";
            m_resultValueDialog = true;
            break;
        case Command::BrowseResult: m_workspace.OpenMemoryViewer(focus.Address, true); break;
        case Command::DisassembleResult: m_workspace.OpenMemoryViewer(focus.Address, false); break;
        case Command::DissectResult: m_workspace.OpenStructure(focus.Address); break;
        case Command::ResultAccesses: m_workspace.StartWatch(Debug::WatchKind::Accesses, focus.Address, size); break;
        case Command::ResultWrites: m_workspace.StartWatch(Debug::WatchKind::Writes, focus.Address, size); break;
        case Command::CopyResults:
        {
            std::string text;
            for (std::uint64_t address : addresses())
                text += Hex(address) + "\n";
            ImGui::SetClipboardText(text.c_str());
            break;
        }
        case Command::SelectAllResults:
        {
            const std::uint64_t count = std::min<std::uint64_t>(scanner.Count(), 100000);
            for (std::uint64_t i = 0; i < count; ++i)
                m_selectedResults.insert(i);
            if (scanner.Count() > count)
                m_engine.Notify(LogLevel::Info, "Selected the first " + Grouped(count) + " results.");
            break;
        }
        default:
            break;
        }
    }

    void MainWindow::ResultValueDialog()
    {
        if (m_resultValueDialog)
        {
            ImGui::OpenPopup("Change value##results");
            m_resultValueDialog = false;
        }
        if (!BeginModal("Change value##results", ICON_FA_EDIT, "Change value", ImVec2(380.0f, 0.0f)))
            return;
        Scan::Scanner& scanner = m_engine.Scanner();
        Muted(std::to_string(m_resultValueTargets.size()) + (m_resultValueTargets.size() == 1 ? " result, " : " results, ") + ScanTypeLabel());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool apply = InputText("##resultvalue", m_resultValueText, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        if (AccentButton("OK", Px(90.0f, 0.0f)) || apply)
        {
            const Lua::CustomType* custom = scanner.Kind() == Scan::ScanKind::Custom ? m_engine.Lua().FindCustomType(scanner.CustomName()) : nullptr;
            if (m_engine.HasProcess())
            {
                for (const ResultTarget& target : m_resultValueTargets)
                {
                    std::optional<std::vector<std::uint8_t>> bytes;
                    if (custom)
                    {
                        std::vector<std::uint8_t> converted;
                        if (m_engine.Lua().CustomFromText(*custom, m_resultValueText, target.Address, converted))
                            bytes = std::move(converted);
                    }
                    else if (scanner.Kind() != Scan::ScanKind::Custom)
                    {
                        bytes = Scan::ParseValue(scanner.Kind() == Scan::ScanKind::All ? target.Type : scanner.Type(), m_resultValueText, scanner.Format());
                    }
                    if (!bytes)
                    {
                        m_workspace.Report(Status::Fail(custom || scanner.Kind() != Scan::ScanKind::Custom ? ErrorCode::ScanBadValue : ErrorCode::ScanCustomMissing,
                            custom || scanner.Kind() != Scan::ScanKind::Custom ? m_resultValueText : scanner.CustomName()));
                        break;
                    }
                    const Status status = m_engine.Process()->Write(target.Address, *bytes);
                    if (!status)
                    {
                        m_workspace.Report(status);
                        break;
                    }
                }
                m_liveValues.clear();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        EndModal();
    }

    Scan::ScanOptions MainWindow::CurrentOptions() const
    {
        Scan::ScanOptions options;
        options.Kind = m_scanKind;
        options.Type = Scan::kScanTypes[m_typeIndex];
        options.AllTypes = m_engine.Options().AllTypes;
        options.CustomName = m_customName;
        options.Against = m_compareTo;
        options.SavedName = m_savedName;
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
        if (!Scan::CompareAllowedFirst(kCompares[m_compareIndex]) || !Scan::CompareAvailable(m_scanKind, kCompares[m_compareIndex]))
            m_compareIndex = 0;
        Scan::ScanOptions options = CurrentOptions();
        if (options.Kind == Scan::ScanKind::Custom)
        {
            if (Status filled = FillCustom(options, m_customName); !filled)
            {
                m_workspace.Report(filled);
                return;
            }
        }
        const Status status = m_engine.Scanner().Start(m_engine.Process(), options, true);
        m_workspace.Report(status);
    }

    void MainWindow::DoNextScan()
    {
        if (!m_engine.HasProcess())
        {
            m_workspace.Report(Status::Fail(ErrorCode::NoProcess));
            return;
        }
        Scan::Scanner& scanner = m_engine.Scanner();
        Scan::ScanOptions options = CurrentOptions();
        options.Kind = scanner.Kind();
        options.Type = scanner.Type();
        if (options.Kind == Scan::ScanKind::Custom)
        {
            if (Status filled = FillCustom(options, scanner.CustomName()); !filled)
            {
                m_workspace.Report(filled);
                return;
            }
        }
        const Status status = scanner.Start(m_engine.Process(), options, false);
        m_workspace.Report(status);
    }

    Status MainWindow::FillCustom(Scan::ScanOptions& options, const std::string& name) const
    {
        const Lua::CustomType* custom = m_engine.Lua().FindCustomType(name);
        if (!custom)
            return Status::Fail(ErrorCode::ScanCustomMissing, name);
        options.CustomName = custom->Name;
        options.CustomSize = custom->ByteCount();
        options.CustomBytes.clear();
        if (options.Mode == Scan::Compare::Exact)
        {
            if (TrimView(options.Value).empty())
                return Status::Fail(ErrorCode::ScanNoValue);
            std::vector<std::uint8_t> bytes;
            if (!m_engine.Lua().CustomFromText(*custom, options.Value, 0, bytes) || bytes.size() != static_cast<std::size_t>(options.CustomSize))
                return Status::Fail(ErrorCode::ScanBadValue, options.Value);
            options.CustomBytes = std::move(bytes);
        }
        return Status::Success();
    }

    std::string MainWindow::ScanTypeLabel() const
    {
        const Scan::Scanner& scanner = m_engine.Scanner();
        const bool scanned = scanner.HasResults();
        const Scan::ScanKind kind = scanned ? scanner.Kind() : m_scanKind;
        switch (kind)
        {
        case Scan::ScanKind::All: return "All";
        case Scan::ScanKind::Grouped: return "Grouped";
        case Scan::ScanKind::Custom: return scanned ? scanner.CustomName() : m_customName;
        default: return Scan::TypeName(scanned ? scanner.Type() : Scan::kScanTypes[m_typeIndex]);
        }
    }

    std::string MainWindow::ResultText(const Scan::ScanRecord& record, std::span<const std::uint8_t> bytes)
    {
        Scan::Scanner& scanner = m_engine.Scanner();
        switch (scanner.Kind())
        {
        case Scan::ScanKind::Grouped:
        {
            const auto group = scanner.Group();
            return group ? group->Describe(bytes, scanner.Format().PointerSize) : HexBytes(bytes);
        }
        case Scan::ScanKind::Custom:
        {
            const Lua::CustomType* custom = m_engine.Lua().FindCustomType(scanner.CustomName());
            std::string text;
            if (!custom || !m_engine.Lua().CustomToText(*custom, bytes, record.Address, text))
                return "??";
            return text;
        }
        case Scan::ScanKind::All:
            return Scan::FormatValue(record.Type, bytes, scanner.Format());
        default:
            return Scan::FormatValue(scanner.Type(), bytes, scanner.Format());
        }
    }

    void MainWindow::AddResult(const Scan::ScanRecord& record)
    {
        Scan::Scanner& scanner = m_engine.Scanner();
        switch (scanner.Kind())
        {
        case Scan::ScanKind::All:
            m_workspace.AddAddress(record.Address, record.Type);
            return;
        case Scan::ScanKind::Custom:
        {
            Table::MemoryRecord& added = m_engine.Table().AddValue("No description", m_engine.Symbols().ModuleOffset(record.Address), Scan::ValueType::FourBytes);
            added.CustomTypeName = scanner.CustomName();
            m_engine.RefreshValues(true);
            return;
        }
        case Scan::ScanKind::Grouped:
        {
            const auto group = scanner.Group();
            if (!group)
                return;
            std::vector<std::uint8_t> bytes(record.Previous.size());
            if (!m_engine.HasProcess() || bytes.empty() || !m_engine.Process()->ReadExact(record.Address, bytes.data(), bytes.size()))
                bytes = record.Previous;
            std::vector<int> offsets = group->Offsets(bytes);
            if (std::find(offsets.begin(), offsets.end(), -1) != offsets.end())
                offsets = group->Offsets(record.Previous);
            Table::MemoryRecord& parent = m_engine.Table().AddGroup("Group at " + m_engine.Symbols().ModuleOffset(record.Address));
            for (std::size_t i = 0; i < group->Elements.size() && i < offsets.size(); ++i)
            {
                const Scan::GroupElement& element = group->Elements[i];
                if (element.Skip || offsets[i] < 0)
                    continue;
                const std::uint64_t address = record.Address + static_cast<std::uint64_t>(offsets[i]);
                Table::MemoryRecord& added = m_engine.Table().AddValue("+" + Hex(static_cast<std::uint64_t>(offsets[i])), m_engine.Symbols().ModuleOffset(address), element.Type, &parent);
                if (element.Type == Scan::ValueType::String)
                {
                    added.Unicode = element.Unicode;
                    added.Length = element.Unicode ? element.Size / 2 : element.Size;
                }
            }
            m_engine.RefreshValues(true);
            return;
        }
        default:
            m_workspace.AddAddress(record.Address, scanner.Type());
            return;
        }
    }

    void MainWindow::RefreshScriptMenu()
    {
        Lua::LuaEngine& lua = m_engine.Lua();
        if (lua.MenuGeneration() == m_scriptMenuGeneration)
            return;
        m_scriptMenuGeneration = lua.MenuGeneration();
        m_scriptMenu = lua.MenuItems(Lua::ScriptMenu::MainMenu);
    }

    bool MainWindow::ScriptMenuMerged(const Lua::ScriptMenuItem& item) const
    {
        return item.Merge == "file" || item.Merge == "edit" || item.Merge == "table" || item.Merge == "help";
    }

    void MainWindow::ScriptMenuInto(const char* merge, int& opened, int& clicked)
    {
        for (const Lua::ScriptMenuItem& root : m_scriptMenu)
        {
            if (root.Merge != merge || root.Children.empty())
                continue;
            if (ImGui::IsWindowAppearing())
                opened = root.Id;
            ImGui::Separator();
            if (const int inner = DrawScriptMenuItems(root.Children))
                clicked = inner;
        }
    }

    void MainWindow::ScriptMenuShortcuts()
    {
        std::vector<ImGuiKeyChord> own;
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope != CommandScope::Global && info.Scope != CommandScope::Main && info.Scope != CommandScope::AddressList && info.Scope != CommandScope::ScanResults)
                continue;
            const Binding binding = CurrentBinding(info.Id);
            if (binding.Chord)
                own.push_back(binding.Chord);
            if (binding.Alternate)
                own.push_back(binding.Alternate);
        }
        std::vector<const Lua::ScriptMenuItem*> pending;
        for (const Lua::ScriptMenuItem& root : m_scriptMenu)
            pending.push_back(&root);
        while (!pending.empty())
        {
            const Lua::ScriptMenuItem* item = pending.back();
            pending.pop_back();
            for (const Lua::ScriptMenuItem& child : item->Children)
                pending.push_back(&child);
            const ImGuiKeyChord chord = ScriptChord(item->Shortcut);
            if (!chord || !item->Enabled || !item->Children.empty() || std::find(own.begin(), own.end(), chord) != own.end())
                continue;
            if (ChordPressed(chord, CommandScope::Main))
            {
                m_engine.Lua().ClickMenuItem(item->Id);
                return;
            }
        }
    }

    void MainWindow::SaveResultsDialog()
    {
        if (m_saveResultsDialog)
        {
            ImGui::OpenPopup("Save results##scan");
            m_saveResultsDialog = false;
        }
        if (!BeginModal("Save results##scan", ICON_FA_SAVE, "Save results", ImVec2(380.0f, 0.0f)))
            return;
        Scan::Scanner& scanner = m_engine.Scanner();
        Muted("A next scan can compare with these results later. They stay until New scan.");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool apply = InputTextWithHint("##savedname", "name", m_saveResultsName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        ImGui::BeginDisabled(TrimView(m_saveResultsName).empty());
        if (AccentButton("Save", Px(90.0f, 0.0f)) || (apply && !TrimView(m_saveResultsName).empty()))
        {
            if (scanner.SaveResults(m_saveResultsName))
            {
                m_savedName = Trim(m_saveResultsName);
                m_engine.Notify(LogLevel::Info, "Saved " + Grouped(scanner.Count()) + " results as \"" + m_savedName + "\".");
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", Px(90.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        EndModal();
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
        const bool iconsOnly = buttonWidth < ImGui::CalcTextSize(ICON_FA_FILTER "  Next scan").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const char* firstLabel = iconsOnly ? ICON_FA_SEARCH "##firstscan" : ICON_FA_SEARCH "  First scan";
        const char* newLabel = iconsOnly ? ICON_FA_REDO "##newscan" : ICON_FA_REDO "  New scan";
        const char* nextLabel = iconsOnly ? ICON_FA_FILTER "##nextscan" : ICON_FA_FILTER "  Next scan";
        const char* undoLabel = iconsOnly ? ICON_FA_UNDO "##undoscan" : ICON_FA_UNDO "  Undo scan";

        ImGui::BeginDisabled(busy);
        if (!scanned)
        {
            if (AccentButton(firstLabel, ImVec2(buttonWidth, 0.0f)))
                DoFirstScan();
        }
        else if (ImGui::Button(newLabel, ImVec2(buttonWidth, 0.0f)))
        {
            NewScan();
        }
        ItemTooltip(scanned ? WithKeys("Forget the results and start over", Command::NewScan).c_str() : WithKeys("Scan the memory for the value", Command::FirstScan).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!scanned);
        if ((scanned ? AccentButton(nextLabel, ImVec2(buttonWidth, 0.0f)) : ImGui::Button(nextLabel, ImVec2(buttonWidth, 0.0f))))
            DoNextScan();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", WithKeys(scanned ? "Keep the results that match" : "Next scan: keep the results that match", Command::FirstScan).c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!scanner.CanUndo());
        if (ImGui::Button(undoLabel, ImVec2(buttonWidth, 0.0f)))
            scanner.Undo();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", WithKeys("Go back to the results before the last scan", Command::UndoScan).c_str());
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
        const Scan::ScanKind kind = scanned ? scanner.Kind() : m_scanKind;
        if (!Scan::CompareAvailable(kind, kCompares[m_compareIndex]))
            m_compareIndex = 0;
        const Scan::Compare compare = kCompares[m_compareIndex];
        const bool needsValue = Scan::CompareNeedsValue(compare) || (kind == Scan::ScanKind::Grouped && compare == Scan::Compare::Unknown && !scanned);
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
        const char* hint = kind == Scan::ScanKind::Grouped ? "4:100 f:1.5 d:*" : Scan::CompareNeedsTwoValues(compare) ? "from" : "value";
        if (InputTextWithHint("##value", hint, m_value, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            if (!scanned)
                DoFirstScan();
            else
                DoNextScan();
        }
        if (kind == Scan::ScanKind::Grouped)
            ItemTooltip("Values next to each other, separated by spaces, each as type:value.\n"
                        "Types: 1, 2, 4, 8 (bytes), f (float), d (double), p (pointer), s (text), u (UTF-16 text), skip:N (N bytes in between).\n"
                        "Values: a number, * for any value, or a range like 10..20. Text with spaces goes in quotes: s:\"two words\".\n"
                        "within:N: the values after the first one may be anywhere in the N bytes from the first, in any order.");
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
                const bool allowed = (scanned ? Scan::CompareAllowedNext(kCompares[i]) : Scan::CompareAllowedFirst(kCompares[i])) && Scan::CompareAvailable(kind, kCompares[i]);
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

        if (scanned)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Compare to");
            ImGui::SameLine(labelWidth);
            if (m_compareTo == Scan::CompareTo::Saved && !scanner.HasSaved(m_savedName))
                m_compareTo = Scan::CompareTo::Previous;
            const bool usesPrevious = Scan::CompareUsesPrevious(compare);
            const std::string preview = !usesPrevious ? "Previous scan" : m_compareTo == Scan::CompareTo::First ? "First scan" : m_compareTo == Scan::CompareTo::Saved ? "Saved: " + m_savedName : "Previous scan";
            const char* saveLabel = ICON_FA_SAVE "  Save";
            const float saveWidth = ImGui::CalcTextSize(saveLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - saveWidth - ImGui::GetStyle().ItemSpacing.x);
            ImGui::BeginDisabled(!usesPrevious);
            if (ImGui::BeginCombo("##compareto", preview.c_str()))
            {
                if (ImGui::Selectable("Previous scan", m_compareTo == Scan::CompareTo::Previous))
                    m_compareTo = Scan::CompareTo::Previous;
                if (ImGui::Selectable("First scan", m_compareTo == Scan::CompareTo::First))
                    m_compareTo = Scan::CompareTo::First;
                const std::vector<std::string> saved = scanner.SavedNames();
                if (!saved.empty())
                    ImGui::Separator();
                for (const std::string& name : saved)
                {
                    ImGui::PushID(name.c_str());
                    if (ImGui::Selectable(("Saved: " + name).c_str(), m_compareTo == Scan::CompareTo::Saved && m_savedName == name))
                    {
                        m_compareTo = Scan::CompareTo::Saved;
                        m_savedName = name;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", usesPrevious ? "What the next scan compares the values with" : "Only scans that compare with an earlier value use this");
            ImGui::SameLine();
            if (ImGui::Button(saveLabel, ImVec2(saveWidth, 0.0f)))
            {
                m_saveResultsName = "Scan " + std::to_string(scanner.ScanNumber());
                m_saveResultsDialog = true;
            }
            ItemTooltip("Save these results, so a later scan can compare with them");
        }

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Value type");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::BeginDisabled(scanned);
        if (m_scanKind == Scan::ScanKind::Custom && !m_engine.Lua().FindCustomType(m_customName))
            m_scanKind = Scan::ScanKind::Single;
        if (ImGui::BeginCombo("##type", ScanTypeLabel().c_str(), ImGuiComboFlags_HeightLarge))
        {
            for (int i = 0; i < static_cast<int>(std::size(Scan::kScanTypes)); ++i)
            {
                if (ImGui::Selectable(Scan::TypeName(Scan::kScanTypes[i]), m_scanKind == Scan::ScanKind::Single && i == m_typeIndex))
                {
                    m_scanKind = Scan::ScanKind::Single;
                    m_typeIndex = i;
                    m_alignment = Scan::DefaultAlignment(Scan::kScanTypes[i]);
                }
            }
            ImGui::Separator();
            if (ImGui::Selectable("All##kind", m_scanKind == Scan::ScanKind::All))
            {
                m_scanKind = Scan::ScanKind::All;
                m_alignment = 1;
            }
            ItemTooltip("Byte, 2, 4 and 8 bytes, float and double in one scan. Settings, Scanner, Types for All picks which of them.");
            if (ImGui::Selectable("Grouped##kind", m_scanKind == Scan::ScanKind::Grouped))
            {
                m_scanKind = Scan::ScanKind::Grouped;
                m_alignment = 4;
            }
            ItemTooltip("Several values that lie next to each other, like 4:100 f:1.5 d:*");
            const std::vector<std::string> customs = m_engine.Lua().CustomTypeNames();
            if (!customs.empty())
                ImGui::Separator();
            for (const std::string& name : customs)
            {
                ImGui::PushID(name.c_str());
                if (ImGui::Selectable(name.c_str(), m_scanKind == Scan::ScanKind::Custom && EqualsNoCase(name, m_customName)))
                {
                    m_scanKind = Scan::ScanKind::Custom;
                    m_customName = name;
                    const Lua::CustomType* custom = m_engine.Lua().FindCustomType(name);
                    const int size = custom ? custom->ByteCount() : 1;
                    m_alignment = size % 4 == 0 ? 4 : size % 2 == 0 ? 2 : 1;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        const Scan::ValueType type = scanned ? scanner.Type() : Scan::kScanTypes[m_typeIndex];
        const float optionIndent = ImGui::GetCursorPosX() + labelWidth - ImGui::GetStyle().WindowPadding.x + Px(2.0f);
        if ((kind == Scan::ScanKind::Single && Scan::IsFloating(type)) || kind == Scan::ScanKind::All)
        {
            ImGui::SetCursorPosX(optionIndent);
            ImGui::RadioButton("Rounded", &m_floatMode, 0);
            ItemTooltip("100.5 finds values from 100.45 to 100.55");
            FlowNext(CheckboxWidth("Truncated"), optionIndent);
            ImGui::RadioButton("Truncated", &m_floatMode, 1);
            ItemTooltip("100.5 finds values from 100.5 to 100.6");
            FlowNext(CheckboxWidth("Exact"), optionIndent);
            ImGui::RadioButton("Exact", &m_floatMode, 2);
        }
        if (kind == Scan::ScanKind::Single && type == Scan::ValueType::String)
        {
            ImGui::SetCursorPosX(optionIndent);
            ImGui::Checkbox("UTF-16", &m_unicode);
            FlowNext(CheckboxWidth("Case sensitive"), optionIndent);
            ImGui::Checkbox("Case sensitive", &m_caseSensitive);
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Memory scan options"))
        {
            const ImGuiStyle& style = ImGui::GetStyle();
            const float stopLabel = ImGui::CalcTextSize("Stop").x;
            const float rangeRoom = ImGui::GetContentRegionAvail().x - labelWidth + style.WindowPadding.x;
            const bool rangeOneLine = rangeRoom >= Px(150.0f) * 2.0f + stopLabel + style.ItemSpacing.x * 2.0f;
            const float rangeWidth = rangeOneLine ? Px(150.0f) : std::max(Px(80.0f), rangeRoom - style.ItemSpacing.x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Start");
            ImGui::SameLine(labelWidth);
            ImGui::SetNextItemWidth(rangeWidth);
            InputText("##start", m_start, ImGuiInputTextFlags_CharsHexadecimal);
            if (rangeOneLine)
                ImGui::SameLine();
            else
                ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Stop");
            ImGui::SameLine(rangeOneLine ? 0.0f : labelWidth);
            ImGui::SetNextItemWidth(rangeWidth);
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
            FlowNext(CheckboxWidth("Executable"));
            TriStateCheckbox("Executable", m_executable);
            FlowNext(CheckboxWidth("Copy on write"));
            TriStateCheckbox("Copy on write", m_copyOnWrite);
            ImGui::Checkbox("Private", &m_private);
            FlowNext(CheckboxWidth("Image"));
            ImGui::Checkbox("Image", &m_image);
            FlowNext(CheckboxWidth("Mapped"));
            ImGui::Checkbox("Mapped", &m_mapped);
            ImGui::Checkbox("Fast scan", &m_fastScan);
            ItemTooltip("Only look at addresses aligned to this many bytes. Values in games are nearly always aligned.");
            FlowNext(Px(70.0f) + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Alignment").x);
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
        const ImGuiStyle& style = ImGui::GetStyle();
        const float iconWidth = ImGui::GetFrameHeight();
        const float gap = Px(4.0f);
        const float right = ImGui::GetWindowContentRegionMax().x;
        const float titleEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const auto labelWidth = [&](const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f; };
        const float textButtons = m_release ? 0.0f : Px(16.0f) + labelWidth(ICON_FA_PLUS "  Address") + labelWidth(ICON_FA_FOLDER_PLUS "  Group") + labelWidth(ICON_FA_CODE "  Script") + gap * 2.0f;
        const float iconButtons = m_release ? 0.0f : Px(16.0f) + iconWidth * 3.0f + gap * 2.0f;
        const float groupsWidth = (iconWidth + gap) * 2.0f + Px(6.0f);
        const float room = right - titleEnd - Px(12.0f);
        float filterWidth = Px(180.0f);
        const bool iconsOnly = textButtons + groupsWidth + filterWidth + gap + iconWidth > room;
        const float buttons = iconsOnly ? iconButtons : textButtons;
        if (buttons + groupsWidth + filterWidth + gap + iconWidth > room)
            filterWidth = std::max(Px(110.0f), room - buttons - groupsWidth - gap - iconWidth);
        const bool ownLine = buttons + groupsWidth + filterWidth + gap + iconWidth > room;
        if (!m_release)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.11f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.16f));
            const ImVec2 iconSize(iconWidth, 0.0f);
            ImGui::SameLine(0.0f, Px(16.0f));
            if (ImGui::Button(iconsOnly ? ICON_FA_PLUS "##addaddress" : ICON_FA_PLUS "  Address", iconsOnly ? iconSize : ImVec2(0.0f, 0.0f)))
                OpenAddDialog();
            ItemTooltip(WithKeys("Add an address by hand", Command::AddAddress).c_str());
            ImGui::SameLine(0.0f, gap);
            if (ImGui::Button(iconsOnly ? ICON_FA_FOLDER_PLUS "##addgroup" : ICON_FA_FOLDER_PLUS "  Group", iconsOnly ? iconSize : ImVec2(0.0f, 0.0f)))
                list.AddGroup("Group");
            ItemTooltip(WithKeys("Add an empty group", Command::AddGroup).c_str());
            ImGui::SameLine(0.0f, gap);
            if (ImGui::Button(iconsOnly ? ICON_FA_CODE "##addscript" : ICON_FA_CODE "  Script", iconsOnly ? iconSize : ImVec2(0.0f, 0.0f)))
                AddScriptRecord();
            ItemTooltip(WithKeys("Add an Auto Assembler script", Command::AddScript).c_str());
            ImGui::PopStyleColor(3);
        }
        const float filterX = right - filterWidth - iconWidth - gap;
        const float groupsX = filterX - groupsWidth;
        ProgressLine(ownLine ? right : groupsX - Px(12.0f));
        if (!ownLine)
            ImGui::SameLine();
        ImGui::SetCursorPosX(groupsX);
        if (IconButton(ICON_FA_EXPAND_ALT, WithKeys("Expand every group", Command::ExpandGroups).c_str()))
            list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = true; });
        ImGui::SameLine(0.0f, gap);
        if (IconButton(ICON_FA_COMPRESS_ALT, WithKeys("Collapse every group", Command::CollapseGroups).c_str()))
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
            if (CommandItem(ICON_FA_PLUS "  Add address manually", Command::AddAddress))
                OpenAddDialog();
            if (CommandItem(ICON_FA_FOLDER_PLUS "  Add group", Command::AddGroup))
                list.AddGroup("Group");
            if (CommandItem(ICON_FA_CODE "  Add Auto Assembler script", Command::AddScript))
                AddScriptRecord();
            if (CommandItem(ICON_FA_PASTE "  Paste", Command::PasteRecords))
                RunRecordCommand(Command::PasteRecords, nullptr);
            ImGui::Separator();
            if (CommandItem(ICON_FA_EXPAND_ALT "  Expand every group", Command::ExpandGroups))
                RunRecordCommand(Command::ExpandGroups, nullptr);
            if (CommandItem(ICON_FA_COMPRESS_ALT "  Collapse every group", Command::CollapseGroups))
                RunRecordCommand(Command::CollapseGroups, nullptr);
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
            actions.push_back({ ICON_FA_FOLDER_OPEN "  Open table", ShortcutText(Command::OpenTable) });
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
                OpenAddDialog();
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
            std::vector<int> shown;
            const auto fresh = [&](int id)
                {
                    Table::MemoryRecord* record = list.FindById(id);
                    if (record && engine.NeedsValue(*record))
                    {
                        engine.RefreshRecord(id);
                        record = list.FindById(id);
                    }
                    return record;
                };
            if (rows.size() <= kUnclippedRows)
            {
                for (const ListRow& entry : rows)
                {
                    if (Table::MemoryRecord* record = fresh(entry.Id))
                    {
                        RecordRow(*record, entry.Depth, row, entry.Match);
                        shown.push_back(entry.Id);
                    }
                }
            }
            else
            {
                const auto indexOf = [&rows](int id)
                    {
                        for (std::size_t i = 0; i < rows.size(); ++i)
                        {
                            if (rows[i].Id == id)
                                return static_cast<int>(i);
                        }
                        return -1;
                    };
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(rows.size()));
                if (const int scrollTo = m_scrollToRecord >= 0 ? indexOf(m_scrollToRecord) : -1; scrollTo >= 0)
                    clipper.IncludeItemByIndex(scrollTo);
                if (const ImGuiPayload* payload = ImGui::GetDragDropPayload(); payload && payload->IsDataType(kRecordDrag) && payload->DataSize == sizeof(int))
                {
                    if (const int dragged = indexOf(*static_cast<const int*>(payload->Data)); dragged >= 0)
                        clipper.IncludeItemByIndex(dragged);
                }
                int firstDrawn = -1;
                float firstTop = 0.0f;
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        const ListRow& entry = rows[static_cast<std::size_t>(i)];
                        Table::MemoryRecord* record = fresh(entry.Id);
                        if (!record)
                            continue;
                        const std::size_t before = m_spots.size();
                        RecordRow(*record, entry.Depth, row, entry.Match);
                        shown.push_back(entry.Id);
                        if (firstDrawn < 0 && m_spots.size() > before)
                        {
                            firstDrawn = i;
                            firstTop = m_spots[before].Top;
                        }
                    }
                }
                const float height = clipper.ItemsHeight;
                clipper.End();
                if (firstDrawn >= 0 && height > 0.0f)
                {
                    m_spots.clear();
                    for (std::size_t i = 0; i < rows.size(); ++i)
                        m_spots.push_back({ rows[i].Id, firstTop + (static_cast<float>(i) - static_cast<float>(firstDrawn)) * height });
                }
            }
            engine.SetVisibleRecords(std::move(shown));
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

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput)
            ListKeys();
        Table::MemoryRecord* focus = FocusRecord();
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::AddressList && Pressed(info.Id))
                RunRecordCommand(info.Id, focus);
        }
        EndCard();
    }

    Table::MemoryRecord* MainWindow::FocusRecord()
    {
        Table::AddressList& list = m_engine.Table();
        if (m_lastClickedRecord >= 0 && list.Selected(m_lastClickedRecord))
        {
            if (Table::MemoryRecord* record = list.FindById(m_lastClickedRecord))
                return record;
        }
        const std::vector<Table::MemoryRecord*> selected = SelectedRecords();
        return selected.empty() ? nullptr : selected.front();
    }

    void MainWindow::WriteValues(const std::vector<int>& ids, const std::string& text)
    {
        Table::AddressList& list = m_engine.Table();
        std::vector<ValueUndo> undo;
        for (int id : ids)
        {
            Table::MemoryRecord* record = list.FindById(id);
            if (!record || record->Kind != Table::RecordKind::Value)
                continue;
            const std::string before = record->ValueText;
            const Status status = m_engine.WriteRecord(*record, text);
            m_workspace.Report(status);
            if (status && before != "??" && !before.empty())
                undo.push_back({ id, before });
        }
        if (!undo.empty())
            m_valueUndo = std::move(undo);
        m_engine.RefreshValues(true);
    }

    void MainWindow::UndoValueChange()
    {
        if (m_valueUndo.empty())
        {
            m_engine.Notify(LogLevel::Info, "There is no value change to undo.");
            return;
        }
        const std::vector<ValueUndo> undo = std::move(m_valueUndo);
        m_valueUndo.clear();
        for (const ValueUndo& entry : undo)
        {
            if (Table::MemoryRecord* record = m_engine.Table().FindById(entry.Id))
                m_workspace.Report(m_engine.WriteRecord(*record, entry.Text));
        }
        m_engine.RefreshValues(true);
    }

    void MainWindow::DeleteSelectedRecords(bool ask)
    {
        const std::vector<int> ids = m_engine.Table().Selection();
        if (ids.empty())
            return;
        const auto remove = [this, ids]()
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
            };
        if (!ask)
        {
            remove();
            return;
        }
        m_workspace.Confirm("Delete records", "Delete " + std::to_string(ids.size()) + (ids.size() == 1 ? " record" : " records") + " and their children?", remove, "Delete");
    }

    void MainWindow::DetectMono()
    {
        const std::shared_ptr<Target::Process>& process = m_engine.Process();
        if (!process)
        {
            m_monoDetected = false;
            return;
        }
        const double now = ImGui::GetTime();
        if (m_engine.ProcessGeneration() != m_monoGeneration)
        {
            m_monoGeneration = m_engine.ProcessGeneration();
            m_monoDetected = false;
            m_monoCheckAt = 0.0;
            m_monoRefreshAt = now + 5.0;
        }
        if (m_engine.MonoPresent())
        {
            m_monoDetected = true;
            return;
        }
        if (m_monoDetected || now < m_monoCheckAt)
            return;
        // ∑ The cached module list is checked once a second; it is read fresh
        // only every few seconds, so a game that loads its runtime after the
        // attach still gets the menu without a module walk every frame.
        m_monoCheckAt = now + 1.0;
        if (now >= m_monoRefreshAt)
        {
            process->RefreshModules();
            m_monoRefreshAt = now + 5.0;
        }
        m_monoDetected = Target::MonoBridge::HasRuntime(*process);
    }

    void MainWindow::MonoMenu()
    {
        if (!ImGui::BeginMenu("Mono"))
            return;
        Target::MonoBridge& mono = m_engine.Mono();
        if (!m_engine.MonoPresent())
        {
            if (ImGui::MenuItem(m_engine.MonoLoading() ? ICON_FA_PLUG "  Loading the Mono runtime..." : ICON_FA_PLUG "  Load Mono runtime", nullptr, false, !m_engine.MonoLoading()))
                m_engine.LoadMono();
            ItemTooltip("Reads the classes and methods of the game's Mono runtime, so method names show in the memory viewer, resolve in addresses and Structure Dissect finds the class of an object.");
        }
        else
        {
            const std::string loaded = mono.Module() + "  ·  " + std::to_string(mono.ClassCount()) + " classes";
            SectionLabel(loaded.c_str());
            if (ImGui::MenuItem(ICON_FA_SYNC "  Reload classes", nullptr, false, !m_engine.MonoLoading()))
                m_engine.ReloadMono();
            ItemTooltip("Reads the class list again, for assemblies the game loaded later");
            if (ImGui::MenuItem(ICON_FA_POWER_OFF "  Shut down Mono", nullptr, false, !m_engine.MonoLoading()))
                m_engine.StopMono();
            ItemTooltip("Ends the helper thread in the game and takes the Unity explorer's callback out. Mono method names and lookups stop until it is loaded again.");
        }
        ImGui::Separator();
        if (CommandItem(ICON_FA_DICE_D6 "  Mono / .NET dissector", Command::OpenMono))
            m_workspace.OpenMono();
        ImGui::EndMenu();
    }

    void MainWindow::RunRecordCommand(Command command, Table::MemoryRecord* record)
    {
        Engine& engine = m_engine;
        Table::AddressList& list = engine.Table();
        const std::vector<Table::MemoryRecord*> selected = SelectedRecords();
        const bool developer = !m_release;
        const bool value = record && record->Kind == Table::RecordKind::Value;
        const auto resolved = [&]() -> std::optional<std::uint64_t>
            {
                if (!value)
                    return std::nullopt;
                engine.ResolveRecord(*record);
                if (!record->AddressOk)
                    return std::nullopt;
                return record->ResolvedAddress;
            };
        const auto freeze = [&](Table::FreezeMode mode)
            {
                for (Table::MemoryRecord* target : selected)
                {
                    if (target->Kind != Table::RecordKind::Value)
                        continue;
                    target->Freeze = mode;
                    if (!target->Active)
                        engine.RequestActive(*target, true);
                }
            };
        switch (command)
        {
        case Command::ToggleRecords:
            if (record)
            {
                const bool target = !record->Active;
                for (Table::MemoryRecord* item : selected)
                    engine.RequestActive(*item, target);
            }
            break;
        case Command::ChangeRecordValue:
            if (value && !engine.Lua().AddressListEvent("onvaluechange", record))
            {
                m_valueTargets = list.Selected(record->Id) ? list.Selection() : std::vector<int>{ record->Id };
                m_valueText = record->ValueText == "??" ? "" : record->ValueText;
                m_valueDialog = true;
            }
            break;
        case Command::EditValueInline:
            if (value)
                StartInlineEdit(*record);
            break;
        case Command::FreezeAllowIncrease:
            if (value)
                freeze(Table::FreezeMode::AllowIncrease);
            break;
        case Command::FreezeAllowDecrease:
            if (value)
                freeze(Table::FreezeMode::AllowDecrease);
            break;
        case Command::UndoRecordValue:
            if (developer)
                UndoValueChange();
            break;
        case Command::SelectAllRecords:
            list.ClearSelection();
            list.ForEach([&](Table::MemoryRecord& item) { list.Select(item.Id, true); });
            break;
        default:
            break;
        }
        if (!developer)
            return;
        switch (command)
        {
        case Command::ChangeDescription:
            if (record && !engine.Lua().AddressListEvent("ondescriptionchange", record))
            {
                m_descriptionDialog = true;
                m_descriptionTarget = record->Id;
                m_descriptionText = record->Description;
            }
            break;
        case Command::EditRecord:
        case Command::AssignHotkey:
            if (record)
                m_workspace.OpenRecordEditor(record->Id);
            break;
        case Command::ShowAsHex:
            if (value)
            {
                const bool target = !record->ShowHex;
                for (Table::MemoryRecord* item : selected)
                    item->ShowHex = target;
                engine.RefreshValues(true);
            }
            break;
        case Command::BrowseRecord:
            if (const auto address = resolved())
                m_workspace.OpenMemoryViewer(*address, true);
            break;
        case Command::DisassembleRecord:
            if (const auto address = resolved())
                m_workspace.OpenMemoryViewer(*address, false);
            break;
        case Command::DissectRecord:
            if (const auto address = resolved())
                m_workspace.OpenStructure(*address);
            break;
        case Command::RecordAccesses:
        case Command::RecordWrites:
            if (const auto address = resolved())
                m_workspace.StartWatch(command == Command::RecordAccesses ? Debug::WatchKind::Accesses : Debug::WatchKind::Writes, *address,
                    record->Size(engine.Process() ? engine.Process()->PointerSize() : 8));
            break;
        case Command::EditScript:
            if (record && record->Kind == Table::RecordKind::Script && !TableHandlesScriptEdit(engine.Lua(), *record))
                m_workspace.OpenScript(record->Script, record->Id);
            break;
        case Command::GroupRecords:
            if (record)
            {
                Table::MemoryRecord* parent = record->Parent;
                const int index = list.IndexOf(*record);
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
            break;
        case Command::ExpandGroups:
        case Command::CollapseGroups:
        {
            const bool expand = command == Command::ExpandGroups;
            list.ForEach([expand](Table::MemoryRecord& item) { if (!item.Children.empty()) item.Expanded = expand; });
            break;
        }
        case Command::CutRecords:
            if (!list.Selection().empty())
            {
                m_workspace.CopyRecords(list.Selection());
                DeleteSelectedRecords(false);
            }
            break;
        case Command::CopyRecords:
            if (!list.Selection().empty())
                m_workspace.CopyRecords(list.Selection());
            break;
        case Command::PasteRecords:
            m_workspace.PasteRecords(kNoRecord);
            break;
        case Command::DeleteRecords:
            DeleteSelectedRecords(true);
            break;
        default:
            break;
        }
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
            WriteValues(list.Selected(record.Id) ? list.Selection() : std::vector<int>{ record.Id }, m_inlineText);
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

        struct Border
        {
            float X = 0.0f;
            ImU32 Color = 0;
        };
        std::vector<Border> borders;
        const float textStart = table->Columns[1].MinX;
        const float textEnd = std::min(x, max.x) + Px(6.0f);
        for (int order = 0; order < table->ColumnsCount; ++order)
        {
            const int index = table->DisplayOrderToIndex[order];
            const ImGuiTableColumn& column = table->Columns[index];
            if (!column.IsEnabled || column.NextEnabledColumn == -1)
                continue;
            if (column.MaxX > table->InnerClipRect.Max.x || column.MaxX <= column.ClipRect.Min.x)
                continue;
            if (column.MaxX >= textStart && column.MaxX <= textEnd)
                continue;
            const bool hovered = table->HoveredColumnBorder == index;
            const bool resized = table->ResizedColumn == index && table->InstanceInteracted == table->InstanceCurrent;
            ImU32 color = table->BorderColorLight;
            if (resized || hovered)
                color = ImGui::GetColorU32(resized ? ImGuiCol_SeparatorActive : ImGuiCol_SeparatorHovered);
            else if (table->FreezeColumnsCount == order + 1)
                color = table->BorderColorStrong;
            borders.push_back({ column.MaxX, color });
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
                   borderColor = ImGui::GetColorU32(ManifoldTheme::Border), line = Px(1.0f), shadow = Px(6.0f), pieces = std::move(pieces), borders = std::move(borders)]()
            {
                draw->PushClipRect(clip.Min, clip.Max, false);
                draw->AddRectFilledMultiColor(ImVec2(min.x, max.y), ImVec2(max.x, max.y + shadow), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 70), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
                draw->AddRectFilled(min, max, fillColor);
                for (const Border& border : borders)
                    draw->AddLineV(border.X, min.y, max.y, border.Color, 1.0f);
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
        auto selected = SelectedRecords();
        ImGui::TextDisabled("%s", record.Description.c_str());
        ImGui::Separator();
        if (CommandItem(record.Active ? ICON_FA_TOGGLE_ON "  Deactivate" : ICON_FA_TOGGLE_OFF "  Activate", Command::ToggleRecords))
            RunRecordCommand(Command::ToggleRecords, &record);
        if (m_release && record.Kind != Table::RecordKind::Value)
            return;
        if (record.Kind == Table::RecordKind::Value)
        {
            if (CommandItem(ICON_FA_EDIT "  Change value", Command::ChangeRecordValue))
                RunRecordCommand(Command::ChangeRecordValue, &record);
            if (CommandItem(ICON_FA_I_CURSOR "  Edit the value in place", Command::EditValueInline))
                RunRecordCommand(Command::EditValueInline, &record);
            if (!m_release && CommandItem(ICON_FA_UNDO_ALT "  Undo last value change", Command::UndoRecordValue, false, !m_valueUndo.empty()))
                RunRecordCommand(Command::UndoRecordValue, &record);
            if (ImGui::BeginMenu(ICON_FA_SNOWFLAKE "  Freeze mode"))
            {
                if (ImGui::MenuItem("Normal", nullptr, record.Freeze == Table::FreezeMode::Normal))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::Normal;
                if (CommandItem("Allow increase", Command::FreezeAllowIncrease, record.Freeze == Table::FreezeMode::AllowIncrease))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::AllowIncrease;
                if (CommandItem("Allow decrease", Command::FreezeAllowDecrease, record.Freeze == Table::FreezeMode::AllowDecrease))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::AllowDecrease;
                ImGui::EndMenu();
            }
            if (m_release)
                return;
            if (CommandItem(ICON_FA_HASHTAG "  Show as hexadecimal", Command::ShowAsHex, record.ShowHex))
                RunRecordCommand(Command::ShowAsHex, &record);
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
        if (record.Kind == Table::RecordKind::Script && CommandItem(ICON_FA_CODE "  Edit script", Command::EditScript))
            RunRecordCommand(Command::EditScript, &record);
        if (CommandItem(ICON_FA_SLIDERS_H "  Edit record...", Command::EditRecord))
            RunRecordCommand(Command::EditRecord, &record);
        if (CommandItem(ICON_FA_KEYBOARD "  Assign a hotkey...", Command::AssignHotkey))
            RunRecordCommand(Command::AssignHotkey, &record);
        if (CommandItem(ICON_FA_I_CURSOR "  Change description", Command::ChangeDescription))
            RunRecordCommand(Command::ChangeDescription, &record);
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
        if (CommandItem(ICON_FA_FOLDER_PLUS "  Put selected into a new group", Command::GroupRecords))
            RunRecordCommand(Command::GroupRecords, &record);
        ImGui::Separator();
        if (record.Kind == Table::RecordKind::Value)
        {
            engine.ResolveRecord(record);
            ImGui::BeginDisabled(!record.AddressOk);
            if (CommandItem(ICON_FA_MEMORY "  Browse this memory region", Command::BrowseRecord))
                RunRecordCommand(Command::BrowseRecord, &record);
            if (CommandItem(ICON_FA_CODE "  Disassemble this memory region", Command::DisassembleRecord))
                RunRecordCommand(Command::DisassembleRecord, &record);
            if (CommandItem(ICON_FA_SITEMAP "  Dissect data at this address", Command::DissectRecord))
                RunRecordCommand(Command::DissectRecord, &record);
            if (CommandItem(ICON_FA_EYE "  Find out what accesses this address", Command::RecordAccesses))
                RunRecordCommand(Command::RecordAccesses, &record);
            if (CommandItem(ICON_FA_PEN "  Find out what writes to this address", Command::RecordWrites))
                RunRecordCommand(Command::RecordWrites, &record);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy address"))
                ImGui::SetClipboardText(Hex(record.ResolvedAddress).c_str());
            ImGui::EndDisabled();
            ImGui::Separator();
        }
        if (CommandItem(ICON_FA_CUT "  Cut", Command::CutRecords))
            RunRecordCommand(Command::CutRecords, &record);
        if (CommandItem(ICON_FA_COPY "  Copy", Command::CopyRecords))
            RunRecordCommand(Command::CopyRecords, &record);
        if (CommandItem(ICON_FA_PASTE "  Paste", Command::PasteRecords))
            RunRecordCommand(Command::PasteRecords, &record);
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste into this record"))
            m_workspace.PasteRecords(record.Id);
        if (DangerCommandItem(ICON_FA_TRASH "  Delete", Command::DeleteRecords))
            RunRecordCommand(Command::DeleteRecords, &record);
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
            WriteValues(m_valueTargets, m_valueText);
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
        const float y = windowSize.y - height + std::round((height - ImGui::GetTextLineHeight()) * 0.5f) - Px(1.0f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(windowPos.x, windowPos.y + windowSize.y - height), ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y - height),
            ImGui::GetColorU32(ManifoldTheme::RowSeparator));

        const auto measure = [](std::string_view text) { return ImGui::CalcTextSize(text.data(), text.data() + text.size()).x; };
        const float margin = Px(14.0f);
        const float gap = Px(18.0f);
        const float left = margin;
        const float right = std::max(left, windowSize.x - margin);
        const float room = right - left;

        const std::string& path = engine.Table().FilePath;
        std::string table = ICON_FA_FILE_ALT "  " + (path.empty() ? std::string("Untitled table") : WideToUtf8(std::filesystem::path(Utf8ToWide(path)).filename().wstring()));
        if (engine.Table().Dirty())
            table += " *";
        const float tableFull = measure(table);
        const float tableFloor = std::min(tableFull, measure(ICON_FA_FILE_ALT "  ..."));
        const float tableShort = std::min(tableFull, std::max(tableFloor, Px(130.0f)));
        float tableWidth = tableFull;

        const bool elevated = Target::IsElevated();
        const char* rightsIcon = elevated ? ICON_FA_SHIELD_ALT : ICON_FA_USER;
        const char* rightsText = elevated ? "Administrator" : "No administrator rights";
        const std::string rightsFull = std::string(rightsIcon) + "  " + rightsText;
        bool rightsShort = false;

        // Frame readout: 0 = work and rate, 1 = rate only, 2 = hidden.
        std::string work;
        std::string rate;
        int framesLevel = engine.Options().ProfilerInStatusBar && FrameReadout(work, rate) ? 0 : 2;
        const std::string framesFull = ICON_FA_TACHOMETER_ALT "  " + work + "  ·  " + rate;
        const std::string framesRate = ICON_FA_TACHOMETER_ALT "  " + rate;
        // ∑ Measured against the widest numbers too, so the parts keep still while the
        // readout changes twice a second.
        const float framesFullWidth = std::max(measure(framesFull), measure(ICON_FA_TACHOMETER_ALT "  0.00 ms  ·  000 fps"));
        const float framesRateWidth = std::max(measure(framesRate), measure(ICON_FA_TACHOMETER_ALT "  000 fps"));

        const std::optional<LogEntry> newest = RuntimeLog::Last();
        const char* logIcon = newest ? LevelIcon(newest->Level) : "";
        const std::string message = newest ? (m_release ? Workspace::WithoutCodes(newest->Message) : newest->Message) : std::string();
        const std::string logText = newest ? std::string(logIcon) + "  " + message : std::string();
        const float logFull = newest ? measure(logText) : 0.0f;
        float logWant = logFull;

        const auto rightsWidth = [&]() { return measure(rightsShort ? std::string_view(rightsIcon) : std::string_view(rightsFull)); };
        const auto framesWidth = [&]() { return framesLevel == 0 ? framesFullWidth : framesLevel == 1 ? framesRateWidth : 0.0f; };
        const auto overflow = [&]()
        {
            float total = tableWidth + gap + rightsWidth();
            if (framesLevel < 2)
                total += gap + framesWidth();
            if (logWant > 0.0f)
                total += gap + logWant;
            return total - room;
        };
        const auto shrinkTable = [&](float floor) { tableWidth = std::max(floor, tableWidth - overflow()); };

        // ∑ The parts never share a pixel; they give up room in a fixed order instead. The
        // rights shrink to their icon as soon as the newest log line would be cut. Then the
        // log line keeps a fair share while the frame readout, the table name and the log
        // line itself get shorter. Whatever is cut shows in full in its tooltip.
        if (overflow() > 0.0f)
            rightsShort = true;
        logWant = std::min(logFull, std::max(Px(150.0f), room * 0.4f));
        for (int step = 0; step < 5 && overflow() > 0.0f; ++step)
        {
            switch (step)
            {
            case 0: framesLevel = std::max(framesLevel, 1); break;
            case 1: shrinkTable(tableShort); break;
            case 2: logWant = std::min(logFull, Px(150.0f)); break;
            case 3: framesLevel = 2; break;
            default: shrinkTable(tableFloor); break;
            }
        }

        ImGui::PushClipRect(ImVec2(windowPos.x + left, windowPos.y + windowSize.y - height), ImVec2(windowPos.x + right, windowPos.y + windowSize.y), true);
        float x = left;
        if (StatusText(x, y, table, tableWidth, ManifoldTheme::Muted))
            ItemTooltip(path.empty() ? "Untitled table" : path.c_str());
        x += tableWidth + gap;

        const float rights = rightsWidth();
        StatusText(x, y, rightsShort ? std::string_view(rightsIcon) : std::string_view(rightsFull), rights, ManifoldTheme::Muted);
        if (rightsShort)
            ItemTooltip(rightsText);
        x += rights + gap;

        if (framesLevel < 2)
        {
            const float frames = framesWidth();
            StatusText(x, y, framesLevel == 0 ? framesFull : framesRate, frames, ManifoldTheme::Muted);
            if (ImGui::IsItemClicked())
                m_workspace.OpenProfiler();
            const std::string tip = framesLevel == 0 ? std::string("Work per frame and frames per second. Click to open the profiler.")
                                                     : "Work per frame " + work + ", " + rate + ". Click to open the profiler.";
            ItemTooltip(tip.c_str());
            x += frames + gap;
        }

        if (newest)
        {
            const float space = right - x;
            float width = std::min(logFull, space);
            std::string_view shown = logText;
            // ∑ Fewer than a few letters say nothing, so the line falls back to its level icon.
            if (width < std::min(logFull, Px(48.0f)))
            {
                shown = logIcon;
                width = measure(logIcon);
            }
            if (width <= space + 0.5f)
            {
                const bool cut = StatusText(right - width, y, shown, width, ManifoldTheme::WithAlpha(LevelColor(newest->Level), 0.85f));
                if (m_release)
                {
                    if (cut)
                        ItemTooltip(message.c_str());
                }
                else
                {
                    if (!ErrorTooltip(newest->Message) && ImGui::IsItemClicked())
                        m_workspace.OpenLog();
                    const std::string tip = cut ? message + "\n\nClick to open the log" : std::string("Click to open the log");
                    ItemTooltip(tip.c_str());
                }
            }
        }
        ImGui::PopClipRect();
    }
}
