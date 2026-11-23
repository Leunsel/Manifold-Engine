#include "UI/Workspace.h"

#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Core/BuildInfo.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "Table/CheatTable.h"
#include "Target/Process.h"
#include "UI/Loading.h"
#include "UI/NopSled.h"
#include "UI/ProfilerWindow.h"
#include "UI/LayoutAuditWindow.h"
#include "UI/ScriptUi.h"
#include "UI/Shortcuts.h"
#include "UI/TemplateManager.h"
#include "UI/UiHost.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/ByteSearch.h"
#include "UI/CodeEditor.h"
#include "UI/CodeTools.h"
#include "UI/DebugWindows.h"
#include "UI/LogWindow.h"
#include "UI/LuaWindow.h"
#include "UI/MainWindow.h"
#include "UI/MemoryViewer.h"
#include "UI/MonoWindow.h"
#include "UI/UnrealWindow.h"
#include "UI/PatchScanWindow.h"
#include "UI/PointerScanWindow.h"
#include "UI/RecordEditor.h"
#include "UI/ScriptWindow.h"
#include "UI/SettingsWindow.h"
#include "UI/StaticPointersWindow.h"
#include "UI/StructureWindow.h"
#include "UI/TableFilesWindow.h"
#include "UI/ToolWindows.h"
#include "UI/WatchWindow.h"
#include "UI/WriteHistoryWindow.h"
#include "UI/CommentListWindow.h"
#include "UI/RecordDebugWindow.h"
#include "UI/FormEditor.h"
#include "UI/OpcodeWindow.h"
#include "UI/ModulesWindow.h"
#include "UI/FlowGraphWindow.h"

#include "imgui_internal.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

namespace Manifold::UI
{
    namespace
    {
        constexpr double kNoticeSeconds = 6.0;

        const std::vector<FileFilter> kTableFilters = { { L"Cheat tables (*.CT)", L"*.CT;*.ct" }, { L"All files", L"*.*" } };

        std::wstring FilterText(const std::vector<FileFilter>& filters)
        {
            std::wstring text;
            for (const FileFilter& filter : filters)
            {
                text += filter.Name;
                text.push_back(L'\0');
                text += filter.Pattern;
                text.push_back(L'\0');
            }
            text.push_back(L'\0');
            return text;
        }

        template <class T>
        void Prune(std::vector<std::unique_ptr<T>>& windows)
        {
            windows.erase(std::remove_if(windows.begin(), windows.end(), [](const std::unique_ptr<T>& window) { return window->Closed(); }), windows.end());
        }
    }

    std::string NextWindowTitle(const char* base, int number)
    {
        return number <= 1 ? std::string(base) + "###" + base + "1" : std::string(base) + " " + std::to_string(number) + "###" + base + std::to_string(number);
    }

    Workspace::Workspace(Engine& engine, Shell& shell) : m_engine(engine), m_shell(shell)
    {
        m_main = std::make_unique<MainWindow>(*this);
        m_scriptForms = std::make_unique<ScriptForms>(engine);
        m_ui = std::make_unique<UiHost>(engine, shell);
        InstallTextMeasurer();
    }

    Workspace::~Workspace() = default;

    bool Workspace::OpenNamed(const std::string& window)
    {
        if (window == "memory")
            OpenMemoryViewer(0);
        else if (window == "hex")
            OpenMemoryViewer(0, true);
        else if (window == "structure")
            OpenStructure(0);
        else if (window == "script")
            OpenScript(Asm::CheatTableTemplate());
        else if (window == "lua")
            OpenLua();
        else if (window == "mono")
            OpenMono();
        else if (window == "unreal")
            OpenUnreal();
        else if (window == "log")
            OpenLog();
        else if (window == "settings")
            OpenSettings();
        else if (window == "symbols")
            OpenSymbols();
        else if (window == "regions")
            OpenRegions();
        else if (window == "tablefiles")
            OpenTableFiles();
        else if (window == "patches")
            OpenPatchScan();
        else if (window == "dissect")
            OpenCodeDissect();
        else if (window == "asmsearch")
            OpenAsmSearch();
        else if (window == "bytes")
            OpenByteSearch();
        else if (window == "pointerscan")
            OpenPointerScan();
        else if (window == "staticpointers")
            OpenStaticPointers();
        else if (window == "shortcuts")
            OpenShortcuts();
        else if (window == "keyboard")
            OpenSettings(UI::SettingsWindow::KeyboardPage);
        else if (window == "appearance")
            OpenSettings(UI::SettingsWindow::ThemePage);
        else if (window == "processes")
            OpenProcessPicker();
        else if (window == "about")
            OpenAbout();
        else if (window == "profiler")
            OpenProfiler(UI::ProfilerWindow::OverviewTab);
        else if (window == "breakdown")
            OpenProfiler(UI::ProfilerWindow::BreakdownTab);
        else if (window == "threads")
            OpenProfiler(UI::ProfilerWindow::ThreadsTab);
        else if (window == "checks")
            OpenProfiler(UI::ProfilerWindow::ChecksTab);
        else if (window == "appinfo")
            OpenProfiler(UI::ProfilerWindow::InfoTab);
        else if (window == "nopsled")
            OpenNopSled();
        else if (window == "writehistory")
            OpenWriteHistory();
        else if (window == "comments")
            OpenCommentList();
        else if (window == "recorddebug")
            OpenRecordDebug();
        else if (window.rfind("recorddebug:", 0) == 0)
        {
            if (const auto id = ParseDecimal(window.substr(12)))
                OpenRecordDebug(static_cast<int>(*id));
            else
                OpenRecordDebugPage(window.substr(12));
        }
        else if (window == "flowgraph")
            OpenFlowGraph();
        else if (window == "opcodes")
            OpenOpcodeConverter();
        else if (window == "templates")
            OpenTemplateManager();
        else if (window == "forms")
            OpenFormEditor();
        else if (window == "modules")
            OpenModules();
        else if (window == "breakpoints")
            OpenBreakpoints();
        else if (window == "threadlist")
            OpenThreadList();
        else if (window == "layoutproblems")
            OpenLayoutProblems();
        else if (window.rfind("settings:", 0) == 0)
        {
            const auto page = ParseDecimal(window.substr(9));
            if (!page)
                return false;
            OpenSettings(static_cast<int>(*page));
        }
        else if (window.rfind("forms:", 0) == 0)
            OpenFormEditor(Utf8ToWide(window.substr(6)));
        else if (window.rfind("templates:", 0) == 0)
            OpenTemplateManager(window.substr(10));
        else
            return false;
        return true;
    }

    const std::vector<std::string>& Workspace::WindowNames()
    {
        static const std::vector<std::string> names = {
            "memory", "hex", "structure", "script", "lua", "mono", "unreal", "log", "settings", "symbols", "regions", "tablefiles", "patches",
            "dissect", "asmsearch", "bytes", "pointerscan", "staticpointers", "shortcuts", "keyboard", "appearance", "processes", "about",
            "profiler", "breakdown", "threads", "checks", "appinfo", "nopsled", "writehistory", "comments", "recorddebug", "flowgraph", "opcodes",
            "templates", "forms", "modules", "breakpoints", "threadlist", "layoutproblems", "settings:2", "settings:3", "settings:4"
        };
        return names;
    }

