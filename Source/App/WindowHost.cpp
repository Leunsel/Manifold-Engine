#include "App/WindowHost.h"

#include "App/Diagnostics.h"
#include "Core/Errors.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Resource.h"
#include "UI/Fonts.h"
#include "UI/Theme.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dwmapi.h>
#include <dxgi1_3.h>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);
void ImGui_ImplDX11_SetSwapChainDescs(const DXGI_SWAP_CHAIN_DESC* desc_templates, int desc_templates_count);

namespace Manifold
{
    namespace
    {
        void PutError(wchar_t* target, std::size_t size, ErrorCode code, std::string_view detail = {})
        {
            ::wcsncpy_s(target, size, Utf8ToWide(ErrorMessage(code, detail)).c_str(), _TRUNCATE);
        }

        std::string HexText(HRESULT result)
        {
            char text[16];
            std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
            return text;
        }

        void (*g_createViewportWindow)(ImGuiViewport*) = nullptr;
        void (*g_setViewportTitle)(ImGuiViewport*, const char*) = nullptr;
        void (*g_destroyViewportWindow)(ImGuiViewport*) = nullptr;
        void (*g_createViewportChain)(ImGuiViewport*) = nullptr;
        void (*g_resizeViewportChain)(ImGuiViewport*, ImVec2) = nullptr;
        HICON g_viewportIcon = nullptr;
        HICON g_viewportIconSmall = nullptr;
        UINT g_frameLatency = 0;

        constexpr ULONGLONG kToolWindowRefreshMilliseconds = 1000;
        constexpr DWORD kUseImmersiveDarkMode = 20;

        struct ToolWindowFrame
        {
            std::uint64_t Hash = 0;
            ULONGLONG ShownAt = 0;
            bool Dirty = true;
        };

        std::unordered_map<ImGuiID, ToolWindowFrame> g_toolFrames;
        std::vector<ImGuiViewport*> g_toolsToShow;

        std::uint64_t Mix(std::uint64_t hash, std::uint64_t value)
        {
            hash ^= value;
            hash *= 0x9E3779B97F4A7C15ull;
            return hash ^ (hash >> 29);
        }

        std::uint64_t HashBytes(std::uint64_t hash, const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            std::size_t at = 0;
            for (; at + 8 <= size; at += 8)
            {
                std::uint64_t word = 0;
                std::memcpy(&word, bytes + at, 8);
                hash = Mix(hash, word);
            }
            std::uint64_t tail = 0;
            if (size > at)
                std::memcpy(&tail, bytes + at, size - at);
            return Mix(hash, tail ^ size);
        }

        std::uint64_t DrawDataHash(const ImDrawData* data)
        {
            if (!data || !data->Valid)
                return 0;
            std::uint64_t hash = 0x6A09E667F3BCC909ull;
            hash = HashBytes(hash, &data->DisplayPos, sizeof(data->DisplayPos));
            hash = HashBytes(hash, &data->DisplaySize, sizeof(data->DisplaySize));
            hash = HashBytes(hash, &data->FramebufferScale, sizeof(data->FramebufferScale));
            for (const ImDrawList* list : data->CmdLists)
            {
                hash = HashBytes(hash, list->VtxBuffer.Data, static_cast<std::size_t>(list->VtxBuffer.size_in_bytes()));
                hash = HashBytes(hash, list->IdxBuffer.Data, static_cast<std::size_t>(list->IdxBuffer.size_in_bytes()));
                for (const ImDrawCmd& command : list->CmdBuffer)
                {
                    hash = HashBytes(hash, &command.ClipRect, sizeof(command.ClipRect));
                    hash = Mix(hash, reinterpret_cast<std::uintptr_t>(command.TexRef._TexData));
                    hash = Mix(hash, static_cast<std::uint64_t>(command.TexRef._TexID));
                    hash = Mix(hash, (static_cast<std::uint64_t>(command.VtxOffset) << 32) | command.IdxOffset);
                    hash = Mix(hash, command.ElemCount);
                    hash = Mix(hash, reinterpret_cast<std::uintptr_t>(command.UserCallback));
                    hash = Mix(hash, reinterpret_cast<std::uintptr_t>(command.UserCallbackData));
                }
            }
            return hash;
        }

        bool ToolWindowChanged(ImGuiViewport* viewport, ULONGLONG now)
        {
            ToolWindowFrame& frame = g_toolFrames[viewport->ID];
            const std::uint64_t hash = DrawDataHash(viewport->DrawData);
            if (!frame.Dirty && hash == frame.Hash && now - frame.ShownAt < kToolWindowRefreshMilliseconds)
                return false;
            frame.Hash = hash;
            frame.ShownAt = now;
            frame.Dirty = false;
            return true;
        }

        IDXGIOutput* g_vblankOutput = nullptr;
        HMONITOR g_vblankMonitor = nullptr;

