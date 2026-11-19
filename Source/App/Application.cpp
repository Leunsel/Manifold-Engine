#include "App/Application.h"

#include "App/Commands.h"
#include "App/CrashHandler.h"
#include "App/Diagnostics.h"
#include "App/Sounds.h"
#include "Core/BuildInfo.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Lua/LuaEngine.h"
#include "UI/LuaDialogs.h"
#include "UI/ProfilerWindow.h"
#include "UI/Theme.h"
#include "UI/SettingsWindow.h"

#include "imgui.h"

#include <algorithm>
#include <fstream>

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
        constexpr unsigned long long kSplashMilliseconds = 2000;

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

        unsigned long long MillisecondsSinceStart()
        {
            FILETIME created{};
            FILETIME ignored{};
            if (!::GetProcessTimes(::GetCurrentProcess(), &created, &ignored, &ignored, &ignored))
                return 0;
            FILETIME now{};
            ::GetSystemTimePreciseAsFileTime(&now);
            const auto ticks = [](const FILETIME& time) { return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
            return (ticks(now) - ticks(created)) / 10000ull;
        }

        constexpr const wchar_t* kPersonalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

        bool SystemUsesLightTheme()
        {
            DWORD value = 0;
            DWORD size = sizeof(value);
            if (::RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
                return false;
            return value != 0;
        }

        Settings LoadSettings()
        {
            Settings settings;
            settings.Load(Paths::SettingsFile());
            for (std::string& recent : settings.RecentTables)
                recent = Paths::CurrentTablePath(recent, Paths::ProgramFolder());
            ManifoldTheme::SetSystemLight(SystemUsesLightTheme());
            ManifoldTheme::Configure(settings.Theme);
            ApplyShortcutSettings(settings.Shortcuts);
            return settings;
        }
    }

    Application::Application(StartOptions options) : m_options(std::move(options)), m_settings(LoadSettings()), m_host(*this, HostSetup())
    {
        RuntimeLog::SetVerbose(m_settings.VerboseLog);
        WatchSystemTheme();
    }

    Application::~Application()
    {
        if (m_themeEvent)
            ::CloseHandle(m_themeEvent);
        if (m_personalizeKey)
            ::RegCloseKey(m_personalizeKey);
        m_splash.reset();
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
        const bool minimised = showCommand == SW_SHOWMINIMIZED || showCommand == SW_SHOWMINNOACTIVE || showCommand == SW_MINIMIZE;
        if (m_settings.ShowSplash && !minimised)
        {
            m_splash = UI::SplashScreen::Show(instance, m_host.Window(), m_settings.Maximised);
            m_splashSince = ::GetTickCount64();
        }
        SplashStatus("Preparing the graphics");
        if (!m_host.StartRendering())
        {
            m_splash.reset();
            return false;
        }

        SplashStatus("Starting Lua");
        m_engine = std::make_unique<Engine>(m_settings);
        m_engine->SetAsync(true);
        SplashStatus("Building the interface");
        m_workspace = std::make_unique<UI::Workspace>(*m_engine, *this);
        m_dialogs = std::make_unique<UI::DialogHost>(m_host.Window(), m_host.Device(), m_host.Context());
        UI::InstallLuaDialogs(*m_dialogs);
        m_dialogs->SetBeforeShow([this]() { m_host.Reveal(); });

        SplashStatus("Opening the window");
        if (!minimised)
            m_host.HideUntilFirstFrame();
        ::ShowWindow(m_host.Window(), m_settings.Maximised && !minimised ? SW_SHOWMAXIMIZED : showCommand);
        ::UpdateWindow(m_host.Window());
        m_host.NoteInput();
        m_running = true;
        LOG_INFO("Program", "Manifold Engine " + VersionText() + " started" + std::string(Target::IsElevated() ? " with administrator rights" : ""));
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
            SplashStatus("Running the autorun scripts");
            const Status status = m_engine->Lua().RunAutorun(Paths::AutorunFolder());
            if (!status)
                m_engine->Notify(status);
        }
        if (!m_options.Process.empty())
        {
            SplashStatus("Opening " + m_options.Process);
            const Status status = m_engine->OpenProcessByName(m_options.Process);
            if (!status)
                m_engine->Notify(status);
        }
        if (!m_options.Table.empty())
        {
            SplashStatus("Loading " + WideToUtf8(m_options.Table.filename().wstring()));
            m_workspace->OpenTable(m_options.Table, false);
        }
        SplashStatus("Ready");
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
            else if (window == "unreal")
                m_workspace->OpenUnreal();
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
            else if (window == "appearance")
                m_workspace->OpenSettings(UI::SettingsWindow::ThemePage);
            else if (window == "processes")
                m_workspace->OpenProcessPicker();
            else if (window == "about")
                m_workspace->OpenAbout();
            else if (window == "profiler")
                m_workspace->OpenProfiler(UI::ProfilerWindow::OverviewTab);
            else if (window == "breakdown")
                m_workspace->OpenProfiler(UI::ProfilerWindow::BreakdownTab);
            else if (window == "threads")
                m_workspace->OpenProfiler(UI::ProfilerWindow::ThreadsTab);
            else if (window == "checks")
                m_workspace->OpenProfiler(UI::ProfilerWindow::ChecksTab);
            else if (window == "appinfo")
                m_workspace->OpenProfiler(UI::ProfilerWindow::InfoTab);
            else if (window == "nopsled")
                m_workspace->OpenNopSled();
            else if (window == "writehistory")
                m_workspace->OpenWriteHistory();
            else if (window == "flowgraph")
                m_workspace->OpenFlowGraph();
        }
        if (!m_options.ProfileReport.empty())
        {
            Diagnostics::Sample(true);
            m_profileUntil = ::GetTickCount64() + static_cast<unsigned long long>(m_options.ProfileSeconds) * 1000ull;
            LOG_INFO("Program", "Measuring for " + std::to_string(m_options.ProfileSeconds) + " s, then writing the diagnostics report to " + WideToUtf8(m_options.ProfileReport.wstring()));
        }
    }

    void Application::ProfileReportWork()
    {
        if (!m_profileUntil || ::GetTickCount64() < m_profileUntil)
            return;
        m_profileUntil = 0;
        const std::string report = UI::DiagnosticsReport(*m_workspace);
        std::ofstream file(m_options.ProfileReport, std::ios::binary | std::ios::trunc);
        file.write(report.data(), static_cast<std::streamsize>(report.size()));
        file.close();
        if (!file)
        {
            m_engine->Notify(Status::Fail(ErrorCode::ReportNotSaved, WideToUtf8(m_options.ProfileReport.wstring())));
            return;
        }
        LOG_INFO("Program", "Wrote the diagnostics report to " + WideToUtf8(m_options.ProfileReport.wstring()));
        if (m_engine->ActiveScripts().empty())
            Close();
        else
            m_workspace->RequestQuit();
    }

    void Application::SplashStatus(const std::string& text)
    {
        if (m_splash)
            m_splash->SetStatus(text);
    }

    void Application::FirstFrameReady()
    {
        m_firstFrameReady = true;
        LOG_INFO("Program", Format("The first frame was ready %llu ms after the start", MillisecondsSinceStart()));
        if (!m_splash)
            m_host.RequestReveal();
    }

    void Application::Revealed()
    {
        if (m_splash)
            m_splash->Close();
        LOG_INFO("Program", Format("The main window appeared %llu ms after the start", MillisecondsSinceStart()));
    }

    void Application::Restyle()
    {
        ManifoldTheme::Configure(m_settings.Theme);
        m_host.Restyle();
    }

    void Application::WatchSystemTheme()
    {
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_NOTIFY | KEY_QUERY_VALUE, &m_personalizeKey) != ERROR_SUCCESS)
        {
            m_personalizeKey = nullptr;
            return;
        }
        m_themeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_themeEvent || ::RegNotifyChangeKeyValue(m_personalizeKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, m_themeEvent, TRUE) != ERROR_SUCCESS)
        {
            if (m_themeEvent)
                ::CloseHandle(m_themeEvent);
            m_themeEvent = nullptr;
        }
    }

    bool Application::HandleMessage(UINT msg, WPARAM, LPARAM lp, LRESULT&)
    {
        if (msg == WM_SETTINGCHANGE && lp && ::lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorScheme") == 0)
            m_systemThemeChanged = true;
        return false;
    }

    void Application::FollowSystemTheme()
    {
        if (m_themeEvent && ::WaitForSingleObject(m_themeEvent, 0) == WAIT_OBJECT_0)
        {
            m_systemThemeChanged = true;
            ::RegNotifyChangeKeyValue(m_personalizeKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, m_themeEvent, TRUE);
        }
        if (!m_systemThemeChanged)
            return;
        m_systemThemeChanged = false;
        const bool light = SystemUsesLightTheme();
        if (light == ManifoldTheme::SystemLight())
            return;
        ManifoldTheme::SetSystemLight(light);
        LOG_INFO("Program", std::string("Windows switched apps to ") + (light ? "light" : "dark") + " mode");
        if (m_settings.Theme.Mode == ThemeMode::System)
            Restyle();
    }

    void Application::BeforeFrame()
    {
        FollowSystemTheme();
        if (m_host.Cloaked() && m_firstFrameReady && (!m_splash || ::GetTickCount64() - m_splashSince >= kSplashMilliseconds))
            m_host.RequestReveal();
        if (m_splash && m_splash->Finished())
            m_splash.reset();
        Sounds::SetEnabled(m_settings.PlaySounds);
        Profiler::SetEnabled(m_settings.ProfilerEnabled || !m_options.ProfileReport.empty());
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
        ProfileReportWork();
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
        m_engine->SaveOptionsNow();
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
            {
                PROFILE_WAIT("Waiting for input");
                if (m_host.Wait(slice))
                    return;
            }
            if (::GetTickCount64() + kIdleTickMilliseconds / 2 < until)
            {
                PROFILE_ZONE("Engine tick while waiting");
                m_engine->Tick();
            }
        }
    }

    int Application::Run()
    {
        while (m_running)
        {
            {
                PROFILE_ZONE("Window messages");
                if (!m_host.PumpMessages())
                    m_running = false;
            }
            if (!m_running)
                break;
            if (m_host.Hidden())
            {
                {
                    PROFILE_ZONE("Engine tick while hidden");
                    m_engine->Tick();
                }
                TickWhileWaiting(kHiddenMilliseconds);
                continue;
            }
            const bool animating = m_workspace->Animating();
            const bool busy = m_engine->Scanner().Busy() || m_engine->Debugger().Broken() || !m_engine->Notifications().Empty() || m_engine->Working();
            if (!animating && !busy && m_host.InputAge() > kBusyMilliseconds)
                TickWhileWaiting(kIdleFrameMilliseconds);
            else if (!animating && busy && m_host.InputAge() > kBusyMilliseconds)
                TickWhileWaiting(33);
            m_host.SetRelaxedPacing(m_workspace->OnlySpinnersAnimating() && m_host.InputAge() > kBusyMilliseconds);
            m_host.ApplyPendingResize();
            m_host.RenderFrame();
        }
        Sounds::Stop();
        RememberPlacement();
        const Status saved = m_engine->SaveOptionsNow();
        if (!saved)
            LOG_ERROR("Program", saved.Text);
        if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename)
            ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        m_engine->Debugger().Detach();
        LOG_INFO("Program", "Closed");
        return 0;
    }
}
