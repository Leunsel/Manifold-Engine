#pragma once

#include "imgui.h"

#include <functional>
#include <string>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

namespace Manifold::UI
{
    class DialogHost
    {
    public:
        DialogHost(HWND owner, ID3D11Device* device, ID3D11DeviceContext* deviceContext);
        ~DialogHost();

        DialogHost(const DialogHost&) = delete;
        DialogHost& operator=(const DialogHost&) = delete;

        bool Available() const;
        void SetBeforeShow(std::function<void()> callback) { m_beforeShow = std::move(callback); }
        bool Run(const std::string& title, const char* icon, float width, const std::function<bool()>& body);

    private:
        bool Create();
        bool ResizeTarget(UINT width, UINT height);
        void ReleaseTarget();
        bool DrawFrame(const std::string& title, const char* icon, const std::function<bool()>& body, float& contentHeight);
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

        std::function<void()> m_beforeShow;
        HWND m_owner = nullptr;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_deviceContext = nullptr;
        DWORD m_thread = 0;
        ImFontAtlas* m_atlas = nullptr;

        HWND m_window = nullptr;
        ImGuiContext* m_context = nullptr;
        IDXGISwapChain1* m_swapChain = nullptr;
        ID3D11RenderTargetView* m_target = nullptr;
        UINT m_width = 0;
        UINT m_height = 0;
        float m_titleHeight = 0.0f;
        float m_closeWidth = 0.0f;
        bool m_running = false;
        bool m_closeRequested = false;
        bool m_failed = false;
    };
}
