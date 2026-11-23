#include "App/Application.h"

#include "App/Commands.h"
#include "App/CrashHandler.h"
#include "App/Diagnostics.h"
#include "App/Sounds.h"
#include "Core/BuildInfo.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Layout/Layout.h"
#include "Lua/LuaEngine.h"
#include "UI/Fonts.h"
#include "UI/LuaDialogs.h"
#include "UI/ProfilerWindow.h"
#include "UI/Theme.h"
#include "UI/SettingsWindow.h"
#include "UI/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <utility>

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
        m_host.SetCorners(m_settings.Theme.Corners);
        WatchSystemTheme();
        Layout::Audit::SetReporter([](const Layout::Audit::Problem& problem) {
            LOG_WARN("Layout", problem.Place + ": " + Layout::Audit::KindName(problem.What) + ", " + problem.Detail);
        });
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
        if (!m_options.LayoutSweep.empty())
            m_host.SetQuiet();
        if (!m_host.Open(instance))
            return false;
        AcceptDroppedFiles();
        WindowPlacement placement;
        placement.X = m_settings.X;
        placement.Y = m_settings.Y;
        placement.Width = m_settings.Width;
        placement.Height = m_settings.Height;
        placement.Placed = m_settings.Placed;
        placement.Maximised = m_settings.Maximised;
        m_host.Place(placement);
        const bool minimised = showCommand == SW_SHOWMINIMIZED || showCommand == SW_SHOWMINNOACTIVE || showCommand == SW_MINIMIZE;
        if (m_settings.ShowSplash && !minimised && !m_host.Quiet())
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
        m_dialogs->SetBeforeShow([this]() {
            if (!m_host.Quiet())
                m_host.Reveal();
        });

        SplashStatus("Opening the window");
        if (!minimised || m_host.Quiet())
            m_host.HideUntilFirstFrame();
        if (m_host.Quiet())
        {
            // ∑ the sweep stays cloaked and must not take the focus from whatever the owner is doing
            ::ShowWindow(m_host.Window(), SW_SHOWNOACTIVATE);
            m_sweep = std::make_unique<UI::LayoutSweep>(m_options.LayoutSweep, m_options.SweepWindows, m_options.SweepScales);
        }
        else
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
            SplashStatus("Running the autorun modules");
            const Status status = m_engine->RunAutorun();
            if (!status)
                m_engine->Notify(status);
            int held = 0;
            std::string names;
            for (const Autorun::Module& module : m_engine->Lua().Modules())
            {
                if (module.State != Autorun::State::Held)
                    continue;
                names += (held++ ? ", " : "") + module.Name;
            }
            if (held > 0)
                m_engine->Notify(LogLevel::Warning, Format("%d autorun %s held until allowed in Extensions > Modules: ", held, held == 1 ? "module is" : "modules are") + names);
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
        const std::vector<Backups::Abandoned> abandoned = m_engine->Backups().TakeAbandoned();
        for (std::size_t i = 1; i < abandoned.size(); ++i)
            LOG_WARN("Backup", "An earlier run did not close properly either. Its last backup is " + WideToUtf8(abandoned[i].File.wstring()));
        if (!abandoned.empty())
            m_workspace->OfferBackup(abandoned.front().File, abandoned.front().Name);
        SplashStatus("Ready");
        for (const std::string& window : m_options.Open)
        {
            if (!m_workspace->OpenNamed(window))
                LOG_WARN("Program", "--open: no window is called " + window);
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
        if (!m_splash && !m_host.Quiet())
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
        m_host.SetCorners(m_settings.Theme.Corners);
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

    void Application::AcceptDroppedFiles()
    {
        constexpr UINT kCopyGlobalData = 0x0049;
        const HWND window = m_host.Window();
        ::DragAcceptFiles(window, TRUE);
        for (const UINT message : { static_cast<UINT>(WM_DROPFILES), static_cast<UINT>(WM_COPYDATA), kCopyGlobalData })
            ::ChangeWindowMessageFilterEx(window, message, MSGFLT_ALLOW, nullptr);
    }

    bool Application::HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
    {
        if (msg == WM_SETTINGCHANGE && lp && ::lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorScheme") == 0)
            m_systemThemeChanged = true;
        if (msg == WM_DROPFILES)
        {
            const auto drop = reinterpret_cast<HDROP>(wp);
            m_dropWindow = WindowHost::DropSource() ? WindowHost::DropSource() : m_host.Window();
            ::DragQueryPoint(drop, &m_dropPoint);
            ::ClientToScreen(m_dropWindow, &m_dropPoint);
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i)
            {
                const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                if (length && ::DragQueryFileW(drop, i, path.data(), length + 1) == length)
                {
                    path.resize(length);
                    m_dropped.emplace_back(std::move(path));
                }
            }
            ::DragFinish(drop);
            m_dropMerges = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            result = 0;
            return true;
        }
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
        if (!m_dropped.empty())
            m_workspace->DropFiles(std::exchange(m_dropped, {}), m_dropMerges, m_dropWindow, static_cast<float>(m_dropPoint.x), static_cast<float>(m_dropPoint.y));
        Layout::Defaults().Bold = Fonts::Bold();
        Layout::Defaults().Mono = Fonts::Mono();
        Layout::NewFrame();
        Layout::Audit::SetEnabled(m_settings.LayoutChecks || m_options.LayoutAudit || m_sweep);
        if (m_sweep && m_started)
            m_sweep->BeforeDraw(*m_workspace, *this);
        m_workspace->Draw();
        Layout::Audit::EndFrame();
        if (m_sweep && m_started)
            m_sweep->AfterDraw();
        UI::ApplyWindowOpacity(static_cast<float>(m_settings.ActiveWindowOpacity) / 100.0f, static_cast<float>(m_settings.InactiveWindowOpacity) / 100.0f);
        if (!m_started && ++m_frames >= 2)
        {
            m_started = true;
            FirstFrameWork();
        }
        ProfileReportWork();
        LayoutSweepWork();
    }

    void Application::SetSweepScale(float scale)
    {
        m_host.SetDpi(static_cast<UINT>(std::lround(96.0f * scale)));
    }

    void Application::SetSweepMainSize(int width, int height)
    {
        m_host.ResizeClient(width, height);
    }

    void Application::LayoutSweepWork()
    {
        if (!m_sweep || !m_sweep->Finished())
            return;
        std::string error;
        if (m_sweep->Write(&error))
            LOG_INFO("Program", Format("The layout sweep found %zu problems, report in ", m_sweep->ProblemCount()) + WideToUtf8(m_options.LayoutSweep.wstring()));
        else
            LOG_ERROR("Program", "Layout sweep: " + error);
        m_sweep.reset();
        Close();
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
            if (m_sweep)
            {
                // the sweep needs every frame
            }
            else if (!animating && !busy && m_host.InputAge() > kBusyMilliseconds)
                TickWhileWaiting(kIdleFrameMilliseconds);
            else if (!animating && busy && m_host.InputAge() > kBusyMilliseconds)
                TickWhileWaiting(33);
            m_host.SetRelaxedPacing(m_workspace->OnlySpinnersAnimating() && m_host.InputAge() > kBusyMilliseconds);
            m_host.ApplyPendingResize();
            m_host.RenderFrame();
        }
        Sounds::Stop();
        m_engine->FlushPendingSave();
        m_engine->Backups().Finish();
        if (!m_host.Quiet())
        {
            // the sweep's sizes and scales are not the owner's placement
            RememberPlacement();
            const Status saved = m_engine->SaveOptionsNow();
            if (!saved)
                LOG_ERROR("Program", saved.Text);
        }
        if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename)
            ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
        m_engine->Debugger().Detach();
        LOG_INFO("Program", "Closed");
        return 0;
    }
}
