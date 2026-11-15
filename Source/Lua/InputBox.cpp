#include "Lua/InputBox.h"

#include "Core/Text.h"

#include <algorithm>

#include <windows.h>

namespace Manifold::Lua
{
    namespace
    {
        constexpr int kEditId = 100;
        constexpr int kOkId = IDOK;
        constexpr int kCancelId = IDCANCEL;
        constexpr const wchar_t* kClassName = L"ManifoldInputBox";

        struct Box
        {
            std::wstring Prompt;
            std::wstring Value;
            bool Accepted = false;
            bool Done = false;
            HWND Edit = nullptr;
            HFONT Font = nullptr;
        };

        DialogProvider& Provider()
        {
            static DialogProvider provider;
            return provider;
        }

        bool ProviderReady()
        {
            return !Provider().Available || Provider().Available();
        }

        LRESULT CALLBACK BoxProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            Box* box = reinterpret_cast<Box*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
            switch (message)
            {
            case WM_NCCREATE:
            {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
                break;
            }
            case WM_CREATE:
            {
                box = reinterpret_cast<Box*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                const UINT dpi = ::GetDpiForWindow(window);
                const auto scaled = [&](int value) { return ::MulDiv(value, static_cast<int>(dpi), 96); };
                NONCLIENTMETRICSW metrics{ sizeof(metrics) };
                ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
                box->Font = ::CreateFontIndirectW(&metrics.lfMessageFont);
                HWND label = ::CreateWindowExW(0, L"STATIC", box->Prompt.c_str(), WS_CHILD | WS_VISIBLE, scaled(12), scaled(12), scaled(356), scaled(36), window, nullptr, nullptr, nullptr);
                box->Edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", box->Value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, scaled(12), scaled(52), scaled(356),
                    scaled(24), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), nullptr, nullptr);
                HWND ok = ::CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, scaled(206), scaled(88), scaled(78), scaled(26), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOkId)), nullptr, nullptr);
                HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, scaled(290), scaled(88), scaled(78), scaled(26), window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelId)), nullptr, nullptr);
                for (HWND child : { label, box->Edit, ok, cancel })
                    ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(box->Font), TRUE);
                ::SetFocus(box->Edit);
                ::SendMessageW(box->Edit, EM_SETSEL, 0, -1);
                return 0;
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == kOkId || LOWORD(wParam) == kCancelId)
                {
                    if (LOWORD(wParam) == kOkId)
                    {
                        const int length = ::GetWindowTextLengthW(box->Edit);
                        box->Value.assign(static_cast<std::size_t>(length) + 1, L'\0');
                        ::GetWindowTextW(box->Edit, box->Value.data(), length + 1);
                        box->Value.resize(static_cast<std::size_t>(length));
                        box->Accepted = true;
                    }
                    box->Done = true;
                    return 0;
                }
                break;
            case WM_CLOSE:
                box->Done = true;
                return 0;
            case WM_DESTROY:
                if (box && box->Font)
                    ::DeleteObject(box->Font);
                return 0;
            default:
                break;
            }
            return ::DefWindowProcW(window, message, wParam, lParam);
        }

        bool NativeText(const std::string& caption, const std::string& prompt, std::string& value)
        {
            static bool registered = false;
            HINSTANCE instance = ::GetModuleHandleW(nullptr);
            if (!registered)
            {
                WNDCLASSEXW wc{ sizeof(wc) };
                wc.lpfnWndProc = &BoxProc;
                wc.hInstance = instance;
                wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
                wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
                wc.lpszClassName = kClassName;
                registered = ::RegisterClassExW(&wc) != 0;
            }
            Box box;
            box.Prompt = Utf8ToWide(prompt);
            box.Value = Utf8ToWide(value);
            HWND owner = ::GetActiveWindow();
            const UINT dpi = owner ? ::GetDpiForWindow(owner) : 96;
            RECT frame{ 0, 0, ::MulDiv(380, static_cast<int>(dpi), 96), ::MulDiv(126, static_cast<int>(dpi), 96) };
            ::AdjustWindowRectExForDpi(&frame, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
            POINT cursor{};
            ::GetCursorPos(&cursor);
            HWND window = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kClassName, Utf8ToWide(caption).c_str(), WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                cursor.x - (frame.right - frame.left) / 2, cursor.y - (frame.bottom - frame.top) / 2, frame.right - frame.left, frame.bottom - frame.top, owner, nullptr, instance, &box);
            if (!window)
                return false;
            if (owner)
                ::EnableWindow(owner, FALSE);
            MSG message{};
            while (!box.Done && ::GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
                    box.Done = true;
                if (!::IsDialogMessageW(window, &message))
                {
                    ::TranslateMessage(&message);
                    ::DispatchMessageW(&message);
                }
            }
            if (owner)
            {
                ::EnableWindow(owner, TRUE);
                ::SetForegroundWindow(owner);
            }
            ::DestroyWindow(window);
            if (box.Accepted)
                value = WideToUtf8(box.Value);
            return box.Accepted;
        }
    }

    void SetDialogProvider(DialogProvider provider)
    {
        Provider() = std::move(provider);
    }

    void ClearDialogProvider()
    {
        Provider() = {};
    }

    int ButtonResult(int button)
    {
        switch (button)
        {
        case ButtonYes: return ResultYes;
        case ButtonNo: return ResultNo;
        case ButtonOk: return ResultOk;
        case ButtonCancel: return ResultCancel;
        case ButtonAbort: return ResultAbort;
        case ButtonRetry: return ResultRetry;
        case ButtonIgnore: return ResultIgnore;
        case ButtonAll: return ResultAll;
        case ButtonNoToAll: return ResultNoToAll;
        case ButtonYesToAll: return ResultYesToAll;
        case ButtonClose: return ResultClose;
        default: return ResultNone;
        }
    }

    const char* ButtonCaption(int button)
    {
        switch (button)
        {
        case ButtonYes: return "Yes";
        case ButtonNo: return "No";
        case ButtonOk: return "OK";
        case ButtonCancel: return "Cancel";
        case ButtonAbort: return "Abort";
        case ButtonRetry: return "Retry";
        case ButtonIgnore: return "Ignore";
        case ButtonAll: return "All";
        case ButtonNoToAll: return "No to all";
        case ButtonYesToAll: return "Yes to all";
        case ButtonHelp: return "Help";
        case ButtonClose: return "Close";
        default: return "OK";
        }
    }

    bool AskText(const std::string& caption, const std::string& prompt, std::string& value)
    {
        if (Provider().AskText && ProviderReady())
        {
            const std::optional<std::string> answer = Provider().AskText(caption, prompt, value);
            if (answer)
                value = *answer;
            return answer.has_value();
        }
        return NativeText(caption, prompt, value);
    }

    int AskMessage(const std::string& title, const std::string& text, MessageKind kind, std::vector<int> buttons)
    {
        if (buttons.empty())
            buttons.push_back(ButtonOk);
        if (Provider().Message && ProviderReady())
            return Provider().Message(title, text, kind, buttons);
        const auto has = [&](int button) { return std::find(buttons.begin(), buttons.end(), button) != buttons.end(); };
        UINT flags = MB_TOPMOST;
        flags |= kind == MessageKind::Warning ? MB_ICONWARNING : kind == MessageKind::Error ? MB_ICONERROR : kind == MessageKind::Confirmation ? MB_ICONQUESTION :
                 kind == MessageKind::Information ? MB_ICONINFORMATION : 0;
        flags |= has(ButtonYes) && has(ButtonNo) ? (has(ButtonCancel) ? MB_YESNOCANCEL : MB_YESNO) : has(ButtonCancel) ? MB_OKCANCEL : MB_OK;
        switch (::MessageBoxW(::GetActiveWindow(), Utf8ToWide(text).c_str(), Utf8ToWide(title).c_str(), flags))
        {
        case IDYES: return ResultYes;
        case IDNO: return ResultNo;
        case IDCANCEL: return ResultCancel;
        default: return ResultOk;
        }
    }

    std::pair<int, std::string> AskSelection(const std::string& title, const std::string& caption, const std::vector<std::string>& items, bool allowCustom)
    {
        if (Provider().Select && ProviderReady())
            return Provider().Select(title, caption, items, allowCustom);
        return { -1, "" };
    }
}
