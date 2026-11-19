#include "TestKit.h"

#include "App/Commands.h"
#include "Core/Errors.h"
#include "Core/MainThread.h"
#include "Core/Paths.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

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

    class DataFolder
    {
    public:
        DataFolder()
        {
            wchar_t temp[MAX_PATH + 1] = L"";
            const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
            m_folder = (length ? std::filesystem::path(temp) : std::filesystem::current_path()) / L"Manifold-Engine-Tests" / std::to_wstring(::GetCurrentProcessId());
            std::error_code ignored;
            std::vector<std::filesystem::path> stale;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(m_folder.parent_path(), ignored))
            {
                const std::wstring name = entry.path().filename().wstring();
                if (name.empty() || name.size() > 10 || !std::all_of(name.begin(), name.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    continue;
                const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(std::stoull(name)));
                const bool running = process && ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
                if (process)
                    ::CloseHandle(process);
                if (!running)
                    stale.push_back(entry.path());
            }
            for (const std::filesystem::path& folder : stale)
                std::filesystem::remove_all(folder, ignored);
            std::filesystem::remove_all(m_folder, ignored);
            std::filesystem::create_directories(m_folder, ignored);
            Paths::SetDataFolder(m_folder);
        }

        ~DataFolder()
        {
            Paths::SetDataFolder({});
            std::error_code ignored;
            std::filesystem::remove_all(m_folder, ignored);
        }

        DataFolder(const DataFolder&) = delete;
        DataFolder& operator=(const DataFolder&) = delete;

    private:
        std::filesystem::path m_folder;
    };
}

int main(int argc, char** argv)
{
    using namespace Manifold::Tests;
    Manifold::BindMainThread();
    const DataFolder data;
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
    return totals.Failures + assembler + (allowSkips ? 0 : totals.Skipped);
}