    void Workspace::Draw()
    {
        if (m_restyle)
        {
            m_restyle = false;
            m_shell.Restyle();
        }
        {
            PROFILE_ZONE("Requests and shortcuts");
            HandleRequests();
            HandleShortcuts();
        }
        {
            PROFILE_ZONE("Main window");
            m_main->Draw();
        }
        {
            PROFILE_ZONE("Tool windows");
            DrawWindows();
        }
        PROFILE_ZONE("Dialogs and notices");
        DrawConfirm();
        DrawQuitPrompt();
        DrawTableScriptPrompt();
        DrawAbout();
        DrawNotices();
    }

    void Workspace::DrawWindows()
    {
        const auto drawAll = [](const char* zone, auto& windows) {
            if (windows.empty())
                return;
            PROFILE_ZONE(zone);
            for (std::size_t i = 0; i < windows.size(); ++i)
                windows[i]->Draw();
        };
        const auto drawOne = [](const char* zone, auto& window) {
            if (!window || window->Closed())
                return;
            PROFILE_ZONE(zone);
            window->Draw();
        };
        drawAll("Memory viewers", m_memoryViewers);
        drawAll("Structure dissect", m_structures);
        drawAll("Script editors", m_scripts);
        drawAll("Watch windows", m_watches);
        drawAll("Record editors", m_editors);
        drawOne("Lua engine", m_lua);
        drawOne("Log", m_log);
        drawOne("Settings", m_settings);
        drawOne("Process list", m_picker);
        drawOne("Symbols", m_symbols);
        drawOne("Memory regions", m_regions);
        drawOne("Table files", m_tableFiles);
        drawOne("Patch scan", m_patchScan);
        drawOne("Code dissect", m_codeDissect);
        drawOne("Assembly search", m_asmSearch);
        drawOne("Byte search", m_byteSearch);
        drawOne("Static pointers", m_staticPointers);
        drawOne("Pointer scan", m_pointerScan);
        drawOne("Breakpoints", m_breakpoints);
        drawOne("Thread list", m_threadList);
        drawOne("Mono / .NET", m_mono);
        drawOne("Unreal Engine", m_unrealWindow);
        drawAll("Traces", m_traces);
        drawOne("Keyboard shortcuts", m_shortcuts);
        drawOne("Template manager", m_templateManager);
        drawOne("Profiler", m_profiler);
        drawOne("Layout problems", m_layoutProblems);
        drawOne("NOP Sled", m_nopSled);
        drawOne("Write history", m_writeHistory);
        drawOne("Comment list", m_commentList);
        drawOne("Address list debug", m_recordDebug);
        drawOne("Form editor", m_formEditor);
        drawOne("Modules", m_modules);
        drawOne("Flow graph", m_flowGraph);
        drawOne("Opcode converter", m_opcodes);
        {
            PROFILE_ZONE("Script forms");
            m_scriptForms->Draw();
        }
        {
            PROFILE_ZONE("Script windows");
            m_ui->Draw();
        }
        Prune(m_memoryViewers);
        m_engine.MemoryViewerVisible = std::any_of(m_memoryViewers.begin(), m_memoryViewers.end(), [](const auto& viewer) { return !viewer->Closed(); });
        Prune(m_structures);
        Prune(m_scripts);
        Prune(m_watches);
        Prune(m_editors);
        Prune(m_traces);
    }

    void Workspace::HandleRequests()
    {
        for (const UiRequest& request : m_engine.TakeRequests())
        {
            switch (request.Kind)
            {
            case UiRequestKind::MemoryViewer: OpenMemoryViewer(request.Address); break;
            case UiRequestKind::MemoryViewerSelect: ActiveMemoryViewer().Select(request.Address); break;
            case UiRequestKind::MemoryViewerHex: ActiveMemoryViewer().ShowHex(request.Address, request.Start, request.Stop); break;
            case UiRequestKind::MemoryViewerRaise: ActiveMemoryViewer().Raise(); break;
            case UiRequestKind::StructureDissect: OpenStructure(request.Address, request.Text); break;
            case UiRequestKind::AutoAssembler: OpenScript(request.Text); break;
            case UiRequestKind::LuaEngine: OpenLua(); break;
            case UiRequestKind::Mono: OpenMono(request.Text); break;
            case UiRequestKind::Quit: RequestQuit(); break;
            }
        }
        const std::uint64_t printed = m_engine.Lua().PrintSignal();
        if (printed != m_seenPrints)
        {
            m_seenPrints = printed;
            if (!m_engine.Options().ReleaseMode && (!m_lua || m_lua->Closed()))
                OpenLua();
        }
    }

    bool Workspace::CanRun(Command command)
    {
        switch (command)
        {
        case Command::OpenMemoryViewer:
        case Command::OpenStructureDissect:
        case Command::OpenAutoAssembler:
        case Command::OpenLuaEngine:
        case Command::OpenMono:
        case Command::OpenUnreal:
        case Command::OpenPointerScan:
        case Command::OpenStaticPointers:
        case Command::OpenFormEditor:
        case Command::OpenOpcodeConverter:
        case Command::OpenRecordDebug:
        case Command::OpenLog:
            return !m_engine.Options().ReleaseMode;
        default:
            return Describe(command).Scope == CommandScope::Global;
        }
    }

    void Workspace::Run(Command command)
    {
        if (!CanRun(command))
            return;
        switch (command)
        {
        case Command::OpenProcess: OpenProcessPicker(); break;
        case Command::OpenTable: OpenTableDialog(false); break;
        case Command::SaveTable: SaveTable(false); break;
        case Command::SaveTableAs: SaveTable(true); break;
        case Command::OpenSettings: OpenSettings(); break;
        case Command::OpenShortcuts: OpenShortcuts(); break;
        case Command::OpenMemoryViewer: OpenMemoryViewer(0); break;
        case Command::OpenStructureDissect: OpenStructure(0); break;
        case Command::OpenAutoAssembler: OpenScript(Asm::CheatTableTemplate()); break;
        case Command::OpenLuaEngine: OpenLua(); break;
        case Command::OpenMono: OpenMono(); break;
        case Command::OpenUnreal: OpenUnreal(); break;
        case Command::OpenPointerScan: OpenPointerScan(); break;
        case Command::OpenStaticPointers: OpenStaticPointers(); break;
        case Command::OpenFormEditor: OpenFormEditor(); break;
        case Command::OpenOpcodeConverter: OpenOpcodeConverter(); break;
        case Command::OpenRecordDebug: OpenRecordDebug(); break;
        case Command::OpenLog: OpenLog(); break;
        case Command::OpenProfiler: OpenProfiler(); break;
        case Command::OpenLayoutProblems: OpenLayoutProblems(); break;
        default: break;
        }
    }

    void Workspace::HandleShortcuts()
    {
        for (const CommandInfo& info : AllCommands())
        {
            if (info.Scope == CommandScope::Global && Pressed(info.Id))
                Run(info.Id);
        }
        SecretKeys();
    }

