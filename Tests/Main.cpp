#include "TestKit.h"

#include "App/Commands.h"
#include "Core/Errors.h"
#include "Core/MainThread.h"
#include "Core/RuntimeLog.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

namespace Manifold::Tests
{
    int RunAssemblerTests();
    int RunCoreTests();
    int RunWorkTests();
    int RunDisasmTests();
    int RunDisasmLive(const std::vector<std::string>& arguments);
    int RunCommandTests();
    int RunTextTests();
    int RunDocTests();
    int RunConsoleTests();
    int RunIntegrationTests(const std::filesystem::path& target);
    int RunCompatHarness(const std::vector<std::string>& arguments);
    int RunCompatTests();
    int RunManifoldApiTests();
    int RunAutorunTests();
    std::string ManifoldMarkdown();
    int RunTemplateTests();
    int RunWatchStress(const std::vector<std::string>& arguments);
    int RunDebuggerCheck(const std::vector<std::string>& arguments);
    int RunMonoCheck(const std::vector<std::string>& arguments);
    int RunMonoDump(const std::vector<std::string>& arguments);
    int RunUnityDump(const std::vector<std::string>& arguments);

    Counter& Totals()
    {
        static Counter counter;
        return counter;
    }
}

int main(int argc, char** argv)
{
    Manifold::BindMainThread();
    std::filesystem::path target;
    bool assemblerOnly = false;
    bool allowSkips = false;
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    for (const std::string& argument : arguments)
    {
        if (argument == "--compat")
            return Manifold::Tests::RunCompatHarness(arguments);
        if (argument == "--watch-stress")
            return Manifold::Tests::RunWatchStress(arguments);
        if (argument == "--debugger-check")
            return Manifold::Tests::RunDebuggerCheck(arguments);
        if (argument == "--mono-check")
            return Manifold::Tests::RunMonoCheck(arguments);
        if (argument == "--mono-dump")
            return Manifold::Tests::RunMonoDump(arguments);
        if (argument == "--unity-dump")
            return Manifold::Tests::RunUnityDump(arguments);
        if (argument == "--disasm-check")
        {
            for (const std::string& other : arguments)
            {
                if (other.rfind("--process=", 0) == 0)
                    return Manifold::Tests::RunDisasmLive(arguments);
            }
            const int failed = Manifold::Tests::RunDisasmTests();
            std::printf("%d checks, %d failed\n", Manifold::Tests::Totals().Checks, failed);
            return failed;
        }
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--errors-markdown")
        {
            std::fputs(Manifold::ErrorsMarkdown().c_str(), stdout);
            return 0;
        }
        if (argument == "--shortcuts-markdown")
        {
            std::fputs(Manifold::ShortcutsMarkdown().c_str(), stdout);
            return 0;
        }
        if (argument == "--manifold-markdown")
        {
            std::fputs(Manifold::Tests::ManifoldMarkdown().c_str(), stdout);
            return 0;
        }
        if (argument.rfind("--target=", 0) == 0)
            target = argument.substr(9);
        if (argument == "--assembler")
            assemblerOnly = true;
        if (argument == "--allow-skips")
            allowSkips = true;
    }
    if (target.empty())
    {
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        target = std::filesystem::path(self).parent_path() / L"TestTarget.exe";
    }

    std::printf("Assembler\n");
    const int assembler = Manifold::Tests::RunAssemblerTests();
    if (assemblerOnly)
        return assembler;
    std::printf("Core\n");
    Manifold::Tests::RunCoreTests();
    std::printf("Background work\n");
    Manifold::Tests::RunWorkTests();
    std::printf("Disassembler\n");
    Manifold::Tests::RunDisasmTests();
    std::printf("Commands and shortcuts\n");
    Manifold::Tests::RunCommandTests();
    std::printf("Text\n");
    Manifold::Tests::RunTextTests();
    std::printf("Documentation\n");
    Manifold::Tests::RunDocTests();
    std::printf("Lua console\n");
    Manifold::Tests::RunConsoleTests();
    std::printf("Compatibility\n");
    Manifold::Tests::RunCompatTests();
    std::printf("Manifold Lua API\n");
    Manifold::Tests::RunManifoldApiTests();
    std::printf("Autorun modules\n");
    Manifold::Tests::RunAutorunTests();
    std::printf("Templates\n");
    Manifold::Tests::RunTemplateTests();
    std::printf("Integration (%ls)\n", target.c_str());
    Manifold::Tests::RunIntegrationTests(target);
    const auto& totals = Manifold::Tests::Totals();
    std::printf("\n%d checks, %d failed, %d skipped; assembler suite: %d failed\n", totals.Checks, totals.Failures, totals.Skipped, assembler);
    if (totals.Skipped && !allowSkips)
        std::printf("Skipped checks fail the run. Run from the repository with TestTarget.exe beside the tests, or pass --allow-skips.\n");
    return totals.Failures + assembler + (allowSkips ? 0 : totals.Skipped);
}
