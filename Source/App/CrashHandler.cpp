#include "App/CrashHandler.h"

#include "Core/Errors.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <system_error>

#include <windows.h>
#include <dbghelp.h>

namespace Manifold::CrashHandler
{
    namespace
    {
        wchar_t g_folder[MAX_PATH * 2] = L"";
        LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

        LONG WINAPI OnCrash(EXCEPTION_POINTERS* pointers)
        {
            static volatile LONG entered = 0;
            if (::InterlockedExchange(&entered, 1) != 0)
                return EXCEPTION_CONTINUE_SEARCH;

            SYSTEMTIME now{};
            ::GetLocalTime(&now);
            wchar_t name[MAX_PATH * 3] = L"";
            ::swprintf_s(name, L"%s\\crash-%04u%02u%02u-%02u%02u%02u", g_folder, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            ::CreateDirectoryW(g_folder, nullptr);

            const std::wstring dumpPath = std::wstring(name) + L".dmp";
            HANDLE file = ::CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION exception{};
                exception.ThreadId = ::GetCurrentThreadId();
                exception.ExceptionPointers = pointers;
                exception.ClientPointers = FALSE;
                ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                    &exception, nullptr, nullptr);
                ::CloseHandle(file);
            }

            const DWORD code = pointers && pointers->ExceptionRecord ? pointers->ExceptionRecord->ExceptionCode : 0;
            const std::uint64_t address = pointers && pointers->ExceptionRecord ? reinterpret_cast<std::uint64_t>(pointers->ExceptionRecord->ExceptionAddress) : 0;
            HMODULE module = nullptr;
            ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module);
            wchar_t moduleName[MAX_PATH] = L"?";
            if (module)
                ::GetModuleFileNameW(module, moduleName, MAX_PATH);
            const std::uint64_t offset = module ? address - reinterpret_cast<std::uint64_t>(module) : address;
            const std::string detail = Hex(code, 8) + " at " + WideToUtf8(std::filesystem::path(moduleName).filename().wstring()) + "+" + Hex(offset);
            LOG_ERROR("Crash", ErrorMessage(ErrorCode::Crashed, detail));

            const std::wstring textPath = std::wstring(name) + L".txt";
            FILE* text = nullptr;
            if (::_wfopen_s(&text, textPath.c_str(), L"w") == 0 && text)
            {
                std::fprintf(text, "%s\n", ErrorMessage(ErrorCode::Crashed, detail).c_str());
                std::fprintf(text, "Dump: %s\n", WideToUtf8(dumpPath).c_str());
                std::fclose(text);
            }
            return g_previous ? g_previous(pointers) : EXCEPTION_EXECUTE_HANDLER;
        }
    }

    void Install(const std::filesystem::path& folder)
    {
        ::wcsncpy_s(g_folder, folder.c_str(), _TRUNCATE);
        g_previous = ::SetUnhandledExceptionFilter(&OnCrash);
    }

    std::optional<std::filesystem::path> TakeLastCrash(const std::filesystem::path& folder)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(folder, error))
            return std::nullopt;
        std::optional<std::filesystem::path> newest;
        std::filesystem::file_time_type newestTime{};
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (entry.path().extension() != L".txt")
                continue;
            const auto time = entry.last_write_time(error);
            if (!newest || time > newestTime)
            {
                newest = entry.path();
                newestTime = time;
            }
        }
        if (!newest)
            return std::nullopt;
        const std::filesystem::path seen = folder / L"seen";
        if (std::filesystem::exists(seen, error) && std::filesystem::last_write_time(seen, error) >= newestTime)
            return std::nullopt;
        FILE* marker = nullptr;
        if (::_wfopen_s(&marker, seen.c_str(), L"w") == 0 && marker)
            std::fclose(marker);
        return newest;
    }
}
