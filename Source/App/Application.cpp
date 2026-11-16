#include "App/Application.h"

#include "App/Commands.h"
#include "App/CrashHandler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "UI/LuaDialogs.h"
#include "UI/Theme.h"
#include "UI/Windows.h"

#include "imgui.h"

#include <algorithm>

#include <windows.h>
#include <shellapi.h>

namespace Manifold
{
    namespace
    {
        constexpr const wchar_t* kWindowClass = L"ManifoldEngineWindow";
        constexpr const wchar_t* kWindowTitle = L"Manifold Engine";
        constexpr int kMinimumWidth = 900;
        constexpr int kMinimumHeight = 600;
        constexpr unsigned long long kBusyMilliseconds = 1200;
        constexpr unsigned kIdleFrameMilliseconds = 100;
        constexpr unsigned kHiddenMilliseconds = 50;
        constexpr unsigned kIdleTickMilliseconds = 15;

        WindowHost::Setup HostSetup()
        {
            WindowHost::Setup setup;
            setup.ClassName = kWindowClass;
            setup.Title = kWindowTitle;
            setup.MinimumWidth = kMinimumWidth;
            setup.MinimumHeight = kMinimumHeight;
            setup.LayoutFile = Paths::LayoutFile();
            return setup;
        }

        Settings LoadSettings()
        {
            Settings settings;
            settings.Load(Paths::SettingsFile());
            ManifoldTheme::Configure(settings.Theme.Colors, settings.Theme.Sizes);
            ApplyShortcutSettings(settings.Shortcuts);
            return settings;
        }
    }

    Application::Application(StartOptions options) : m_options(std::move(options)), m_settings(LoadSettings()), m_host(*this, HostSetup())
    {
        RuntimeLog::SetVerbose(m_settings.VerboseLog);
    }

    Application::~Application()
    {
        UI::RemoveLuaDialogs();
        m_dialogs.reset();
        m_workspace.reset();
        m_engine.reset();
    }

    bool Application::Initialise(HINSTANCE instance, int showCommand)
    {
        if (!m_host.Open(instance))
            return false;
        WindowPlacement placement;
        placement.X = m_settings.X;
        placement.Y = m_settings.Y;
        placement.Width = m_settings.Width;
        placement.Height = m_settings.Height;
        placement.Placed = m_settings.Placed;
        placement.Maximised = m_settings.Maximised;
        m_host.Place(placement);
        if (!m_host.StartRendering())
            return false;

        m_engine = std::make_unique<Engine>(m_settings);
        m_engine->SetAsync(true);
        m_workspace = std::make_unique<UI::Workspace>(*m_engine, *this);
        m_dialogs = std::make_unique<UI::DialogHost>(m_host.Window(), m_host.Device(), m_host.Context());
        UI::InstallLuaDialogs(*m_dialogs);

        const bool minimised = showCommand == SW_SHOWMINIMIZED || showCommand == SW_SHOWMINNOACTIVE || showCommand == SW_MINIMIZE;
        ::ShowWindow(m_host.Window(), m_settings.Maximised && !minimised ? SW_SHOWMAXIMIZED : showCommand);
        ::UpdateWindow(m_host.Window());
        m_host.NoteInput();
        m_running = true;
        LOG_INFO("Program", "Manifold Engine started" + std::string(Target::IsElevated() ? " with administrator rights" : ""));
        return true;
    }

    void Application::FirstFrameWork()
    {
        if (!Target::IsElevated())
            LOG_WARN("Program", ErrorMessage(ErrorCode::NotElevated));
        if (const auto crash = CrashHandler::TakeLastCrash(Paths::CrashFolder()))
            m_engine->Notify(LogLevel::Warning, ErrorMessage(ErrorCode::LastRunCrashed, WideToUtf8(crash->wstring())));
        if (m_settings.AutorunEnabled)
        {
            const Status status = m_engine->Lua().RunAutorun(Paths::AutorunFolder());
            if (!status)
                m_engine->Notify(status);
        }
        if (!m_options.Process.empty())
        {
            const Status status = m_engine->OpenProcessByName(m_options.Process);
            if (!status)
                m_engine->Notify(status);
        }
        if (!m_options.Table.empty())
            m_workspace->OpenTable(m_options.Table, false);
        for (const std::string& window : m_options.Open)
        {
            if (window == "memory")
                m_workspace->OpenMemoryViewer(0);
            else if (window == "hex")
                m_workspace->OpenMemoryViewer(0, true);
            else if (window == "structure")
                m_workspace->OpenStructure(0);
            else if (window == "script")
                m_workspace->OpenScript(Asm::CheatTableTemplate());
            else if (window == "lua")
                m_workspace->OpenLua();
            else if (window == "mono")
                m_workspace->OpenMono();
            else if (window == "log")
                m_workspace->OpenLog();
            else if (window == "settings")
                m_workspace->OpenSettings();
            else if (window == "symbols")
                m_workspace->OpenSymbols();
            else if (window == "regions")
                m_workspace->OpenRegions();
            else if (window == "tablefiles")
                m_workspace->OpenTableFiles();
            else if (window == "patches")
                m_workspace->OpenPatchScan();
            else if (window == "dissect")
                m_workspace->OpenCodeDissect();
            else if (window == "asmsearch")
                m_workspace->OpenAsmSearch();
            else if (window == "bytes")
                m_workspace->OpenByteSearch();
            else if (window == "shortcuts")
                m_workspace->OpenShortcuts();
            else if (window == "keyboard")
                m_workspace->OpenSettings(UI::SettingsWindow::KeyboardPage);
            else if (window == "processes")
                m_workspace->OpenProcessPicker();
            else if (window == "about")
                m_workspace->OpenAbout();
        }
    }

