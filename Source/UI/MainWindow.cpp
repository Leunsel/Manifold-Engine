#include "UI/MainWindow.h"

#include "Core/Errors.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "UI/CodeEditor.h"
#include "UI/Fonts.h"
#include "UI/Loading.h"
#include "UI/ProfilerWindow.h"
#include "UI/ScriptUi.h"
#include "UI/Shortcuts.h"
#include "UI/UiHost.h"
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

        constexpr double kSpeedPresets[] = { 0.25, 0.5, 1.0, 2.0, 5.0, 10.0 };
        constexpr double kSpeedSteps[] = { 0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0, 20.0, 50.0, 100.0 };

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

        bool MergedIntoOwnMenu(const Lua::ScriptMenuItem& item)
        {
            return item.Merge == "file" || item.Merge == "edit" || item.Merge == "table" || item.Merge == "help";
        }

        bool WindowButton(const char* id, const char* icon, float width, float height, bool danger)
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, height));
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            if (hovered)
                drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), danger ? IM_COL32(196, 43, 28, 255) : ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.07f)));
            const ImVec2 size = ImGui::CalcTextSize(icon);
            const ImU32 iconColor = hovered && danger ? IM_COL32(255, 255, 255, 255)
                : ImGui::GetColorU32(hovered ? ManifoldTheme::Text : ManifoldTheme::Mix(ManifoldTheme::Muted, ManifoldTheme::Text, 0.2f));
            drawList->AddText(ImVec2(std::round(pos.x + (width - size.x) * 0.5f), std::round(pos.y + (height - size.y) * 0.5f)), iconColor, icon);
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

        m_release = m_engine.Options().ReleaseMode;
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
        HoldEscape();
        ImGui::End();
    }

    void MainWindow::MainShortcuts()
    {
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::Main && Pressed(info.Id))
                Run(info.Id);
        }
        ScriptMenuShortcuts();
    }

    bool MainWindow::CanRun(Command command)
    {
        Scan::Scanner& scanner = m_engine.Scanner();
        const bool scannerShown = !m_engine.Options().CompactMode;
        switch (command)
        {
        case Command::OpenLastProcess: return m_workspace.CanOpenLastProcess();
        case Command::FocusScanValue: return scannerShown;
        case Command::FirstScan: return scannerShown && !scanner.Busy();
        case Command::NewScan: return scannerShown && !scanner.Busy() && scanner.HasResults();
        case Command::UndoScan: return !m_release && !scanner.Busy() && scanner.CanUndo();
        case Command::TableLuaScript:
        case Command::TableFiles:
        case Command::AddAddress:
        case Command::AddGroup:
        case Command::AddScript:
            return !m_release;
        case Command::ToggleSpeedhack:
        case Command::SpeedhackFaster:
        case Command::SpeedhackSlower:
            return m_engine.HasProcess();
        case Command::NewTable:
        case Command::DeactivateAll:
        case Command::CompactMode:
        case Command::ReleaseMode:
            return true;
        default:
            return m_workspace.CanRun(command);
        }
    }

    void MainWindow::Run(Command command)
    {
        if (!CanRun(command))
            return;
        switch (command)
        {
        case Command::NewTable: m_workspace.NewTable(); break;
        case Command::OpenLastProcess: m_workspace.OpenLastProcess(); break;
        case Command::DeactivateAll: m_workspace.TurnOffEverything(); break;
        case Command::CompactMode: ToggleCompactMode(); break;
        case Command::ReleaseMode: ToggleReleaseMode(); break;
        case Command::ToggleSpeedhack: ToggleSpeedhack(); break;
        case Command::SpeedhackFaster: StepSpeed(1); break;
        case Command::SpeedhackSlower: StepSpeed(-1); break;
        case Command::FocusScanValue: m_focusValue = true; break;
        case Command::FirstScan:
            if (m_engine.Scanner().HasResults())
                DoNextScan();
            else
                DoFirstScan();
            break;
        case Command::NewScan: NewScan(); break;
        case Command::UndoScan: m_engine.Scanner().Undo(); break;
        case Command::TableLuaScript: m_workspace.OpenScript(m_engine.Table().TableLuaScript, kTableLuaScript, true); break;
        case Command::TableFiles: m_workspace.OpenTableFiles(); break;
        case Command::AddAddress: OpenAddDialog(); break;
        case Command::AddGroup: m_engine.Table().AddGroup("Group"); break;
        case Command::AddScript: AddScriptRecord(); break;
        default: m_workspace.Run(command); break;
        }
    }

    bool MainWindow::Item(const char* label, Command command, bool selected)
    {
        if (!CommandItem(label, command, selected, CanRun(command)))
            return false;
        Run(command);
        return true;
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
        const bool scriptRoots = std::any_of(m_scriptMenu.begin(), m_scriptMenu.end(), [&](const Lua::ScriptMenuItem& root) { return !MergedIntoOwnMenu(root) && root.Caption != "-"; });
        const bool extensions = scriptRoots || m_workspace.Ui().HasMenu() || !m_release;
        const float buttonWidth = Px(46.0f);
        const float buttonsStart = width - buttonWidth * 3.0f;
        const float menusRoom = buttonsStart - menusStart - Px(8.0f);
        const float menuChildWidth = std::max(Px(120.0f), m_menusWidth > 0.0f ? std::min(m_menusWidth, menusRoom) : menusRoom);
        ImGui::BeginChild("##menus", ImVec2(menuChildWidth, menuHeight), ImGuiChildFlags_None, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        float menusEnd = origin.x + menusStart;

        Engine& engine = m_engine;
        Workspace& workspace = m_workspace;
        Table::AddressList& list = engine.Table();
        int scriptOpened = 0;
        int scriptClicked = 0;
        ImGui::BeginMenuBar();
        if (ImGui::BeginMenu("File"))
        {
            Item(ICON_FA_FILE "  New Table", Command::NewTable);
            Item(ICON_FA_FOLDER_OPEN "  Open Table...", Command::OpenTable);
            if (!m_release && ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Merge Table..."))
                workspace.OpenTableDialog(true);
            if (ImGui::BeginMenu(ICON_FA_HISTORY "  Recent Tables", !engine.Options().RecentTables.empty()))
            {
                const std::vector<std::string> recent = engine.Options().RecentTables;
                for (const std::string& path : recent)
                {
                    if (ImGui::MenuItem(path.c_str()))
                        workspace.OpenTable(Utf8ToWide(path), false);
                }
                ImGui::EndMenu();
            }
            Item(ICON_FA_SAVE "  Save", Command::SaveTable);
            Item(ICON_FA_SAVE "  Save As...", Command::SaveTableAs);
            if (ImGui::BeginMenu(ICON_FA_LIFE_RING "  Backups"))
            {
                if (ImGui::IsWindowAppearing())
                {
                    m_backupVersions = Backups::Versions(engine.Backups().SessionFolder());
                    if (m_backupVersions.size() > 12)
                        m_backupVersions.resize(12);
                }
                const std::vector<std::filesystem::path> versions = m_backupVersions;
                Muted(versions.empty() ? "No backups of " + engine.Backups().Name() + " yet" : "Backups of " + engine.Backups().Name());
                for (const std::filesystem::path& version : versions)
                {
                    if (ImGui::MenuItem((ICON_FA_CLOCK "  " + Backups::When(version) + "##" + WideToUtf8(version.filename().wstring())).c_str()))
                        workspace.OpenBackup(version);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open a Backup..."))
                    workspace.OpenBackupDialog();
                if (ImGui::MenuItem(ICON_FA_SAVE "  Back Up Now"))
                    workspace.BackUpNow();
                if (ImGui::MenuItem(ICON_FA_ARCHIVE "  Open the Backups Folder"))
                    workspace.OpenBackupFolder();
                ImGui::EndMenu();
            }
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
                Item(ICON_FA_UNDO "  Undo Last Scan", Command::UndoScan);
                if (CommandItem(ICON_FA_UNDO_ALT "  Undo Last Value Change", Command::UndoRecordValue, false, !m_valueUndo.empty()))
                    UndoValueChange();
                ImGui::Separator();
                const bool selection = !list.Selection().empty();
                if (CommandItem(ICON_FA_CUT "  Cut Records", Command::CutRecords, false, selection))
                    RunRecordCommand(Command::CutRecords, nullptr);
                if (CommandItem(ICON_FA_COPY "  Copy Records", Command::CopyRecords, false, selection))
                    RunRecordCommand(Command::CopyRecords, nullptr);
                if (CommandItem(ICON_FA_PASTE "  Paste Records", Command::PasteRecords))
                    RunRecordCommand(Command::PasteRecords, nullptr);
                if (CommandItem(ICON_FA_CHECK_DOUBLE "  Select All Records", Command::SelectAllRecords))
                    RunRecordCommand(Command::SelectAllRecords, nullptr);
                if (DangerCommandItem(ICON_FA_TRASH "  Delete Records", Command::DeleteRecords, selection))
                    RunRecordCommand(Command::DeleteRecords, nullptr);
                ImGui::Separator();
            }
            Item(ICON_FA_COG "  Settings", Command::OpenSettings);
            ScriptMenuInto("edit", scriptOpened, scriptClicked);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Process"))
        {
            ProcessItems();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Table"))
        {
            if (!m_release)
            {
                Item(ICON_FA_PLUS "  Add Address Manually", Command::AddAddress);
                Item(ICON_FA_FOLDER_PLUS "  Add Group", Command::AddGroup);
                Item(ICON_FA_CODE "  Add Auto Assembler Script", Command::AddScript);
                ImGui::Separator();
                Item(ICON_FA_SCROLL "  Table Lua Script", Command::TableLuaScript);
                Item(ICON_FA_FILE_ARCHIVE "  Table Files", Command::TableFiles);
                ImGui::Separator();
            }
            Item(ICON_FA_TOGGLE_OFF "  Deactivate All Records", Command::DeactivateAll);
            ScriptMenuInto("table", scriptOpened, scriptClicked);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            Settings& options = engine.Options();
            Item(ICON_FA_COMPRESS_ALT "  Compact Mode", Command::CompactMode, options.CompactMode);
            ItemTooltip("Only the address list, without the scanner");
            Item(ICON_FA_GAMEPAD "  Release Mode", Command::ReleaseMode, options.ReleaseMode);
            ItemTooltip("Hides addresses, types and the developer tools, so a table is simple to use. Turn it off here again.");
            if (!m_release)
            {
                ImGui::Separator();
                if (ImGui::BeginMenu("Address Column"))
                {
                    const char* const columns[] = { "As written", "Resolved address", "Written and resolved" };
                    for (int i = 0; i < 3; ++i)
                    {
                        if (ImGui::MenuItem(columns[i], nullptr, options.AddressColumn == i))
                        {
                            options.AddressColumn = i;
                            engine.SaveOptionsSoon();
                        }
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu("Address Digits"))
                {
                    const char* const digits[] = { "Short (P->F40000)", "At least 8 (P->00F40000)", "Pointer width (8 or 16)" };
                    for (int i = 0; i < 3; ++i)
                    {
                        if (ImGui::MenuItem(digits[i], nullptr, options.AddressDigits == i))
                        {
                            options.AddressDigits = i;
                            engine.SaveOptionsSoon();
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
        if (!m_release && ImGui::BeginMenu("Tools"))
        {
            Item(ICON_FA_MEMORY "  Memory Viewer", Command::OpenMemoryViewer);
            if (ImGui::MenuItem(ICON_FA_CLONE "  New Memory Viewer Window"))
                workspace.OpenMemoryViewer(0, false, true);
            Item(ICON_FA_SITEMAP "  Structure Dissect", Command::OpenStructureDissect);
            Item(ICON_FA_ROUTE "  Pointer Scan", Command::OpenPointerScan);
            Item(ICON_FA_CODE "  Auto Assembler", Command::OpenAutoAssembler);
            Item(ICON_FA_MOON "  Lua Engine", Command::OpenLuaEngine);
            Item(ICON_FA_STREAM "  Log", Command::OpenLog);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PUZZLE_PIECE "  Autorun Modules"))
                workspace.OpenModules();
            if (ImGui::MenuItem(ICON_FA_SYNC "  Reload All Modules"))
                workspace.Report(engine.RunAutorun());
            if (ImGui::MenuItem(ICON_FA_FOLDER "  Open Autorun Folder"))
            {
                std::error_code ignored;
                std::filesystem::create_directories(Paths::AutorunFolder(), ignored);
                ::ShellExecuteW(nullptr, L"open", Paths::AutorunFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::Separator();
            SectionLabel("Code, memory and debugger tools are in the memory viewer's menus");
            ImGui::EndMenu();
        }
        if (extensions && ImGui::BeginMenu("Extensions"))
        {
            if (!m_release)
            {
                UnrealMenu();
                UnityMenu();
                RttiMenu();
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_PUZZLE_PIECE "  Modules"))
                    workspace.OpenModules();
                if (scriptRoots || workspace.Ui().HasMenu())
                    ImGui::Separator();
            }
            const ScriptMenuClicks clicks = DrawScriptMenuRoots(m_scriptMenu, "##mainscript", &MergedIntoOwnMenu);
            if (clicks.Opened)
                scriptOpened = clicks.Opened;
            if (clicks.Clicked)
                scriptClicked = clicks.Clicked;
            if (workspace.Ui().HasMenu())
            {
                if (scriptRoots)
                    ImGui::Separator();
                workspace.Ui().DrawMenuItems();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help"))
        {
            Item(ICON_FA_KEYBOARD "  Keyboard Shortcuts", Command::OpenShortcuts);
            if (!m_release && ImGui::MenuItem(ICON_FA_LIST_OL "  Error Codes"))
                workspace.OpenLog();
            if (engine.Options().ProfilerEnabled)
                Item(ICON_FA_TACHOMETER_ALT "  Profiler and Diagnostics", Command::OpenProfiler);
            if (!Target::IsElevated() && ImGui::MenuItem(ICON_FA_SHIELD_ALT "  Restart as Administrator"))
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
        m_menusWidth = menusEnd - (origin.x + menusStart) + Px(16.0f);
        if (scriptOpened)
            engine.Lua().ClickMenuItem(scriptOpened);
        if (scriptClicked)
            engine.Lua().ClickMenuItem(scriptClicked);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);

        const float dragStart = menusEnd - origin.x + Px(8.0f);
        const float toolsStart = TitleTools(dragStart + Px(10.0f), buttonsStart - Px(8.0f), height);
        workspace.Window().AddDragArea(dragStart, 0.0f, std::max(dragStart, toolsStart - Px(6.0f)), height);
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

    float MainWindow::TitleTools(float left, float right, float height)
    {
        const float frame = ImGui::GetFrameHeight();
        const float y = std::round((height - frame) * 0.5f);
        float x = right;
        {
            const GhostButtons ghost;
            if (!m_release && x - frame > left)
            {
                x -= frame;
                ImGui::SetCursorPos(ImVec2(x, y));
                if (IconButton(ICON_FA_MEMORY "##titlememory", WithKeys("Memory view: the disassembler and hex view of the process", Command::OpenMemoryViewer).c_str()))
                    Run(Command::OpenMemoryViewer);
                x -= Px(4.0f);
            }
            const float tools = m_workspace.Ui().ToolbarWidth();
            if (tools > 0.0f && x - tools > left)
            {
                m_workspace.Ui().DrawToolbar(x, y);
                x -= tools + Px(4.0f);
            }
        }
        return ProcessChip(left, x - Px(6.0f), y, frame);
    }

    float MainWindow::ProcessChip(float left, float right, float y, float height)
    {
        Engine& engine = m_engine;
        const bool hasProcess = engine.HasProcess();
        const float pad = Px(10.0f);
        const float gap = Px(7.0f);
        const float line = ImGui::GetTextLineHeight();
        const float icon = std::round(line);
        if (right - left < height)
            return right;

        std::string name;
        std::string detail;
        std::string badge;
        ImVec4 badgeColor = ManifoldTheme::Accent;
        Debug::Debugger& debugger = engine.Debugger();
        if (hasProcess)
        {
            const Target::Process& process = *engine.Process();
            name = process.Name();
            if (!m_release)
                detail = Format("%s  ·  pid %u", process.Is64Bit() ? "64-bit" : "32-bit", process.Id());
            if (!m_release && debugger.Attached())
            {
                badge = debugger.Broken() ? ICON_FA_PAUSE_CIRCLE : ICON_FA_BUG;
                badgeColor = debugger.Broken() ? ManifoldTheme::Warning : ManifoldTheme::Accent;
            }
        }
        else
        {
            name = m_release ? "Open a Game" : "Open Process";
        }
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        const float nameFull = ImGui::CalcTextSize(name.c_str()).x;
        ImGui::PopFont();
        const float detailWidth = detail.empty() ? 0.0f : ImGui::CalcTextSize(detail.c_str()).x;
        const float chevronSize = std::round(ImGui::GetFontSize() * 0.72f);
        const float chevronWidth = ImGui::GetFont()->CalcTextSizeA(chevronSize, FLT_MAX, 0.0f, ICON_FA_CHEVRON_DOWN).x;
        const float badgeWidth = badge.empty() ? 0.0f : ImGui::CalcTextSize(badge.c_str()).x;
        const auto measure = [&](bool withDetail, bool withChevron, float nameWidth)
            {
                float width = pad + icon + gap + nameWidth + pad;
                if (withDetail && detailWidth > 0.0f)
                    width += gap + Px(2.0f) + detailWidth;
                if (badgeWidth > 0.0f)
                    width += gap + badgeWidth;
                if (withChevron)
                    width += gap + chevronWidth;
                return width;
            };
        const float room = right - left;
        bool withDetail = measure(true, true, nameFull) <= room;
        bool withChevron = measure(false, true, nameFull) <= room;
        const float nameWidth = std::min(nameFull, std::max(0.0f, room - measure(withDetail, withChevron, 0.0f)));
        const float width = std::min(room, measure(withDetail, withChevron, nameWidth));
        const float x = right - width;

        ImGui::SetCursorPos(ImVec2(x, y));
        const bool clicked = ImGui::InvisibleButton("##processchip", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const bool callToAction = !hasProcess;
        const double now = ImGui::GetTime();
        const std::uint32_t pid = hasProcess ? Raw(engine.Process()->Id()) : 0u;
        if (pid != m_chipProcess)
        {
            m_chipProcess = pid;
            m_chipConnectedAt = pid ? now : -1.0;
        }
        const float target = hovered || active ? 1.0f : 0.0f;
        m_chipHover += (target - m_chipHover) * std::min(1.0f, ImGui::GetIO().DeltaTime * 10.0f);
        if (std::fabs(target - m_chipHover) < 0.01f)
            m_chipHover = target;
        else
            NoteAnimation();
        const float hover = m_chipHover;
        const float rounding = height * 0.5f;
        const ImVec4 accent = ManifoldTheme::Accent;
        const float press = active ? 1.0f : 0.0f;
        const ImVec4 fill = callToAction ? ManifoldTheme::WithAlpha(accent, 0.18f + 0.09f * hover + 0.07f * press)
                                         : ManifoldTheme::Mix(ManifoldTheme::WithAlpha(ManifoldTheme::Text, 0.045f), ManifoldTheme::WithAlpha(accent, 0.11f + 0.06f * press), hover);
        const ImVec4 edge = callToAction ? ManifoldTheme::WithAlpha(accent, 0.55f + 0.30f * hover) : ManifoldTheme::Mix(ManifoldTheme::Border, ManifoldTheme::WithAlpha(accent, 0.70f), hover);
        if (hover > 0.01f)
        {
            for (int ring = 1; ring <= 3; ++ring)
            {
                const float grow = Px(1.0f) * static_cast<float>(ring);
                draw->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), ImGui::GetColorU32(ManifoldTheme::WithAlpha(accent, 0.14f * hover / static_cast<float>(ring))),
                    rounding + grow, 0, Px(1.0f));
            }
        }
        draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
        if (callToAction)
        {
            constexpr double kPeriod = 6.5;
            constexpr double kSweep = 1.1;
            const double phase = std::fmod(now, kPeriod);
            if (phase < kSweep)
            {
                NoteAnimation();
                const float t = static_cast<float>(phase / kSweep);
                const float eased = t * t * (3.0f - 2.0f * t);
                const float band = Px(22.0f);
                const float skew = Px(8.0f);
                const float from = min.x + rounding + skew;
                const float to = max.x - rounding - skew;
                const float center = from - band + (to - from + band * 2.0f) * eased;
                const float strength = std::sin(t * 3.14159265f);
                for (int layer = 0; layer < 3; ++layer)
                {
                    const float half = band * 0.5f * (1.0f - 0.3f * static_cast<float>(layer));
                    const float a = std::max(from, center - half);
                    const float b = std::min(to, center + half);
                    if (b <= a)
                        continue;
                    const ImVec2 quad[4] = { ImVec2(a + skew, min.y + Px(1.0f)), ImVec2(b + skew, min.y + Px(1.0f)), ImVec2(b - skew, max.y - Px(1.0f)), ImVec2(a - skew, max.y - Px(1.0f)) };
                    draw->AddConvexPolyFilled(quad, 4, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.065f * strength)));
                }
            }
        }
        draw->AddRect(min, max, ImGui::GetColorU32(edge), rounding, 0, Px(1.0f));
        float connect = 0.0f;
        if (m_chipConnectedAt >= 0.0)
        {
            const float since = static_cast<float>(now - m_chipConnectedAt);
            if (since < 1.0f)
            {
                NoteAnimation();
                connect = 1.0f - since;
                const float out = 1.0f - connect * connect * connect;
                const float grow = Px(1.0f) + std::max(0.0f, y - Px(1.5f)) * out;
                draw->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), ImGui::GetColorU32(ManifoldTheme::WithAlpha(accent, 0.6f * connect)),
                    rounding + grow, 0, Px(1.5f));
            }
            else
                m_chipConnectedAt = -1.0;
        }

        const float textY = std::round(min.y + (height - line) * 0.5f);
        float cx = min.x + pad;
        void* texture = hasProcess ? m_processIcon.Texture(m_workspace.Window(), engine.Process()->Path(), static_cast<int>(icon)) : nullptr;
        if (texture)
            draw->AddImage(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture))), ImVec2(cx, textY), ImVec2(cx + icon, textY + icon));
        else
            draw->AddText(ImVec2(cx, textY), ImGui::GetColorU32(callToAction ? accent : ManifoldTheme::Label), ICON_FA_DESKTOP);
        if (hasProcess)
        {
            const ImVec2 dot(cx + icon - Px(1.0f), textY + icon - Px(1.0f));
            const float radius = Px(2.6f) + Px(1.6f) * connect;
            draw->AddCircleFilled(dot, radius + Px(1.3f), ImGui::GetColorU32(ManifoldTheme::Sidebar));
            draw->AddCircleFilled(dot, radius, ImGui::GetColorU32(debugger.Broken() ? ManifoldTheme::Warning : ManifoldTheme::Success));
        }
        cx += icon + gap;
        if (nameWidth > Px(4.0f))
        {
            ImGui::PushFont(Fonts::Bold(), 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, callToAction ? ManifoldTheme::Accent : ManifoldTheme::Text);
            ImGui::RenderTextEllipsis(draw, ImVec2(cx, textY), ImVec2(cx + nameWidth, textY + line), cx + nameWidth, name.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            cx += nameWidth;
        }
        if (withDetail && detailWidth > 0.0f)
        {
            cx += gap + Px(2.0f);
            draw->AddText(ImVec2(cx, textY), ImGui::GetColorU32(ManifoldTheme::Muted), detail.c_str());
            cx += detailWidth;
        }
        ImRect badgeRect;
        if (badgeWidth > 0.0f)
        {
            cx += gap;
            draw->AddText(ImVec2(cx, textY), ImGui::GetColorU32(badgeColor), badge.c_str());
            badgeRect = ImRect(ImVec2(cx - Px(3.0f), min.y), ImVec2(cx + badgeWidth + Px(3.0f), max.y));
            cx += badgeWidth;
        }
        if (withChevron)
        {
            cx += gap;
            draw->AddText(ImGui::GetFont(), chevronSize, ImVec2(cx, std::round(min.y + (height - chevronSize) * 0.5f + Px(1.5f) * hover)),
                ImGui::GetColorU32(ManifoldTheme::Mix(ManifoldTheme::Muted, accent, hover)), ICON_FA_CHEVRON_DOWN);
        }

        const bool onBadge = badgeWidth > 0.0f && badgeRect.Contains(ImGui::GetIO().MousePos);
        if (clicked)
        {
            if (onBadge && debugger.Broken())
                m_workspace.OpenMemoryViewer(debugger.Break().Address);
            else
                m_workspace.OpenProcessPicker();
        }
        if (rightClicked)
            ImGui::OpenPopup("##processchipmenu");
        if (hovered)
        {
            std::string tip;
            if (onBadge)
                tip = debugger.Broken() ? "The process is stopped at a breakpoint. Click to show it." : "The debugger is attached to the process.";
            else if (hasProcess)
            {
                const Target::Process& process = *engine.Process();
                tip = process.Name() + "\n" + WideToUtf8(process.Path());
                if (!m_release)
                    tip += Format("\npid %u  ·  %s  ·  %zu modules", process.Id(), process.Is64Bit() ? "64-bit" : "32-bit", process.Modules()->size());
                tip += "\n\n" + WithKeys("Click to pick another process, right-click for more", Command::OpenProcess);
            }
            else
                tip = WithKeys("Pick the process to work on", Command::OpenProcess);
            ItemTooltip(tip.c_str());
        }
        ProcessChipMenu();
        return x;
    }

    void MainWindow::ProcessItems()
    {
        Item(ICON_FA_DESKTOP "  Open Process...", Command::OpenProcess);
        const std::string& last = m_engine.Options().LastProcess;
        const std::string lastLabel = ICON_FA_HISTORY "  Open " + (last.empty() ? std::string("the last process") : last);
        Item(lastLabel.c_str(), Command::OpenLastProcess);
        if (ImGui::MenuItem(ICON_FA_TIMES "  Close Process", nullptr, false, m_engine.HasProcess()))
            m_engine.CloseProcess();
    }

    void MainWindow::ProcessChipMenu()
    {
        if (!ImGui::BeginPopup("##processchipmenu"))
            return;
        Engine& engine = m_engine;
        const bool hasProcess = engine.HasProcess();
        ProcessItems();
        if (hasProcess && engine.HasProcess())
        {
            const Target::Process& process = *engine.Process();
            const std::string name = process.Name();
            const std::string id = ToText(process.Id());
            const std::wstring path = process.Path();
            if (!m_release)
            {
                ImGui::Separator();
                Item(ICON_FA_MEMORY "  Memory Viewer", Command::OpenMemoryViewer);
                if (engine.Debugger().Broken() && ImGui::MenuItem(ICON_FA_PAUSE_CIRCLE "  Show the Break"))
                    m_workspace.OpenMemoryViewer(engine.Debugger().Break().Address);
            }
            ImGui::Separator();
            SpeedhackMenu();
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Name"))
                ImGui::SetClipboardText(name.c_str());
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy the Process ID"))
                ImGui::SetClipboardText(id.c_str());
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show the File in Explorer", nullptr, false, !path.empty()))
            {
                const std::wstring arguments = L"/select,\"" + path + L"\"";
                ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::EndPopup();
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
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(ICON_FA_ARROW_DOWN "  Add 0000 Selected").x - Px(16.0f));
            if (ImGui::SmallButton((ICON_FA_ARROW_DOWN "  Add " + std::to_string(m_selectedResults.size()) + " Selected").c_str()))
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
        if (CommandItem(ICON_FA_ARROW_DOWN "  Add Selected to the Address List", Command::AddResults))
            RunResultCommand(Command::AddResults, index);
        if (!m_release && CommandItem(ICON_FA_EDIT "  Change the Value of the Selected Results", Command::ChangeResultValue))
            RunResultCommand(Command::ChangeResultValue, index);
        if (m_release)
            return;
        ImGui::Separator();
        if (CommandItem(ICON_FA_MEMORY "  Browse This Memory Region", Command::BrowseResult))
            RunResultCommand(Command::BrowseResult, index);
        if (CommandItem(ICON_FA_CODE "  Disassemble This Memory Region", Command::DisassembleResult))
            RunResultCommand(Command::DisassembleResult, index);
        if (CommandItem(ICON_FA_SITEMAP "  Dissect Data at This Address", Command::DissectResult))
            RunResultCommand(Command::DissectResult, index);
        ImGui::Separator();
        if (CommandItem(ICON_FA_EYE "  Find Out What Accesses This Address", Command::ResultAccesses))
            RunResultCommand(Command::ResultAccesses, index);
        if (CommandItem(ICON_FA_PEN "  Find Out What Writes to This Address", Command::ResultWrites))
            RunResultCommand(Command::ResultWrites, index);
        ImGui::Separator();
        if (CommandItem(ICON_FA_COPY "  Copy the Selected Addresses", Command::CopyResults))
            RunResultCommand(Command::CopyResults, index);
        if (CommandItem(ICON_FA_CHECK_DOUBLE "  Select All Results", Command::SelectAllResults))
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
        if (!BeginModal("Change value##results", ICON_FA_EDIT, "Change Value", ImVec2(380.0f, 0.0f)))
            return;
        Scan::Scanner& scanner = m_engine.Scanner();
        Muted(std::to_string(m_resultValueTargets.size()) + (m_resultValueTargets.size() == 1 ? " result, " : " results, ") + ScanTypeLabel());
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool apply = InputText("##resultvalue", m_resultValueText, ImGuiInputTextFlags_EnterReturnsTrue);
        if (DialogButtons("OK", true, false) == DialogChoice::Ok || apply)
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
        if (const int clicked = ScriptShortcutPressed(m_scriptMenu, own, CommandScope::Main))
            m_engine.Lua().ClickMenuItem(clicked);
    }

    void MainWindow::SaveResultsDialog()
    {
        if (m_saveResultsDialog)
        {
            ImGui::OpenPopup("Save results##scan");
            m_saveResultsDialog = false;
        }
        if (!BeginModal("Save results##scan", ICON_FA_SAVE, "Save Results", ImVec2(380.0f, 0.0f)))
            return;
        Scan::Scanner& scanner = m_engine.Scanner();
        Muted("A next scan can compare with these results later. They stay until New scan.");
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool apply = InputTextWithHint("##savedname", "name", m_saveResultsName, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool named = !TrimView(m_saveResultsName).empty();
        if (DialogButtons("Save", named, false) == DialogChoice::Ok || (apply && named))
        {
            if (scanner.SaveResults(m_saveResultsName))
            {
                m_savedName = Trim(m_saveResultsName);
                m_engine.Notify(LogLevel::Info, "Saved " + Grouped(scanner.Count()) + " results as \"" + m_savedName + "\".");
            }
            ImGui::CloseCurrentPopup();
        }
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
        const bool iconsOnly = buttonWidth < ImGui::CalcTextSize(ICON_FA_FILTER "  Next Scan").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const char* firstLabel = iconsOnly ? ICON_FA_SEARCH "##firstscan" : ICON_FA_SEARCH "  First Scan";
        const char* newLabel = iconsOnly ? ICON_FA_REDO "##newscan" : ICON_FA_REDO "  New Scan";
        const char* nextLabel = iconsOnly ? ICON_FA_FILTER "##nextscan" : ICON_FA_FILTER "  Next Scan";
        const char* undoLabel = iconsOnly ? ICON_FA_UNDO "##undoscan" : ICON_FA_UNDO "  Undo Scan";

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
                    ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(module.Base)));
                    if (ImGui::Selectable(module.Name.c_str()))
                    {
                        m_start = Hex(module.Base);
                        m_stop = Hex(module.Base + module.Size);
                    }
                    ImGui::PopID();
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
        SpeedhackSection();
        EndCard();
    }

    void MainWindow::ApplySpeed(double speed, bool enable)
    {
        Target::Speedhack& speedhack = m_engine.Speed();
        m_speedValue = std::clamp(speed, 0.0, Target::Speedhack::kMaxSpeed);
        if (!enable && !speedhack.Enabled())
            return;
        if (const Status status = speedhack.SetSpeed(m_speedValue); !status)
            m_engine.Notify(status);
    }

    void MainWindow::ToggleSpeedhack()
    {
        Target::Speedhack& speedhack = m_engine.Speed();
        if (!speedhack.Enabled())
        {
            ApplySpeed(m_speedValue, true);
            return;
        }
        if (const Status status = speedhack.Disable(); !status)
            m_engine.Notify(status);
    }

    void MainWindow::StepSpeed(int direction)
    {
        const double current = m_engine.Speed().Enabled() ? m_engine.Speed().Speed() : 1.0;
        double next = current;
        if (direction > 0)
        {
            const auto above = std::find_if(std::begin(kSpeedSteps), std::end(kSpeedSteps), [&](double step) { return step > current * 1.0001; });
            next = above != std::end(kSpeedSteps) ? *above : current;
        }
        else
        {
            for (double step : kSpeedSteps)
            {
                if (step < current * 0.9999)
                    next = step;
            }
        }
        ApplySpeed(next, true);
    }

    void MainWindow::RemoveSpeedhackHooks()
    {
        m_workspace.Confirm("Remove Speedhack Hooks",
            "The time functions of the process get their original bytes back. Its clocks then jump to the real time, backwards if the game ran faster, and some games freeze or stutter until the "
            "real time has caught up.",
            [this]()
            {
                if (const Status status = m_engine.Speed().Remove(); !status)
                    m_engine.Notify(status);
            },
            "Remove");
    }

    void MainWindow::SpeedhackMenu()
    {
        Target::Speedhack& speedhack = m_engine.Speed();
        const bool enabled = speedhack.Enabled();
        const double speed = speedhack.Speed();
        if (!ImGui::BeginMenu(ICON_FA_TACHOMETER_ALT "  Speedhack", m_engine.HasProcess()))
            return;
        Item(enabled ? ICON_FA_TOGGLE_ON "  Turn Off" : ICON_FA_TOGGLE_OFF "  Turn On", Command::ToggleSpeedhack);
        Item(ICON_FA_FORWARD "  Faster", Command::SpeedhackFaster);
        Item(ICON_FA_BACKWARD "  Slower", Command::SpeedhackSlower);
        ImGui::Separator();
        for (double preset : kSpeedPresets)
        {
            if (ImGui::MenuItem((Target::Speedhack::Text(preset) + "x").c_str(), nullptr, enabled && speed == preset))
                ApplySpeed(preset, true);
        }
        ImGui::Separator();
        if (DangerMenuItem(ICON_FA_UNDO "  Remove Hooks...", nullptr, speedhack.Installed()))
            RemoveSpeedhackHooks();
        ImGui::EndMenu();
    }

    void MainWindow::SpeedhackSection()
    {
        Target::Speedhack& speedhack = m_engine.Speed();
        const bool hasProcess = m_engine.HasProcess();
        bool enabled = speedhack.Enabled();
        const bool installed = speedhack.Installed();
        if (enabled)
            m_speedValue = speedhack.Speed();
        const std::string label = "Speedhack" + (enabled ? "  \xc2\xb7  " + Target::Speedhack::Text(m_speedValue) + "x" : std::string()) + "###speedhack";
        if ((enabled || installed) && !m_speedShown)
            ImGui::SetNextItemOpen(true);
        m_speedShown = enabled || installed;
        if (!ImGui::CollapsingHeader(label.c_str()))
            return;
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::BeginDisabled(!hasProcess);
        if (ToggleSwitch("##speedhackon", &enabled))
            ToggleSpeedhack();
        ItemTooltip(WithKeys("The time functions of the process run faster or slower by the factor", Command::ToggleSpeedhack).c_str());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Enable Speedhack");
        ImGui::SameLine();
        const float fieldWidth = Px(64.0f);
        const float room = ImGui::GetContentRegionAvail().x;
        if (room > fieldWidth + ImGui::CalcTextSize("Speed").x + style.ItemSpacing.x * 2.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + room - fieldWidth);
        ImGui::SetNextItemWidth(fieldWidth);
        double typed = m_speedValue;
        if (ImGui::InputDouble("##speedvalue", &typed, 0.0, 0.0, "%.4g"))
            m_speedTyped = typed;
        if (ImGui::IsItemDeactivatedAfterEdit())
            ApplySpeed(m_speedTyped, false);
        ItemTooltip("The speed factor: 2 is twice as fast, 0.5 half as fast, 0 stops the time");

        float slider = static_cast<float>(m_speedValue);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##speedslider", &slider, 0.1f, 10.0f, "%.2fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput))
            ApplySpeed(std::round(static_cast<double>(slider) * 100.0) / 100.0, false);
        ItemTooltip(WithKeys("Drag to change the speed", Command::SpeedhackFaster).c_str());

        const float presetWidth = std::floor((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * static_cast<float>(std::size(kSpeedPresets) - 1)) / static_cast<float>(std::size(kSpeedPresets)));
        for (std::size_t i = 0; i < std::size(kSpeedPresets); ++i)
        {
            const double preset = kSpeedPresets[i];
            const std::string text = Target::Speedhack::Text(preset) + "x##speedpreset";
            if (i > 0)
                ImGui::SameLine();
            const bool current = enabled && m_speedValue == preset;
            if (current ? AccentButton(text.c_str(), ImVec2(presetWidth, 0.0f)) : ImGui::Button(text.c_str(), ImVec2(presetWidth, 0.0f)))
                ApplySpeed(preset, true);
        }
        ImGui::EndDisabled();
        if (installed && !enabled)
        {
            Muted("Off. The hooks stay, so the game's clocks do not jump.");
            FlowNext(ImGui::CalcTextSize("Remove Hooks...").x + style.FramePadding.x * 2.0f);
            if (ImGui::SmallButton("Remove Hooks..."))
                RemoveSpeedhackHooks();
            ItemTooltip("Restores the time functions byte for byte. The game's clocks then jump to the real time.");
        }
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
        CardTitle(ICON_FA_LIST, "Address List", list.Count() ? std::to_string(list.Count()) + (list.Count() == 1 ? " record" : " records") : "");
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
            ItemTooltip(WithKeys("Add Address Manually", Command::AddAddress).c_str());
            ImGui::SameLine(0.0f, gap);
            if (ImGui::Button(iconsOnly ? ICON_FA_FOLDER_PLUS "##addgroup" : ICON_FA_FOLDER_PLUS "  Group", iconsOnly ? iconSize : ImVec2(0.0f, 0.0f)))
                list.AddGroup("Group");
            ItemTooltip(WithKeys("Add Group", Command::AddGroup).c_str());
            ImGui::SameLine(0.0f, gap);
            if (ImGui::Button(iconsOnly ? ICON_FA_CODE "##addscript" : ICON_FA_CODE "  Script", iconsOnly ? iconSize : ImVec2(0.0f, 0.0f)))
                AddScriptRecord();
            ItemTooltip(WithKeys("Add Auto Assembler Script", Command::AddScript).c_str());
            ImGui::PopStyleColor(3);
        }
        const float filterX = right - filterWidth - iconWidth - gap;
        const float groupsX = filterX - groupsWidth;
        ProgressLine(ownLine ? right : groupsX - Px(12.0f));
        if (!ownLine)
            ImGui::SameLine();
        ImGui::SetCursorPosX(groupsX);
        if (IconButton(ICON_FA_EXPAND_ALT, WithKeys("Expand Every Group", Command::ExpandGroups).c_str()))
            list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = true; });
        ImGui::SameLine(0.0f, gap);
        if (IconButton(ICON_FA_COMPRESS_ALT, WithKeys("Collapse Every Group", Command::CollapseGroups).c_str()))
            list.ForEach([](Table::MemoryRecord& record) { if (!record.Children.empty()) record.Expanded = false; });
        ImGui::SameLine();
        ImGui::SetCursorPosX(filterX);
        ImGui::SetNextItemWidth(filterWidth);
        InputTextWithHint("##tablefilter", ICON_FA_SEARCH " Filter", m_tableFilter);
        ImGui::SameLine(0.0f, gap);
        const bool filterOptions = m_filterActiveOnly || m_filterKind != 0;
        if (filterOptions)
            ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
        if (IconButton(ICON_FA_FILTER, filterOptions ? "Filter Options (some are on)" : "Filter Options"))
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
            Item(ICON_FA_PLUS "  Add Address Manually", Command::AddAddress);
            Item(ICON_FA_FOLDER_PLUS "  Add Group", Command::AddGroup);
            Item(ICON_FA_CODE "  Add Auto Assembler Script", Command::AddScript);
            if (CommandItem(ICON_FA_PASTE "  Paste", Command::PasteRecords))
                RunRecordCommand(Command::PasteRecords, nullptr);
            ImGui::Separator();
            if (CommandItem(ICON_FA_EXPAND_ALT "  Expand Every Group", Command::ExpandGroups))
                RunRecordCommand(Command::ExpandGroups, nullptr);
            if (CommandItem(ICON_FA_COMPRESS_ALT "  Collapse Every Group", Command::CollapseGroups))
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
            actions.push_back({ ICON_FA_FOLDER_OPEN "  Open Table", ShortcutText(Command::OpenTable) });
            if (!m_release)
                actions.push_back({ ICON_FA_PLUS "  Add Address" });
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
        m_workspace.Confirm("Delete Records", "Delete " + std::to_string(ids.size()) + (ids.size() == 1 ? " record" : " records") + " and their children?", remove, "Delete");
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

    void MainWindow::UnrealMenu()
    {
        if (!ImGui::BeginMenu(ICON_FA_CUBES "  Unreal Engine"))
            return;
        UnrealSession& unreal = m_engine.Unreal();
        Target::UnrealConfig& config = unreal.Config();
        const bool process = m_engine.HasProcess();
        const auto runtime = unreal.Runtime();
        SectionLabel(process ? unreal.StatusText().c_str() : "No process is open");
        if (unreal.Resolving())
        {
            ImGui::MenuItem(ICON_FA_SPINNER "  Reading the Reflection...", nullptr, false, false);
        }
        else if (ImGui::MenuItem(runtime ? ICON_FA_SYNC "  Resolve Again" : ICON_FA_PLAY "  Start", nullptr, false, process))
        {
            unreal.Start(false);
        }
        ItemTooltip("Reads the game's name table, object array and reflection layout, also in a game that does not look like Unreal.");
        if (ImGui::MenuItem(ICON_FA_POWER_OFF "  Shut Down", nullptr, false, !unreal.Stopped()))
            unreal.Stop();
        ItemTooltip("Drops what was resolved; dissects, symbols and types stop until Start, also in the next game.");
        ImGui::Separator();
        const auto dissect = [&](const char* label, std::uint64_t (Target::UnrealRuntime::*get)() const, const char* what)
            {
                if (!ImGui::MenuItem(label, nullptr, false, runtime != nullptr) || !runtime)
                    return;
                const std::uint64_t address = ((*runtime).*get)();
                if (address)
                    m_workspace.OpenStructure(address);
                else
                    m_engine.Notify(LogLevel::Info, std::string(what) + " was not found. Is a level loaded?");
            };
        dissect(ICON_FA_COGS "  Dissect the Engine", &Target::UnrealRuntime::Engine, "The engine object");
        dissect(ICON_FA_GLOBE "  Dissect the World", &Target::UnrealRuntime::World, "The world");
        dissect(ICON_FA_GAMEPAD "  Dissect the Player Controller", &Target::UnrealRuntime::Controller, "The local player controller");
        dissect(ICON_FA_USER "  Dissect the Pawn", &Target::UnrealRuntime::Pawn, "The local pawn");
        ImGui::Separator();
        Item(ICON_FA_CUBES "  Types, Properties and Who Owns an Address", Command::OpenUnreal);
        ImGui::Separator();
        SectionLabel("Built in");
        ImGui::MenuItem("Start by Itself in Unreal Games", nullptr, &config.AutoStart);
        ImGui::MenuItem("Structure Dissect Fields", nullptr, &config.Dissect);
        ItemTooltip("Structure Dissect fills Unreal objects with their reflected properties, nested structs, bitfield bools and the native gaps.");
        ImGui::MenuItem("Structure Names", nullptr, &config.Names);
        ItemTooltip("New structures are named after the object's class and start at the object, also for an address inside it.");
        ImGui::MenuItem("Offset Symbols", nullptr, &config.Symbols);
        ItemTooltip("Type_Property symbols such as Actor_RootComponent, and GObjects, FNamePool and GameEngine, work in addresses.");
        ImGui::MenuItem("Custom Types", nullptr, &config.Types);
        ItemTooltip("UE FName, UE FString and UE Object Index show names, strings and weak references as text.");
        ImGui::EndMenu();
    }

    void MainWindow::RttiMenu()
    {
        if (!ImGui::BeginMenu(ICON_FA_SITEMAP "  C++ RTTI"))
            return;
        RttiSession& rtti = m_engine.Rtti();
        SectionLabel(m_engine.HasProcess() ? rtti.StatusText().c_str() : "No process is open");
        ImGui::Separator();
        bool enabled = rtti.Enabled();
        if (ImGui::MenuItem("Structure Dissect Uses RTTI", nullptr, &enabled))
        {
            rtti.SetEnabled(enabled);
            m_engine.SaveOptionsSoon();
        }
        ItemTooltip("New structures and expanded pointers are named after the object's C++ class, pointer fields are labelled (vftable, Class*), "
            "and member objects with a virtual function table of their own become embedded structures. Programs built with MSVC or clang-cl, 64-bit and 32-bit.");
        if (ImGui::MenuItem(ICON_FA_ERASER "  Forget What Was Learned", nullptr, false, m_engine.HasProcess()))
            rtti.Reset();
        ItemTooltip("Drops the classes seen in this process and the Class::vftable symbols registered for them. The next lookup starts fresh.");
        ImGui::Separator();
        SectionLabel("Who owns an address, objects of a class: manifold.rtti in the Lua engine");
        ImGui::EndMenu();
    }

    void MainWindow::UnityMenu()
    {
        if (!ImGui::BeginMenu(ICON_FA_DICE_D6 "  Unity (Mono / IL2CPP)"))
            return;
        Target::MonoBridge& mono = m_engine.Mono();
        if (!m_engine.MonoPresent() && !m_monoDetected)
        {
            SectionLabel(m_engine.HasProcess() ? "No Mono or IL2CPP runtime in this process" : "No process is open");
        }
        else if (!m_engine.MonoPresent())
        {
            const bool il2cpp = m_engine.Process() && Target::MonoBridge::DetectRuntime(*m_engine.Process()) == Target::ManagedRuntime::Il2Cpp;
            const char* load = il2cpp ? ICON_FA_PLUG "  Load IL2CPP Runtime" : ICON_FA_PLUG "  Load Mono Runtime";
            if (ImGui::MenuItem(m_engine.Managed().Loading() ? ICON_FA_PLUG "  Loading the Runtime..." : load, nullptr, false, !m_engine.Managed().Loading()))
                m_engine.Managed().Load();
            ItemTooltip("Reads the classes and methods of the game's Mono or IL2CPP runtime, so method names show in the memory viewer, resolve in addresses and Structure Dissect finds the class of an object.");
        }
        else
        {
            const std::string loaded = mono.Module() + "  ·  " + Target::MonoBridge::RuntimeName(mono.Runtime()) + "  ·  " + std::to_string(mono.ClassCount()) + " classes";
            SectionLabel(loaded.c_str());
            if (ImGui::MenuItem(ICON_FA_SYNC "  Reload Classes", nullptr, false, !m_engine.Managed().Loading()))
                m_engine.Managed().Reload();
            ItemTooltip("Reads the class list again, for assemblies the game loaded later");
            if (ImGui::MenuItem(mono.IsIl2Cpp() ? ICON_FA_POWER_OFF "  Shut Down IL2CPP" : ICON_FA_POWER_OFF "  Shut Down Mono", nullptr, false, !m_engine.Managed().Loading()))
                m_engine.Managed().Stop();
            ItemTooltip("Ends the helper thread in the game and takes the Unity explorer's callback out. Managed method names and lookups stop until it is loaded again.");
        }
        ImGui::Separator();
        Item(ICON_FA_DICE_D6 "  Mono / .NET Dissector", Command::OpenMono);
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
                else if (column == 2)
                {
                    if (!lua.AddressListEvent("onaddresschange", &record))
                        m_workspace.OpenRecordEditor(record.Id, RecordField::Address);
                }
                else if (column == 3 && record.Kind == Table::RecordKind::Value)
                {
                    if (!lua.AddressListEvent("ontypechange", &record))
                        m_workspace.OpenRecordEditor(record.Id, RecordField::Type);
                }
                else if (column == 4 && record.Kind == Table::RecordKind::Value)
                {
                    StartInlineEdit(record);
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
        const bool addressHeader = record.Kind == Table::RecordKind::Group && record.AddressGroupHeader;
        if (!m_release && addressHeader)
            engine.ResolveRecord(record);
        if (!m_release && (record.Kind == Table::RecordKind::Value || addressHeader))
        {
            const RecordAddressText address = engine.Tables().AddressTextOf(record);
            ImGui::TextUnformatted(address.Text.c_str());
            const bool failed = !record.IsPointer() && !record.AddressOk && !record.AddressError.empty();
            if (failed)
                ErrorTooltip(record.AddressError);
            else if (!address.Tooltip.empty())
                ItemTooltip(address.Tooltip.c_str());
            if (!address.Resolved.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored(ManifoldTheme::Muted, "%s", address.Resolved.c_str());
                if (!address.Tooltip.empty())
                    ItemTooltip(address.Tooltip.c_str());
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
        m_inlineTargets = list.Selection();
        m_inlineText = record.ValueText == "??" ? "" : record.ValueText;
        m_inlineOriginal = m_inlineText;
        m_inlineFocus = true;
        m_inlineFrames = 0;
    }

    void MainWindow::InlineValue(Table::MemoryRecord& record)
    {
        const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(4.0f), 0.0f));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_inlineFocus && (m_inlineFrames == 0 || m_inlineFrames == 4))
            ImGui::ActivateItemByID(ImGui::GetID("##inlinevalue"));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, false);
        const bool enter = InputText("##inlinevalue", m_inlineText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopItemFlag();
        const ImGuiID id = ImGui::GetItemID();
        const bool active = ImGui::IsItemActive();
        const bool deactivated = ImGui::IsItemDeactivated();
        ImGui::PopStyleVar();
        if (m_inlineFocus && (active || ++m_inlineFrames > 8))
            m_inlineFocus = false;
        if (escape)
        {
            m_inlineEdit = -1;
            return;
        }
        if (enter || (deactivated && m_inlineText != m_inlineOriginal))
        {
            const std::vector<int> targets = m_inlineTargets.empty() ? std::vector<int>{ record.Id } : m_inlineTargets;
            m_inlineEdit = -1;
            WriteValues(targets, m_inlineText);
            return;
        }
        if (deactivated || (!active && !m_inlineFocus))
        {
            m_inlineEdit = -1;
            return;
        }
        ImGui::SetKeyOwner(ImGuiKey_Escape, id);
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
                   borderColor = ImGui::GetColorU32(ManifoldTheme::Border), shade = ImGui::GetColorU32(ManifoldTheme::Shadow), fade = ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Shadow, 0.0f)),
                   line = Px(1.0f), shadow = Px(6.0f), pieces = std::move(pieces), borders = std::move(borders)]()
            {
                draw->PushClipRect(clip.Min, clip.Max, false);
                draw->AddRectFilledMultiColor(ImVec2(min.x, max.y), ImVec2(max.x, max.y + shadow), shade, shade, fade, fade);
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
            if (CommandItem(ICON_FA_EDIT "  Change Value", Command::ChangeRecordValue))
                RunRecordCommand(Command::ChangeRecordValue, &record);
            if (CommandItem(ICON_FA_I_CURSOR "  Edit the Value in Place", Command::EditValueInline))
                RunRecordCommand(Command::EditValueInline, &record);
            if (!m_release && CommandItem(ICON_FA_UNDO_ALT "  Undo Last Value Change", Command::UndoRecordValue, false, !m_valueUndo.empty()))
                RunRecordCommand(Command::UndoRecordValue, &record);
            if (ImGui::BeginMenu(ICON_FA_SNOWFLAKE "  Freeze Mode"))
            {
                if (ImGui::MenuItem("Normal", nullptr, record.Freeze == Table::FreezeMode::Normal))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::Normal;
                if (CommandItem("Allow Increase", Command::FreezeAllowIncrease, record.Freeze == Table::FreezeMode::AllowIncrease))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::AllowIncrease;
                if (CommandItem("Allow Decrease", Command::FreezeAllowDecrease, record.Freeze == Table::FreezeMode::AllowDecrease))
                    for (auto* target : selected) target->Freeze = Table::FreezeMode::AllowDecrease;
                ImGui::EndMenu();
            }
            if (m_release)
                return;
            if (CommandItem(ICON_FA_HASHTAG "  Show as Hexadecimal", Command::ShowAsHex, record.ShowHex))
                RunRecordCommand(Command::ShowAsHex, &record);
            if (ImGui::MenuItem(ICON_FA_MINUS_CIRCLE "  Show as Signed", nullptr, record.ShowSigned))
            {
                for (auto* target : selected)
                    target->ShowSigned = !record.ShowSigned;
                engine.RefreshValues(true);
            }
            if (ImGui::BeginMenu(ICON_FA_EXCHANGE_ALT "  Change Type"))
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
        if (record.Kind == Table::RecordKind::Script && CommandItem(ICON_FA_CODE "  Edit Script", Command::EditScript))
            RunRecordCommand(Command::EditScript, &record);
        if (CommandItem(ICON_FA_SLIDERS_H "  Edit Record...", Command::EditRecord))
            RunRecordCommand(Command::EditRecord, &record);
        if (CommandItem(ICON_FA_KEYBOARD "  Assign a Hotkey...", Command::AssignHotkey))
            RunRecordCommand(Command::AssignHotkey, &record);
        if (CommandItem(ICON_FA_I_CURSOR "  Change Description", Command::ChangeDescription))
            RunRecordCommand(Command::ChangeDescription, &record);
        if (ImGui::BeginMenu(ICON_FA_SITEMAP "  Group Options"))
        {
            ImGui::MenuItem("Hide Children When Deactivated", nullptr, &record.Options.HideChildren);
            ImGui::MenuItem("Activate Children as Well", nullptr, &record.Options.ActivateChildren);
            ImGui::MenuItem("Deactivate Children as Well", nullptr, &record.Options.DeactivateChildren);
            ImGui::MenuItem("Set Value of Children Too", nullptr, &record.Options.RecursiveSetValue);
            ImGui::MenuItem("Always Hide Children", nullptr, &record.Options.AlwaysHideChildren);
            ImGui::MenuItem("Allow Manual Collapse and Expand", nullptr, &record.Options.AllowManualExpand);
            ImGui::EndMenu();
        }
        if (CommandItem(ICON_FA_FOLDER_PLUS "  Put Selected into a New Group", Command::GroupRecords))
            RunRecordCommand(Command::GroupRecords, &record);
        ImGui::Separator();
        if (record.Kind == Table::RecordKind::Value)
        {
            engine.ResolveRecord(record);
            ImGui::BeginDisabled(!record.AddressOk);
            if (CommandItem(ICON_FA_MEMORY "  Browse This Memory Region", Command::BrowseRecord))
                RunRecordCommand(Command::BrowseRecord, &record);
            if (CommandItem(ICON_FA_CODE "  Disassemble This Memory Region", Command::DisassembleRecord))
                RunRecordCommand(Command::DisassembleRecord, &record);
            if (CommandItem(ICON_FA_SITEMAP "  Dissect Data at This Address", Command::DissectRecord))
                RunRecordCommand(Command::DissectRecord, &record);
            if (ImGui::MenuItem(ICON_FA_ROUTE "  Pointer Scan for This Address"))
                m_workspace.OpenPointerScan(record.ResolvedAddress);
            if (CommandItem(ICON_FA_EYE "  Find Out What Accesses This Address", Command::RecordAccesses))
                RunRecordCommand(Command::RecordAccesses, &record);
            if (CommandItem(ICON_FA_PEN "  Find Out What Writes to This Address", Command::RecordWrites))
                RunRecordCommand(Command::RecordWrites, &record);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy Address"))
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
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste into This Record"))
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
        if (!BeginModal("Change value##main", ICON_FA_EDIT, "Change Value", ImVec2(entries.empty() ? 380.0f : 440.0f, 0.0f)))
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
            apply = InputText("##newvalue", m_valueText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll) || apply;
            if (ImGui::IsItemEdited() && !entries.empty())
            {
                m_valueCursor = FindEntry(entries, m_valueText);
                m_valueScroll = m_valueCursor >= 0 ? 2 : 0;
            }
        }
        if (DialogButtons("OK", true, false) == DialogChoice::Ok || apply)
        {
            WriteValues(m_valueTargets, m_valueText);
            ImGui::CloseCurrentPopup();
        }
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
        if (!BeginModal("Add address##main", ICON_FA_PLUS, "Add Address", ImVec2(420.0f, 0.0f)))
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
        if (DialogButtons("Add", true, false) == DialogChoice::Ok || (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && !m_addAddress.empty()))
        {
            m_engine.Table().AddValue(m_addDescription, Trim(m_addAddress), Scan::kRecordTypes[m_addType]);
            m_engine.RefreshValues(true);
            ImGui::CloseCurrentPopup();
        }
        EndModal();
    }

    void MainWindow::DescriptionDialog()
    {
        if (m_descriptionDialog)
        {
            ImGui::OpenPopup("Description##main");
            m_descriptionDialog = false;
        }
        if (!BeginModal("Description##main", ICON_FA_I_CURSOR, "Change Description", ImVec2(380.0f, 0.0f)))
            return;
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = InputText("##text", m_descriptionText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (DialogButtons("OK", true, false) == DialogChoice::Ok || enter)
        {
            if (Table::MemoryRecord* record = m_engine.Table().FindById(m_descriptionTarget))
            {
                record->Description = m_descriptionText;
                m_engine.Table().Touch();
            }
            ImGui::CloseCurrentPopup();
        }
        EndModal();
    }

    void MainWindow::SaveOptions()
    {
        m_engine.SaveOptionsSoon();
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
        const ErrorInfo* logError = newest && !m_release ? ErrorOf(newest->Message) : nullptr;
        const std::string logCode = logError ? ErrorName(logError->Code) : std::string();
        const std::string logText = newest ? std::string(logIcon) + "  " + (logError ? logCode : message) : std::string();
        const float logFull = newest ? measure(logText) : 0.0f;
        float logWant = logFull;

        const auto rightsWidth = [&]() { return measure(rightsShort ? std::string_view(rightsIcon) : std::string_view(rightsFull)); };
        const auto framesWidth = [&]() { return framesLevel == 0 ? framesFullWidth : framesLevel == 1 ? framesRateWidth : 0.0f; };
        const float scriptStatus = m_workspace.Ui().StatusWidth(gap);
        const auto overflow = [&]()
        {
            float total = tableWidth + gap + rightsWidth() + scriptStatus;
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
        if (scriptStatus > 0.0f)
            m_workspace.Ui().DrawStatus(x, y, gap);

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
                    // ∑ One action per click: the left button opens the log, the code and
                    // the message are copied from the right-click menu. A code stands in for
                    // the message in the bar; the tooltip explains it and keeps the text.
                    if (ImGui::IsItemHovered())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                        m_workspace.OpenLog();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !ImGui::IsPopupOpen("##statuslog"))
                    {
                        ShowTooltip([&]()
                            {
                                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                                if (logError)
                                {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Accent);
                                    ImGui::Text("%s  %s", logCode.c_str(), std::string(ErrorArea(logError->Code)).c_str());
                                    ImGui::PopStyleColor();
                                    ImGui::TextUnformatted(logError->Cause.data(), logError->Cause.data() + logError->Cause.size());
                                    ImGui::PushStyleColor(ImGuiCol_Text, ManifoldTheme::Label);
                                    ImGui::TextUnformatted(logError->Help.data(), logError->Help.data() + logError->Help.size());
                                    ImGui::PopStyleColor();
                                    ImGui::Separator();
                                }
                                if (logError || cut)
                                    ImGui::TextColored(ManifoldTheme::Muted, "%s", message.c_str());
                                ImGui::TextDisabled("%s", logError ? "Click to open the log, right-click to copy the code" : "Click to open the log, right-click to copy");
                                ImGui::PopTextWrapPos();
                            });
                    }
                    if (ImGui::BeginPopupContextItem("##statuslog"))
                    {
                        if (logError && ImGui::MenuItem(("Copy " + logCode).c_str()))
                            ImGui::SetClipboardText(logCode.c_str());
                        if (ImGui::MenuItem("Copy the Message"))
                            ImGui::SetClipboardText(message.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem(ICON_FA_STREAM "  Open the Log"))
                            m_workspace.OpenLog();
                        ImGui::EndPopup();
                    }
                }
            }
        }
        ImGui::PopClipRect();
    }
}