        void ReleaseVBlankOutput()
        {
            if (g_vblankOutput)
                g_vblankOutput->Release();
            g_vblankOutput = nullptr;
            g_vblankMonitor = nullptr;
        }

        bool WaitForVBlank(ID3D11Device* device, HWND window)
        {
            const HMONITOR monitor = ::MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            if (monitor != g_vblankMonitor)
            {
                ReleaseVBlankOutput();
                g_vblankMonitor = monitor;
                IDXGIDevice* dxgi = nullptr;
                IDXGIAdapter* adapter = nullptr;
                if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
                {
                    IDXGIOutput* output = nullptr;
                    for (UINT index = 0; !g_vblankOutput && adapter->EnumOutputs(index, &output) != DXGI_ERROR_NOT_FOUND; ++index)
                    {
                        DXGI_OUTPUT_DESC desc{};
                        if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                            g_vblankOutput = output;
                        else
                            output->Release();
                        output = nullptr;
                    }
                }
                if (adapter)
                    adapter->Release();
                if (dxgi)
                    dxgi->Release();
            }
            return g_vblankOutput && SUCCEEDED(g_vblankOutput->WaitForVBlank());
        }

        void MatchFrameLatency(ID3D11Device* device, UINT presents)
        {
            presents = std::clamp<UINT>(presents, 1, 16);
            if (presents == g_frameLatency || !device)
                return;
            IDXGIDevice1* dxgi = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))))
            {
                if (SUCCEEDED(dxgi->SetMaximumFrameLatency(presents)))
                    g_frameLatency = presents;
                dxgi->Release();
            }
        }

        void CountPresentationMode(IDXGISwapChain* chain)
        {
            IDXGISwapChainMedia* media = nullptr;
            if (!chain || FAILED(chain->QueryInterface(IID_PPV_ARGS(&media))))
                return;
            DXGI_FRAME_STATISTICS_MEDIA stats{};
            if (SUCCEEDED(media->GetFrameStatisticsMedia(&stats)))
                Profiler::Count("Main window shown as an overlay (1 = yes)", stats.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_OVERLAY ? 1.0 : 0.0);
            media->Release();
        }

        void DestroyViewportWindow(ImGuiViewport* viewport)
        {
            PROFILE_ZONE("Close a tool window");
            g_toolFrames.erase(viewport->ID);
            g_destroyViewportWindow(viewport);
        }

        void CreateViewportChain(ImGuiViewport* viewport)
        {
            PROFILE_ZONE("Create a tool window's swap chain");
            g_toolFrames[viewport->ID].Dirty = true;
            g_createViewportChain(viewport);
        }

        void ResizeViewportChain(ImGuiViewport* viewport, ImVec2 size)
        {
            PROFILE_ZONE("Resize a tool window's swap chain");
            g_toolFrames[viewport->ID].Dirty = true;
            g_resizeViewportChain(viewport, size);
        }

        bool PrivateUse(std::uint32_t codepoint)
        {
            return (codepoint >= 0xE000 && codepoint <= 0xF8FF) || codepoint >= 0xF0000;
        }

        std::string PlainTitle(const char* title)
        {
            std::string plain;
            const std::string_view text(title ? title : "");
            for (std::size_t at = 0; at < text.size();)
            {
                const unsigned char lead = static_cast<unsigned char>(text[at]);
                const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
                std::uint32_t codepoint = length == 1 ? lead : lead & (0xFF >> (length + 1));
                for (std::size_t i = 1; i < length && at + i < text.size(); ++i)
                    codepoint = (codepoint << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3F);
                if (!PrivateUse(codepoint))
                    plain.append(text.substr(at, std::min(length, text.size() - at)));
                at += length;
            }
            std::string collapsed;
            for (char c : plain)
            {
                if (c == ' ' && (collapsed.empty() || collapsed.back() == ' '))
                    continue;
                collapsed += c;
            }
            while (!collapsed.empty() && collapsed.back() == ' ')
                collapsed.pop_back();
            return collapsed.empty() ? std::string("Manifold Engine") : collapsed;
        }

        void MatchViewportChrome(ImGuiViewport* viewport)
        {
            HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
            if (!hwnd)
                return;
            const BOOL dark = ManifoldTheme::IsLight() ? FALSE : TRUE;
            ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
        }

        void CreateViewportWindow(ImGuiViewport* viewport)
        {
            PROFILE_ZONE("Create a tool window");
            g_createViewportWindow(viewport);
            HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
            if (!hwnd)
                return;
            MatchViewportChrome(viewport);
            if (g_viewportIcon)
                ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_viewportIcon));
            if (g_viewportIconSmall)
                ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_viewportIconSmall));
        }

        void SetViewportTitle(ImGuiViewport* viewport, const char* title)
        {
            g_setViewportTitle(viewport, PlainTitle(title).c_str());
        }

        constexpr DWORD kWindowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;

        constexpr DWORD kWindowCornerPreference = 33;
        constexpr DWORD kBorderColor = 34;
        constexpr DWORD kCornerRound = 2;
        constexpr COLORREF kColorNone = 0xFFFFFFFE;
        constexpr float kCornerRadius = 8.0f;
        constexpr float kResizeEdge = 5.0f;
        constexpr float kResizeCorner = 14.0f;
        constexpr UINT_PTR kSizeMoveTimer = 1;
        constexpr unsigned kSizeMoveFrameMilliseconds = 33;

        bool ClicksOrKeysQueued()
        {
            for (const ImGuiInputEvent& event : ImGui::GetCurrentContext()->InputEventsQueue)
            {
                if (event.Type != ImGuiInputEventType_MousePos && event.Type != ImGuiInputEventType_MouseWheel && event.Type != ImGuiInputEventType_MouseViewport)
                    return true;
            }
            return false;
        }

        void ScaleStyle(const ImGuiStyle& base, float scale)
        {
            ImGuiStyle& style = ImGui::GetStyle();
            style = base;
            style.ScaleAllSizes(scale);
            if (scale < 1.0f)
            {
                style.WindowBorderSize = base.WindowBorderSize;
                style.ChildBorderSize = base.ChildBorderSize;
                style.PopupBorderSize = base.PopupBorderSize;
                style.FrameBorderSize = base.FrameBorderSize;
            }
            style.FontScaleDpi = scale;
        }

        Diagnostics::Adapter AdapterOf(IDXGIAdapter* adapter)
        {
            Diagnostics::Adapter info;
            IDXGIAdapter1* first = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&first))))
            {
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(first->GetDesc1(&desc)))
                {
                    info.Name = WideToUtf8(desc.Description);
                    info.Vendor = desc.VendorId;
                    info.VideoMemory = desc.DedicatedVideoMemory;
                    info.Luid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
                    info.Software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                }
                first->Release();
            }
            return info;
        }

        IDXGIAdapter1* HardwareAdapter(std::uint64_t luid)
        {
            if (!luid)
                return nullptr;
            IDXGIFactory1* factory = nullptr;
            if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return nullptr;
            IDXGIAdapter1* found = nullptr;
            IDXGIAdapter1* adapter = nullptr;
            for (UINT index = 0; !found && factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index)
            {
                DXGI_ADAPTER_DESC1 desc{};
                const std::uint64_t id = SUCCEEDED(adapter->GetDesc1(&desc)) ? (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart : 0;
                if (id == luid && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                    found = adapter;
                else
                    adapter->Release();
                adapter = nullptr;
            }
            factory->Release();
            return found;
        }

        void DescribeGraphics(HWND window, IDXGIAdapter* adapter, IDXGIFactory2* factory, D3D_FEATURE_LEVEL level, bool warp, std::string choice, bool chosenByWindows)
        {
            Diagnostics::GraphicsInfo info;
            info.Window = window;
            info.Warp = warp;
            info.Choice = std::move(choice);
            info.ChosenByWindows = chosenByWindows;
            info.Device = AdapterOf(adapter);
            LARGE_INTEGER version{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
            {
                char text[48];
                std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(version.HighPart), LOWORD(version.HighPart), HIWORD(version.LowPart), LOWORD(version.LowPart));
                info.Driver = text;
            }
            char feature[24];
            std::snprintf(feature, sizeof(feature), "Direct3D %u_%u", (static_cast<unsigned>(level) >> 12) & 0xFu, (static_cast<unsigned>(level) >> 8) & 0xFu);
            info.FeatureLevel = feature;
            info.SwapEffect = "flip discard, 3 buffers, synced to the display";
            IDXGIAdapter1* other = nullptr;
            for (UINT index = 0; factory->EnumAdapters1(index, &other) != DXGI_ERROR_NOT_FOUND; ++index)
            {
                info.Adapters.push_back(AdapterOf(other));
                other->Release();
                other = nullptr;
            }
            Diagnostics::SetGraphics(std::move(info));
        }

        void CountDrawData()
        {
            int lists = 0;
            int commands = 0;
            int vertices = 0;
            int viewports = 0;
            for (ImGuiViewport* viewport : ImGui::GetPlatformIO().Viewports)
            {
                const ImDrawData* data = viewport->DrawData;
                if (!data || !data->Valid)
                    continue;
                ++viewports;
                lists += data->CmdListsCount;
                vertices += data->TotalVtxCount;
                for (const ImDrawList* list : data->CmdLists)
                    commands += list->CmdBuffer.Size;
            }
            Profiler::Count("Draw calls", commands);
            Profiler::Count("Vertices", vertices);
            Profiler::Count("Draw lists", lists);
            Profiler::Count("Viewports drawn", viewports);
            Profiler::Count("ImGui windows", ImGui::GetCurrentContext()->Windows.Size);
        }

        bool IsInputMessage(UINT message)
        {
            return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
                   message == WM_NCLBUTTONDOWN || message == WM_NCMOUSEMOVE || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SIZE ||
                   message == WM_MOVE || message == WM_ACTIVATE;
        }
    }

    WindowHost::WindowHost(Client& client, Setup setup) : m_client(client), m_setup(std::move(setup))
    {
        m_layoutFile = WideToUtf8(m_setup.LayoutFile.wstring());
    }

    WindowHost::~WindowHost()
    {
        m_closing = true;
        if (ImGui::GetCurrentContext())
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            m_imgui = nullptr;
        }
        DestroyDeviceAndSwapChain();
        if (m_hwnd)
            ::DestroyWindow(m_hwnd);
        if (m_registered)
            ::UnregisterClassW(m_setup.ClassName, m_instance);
        delete static_cast<ImGuiStyle*>(m_baseStyle);
    }

    bool WindowHost::Open(HINSTANCE instance)
    {
        m_instance = instance;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &WindowHost::WndProcThunk;
        wc.hInstance = instance;
        wc.hIcon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), 0));
        wc.hIconSm = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), 0));
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = m_setup.ClassName;
        if (!::RegisterClassExW(&wc))
        {
            PutError(m_error, ARRAYSIZE(m_error), ErrorCode::WindowClassFailed);
            return false;
        }
        m_registered = true;

        m_hwnd = ::CreateWindowExW(0, m_setup.ClassName, m_setup.Title, kWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT, m_setup.MinimumWidth, m_setup.MinimumHeight, nullptr,
            nullptr, instance, this);
        if (!m_hwnd)
        {
            PutError(m_error, ARRAYSIZE(m_error), ErrorCode::WindowFailed);
            return false;
        }
        m_dpi = ::GetDpiForWindow(m_hwnd);
        return true;
    }

    SIZE WindowHost::MinimumSize() const
    {
        const float scale = WindowScale();
        return { std::lround(m_setup.MinimumWidth * scale), std::lround(m_setup.MinimumHeight * scale) };
    }

    void WindowHost::Place(const WindowPlacement& placement)
    {
        const float scale = WindowScale();
        const SIZE smallest = MinimumSize();
        SIZE size{ (std::max)(smallest.cx, std::lround(placement.Width * scale)), (std::max)(smallest.cy, std::lround(placement.Height * scale)) };
        int x = 0;
        int y = 0;
        bool placed = false;

        const RECT saved{ placement.X, placement.Y, placement.X + size.cx, placement.Y + size.cy };
        if (placement.Placed && ::MonitorFromRect(&saved, MONITOR_DEFAULTTONULL))
        {
            x = saved.left;
            y = saved.top;
            placed = true;
        }

        MONITORINFO info = { sizeof(info), {}, {}, 0 };
        const HMONITOR monitor = placed ? ::MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST) : ::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor && ::GetMonitorInfoW(monitor, &info))
        {
            const LONG workWidth = info.rcWork.right - info.rcWork.left;
            const LONG workHeight = info.rcWork.bottom - info.rcWork.top;
            size.cx = (std::max)(smallest.cx, (std::min)(size.cx, workWidth));
            size.cy = (std::max)(smallest.cy, (std::min)(size.cy, workHeight));
            if (!placed)
            {
                x = info.rcWork.left + (workWidth - size.cx) / 2;
                y = info.rcWork.top + (std::max)(0L, (workHeight - size.cy) / 2);
            }
        }
        ::SetWindowPos(m_hwnd, nullptr, x, y, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ApplyWindowChrome();
    }

    std::optional<WindowPlacement> WindowHost::Placement() const
    {
        if (!m_hwnd)
            return std::nullopt;
        WINDOWPLACEMENT placement{ sizeof(placement) };
        if (!::GetWindowPlacement(m_hwnd, &placement))
            return std::nullopt;
        RECT rect = placement.rcNormalPosition;
        if (!::IsIconic(m_hwnd) && !::IsZoomed(m_hwnd) && ::IsWindowVisible(m_hwnd))
        {
            if (!::GetWindowRect(m_hwnd, &rect))
                return std::nullopt;
        }
        else
        {
            MONITORINFO info = { sizeof(info), {}, {}, 0 };
            if (::GetMonitorInfoW(::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info))
                ::OffsetRect(&rect, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
        }
        const float scale = WindowScale();
        WindowPlacement result;
        result.X = rect.left;
        result.Y = rect.top;
        result.Width = std::lround((rect.right - rect.left) / scale);
        result.Height = std::lround((rect.bottom - rect.top) / scale);
        result.Maximised = ::IsZoomed(m_hwnd) || (::IsIconic(m_hwnd) && (placement.flags & WPF_RESTORETOMAXIMIZED));
        result.Placed = true;
        return result;
    }

    void WindowHost::ApplyWindowChrome()
    {
        const BOOL dark = ManifoldTheme::IsLight() ? FALSE : TRUE;
        ::DwmSetWindowAttribute(m_hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));
        const DWORD corner = kCornerRound;
        m_roundedCorners = SUCCEEDED(::DwmSetWindowAttribute(m_hwnd, kWindowCornerPreference, &corner, sizeof(corner)));
        const COLORREF border = kColorNone;
        ::DwmSetWindowAttribute(m_hwnd, kBorderColor, &border, sizeof(border));
    }

    float WindowHost::CornerRadius() const
    {
        return m_roundedCorners && !Maximised() ? kCornerRadius * WindowScale() : 0.0f;
    }

    void WindowHost::AddDragArea(float left, float top, float right, float bottom)
    {
        m_dragAreas.push_back(RECT{ std::lround(left), std::lround(top), std::lround(right), std::lround(bottom) });
    }

    bool WindowHost::CreateDeviceAndSwapChain()
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL obtained = {};
        bool warp = false;
        HRESULT hr = E_FAIL;
        const std::string preference = Diagnostics::WindowsGraphicsPreference();
        const bool chosenByWindows = !preference.empty();
        std::string choice = chosenByWindows ? "Windows graphics setting for this program (" + preference + ")" : "Windows default (no card found for the display)";
        if (!chosenByWindows)
        {
            if (IDXGIAdapter1* display = HardwareAdapter(Diagnostics::DisplayAdapterLuid(m_hwnd)))
            {
                hr = ::D3D11CreateDevice(display, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &obtained, &m_context);
                display->Release();
                choice = SUCCEEDED(hr) ? "the card the display is connected to" : "Windows default (the display's card failed with " + HexText(hr) + ")";
            }
        }
        if (FAILED(hr))
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &obtained, &m_context);
        if (hr == DXGI_ERROR_UNSUPPORTED)
        {
            warp = true;
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, &obtained, &m_context);
        }
        if (FAILED(hr))
        {
            PutError(m_error, ARRAYSIZE(m_error), ErrorCode::DeviceFailed, HexText(hr));
            return false;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        auto release = [&]
            {
                if (factory) factory->Release();
                if (adapter) adapter->Release();
                if (dxgiDevice) dxgiDevice->Release();
            };

        hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            release();
            PutError(m_error, ARRAYSIZE(m_error), ErrorCode::FactoryFailed, HexText(hr));
            return false;
        }
        DescribeGraphics(m_hwnd, adapter, factory, obtained, warp, warp ? std::string("software fallback") : std::move(choice), chosenByWindows);

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 3;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(m_device, m_hwnd, &desc, nullptr, nullptr, &m_swapChain);
        if (FAILED(hr))
        {
            release();
            PutError(m_error, ARRAYSIZE(m_error), ErrorCode::SwapChainFailed, HexText(hr));
            return false;
        }
        factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
        release();
        return CreateRenderTarget();
    }

    bool WindowHost::CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr) || !backBuffer)
            return false;
        hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_renderTarget);
        backBuffer->Release();
        return SUCCEEDED(hr);
    }

    void WindowHost::DestroyRenderTarget()
    {
        if (m_renderTarget)
        {
            m_renderTarget->Release();
            m_renderTarget = nullptr;
        }
    }

    void WindowHost::DestroyDeviceAndSwapChain()
    {
        ReleaseVBlankOutput();
        DestroyRenderTarget();
        if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
        if (m_context) { m_context->Release(); m_context = nullptr; }
        if (m_device) { m_device->Release(); m_device = nullptr; }
    }

    float WindowHost::WindowScale() const
    {
        return static_cast<float>(m_dpi) / 96.0f;
    }

    void WindowHost::RescaleForDpi(UINT dpi)
    {
        m_dpi = dpi;
        if (m_baseStyle)
            ScaleStyle(*static_cast<const ImGuiStyle*>(m_baseStyle), WindowScale());
    }

    void WindowHost::Restyle()
    {
        if (!m_baseStyle)
            return;
        ImGuiStyle style;
        if (ManifoldTheme::IsLight())
            ImGui::StyleColorsLight(&style);
        else
            ImGui::StyleColorsDark(&style);
        ManifoldTheme::Apply(style);
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        *static_cast<ImGuiStyle*>(m_baseStyle) = style;
        RescaleForDpi(m_dpi);
        if (m_hwnd)
            ApplyWindowChrome();
        if (m_imgui)
        {
            const ImGuiPlatformIO& platform = ImGui::GetPlatformIO(m_imgui);
            for (int i = 1; i < platform.Viewports.Size; ++i)
                MatchViewportChrome(platform.Viewports[i]);
        }
    }

    void WindowHost::ApplyPendingResize()
    {
        if (m_pendingWidth == 0 || m_pendingHeight == 0 || !m_swapChain)
            return;
        DestroyRenderTarget();
        m_swapChain->ResizeBuffers(0, m_pendingWidth, m_pendingHeight, DXGI_FORMAT_UNKNOWN, 0);
        m_pendingWidth = m_pendingHeight = 0;
        CreateRenderTarget();
    }

    void WindowHost::RenderFrame()
    {
        if (m_inFrame || ImGui::GetCurrentContext() != m_imgui)
            return;
        m_inFrame = true;
        struct FrameDone
        {
            bool& Flag;
            ~FrameDone() { Flag = false; }
        } frameDone{ m_inFrame };

        Profiler::BeginFrame();
        {
            PROFILE_ZONE("Engine tick");
            m_client.BeforeFrame();
        }

        {
            PROFILE_ZONE("Input and new frame");
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::GetIO().ConfigInputTrickleEventQueue = ClicksOrKeysQueued();
            ImGui::NewFrame();
        }

        {
            PROFILE_ZONE("Build the interface");
            m_dragAreas.clear();
            m_client.DrawFrame();
        }

        {
            PROFILE_ZONE("Finish the draw lists");
            ImGui::Render();
        }
        if (Profiler::Enabled())
            CountDrawData();

        const bool mainShown = ::IsWindowVisible(m_hwnd) && !::IsIconic(m_hwnd);
        if (mainShown)
        {
            PROFILE_ZONE("Draw the main window");
            const ImVec4 clear = ManifoldTheme::Page;
            const float rgba[4] = { clear.x, clear.y, clear.z, 1.0f };
            m_context->OMSetRenderTargets(1, &m_renderTarget, nullptr);
            m_context->ClearRenderTargetView(m_renderTarget, rgba);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        UINT presents = mainShown ? 1 : 0;
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
            g_toolsToShow.clear();
            {
                PROFILE_ZONE("Draw the tool windows");
                ImGui::UpdatePlatformWindows();
                const ULONGLONG now = ::GetTickCount64();
                int unchanged = 0;
                for (int i = 1; i < platform.Viewports.Size; ++i)
                {
                    ImGuiViewport* viewport = platform.Viewports[i];
                    if (viewport->Flags & ImGuiViewportFlags_IsMinimized)
                    {
                        g_toolFrames[viewport->ID].Dirty = true;
                        continue;
                    }
                    if (!ToolWindowChanged(viewport, now))
                    {
                        ++unchanged;
                        continue;
                    }
                    g_toolsToShow.push_back(viewport);
                    if (platform.Platform_RenderWindow)
                        platform.Platform_RenderWindow(viewport, nullptr);
                    if (platform.Renderer_RenderWindow)
                        platform.Renderer_RenderWindow(viewport, nullptr);
                }
                if (Profiler::Enabled())
                    Profiler::Count("Tool windows left as they were", unchanged);
            }
            presents += static_cast<UINT>(g_toolsToShow.size());
            MatchFrameLatency(m_device, presents);
            PROFILE_WAIT("Present the tool windows");
            for (ImGuiViewport* viewport : g_toolsToShow)
            {
                if (platform.Platform_SwapBuffers)
                    platform.Platform_SwapBuffers(viewport, nullptr);
                if (platform.Renderer_SwapBuffers)
                    platform.Renderer_SwapBuffers(viewport, nullptr);
            }
        }
        else
            MatchFrameLatency(m_device, presents);

        PROFILE_WAIT("Present (waits for the display)");
        if (mainShown)
        {
            const HRESULT hr = m_swapChain->Present(m_relaxedPacing && RefreshRate() >= 100 ? 2 : 1, 0);
            m_occluded = hr == DXGI_STATUS_OCCLUDED && !m_cloaked;
            if (Profiler::Enabled())
                CountPresentationMode(m_swapChain);
            if (m_cloaked && !m_presentedCloaked)
            {
                m_presentedCloaked = true;
                m_client.FirstFrameReady();
            }
            if (m_cloaked && m_revealRequested)
                Reveal();
        }
        if (!mainShown || m_occluded)
        {
            HWND pacer = m_hwnd;
            const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
            for (int i = 1; i < platform.Viewports.Size; ++i)
            {
                const ImGuiViewport* viewport = platform.Viewports[i];
                HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
                if (hwnd && !(viewport->Flags & ImGuiViewportFlags_IsMinimized))
                {
                    pacer = hwnd;
                    break;
                }
            }
            if (!WaitForVBlank(m_device, pacer))
                ::DwmFlush();
        }
    }

    UINT WindowHost::RefreshRate()
    {
        const ULONGLONG now = ::GetTickCount64();
        if (m_refreshRate && now - m_refreshCheckedAt < 2000)
            return m_refreshRate;
        m_refreshCheckedAt = now;
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(::DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiDenominator)
            m_refreshRate = static_cast<UINT>((timing.rateRefresh.uiNumerator + timing.rateRefresh.uiDenominator / 2) / timing.rateRefresh.uiDenominator);
        else
            m_refreshRate = 60;
        return m_refreshRate;
    }

    void WindowHost::HideUntilFirstFrame()
    {
        const BOOL cloak = TRUE;
        m_cloaked = SUCCEEDED(::DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
    }

    void WindowHost::Reveal()
    {
        if (!m_cloaked)
            return;
        const BOOL cloak = FALSE;
        ::DwmSetWindowAttribute(m_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        m_cloaked = false;
        m_revealRequested = false;
        m_occluded = false;
        NoteInput();
        m_client.Revealed();
    }

    bool WindowHost::StartRendering()
    {
        if (!CreateDeviceAndSwapChain())
            return false;

        IMGUI_CHECKVERSION();
        m_imgui = ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
        io.ConfigDpiScaleFonts = false;
        io.ConfigDpiScaleViewports = false;
        io.ConfigDockingWithShift = true;
        io.ConfigWindowsMoveFromTitleBarOnly = true;
        io.ConfigViewportsNoTaskBarIcon = false;
        io.IniFilename = m_layoutFile.empty() ? nullptr : m_layoutFile.c_str();
        io.LogFilename = nullptr;

        m_baseStyle = new ImGuiStyle(ImGui::GetStyle());
        Restyle();

        const char* fontError = nullptr;
        if (!Fonts::Build(&fontError))
        {
            ::wcsncpy_s(m_error, Utf8ToWide(fontError ? fontError : ErrorMessage(ErrorCode::FontAtlasFailed)).c_str(), _TRUNCATE);
            return false;
        }

        if (!ImGui_ImplWin32_Init(m_hwnd) || !ImGui_ImplDX11_Init(m_device, m_context))
        {
            PutError(m_error, ARRAYSIZE(m_error), ErrorCode::ImGuiFailed);
            return false;
        }
        DXGI_SWAP_CHAIN_DESC viewportChain = {};
        viewportChain.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        viewportChain.SampleDesc.Count = 1;
        viewportChain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        viewportChain.BufferCount = 3;
        viewportChain.Windowed = TRUE;
        viewportChain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ImGui_ImplDX11_SetSwapChainDescs(&viewportChain, 1);
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        g_viewportIcon = static_cast<HICON>(::LoadImageW(m_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_SHARED));
        g_viewportIconSmall = static_cast<HICON>(::LoadImageW(m_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (platform.Platform_CreateWindow && platform.Platform_CreateWindow != &CreateViewportWindow)
        {
            g_createViewportWindow = platform.Platform_CreateWindow;
            platform.Platform_CreateWindow = &CreateViewportWindow;
        }
        if (platform.Platform_SetWindowTitle && platform.Platform_SetWindowTitle != &SetViewportTitle)
        {
            g_setViewportTitle = platform.Platform_SetWindowTitle;
            platform.Platform_SetWindowTitle = &SetViewportTitle;
        }
        if (platform.Platform_DestroyWindow && platform.Platform_DestroyWindow != &DestroyViewportWindow)
        {
            g_destroyViewportWindow = platform.Platform_DestroyWindow;
            platform.Platform_DestroyWindow = &DestroyViewportWindow;
        }
        if (platform.Renderer_CreateWindow && platform.Renderer_CreateWindow != &CreateViewportChain)
        {
            g_createViewportChain = platform.Renderer_CreateWindow;
            platform.Renderer_CreateWindow = &CreateViewportChain;
        }
        if (platform.Renderer_SetWindowSize && platform.Renderer_SetWindowSize != &ResizeViewportChain)
        {
            g_resizeViewportChain = platform.Renderer_SetWindowSize;
            platform.Renderer_SetWindowSize = &ResizeViewportChain;
        }
        return true;
    }

    bool WindowHost::PumpMessages()
    {
        bool quit = false;
        MSG msg = {};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (IsInputMessage(msg.message))
                m_lastInputTick = ::GetTickCount64();
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                quit = true;
        }
        return !quit;
    }

    bool WindowHost::Hidden()
    {
        if (m_cloaked)
            return false;
        if (!::IsWindowVisible(m_hwnd) || ::IsIconic(m_hwnd) || (m_occluded && m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED))
            return !ToolWindowShown();
        m_occluded = false;
        return false;
    }

    bool WindowHost::ToolWindowShown() const
    {
        if (!m_imgui)
            return false;
        const ImGuiPlatformIO& platform = ImGui::GetPlatformIO(m_imgui);
        for (int i = 1; i < platform.Viewports.Size; ++i)
        {
            const ImGuiViewport* viewport = platform.Viewports[i];
            HWND hwnd = static_cast<HWND>(viewport->PlatformHandleRaw ? viewport->PlatformHandleRaw : viewport->PlatformHandle);
            if (hwnd && ::IsWindowVisible(hwnd) && !::IsIconic(hwnd))
                return true;
        }
        return false;
    }

    bool WindowHost::Wait(unsigned milliseconds) const
    {
        return ::MsgWaitForMultipleObjectsEx(0, nullptr, milliseconds, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0;
    }

    void WindowHost::NoteInput()
    {
        m_lastInputTick = ::GetTickCount64();
    }

    unsigned long long WindowHost::InputAge() const
    {
        return ::GetTickCount64() - m_lastInputTick;
    }

    LRESULT CALLBACK WindowHost::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        WindowHost* self = nullptr;
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<WindowHost*>(cs->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->m_hwnd = hwnd;
        }
        else
        {
            self = reinterpret_cast<WindowHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self)
            return self->WndProc(hwnd, msg, wp, lp);
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WindowHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (IsInputMessage(msg))
            m_lastInputTick = ::GetTickCount64();

        if (!m_closing)
        {
            LRESULT handled = 0;
            if (m_client.HandleMessage(msg, wp, lp, handled))
                return handled;
        }

        if (!m_closing && m_imgui && ImGui_ImplWin32_WndProcHandlerEx(hwnd, msg, wp, lp, ImGui::GetIO(m_imgui)))
            return 1;

        switch (msg)
        {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED)
            {
                m_pendingWidth = LOWORD(lp);
                m_pendingHeight = HIWORD(lp);
                if (m_inSizeMove && !m_inFrame && m_renderTarget && ImGui::GetCurrentContext())
                {
                    ApplyPendingResize();
                    RenderFrame();
                }
            }
            return 0;

        case WM_ENTERSIZEMOVE:
            m_inSizeMove = true;
            ::SetTimer(hwnd, kSizeMoveTimer, kSizeMoveFrameMilliseconds, nullptr);
            return 0;

        case WM_EXITSIZEMOVE:
            m_inSizeMove = false;
            ::KillTimer(hwnd, kSizeMoveTimer);
            return 0;

        case WM_TIMER:
            if (wp == kSizeMoveTimer && !m_inFrame && m_renderTarget && ImGui::GetCurrentContext())
            {
                ApplyPendingResize();
                RenderFrame();
            }
            return 0;

        case WM_DPICHANGED:
        {
            RescaleForDpi(HIWORD(wp));
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        case WM_GETMINMAXINFO:
        {
            const SIZE smallest = MinimumSize();
            auto* limits = reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize = { smallest.cx, smallest.cy };
            return 0;
        }

        case WM_NCCALCSIZE:
            if (wp)
            {
                if (::IsZoomed(hwnd))
                {
                    const UINT dpi = ::GetDpiForWindow(hwnd);
                    const int padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    auto* sizes = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    ::InflateRect(&sizes->rgrc[0], -(::GetSystemMetricsForDpi(SM_CXFRAME, dpi) + padding), -(::GetSystemMetricsForDpi(SM_CYFRAME, dpi) + padding));
                }
                return 0;
            }
            break;

        case WM_NCHITTEST:
        {
            POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ::ScreenToClient(hwnd, &point);
            if (!::IsZoomed(hwnd))
            {
                RECT client{};
                ::GetClientRect(hwnd, &client);
                const LONG edge = std::lround(kResizeEdge * WindowScale());
                const LONG corner = std::lround(kResizeCorner * WindowScale());
                const bool nearLeft = point.x < corner;
                const bool nearRight = point.x >= client.right - corner;
                const bool nearTop = point.y < corner;
                const bool nearBottom = point.y >= client.bottom - corner;
                if (point.y < edge)
                    return nearLeft ? HTTOPLEFT : nearRight ? HTTOPRIGHT : HTTOP;
                if (point.y >= client.bottom - edge)
                    return nearLeft ? HTBOTTOMLEFT : nearRight ? HTBOTTOMRIGHT : HTBOTTOM;
                if (point.x < edge)
                    return nearTop ? HTTOPLEFT : nearBottom ? HTBOTTOMLEFT : HTLEFT;
                if (point.x >= client.right - edge)
                    return nearTop ? HTTOPRIGHT : nearBottom ? HTBOTTOMRIGHT : HTRIGHT;
            }
            for (const RECT& area : m_dragAreas)
            {
                if (::PtInRect(&area, point))
                    return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0)
                return 0;
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            ::ValidateRect(hwnd, nullptr);
            return 0;

        case WM_CLOSE:
            if (!m_closing)
                m_client.CloseRequested();
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}
