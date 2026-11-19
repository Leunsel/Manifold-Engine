#include "Lua/LuaCompatGaps.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Asm/Assembler.h"
#include "Core/Text.h"
#include "Core/Types.h"
#include "Lua/LuaApi.h"
#include "Lua/LuaArgs.h"
#include "Lua/LuaEngine.h"
#include "Table/AddressList.h"
#include "Table/CheatTable.h"

#include "lua.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <windows.h>
#include <intrin.h>
#include <mmsystem.h>
#include <sapi.h>
#include <xinput.h>

#pragma comment(lib, "winmm.lib")

namespace Manifold::Lua
{
    namespace
    {
        constexpr const char* kAssemblerModeKey = "Manifold.AssemblerMode";
        constexpr int kKeyPressMs = 50;

        void PushBytes(lua_State* state, const std::vector<std::uint8_t>& bytes)
        {
            lua_createtable(state, static_cast<int>(bytes.size()), 0);
            for (std::size_t i = 0; i < bytes.size(); ++i)
            {
                lua_pushinteger(state, bytes[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
        }

        void SetField(lua_State* state, const char* key, lua_Integer value)
        {
            lua_pushinteger(state, value);
            lua_setfield(state, -2, key);
        }

        void SetField(lua_State* state, const char* key, const std::string& value)
        {
            lua_pushlstring(state, value.data(), value.size());
            lua_setfield(state, -2, key);
        }

        void SetFlag(lua_State* state, const char* key, bool value)
        {
            lua_pushboolean(state, value ? 1 : 0);
            lua_setfield(state, -2, key);
        }

        std::string LuaText(lua_State* state, int index)
        {
            std::size_t length = 0;
            const char* text = luaL_checklstring(state, index, &length);
            return std::string(text, length);
        }

        int PushStatus(lua_State* state, const Status& status)
        {
            if (status)
            {
                lua_pushboolean(state, 1);
                return 1;
            }
            lua_pushnil(state);
            lua_pushstring(state, status.Text.c_str());
            return 2;
        }

        int Integer(lua_State* state, int index)
        {
            return static_cast<int>(luaL_checknumber(state, index));
        }

        int SetMousePos(lua_State* state)
        {
            ::SetCursorPos(Integer(state, 1), Integer(state, 2));
            return 0;
        }

        WORD KeyArgument(lua_State* state, int index)
        {
            if (lua_type(state, index) == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char* text = lua_tolstring(state, index, &length);
                if (length == 1)
                    return static_cast<WORD>(std::toupper(static_cast<unsigned char>(text[0])));
            }
            return static_cast<WORD>(luaL_checkinteger(state, index));
        }

        bool ExtendedKey(WORD key)
        {
            switch (key)
            {
            case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU:
            case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT: case VK_LWIN: case VK_RWIN: case VK_APPS:
                return true;
            default:
                return false;
            }
        }

        void SendKey(WORD key, bool up)
        {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = key;
            input.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
            input.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ExtendedKey(key) ? KEYEVENTF_EXTENDEDKEY : 0);
            ::SendInput(1, &input, sizeof(input));
        }

        int KeyDown(lua_State* state)
        {
            SendKey(KeyArgument(state, 1), false);
            return 0;
        }

        int KeyUp(lua_State* state)
        {
            SendKey(KeyArgument(state, 1), true);
            return 0;
        }

        int KeyPressDone(lua_State*, int, lua_KContext context)
        {
            SendKey(static_cast<WORD>(context), true);
            return 0;
        }

        int DoKeyPress(lua_State* state)
        {
            const WORD key = KeyArgument(state, 1);
            SendKey(key, false);
            LuaEngine& engine = EngineOf(state);
            if (lua_isyieldable(state) && engine.IsCooperative(state))
            {
                lua_pushinteger(state, kKeyPressMs);
                return lua_yieldk(state, 1, static_cast<lua_KContext>(key), &KeyPressDone);
            }
            engine.BlockingSleep(kKeyPressMs);
            SendKey(key, true);
            return 0;
        }

        int MouseEvent(lua_State* state)
        {
            ::mouse_event(static_cast<DWORD>(luaL_checkinteger(state, 1)), static_cast<DWORD>(luaL_optinteger(state, 2, 0)), static_cast<DWORD>(luaL_optinteger(state, 3, 0)),
                static_cast<DWORD>(luaL_optinteger(state, 4, 0)), static_cast<ULONG_PTR>(luaL_optinteger(state, 5, 0)));
            return 0;
        }

        int GetPixelCompat(lua_State* state)
        {
            HDC screen = ::GetDC(nullptr);
            const COLORREF color = ::GetPixel(screen, Integer(state, 1), Integer(state, 2));
            ::ReleaseDC(nullptr, screen);
            if (color == CLR_INVALID)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, static_cast<lua_Integer>(color));
            return 1;
        }

        int SystemMetrics(lua_State* state)
        {
            lua_pushinteger(state, ::GetSystemMetrics(static_cast<int>(luaL_checkinteger(state, 1))));
            return 1;
        }

        int BeepCompat(lua_State*)
        {
            ::MessageBeep(MB_OK);
            return 0;
        }

        int OutputDebugText(lua_State* state)
        {
            ::OutputDebugStringW(Utf8ToWide(LuaText(state, 1)).c_str());
            return 0;
        }

        int CpuCount(lua_State* state)
        {
            SYSTEM_INFO info{};
            ::GetSystemInfo(&info);
            lua_pushinteger(state, static_cast<lua_Integer>(info.dwNumberOfProcessors));
            return 1;
        }

        int CpuId(lua_State* state)
        {
            int registers[4]{};
            __cpuidex(registers, static_cast<int>(luaL_checkinteger(state, 1)), static_cast<int>(luaL_optinteger(state, 2, 0)));
            const char* names[4][2] = { { "EAX", "eax" }, { "EBX", "ebx" }, { "ECX", "ecx" }, { "EDX", "edx" } };
            lua_createtable(state, 0, 8);
            for (int i = 0; i < 4; ++i)
            {
                SetField(state, names[i][0], static_cast<lua_Integer>(static_cast<std::uint32_t>(registers[i])));
                SetField(state, names[i][1], static_cast<lua_Integer>(static_cast<std::uint32_t>(registers[i])));
            }
            return 1;
        }

        int PointerSize(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            lua_pushinteger(state, process ? process->PointerSize() : 8);
            return 1;
        }

        int FileVersion(lua_State* state)
        {
            constexpr std::uint64_t major = 7, minor = 5, release = 0, build = 7431;
            lua_pushinteger(state, static_cast<lua_Integer>((major << 48) | (minor << 32) | (release << 16) | build));
            lua_createtable(state, 0, 4);
            SetField(state, "major", static_cast<lua_Integer>(major));
            SetField(state, "minor", static_cast<lua_Integer>(minor));
            SetField(state, "release", static_cast<lua_Integer>(release));
            SetField(state, "build", static_cast<lua_Integer>(build));
            return 2;
        }

        int UserEnvironment(lua_State* state)
        {
            const std::wstring name = Utf8ToWide(LuaText(state, 1));
            DWORD bytes = 0;
            constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
            if (::RegGetValueW(HKEY_CURRENT_USER, L"Environment", name.c_str(), flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
            {
                lua_pushnil(state);
                return 1;
            }
            std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
            bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            if (::RegGetValueW(HKEY_CURRENT_USER, L"Environment", name.c_str(), flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
            {
                lua_pushnil(state);
                return 1;
            }
            value.resize(::wcsnlen(value.c_str(), value.size()));
            lua_pushstring(state, WideToUtf8(value).c_str());
            return 1;
        }

        int SetUserEnvironment(lua_State* state)
        {
            const std::wstring name = Utf8ToWide(LuaText(state, 1));
            const std::wstring value = Utf8ToWide(LuaText(state, 2));
            const LSTATUS result = ::RegSetKeyValueW(HKEY_CURRENT_USER, L"Environment", name.c_str(), REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            lua_pushboolean(state, result == ERROR_SUCCESS);
            return 1;
        }

        int SendMessageCompat(lua_State* state)
        {
            const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(luaL_checkinteger(state, 1)));
            const LRESULT result = ::SendMessageW(window, static_cast<UINT>(luaL_checkinteger(state, 2)), static_cast<WPARAM>(luaL_optinteger(state, 3, 0)), static_cast<LPARAM>(luaL_optinteger(state, 4, 0)));
            lua_pushinteger(state, static_cast<lua_Integer>(result));
            return 1;
        }

        int InvertColor(lua_State* state)
        {
            lua_pushinteger(state, (~luaL_checkinteger(state, 1)) & 0xFFFFFF);
            return 1;
        }

        int WindowList(lua_State* state)
        {
            struct Window
            {
                DWORD Process = 0;
                HWND Handle = nullptr;
                std::string Caption;
            };
            std::vector<Window> windows;
            ::EnumWindows([](HWND handle, LPARAM user) -> BOOL
                {
                    if (!::IsWindowVisible(handle))
                        return TRUE;
                    const int length = ::GetWindowTextLengthW(handle);
                    if (length <= 0)
                        return TRUE;
                    std::wstring caption(static_cast<std::size_t>(length) + 1, L'\0');
                    caption.resize(static_cast<std::size_t>(::GetWindowTextW(handle, caption.data(), length + 1)));
                    Window window;
                    ::GetWindowThreadProcessId(handle, &window.Process);
                    window.Handle = handle;
                    window.Caption = WideToUtf8(caption);
                    reinterpret_cast<std::vector<Window>*>(user)->push_back(std::move(window));
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&windows));
            lua_createtable(state, static_cast<int>(windows.size()), 0);
            for (std::size_t i = 0; i < windows.size(); ++i)
            {
                lua_createtable(state, 0, 3);
                SetField(state, "pid", static_cast<lua_Integer>(windows[i].Process));
                SetField(state, "window", static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(windows[i].Handle)));
                SetField(state, "caption", windows[i].Caption);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int ThreadList(lua_State* state)
        {
            std::vector<std::uint32_t> threads;
            if (const auto& process = Owner(state).Process())
                threads = process->Threads();
            lua_createtable(state, static_cast<int>(threads.size()), 0);
            for (std::size_t i = 0; i < threads.size(); ++i)
            {
                lua_pushinteger(state, threads[i]);
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int FileExists(lua_State* state)
        {
            std::error_code ignored;
            lua_pushboolean(state, std::filesystem::is_regular_file(Utf8ToWide(LuaText(state, 1)), ignored));
            return 1;
        }

        int DeleteFileCompat(lua_State* state)
        {
            const std::wstring path = Utf8ToWide(LuaText(state, 1));
            std::error_code ignored;
            if (!std::filesystem::is_regular_file(path, ignored))
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            lua_pushboolean(state, ::DeleteFileW(path.c_str()) != 0);
            return 1;
        }

        std::wstring FromCodePage(std::string_view text, UINT codePage)
        {
            if (text.empty())
                return {};
            const int count = ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(std::max(count, 0)), L'\0');
            if (count > 0)
                ::MultiByteToWideChar(codePage, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
            return wide;
        }

        int AnsiToUtf8(lua_State* state)
        {
            const std::string text = WideToUtf8(FromCodePage(LuaText(state, 1), CP_ACP));
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int Utf8ToAnsi(lua_State* state)
        {
            const std::wstring wide = FromCodePage(LuaText(state, 1), CP_UTF8);
            std::string text;
            if (!wide.empty())
            {
                const int count = ::WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
                text.resize(static_cast<std::size_t>(std::max(count, 0)));
                if (count > 0)
                    ::WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), text.data(), count, nullptr, nullptr);
            }
            lua_pushlstring(state, text.data(), text.size());
            return 1;
        }

        int ByteTableToExtended(lua_State* state)
        {
            const std::vector<std::uint8_t> bytes = ByteTable(state, 1);
            if (bytes.size() < 10)
            {
                lua_pushnil(state);
                return 1;
            }
            std::uint64_t mantissa = 0;
            std::memcpy(&mantissa, bytes.data(), sizeof(mantissa));
            const int exponent = (bytes[8] | (bytes[9] << 8)) & 0x7FFF;
            const bool negative = (bytes[9] & 0x80) != 0;
            double value = 0.0;
            if (exponent == 0x7FFF)
                value = (mantissa << 1) == 0 ? HUGE_VAL : std::nan("");
            else if (mantissa != 0)
                value = std::ldexp(static_cast<double>(mantissa), (exponent == 0 ? 1 : exponent) - 16383 - 63);
            lua_pushnumber(state, negative ? -value : value);
            return 1;
        }

        int ExtendedToByteTable(lua_State* state)
        {
            const double value = luaL_checknumber(state, 1);
            std::uint64_t mantissa = 0;
            int exponent = 0;
            const bool negative = std::signbit(value);
            if (std::isnan(value))
            {
                mantissa = 0xC000000000000000ull;
                exponent = 0x7FFF;
            }
            else if (std::isinf(value))
            {
                mantissa = 0x8000000000000000ull;
                exponent = 0x7FFF;
            }
            else if (value != 0.0)
            {
                int binary = 0;
                const double fraction = std::frexp(std::fabs(value), &binary);
                mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
                exponent = binary - 1 + 16383;
            }
            std::vector<std::uint8_t> bytes(10, 0);
            std::memcpy(bytes.data(), &mantissa, sizeof(mantissa));
            bytes[8] = static_cast<std::uint8_t>(exponent & 0xFF);
            bytes[9] = static_cast<std::uint8_t>(((exponent >> 8) & 0x7F) | (negative ? 0x80 : 0));
            PushBytes(state, bytes);
            return 1;
        }

        int SignExtend(lua_State* state)
        {
            std::uint64_t value = static_cast<std::uint64_t>(luaL_checkinteger(state, 1));
            const int bit = std::clamp(static_cast<int>(luaL_checkinteger(state, 2)), 0, 63);
            if (bit < 63)
            {
                const std::uint64_t sign = 1ull << bit;
                const std::uint64_t mask = (sign << 1) - 1;
                value &= mask;
                if (value & sign)
                    value |= ~mask;
            }
            lua_pushinteger(state, static_cast<lua_Integer>(value));
            return 1;
        }

        int OpenedProcessHandle(lua_State* state)
        {
            const auto& process = Owner(state).Process();
            lua_pushinteger(state, process ? static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(process->Handle())) : 0);
            return 1;
        }

        HMODULE XInputModule()
        {
            static const HMODULE module = []() -> HMODULE
                {
                    for (const wchar_t* name : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
                    {
                        if (HMODULE loaded = ::LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                            return loaded;
                    }
                    return nullptr;
                }();
            return module;
        }

        int ControllerState(lua_State* state)
        {
            using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
            const HMODULE module = XInputModule();
            const auto get = module ? reinterpret_cast<GetState>(reinterpret_cast<void*>(::GetProcAddress(module, "XInputGetState"))) : nullptr;
            const DWORD controller = static_cast<DWORD>(luaL_optinteger(state, 1, 0));
            XINPUT_STATE input{};
            if (!get || controller > 3 || get(controller, &input) != ERROR_SUCCESS)
            {
                lua_pushnil(state);
                return 1;
            }
            const WORD buttons = input.Gamepad.wButtons;
            lua_createtable(state, 0, 22);
            SetField(state, "ControllerID", static_cast<lua_Integer>(controller));
            SetField(state, "PacketNumber", static_cast<lua_Integer>(input.dwPacketNumber));
            constexpr std::pair<const char*, WORD> kButtons[] = {
                { "GAMEPAD_DPAD_UP", XINPUT_GAMEPAD_DPAD_UP }, { "GAMEPAD_DPAD_DOWN", XINPUT_GAMEPAD_DPAD_DOWN },
                { "GAMEPAD_DPAD_LEFT", XINPUT_GAMEPAD_DPAD_LEFT }, { "GAMEPAD_DPAD_RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT },
                { "GAMEPAD_START", XINPUT_GAMEPAD_START }, { "GAMEPAD_BACK", XINPUT_GAMEPAD_BACK },
                { "GAMEPAD_LEFT_THUMB", XINPUT_GAMEPAD_LEFT_THUMB }, { "GAMEPAD_RIGHT_THUMB", XINPUT_GAMEPAD_RIGHT_THUMB },
                { "GAMEPAD_LEFT_SHOULDER", XINPUT_GAMEPAD_LEFT_SHOULDER }, { "GAMEPAD_RIGHT_SHOULDER", XINPUT_GAMEPAD_RIGHT_SHOULDER },
                { "GAMEPAD_A", XINPUT_GAMEPAD_A }, { "GAMEPAD_B", XINPUT_GAMEPAD_B }, { "GAMEPAD_X", XINPUT_GAMEPAD_X }, { "GAMEPAD_Y", XINPUT_GAMEPAD_Y },
            };
            for (const auto& [name, mask] : kButtons)
                SetFlag(state, name, (buttons & mask) != 0);
            SetField(state, "LeftTrigger", static_cast<lua_Integer>(input.Gamepad.bLeftTrigger));
            SetField(state, "RightTrigger", static_cast<lua_Integer>(input.Gamepad.bRightTrigger));
            SetField(state, "ThumbLeftX", static_cast<lua_Integer>(input.Gamepad.sThumbLX));
            SetField(state, "ThumbLeftY", static_cast<lua_Integer>(input.Gamepad.sThumbLY));
            SetField(state, "ThumbRightX", static_cast<lua_Integer>(input.Gamepad.sThumbRX));
            SetField(state, "ThumbRightY", static_cast<lua_Integer>(input.Gamepad.sThumbRY));
            return 1;
        }

        int ControllerVibration(lua_State* state)
        {
            using SetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
            const HMODULE module = XInputModule();
            const auto set = module ? reinterpret_cast<SetState>(reinterpret_cast<void*>(::GetProcAddress(module, "XInputSetState"))) : nullptr;
            XINPUT_VIBRATION vibration{};
            vibration.wLeftMotorSpeed = static_cast<WORD>(std::clamp<lua_Integer>(luaL_checkinteger(state, 2), 0, 65535));
            vibration.wRightMotorSpeed = static_cast<WORD>(std::clamp<lua_Integer>(luaL_checkinteger(state, 3), 0, 65535));
            const DWORD controller = static_cast<DWORD>(luaL_checkinteger(state, 1));
            lua_pushboolean(state, set && controller <= 3 && set(controller, &vibration) == ERROR_SUCCESS);
            return 1;
        }

        std::vector<int> KeysArgument(lua_State* state)
        {
            std::vector<int> keys;
            if (lua_istable(state, 1))
            {
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, 1));
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(state, 1, i);
                    if (lua_isinteger(state, -1) && lua_tointeger(state, -1) != 0)
                        keys.push_back(static_cast<int>(lua_tointeger(state, -1)));
                    lua_pop(state, 1);
                }
                return keys;
            }
            for (int i = 1; i <= lua_gettop(state); ++i)
            {
                if (lua_isinteger(state, i) && lua_tointeger(state, i) != 0)
                    keys.push_back(static_cast<int>(lua_tointeger(state, i)));
            }
            return keys;
        }

        int KeyComboText(lua_State* state)
        {
            lua_pushstring(state, Table::KeysText(KeysArgument(state)).c_str());
            return 1;
        }

        constexpr int kShortcutShift = 0x2000;
        constexpr int kShortcutCtrl = 0x4000;
        constexpr int kShortcutAlt = 0x8000;
        constexpr int kShortcutMeta = 0x1000;

        struct KeyName
        {
            int Key;
            const char* Name;
        };

        constexpr KeyName kKeyNames[] = {
            { VK_BACK, "BkSp" }, { VK_TAB, "Tab" }, { VK_RETURN, "Enter" }, { VK_ESCAPE, "Esc" }, { VK_SPACE, "Space" },
            { VK_PRIOR, "PgUp" }, { VK_NEXT, "PgDn" }, { VK_END, "End" }, { VK_HOME, "Home" }, { VK_LEFT, "Left" },
            { VK_UP, "Up" }, { VK_RIGHT, "Right" }, { VK_DOWN, "Down" }, { VK_INSERT, "Ins" }, { VK_DELETE, "Del" },
            { VK_MULTIPLY, "Num *" }, { VK_ADD, "Num +" }, { VK_SUBTRACT, "Num -" }, { VK_DECIMAL, "Num ." }, { VK_DIVIDE, "Num /" },
            { VK_PAUSE, "Pause" }, { VK_CAPITAL, "CapsLock" }, { VK_SNAPSHOT, "PrintScreen" }, { VK_APPS, "Menu" },
        };

        std::string ShortcutKeyName(int key)
        {
            if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
                return std::string(1, static_cast<char>(key));
            if (key >= VK_F1 && key <= VK_F24)
                return "F" + std::to_string(key - VK_F1 + 1);
            if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
                return "Num " + std::to_string(key - VK_NUMPAD0);
            for (const KeyName& name : kKeyNames)
            {
                if (name.Key == key)
                    return name.Name;
            }
            return key ? "Key " + std::to_string(key) : std::string();
        }

        int ShortcutToText(lua_State* state)
        {
            const int shortcut = static_cast<int>(luaL_checkinteger(state, 1));
            std::string text;
            if (shortcut & kShortcutShift)
                text += "Shift+";
            if (shortcut & kShortcutCtrl)
                text += "Ctrl+";
            if (shortcut & kShortcutAlt)
                text += "Alt+";
            if (shortcut & kShortcutMeta)
                text += "Meta+";
            const std::string key = ShortcutKeyName(shortcut & 0xFF);
            lua_pushstring(state, key.empty() ? "" : (text + key).c_str());
            return 1;
        }

        int TextToShortcut(lua_State* state)
        {
            std::string rest = Trim(LuaText(state, 1));
            int modifiers = 0;
            for (;;)
            {
                const std::size_t plus = rest.find('+', 1);
                if (plus == std::string::npos)
                    break;
                const std::string part = Lower(Trim(rest.substr(0, plus)));
                if (part == "shift")
                    modifiers |= kShortcutShift;
                else if (part == "ctrl" || part == "control")
                    modifiers |= kShortcutCtrl;
                else if (part == "alt")
                    modifiers |= kShortcutAlt;
                else if (part == "meta" || part == "win")
                    modifiers |= kShortcutMeta;
                else
                    break;
                rest = rest.substr(plus + 1);
            }
            const std::string key = Lower(Trim(rest));
            int code = 0;
            if (key.size() == 1 && std::isalnum(static_cast<unsigned char>(key[0])))
                code = std::toupper(static_cast<unsigned char>(key[0]));
            else if (key.size() > 1 && key[0] == 'f' && std::all_of(key.begin() + 1, key.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            {
                const int number = std::atoi(key.c_str() + 1);
                if (number >= 1 && number <= 24)
                    code = VK_F1 + number - 1;
            }
            else
            {
                for (int candidate = 1; candidate < 256 && !code; ++candidate)
                {
                    if (Lower(ShortcutKeyName(candidate)) == key)
                        code = candidate;
                }
            }
            lua_pushinteger(state, code ? code | modifiers : 0);
            return 1;
        }

        std::optional<std::vector<std::uint8_t>> ReadLocalBytes(std::uint64_t address, std::size_t size)
        {
            std::vector<std::uint8_t> bytes(size);
            SIZE_T done = 0;
            if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), bytes.data(), size, &done) || done != size)
                return std::nullopt;
            return bytes;
        }

        bool WriteLocalBytes(std::uint64_t address, const std::vector<std::uint8_t>& bytes)
        {
            SIZE_T done = 0;
            return ::WriteProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPVOID>(address), bytes.data(), bytes.size(), &done) && done == bytes.size();
        }

        std::optional<std::vector<std::uint8_t>> ReadTargetBytes(Engine& engine, std::uint64_t address, std::size_t size)
        {
            const auto& process = engine.Process();
            if (!process)
                return std::nullopt;
            std::vector<std::uint8_t> bytes(size);
            if (!process->ReadExact(address, bytes.data(), size))
                return std::nullopt;
            return bytes;
        }

        int CopyMemoryCompat(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::uint64_t source = AddressArgument(state, 1);
            const auto size = static_cast<std::size_t>(std::max<lua_Integer>(luaL_checkinteger(state, 2), 0));
            const int method = static_cast<int>(luaL_optinteger(state, 4, 0));
            const bool fromTarget = method == 0 || method == 1;
            const bool toTarget = method == 0 || method == 2;
            const auto bytes = fromTarget ? ReadTargetBytes(engine, source, size) : ReadLocalBytes(source, size);
            if (!bytes)
            {
                lua_pushnil(state);
                lua_pushstring(state, "the source could not be read");
                return 2;
            }
            std::uint64_t destination = lua_isnoneornil(state, 3) ? 0 : AddressArgument(state, 3);
            if (!destination)
            {
                if (toTarget)
                {
                    if (!engine.Process())
                        return PushStatus(state, Status::Fail(ErrorCode::NoProcess));
                    const auto allocated = engine.Process()->Allocate(std::max<std::size_t>(size, 1), PAGE_EXECUTE_READWRITE);
                    if (!allocated)
                        return PushStatus(state, allocated.Error());
                    destination = *allocated;
                }
                else
                    destination = reinterpret_cast<std::uint64_t>(::VirtualAlloc(nullptr, std::max<std::size_t>(size, 1), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            }
            bool written = false;
            if (toTarget)
                written = engine.Process() && engine.Process()->Write(destination, bytes->data(), bytes->size()).Ok();
            else
                written = destination != 0 && WriteLocalBytes(destination, *bytes);
            if (!written)
            {
                lua_pushnil(state);
                lua_pushstring(state, "the destination could not be written");
                return 2;
            }
            lua_pushinteger(state, static_cast<lua_Integer>(destination));
            return 1;
        }

        int CompareMemory(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::uint64_t first = AddressArgument(state, 1);
            const std::uint64_t second = AddressArgument(state, 2);
            const auto size = static_cast<std::size_t>(std::max<lua_Integer>(luaL_checkinteger(state, 3), 0));
            const int method = static_cast<int>(luaL_optinteger(state, 4, 0));
            const auto left = method == 2 ? ReadLocalBytes(first, size) : ReadTargetBytes(engine, first, size);
            const auto right = method == 0 ? ReadTargetBytes(engine, second, size) : ReadLocalBytes(second, size);
            if (!left || !right)
            {
                lua_pushnil(state);
                lua_pushstring(state, "the memory could not be read");
                return 2;
            }
            const auto difference = std::mismatch(left->begin(), left->end(), right->begin());
            if (difference.first == left->end())
            {
                lua_pushboolean(state, 1);
                return 1;
            }
            lua_pushboolean(state, 0);
            lua_pushinteger(state, static_cast<lua_Integer>(difference.first - left->begin()));
            return 2;
        }

        bool Flag(lua_State* state, int table, const char* upper, const char* lower)
        {
            lua_getfield(state, table, upper);
            bool value = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            if (!value)
            {
                lua_getfield(state, table, lower);
                value = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
            }
            return value;
        }

        int SetMemoryProtection(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::uint64_t address = AddressArgument(state, 1);
            const auto size = static_cast<std::size_t>(std::max<lua_Integer>(luaL_checkinteger(state, 2), 1));
            luaL_checktype(state, 3, LUA_TTABLE);
            const bool read = Flag(state, 3, "R", "r");
            const bool write = Flag(state, 3, "W", "w");
            const bool execute = Flag(state, 3, "X", "x");
            DWORD protect = PAGE_NOACCESS;
            if (execute)
                protect = write ? PAGE_EXECUTE_READWRITE : read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
            else
                protect = write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
            lua_pushboolean(state, engine.Process() && engine.Process()->Protect(address, size, protect).has_value());
            return 1;
        }

        int ReadRegionFromFile(lua_State* state)
        {
            Engine& engine = Owner(state);
            std::ifstream file(Utf8ToWide(LuaText(state, 1)), std::ios::binary);
            if (!file)
            {
                lua_pushnil(state);
                lua_pushstring(state, "the file could not be read");
                return 2;
            }
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const std::uint64_t address = AddressArgument(state, 2);
            if (!engine.Process())
                return PushStatus(state, Status::Fail(ErrorCode::NoProcess));
            const Status status = engine.Process()->Write(address, bytes.data(), bytes.size());
            if (!status)
                return PushStatus(state, status);
            lua_pushinteger(state, static_cast<lua_Integer>(bytes.size()));
            return 1;
        }

        int WriteRegionToFile(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string path = LuaText(state, 1);
            const std::uint64_t address = AddressArgument(state, 2);
            const auto size = static_cast<std::size_t>(std::max<lua_Integer>(luaL_checkinteger(state, 3), 0));
            const auto& process = engine.Process();
            std::vector<std::uint8_t> bytes(size);
            const std::size_t read = process ? process->Read(address, bytes.data(), size) : 0;
            bytes.resize(read);
            std::ofstream file(Utf8ToWide(path), std::ios::binary | std::ios::trunc);
            if (!file)
            {
                lua_pushinteger(state, 0);
                return 1;
            }
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            lua_pushinteger(state, file ? static_cast<lua_Integer>(bytes.size()) : 0);
            return 1;
        }

        int SharedMemoryLocal(lua_State* state)
        {
            const std::wstring name = Utf8ToWide(LuaText(state, 1));
            const auto size = static_cast<std::uint64_t>(std::max<lua_Integer>(luaL_optinteger(state, 2, 4096), 1));
            HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), name.c_str());
            if (!mapping)
            {
                lua_pushnil(state);
                return 1;
            }
            void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, 0);
            if (!view)
            {
                ::CloseHandle(mapping);
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(view)));
            return 1;
        }

        int CreateRemoteThreadCompat(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::uint64_t address = AddressArgument(state, 1);
            const std::uint64_t parameter = lua_isnoneornil(state, 2) ? 0 : AddressArgument(state, 2);
            if (!engine.Process())
                return PushStatus(state, Status::Fail(ErrorCode::NoProcess));
            return PushStatus(state, engine.Process()->CreateThread(address, parameter, false));
        }

        int Note(lua_State* state, bool header)
        {
            Engine& engine = Owner(state);
            const std::string key = engine.Symbols().ModuleOffset(AddressArgument(state, 1));
            const auto found = engine.Table().DisasmNotes.find(key);
            if (found == engine.Table().DisasmNotes.end())
            {
                lua_pushstring(state, "");
                return 1;
            }
            lua_pushstring(state, (header ? found->second.Header : found->second.Comment).c_str());
            return 1;
        }

        int SetNote(lua_State* state, bool header)
        {
            Engine& engine = Owner(state);
            const std::string key = engine.Symbols().ModuleOffset(AddressArgument(state, 1));
            const std::string text = lua_isnoneornil(state, 2) ? std::string() : LuaText(state, 2);
            Table::AddressList::DisasmNote& note = engine.Table().DisasmNotes[key];
            (header ? note.Header : note.Comment) = text;
            if (note.Comment.empty() && note.Header.empty())
                engine.Table().DisasmNotes.erase(key);
            engine.Table().Touch();
            return 0;
        }

        int SymbolInfo(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string name = LuaText(state, 1);
            const auto address = engine.Evaluate(name);
            if (!address)
                return 0;
            std::string module;
            std::uint64_t size = 0;
            if (const auto& process = engine.Process())
            {
                if (const auto found = process->ModuleAt(*address))
                {
                    module = found->Name;
                    if (found->Base == *address && Lower(found->Name) == Lower(name))
                        size = found->Size;
                }
            }
            lua_createtable(state, 0, 4);
            SetField(state, "modulename", module);
            SetField(state, "searchkey", name);
            SetField(state, "address", static_cast<lua_Integer>(*address));
            SetField(state, "size", static_cast<lua_Integer>(size));
            return 1;
        }

        int RegisteredSymbols(lua_State* state)
        {
            const std::vector<Target::UserSymbol> symbols = Owner(state).Symbols().Registered();
            lua_createtable(state, static_cast<int>(symbols.size()), 0);
            for (std::size_t i = 0; i < symbols.size(); ++i)
            {
                lua_createtable(state, 0, 2);
                SetField(state, "symbolname", symbols[i].Name);
                SetField(state, "address", static_cast<lua_Integer>(symbols[i].Address));
                lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
            }
            return 1;
        }

        int ReinitializeSymbols(lua_State* state)
        {
            if (const auto& process = Owner(state).Process())
                process->RefreshModules();
            lua_pushboolean(state, 1);
            return 1;
        }

        std::string DemangleTypeName(std::string name)
        {
            if (name.rfind(".?A", 0) != 0 || name.size() < 5)
                return {};
            name = name.substr(4);
            if (name.size() >= 2 && name.compare(name.size() - 2, 2, "@@") == 0)
                name.resize(name.size() - 2);
            if (name.find('?') != std::string::npos || name.find('$') != std::string::npos)
                return name;
            std::vector<std::string> parts = Split(name, '@');
            std::string result;
            for (auto part = parts.rbegin(); part != parts.rend(); ++part)
            {
                if (part->empty())
                    continue;
                if (!result.empty())
                    result += "::";
                result += *part;
            }
            return result;
        }

        int RttiClassName(lua_State* state)
        {
            Engine& engine = Owner(state);
            const auto& process = engine.Process();
            const auto object = ResolveAddress(state, 1);
            if (!process || !object)
                return 0;
            const int pointer = process->PointerSize();
            const auto table = process->ReadPointer(*object);
            if (!table || *table < static_cast<std::uint64_t>(pointer))
                return 0;
            const auto locator = process->ReadPointer(*table - static_cast<std::uint64_t>(pointer));
            if (!locator)
                return 0;
            std::uint64_t descriptor = 0;
            std::uint64_t nameOffset = 0;
            if (pointer == 8)
            {
                const auto signature = process->ReadValue<std::uint32_t>(*locator);
                const auto typeRva = process->ReadValue<std::uint32_t>(*locator + 0x0C);
                const auto selfRva = process->ReadValue<std::uint32_t>(*locator + 0x14);
                if (!signature || *signature != 1 || !typeRva || !selfRva || *selfRva > *locator)
                    return 0;
                descriptor = *locator - *selfRva + *typeRva;
                nameOffset = 0x10;
            }
            else
            {
                const auto signature = process->ReadValue<std::uint32_t>(*locator);
                const auto type = process->ReadValue<std::uint32_t>(*locator + 0x0C);
                if (!signature || *signature != 0 || !type)
                    return 0;
                descriptor = *type;
                nameOffset = 0x08;
            }
            char raw[256]{};
            const std::size_t read = process->Read(descriptor + nameOffset, raw, sizeof(raw) - 1);
            if (read == 0)
                return 0;
            raw[std::min(read, sizeof(raw) - 1)] = '\0';
            const std::string name = DemangleTypeName(raw);
            if (name.empty())
                return 0;
            lua_pushstring(state, name.c_str());
            return 1;
        }

        int Assemble(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::string line = LuaText(state, 1);
            Asm::AssembleOptions options;
            options.Address = lua_isnoneornil(state, 2) ? 0 : AddressArgument(state, 2);
            options.DefaultToDword = true;
            options.Is64Bit = engine.Process() ? engine.Process()->Is64Bit() : true;
            lua_getfield(state, LUA_REGISTRYINDEX, kAssemblerModeKey);
            if (lua_isinteger(state, -1))
                options.Is64Bit = lua_tointeger(state, -1) != 0;
            lua_pop(state, 1);
            options.Resolve = [&engine](std::string_view name) -> Asm::SymbolValue
                {
                    if (const auto value = engine.Symbols().Resolve(name))
                        return { Asm::SymbolState::Known, *value };
                    return {};
                };
            const Asm::AssembleResult result = Asm::Assemble(line, options);
            if (!result.Ok)
            {
                lua_pushnil(state);
                lua_pushstring(state, result.Message.empty() ? std::string(Asm::ErrorName(result.Error)).c_str() : result.Message.c_str());
                return 2;
            }
            PushBytes(state, result.Bytes);
            return 1;
        }

        int SetAssemblerMode(lua_State* state)
        {
            lua_pushinteger(state, luaL_checkinteger(state, 1) != 0 ? 1 : 0);
            lua_setfield(state, LUA_REGISTRYINDEX, kAssemblerModeKey);
            return 0;
        }

        int CreateProcessCompat(lua_State* state)
        {
            Engine& engine = Owner(state);
            const std::wstring path = Utf8ToWide(LuaText(state, 1));
            const std::wstring parameters = lua_isnoneornil(state, 2) ? std::wstring() : Utf8ToWide(LuaText(state, 2));
            std::wstring command = L"\"" + path + L"\"";
            if (!parameters.empty())
                command += L" " + parameters;
            const std::wstring folder = std::filesystem::path(path).parent_path().wstring();
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION created{};
            if (!::CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.empty() ? nullptr : folder.c_str(), &startup, &created))
                return PushStatus(state, Status::Fail(ErrorCode::ProcessNotFound, WideToUtf8(path)));
            ::CloseHandle(created.hThread);
            ::CloseHandle(created.hProcess);
            Status status = engine.OpenProcess(MakeProcessId(created.dwProcessId));
            if (status && lua_toboolean(state, 3))
                status = engine.EnsureDebugger();
            if (!status)
                return PushStatus(state, status);
            lua_pushinteger(state, static_cast<lua_Integer>(created.dwProcessId));
            return 1;
        }

        std::wstring QuoteArgument(const std::wstring& argument)
        {
            if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos)
                return argument;
            std::wstring quoted = L"\"";
            std::size_t slashes = 0;
            for (const wchar_t c : argument)
            {
                if (c == L'\\')
                {
                    ++slashes;
                    continue;
                }
                quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                slashes = 0;
                quoted += c;
            }
            quoted.append(slashes * 2, L'\\');
            quoted += L"\"";
            return quoted;
        }

