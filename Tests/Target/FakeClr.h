#pragma once

#include <windows.h>
#include <metahost.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// TestTarget --clr=<ManifoldFakeGame.dll> hosts the .NET Framework 4 and
// TestTarget --coreclr=<ManifoldFakeGame.dll> --runtime=<folder> hosts .NET
// (coreclr.dll from that shared runtime folder), each with the fake game
// assembly. The game builds its world once, prints a few object addresses and
// "clr ready", and then ticks every half second until the process is ended.
// A real runtime, so the .NET reader is tested against what it will meet in a
// game, not a stand-in.
namespace FakeClr
{
    inline int Fail(const char* what, long code)
    {
        std::printf("clr failed: %s (0x%08lX)\n", what, static_cast<unsigned long>(code));
        std::fflush(stdout);
        return 3;
    }

    inline int RunFramework(const std::wstring& assembly)
    {
        HMODULE mscoree = ::LoadLibraryW(L"mscoree.dll");
        const auto create = mscoree ? reinterpret_cast<CLRCreateInstanceFnPtr>(reinterpret_cast<void*>(::GetProcAddress(mscoree, "CLRCreateInstance"))) : nullptr;
        if (!create)
            return Fail("no mscoree.dll", 0);
        constexpr GUID kMetaHost = { 0x9280188D, 0x0E8E, 0x4867, { 0xB3, 0x0C, 0x7F, 0xA8, 0x38, 0x84, 0xE8, 0xDE } };
        constexpr GUID kRuntimeHost = { 0x90F1A06E, 0x7712, 0x4762, { 0x86, 0xB5, 0x7A, 0x5E, 0xBA, 0x6B, 0xDB, 0x02 } };
        ICLRMetaHost* host = nullptr;
        ICLRRuntimeInfo* info = nullptr;
        ICLRRuntimeHost* runtime = nullptr;
        HRESULT result = create(kMetaHost, __uuidof(ICLRMetaHost), reinterpret_cast<void**>(&host));
        if (FAILED(result) || !host)
            return Fail("CLRCreateInstance", result);
        result = host->GetRuntime(L"v4.0.30319", __uuidof(ICLRRuntimeInfo), reinterpret_cast<void**>(&info));
        if (FAILED(result) || !info)
            return Fail("GetRuntime v4", result);
        result = info->GetInterface(kRuntimeHost, __uuidof(ICLRRuntimeHost), reinterpret_cast<void**>(&runtime));
        if (FAILED(result) || !runtime)
            return Fail("GetInterface", result);
        result = runtime->Start();
        if (FAILED(result))
            return Fail("Start", result);
        DWORD built = 0;
        result = runtime->ExecuteInDefaultAppDomain(assembly.c_str(), L"Manifold.FakeGame.World", L"Build", L"", &built);
        if (FAILED(result) || built != 1)
            return Fail("World.Build", result);
        for (;;)
        {
            ::Sleep(500);
            DWORD ticks = 0;
            runtime->ExecuteInDefaultAppDomain(assembly.c_str(), L"Manifold.FakeGame.World", L"Tick", L"", &ticks);
        }
    }

    using CoreInitialize = int(__cdecl*)(const char* exePath, const char* domainName, int count, const char** keys, const char** values, void** host, unsigned int* domain);
    using CoreCreateDelegate = int(__cdecl*)(void* host, unsigned int domain, const char* assembly, const char* type, const char* method, void** delegate);
    using Entry = int(STDMETHODCALLTYPE*)(int);

    inline std::string Utf8(const std::wstring& text)
    {
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), '\0');
        if (size > 1)
            ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
        return out;
    }

    inline int RunCore(const std::wstring& assembly, const std::wstring& runtimeFolder)
    {
        const std::filesystem::path folder(runtimeFolder);
        HMODULE coreclr = ::LoadLibraryW((folder / L"coreclr.dll").c_str());
        if (!coreclr)
            return Fail("could not load coreclr.dll", static_cast<long>(::GetLastError()));
        const auto initialize = reinterpret_cast<CoreInitialize>(reinterpret_cast<void*>(::GetProcAddress(coreclr, "coreclr_initialize")));
        const auto createDelegate = reinterpret_cast<CoreCreateDelegate>(reinterpret_cast<void*>(::GetProcAddress(coreclr, "coreclr_create_delegate")));
        if (!initialize || !createDelegate)
            return Fail("coreclr.dll has no hosting exports", 0);
        std::string trusted;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            if (entry.path().extension() == L".dll")
                trusted += Utf8(entry.path().wstring()) + ";";
        }
        trusted += Utf8(assembly);
        const std::string appPath = Utf8(std::filesystem::path(assembly).parent_path().wstring());
        const char* keys[] = { "TRUSTED_PLATFORM_ASSEMBLIES", "APP_PATHS" };
        const char* values[] = { trusted.c_str(), appPath.c_str() };
        wchar_t self[MAX_PATH] = L"";
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        void* host = nullptr;
        unsigned int domain = 0;
        int result = initialize(Utf8(self).c_str(), "ManifoldFakeGame", 2, keys, values, &host, &domain);
        if (result < 0)
            return Fail("coreclr_initialize", result);
        Entry build = nullptr;
        Entry tick = nullptr;
        result = createDelegate(host, domain, "ManifoldFakeGame", "Manifold.FakeGame.World", "BuildCore", reinterpret_cast<void**>(&build));
        if (result < 0 || !build)
            return Fail("create BuildCore", result);
        result = createDelegate(host, domain, "ManifoldFakeGame", "Manifold.FakeGame.World", "TickCore", reinterpret_cast<void**>(&tick));
        if (result < 0 || !tick)
            return Fail("create TickCore", result);
        if (build(0) != 1)
            return Fail("World.Build", 0);
        for (;;)
        {
            ::Sleep(500);
            tick(0);
        }
    }

    // Returns -1 when the command line asks for no .NET host.
    inline int Run(int argc, char** argv)
    {
        std::wstring framework, core, runtime;
        const auto wide = [](const char* text)
            {
                const int size = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
                std::wstring out(static_cast<std::size_t>(size > 0 ? size - 1 : 0), L'\0');
                if (size > 1)
                    ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
                return out;
            };
        for (int i = 1; i < argc; ++i)
        {
            const std::string argument = argv[i];
            if (argument.rfind("--clr=", 0) == 0)
                framework = wide(argv[i] + 6);
            else if (argument.rfind("--coreclr=", 0) == 0)
                core = wide(argv[i] + 10);
            else if (argument.rfind("--runtime=", 0) == 0)
                runtime = wide(argv[i] + 10);
        }
        if (!framework.empty())
            return RunFramework(framework);
        if (!core.empty())
            return RunCore(core, runtime);
        return -1;
    }
}
