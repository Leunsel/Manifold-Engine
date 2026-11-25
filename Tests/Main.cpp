#include "TestKit.h"

#include "App/Commands.h"
#include "Core/Errors.h"
#include "Core/MainThread.h"
#include "Core/Paths.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"
#include "Target/AgentLife.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <dbghelp.h>

namespace Manifold::Tests
{
    Counter& Totals()
    {
        static Counter counter;
        return counter;
    }

    int RunAssemblerTests();

    std::vector<Suite>& Suites()
    {
        static std::vector<Suite> suites;
        return suites;
    }

    std::vector<Mode>& Modes()
    {
        static std::vector<Mode> modes;
        return modes;
    }

    MANIFOLD_SUITE("assembler", "Assembler", 0, [](const SuiteContext&) { return RunAssemblerTests(); });

    MANIFOLD_MODE("--errors-markdown", [](const std::vector<std::string>&)
        {
            std::fputs(ErrorsMarkdown().c_str(), stdout);
            return 0;
        });

    MANIFOLD_MODE("--shortcuts-markdown", [](const std::vector<std::string>&)
        {
            std::fputs(ShortcutsMarkdown().c_str(), stdout);
            return 0;
        });

    MANIFOLD_MODE("--crash-check", [](const std::vector<std::string>&)
        {
            ::RaiseException(0xE0C0FFEE, 0, 0, nullptr);
            return 0;
        });

    constexpr const wchar_t* kKeptMarker = L"kept.txt";
    constexpr auto kKeptFor = std::chrono::hours(24 * 7);

    wchar_t g_dumpPath[MAX_PATH * 2] = L"";
    wchar_t g_keptPath[MAX_PATH * 2] = L"";

    LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* info)
    {
        std::printf("\nCRASH: exception %08lX at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
        if (g_keptPath[0])
        {
            const HANDLE marker = ::CreateFileW(g_keptPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (marker != INVALID_HANDLE_VALUE)
                ::CloseHandle(marker);
        }
        if (g_dumpPath[0])
        {
            const HANDLE file = ::CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION exception{ ::GetCurrentThreadId(), info, FALSE };
                const BOOL written = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, MiniDumpNormal, &exception, nullptr, nullptr);
                ::CloseHandle(file);
                std::printf("%s %ls\n", written ? "The crash dump is" : "No crash dump could be written to", g_dumpPath);
            }
        }
        std::fflush(stdout);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // ∑ MANIFOLD_TEST_TRACE_FAULTS=1 writes every first-chance fault, with its stack as module+offset, straight
    //   to stderr. It is for crashes that bypass the crash dump above: a fault while the program exits, or one
    //   that takes the unhandled-exception filter with it, loses the buffered output and writes no dump.
    //   It only uses the stack and WriteFile, so it works when the heap or the C runtime is already broken.
    //   Handled faults show too; read the last one before the exit. llvm-symbolizer turns the offsets into lines.
    void WriteFaultText(const char* text)
    {
        DWORD written = 0;
        ::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    }

    void WriteFaultPlace(const char* label, const void* address)
    {
        char line[600];
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<const char*>(address), &module) && module)
            ::GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        std::snprintf(line, sizeof(line), "  %s %p  %s+0x%llX\n", label, address, module ? name : "(no module)",
            module ? static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)) : 0ull);
        WriteFaultText(line);
    }

    LONG WINAPI TraceFault(EXCEPTION_POINTERS* info)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code < 0xC0000000u || code == 0xE06D7363u)
            return EXCEPTION_CONTINUE_SEARCH;
        char line[200];
        std::snprintf(line, sizeof(line), "\nFAULT %08lX thread %lu", code, ::GetCurrentThreadId());
        WriteFaultText(line);
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        {
            std::snprintf(line, sizeof(line), " (%s %p)", info->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : info->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : "writing",
                reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
            WriteFaultText(line);
        }
        WriteFaultText("\n");
        WriteFaultPlace("at", info->ExceptionRecord->ExceptionAddress);
        void* frames[32] = {};
        const USHORT count = ::RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
            WriteFaultPlace("  ", frames[i]);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    class DataFolder
    {
    public:
        DataFolder()
        {
            wchar_t given[MAX_PATH + 1] = L"";
            const DWORD givenLength = ::GetEnvironmentVariableW(L"MANIFOLD_TEST_DATA", given, MAX_PATH + 1);
            m_given = givenLength > 0 && givenLength <= MAX_PATH;
            wchar_t temp[MAX_PATH + 1] = L"";
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
            const std::filesystem::path root = m_given ? std::filesystem::path(given) : (length ? std::filesystem::path(temp) : std::filesystem::current_path()) / L"Manifold-Engine-Tests";
            m_folder = root / std::to_wstring(::GetCurrentProcessId());
            std::error_code ignored;
            std::vector<std::filesystem::path> stale;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, ignored))
            {
                if (m_given)
                    break;
                const std::wstring name = entry.path().filename().wstring();
                if (name.empty() || name.size() > 10 || !std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    continue;
                const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoull(name)));
                const bool running = process && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
                if (process)
                    ::CloseHandle(process);
                std::error_code unmarked;
                const std::filesystem::file_time_type marked = std::filesystem::last_write_time(entry.path() / kKeptMarker, unmarked);
                const bool kept = !unmarked && std::filesystem::file_time_type::clock::now() - marked < kKeptFor;
                if (!running && !kept)
                    stale.push_back(entry.path());
            }
            for (const std::filesystem::path& folder : stale)
                std::filesystem::remove_all(folder, ignored);
            std::filesystem::remove_all(m_folder, ignored);
            std::filesystem::create_directories(m_folder, ignored);
            Paths::SetDataFolder(m_folder);
            std::filesystem::create_directories(Paths::CrashFolder(), ignored);
            ::wcsncpy_s(g_dumpPath, (Paths::CrashFolder() / (L"tests-" + std::to_wstring(::GetCurrentProcessId()) + L".dmp")).c_str(), _TRUNCATE);
            ::wcsncpy_s(g_keptPath, (m_folder / kKeptMarker).c_str(), _TRUNCATE);
            ::SetUnhandledExceptionFilter(&WriteCrashDump);
        }

        ~DataFolder()
        {
            RuntimeLog::OpenFile({});
            Paths::SetDataFolder({});
            g_dumpPath[0] = 0;
            g_keptPath[0] = 0;
            std::error_code ignored;
            if (!m_keep && !m_given)
                std::filesystem::remove_all(m_folder, ignored);
        }

        DataFolder(const DataFolder&) = delete;
        DataFolder& operator=(const DataFolder&) = delete;

        const std::filesystem::path& Folder() const { return m_folder; }

        void Keep()
        {
            m_keep = true;
            const HANDLE marker = ::CreateFileW(g_keptPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (marker != INVALID_HANDLE_VALUE)
                ::CloseHandle(marker);
        }

    private:
        std::filesystem::path m_folder;
        bool m_given = false;
        bool m_keep = false;
    };
}