        int RunCommand(lua_State* state)
        {
            std::wstring command = QuoteArgument(Utf8ToWide(LuaText(state, 1)));
            if (lua_istable(state, 2))
            {
                const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, 2));
                for (lua_Integer i = 1; i <= count; ++i)
                {
                    lua_rawgeti(state, 2, i);
                    if (const char* text = lua_tostring(state, -1))
                        command += L" " + QuoteArgument(Utf8ToWide(text));
                    lua_pop(state, 1);
                }
            }
            else if (!lua_isnoneornil(state, 2))
                command += L" " + Utf8ToWide(LuaText(state, 2));
            const std::wstring folder = lua_isnoneornil(state, 3) ? std::wstring() : Utf8ToWide(LuaText(state, 3));

            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE readEnd = nullptr;
            HANDLE writeEnd = nullptr;
            if (!::CreatePipe(&readEnd, &writeEnd, &security, 0))
                return PushStatus(state, Status::Fail(ErrorCode::LuaRuntime, "runCommand could not create a pipe"));
            ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            startup.hStdOutput = writeEnd;
            startup.hStdError = writeEnd;
            PROCESS_INFORMATION created{};
            const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, folder.empty() ? nullptr : folder.c_str(), &startup, &created);
            ::CloseHandle(writeEnd);
            if (!started)
            {
                ::CloseHandle(readEnd);
                return PushStatus(state, Status::Fail(ErrorCode::LuaRuntime, "runCommand could not start " + WideToUtf8(command)));
            }
            std::string output;
            char buffer[4096];
            DWORD read = 0;
            while (::ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                output.append(buffer, read);
            ::CloseHandle(readEnd);
            ::WaitForSingleObject(created.hProcess, INFINITE);
            DWORD exitCode = 0;
            ::GetExitCodeProcess(created.hProcess, &exitCode);
            ::CloseHandle(created.hThread);
            ::CloseHandle(created.hProcess);
            lua_pushlstring(state, output.data(), output.size());
            lua_pushinteger(state, static_cast<lua_Integer>(exitCode));
            return 2;
        }

        int DebuggerInterface(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (!engine.Debugger().Attached())
                return 0;
            lua_pushinteger(state, engine.Debugger().UsingVeh() ? 2 : 1);
            return 1;
        }

        int DebuggerCanBreak(lua_State* state)
        {
            lua_pushboolean(state, Owner(state).Debugger().Attached());
            return 1;
        }

        int OpenAutoAssembler(lua_State* state)
        {
            UiRequest request;
            request.Kind = UiRequestKind::AutoAssembler;
            request.Text = lua_isnoneornil(state, 1) ? std::string() : LuaText(state, 1);
            Owner(state).Request(std::move(request));
            return 0;
        }

        int TimerIntervals(lua_State* state)
        {
            const Settings& settings = Owner(state).Options();
            lua_pushinteger(state, settings.FreezeIntervalMs);
            lua_pushinteger(state, settings.UpdateIntervalMs);
            return 2;
        }

        std::filesystem::path StreamFile(const char* name)
        {
            wchar_t temp[MAX_PATH + 1] = L"";
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
            std::filesystem::path folder = (length ? std::filesystem::path(temp) : std::filesystem::current_path()) / L"Manifold-Engine";
            std::error_code ignored;
            std::filesystem::create_directories(folder, ignored);
            return folder / (Utf8ToWide(name) + L"-" + std::to_wstring(::GetCurrentProcessId()) + L".CT");
        }

        int LoadTableCompat(lua_State* state)
        {
            Engine& engine = Owner(state);
            const bool merge = lua_toboolean(state, 2) != 0;
            std::filesystem::path path;
            if (lua_type(state, 1) == LUA_TSTRING)
                path = Utf8ToWide(lua_tostring(state, 1));
            else if (const auto data = StreamData(state, 1))
            {
                path = StreamFile("stream-table");
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
                if (!file)
                    return PushStatus(state, Status::Fail(ErrorCode::TableNotRead, WideToUtf8(path.wstring())));
            }
            else
                return luaL_argerror(state, 1, "a file name or a stream");
            return PushStatus(state, engine.LoadTable(path, merge));
        }

        int SaveTableCompat(lua_State* state)
        {
            Engine& engine = Owner(state);
            if (lua_type(state, 1) == LUA_TSTRING)
                return PushStatus(state, engine.SaveTable(Utf8ToWide(lua_tostring(state, 1))));
            if (!StreamData(state, 1))
                return luaL_argerror(state, 1, "a file name or a stream");
            const std::filesystem::path path = StreamFile("saved-table");
            const Status status = Table::SaveTable(path, engine.Table(), engine.Structures(), engine.TableSymbols());
            if (!status)
                return PushStatus(state, status);
            std::ifstream file(path, std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            lua_pushboolean(state, SetStreamData(state, 1, std::move(bytes)));
            return 1;
        }

        int PlaySoundCompat(lua_State* state)
        {
            static std::vector<std::uint8_t> playing;
            std::vector<std::uint8_t> data;
            if (lua_type(state, 1) == LUA_TSTRING)
            {
                std::ifstream file(Utf8ToWide(lua_tostring(state, 1)), std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
            else if (auto stream = StreamData(state, 1))
                data = std::move(*stream);
            if (data.size() < 12)
            {
                lua_pushboolean(state, 0);
                return 1;
            }
            const bool wait = lua_toboolean(state, 2) != 0;
            ::PlaySoundW(nullptr, nullptr, 0);
            playing = std::move(data);
            const BOOL played = ::PlaySoundW(reinterpret_cast<LPCWSTR>(playing.data()), nullptr, SND_MEMORY | SND_NODEFAULT | (wait ? SND_SYNC : SND_ASYNC));
            lua_pushboolean(state, played);
            return 1;
        }

        class Speech
        {
        public:
            static Speech& Instance()
            {
                static Speech* speech = new Speech();
                return *speech;
            }

            void Say(std::wstring text, DWORD flags, bool wait)
            {
                std::shared_ptr<std::promise<void>> done;
                std::future<void> finished;
                if (wait)
                {
                    done = std::make_shared<std::promise<void>>();
                    finished = done->get_future();
                }
                {
                    std::lock_guard lock(m_lock);
                    if (flags & SPF_PURGEBEFORESPEAK)
                    {
                        for (Job& job : m_jobs)
                        {
                            if (job.Done)
                                job.Done->set_value();
                        }
                        m_jobs.clear();
                    }
                    m_jobs.push_back({ std::move(text), flags & ~static_cast<DWORD>(SPF_ASYNC | SPF_PURGEBEFORESPEAK), done });
                    if (!m_started)
                    {
                        m_started = true;
                        std::thread([this]() { Run(); }).detach();
                    }
                }
                m_wake.notify_one();
                if (finished.valid())
                    finished.wait();
            }

        private:
            struct Job
            {
                std::wstring Text;
                DWORD Flags = 0;
                std::shared_ptr<std::promise<void>> Done;
            };

            void Run()
            {
                ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                ISpVoice* voice = nullptr;
                if (FAILED(::CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice), reinterpret_cast<void**>(&voice))))
                    voice = nullptr;
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock lock(m_lock);
                        m_wake.wait(lock, [this]() { return !m_jobs.empty(); });
                        job = std::move(m_jobs.front());
                        m_jobs.pop_front();
                    }
                    if (voice)
                        voice->Speak(job.Text.c_str(), job.Flags, nullptr);
                    if (job.Done)
                        job.Done->set_value();
                }
            }

            std::mutex m_lock;
            std::condition_variable m_wake;
            std::deque<Job> m_jobs;
            bool m_started = false;
        };

        int Speak(lua_State* state)
        {
            std::wstring text = Utf8ToWide(LuaText(state, 1));
            DWORD flags = SPF_DEFAULT;
            bool wait = false;
            if (lua_type(state, 2) == LUA_TNUMBER)
            {
                flags = static_cast<DWORD>(lua_tointeger(state, 2));
                wait = (flags & SPF_ASYNC) == 0;
            }
            else
                wait = lua_toboolean(state, 2) != 0;
            Speech::Instance().Say(std::move(text), flags, wait);
            return 0;
        }

        int SpeakEnglish(lua_State* state)
        {
            std::wstring text = L"<voice required=\"Language=409\">" + Utf8ToWide(LuaText(state, 1)) + L"</voice>";
            Speech::Instance().Say(std::move(text), SPF_IS_XML, lua_toboolean(state, 2) != 0);
            return 0;
        }

        int ReturnFalse(lua_State* state)
        {
            lua_pushboolean(state, 0);
            return 1;
        }

        struct Function
        {
            const char* Name;
            lua_CFunction Call;
        };

        const Function kFunctions[] = {
            { "setMousePos", &SetMousePos },
            { "keyDown", &KeyDown },
            { "keyUp", &KeyUp },
            { "doKeyPress", &DoKeyPress },
            { "mouse_event", &MouseEvent },
            { "getPixel", &GetPixelCompat },
            { "getSystemMetrics", &SystemMetrics },
            { "beep", &BeepCompat },
            { "playSound", &PlaySoundCompat },
            { "speak", &Speak },
            { "speakEnglish", &SpeakEnglish },
            { "outputDebugString", &OutputDebugText },
            { "getCPUCount", &CpuCount },
            { "cpuid", &CpuId },
            { "getOperatingSystem", [](lua_State* s) -> int { lua_pushinteger(s, 0); return 1; } },
            { "targetIsX86", [](lua_State* s) -> int { lua_pushboolean(s, 1); return 1; } },
            { "targetIsArm", &ReturnFalse },
            { "targetIsAndroid", &ReturnFalse },
            { "isConnectedToCEServer", &ReturnFalse },
            { "getPointerSize", &PointerSize },
            { "getCheatEngineFileVersion", &FileVersion },
            { "getUserRegistryEnvironmentVariable", &UserEnvironment },
            { "setUserRegistryEnvironmentVariable", &SetUserEnvironment },
            { "sendMessage", &SendMessageCompat },
            { "invertColor", &InvertColor },
            { "fileExists", &FileExists },
            { "deleteFile", &DeleteFileCompat },
            { "ansiToUtf8", &AnsiToUtf8 },
            { "utf8ToAnsi", &Utf8ToAnsi },
            { "byteTableToExtended", &ByteTableToExtended },
            { "extendedToByteTable", &ExtendedToByteTable },
            { "signExtend", &SignExtend },
            { "getOpenedProcessHandle", &OpenedProcessHandle },
            { "getXBox360ControllerState", &ControllerState },
            { "setXBox360ControllerVibration", &ControllerVibration },
            { "convertKeyComboToString", &KeyComboText },
            { "shortCutToText", &ShortcutToText },
            { "textToShortCut", &TextToShortcut },
            { "copyMemory", &CopyMemoryCompat },
            { "compareMemory", &CompareMemory },
            { "setMemoryProtection", &SetMemoryProtection },
            { "readRegionFromFile", &ReadRegionFromFile },
            { "writeRegionToFile", &WriteRegionToFile },
            { "allocateSharedMemoryLocal", &SharedMemoryLocal },
            { "createRemoteThread", &CreateRemoteThreadCompat },
            { "getComment", [](lua_State* s) -> int { return Note(s, false); } },
            { "setComment", [](lua_State* s) -> int { return SetNote(s, false); } },
            { "getHeader", [](lua_State* s) -> int { return Note(s, true); } },
            { "setHeader", [](lua_State* s) -> int { return SetNote(s, true); } },
            { "getSymbolInfo", &SymbolInfo },
            { "enumRegisteredSymbols", &RegisteredSymbols },
            { "reinitializeSymbolhandler", &ReinitializeSymbols },
            { "getRTTIClassName", &RttiClassName },
            { "assemble", &Assemble },
            { "setAssemblerMode", &SetAssemblerMode },
            { "createProcess", &CreateProcessCompat },
            { "runCommand", &RunCommand },
            { "saveTable", &SaveTableCompat },
            { "speedhack_getSpeed", [](lua_State* s) -> int { lua_pushnumber(s, 1.0); return 1; } },
            { "debug_getCurrentDebuggerInterface", &DebuggerInterface },
            { "debug_canBreak", &DebuggerCanBreak },
            { "debug_getMaxLastBranchRecord", [](lua_State* s) -> int { lua_pushinteger(s, -1); return 1; } },
            { "__manifoldWindowList", &WindowList },
            { "__manifoldThreadList", &ThreadList },
            { "__manifoldLoadTable", &LoadTableCompat },
            { "__manifoldOpenAutoAssembler", &OpenAutoAssembler },
            { "__manifoldTimerIntervals", &TimerIntervals },
            { "__manifoldCanWait", [](lua_State* s) -> int { lua_pushboolean(s, lua_isyieldable(s) && EngineOf(s).IsCooperative(s)); return 1; } },
        };
    }

    void RegisterCompatSystem(LuaEngine& engine, lua_State* state)
    {
        (void)engine;
        for (const Function& function : kFunctions)
        {
            lua_pushcfunction(state, function.Call);
            lua_setglobal(state, function.Name);
            if (function.Name[0] != '_')
                NoteFunction(function.Name);
        }
    }
}
