#include "App/Application.h"
#include "App/CrashHandler.h"
#include "App/Settings.h"
#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

#include "Syscalls/Syscall.h"

namespace
{
    Manifold::StartOptions ReadArguments()
    {
        Manifold::StartOptions options;
        int count = 0;
        wchar_t** arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        if (!arguments)
            return options;
        for (int i = 1; i < count; ++i)
        {
            const std::wstring argument = arguments[i];
            if (argument.rfind(L"--process=", 0) == 0)
                options.Process = Manifold::WideToUtf8(argument.substr(10));
            else if (argument.rfind(L"--open=", 0) == 0)
                options.Open = Manifold::Split(Manifold::WideToUtf8(argument.substr(7)), ',');
            else if (argument.rfind(L"--table=", 0) == 0)
                options.Table = argument.substr(8);
            else if (!argument.empty() && argument[0] != L'-')
                options.Table = argument;
        }
        ::LocalFree(arguments);
        return options;
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int showCommand)
{
    InitSysCalls();
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    Manifold::Paths::AdoptEarlierFiles();
    Manifold::RuntimeLog::OpenFile(Manifold::Paths::LogFile());
    Manifold::CrashHandler::Install(Manifold::Paths::CrashFolder());

    int result = 0;
    {
        Manifold::Application app(ReadArguments());
        if (!app.Initialise(instance, showCommand))
        {
            ::MessageBoxW(nullptr, app.LastError(), L"Manifold Engine could not be started", MB_ICONERROR | MB_OK);
            return 1;
        }
        result = app.Run();
    }
    Manifold::RuntimeLog::OpenFile({});
    return result;
}