    void Application::BeforeFrame()
    {
        if (m_engine)
            m_engine->Tick();
    }

    void Application::DrawFrame()
    {
        if (!m_workspace)
            return;
        m_workspace->Draw();
        if (!m_started && ++m_frames >= 2)
        {
            m_started = true;
            FirstFrameWork();
        }
    }

    void Application::CloseRequested()
    {
        if (m_workspace && !m_workspace->QuitConfirmed())
        {
            m_workspace->RequestQuit();
            return;
        }
        m_running = false;
    }

    void Application::Close()
    {
        m_running = false;
    }

    void Application::RestartElevated()
    {
        std::wstring program(MAX_PATH, L'\0');
        const DWORD length = ::GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
        program.resize(length);
        std::wstring parameters;
        if (!m_engine->Table().FilePath.empty())
            parameters = L"\"" + Utf8ToWide(m_engine->Table().FilePath) + L"\"";
        if (m_engine->HasProcess())
            parameters += L" --process=\"" + Utf8ToWide(m_engine->Process()->Name()) + L"\"";
        RememberPlacement();
        m_settings.Save(Paths::SettingsFile());
        const auto result = reinterpret_cast<INT_PTR>(::ShellExecuteW(m_host.Window(), L"runas", program.c_str(), parameters.c_str(), nullptr, SW_SHOWNORMAL));
        if (result > 32)
            m_running = false;
        else
            m_engine->Notify(LogLevel::Warning, "The program was not restarted with administrator rights.");
    }

    void Application::RememberPlacement()
    {
        const std::optional<WindowPlacement> placement = m_host.Placement();
        if (!placement)
            return;
        m_settings.X = placement->X;
        m_settings.Y = placement->Y;
        m_settings.Width = placement->Width;
        m_settings.Height = placement->Height;
        m_settings.Maximised = placement->Maximised;
        m_settings.Placed = true;
    }

    void Application::TickWhileWaiting(unsigned milliseconds)
    {
        const ULONGLONG until = ::GetTickCount64() + milliseconds;
        for (;;)
        {
            const ULONGLONG now = ::GetTickCount64();
            if (now >= until)
                return;
            const unsigned slice = static_cast<unsigned>(std::min<ULONGLONG>(kIdleTickMilliseconds, until - now));
            if (m_host.Wait(slice))
                return;
            if (::GetTickCount64() + kIdleTickMilliseconds / 2 < until)
                m_engine->Tick();
        }
    }

    int Application::Run()
    {
        while (m_running)
        {
            if (!m_host.PumpMessages())
                m_running = false;
            if (!m_running)
                break;
            if (m_host.Hidden())
            {
                m_engine->Tick();
                TickWhileWaiting(kHiddenMilliseconds);
                continue;
            }
            const bool busy = m_engine->Scanner().Busy() || m_engine->Debugger().Broken() || !m_engine->Notices().empty() || m_engine->Working();
            if (!busy && m_host.InputAge() > kBusyMilliseconds)
                TickWhileWaiting(kIdleFrameMilliseconds);
            else if (busy && m_host.InputAge() > kBusyMilliseconds)
                TickWhileWaiting(33);
            m_host.ApplyPendingResize();
            m_host.RenderFrame();
        }
        RememberPlacement();
        const Status saved = m_settings.Save(Paths::SettingsFile());
        if (!saved)
            LOG_ERROR("Program", saved.Text);
        if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename)
            ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        m_engine->Debugger().Detach();
        LOG_INFO("Program", "Closed");
        return 0;
    }
}
