#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

namespace Manifold::UI
{
    class SplashScreen
    {
    public:
        static std::unique_ptr<SplashScreen> Show(HINSTANCE instance, HWND mainWindow, bool maximised);
        ~SplashScreen();

        SplashScreen(const SplashScreen&) = delete;
        SplashScreen& operator=(const SplashScreen&) = delete;

        void SetStatus(std::string text);
        void Close(bool immediately = false);
        bool Finished() const { return m_finished.load(); }

    private:
        SplashScreen() = default;
        void Run(HINSTANCE instance, RECT area, UINT dpi);

        std::thread m_thread;
        std::atomic<HWND> m_window{ nullptr };
        std::atomic<bool> m_finished{ false };
        std::atomic<int> m_close{ 0 };
        std::mutex m_lock;
        std::string m_status;
        bool m_statusChanged = false;
    };
}
