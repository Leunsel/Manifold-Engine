#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

struct ImGuiContext;

namespace Manifold
{
    struct WindowPlacement
    {
        int X = 0;
        int Y = 0;
        int Width = 0;
        int Height = 0;
        bool Placed = false;
        bool Maximised = false;
    };

    class WindowHost final
    {
    public:
        class Client
        {
        public:
            virtual void BeforeFrame() {}
            virtual void DrawFrame() = 0;
            virtual bool HandleMessage(UINT, WPARAM, LPARAM, LRESULT&) { return false; }
            virtual void CloseRequested() = 0;

        protected:
            Client() = default;
            Client(const Client&) = default;
            Client& operator=(const Client&) = default;
            ~Client() = default;
        };

        struct Setup
        {
            const wchar_t* ClassName = nullptr;
            const wchar_t* Title = nullptr;
            int MinimumWidth = 0;
            int MinimumHeight = 0;
            std::filesystem::path LayoutFile;
        };

        WindowHost(Client& client, Setup setup);
        ~WindowHost();

        WindowHost(const WindowHost&) = delete;
        WindowHost& operator=(const WindowHost&) = delete;

        bool Open(HINSTANCE instance);
        void Place(const WindowPlacement& placement);
        std::optional<WindowPlacement> Placement() const;
        bool StartRendering();

        bool PumpMessages();
        bool Hidden();
        bool Wait(unsigned milliseconds) const;
        void NoteInput();
        unsigned long long InputAge() const;
        void ApplyPendingResize();
        void RenderFrame();

        HWND Window() const { return m_hwnd; }
        ID3D11Device* Device() const { return m_device; }
        ID3D11DeviceContext* Context() const { return m_context; }
        float WindowScale() const;
        const wchar_t* LastError() const { return m_error; }

        float CornerRadius() const;
        void Restyle();
        void AddDragArea(float left, float top, float right, float bottom);
        void Minimise() { ::ShowWindow(m_hwnd, SW_MINIMIZE); }
        bool Maximised() const { return ::IsZoomed(m_hwnd) != FALSE; }
        void ToggleMaximised() { ::ShowWindow(m_hwnd, Maximised() ? SW_RESTORE : SW_MAXIMIZE); }

    private:
        bool CreateDeviceAndSwapChain();
        void DestroyDeviceAndSwapChain();
        bool CreateRenderTarget();
        void DestroyRenderTarget();

        void ApplyWindowChrome();
        void RescaleForDpi(UINT dpi);
        SIZE MinimumSize() const;

        static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
        LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

        Client& m_client;
        Setup m_setup;
        std::string m_layoutFile;

        HWND m_hwnd = nullptr;
        HINSTANCE m_instance = nullptr;
        bool m_registered = false;
        bool m_closing = false;

        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        IDXGISwapChain1* m_swapChain = nullptr;
        ID3D11RenderTargetView* m_renderTarget = nullptr;

        UINT m_pendingWidth = 0;
        UINT m_pendingHeight = 0;
        bool m_occluded = false;
        bool m_inSizeMove = false;
        bool m_inFrame = false;
        UINT m_dpi = 96;
        bool m_roundedCorners = false;
        std::vector<RECT> m_dragAreas;
        ULONGLONG m_lastInputTick = 0;

        void* m_baseStyle = nullptr;
        ImGuiContext* m_imgui = nullptr;

        wchar_t m_error[256] = L"";
    };
}
