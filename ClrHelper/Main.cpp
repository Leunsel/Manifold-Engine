#include "ClrHelper/DacSession.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

// ManifoldClr.exe / ManifoldClr32.exe: reads a .NET game for Manifold Engine
// through the runtime's data access library, from outside the game. The
// program starts it with the pipe to answer on and, when it can, an inherited
// handle to the game; the helper ends when the pipe closes. It never writes to
// the game and never runs code in it. A separate process because the DAC must
// match the game's bitness (a 32-bit game needs a 32-bit reader), and so a
// DAC that trips over a half-written structure takes down the helper, not the
// program.
namespace
{
    bool ReadAll(HANDLE pipe, void* buffer, DWORD size)
    {
        auto* bytes = static_cast<std::uint8_t*>(buffer);
        while (size)
        {
            DWORD got = 0;
            if (!::ReadFile(pipe, bytes, size, &got, nullptr) || got == 0)
                return false;
            bytes += got;
            size -= got;
        }
        return true;
    }

    bool WriteAll(HANDLE pipe, const std::vector<std::uint8_t>& bytes)
    {
        std::size_t at = 0;
        while (at < bytes.size())
        {
            DWORD wrote = 0;
            if (!::WriteFile(pipe, bytes.data() + at, static_cast<DWORD>(bytes.size() - at), &wrote, nullptr) || wrote == 0)
                return false;
            at += wrote;
        }
        return true;
    }

    const wchar_t* Argument(int argc, wchar_t** argv, const wchar_t* prefix)
    {
        const std::size_t length = std::wcslen(prefix);
        for (int i = 1; i < argc; ++i)
        {
            if (std::wcsncmp(argv[i], prefix, length) == 0)
                return argv[i] + length;
        }
        return nullptr;
    }
}

int wmain(int argc, wchar_t** argv)
{
    // No error dialogs: a crash here must end the helper quietly so the
    // program sees the pipe close and reports it.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const wchar_t* pipeName = Argument(argc, argv, L"--pipe=");
    if (!pipeName || std::wcsncmp(pipeName, L"\\\\.\\pipe\\", 9) != 0)
        return 2;
    HANDLE pipe = ::CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return 3;
    // MANIFOLD_CLR_TRACE=<file> appends one line per request: the operation,
    // its argument, the reply size and how long it took.
    std::FILE* trace = nullptr;
    wchar_t tracePath[MAX_PATH] = L"";
    if (::GetEnvironmentVariableW(L"MANIFOLD_CLR_TRACE", tracePath, MAX_PATH))
        _wfopen_s(&trace, tracePath, L"a");
    LARGE_INTEGER frequency{};
    ::QueryPerformanceFrequency(&frequency);
    Manifold::ClrHelper::DacSession session;
    std::vector<std::uint8_t> request;
    for (;;)
    {
        std::uint32_t size = 0;
        if (!ReadAll(pipe, &size, sizeof(size)) || size == 0 || size > Manifold::Target::ClrProtocol::kMaxMessage)
            break;
        request.resize(size);
        if (!ReadAll(pipe, request.data(), size))
            break;
        LARGE_INTEGER started{};
        ::QueryPerformanceCounter(&started);
        const std::vector<std::uint8_t> reply = session.Handle(request);
        if (trace)
        {
            LARGE_INTEGER ended{};
            ::QueryPerformanceCounter(&ended);
            std::uint64_t argument = 0;
            if (request.size() >= 9)
                std::memcpy(&argument, request.data() + 1, sizeof(argument));
            std::fprintf(trace, "op %u arg %llX reply %zu bytes %.2f ms\n", request[0], static_cast<unsigned long long>(argument), reply.size(),
                1000.0 * static_cast<double>(ended.QuadPart - started.QuadPart) / static_cast<double>(frequency.QuadPart));
            std::fflush(trace);
        }
        if (!WriteAll(pipe, reply))
            break;
    }
    if (trace)
        std::fclose(trace);
    ::CloseHandle(pipe);
    return 0;
}