int main(int argc, char** argv)
{
    using namespace Manifold::Tests;
    if (char trace[4] = ""; ::GetEnvironmentVariableA("MANIFOLD_TEST_TRACE_FAULTS", trace, sizeof(trace)) && trace[0] == '1')
        ::AddVectoredExceptionHandler(1, &TraceFault);
    Manifold::BindMainThread();
    // ∑ The agent unloads a moment after its last part let go (8 s in the program); the tests,
    //   and the host processes they start, check that within a second or two.
    Manifold::Target::SetAgentGrace(300);
    DataFolder data;
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    for (const std::string& argument : arguments)
    {
        for (const Mode& mode : Modes())
        {
            if (argument == mode.Flag)
                return mode.Run(arguments);
        }
    }

    SuiteContext context;
    bool allowSkips = false;
    bool list = false;
    std::vector<std::string> filters;
    for (const std::string& argument : arguments)
    {
        if (argument.rfind("--target=", 0) == 0)
            context.Target = argument.substr(9);
        else if (argument == "--assembler")
            filters.push_back("assembler");
        else if (argument.rfind("--filter=", 0) == 0)
        {
            for (const std::string& part : Manifold::Split(argument.substr(9), ','))
            {
                if (!Manifold::Trim(part).empty())
                    filters.push_back(Manifold::Lower(Manifold::Trim(part)));
            }
        }
        else if (argument == "--list")
            list = true;
        else if (argument == "--allow-skips")
            allowSkips = true;
    }
    if (context.Target.empty())
    {
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        context.Target = std::filesystem::path(self).parent_path() / L"TestTarget.exe";
    }

    std::vector<Suite> suites = Suites();
    std::stable_sort(suites.begin(), suites.end(), [](const Suite& a, const Suite& b) { return a.Order < b.Order; });
    if (list)
    {
        for (const Suite& suite : suites)
            std::printf("%-14s %s\n", suite.Name, suite.Title);
        return 0;
    }
    const auto chosen = [&](const Suite& suite)
        {
            if (filters.empty())
                return true;
            const std::string name = Manifold::Lower(suite.Name);
            return std::any_of(filters.begin(), filters.end(), [&](const std::string& filter) { return name.find(filter) != std::string::npos; });
        };

    Manifold::RuntimeLog::OpenFile(Manifold::Paths::LogFile());
    int assembler = 0;
    int ran = 0;
    for (const Suite& suite : suites)
    {
        if (!chosen(suite))
            continue;
        ++ran;
        std::printf("%s\n", suite.Title);
        const int result = suite.Run(context);
        if (std::string_view(suite.Name) == "assembler")
            assembler = result;
    }
    if (ran == 0)
    {
        std::printf("No suite matches the filter; --list shows the suites.\n");
        return 1;
    }
    const Counter& totals = Totals();
    std::printf("\n%d checks, %d failed, %d skipped; assembler suite: %d failed\n", totals.Checks, totals.Failures, totals.Skipped, assembler);
    if (totals.Skipped && !allowSkips)
        std::printf("Skipped checks fail the run. Run from the repository with TestTarget.exe beside the tests, or pass --allow-skips.\n");
    const int result = totals.Failures + assembler + (allowSkips ? 0 : totals.Skipped);
    if (result != 0)
    {
        data.Keep();
        std::printf("The log and the settings of this run stay in %ls\n", data.Folder().c_str());
    }
    return result;
}