    void Workspace::SecretKeys()
    {
        static constexpr ImGuiKey kSequence[] = { ImGuiKey_UpArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
            ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_B, ImGuiKey_A };
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || io.KeyCtrl || io.KeyAlt)
            return;
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
        {
            const ImGuiKey pressed = static_cast<ImGuiKey>(key);
            if (pressed == ImGuiKey_LeftShift || pressed == ImGuiKey_RightShift || (pressed >= ImGuiKey_MouseLeft && pressed <= ImGuiKey_MouseWheelY) ||
                !ImGui::IsKeyPressed(pressed, false))
                continue;
            if (pressed == kSequence[m_secretStep])
                ++m_secretStep;
            else
                m_secretStep = pressed == kSequence[0] ? 1 : 0;
            if (m_secretStep == static_cast<int>(std::size(kSequence)))
            {
                m_secretStep = 0;
                OpenNopSled();
            }
        }
    }

    void Workspace::OpenTableDialog(bool merge)
    {
        if (const auto path = OpenFileDialog(merge ? std::vector<FileFilter>{ { L"Cheat tables (*.CT)", L"*.CT;*.ct" } } : kTableFilters, Paths::TablesFolder()))
            OpenTable(*path, merge);
    }

    void Workspace::RefreshLastProcess()
    {
        const std::string& last = m_engine.Options().LastProcess;
        const double now = ImGui::GetTime();
        if (last.empty() || m_lastProcessLookup || now - m_lastProcessCheckedAt <= 2.0)
            return;
        m_lastProcessCheckedAt = now;
        m_lastProcessLookup = true;
        m_engine.LookUpProcessLater(last, [this, last](std::optional<ProcessId> id)
            {
                m_lastProcessLookup = false;
                if (last == m_engine.Options().LastProcess)
                    m_lastProcessId = id;
            });
    }

    bool Workspace::CanOpenLastProcess()
    {
        if (m_engine.Options().LastProcess.empty())
            return false;
        RefreshLastProcess();
        return m_lastProcessId && (!m_engine.HasProcess() || m_engine.Process()->Id() != *m_lastProcessId);
    }

    void Workspace::OpenLastProcess()
    {
        const std::string& last = m_engine.Options().LastProcess;
        if (last.empty())
        {
            OpenProcessPicker();
            return;
        }
        const std::optional<ProcessId> id = Target::FindProcessId(last);
        m_lastProcessCheckedAt = -100.0;
        if (!id)
        {
            Report(Status::Fail(ErrorCode::ProcessNotFound, last));
            return;
        }
        Report(m_engine.OpenProcess(*id));
    }

    void Workspace::OpenShortcuts()
    {
        if (!m_shortcuts)
            m_shortcuts = std::make_unique<ShortcutsWindow>(*this);
        m_shortcuts->Show();
    }

    void Workspace::OpenTemplateManager(const std::string& id, std::optional<std::string> script)
    {
        if (!m_templateManager)
            m_templateManager = std::make_unique<TemplateManager>(*this);
        m_templateManager->Show();
        if (script)
            m_templateManager->NewTemplate(*script);
        else if (!id.empty())
            m_templateManager->Select(id);
    }

    MemoryViewer& Workspace::ActiveMemoryViewer()
    {
        for (const auto& viewer : m_memoryViewers)
        {
            if (viewer->Number() == ActiveViewer && !viewer->Closed())
                return *viewer;
        }
        if (m_memoryViewers.empty())
            OpenMemoryViewer(0);
        return *m_memoryViewers.back();
    }

    void Workspace::OpenMemoryViewer(std::uint64_t address, bool hexView, bool newWindow)
    {
        if (m_memoryViewers.empty() || newWindow)
        {
            int number = 1;
            while (std::any_of(m_memoryViewers.begin(), m_memoryViewers.end(), [&](const auto& viewer) { return viewer->Number() == number; }))
                ++number;
            m_memoryViewers.push_back(std::make_unique<MemoryViewer>(*this, number));
        }
        MemoryViewer& viewer = *m_memoryViewers.back();
        if (!m_engine.HasProcess())
        {
            viewer.Show(0, hexView);
            return;
        }
        if (address == 0)
        {
            if (const auto main = m_engine.Process()->MainModule())
                address = main->Base + 0x1000;
        }
        viewer.Show(address, hexView);
    }

    void Workspace::OpenStructure(std::uint64_t address, const std::string& structure, bool newWindow)
    {
        StructureWindow* target = nullptr;
        for (auto it = m_structures.rbegin(); it != m_structures.rend() && !newWindow; ++it)
        {
            if (!(*it)->Closed())
            {
                target = it->get();
                break;
            }
        }
        if (!target && !newWindow && !m_structures.empty())
            target = m_structures.back().get();
        if (!target)
        {
            m_structures.push_back(std::make_unique<StructureWindow>(*this, m_nextWindow++));
            target = m_structures.back().get();
        }
        target->Show(address, structure);
    }

    std::uint32_t Workspace::DockGroup(const std::vector<std::string>& others, std::uint32_t& dock, int& frame)
    {
        if (frame == ImGui::GetFrameCount() && ImGui::DockBuilderGetNode(dock))
            return dock;
        for (const std::string& name : others)
        {
            ImGuiWindow* window = ImGui::FindWindowByName(name.c_str());
            if (!window || !(window->Active || window->WasActive))
                continue;
            if (window->DockId != 0)
            {
                const ImGuiDockNode* node = ImGui::DockBuilderGetNode(window->DockId);
                if (node && !node->IsCentralNode())
                    return window->DockId;
                continue;
            }
            const ImGuiID node = ImGui::DockBuilderAddNode(0, ImGuiDockNodeFlags_None);
            ImGui::DockBuilderSetNodePos(node, window->Pos);
            ImGui::DockBuilderSetNodeSize(node, window->Size);
            ImGui::DockBuilderDockWindow(window->Name, node);
            ImGui::DockBuilderFinish(node);
            dock = node;
            frame = ImGui::GetFrameCount();
            return node;
        }
        return 0;
    }

    std::uint32_t Workspace::StructureDock(const StructureWindow& opening)
    {
        std::vector<std::string> others;
        for (auto it = m_structures.rbegin(); it != m_structures.rend(); ++it)
        {
            const StructureWindow& other = **it;
            if (&other != &opening && !other.Closed() && other.Placed())
                others.push_back(other.WindowId());
        }
        return DockGroup(others, m_structureDock, m_structureDockFrame);
    }

    void Workspace::OpenScript(std::string script, int recordId, bool lua)
    {
        if (recordId != kNoRecord)
        {
            for (auto& window : m_scripts)
            {
                if (window->RecordId() == recordId)
                {
                    window->Focus();
                    return;
                }
            }
        }
        m_scripts.push_back(std::make_unique<ScriptWindow>(*this, m_nextWindow++, std::move(script), recordId, lua));
    }

    ScriptWindow* Workspace::ScriptFile(const std::filesystem::path& path)
    {
        std::error_code error;
        for (auto& window : m_scripts)
        {
            if (window->Closed() || window->Path().empty())
                continue;
            if (window->Path() == path || std::filesystem::equivalent(window->Path(), path, error))
                return window.get();
        }
        return nullptr;
    }

    ScriptWindow* Workspace::OpenScriptFile(const std::filesystem::path& path, int line)
    {
        ScriptWindow* window = ScriptFile(path);
        if (!window)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return nullptr;
            std::stringstream content;
            content << file.rdbuf();
            m_scripts.push_back(std::make_unique<ScriptWindow>(*this, m_nextWindow++, content.str(), kNoRecord, true));
            window = m_scripts.back().get();
            window->SetFile(path);
        }
        window->Focus();
        window->GotoLine(line);
        return window;
    }

    std::uint32_t Workspace::ScriptEditorDock(const ScriptWindow& opening)
    {
        std::vector<std::string> others;
        for (auto it = m_scripts.rbegin(); it != m_scripts.rend(); ++it)
        {
            const ScriptWindow& other = **it;
            if (&other != &opening && !other.Closed() && other.Placed())
                others.push_back(other.WindowId());
        }
        return DockGroup(others, m_scriptDock, m_scriptDockFrame);
    }

    void Workspace::OpenLua()
    {
        if (!m_lua)
            m_lua = std::make_unique<LuaWindow>(*this);
        m_lua->Show();
    }

    void Workspace::OpenLog()
    {
        if (!m_log)
            m_log = std::make_unique<LogWindow>(*this);
        m_log->Show();
    }

    void Workspace::OpenSettings(int page)
    {
        if (!m_settings)
            m_settings = std::make_unique<SettingsWindow>(*this);
        m_settings->Show(page);
    }

    void Workspace::OpenProcessPicker()
    {
        if (!m_picker)
            m_picker = std::make_unique<ProcessPicker>(*this);
        m_picker->Show();
    }

    void Workspace::NoProcessState(const char* icon, std::string_view text)
    {
        const std::string& last = m_engine.Options().LastProcess;
        RefreshLastProcess();

        const bool release = m_engine.Options().ReleaseMode;
        std::vector<EmptyAction> actions;
        actions.push_back({ ICON_FA_DESKTOP "  Open Process", ShortcutText(Command::OpenProcess), true });
        std::string footer;
        if (!last.empty() && m_lastProcessId)
            actions.push_back({ ICON_FA_HISTORY "  Open " + last, nullptr, false, true, "The process you opened last time is running" });
        else if (!last.empty())
            footer = "Last opened: " + last + " (not running)";
        const int clicked = EmptyState(icon, release ? "No game opened yet" : "No process opened", text, actions, true, footer);
        if (clicked == 0)
            OpenProcessPicker();
        else if (clicked == 1)
        {
            Report(m_engine.OpenProcess(*m_lastProcessId));
            m_lastProcessCheckedAt = -100.0;
        }
    }

    void Workspace::OpenSymbols()
    {
        if (!m_symbols)
            m_symbols = std::make_unique<SymbolsWindow>(*this);
        m_symbols->Show();
    }

    void Workspace::OpenTableFiles()
    {
        if (!m_tableFiles)
            m_tableFiles = std::make_unique<TableFilesWindow>(*this);
        m_tableFiles->Show();
    }

    void Workspace::OpenCodeDissect(int tab, std::uint64_t address)
    {
        if (!m_codeDissect)
            m_codeDissect = std::make_unique<CodeDissectWindow>(*this);
        m_codeDissect->Show(tab, address);
    }

    void Workspace::OpenAsmSearch()
    {
        if (!m_asmSearch)
            m_asmSearch = std::make_unique<AsmSearchWindow>(*this);
        m_asmSearch->Show();
    }

    void Workspace::OpenByteSearch(const std::string& pattern)
    {
        if (!m_byteSearch)
            m_byteSearch = std::make_unique<ByteSearchWindow>(*this);
        if (pattern.empty())
            m_byteSearch->Show();
        else
            m_byteSearch->Search(pattern);
    }

    void Workspace::FindStaticPointers(std::uint64_t address)
    {
        if (!m_staticPointers)
            m_staticPointers = std::make_unique<StaticPointersWindow>(*this);
        m_staticPointers->Search(address);
    }

    void Workspace::OpenStaticPointers()
    {
        if (!m_staticPointers)
            m_staticPointers = std::make_unique<StaticPointersWindow>(*this);
        m_staticPointers->Show();
    }

    void Workspace::OpenPointerScan(std::uint64_t address)
    {
        if (!m_pointerScan)
            m_pointerScan = std::make_unique<PointerScanWindow>(*this);
        m_pointerScan->Prepare(address);
    }

    void Workspace::RevealInMemoryViewer(std::uint64_t address, std::uint64_t length, bool code, bool raise)
    {
        MemoryViewer& viewer = ActiveMemoryViewer();
        if (code)
            viewer.Select(address);
        viewer.ShowHex(address, address, address + std::max<std::uint64_t>(length, 1) - 1);
        if (raise)
            viewer.Raise();
    }

    void Workspace::OpenBreakpoints()
    {
        if (!m_breakpoints)
            m_breakpoints = std::make_unique<BreakpointWindow>(*this);
        m_breakpoints->Show();
    }

    void Workspace::OpenBreakpointEditor(std::uint64_t address, Debug::BreakpointType type, int size, unsigned int viewport)
    {
        OpenBreakpoints();
        m_breakpoints->Edit(address, type, size, viewport);
    }

    void Workspace::OpenThreadList()
    {
        if (!m_threadList)
            m_threadList = std::make_unique<ThreadListWindow>(*this);
        m_threadList->Show();
    }

    void Workspace::OpenMono(const std::string& className)
    {
        if (!m_mono)
            m_mono = std::make_unique<MonoWindow>(*this);
        m_mono->Show();
        if (!className.empty())
            m_mono->ShowClass(className);
    }

    void Workspace::OpenUnreal(const std::string& type, std::uint64_t address)
    {
        if (!m_unrealWindow)
            m_unrealWindow = std::make_unique<UnrealWindow>(*this);
        m_unrealWindow->Show();
        if (!type.empty())
            m_unrealWindow->ShowType(type);
        if (address)
            m_unrealWindow->ShowAddress(address);
    }

    void Workspace::OpenProfiler(int tab)
    {
        if (!m_profiler)
            m_profiler = std::make_unique<ProfilerWindow>(*this);
        m_profiler->Show(tab);
    }

    void Workspace::OpenLayoutProblems()
    {
        if (!m_layoutProblems)
            m_layoutProblems = std::make_unique<LayoutAuditWindow>(*this);
        m_layoutProblems->Show();
    }

    void Workspace::OpenNopSled()
    {
        if (!m_nopSled)
            m_nopSled = std::make_unique<NopSledWindow>(*this);
        m_nopSled->Show();
    }

    void Workspace::OpenModules()
    {
        if (!m_modules)
            m_modules = std::make_unique<ModulesWindow>(*this);
        m_modules->Show();
    }

    void Workspace::OpenWriteHistory()
    {
        if (!m_writeHistory)
            m_writeHistory = std::make_unique<WriteHistoryWindow>(*this);
        m_writeHistory->Show();
    }

    void Workspace::OpenFormEditor(const std::filesystem::path& path)
    {
        if (!m_formEditor)
            m_formEditor = std::make_unique<FormEditorWindow>(*this);
        m_formEditor->Show();
        if (!path.empty())
            m_formEditor->OpenFile(path);
    }

    void Workspace::OpenCommentList()
    {
        if (!m_commentList)
            m_commentList = std::make_unique<CommentListWindow>(*this);
        m_commentList->Show();
    }

    void Workspace::OpenRecordDebug(int recordId)
    {
        if (!m_recordDebug)
            m_recordDebug = std::make_unique<RecordDebugWindow>(*this);
        m_recordDebug->Focus(recordId);
    }

    bool Workspace::OpenRecordDebugPage(std::string_view page)
    {
        if (!m_recordDebug)
            m_recordDebug = std::make_unique<RecordDebugWindow>(*this);
        return m_recordDebug->ShowPage(page);
    }

    void Workspace::RevealRecord(int recordId)
    {
        if (m_main)
            m_main->RevealRecord(recordId);
    }

    void Workspace::OpenOpcodeConverter(std::uint64_t address, std::span<const std::uint8_t> bytes)
    {
        if (!m_opcodes)
            m_opcodes = std::make_unique<OpcodeWindow>(*this);
        if (address || !bytes.empty())
            m_opcodes->Load(address, bytes);
        else
            m_opcodes->Show();
    }

    void Workspace::OpenFlowGraph(std::uint64_t address)
    {
        if (!m_flowGraph)
            m_flowGraph = std::make_unique<FlowGraphWindow>(*this);
        if (address)
            m_flowGraph->ShowFunction(address);
        else
            m_flowGraph->Show();
    }

    bool Workspace::Animating() const
    {
        return (m_nopSled && m_nopSled->Animating()) || AnimationShown();
    }

    bool Workspace::OnlySpinnersAnimating() const
    {
        return AnimationShown() && !(m_nopSled && m_nopSled->Animating());
    }

    void Workspace::OpenTrace(std::uint64_t address, unsigned int viewport)
    {
        auto window = std::make_unique<TraceWindow>(*this, m_nextTrace++);
        window->Start(address, viewport);
        m_traces.push_back(std::move(window));
    }

    void Workspace::OpenPatchScan()
    {
        if (!m_patchScan)
            m_patchScan = std::make_unique<PatchScanWindow>(*this);
        m_patchScan->Show();
    }

    void Workspace::OpenRegions()
    {
        if (!m_regions)
            m_regions = std::make_unique<RegionsWindow>(*this);
        m_regions->Show();
    }

    void Workspace::OpenAbout()
    {
        m_aboutOpen = true;
    }

    void Workspace::OpenRecordEditor(int recordId, RecordField field)
    {
        for (auto& editor : m_editors)
        {
            if (editor->RecordId() == recordId)
            {
                editor->Focus(field);
                return;
            }
        }
        if (m_engine.Table().FindById(recordId))
            m_editors.push_back(std::make_unique<RecordEditor>(*this, recordId, field));
    }

    void Workspace::StartWatch(Debug::WatchKind kind, std::uint64_t address, int size)
    {
        Status status = m_engine.EnsureDebugger();
        int id = 0;
        if (status)
            status = m_engine.Debugger().StartWatch(kind, address, size, id);
        if (!status)
        {
            Report(status);
            return;
        }
        m_watches.push_back(std::make_unique<WatchWindow>(*this, id, kind, address, size));
    }

    void Workspace::AddAddress(std::uint64_t address, Scan::ValueType type, std::string description)
    {
        const std::string text = m_engine.Symbols().ModuleOffset(address);
        Table::MemoryRecord& record = m_engine.Table().AddValue(description.empty() ? "No description" : std::move(description), text, type);
        if (type == Scan::ValueType::String || type == Scan::ValueType::ByteArray)
            record.Length = std::max(1, m_engine.Scanner().Format().Length);
        record.Unicode = m_engine.Scanner().Format().Unicode && type == Scan::ValueType::String;
        m_engine.RefreshValues(true);
    }

    void Workspace::Confirm(std::string title, std::string text, std::function<void()> yes, std::string yesLabel)
    {
        m_confirms.push_back({ std::move(title), std::move(text), std::move(yesLabel), std::move(yes), CallerViewport() });
    }

    void Workspace::Report(const Status& status)
    {
        if (!status.Ok())
            m_engine.Notify(LogLevel::Error, status.Text);
    }

    std::optional<std::filesystem::path> Workspace::OpenFileDialog(const std::vector<FileFilter>& filters, const std::filesystem::path& folder)
    {
        wchar_t file[MAX_PATH * 4] = L"";
        const std::wstring filter = FilterText(filters);
        const std::wstring start = folder.wstring();
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = static_cast<HWND>(m_shell.NativeWindow());
        dialog.lpstrFilter = filter.c_str();
        dialog.lpstrFile = file;
        dialog.nMaxFile = static_cast<DWORD>(std::size(file));
        dialog.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!::GetOpenFileNameW(&dialog))
            return std::nullopt;
        return std::filesystem::path(file);
    }

    std::vector<std::filesystem::path> Workspace::OpenFilesDialog(const std::vector<FileFilter>& filters, const std::filesystem::path& folder)
    {
        std::vector<wchar_t> buffer(1 << 17, L'\0');
        const std::wstring filter = FilterText(filters);
        const std::wstring start = folder.wstring();
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = static_cast<HWND>(m_shell.NativeWindow());
        dialog.lpstrFilter = filter.c_str();
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
        if (!::GetOpenFileNameW(&dialog))
            return {};
        const wchar_t* at = buffer.data();
        const std::wstring first = at;
        at += first.size() + 1;
        if (*at == L'\0')
            return { std::filesystem::path(first) };
        std::vector<std::filesystem::path> files;
        while (*at != L'\0')
        {
            const std::wstring name = at;
            files.push_back(std::filesystem::path(first) / name);
            at += name.size() + 1;
        }
        return files;
    }

    std::optional<std::filesystem::path> Workspace::FolderDialog()
    {
        const HRESULT started = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::optional<std::filesystem::path> chosen;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(::CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            if (SUCCEEDED(dialog->Show(static_cast<HWND>(m_shell.NativeWindow()))))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        chosen = std::filesystem::path(path);
                        ::CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        if (SUCCEEDED(started))
            ::CoUninitialize();
        return chosen;
    }

    std::optional<std::filesystem::path> Workspace::SaveFileDialog(const std::vector<FileFilter>& filters, const std::string& name, const wchar_t* extension)
    {
        wchar_t file[MAX_PATH * 4] = L"";
        ::wcsncpy_s(file, Utf8ToWide(name).c_str(), _TRUNCATE);
        const std::wstring filter = FilterText(filters);
        OPENFILENAMEW dialog{ sizeof(dialog) };
        dialog.hwndOwner = static_cast<HWND>(m_shell.NativeWindow());
        dialog.lpstrFilter = filter.c_str();
        dialog.lpstrFile = file;
        dialog.nMaxFile = static_cast<DWORD>(std::size(file));
        dialog.lpstrDefExt = extension;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!::GetSaveFileNameW(&dialog))
            return std::nullopt;
        return std::filesystem::path(file);
    }

    bool Workspace::OpenTable(const std::filesystem::path& path, bool merge)
    {
        const auto load = [this, path, merge]()
            {
                const Status status = m_engine.LoadTable(path, merge);
                if (!status)
                    Report(status);
                else
                    m_engine.Notify(LogLevel::Success, "Loaded " + WideToUtf8(path.filename().wstring()));
            };
        if (!merge && UnsavedTable())
        {
            Confirm("Open Table", "The current table has changes that are not saved. Open the other table anyway?", load, "Open");
            return true;
        }
        load();
        return true;
    }

    void Workspace::DropFiles(const std::vector<std::filesystem::path>& files, bool merge, void* window, float x, float y)
    {
        if (m_tableFiles && !m_tableFiles->Closed() && m_tableFiles->TakesDrop(window, x, y))
        {
            LOG_INFO("Table", std::to_string(files.size()) + (files.size() == 1 ? " file was" : " files were") + " dropped onto the table files window");
            m_tableFiles->AddFiles(files);
            return;
        }
        std::vector<std::filesystem::path> tables;
        for (const std::filesystem::path& file : files)
        {
            std::error_code missing;
            if (EqualsNoCase(WideToUtf8(file.extension().wstring()), ".ct") && std::filesystem::is_regular_file(file, missing))
                tables.push_back(file);
        }
        if (tables.empty())
        {
            const std::string what = files.size() == 1 ? WideToUtf8(files.front().filename().wstring()) + " is not" : "None of the dropped files is";
            m_engine.Notify(LogLevel::Warning, what + " a cheat table (.CT).");
            return;
        }
        if (tables.size() > 1)
            m_engine.Notify(LogLevel::Info, std::to_string(tables.size()) + " tables were dropped; " + WideToUtf8(tables.front().filename().wstring()) + " is opened. Drop one table at a time.");
        LOG_INFO("Table", std::string(merge ? "Merging" : "Opening") + " the dropped table " + WideToUtf8(tables.front().wstring()));
        OpenTable(tables.front(), merge);
    }

    bool Workspace::SaveTable(bool askName)
    {
        std::filesystem::path path = Utf8ToWide(m_engine.Table().FilePath);
        if (askName || path.empty())
        {
            const std::string& restored = m_engine.Backups().RestoredName();
            std::string name = !path.empty() ? WideToUtf8(path.filename().wstring()) : !restored.empty() ? restored + ".CT" : m_engine.HasProcess() ? m_engine.Process()->Name() : std::string("table");
            if (const std::size_t dot = name.rfind('.'); dot != std::string::npos)
                name.resize(dot);
            std::error_code ignored;
            std::filesystem::create_directories(Paths::TablesFolder(), ignored);
            const std::string start = path.empty() ? WideToUtf8((Paths::TablesFolder() / Utf8ToWide(name + ".CT")).wstring()) : WideToUtf8(path.wstring());
            const auto chosen = SaveFileDialog(kTableFilters, start, L"CT");
            if (!chosen)
                return false;
            path = *chosen;
        }
        // ∑ The engine logs and toasts the outcome itself, once: this notice was the second "Saved" entry per save in the Log.
        if (askName)
            return m_engine.SaveTable(path, true).Ok();
        return m_engine.RequestSave(path) != SaveKind::Failed;
    }

    void Workspace::OpenBackupDialog()
    {
        std::filesystem::path folder = m_engine.Backups().SessionFolder();
        std::error_code missing;
        if (!std::filesystem::is_directory(folder, missing))
        {
            folder = Backups::Folder(m_engine.Options());
            std::filesystem::create_directories(folder, missing);
        }
        if (const auto path = OpenFileDialog({ { L"Backups (*.CT)", L"*.CT;*.ct" } }, folder))
            OpenBackup(*path);
    }

    void Workspace::OpenBackup(const std::filesystem::path& path)
    {
        const auto load = [this, path]()
            {
                const Status status = m_engine.RestoreTable(path);
                if (!status)
                {
                    Report(status);
                    return;
                }
                const std::string when = Backups::When(path);
                m_engine.Notify(LogLevel::Success, "Opened the backup " + (when.empty() ? WideToUtf8(path.filename().wstring()) : "from " + when) + ". Save it under a name to keep it.");
            };
        if (UnsavedTable())
        {
            Confirm("Open Backup", "The current table has changes that are not saved. Open the backup anyway?", load, "Open");
            return;
        }
        load();
    }

    void Workspace::OfferBackup(const std::filesystem::path& path, const std::string& name)
    {
        LOG_WARN("Backup", "The last run did not close properly. Its last backup is " + WideToUtf8(path.wstring()));
        if (Table::TableHasContent(m_engine.Table(), m_engine.Structures(), m_engine.TableSymbols()))
        {
            m_engine.Notify(LogLevel::Warning, "The last run did not close properly. Its last backup of " + name + " from " + Backups::When(path) + " can be opened with File, Backups, Open a backup.");
            return;
        }
        Confirm("Open Backup", "The last run did not close properly and left changes that were not saved.\n\nOpen the backup of " + name + " from " + Backups::When(path) + "?",
            [this, path]() { OpenBackup(path); }, "Open");
    }

    void Workspace::BackUpNow()
    {
        bool written = false;
        const Status status = m_engine.Backups().BackUpNow(&written);
        if (!status)
            Report(status);
        else if (written)
            m_engine.Notify(LogLevel::Success, "Backed up to " + WideToUtf8(m_engine.Backups().LastFile().wstring()));
        else
            m_engine.Notify(LogLevel::Info, "Nothing to back up: there are no changes since the last save or backup.");
    }

    void Workspace::OpenBackupFolder()
    {
        const std::filesystem::path folder = Backups::Folder(m_engine.Options());
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    bool Workspace::UnsavedTable()
    {
        m_engine.FlushPendingSave();
        return m_engine.Table().Dirty() && Table::TableHasContent(m_engine.Table(), m_engine.Structures(), m_engine.TableSymbols());
    }

    void Workspace::NewTable()
    {
        const auto clear = [this]() { m_engine.NewTable(); };
        if (UnsavedTable())
            Confirm("New Table", "The current table has changes that are not saved. Throw it away?", clear, "Clear");
        else
            clear();
    }

    void Workspace::RequestQuit()
    {
        if (UnsavedTable())
        {
            Confirm("Quit", "The table has changes that are not saved. Quit anyway?", [this]() { QuitAfterTable(); }, "Quit");
            return;
        }
        QuitAfterTable();
    }

    void Workspace::QuitAfterTable()
    {
        if (!m_engine.ActiveScripts().empty())
        {
            switch (m_engine.Options().ActiveScriptsOnQuit)
            {
            case QuitPolicy::Disable:
                TurnOffAndQuit();
                return;
            case QuitPolicy::Ask:
                m_quitPrompt = true;
                m_quitRemember = false;
                return;
            case QuitPolicy::Leave:
                break;
            }
        }
        FinishQuit();
    }

    void Workspace::TurnOffEverything()
    {
        m_engine.RequestDeactivateAll();
    }

    // A process stopped at a breakpoint holds its locks (the loader lock, the
    // heap's, its own): a script that turns off with a remote call or a new
    // thread would wait on it forever and the quit would hang with the game
    // frozen. The debugger goes away on quit anyway, so it goes first.
    void Workspace::TurnOffAndQuit()
    {
        m_engine.Debugger().Detach();
        const std::vector<std::string> failures = m_engine.DeactivateAll();
        if (failures.empty())
        {
            FinishQuit();
            return;
        }
        std::string text = failures.size() == 1 ? "This script could not be turned off and may leave changes in the game:\n" : "These scripts could not be turned off and may leave changes in the game:\n";
        for (std::size_t i = 0; i < failures.size() && i < 6; ++i)
            text += "\n" + failures[i];
        if (failures.size() > 6)
            text += "\n... and " + std::to_string(failures.size() - 6) + " more";
        text += "\n\nQuit anyway?";
        Confirm("Quit", text, [this]() { FinishQuit(); }, "Quit");
    }

    void Workspace::FinishQuit()
    {
        m_engine.Debugger().Detach();
        m_quitConfirmed = true;
        m_shell.Close();
    }

    void Workspace::DrawQuitPrompt()
    {
        if (!m_quitPrompt)
            return;
        const char* id = "Active scripts##quit";
        if (!ImGui::IsPopupOpen(id))
            ImGui::OpenPopup(id);
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        if (BeginModal(id, ICON_FA_TOGGLE_ON, "Scripts are still active", ImVec2(460.0f, 0.0f)))
        {
            const std::vector<Table::MemoryRecord*> active = m_engine.ActiveScripts();
            const std::string process = m_engine.HasProcess() ? m_engine.Process()->Name() : std::string("the game");
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted((active.size() == 1 ? "1 script is still active in " + process + ". Turning it off first takes its changes out of the game before the program closes."
                : std::to_string(active.size()) + " scripts are still active in " + process + ". Turning them off first takes their changes out of the game before the program closes.").c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            for (std::size_t i = 0; i < active.size() && i < 6; ++i)
                Muted(ICON_FA_CODE "  " + active[i]->Description);
            if (active.size() > 6)
                Muted("... and " + std::to_string(active.size() - 6) + " more");
            ImGui::Spacing();
            ImGui::Checkbox("Remember this choice", &m_quitRemember);
            ItemTooltip("Can be changed later in Settings, under Scripts");
            ImGui::Spacing();
            int choice = 0;
            if (AccentButton(ICON_FA_TOGGLE_OFF "  Turn Off and Quit") || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                choice = 1;
            ImGui::SameLine();
            if (ImGui::Button("Quit and Leave Them"))
                choice = 2;
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                choice = 3;
            if (choice != 0)
            {
                ImGui::CloseCurrentPopup();
                m_quitPrompt = false;
                if (m_quitRemember && choice != 3)
                {
                    m_engine.Options().ActiveScriptsOnQuit = choice == 1 ? QuitPolicy::Disable : QuitPolicy::Leave;
                    Report(m_engine.SaveOptionsNow());
                }
            }
            EndModal();
            if (choice == 1)
                TurnOffAndQuit();
            else if (choice == 2)
                FinishQuit();
        }
    }

    void Workspace::CopyRecords(const std::vector<int>& ids)
    {
        std::vector<const Table::MemoryRecord*> records;
        for (int id : ids)
        {
            const Table::MemoryRecord* record = m_engine.Table().FindById(id);
            if (!record)
                continue;
            bool covered = false;
            for (int other : ids)
            {
                const Table::MemoryRecord* parent = m_engine.Table().FindById(other);
                covered = covered || (parent && parent != record && record->IsDescendantOf(parent));
            }
            if (!covered)
                records.push_back(record);
        }
        if (records.empty())
            return;
        const std::string xml = Table::RecordsToXml(records);
        ImGui::SetClipboardText(xml.c_str());
        m_engine.Notify(LogLevel::Info, "Copied " + std::to_string(records.size()) + (records.size() == 1 ? " record" : " records"));
    }

    void Workspace::PasteRecords(int parentId)
    {
        const char* clipboard = ImGui::GetClipboardText();
        if (!clipboard)
            return;
        auto records = Table::RecordsFromXml(clipboard);
        if (records.empty())
        {
            m_engine.Notify(LogLevel::Warning, "The clipboard holds no records.");
            return;
        }
        Table::MemoryRecord* parent = parentId >= 0 ? m_engine.Table().FindById(parentId) : nullptr;
        const std::function<void(Table::MemoryRecord&)> renumber = [&](Table::MemoryRecord& record)
            {
                record.Id = m_engine.Table().NextId();
                for (auto& child : record.Children)
                {
                    child->Parent = &record;
                    renumber(*child);
                }
            };
        for (auto& record : records)
        {
            renumber(*record);
            m_engine.Table().Add(std::move(record), parent);
        }
        m_engine.RefreshValues(true);
    }

    std::string Workspace::WithoutCodes(std::string_view text)
    {
        std::string plain;
        for (std::size_t at = 0; at < text.size();)
        {
            if (text.compare(at, 5, " (ME-") == 0)
            {
                const std::size_t close = text.find(')', at);
                if (close != std::string_view::npos && close - at <= 11)
                {
                    at = close + 1;
                    continue;
                }
            }
            plain += text[at++];
        }
        while (plain.size() > 1 && plain.back() == '.' && plain[plain.size() - 2] == '.')
            plain.pop_back();
        return plain;
    }

    void Workspace::DrawNotices()
    {
        const double now = m_engine.Now();
        const std::vector<Notice> notices = m_engine.Notifications().Visible(now, kNoticeSeconds);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float y = DrawWorkToasts(m_engine, viewport->Pos.y + viewport->Size.y - Px(40.0f));
        if (notices.empty())
            return;
        const float width = std::min(Px(440.0f), viewport->Size.x - Px(40.0f));
        for (auto it = notices.rbegin(); it != notices.rend(); ++it)
        {
            const Notice& notice = *it;
            const std::string id = "##notice" + std::to_string(notice.Id);
            const float age = static_cast<float>(now - notice.At);
            const float alpha = !notice.Sticky && age > kNoticeSeconds - 0.6 ? std::max(0.0f, static_cast<float>((kNoticeSeconds - age) / 0.6)) : 1.0f;
            ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x - Px(20.0f), y), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(Px(200.0f), 0.0f), ImVec2(width, FLT_MAX));
            ImGui::SetNextWindowBgAlpha(0.97f * alpha);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(6.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(12.0f, 9.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Card);
            ImGui::PushStyleColor(ImGuiCol_Border, ManifoldTheme::WithAlpha(LevelColor(notice.Level), 0.55f));
            ImGui::Begin(id.c_str(), nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
            ImGui::PushStyleColor(ImGuiCol_Text, LevelColor(notice.Level));
            ImGui::TextUnformatted(LevelIcon(notice.Level));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - Px(50.0f));
            const std::string text = (m_engine.Options().ReleaseMode ? WithoutCodes(notice.Text) : notice.Text) + (notice.Count > 1 ? " (" + std::to_string(notice.Count) + "x)" : std::string());
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
            if (notice.Progress >= 0.0)
            {
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, LevelColor(notice.Level));
                ImGui::ProgressBar(static_cast<float>(std::clamp(notice.Progress, 0.0, 1.0)), ImVec2(width - Px(30.0f), Px(4.0f)), "");
                ImGui::PopStyleColor();
            }
            ErrorTooltip(notice.Text);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                m_engine.Notifications().Expire(notice.Id, now - kNoticeSeconds);
            y -= ImGui::GetWindowHeight() + Px(8.0f);
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }
    }

    void Workspace::DrawConfirm()
    {
        if (m_confirms.empty())
            return;
        ConfirmRequest& request = m_confirms.front();
        const std::string id = "Confirm##workspace";
        if (!ImGui::IsPopupOpen(id.c_str()))
            ImGui::OpenPopup(id.c_str());
        if (BeginModal(id.c_str(), ICON_FA_QUESTION_CIRCLE, request.Title.c_str(), ImVec2(420.0f, 0.0f), nullptr, request.Viewport))
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(request.Text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            bool done = false;
            if (AccentButton(request.YesLabel.c_str(), Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
            {
                std::function<void()> yes = std::move(request.Yes);
                done = true;
                ImGui::CloseCurrentPopup();
                m_confirms.erase(m_confirms.begin());
                if (yes)
                    yes();
            }
            ImGui::SameLine();
            if (!done && (ImGui::Button("Cancel", Px(100.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            {
                ImGui::CloseCurrentPopup();
                m_confirms.erase(m_confirms.begin());
            }
            EndModal();
        }
    }

    // The script is shown read-only inside the prompt, as Cheat Engine does:
    // the prompt is modal, so a script window opened beside it could not be
    // reached until the question was answered.
    void Workspace::DrawTableScriptPrompt()
    {
        if (!m_engine.TableScriptWaiting())
        {
            m_tableScriptShown.clear();
            return;
        }
        const char* id = "Table script##workspace";
        if (!ImGui::IsPopupOpen(id))
        {
            ImGui::OpenPopup(id);
            m_tableScriptShown = m_engine.Table().TableLuaScript;
            m_tableScriptView = {};
            m_tableScriptView.ReadOnly = true;
            m_tableScriptNext = static_cast<int>(m_engine.Options().TableScripts);
        }
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        if (!BeginModal(id, ICON_FA_SCROLL, "Run this table's Lua script?", ImVec2(720.0f, 0.0f)))
            return;
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        Muted("Lua scripts can do anything this program can, including reading and writing files. Run it only if you trust the table.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        const int lines = static_cast<int>(std::count(m_tableScriptShown.begin(), m_tableScriptShown.end(), '\n')) + 1;
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        const float height = std::clamp(static_cast<float>(lines) * lineHeight + Px(16.0f), Px(120.0f), Px(340.0f));
        CodeEditor("##tablescriptview", m_tableScriptShown, Text::Language::Lua, ImVec2(-1.0f, height), m_tableScriptView);
        Muted(std::to_string(lines) + (lines == 1 ? " line" : " lines") + "  ·  read only");
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Next time a table has a Lua script:");
        ImGui::SameLine();
        const char* const policies[] = { "Ask", "Run it", "Do not run it" };
        ImGui::SetNextItemWidth(Px(150.0f));
        ImGui::Combo("##tablescriptnext", &m_tableScriptNext, policies, 3);
        ItemTooltip("Also in Settings > Scripts.");
        ImGui::Spacing();
        const auto answer = [&](bool run)
            {
                Settings& options = m_engine.Options();
                if (static_cast<int>(options.TableScripts) != m_tableScriptNext)
                {
                    options.TableScripts = static_cast<TableScriptPolicy>(std::clamp(m_tableScriptNext, 0, 2));
                    m_engine.SaveOptionsSoon();
                }
                ImGui::CloseCurrentPopup();
                m_tableScriptShown.clear();
                m_engine.AnswerTableScript(run);
            };
        if (AccentButton("Run the Script", Px(130.0f, 0.0f)))
            answer(true);
        ImGui::SameLine();
        if (ImGui::Button("Do Not Run It", Px(130.0f, 0.0f)))
            answer(false);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY "  Copy"))
            ImGui::SetClipboardText(m_tableScriptShown.c_str());
        EndModal();
    }

    void Workspace::DrawAbout()
    {
        if (!m_aboutOpen)
            return;
        const char* id = "About##workspace";
        if (!ImGui::IsPopupOpen(id))
            ImGui::OpenPopup(id);
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        if (BeginModal(id, ICON_FA_MICROCHIP, "About Manifold Engine", ImVec2(460.0f, 0.0f), &m_aboutOpen))
        {
            ImGui::Text("Version %s", VersionText().c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                const double now = ImGui::GetTime();
                m_versionClicks = now - m_versionClickedAt < 1.5 ? m_versionClicks + 1 : 1;
                m_versionClickedAt = now;
            }
            if (m_versionClicks >= 4 && ImGui::GetTime() - m_versionClickedAt < 1.5)
            {
                ImGui::SameLine(0.0f, Px(10.0f));
                Muted(std::string(static_cast<std::size_t>(m_versionClicks - 3), '.') + " " + std::to_string(7 - m_versionClicks));
            }
            if (m_versionClicks >= 7)
            {
                m_versionClicks = 0;
                m_aboutOpen = false;
                ImGui::CloseCurrentPopup();
                OpenNopSled();
            }
            Muted("Memory scanner, debugger, assembler and Lua scripting for Windows processes.");
            ImGui::Spacing();
            ImGui::TextUnformatted("Built with Dear ImGui (docking), Zydis 5 and Lua 5.4.");
            Muted("Licenses are in the Licenses folder next to the program.");
            ImGui::Spacing();
            ImGui::TextWrapped("Use only on software you own or are allowed to study, and keep a backup of your save. Debugging and memory edits can crash a "
                               "program or trip its anti-cheat; some games forbid it in their terms, and online use can get an account banned. The in-process "
                               "(VEH) debugger runs inside the target: use it deliberately. You are responsible for how you use this tool.");
            ImGui::Spacing();
            ImGui::Text("Administrator rights: %s", Target::IsElevated() ? "yes" : "no");
            if (!Target::IsElevated())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Restart as Administrator"))
                    m_shell.RestartElevated();
            }
            ImGui::Spacing();
            if (ImGui::Button("Close", Px(100.0f, 0.0f)))
            {
                m_aboutOpen = false;
                ImGui::CloseCurrentPopup();
            }
            EndModal();
        }
    }
}
