#include "UI/Workspace.h"

#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Generated/IconsFontAwesome5.h"
#include "Lua/LuaEngine.h"
#include "Table/CheatTable.h"
#include "Target/Process.h"
#include "UI/NopSled.h"
#include "UI/ProfilerWindow.h"
#include "UI/ScriptUi.h"
#include "UI/Shortcuts.h"
#include "UI/TemplateManager.h"
#include "UI/UiScale.h"
#include "UI/Widgets.h"
#include "UI/Windows.h"

#include "imgui_internal.h"

#include <algorithm>
#include <vector>

#include <windows.h>
#include <commdlg.h>
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
        InstallTextMeasurer();
    }

    Workspace::~Workspace() = default;

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
        drawOne("Breakpoints", m_breakpoints);
        drawOne("Thread list", m_threadList);
        drawOne("Mono / .NET", m_mono);
        drawAll("Traces", m_traces);
        drawOne("Keyboard shortcuts", m_shortcuts);
        drawOne("Template manager", m_templateManager);
        drawOne("Profiler", m_profiler);
        drawOne("NOP Sled", m_nopSled);
        {
            PROFILE_ZONE("Script forms");
            m_scriptForms->Draw();
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
            case UiRequestKind::Mono: OpenMono(); break;
            case UiRequestKind::Quit: RequestQuit(); break;
            }
        }
    }

    void Workspace::HandleShortcuts()
    {
        const bool release = m_engine.Options().ReleaseMode;
        if (Pressed(Command::OpenProcess))
            OpenProcessPicker();
        if (Pressed(Command::OpenTable))
            OpenTableDialog(false);
        if (Pressed(Command::SaveTable))
            SaveTable(false);
        if (Pressed(Command::SaveTableAs))
            SaveTable(true);
        if (Pressed(Command::OpenSettings))
            OpenSettings();
        if (Pressed(Command::OpenShortcuts))
            OpenShortcuts();
        if (Pressed(Command::OpenMemoryViewer) && !release)
            OpenMemoryViewer(0);
        if (Pressed(Command::OpenStructureDissect) && !release)
            OpenStructure(0);
        if (Pressed(Command::OpenAutoAssembler) && !release)
            OpenScript(Asm::CheatTableTemplate());
        if (Pressed(Command::OpenLuaEngine) && !release)
            OpenLua();
        if (Pressed(Command::OpenLog) && !release)
            OpenLog();
        if (Pressed(Command::OpenProfiler))
            OpenProfiler();
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

    bool Workspace::CanOpenLastProcess()
    {
        const std::string& last = m_engine.Options().LastProcess;
        if (last.empty())
            return false;
        const double now = ImGui::GetTime();
        if (now - m_lastProcessCheckedAt > 2.0)
        {
            m_lastProcessCheckedAt = now;
            m_lastProcessId = Target::FindProcessId(last).value_or(0);
        }
        return m_lastProcessId != 0 && (!m_engine.HasProcess() || m_engine.Process()->Id() != m_lastProcessId);
    }

    void Workspace::OpenLastProcess()
    {
        const std::string& last = m_engine.Options().LastProcess;
        if (last.empty())
        {
            OpenProcessPicker();
            return;
        }
        const std::uint32_t id = Target::FindProcessId(last).value_or(0);
        m_lastProcessCheckedAt = -100.0;
        if (!id)
        {
            Report(Status::Fail(ErrorCode::ProcessNotFound, last));
            return;
        }
        Report(m_engine.OpenProcess(id));
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

    void Workspace::OpenStructure(std::uint64_t address, const std::string& structure)
    {
        if (m_structures.empty())
            m_structures.push_back(std::make_unique<StructureWindow>(*this, m_nextWindow++));
        m_structures.back()->Show(address, structure);
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
        const double now = ImGui::GetTime();
        if (!last.empty() && now - m_lastProcessCheckedAt > 2.0)
        {
            m_lastProcessCheckedAt = now;
            m_lastProcessId = Target::FindProcessId(last).value_or(0);
        }

        const bool release = m_engine.Options().ReleaseMode;
        std::vector<EmptyAction> actions;
        actions.push_back({ ICON_FA_DESKTOP "  Open process", ShortcutText(Command::OpenProcess), true });
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
            Report(m_engine.OpenProcess(m_lastProcessId));
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

    void Workspace::OpenMono()
    {
        if (!m_mono)
            m_mono = std::make_unique<MonoWindow>(*this);
        m_mono->Show();
    }

    void Workspace::OpenProfiler(int tab)
    {
        if (!m_profiler)
            m_profiler = std::make_unique<ProfilerWindow>(*this);
        m_profiler->Show(tab);
    }

    void Workspace::OpenNopSled()
    {
        if (!m_nopSled)
            m_nopSled = std::make_unique<NopSledWindow>(*this);
        m_nopSled->Show();
    }

    bool Workspace::Animating() const
    {
        return m_nopSled && m_nopSled->Animating();
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

    void Workspace::OpenRecordEditor(int recordId)
    {
        for (auto& editor : m_editors)
        {
            if (editor->RecordId() == recordId)
            {
                editor->Focus();
                return;
            }
        }
        if (m_engine.Table().FindById(recordId))
            m_editors.push_back(std::make_unique<RecordEditor>(*this, recordId));
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
        if (!merge && m_engine.Table().Dirty() && m_engine.Table().Count() > 0)
        {
            Confirm("Open table", "The current table has changes that are not saved. Open the other table anyway?", load, "Open");
            return true;
        }
        load();
        return true;
    }

    bool Workspace::SaveTable(bool askName)
    {
        std::filesystem::path path = Utf8ToWide(m_engine.Table().FilePath);
        if (askName || path.empty())
        {
            std::string name = path.empty() ? (m_engine.HasProcess() ? m_engine.Process()->Name() : std::string("table")) : WideToUtf8(path.filename().wstring());
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
        const Status status = m_engine.SaveTable(path);
        if (!status)
        {
            Report(status);
            return false;
        }
        m_engine.Notify(LogLevel::Success, "Saved " + WideToUtf8(path.filename().wstring()));
        return true;
    }

    void Workspace::NewTable()
    {
        const auto clear = [this]() { m_engine.NewTable(); };
        if (m_engine.Table().Dirty() && m_engine.Table().Count() > 0)
            Confirm("New table", "Throw away the records of the current table?", clear, "Clear");
        else
            clear();
    }

    void Workspace::RequestQuit()
    {
        if (m_engine.Table().Dirty() && m_engine.Table().Count() > 0)
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

    void Workspace::TurnOffAndQuit()
    {
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
            if (AccentButton(ICON_FA_TOGGLE_OFF "  Turn off and quit") || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
                choice = 1;
            ImGui::SameLine();
            if (ImGui::Button("Quit and leave them"))
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
                    Report(m_engine.Options().Save(Paths::SettingsFile()));
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
        auto& notices = m_engine.Notices();
        const double now = m_engine.Now();
        notices.erase(std::remove_if(notices.begin(), notices.end(), [&](const Notice& notice) { return now - notice.At > kNoticeSeconds; }), notices.end());
        if (notices.empty())
            return;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float y = viewport->Pos.y + viewport->Size.y - Px(40.0f);
        const float width = std::min(Px(440.0f), viewport->Size.x - Px(40.0f));
        for (auto it = notices.rbegin(); it != notices.rend(); ++it)
        {
            Notice& notice = *it;
            const std::string id = "##notice" + std::to_string(notice.Id);
            const float age = static_cast<float>(now - notice.At);
            const float alpha = age > kNoticeSeconds - 0.6 ? std::max(0.0f, static_cast<float>((kNoticeSeconds - age) / 0.6)) : 1.0f;
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
            ImGui::TextUnformatted(m_engine.Options().ReleaseMode ? WithoutCodes(notice.Text).c_str() : notice.Text.c_str());
            ImGui::PopTextWrapPos();
            ErrorTooltip(notice.Text);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                notice.At = now - kNoticeSeconds;
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

    void Workspace::DrawTableScriptPrompt()
    {
        if (!m_engine.TableScriptWaiting())
            return;
        const char* id = "Table script##workspace";
        if (!ImGui::IsPopupOpen(id))
            ImGui::OpenPopup(id);
        ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
        if (BeginModal(id, ICON_FA_SCROLL, "This table has a Lua script", ImVec2(460.0f, 0.0f)))
        {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted("Lua scripts can do anything this program can, including reading and writing files. Run it only if you trust the table.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (AccentButton("Run the script", Px(130.0f, 0.0f)))
            {
                ImGui::CloseCurrentPopup();
                m_engine.AnswerTableScript(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Do not run it", Px(130.0f, 0.0f)))
            {
                ImGui::CloseCurrentPopup();
                m_engine.AnswerTableScript(false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Show it"))
            {
                OpenScript(m_engine.Table().TableLuaScript, kTableLuaScript, true);
            }
            EndModal();
        }
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
            ImGui::Text("Version %s", MANIFOLD_VERSION_TEXT);
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
                if (ImGui::SmallButton("Restart as administrator"))
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
