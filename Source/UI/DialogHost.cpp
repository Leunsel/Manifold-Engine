#include "UI/DialogHost.h"

#include "Generated/IconsFontAwesome5.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"
#include "UI/UiScale.h"

#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <dwmapi.h>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);

namespace Manifold::UI
{
    namespace
    {
        constexpr const wchar_t* kClassName = L"ManifoldDialog";
        constexpr DWORD kUseImmersiveDarkMode = 20;
        constexpr DWORD kWindowCornerPreference = 33;
        constexpr DWORD kCornerRound = 2;
        constexpr UINT kStartHeight = 160;

        struct ThreadWindows
        {
            HWND Skip = nullptr;
            std::vector<HWND> Windows;
        };

        BOOL CALLBACK CollectEnabled(HWND window, LPARAM parameter)
        {
            auto* list = reinterpret_cast<ThreadWindows*>(parameter);
            if (window != list->Skip && ::IsWindowVisible(window) && ::IsWindowEnabled(window))
                list->Windows.push_back(window);
            return TRUE;
        }

        class ContextScope
        {
        public:
            explicit ContextScope(ImGuiContext* context) : m_previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
            ~ContextScope() { ImGui::SetCurrentContext(m_previous); }
            ContextScope(const ContextScope&) = delete;
            ContextScope& operator=(const ContextScope&) = delete;

        private:
            ImGuiContext* m_previous = nullptr;
        };

        RECT WorkArea(const RECT& anchor)
        {
            MONITORINFO info{ sizeof(info) };
            if (::GetMonitorInfoW(::MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &info))
                return info.rcWork;
            return anchor;
        }
    }

    DialogHost::DialogHost(HWND owner, ID3D11Device* device, ID3D11DeviceContext* deviceContext)
        : m_owner(owner), m_device(device), m_deviceContext(deviceContext), m_thread(::GetCurrentThreadId())
    {
        if (ImGui::GetCurrentContext())
            m_atlas = ImGui::GetIO().Fonts;
    }

    DialogHost::~DialogHost()
    {
        if (m_context)
        {
            {
                ContextScope scope(m_context);
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
            }
            ImGui::DestroyContext(m_context);
        }
        ReleaseTarget();
        if (m_swapChain)
            m_swapChain->Release();
        if (m_window)
            ::DestroyWindow(m_window);
    }

    bool DialogHost::Available() const
    {
        return !m_running && !m_failed && m_device && m_deviceContext && m_atlas && ::GetCurrentThreadId() == m_thread;
    }

