#include "TestKit.h"

#include "App/Commands.h"
#include "Core/Errors.h"
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
    int RunCommandTests();
    int RunIntegrationTests(const std::filesystem::path& target);
    int RunCompatHarness(const std::vector<std::string>& arguments);
    int RunCompatTests();
    int RunTemplateTests();
    int RunWatchStress(const std::vector<std::string>& arguments);
    int RunDebuggerCheck(const std::vector<std::string>& arguments);

    Counter& Totals()
    {
        static Counter counter;
        return counter;
    }
}

int main(int argc, char** argv)
{
    std::filesystem::path target;
    bool assemblerOnly = false;
    const std::vector<std::string> arguments(argv + 1, argv + argc);
    for (const std::string& argument : arguments)
    {
        if (argument == "--compat")
            return Manifold::Tests::RunCompatHarness(arguments);
        if (argument == "--watch-stress")
            return Manifold::Tests::RunWatchStress(arguments);
        if (argument == "--debugger-check")
            return Manifold::Tests::RunDebuggerCheck(arguments);
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
        if (argument.rfind("--target=", 0) == 0)
            target = argument.substr(9);
        if (argument == "--assembler")
            assemblerOnly = true;
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
    std::printf("Commands and shortcuts\n");
    Manifold::Tests::RunCommandTests();
    std::printf("Compatibility\n");
    Manifold::Tests::RunCompatTests();
    std::printf("Templates\n");
    Manifold::Tests::RunTemplateTests();
    std::printf("Integration (%ls)\n", target.c_str());
    Manifold::Tests::RunIntegrationTests(target);
    const auto& totals = Manifold::Tests::Totals();
    std::printf("\n%d checks, %d failed; assembler suite: %d failed\n", totals.Checks, totals.Failures, assembler);
    return totals.Failures + assembler;
}
