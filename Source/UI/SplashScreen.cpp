#include "UI/SplashScreen.h"

#include "Core/MainThread.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Resource.h"
#include "UI/Theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Manifold::UI
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"ManifoldEngineSplash";
        constexpr UINT_PTR kFrameTimer = 1;
        constexpr UINT kFrameMilliseconds = 15;
        constexpr float kCardWidth = 440.0f;
        constexpr float kCardHeight = 248.0f;
        constexpr float kMargin = 24.0f;
        constexpr float kRadius = 10.0f;
        constexpr double kFadeInSeconds = 0.09;
        constexpr double kFadeOutSeconds = 0.16;
        constexpr int kCloseFade = 1;
        constexpr int kCloseNow = 2;

        struct Rgb
        {
            float R = 0.0f;
            float G = 0.0f;
            float B = 0.0f;
        };

        Rgb Of(const ImVec4& color)
        {
            return { color.x, color.y, color.z };
        }

        Rgb Mix(Rgb from, Rgb to, float amount)
        {
            return { from.R + (to.R - from.R) * amount, from.G + (to.G - from.G) * amount, from.B + (to.B - from.B) * amount };
        }

        float Clamp01(float value)
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        float RoundedBox(float x, float y, float centerX, float centerY, float halfWidth, float halfHeight, float radius)
        {
            const float qx = std::fabs(x - centerX) - (halfWidth - radius);
            const float qy = std::fabs(y - centerY) - (halfHeight - radius);
            const float ox = std::max(qx, 0.0f);
            const float oy = std::max(qy, 0.0f);
            return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
        }

        UINT MonitorDpi(HMONITOR monitor)
        {
            using DpiFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            static const DpiFunction function = []
                {
                    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
                    return shcore ? reinterpret_cast<DpiFunction>(reinterpret_cast<void*>(::GetProcAddress(shcore, "GetDpiForMonitor"))) : nullptr;
                }();
            UINT x = 0;
            UINT y = 0;
            if (function && SUCCEEDED(function(monitor, 0, &x, &y)) && x)
                return x;
            return ::GetDpiForSystem();
        }

        struct Pixel
        {
            float B = 0.0f;
            float G = 0.0f;
            float R = 0.0f;
            float A = 0.0f;
        };

        struct Mask
        {
            int Width = 0;
            int Height = 0;
            std::vector<std::uint8_t> Coverage;
        };

        HFONT MakeFont(float pixels, bool bold)
        {
            return ::CreateFontW(-static_cast<int>(std::lround(pixels)), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
        }

        Mask RenderText(HFONT font, const std::wstring& text)
        {
            Mask mask;
            if (text.empty())
                return mask;
            HDC dc = ::CreateCompatibleDC(nullptr);
            HGDIOBJ oldFont = ::SelectObject(dc, font);
            SIZE size{};
            ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            TEXTMETRICW metrics{};
            ::GetTextMetricsW(dc, &metrics);
            mask.Width = size.cx + 2;
            mask.Height = metrics.tmHeight;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = mask.Width;
            info.bmiHeader.biHeight = -mask.Height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap && bits)
            {
                HGDIOBJ oldBitmap = ::SelectObject(dc, bitmap);
                std::memset(bits, 0, static_cast<std::size_t>(mask.Width) * static_cast<std::size_t>(mask.Height) * 4);
                ::SetBkMode(dc, TRANSPARENT);
                ::SetTextColor(dc, RGB(255, 255, 255));
                ::TextOutW(dc, 1, 0, text.c_str(), static_cast<int>(text.size()));
                ::GdiFlush();
                const auto* pixels = static_cast<const std::uint8_t*>(bits);
                mask.Coverage.resize(static_cast<std::size_t>(mask.Width) * static_cast<std::size_t>(mask.Height));
                for (std::size_t i = 0; i < mask.Coverage.size(); ++i)
                    mask.Coverage[i] = std::max({ pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2] });
                ::SelectObject(dc, oldBitmap);
            }
            if (bitmap)
                ::DeleteObject(bitmap);
            ::SelectObject(dc, oldFont);
            ::DeleteDC(dc);
            return mask;
        }

        struct IconImage
        {
            int Size = 0;
            std::vector<std::uint8_t> Bgra;
        };

        IconImage LoadIconImage(HINSTANCE instance, int size)
        {
            IconImage image;
            HICON icon = static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
            if (!icon)
                return image;
            ICONINFO parts{};
            if (::GetIconInfo(icon, &parts) && parts.hbmColor)
            {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(info.bmiHeader);
                info.bmiHeader.biWidth = size;
                info.bmiHeader.biHeight = -size;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                image.Bgra.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
                HDC dc = ::CreateCompatibleDC(nullptr);
                if (::GetDIBits(dc, parts.hbmColor, 0, static_cast<UINT>(size), image.Bgra.data(), &info, DIB_RGB_COLORS) == size)
                {
                    image.Size = size;
                    bool anyAlpha = false;
                    for (std::size_t i = 3; i < image.Bgra.size(); i += 4)
                        anyAlpha = anyAlpha || image.Bgra[i] != 0;
                    if (!anyAlpha && parts.hbmMask)
                    {
                        std::vector<std::uint8_t> mask(image.Bgra.size());
                        if (::GetDIBits(dc, parts.hbmMask, 0, static_cast<UINT>(size), mask.data(), &info, DIB_RGB_COLORS) == size)
                        {
                            for (std::size_t i = 0; i < image.Bgra.size(); i += 4)
                                image.Bgra[i + 3] = mask[i] ? 0 : 255;
                        }
                    }
                }
                ::DeleteDC(dc);
            }
            if (parts.hbmColor)
                ::DeleteObject(parts.hbmColor);
            if (parts.hbmMask)
                ::DeleteObject(parts.hbmMask);
            ::DestroyIcon(icon);
            return image;
        }

        class Painter
        {
        public:
            Painter(HINSTANCE instance, float scale) : m_scale(scale)
            {
                m_margin = std::round(kMargin * scale);
                m_cardWidth = std::round(kCardWidth * scale);
                m_cardHeight = std::round(kCardHeight * scale);
                Width = static_cast<int>(m_cardWidth + m_margin * 2.0f);
                Height = static_cast<int>(m_cardHeight + m_margin * 2.0f);
                m_title = MakeFont(23.0f * scale, true);
                m_body = MakeFont(13.0f * scale, false);
                m_small = MakeFont(12.0f * scale, false);
                BuildBase(instance);
            }

            ~Painter()
            {
                ::DeleteObject(m_title);
                ::DeleteObject(m_body);
                ::DeleteObject(m_small);
            }

            Painter(const Painter&) = delete;
            Painter& operator=(const Painter&) = delete;

            void SetStatus(const std::string& status)
            {
                m_status = RenderText(m_small, Utf8ToWide(status));
            }

            void Paint(std::uint32_t* target, double seconds) const
            {
                std::memcpy(target, m_base.data(), m_base.size() * sizeof(std::uint32_t));
                const float left = m_margin + std::round(28.0f * m_scale);
                const float right = m_margin + m_cardWidth - std::round(28.0f * m_scale);
                const float barHeight = std::max(2.0f, std::round(3.0f * m_scale));
                const float barTop = m_margin + m_cardHeight - std::round(26.0f * m_scale);
                const int statusTop = static_cast<int>(barTop - std::round(10.0f * m_scale)) - m_status.Height;
                Stamp(target, m_status, static_cast<int>(left), statusTop, Of(ManifoldTheme::Muted), 1.0f);

                const Rgb track = Mix(m_cardColor, Of(ManifoldTheme::Text), 0.07f);
                const Rgb accent = Of(ManifoldTheme::Accent);
                const double phase = std::fmod(seconds / 1.4, 1.0);
                const double eased = phase < 0.5 ? 2.0 * phase * phase : 1.0 - std::pow(-2.0 * phase + 2.0, 2.0) * 0.5;
                const float span = right - left;
                const float segment = span * 0.32f;
                const float segmentLeft = left - segment + (span + segment) * static_cast<float>(eased);
                const float segmentRight = segmentLeft + segment;
                for (int y = static_cast<int>(barTop); y < static_cast<int>(barTop + barHeight); ++y)
                {
                    for (int x = static_cast<int>(left); x < static_cast<int>(right); ++x)
                    {
                        const float lit = Clamp01(std::min(static_cast<float>(x + 1), segmentRight) - std::max(static_cast<float>(x), segmentLeft));
                        Blend(target, x, y, Mix(track, accent, lit), 1.0f);
                    }
                }
            }

            int Width = 0;
            int Height = 0;

        private:
            void Blend(std::uint32_t* target, int x, int y, Rgb color, float alpha) const
            {
                if (x < 0 || y < 0 || x >= Width || y >= Height || alpha <= 0.0f)
                    return;
                std::uint32_t& pixel = target[static_cast<std::size_t>(y) * static_cast<std::size_t>(Width) + static_cast<std::size_t>(x)];
                const float b = static_cast<float>(pixel & 0xFF);
                const float g = static_cast<float>((pixel >> 8) & 0xFF);
                const float r = static_cast<float>((pixel >> 16) & 0xFF);
                const std::uint32_t a = pixel >> 24;
                const float keep = 1.0f - alpha;
                const auto channel = [&](float base, float value) { return static_cast<std::uint32_t>(std::lround(std::clamp(base * keep + value * 255.0f * alpha * (static_cast<float>(a) / 255.0f), 0.0f, 255.0f))); };
                pixel = channel(b, color.B) | (channel(g, color.G) << 8) | (channel(r, color.R) << 16) | (a << 24);
            }

            void Stamp(std::uint32_t* target, const Mask& mask, int left, int top, Rgb color, float opacity) const
            {
                for (int y = 0; y < mask.Height; ++y)
                {
                    for (int x = 0; x < mask.Width; ++x)
                    {
                        const std::uint8_t coverage = mask.Coverage[static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.Width) + static_cast<std::size_t>(x)];
                        if (coverage)
                            Blend(target, left + x, top + y, color, opacity * static_cast<float>(coverage) / 255.0f);
                    }
                }
            }

            void StampCentered(std::uint32_t* target, const Mask& mask, float centerX, int top, Rgb color) const
            {
                Stamp(target, mask, static_cast<int>(std::lround(centerX - static_cast<float>(mask.Width) * 0.5f)), top, color, 1.0f);
            }

            void BuildBase(HINSTANCE instance)
            {
                m_cardColor = Mix(Of(ManifoldTheme::Card), Of(ManifoldTheme::Text), 0.025f);
                const Rgb border = Of(ManifoldTheme::Border);
                const float borderAlpha = ManifoldTheme::Border.w;
                const Rgb accent = Of(ManifoldTheme::Accent);
                const float centerX = m_margin + m_cardWidth * 0.5f;
                const float centerY = m_margin + m_cardHeight * 0.5f;
                const float halfWidth = m_cardWidth * 0.5f;
                const float halfHeight = m_cardHeight * 0.5f;
                const float radius = std::round(kRadius * m_scale);
                const float borderWidth = std::max(1.0f, std::round(m_scale));
                const float blur = 18.0f * m_scale;
                const float shadowOffset = 5.0f * m_scale;
                const float iconSize = std::round(64.0f * m_scale);
                const float iconTop = m_margin + std::round(30.0f * m_scale);
                const float glowX = centerX;
                const float glowY = iconTop + iconSize * 0.5f;
                const float glowRadius = 150.0f * m_scale;

                std::vector<Pixel> pixels(static_cast<std::size_t>(Width) * static_cast<std::size_t>(Height));
                for (int y = 0; y < Height; ++y)
                {
                    for (int x = 0; x < Width; ++x)
                    {
                        const float px = static_cast<float>(x) + 0.5f;
                        const float py = static_cast<float>(y) + 0.5f;
                        const float distance = RoundedBox(px, py, centerX, centerY, halfWidth, halfHeight, radius);
                        const float shadowDistance = RoundedBox(px, py, centerX, centerY + shadowOffset, halfWidth, halfHeight, radius);
                        const float shadow = (ManifoldTheme::IsLight() ? 0.16f : 0.32f) * std::pow(Clamp01(1.0f - std::max(shadowDistance, 0.0f) / blur), 2.2f);
                        const float cover = Clamp01(0.5f - distance);
                        Rgb color = m_cardColor;
                        const float glowDistance = std::sqrt((px - glowX) * (px - glowX) + (py - glowY) * (py - glowY));
                        color = Mix(color, accent, 0.075f * std::pow(Clamp01(1.0f - glowDistance / glowRadius), 2.0f));
                        if (cover > 0.0f)
                        {
                            const float ring = cover - Clamp01(0.5f - (distance + borderWidth));
                            color = Mix(color, border, Clamp01(ring / cover) * borderAlpha);
                        }
                        Pixel& out = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(Width) + static_cast<std::size_t>(x)];
                        out.A = cover + shadow * (1.0f - cover);
                        out.R = color.R * cover;
                        out.G = color.G * cover;
                        out.B = color.B * cover;
                    }
                }

                const IconImage icon = LoadIconImage(instance, static_cast<int>(iconSize));
                const int iconLeft = static_cast<int>(std::lround(centerX - iconSize * 0.5f));
                for (int y = 0; y < icon.Size; ++y)
                {
                    for (int x = 0; x < icon.Size; ++x)
                    {
                        const std::uint8_t* source = &icon.Bgra[(static_cast<std::size_t>(y) * static_cast<std::size_t>(icon.Size) + static_cast<std::size_t>(x)) * 4];
                        const float alpha = static_cast<float>(source[3]) / 255.0f;
                        const int tx = iconLeft + x;
                        const int ty = static_cast<int>(iconTop) + y;
                        if (alpha <= 0.0f || tx < 0 || ty < 0 || tx >= Width || ty >= Height)
                            continue;
                        Pixel& out = pixels[static_cast<std::size_t>(ty) * static_cast<std::size_t>(Width) + static_cast<std::size_t>(tx)];
                        const float keep = 1.0f - alpha;
                        out.B = out.B * keep + static_cast<float>(source[0]) / 255.0f * alpha;
                        out.G = out.G * keep + static_cast<float>(source[1]) / 255.0f * alpha;
                        out.R = out.R * keep + static_cast<float>(source[2]) / 255.0f * alpha;
                        out.A = out.A * keep + alpha;
                    }
                }

                m_base.resize(pixels.size());
                for (std::size_t i = 0; i < pixels.size(); ++i)
                {
                    const auto byte = [](float value) { return static_cast<std::uint32_t>(std::lround(Clamp01(value) * 255.0f)); };
                    m_base[i] = byte(pixels[i].B) | (byte(pixels[i].G) << 8) | (byte(pixels[i].R) << 16) | (byte(pixels[i].A) << 24);
                }

                const Mask title = RenderText(m_title, L"Manifold Engine");
                const Mask tagline = RenderText(m_body, L"Memory scanner, debugger, assembler and Lua scripting");
                const Mask version = RenderText(m_small, Utf8ToWide(std::string("Version ") + MANIFOLD_VERSION_TEXT));
                int y = static_cast<int>(iconTop + iconSize + std::round(16.0f * m_scale));
                StampCentered(m_base.data(), title, centerX, y, Of(ManifoldTheme::Text));
                y += title.Height + static_cast<int>(std::round(4.0f * m_scale));
                StampCentered(m_base.data(), tagline, centerX, y, Of(ManifoldTheme::Label));
                const float right = m_margin + m_cardWidth - std::round(28.0f * m_scale);
                const float barTop = m_margin + m_cardHeight - std::round(26.0f * m_scale);
                const int versionTop = static_cast<int>(barTop - std::round(10.0f * m_scale)) - version.Height;
                Stamp(m_base.data(), version, static_cast<int>(right) - version.Width, versionTop, Of(ManifoldTheme::Muted), 0.85f);
            }

            float m_scale = 1.0f;
            float m_margin = 0.0f;
            float m_cardWidth = 0.0f;
            float m_cardHeight = 0.0f;
            HFONT m_title = nullptr;
            HFONT m_body = nullptr;
            HFONT m_small = nullptr;
            Rgb m_cardColor;
            std::vector<std::uint32_t> m_base;
            Mask m_status;
        };

        LRESULT CALLBACK SplashProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_NCHITTEST)
                return HTTRANSPARENT;
            if (message == WM_MOUSEACTIVATE)
                return MA_NOACTIVATE;
            return ::DefWindowProcW(window, message, wParam, lParam);
        }
    }

    std::unique_ptr<SplashScreen> SplashScreen::Show(HINSTANCE instance, HWND mainWindow, bool maximised)
    {
        RECT target{};
        if (!mainWindow || !::GetWindowRect(mainWindow, &target))
            return nullptr;
        const HMONITOR monitor = ::MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info), {}, {}, 0 };
        if (!::GetMonitorInfoW(monitor, &info))
            return nullptr;
        RECT area = maximised ? info.rcWork : target;
        RECT visible{};
        if (!::IntersectRect(&visible, &area, &info.rcWork))
            area = info.rcWork;
        else
            area = visible;
        std::unique_ptr<SplashScreen> splash(new SplashScreen());
        const UINT dpi = MonitorDpi(monitor);
        splash->m_thread = std::thread(&SplashScreen::Run, splash.get(), instance, area, dpi);
        return splash;
    }

    SplashScreen::~SplashScreen()
    {
        Close(true);
        if (m_thread.joinable())
            m_thread.join();
    }

    void SplashScreen::SetStatus(std::string text)
    {
        std::lock_guard lock(m_lock);
        m_status = std::move(text);
        m_statusChanged = true;
    }

    void SplashScreen::Close(bool immediately)
    {
        int wanted = immediately ? kCloseNow : kCloseFade;
        int current = m_close.load();
        while (current < wanted && !m_close.compare_exchange_weak(current, wanted))
        {
        }
        if (HWND window = m_window.load())
            ::PostMessageW(window, WM_NULL, 0, 0);
    }

    void SplashScreen::Run(HINSTANCE instance, RECT area, UINT dpi)
    {
        SetThreadName(L"Splash screen");
        static const ATOM registered = [instance]
            {
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = &SplashProc;
                wc.hInstance = instance;
                wc.hCursor = ::LoadCursorW(nullptr, IDC_APPSTARTING);
                wc.lpszClassName = kClassName;
                return ::RegisterClassExW(&wc);
            }();
        if (!registered)
        {
            m_finished = true;
            return;
        }

        const float scale = static_cast<float>(dpi) / 96.0f;
        Painter painter(instance, scale);
        {
            std::lock_guard lock(m_lock);
            painter.SetStatus(m_status.empty() ? std::string("Starting") : m_status);
            m_statusChanged = false;
        }
        const int width = painter.Width;
        const int height = painter.Height;
        const int x = std::clamp(static_cast<int>((area.left + area.right - width) / 2), static_cast<int>(area.left), std::max(static_cast<int>(area.left), static_cast<int>(area.right) - width));
        const int y = std::clamp(static_cast<int>((area.top + area.bottom - height) / 2), static_cast<int>(area.top), std::max(static_cast<int>(area.top), static_cast<int>(area.bottom) - height));

        HWND window = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName, L"Manifold Engine", WS_POPUP, x, y,
            width, height, nullptr, nullptr, instance, nullptr);
        if (!window)
        {
            m_finished = true;
            return;
        }
        m_window = window;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC screen = ::GetDC(nullptr);
        HDC memory = ::CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ previous = bitmap ? ::SelectObject(memory, bitmap) : nullptr;

        using Clock = std::chrono::steady_clock;
        const Clock::time_point started = Clock::now();
        Clock::time_point closing{};
        float closingFrom = 1.0f;
        bool shown = false;
        bool done = !bits;

        const auto present = [&](float alpha)
            {
                painter.Paint(static_cast<std::uint32_t*>(bits), std::chrono::duration<double>(Clock::now() - started).count());
                POINT origin{ x, y };
                SIZE size{ width, height };
                POINT source{ 0, 0 };
                BLENDFUNCTION blend{ AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(Clamp01(alpha) * 255.0f)), AC_SRC_ALPHA };
                ::UpdateLayeredWindow(window, screen, &origin, &size, memory, &source, 0, &blend, ULW_ALPHA);
            };

        const auto step = [&]()
            {
                {
                    std::lock_guard lock(m_lock);
                    if (m_statusChanged)
                    {
                        painter.SetStatus(m_status);
                        m_statusChanged = false;
                    }
                }
                const Clock::time_point now = Clock::now();
                const float fadeIn = static_cast<float>(Clamp01(static_cast<float>(std::chrono::duration<double>(now - started).count() / kFadeInSeconds)));
                const int close = m_close.load();
                if (close == kCloseNow)
                {
                    done = true;
                    return;
                }
                float alpha = fadeIn;
                if (close == kCloseFade)
                {
                    if (closing == Clock::time_point{})
                    {
                        closing = now;
                        closingFrom = fadeIn;
                    }
                    const float out = static_cast<float>(std::chrono::duration<double>(now - closing).count() / kFadeOutSeconds);
                    alpha = closingFrom * (1.0f - Clamp01(out));
                    if (out >= 1.0f || !shown)
                    {
                        done = true;
                        return;
                    }
                }
                present(alpha);
                if (!shown)
                {
                    ::ShowWindow(window, SW_SHOWNOACTIVATE);
                    shown = true;
                }
            };

        step();
        ::SetTimer(window, kFrameTimer, kFrameMilliseconds, nullptr);
        MSG message{};
        while (!done && ::GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.hwnd == window && (message.message == WM_TIMER || message.message == WM_NULL))
            {
                step();
                continue;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::KillTimer(window, kFrameTimer);
        m_window = nullptr;
        ::DestroyWindow(window);
        if (previous)
            ::SelectObject(memory, previous);
        if (bitmap)
            ::DeleteObject(bitmap);
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
        m_finished = true;
    }
}