    bool DialogHost::Create()
    {
        if (m_window)
            return true;
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{ sizeof(wc) };
        if (!::GetClassInfoExW(instance, kClassName, &wc))
        {
            wc = { sizeof(wc) };
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = &DialogHost::WindowProc;
            wc.hInstance = instance;
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClassName;
            if (!::RegisterClassExW(&wc))
                return false;
        }
        m_window = ::CreateWindowExW(0, kClassName, L"", WS_POPUP, 0, 0, 100, static_cast<int>(kStartHeight), m_owner, nullptr, instance, this);
        if (!m_window)
            return false;
        const BOOL dark = TRUE;
        ::DwmSetWindowAttribute(m_window, kUseImmersiveDarkMode, &dark, sizeof(dark));
        const DWORD corner = kCornerRound;
        ::DwmSetWindowAttribute(m_window, kWindowCornerPreference, &corner, sizeof(corner));

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr))
            hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr))
            hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr))
        {
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            desc.Width = 100;
            desc.Height = kStartHeight;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            hr = factory->CreateSwapChainForHwnd(m_device, m_window, &desc, nullptr, nullptr, &m_swapChain);
            if (SUCCEEDED(hr))
                factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
        }
        if (factory)
            factory->Release();
        if (adapter)
            adapter->Release();
        if (dxgiDevice)
            dxgiDevice->Release();
        if (FAILED(hr) || !m_swapChain)
            return false;
        m_width = 100;
        m_height = kStartHeight;
        if (!ResizeTarget(m_width, m_height))
            return false;

        m_context = ImGui::CreateContext(m_atlas);
        ContextScope scope(m_context);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        return ImGui_ImplWin32_Init(m_window) && ImGui_ImplDX11_Init(m_device, m_deviceContext);
    }

    void DialogHost::ReleaseTarget()
    {
        if (m_target)
        {
            m_target->Release();
            m_target = nullptr;
        }
    }

    bool DialogHost::ResizeTarget(UINT width, UINT height)
    {
        width = std::max<UINT>(width, 1);
        height = std::max<UINT>(height, 1);
        if (m_target && width == m_width && height == m_height)
            return true;
        ReleaseTarget();
        if (FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
            return false;
        ID3D11Texture2D* buffer = nullptr;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer)
            return false;
        const HRESULT hr = m_device->CreateRenderTargetView(buffer, nullptr, &m_target);
        buffer->Release();
        m_width = width;
        m_height = height;
        return SUCCEEDED(hr);
    }

    bool DialogHost::DrawFrame(const std::string& title, const char* icon, const std::function<bool()>& body, float& contentHeight)
    {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImVec2 size = ImGui::GetIO().DisplaySize;
        m_titleHeight = std::round(Px(36.0f));
        m_closeWidth = std::round(Px(46.0f));
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ManifoldTheme::Card);
        ImGui::Begin("##dialog", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(size.x, m_titleHeight), ImGui::GetColorU32(ManifoldTheme::Sidebar));
        draw->AddLine(ImVec2(0.0f, m_titleHeight - 1.0f), ImVec2(size.x, m_titleHeight - 1.0f), ImGui::GetColorU32(ManifoldTheme::Border));
        const float textY = std::round((m_titleHeight - ImGui::GetTextLineHeight()) * 0.5f);
        float x = std::round(Px(14.0f));
        if (icon && icon[0])
        {
            draw->AddText(ImVec2(x, textY), ImGui::GetColorU32(ManifoldTheme::Accent), icon);
            x += ImGui::CalcTextSize(icon).x + std::round(Px(9.0f));
        }
        ImGui::PushFont(Fonts::Bold(), 0.0f);
        draw->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(size.x - m_closeWidth - Px(8.0f), m_titleHeight), true);
        draw->AddText(ImVec2(x, textY), ImGui::GetColorU32(ManifoldTheme::Text), title.c_str());
        draw->PopClipRect();
        ImGui::PopFont();

        ImGui::SetCursorPos(ImVec2(size.x - m_closeWidth, 0.0f));
        if (ImGui::InvisibleButton("##close", ImVec2(m_closeWidth, m_titleHeight)))
            m_closeRequested = true;
        const bool closeHovered = ImGui::IsItemHovered();
        if (closeHovered)
            draw->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ManifoldTheme::WithAlpha(ManifoldTheme::Error, 0.85f)));
        const ImVec2 glyph = ImGui::CalcTextSize(ICON_FA_TIMES);
        draw->AddText(ImVec2(std::round(size.x - m_closeWidth * 0.5f - glyph.x * 0.5f), textY), ImGui::GetColorU32(closeHovered ? ManifoldTheme::Text : ManifoldTheme::Muted), ICON_FA_TIMES);

        ImGui::SetCursorPos(ImVec2(0.0f, m_titleHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, PxWhole(18.0f, 16.0f));
        ImGui::BeginChild("##body", ImVec2(size.x, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        const bool finished = body();
        ImGui::EndChild();
        contentHeight = m_titleHeight + ImGui::GetItemRectSize().y;

        ImGui::GetForegroundDrawList()->AddRect(ImVec2(0.0f, 0.0f), size, ImGui::GetColorU32(ManifoldTheme::Border));
        ImGui::End();
        ImGui::Render();
        return finished;
    }

    bool DialogHost::Run(const std::string& title, const char* icon, float width, const std::function<bool()>& body)
    {
        if (!Available())
            return false;
        if (!Create())
        {
            m_failed = true;
            return false;
        }
        m_running = true;
        m_closeRequested = false;

        const ImGuiStyle style = ImGui::GetStyle();
        ImFont* font = ImGui::GetIO().FontDefault;
        HWND active = ::GetActiveWindow();
        RECT anchor{};
        ::GetWindowRect(active ? active : m_owner, &anchor);

        bool finished = false;
        {
            ContextScope scope(m_context);
            ImGui::GetStyle() = style;
            ImGui::GetIO().FontDefault = font;
            const float scale = style.FontScaleDpi > 0.0f ? style.FontScaleDpi : 1.0f;
            const UINT pixelWidth = static_cast<UINT>(std::lround(width * scale));
            ResizeTarget(pixelWidth, m_height);
            ::SetWindowPos(m_window, nullptr, anchor.left, anchor.top, static_cast<int>(pixelWidth), static_cast<int>(m_height), SWP_NOZORDER | SWP_NOACTIVATE);

            ThreadWindows disabled{ m_window, {} };
            ::EnumThreadWindows(m_thread, &CollectEnabled, reinterpret_cast<LPARAM>(&disabled));
            for (HWND window : disabled.Windows)
                ::EnableWindow(window, FALSE);

            bool shown = false;
            for (int frame = 0;; ++frame)
            {
                bool quit = false;
                MSG message{};
                while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == WM_QUIT)
                    {
                        ::PostQuitMessage(static_cast<int>(message.wParam));
                        quit = true;
                        break;
                    }
                    ::TranslateMessage(&message);
                    ::DispatchMessageW(&message);
                }
                if (quit || m_closeRequested)
                    break;

                float contentHeight = 0.0f;
                finished = DrawFrame(title, icon, body, contentHeight);
                const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                m_deviceContext->OMSetRenderTargets(1, &m_target, nullptr);
                m_deviceContext->ClearRenderTargetView(m_target, clear);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                m_swapChain->Present(shown ? 1 : 0, 0);
                if (finished)
                    break;

                const UINT wanted = static_cast<UINT>(std::lround(contentHeight));
                const bool showNow = !shown && frame >= 2;
                if (frame >= 1 && wanted > 0 && (wanted != m_height || showNow))
                {
                    RECT rect{};
                    ::GetWindowRect(m_window, &rect);
                    int left = rect.left;
                    int top = rect.top;
                    if (!shown)
                    {
                        const RECT work = WorkArea(anchor);
                        left = (anchor.left + anchor.right - static_cast<int>(pixelWidth)) / 2;
                        top = (anchor.top + anchor.bottom - static_cast<int>(wanted)) / 2;
                        left = std::clamp(left, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - static_cast<int>(pixelWidth)));
                        top = std::clamp(top, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - static_cast<int>(wanted)));
                    }
                    ResizeTarget(pixelWidth, wanted);
                    ::SetWindowPos(m_window, nullptr, left, top, static_cast<int>(pixelWidth), static_cast<int>(wanted), SWP_NOZORDER | SWP_NOACTIVATE);
                }
                if (showNow)
                {
                    ::ShowWindow(m_window, SW_SHOW);
                    ::SetForegroundWindow(m_window);
                    ::SetFocus(m_window);
                    shown = true;
                }
            }

            ::ShowWindow(m_window, SW_HIDE);
            for (HWND window : disabled.Windows)
                ::EnableWindow(window, TRUE);
        }

        if (ImGui::GetCurrentContext())
        {
            ImGui::GetIO().ClearInputMouse();
            ImGui::GetIO().ClearInputKeys();
        }
        if (active && ::IsWindow(active))
            ::SetForegroundWindow(active);
        m_running = false;
        return finished;
    }

    LRESULT CALLBACK DialogHost::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return ::DefWindowProcW(window, message, wParam, lParam);
        }
        auto* self = reinterpret_cast<DialogHost*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && self->m_context && ImGui_ImplWin32_WndProcHandlerEx(window, message, wParam, lParam, ImGui::GetIO(self->m_context)))
            return 1;
        switch (message)
        {
        case WM_NCHITTEST:
        {
            const LRESULT hit = ::DefWindowProcW(window, message, wParam, lParam);
            if (hit == HTCLIENT && self)
            {
                POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ::ScreenToClient(window, &point);
                RECT client{};
                ::GetClientRect(window, &client);
                if (static_cast<float>(point.y) < self->m_titleHeight && static_cast<float>(point.x) < static_cast<float>(client.right) - self->m_closeWidth)
                    return HTCAPTION;
            }
            return hit;
        }
        case WM_CLOSE:
            if (self)
                self->m_closeRequested = true;
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
}
