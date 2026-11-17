#pragma once

#include "App/Engine.h"
#include "App/Settings.h"
#include "App/WindowHost.h"
#include "UI/DialogHost.h"
#include "UI/SplashScreen.h"
#include "UI/Workspace.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold
{
    struct StartOptions
    {
        std::filesystem::path Table;
        std::string Process;
        std::vector<std::string> Open;
        std::filesystem::path ProfileReport;
        int ProfileSeconds = 10;
    };

    class Application final : private WindowHost::Client, public UI::Shell
    {
    public:
        explicit Application(StartOptions options);
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        bool Initialise(HINSTANCE instance, int showCommand);
        int Run();
        const wchar_t* LastError() const { return m_host.LastError(); }

        void Minimise() override { m_host.Minimise(); }
        void ToggleMaximised() override { m_host.ToggleMaximised(); }
        bool Maximised() const override { return m_host.Maximised(); }
        void Close() override;
        void AddDragArea(float left, float top, float right, float bottom) override { m_host.AddDragArea(left, top, right, bottom); }
        float CornerRadius() const override { return m_host.CornerRadius(); }
        void* NativeWindow() const override { return m_host.Window(); }
        void RestartElevated() override;
        void Restyle() override { m_host.Restyle(); }

    private:
        void BeforeFrame() override;
        void DrawFrame() override;
        void FirstFrameReady() override;
        void Revealed() override;
        void SplashStatus(const std::string& text);
        void CloseRequested() override;
        void RememberPlacement();
        void FirstFrameWork();
        void TickWhileWaiting(unsigned milliseconds);
        void ProfileReportWork();

        StartOptions m_options;
        Settings m_settings;
        WindowHost m_host;
        std::unique_ptr<Engine> m_engine;
        std::unique_ptr<UI::Workspace> m_workspace;
        std::unique_ptr<UI::DialogHost> m_dialogs;
        std::unique_ptr<UI::SplashScreen> m_splash;
        unsigned long long m_splashSince = 0;
        bool m_firstFrameReady = false;
        bool m_running = false;
        bool m_started = false;
        int m_frames = 0;
        unsigned long long m_profileUntil = 0;
    };
}
