#include "App/Diagnostics.h"

#include "App/Engine.h"
#include "App/Settings.h"
#include "Core/Profiler.h"
#include "Core/Text.h"
#include "Core/Version.h"
#include "Lua/LuaEngine.h"
#include "Target/Process.h"

#include "imgui.h"

extern "C"
{
#include "lua.h"
}

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>

#include <windows.h>
#include <intrin.h>
#include <psapi.h>

namespace Manifold::Diagnostics
{
    namespace
    {
        using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
        using NtQueryTimerResolutionFunction = LONG(WINAPI*)(ULONG*, ULONG*, ULONG*);
        using NtQueryInformationThreadFunction = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        using NtQuerySystemInformationFunction = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);
        using GetThreadDescriptionFunction = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        using NtGetNextThreadFunction = LONG(WINAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

        constexpr std::uint32_t kVendorNvidia = 0x10DE;
        constexpr std::uint32_t kVendorAmd = 0x1002;
        constexpr std::uint32_t kVendorIntel = 0x8086;
        constexpr std::uint32_t kVminFixMicrocode = 0x12B;

        GraphicsInfo& GraphicsStore()
        {
            static GraphicsInfo info;
            return info;
        }

        struct SamplerState
        {
            bool Primed = false;
            std::uint64_t LastWall = 0;
            std::uint64_t LastCpu = 0;
            std::map<DWORD, std::uint64_t> ThreadCpu;
            std::uint64_t ThreadWall = 0;
            ProcessSample Last;
            std::vector<ThreadSample> Threads;
            std::uint64_t Started = 0;
        };

        SamplerState& Sampler()
        {
            static SamplerState state;
            return state;
        }

        std::uint64_t FileTimeValue(const FILETIME& time)
        {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }

        std::uint64_t WallNow()
        {
            FILETIME now{};
            ::GetSystemTimeAsFileTime(&now);
            return FileTimeValue(now);
        }

        void* NtFunction(const char* name)
        {
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            return ntdll ? reinterpret_cast<void*>(::GetProcAddress(ntdll, name)) : nullptr;
        }

        std::string Hex(std::uint64_t value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
            return text;
        }

        std::string Fixed(double value, int digits)
        {
            char text[64];
            std::snprintf(text, sizeof(text), "%.*f", digits, value);
            return text;
        }

        std::string Percent(double value)
        {
            return Fixed(value, value < 10.0 ? 1 : 0) + " %";
        }

        std::string RegistryText(HKEY root, const wchar_t* key, const wchar_t* value)
        {
            wchar_t buffer[512] = L"";
            DWORD size = sizeof(buffer);
            if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
                return {};
            return Trim(WideToUtf8(buffer));
        }

        std::optional<DWORD> RegistryNumber(HKEY root, const wchar_t* key, const wchar_t* value)
        {
            DWORD number = 0;
            DWORD size = sizeof(number);
            if (::RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &number, &size) != ERROR_SUCCESS)
                return std::nullopt;
            return number;
        }

        struct WindowsVersion
        {
            DWORD Major = 0;
            DWORD Minor = 0;
            DWORD Build = 0;
            DWORD Revision = 0;
            std::string Name;
            std::string Release;
        };

        WindowsVersion ReadWindowsVersion()
        {
            WindowsVersion version;
            OSVERSIONINFOW info{ sizeof(info) };
            if (const auto get = reinterpret_cast<RtlGetVersionFunction>(NtFunction("RtlGetVersion")); get && get(&info) == 0)
            {
                version.Major = info.dwMajorVersion;
                version.Minor = info.dwMinorVersion;
                version.Build = info.dwBuildNumber;
            }
            const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            version.Name = RegistryText(HKEY_LOCAL_MACHINE, key, L"ProductName");
            if (version.Build >= 22000 && version.Name.rfind("Windows 10", 0) == 0)
                version.Name.replace(0, 10, "Windows 11");
            if (version.Name.empty())
                version.Name = "Windows";
            version.Release = RegistryText(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
            version.Revision = RegistryNumber(HKEY_LOCAL_MACHINE, key, L"UBR").value_or(0);
            return version;
        }

        struct CpuInfo
        {
            std::string Brand;
            std::string Vendor;
            bool Hypervisor = false;
            std::string HypervisorName;
            int Cores = 0;
            int PerformanceCores = 0;
            int Threads = 0;
            std::optional<std::uint32_t> Microcode;
        };

        CpuInfo ReadCpu()
        {
            CpuInfo cpu;
            int regs[4] = {};
            __cpuid(regs, 0);
            char vendor[13] = {};
            std::memcpy(vendor, &regs[1], 4);
            std::memcpy(vendor + 4, &regs[3], 4);
            std::memcpy(vendor + 8, &regs[2], 4);
            cpu.Vendor = vendor;
            __cpuid(regs, 1);
            cpu.Hypervisor = (static_cast<unsigned>(regs[2]) >> 31) & 1u;
            if (cpu.Hypervisor)
            {
                __cpuid(regs, 0x40000000);
                char name[13] = {};
                std::memcpy(name, &regs[1], 4);
                std::memcpy(name + 4, &regs[2], 4);
                std::memcpy(name + 8, &regs[3], 4);
                cpu.HypervisorName = Trim(name);
            }
            __cpuid(regs, static_cast<int>(0x80000000));
            if (static_cast<unsigned>(regs[0]) >= 0x80000004u)
            {
                char brand[49] = {};
                for (int leaf = 0; leaf < 3; ++leaf)
                {
                    __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(leaf)));
                    std::memcpy(brand + leaf * 16, regs, 16);
                }
                cpu.Brand = Trim(brand);
            }
            DWORD length = 0;
            ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
            std::vector<std::uint8_t> buffer(length);
            if (length && ::GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length))
            {
                BYTE highest = 0;
                std::vector<BYTE> classes;
                for (DWORD at = 0; at < length;)
                {
                    const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
                    ++cpu.Cores;
                    classes.push_back(entry->Processor.EfficiencyClass);
                    highest = std::max(highest, entry->Processor.EfficiencyClass);
                    for (WORD group = 0; group < entry->Processor.GroupCount; ++group)
                        cpu.Threads += std::popcount(static_cast<std::uint64_t>(entry->Processor.GroupMask[group].Mask));
                    at += entry->Size;
                }
                if (highest > 0)
                    cpu.PerformanceCores = static_cast<int>(std::count(classes.begin(), classes.end(), highest));
            }
            std::uint8_t revision[8] = {};
            DWORD size = sizeof(revision);
            if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"Update Revision", RRF_RT_REG_BINARY, nullptr, revision, &size) == ERROR_SUCCESS &&
                size >= 8)
            {
                std::uint32_t high = 0;
                std::memcpy(&high, revision + 4, 4);
                std::uint32_t low = 0;
                std::memcpy(&low, revision, 4);
                cpu.Microcode = high ? high : low;
            }
            return cpu;
        }

        bool AffectedIntelDesktop(const std::string& brand)
        {
            if (brand.find("13th Gen") == std::string::npos && brand.find("14th Gen") == std::string::npos)
                return false;
            const std::size_t at = brand.find("i9-");
            const std::size_t i7 = brand.find("i7-");
            const std::size_t i5 = brand.find("i5-");
            std::size_t model = at != std::string::npos ? at : i7 != std::string::npos ? i7 : i5;
            if (model == std::string::npos)
                return false;
            model += 3;
            std::size_t end = model;
            while (end < brand.size() && std::isalnum(static_cast<unsigned char>(brand[end])))
                ++end;
            const std::string name = brand.substr(model, end - model);
            if (name.size() < 5)
                return false;
            const std::string suffix = name.substr(5);
            return suffix.empty() || suffix == "K" || suffix == "KF" || suffix == "KS" || suffix == "F" || suffix == "T";
        }

        std::string PowerPlan()
        {
            const std::string guid = Lower(RegistryText(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes", L"ActivePowerScheme"));
            if (guid.empty())
                return {};
            if (guid == "a1841308-3541-4fab-bc81-f71556f20b4a")
                return "Power saver";
            if (guid == "381b4222-f694-41f0-9685-ff5bb260df2e")
                return "Balanced";
            if (guid == "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c")
                return "High performance";
            if (guid == "e9a42b02-d5df-448d-aa00-03f14749eb61")
                return "Ultimate performance";
            const std::wstring key = L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes\\" + Utf8ToWide(guid);
            std::string name = RegistryText(HKEY_LOCAL_MACHINE, key.c_str(), L"FriendlyName");
            if (!name.empty() && name[0] == '@')
            {
                const std::size_t comma = name.rfind(',');
                name = comma == std::string::npos ? std::string() : Trim(name.substr(comma + 1));
            }
            return name.empty() ? "Custom plan (" + guid + ")" : name + " (custom plan)";
        }

        std::optional<double> TimerResolutionMs()
        {
            const auto query = reinterpret_cast<NtQueryTimerResolutionFunction>(NtFunction("NtQueryTimerResolution"));
            ULONG coarsest = 0;
            ULONG finest = 0;
            ULONG current = 0;
            if (!query || query(&coarsest, &finest, &current) != 0)
                return std::nullopt;
            return current / 10000.0;
        }

        std::optional<bool> HvciRunning()
        {
            struct CodeIntegrity
            {
                ULONG Length;
                ULONG Options;
            } info{ sizeof(CodeIntegrity), 0 };
            const auto query = reinterpret_cast<NtQuerySystemInformationFunction>(NtFunction("NtQuerySystemInformation"));
            if (!query || query(103, &info, sizeof(info), nullptr) != 0)
                return std::nullopt;
            return (info.Options & 0x400) != 0;
        }

        std::string DpiAwareness()
        {
            const DPI_AWARENESS_CONTEXT context = ::GetThreadDpiAwarenessContext();
            if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                return "Per monitor (v2)";
            switch (::GetAwarenessFromDpiAwarenessContext(context))
            {
            case DPI_AWARENESS_PER_MONITOR_AWARE: return "Per monitor";
            case DPI_AWARENESS_SYSTEM_AWARE: return "System";
            case DPI_AWARENESS_UNAWARE: return "Unaware";
            default: return "Unknown";
            }
        }

        struct DisplayInfo
        {
            int RefreshHz = 0;
            int Width = 0;
            int Height = 0;
            unsigned Dpi = 0;
        };

        DisplayInfo ReadDisplay()
        {
            DisplayInfo display;
            HWND window = static_cast<HWND>(GraphicsStore().Window);
            HMONITOR monitor = window ? ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (monitor && ::GetMonitorInfoW(monitor, &info))
            {
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
                {
                    display.RefreshHz = static_cast<int>(mode.dmDisplayFrequency);
                    display.Width = static_cast<int>(mode.dmPelsWidth);
                    display.Height = static_cast<int>(mode.dmPelsHeight);
                }
            }
            if (window)
                display.Dpi = ::GetDpiForWindow(window);
            return display;
        }

        struct LoadedModule
        {
            std::string Name;
            std::string Path;
            std::string Kind;
        };

        bool StartsWithFolder(const std::wstring& path, const std::wstring& folder)
        {
            if (folder.empty() || path.size() < folder.size())
                return false;
            return ::CompareStringOrdinal(path.c_str(), static_cast<int>(folder.size()), folder.c_str(), static_cast<int>(folder.size()), TRUE) == CSTR_EQUAL;
        }

        std::vector<LoadedModule> ForeignModules()
        {
            std::vector<LoadedModule> found;
            HMODULE modules[1024];
            DWORD needed = 0;
            if (!::EnumProcessModules(::GetCurrentProcess(), modules, sizeof(modules), &needed))
                return found;
            wchar_t windows[MAX_PATH] = L"";
            ::GetWindowsDirectoryW(windows, MAX_PATH);
            std::wstring windowsFolder = windows;
            if (!windowsFolder.empty() && windowsFolder.back() != L'\\')
                windowsFolder += L'\\';
            std::wstring programFolder = Paths::ProgramFolder().wstring();
            if (!programFolder.empty() && programFolder.back() != L'\\')
                programFolder += L'\\';
            const std::size_t count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
            for (std::size_t i = 0; i < count; ++i)
            {
                wchar_t path[MAX_PATH * 2] = L"";
                if (!::GetModuleFileNameW(modules[i], path, static_cast<DWORD>(std::size(path))))
                    continue;
                const std::wstring file = path;
                if (StartsWithFolder(file, windowsFolder) || ContainsNoCase(WideToUtf8(file), "\\DriverStore\\"))
                    continue;
                LoadedModule module;
                module.Path = WideToUtf8(file);
                module.Name = WideToUtf8(std::filesystem::path(file).filename().wstring());
                module.Kind = StartsWithFolder(file, programFolder) ? "program" : "other";
                if (module.Kind == "program" && (EqualsNoCase(module.Name, "Manifold-Engine.exe") || EqualsNoCase(module.Name, "Manifold-Engine.Tests.exe")))
                    continue;
                found.push_back(std::move(module));
            }
            return found;
        }

        const char* KnownHook(const std::string& name)
        {
            static const std::pair<const char*, const char*> kHooks[] = {
                { "RTSSHooks64.dll", "RivaTuner Statistics Server" },
                { "RTSSHooks.dll", "RivaTuner Statistics Server" },
                { "DiscordHook64.dll", "Discord overlay" },
                { "GameOverlayRenderer64.dll", "Steam overlay" },
                { "graphics-hook64.dll", "OBS game capture" },
                { "ReShade64.dll", "ReShade" },
                { "dxgi.dll", "a replacement dxgi.dll (ReShade, ENB or a mod loader)" },
                { "d3d11.dll", "a replacement d3d11.dll (ReShade, ENB or a mod loader)" },
                { "SbieDll.dll", "Sandboxie" },
                { "MSIAfterburner.dll", "MSI Afterburner" },
            };
            for (const auto& [file, product] : kHooks)
            {
                if (EqualsNoCase(name, file))
                    return product;
            }
            return nullptr;
        }

        int CountFiles(const std::filesystem::path& folder)
        {
            std::error_code error;
            int count = 0;
            for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
            {
                if (it->is_regular_file(error))
                    ++count;
            }
            return count;
        }

        bool FolderWritable(const std::filesystem::path& folder)
        {
            const std::filesystem::path probe = folder / (L"Manifold-Engine.write-check-" + std::to_wstring(::GetCurrentProcessId()) + L".tmp");
            const HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return ::GetLastError() == ERROR_FILE_EXISTS;
            ::CloseHandle(file);
            return true;
        }

        std::string Uptime(double seconds)
        {
            const long long total = static_cast<long long>(seconds);
            char text[48];
            if (total >= 3600)
                std::snprintf(text, sizeof(text), "%lld h %02lld min", total / 3600, (total / 60) % 60);
            else if (total >= 60)
                std::snprintf(text, sizeof(text), "%lld min %02lld s", total / 60, total % 60);
            else
                std::snprintf(text, sizeof(text), "%lld s", total);
            return text;
        }

        std::string ModuleOf(void* address)
        {
            HMODULE module = nullptr;
            if (!address || !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
                return address ? Hex(reinterpret_cast<std::uintptr_t>(address)) : std::string();
            wchar_t path[MAX_PATH] = L"";
            ::GetModuleFileNameW(module, path, MAX_PATH);
            const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module);
            return WideToUtf8(std::filesystem::path(path).filename().wstring()) + "+" + Hex(offset);
        }

        std::string CompilerText()
        {
#if defined(__clang__)
            return Trim(std::string("clang-cl ") + __clang_version__);
#elif defined(_MSC_VER)
            return "MSVC " + std::to_string(_MSC_VER);
#else
            return "unknown";
#endif
        }

        std::string BuildKind()
        {
#if defined(NDEBUG)
            const char* kind = "Release";
#else
            const char* kind = "Debug";
#endif
#if defined(_WIN64)
            return std::string(kind) + ", x64";
#else
            return std::string(kind) + ", x86";
#endif
        }

        bool DebugBuild()
        {
#if defined(NDEBUG)
            return false;
#else
            return true;
#endif
        }

        std::string VendorName(std::uint32_t vendor)
        {
            switch (vendor)
            {
            case kVendorNvidia: return "NVIDIA";
            case kVendorAmd: return "AMD";
            case kVendorIntel: return "Intel";
            case 0x1414: return "Microsoft";
            default: return Hex(vendor);
            }
        }

        void Add(std::vector<Check>& checks, Level level, const char* area, std::string title, std::string detail, std::string fix = {}, Action action = Action::None)
        {
            checks.push_back({ level, area, std::move(title), std::move(detail), std::move(fix), action });
        }
    }

    void SetGraphics(GraphicsInfo info)
    {
        info.Known = true;
        GraphicsStore() = std::move(info);
    }

    const GraphicsInfo& Graphics()
    {
        return GraphicsStore();
    }

    const char* LevelName(Level level)
    {
        switch (level)
        {
        case Level::Ok: return "ok";
        case Level::Info: return "info";
        case Level::Warning: return "warning";
        case Level::Problem: return "problem";
        }
        return "";
    }

    std::string Bytes(std::uint64_t bytes)
    {
        const double value = static_cast<double>(bytes);
        if (bytes >= (1ull << 30))
            return Fixed(value / (1ull << 30), 2) + " GB";
        if (bytes >= (1ull << 20))
            return Fixed(value / (1ull << 20), 1) + " MB";
        if (bytes >= (1ull << 10))
            return Fixed(value / (1ull << 10), 0) + " KB";
        return std::to_string(bytes) + " bytes";
    }

    std::string Milliseconds(double ms)
    {
        if (ms >= 100.0)
            return Fixed(ms, 0) + " ms";
        if (ms >= 10.0)
            return Fixed(ms, 1) + " ms";
        if (ms >= 0.1)
            return Fixed(ms, 2) + " ms";
        return Fixed(ms * 1000.0, 0) + " us";
    }

    void Sample(bool threads)
    {
        SamplerState& state = Sampler();
        const std::uint64_t wall = WallNow();
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
        if (!state.Started)
            state.Started = FileTimeValue(created);
        const std::uint64_t cpu = FileTimeValue(kernel) + FileTimeValue(user);
        ProcessSample& sample = state.Last;
        if (state.Primed && wall > state.LastWall)
        {
            const double share = static_cast<double>(cpu - state.LastCpu) / static_cast<double>(wall - state.LastWall);
            SYSTEM_INFO system{};
            ::GetSystemInfo(&system);
            sample.CpuOfOneCore = share * 100.0;
            sample.CpuOfSystem = share * 100.0 / std::max<DWORD>(1, system.dwNumberOfProcessors);
            sample.Valid = true;
        }
        state.Primed = true;
        state.LastWall = wall;
        state.LastCpu = cpu;

        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        {
            sample.PrivateBytes = memory.PrivateUsage;
            sample.PeakPrivateBytes = memory.PeakPagefileUsage;
            sample.WorkingSet = memory.WorkingSetSize;
            sample.PeakWorkingSet = memory.PeakWorkingSetSize;
        }
        DWORD handles = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &handles);
        sample.Handles = handles;
        sample.GdiObjects = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
        sample.UserObjects = ::GetGuiResources(::GetCurrentProcess(), GR_USEROBJECTS);
        sample.UptimeSeconds = state.Started && wall > state.Started ? static_cast<double>(wall - state.Started) / 1.0e7 : 0.0;

        const auto nextThread = reinterpret_cast<NtGetNextThreadFunction>(NtFunction("NtGetNextThread"));
        if (!nextThread)
            return;
        int count = 0;
        std::vector<ThreadSample> list;
        std::map<DWORD, std::uint64_t> times;
        const auto queryThread = reinterpret_cast<NtQueryInformationThreadFunction>(NtFunction("NtQueryInformationThread"));
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        const auto describe = kernel32 ? reinterpret_cast<GetThreadDescriptionFunction>(reinterpret_cast<void*>(::GetProcAddress(kernel32, "GetThreadDescription"))) : nullptr;
        const DWORD mainThread = ::GetCurrentThreadId();
        HANDLE thread = nullptr;
        for (HANDLE next = nullptr; nextThread(::GetCurrentProcess(), thread, THREAD_QUERY_LIMITED_INFORMATION | THREAD_QUERY_INFORMATION, 0, 0, &next) == 0;)
        {
            if (thread)
                ::CloseHandle(thread);
            thread = next;
            ++count;
            if (!threads)
                continue;
            ThreadSample info;
            info.Id = ::GetThreadId(thread);
            info.Main = info.Id == mainThread;
            FILETIME a{};
            FILETIME b{};
            FILETIME k{};
            FILETIME u{};
            if (::GetThreadTimes(thread, &a, &b, &k, &u))
            {
                const std::uint64_t used = FileTimeValue(k) + FileTimeValue(u);
                times[info.Id] = used;
                info.CpuSeconds = static_cast<double>(used) / 1.0e7;
                const auto before = state.ThreadCpu.find(info.Id);
                if (before != state.ThreadCpu.end() && state.ThreadWall && wall > state.ThreadWall && used >= before->second)
                    info.CpuOfOneCore = static_cast<double>(used - before->second) * 100.0 / static_cast<double>(wall - state.ThreadWall);
            }
            info.Priority = ::GetThreadPriority(thread);
            void* start = nullptr;
            if (queryThread && queryThread(thread, 9, &start, sizeof(start), nullptr) == 0)
                info.Start = ModuleOf(start);
            if (describe)
            {
                PWSTR text = nullptr;
                if (SUCCEEDED(describe(thread, &text)) && text)
                {
                    info.Description = WideToUtf8(text);
                    ::LocalFree(text);
                }
            }
            if (info.Main && info.Description.empty())
                info.Description = "Main thread (windows, Lua)";
            list.push_back(std::move(info));
        }
        if (thread)
            ::CloseHandle(thread);
        sample.Threads = count;
        if (threads)
        {
            std::sort(list.begin(), list.end(), [](const ThreadSample& a, const ThreadSample& b) {
                if (a.CpuOfOneCore != b.CpuOfOneCore)
                    return a.CpuOfOneCore > b.CpuOfOneCore;
                return a.CpuSeconds > b.CpuSeconds;
            });
            state.Threads = std::move(list);
            state.ThreadCpu = std::move(times);
            state.ThreadWall = wall;
        }
    }

    const ProcessSample& LastSample()
    {
        return Sampler().Last;
    }

    const std::vector<ThreadSample>& LastThreads()
    {
        return Sampler().Threads;
    }

    std::vector<InfoGroup> CollectInfo(Engine& engine)
    {
        std::vector<InfoGroup> groups;
        const Settings& settings = engine.Options();

        InfoGroup build{ "Build", {} };
        build.Rows.push_back({ "Version", MANIFOLD_VERSION_TEXT });
        build.Rows.push_back({ "Build", BuildKind() });
        build.Rows.push_back({ "Compiler", CompilerText() });
        build.Rows.push_back({ "Built", std::string(__DATE__) + " " + __TIME__ });
        build.Rows.push_back({ "Dear ImGui", IMGUI_VERSION });
        build.Rows.push_back({ "Lua", LUA_RELEASE });
        build.Rows.push_back({ "Program folder", WideToUtf8(Paths::ProgramFolder().wstring()) });
        build.Rows.push_back({ "Settings file", WideToUtf8(Paths::SettingsFile().wstring()) });
        groups.push_back(std::move(build));

        InfoGroup system{ "System", {} };
        const WindowsVersion windows = ReadWindowsVersion();
        std::string version = windows.Name;
        if (!windows.Release.empty())
            version += " " + windows.Release;
        version += " (build " + std::to_string(windows.Build) + (windows.Revision ? "." + std::to_string(windows.Revision) : std::string()) + ")";
        system.Rows.push_back({ "Windows", version });
        const CpuInfo cpu = ReadCpu();
        system.Rows.push_back({ "Processor", cpu.Brand.empty() ? cpu.Vendor : cpu.Brand });
        std::string cores = std::to_string(cpu.Cores) + " cores";
        if (cpu.PerformanceCores > 0 && cpu.PerformanceCores < cpu.Cores)
            cores += " (" + std::to_string(cpu.PerformanceCores) + " performance, " + std::to_string(cpu.Cores - cpu.PerformanceCores) + " efficient)";
        cores += ", " + std::to_string(cpu.Threads) + " threads";
        system.Rows.push_back({ "Cores", cores });
        if (cpu.Microcode)
            system.Rows.push_back({ "Microcode", Hex(*cpu.Microcode) });
        MEMORYSTATUSEX memory{ sizeof(memory) };
        if (::GlobalMemoryStatusEx(&memory))
        {
            system.Rows.push_back({ "Memory", Bytes(memory.ullTotalPhys) + " (" + Bytes(memory.ullAvailPhys) + " free, " + std::to_string(memory.dwMemoryLoad) + " % in use)" });
        }
        system.Rows.push_back({ "Hypervisor", cpu.Hypervisor ? (cpu.HypervisorName.empty() ? std::string("yes") : cpu.HypervisorName) : std::string("none") });
        if (const auto hvci = HvciRunning())
            system.Rows.push_back({ "Memory integrity (HVCI)", *hvci ? "on" : "off" });
        if (const std::string plan = PowerPlan(); !plan.empty())
            system.Rows.push_back({ "Power plan", plan });
        if (const auto resolution = TimerResolutionMs())
            system.Rows.push_back({ "Timer resolution", Fixed(*resolution, 2) + " ms" });
        system.Rows.push_back({ "Administrator", Target::IsElevated() ? "yes" : "no" });
        groups.push_back(std::move(system));

        InfoGroup graphics{ "Graphics", {} };
        const GraphicsInfo& gpu = Graphics();
        if (gpu.Known)
        {
            graphics.Rows.push_back({ "Adapter", gpu.Device.Name + (gpu.Warp ? " (software)" : "") });
            graphics.Rows.push_back({ "Vendor", VendorName(gpu.Device.Vendor) });
            graphics.Rows.push_back({ "Video memory", Bytes(gpu.Device.VideoMemory) });
            if (!gpu.Driver.empty())
                graphics.Rows.push_back({ "Driver", gpu.Driver });
            graphics.Rows.push_back({ "Feature level", gpu.FeatureLevel });
            graphics.Rows.push_back({ "Swap chains", gpu.SwapEffect });
            for (std::size_t i = 0; i < gpu.Adapters.size(); ++i)
            {
                const Adapter& other = gpu.Adapters[i];
                graphics.Rows.push_back({ "Adapter " + std::to_string(i), other.Name + " (" + Bytes(other.VideoMemory) + (other.Software ? ", software" : "") + ")" });
            }
        }
        const DisplayInfo display = ReadDisplay();
        if (display.Width)
            graphics.Rows.push_back({ "Display", std::to_string(display.Width) + " x " + std::to_string(display.Height) + " at " + std::to_string(display.RefreshHz) + " Hz" });
        if (display.Dpi)
            graphics.Rows.push_back({ "Scale", std::to_string(display.Dpi * 100 / 96) + " % (" + std::to_string(display.Dpi) + " dpi)" });
        graphics.Rows.push_back({ "DPI awareness", DpiAwareness() });
        groups.push_back(std::move(graphics));

        InfoGroup self{ "This program", {} };
        const ProcessSample& sample = LastSample();
        self.Rows.push_back({ "Process id", std::to_string(::GetCurrentProcessId()) });
        if (sample.UptimeSeconds > 0.0)
            self.Rows.push_back({ "Running for", Uptime(sample.UptimeSeconds) });
        if (sample.Valid)
            self.Rows.push_back({ "Processor use", Percent(sample.CpuOfOneCore) + " of one core (" + Percent(sample.CpuOfSystem) + " of all)" });
        self.Rows.push_back({ "Private memory", Bytes(sample.PrivateBytes) + " (peak " + Bytes(sample.PeakPrivateBytes) + ")" });
        self.Rows.push_back({ "Working set", Bytes(sample.WorkingSet) + " (peak " + Bytes(sample.PeakWorkingSet) + ")" });
        self.Rows.push_back({ "Threads", std::to_string(sample.Threads) });
        self.Rows.push_back({ "Handles", std::to_string(sample.Handles) });
        self.Rows.push_back({ "GDI / USER objects", std::to_string(sample.GdiObjects) + " / " + std::to_string(sample.UserObjects) });
        const DWORD priority = ::GetPriorityClass(::GetCurrentProcess());
        self.Rows.push_back({ "Priority", priority == HIGH_PRIORITY_CLASS ? "High" : priority == ABOVE_NORMAL_PRIORITY_CLASS ? "Above normal" : priority == BELOW_NORMAL_PRIORITY_CLASS ? "Below normal" : priority == IDLE_PRIORITY_CLASS ? "Idle" : priority == REALTIME_PRIORITY_CLASS ? "Realtime" : "Normal" });
        self.Rows.push_back({ "Debugger attached to it", ::IsDebuggerPresent() ? "yes" : "no" });
        groups.push_back(std::move(self));

        InfoGroup state{ "Engine", {} };
        if (engine.HasProcess())
        {
            const auto& process = engine.Process();
            state.Rows.push_back({ "Process", process->Name() + " (" + std::to_string(process->Id()) + ", " + (process->Is64Bit() ? "64-bit" : "32-bit") + ")" });
        }
        else
        {
            state.Rows.push_back({ "Process", "none" });
        }
        std::size_t records = 0;
        std::size_t active = 0;
        std::size_t frozen = 0;
        std::size_t hotkeys = 0;
        engine.Table().ForEach([&](Table::MemoryRecord& record) {
            ++records;
            if (record.Active && record.Kind == Table::RecordKind::Script)
                ++active;
            else if (record.Active && record.Kind == Table::RecordKind::Value)
                ++frozen;
            hotkeys += record.Hotkeys.size();
        });
        state.Rows.push_back({ "Records", std::to_string(records) + " (" + std::to_string(active) + " scripts on, " + std::to_string(frozen) + " frozen, " + std::to_string(hotkeys) + " hotkeys)" });
        state.Rows.push_back({ "Scanner", engine.Scanner().Busy() ? "scanning" : engine.Scanner().HasResults() ? std::to_string(engine.Scanner().Count()) + " results" : "no scan" });
        const Debug::Debugger& debugger = engine.Debugger();
        state.Rows.push_back({ "Debugger", debugger.Attached() ? std::string(debugger.UsingVeh() ? "in-process (VEH)" : "Windows") + (debugger.Broken() ? ", stopped at a break" : ", running") : std::string("not attached") });
        state.Rows.push_back({ "Symbols", engine.SymbolsLoaded() ? "loaded (" + std::to_string(engine.SymbolsModulesDone()) + " modules)" : engine.SymbolsActive() ? "loading" : "exports only" });
        state.Rows.push_back({ "Mono runtime", engine.MonoPresent() ? "attached" : "no" });
        state.Rows.push_back({ "Background work", engine.Jobs().Busy() || engine.Working() ? "busy" : "idle" });
        groups.push_back(std::move(state));

        InfoGroup lua{ "Lua", {} };
        lua_State* L = engine.Lua().State();
        if (L)
        {
            const double kilobytes = lua_gc(L, LUA_GCCOUNT, 0) + lua_gc(L, LUA_GCCOUNTB, 0) / 1024.0;
            lua.Rows.push_back({ "Memory", Bytes(static_cast<std::uint64_t>(kilobytes * 1024.0)) });
        }
        int timers = 0;
        int fastest = 0;
        for (const auto& [id, timer] : engine.Lua().Timers())
        {
            if (timer.Destroyed || !timer.Enabled || timer.Callback <= 0)
                continue;
            ++timers;
            fastest = fastest == 0 ? timer.Interval : std::min(fastest, timer.Interval);
        }
        lua.Rows.push_back({ "Timers running", std::to_string(timers) + (timers ? " (fastest every " + std::to_string(fastest) + " ms)" : std::string()) });
        lua.Rows.push_back({ "Tasks", std::to_string(engine.Lua().TaskCount()) });
        lua.Rows.push_back({ "Output lines", std::to_string(engine.Lua().OutputCount()) });
        lua.Rows.push_back({ "Autorun", settings.AutorunEnabled ? std::to_string(CountFiles(Paths::AutorunFolder())) + " files in the autorun folder" : std::string("off") });
        groups.push_back(std::move(lua));

        InfoGroup options{ "Settings", {} };
        options.Rows.push_back({ "Address list refresh", std::to_string(settings.UpdateIntervalMs) + " ms" });
        options.Rows.push_back({ "Freeze interval", std::to_string(settings.FreezeIntervalMs) + " ms" });
        options.Rows.push_back({ "Found list refresh", std::to_string(settings.ResultValueRefreshMs) + " ms" });
        options.Rows.push_back({ "Scan threads", settings.ScanThreads == 0 ? std::string("auto") : std::to_string(settings.ScanThreads) });
        options.Rows.push_back({ "Mode", std::string(settings.ReleaseMode ? "release" : "developer") + (settings.CompactMode ? ", compact" : "") });
        options.Rows.push_back({ "Debugger", settings.UseInProcessDebugger ? "in-process (VEH)" : "Windows" });
        options.Rows.push_back({ "Profiler", Profiler::Enabled() ? "on" : "off" });
        groups.push_back(std::move(options));
        return groups;
    }

    std::vector<Check> RunChecks(Engine& engine)
    {
        std::vector<Check> checks;
        const Settings& settings = engine.Options();

        if (DebugBuild())
            Add(checks, Level::Warning, "Build", "This is a debug build", "Debug builds run several times slower than release builds; frame times and scan speeds are not representative.",
                "Build and run the Release configuration.");
        else
            Add(checks, Level::Ok, "Build", "Release build", BuildKind() + ", " + CompilerText() + ".");

        if (Target::IsElevated())
            Add(checks, Level::Ok, "Rights", "Runs as administrator", "Protected and elevated processes can be opened.");
        else
            Add(checks, Level::Warning, "Rights", "No administrator rights", "Games that run as administrator, and some with protection, cannot be opened.",
                "Restart the program as administrator.", Action::RestartElevated);

        bool writable = false;
        {
            PROFILE_ZONE("Checks: program folder");
            writable = FolderWritable(Paths::ProgramFolder());
        }
        if (writable)
            Add(checks, Level::Ok, "Files", "The program folder is writable", "Settings, layout, log and tables can be saved next to the program.");
        else
            Add(checks, Level::Problem, "Files", "The program folder is not writable",
                "Settings, the window layout, the log and saved tables are written next to the program, in " + WideToUtf8(Paths::ProgramFolder().wstring()) + ".",
                "Move the program to a folder you can write to, such as one in your user folder, or run it as administrator.");

        const GraphicsInfo& gpu = Graphics();
        if (gpu.Known)
        {
            if (gpu.Warp || gpu.Device.Software)
                Add(checks, Level::Problem, "Graphics", "Drawing runs on the processor", "No usable graphics driver was found, so Windows draws with its software renderer (WARP). The interface is slow and uses a lot of processor time.",
                    "Install or repair the graphics driver.");
            else
                Add(checks, Level::Ok, "Graphics", "Hardware drawing", gpu.Device.Name + ", " + gpu.FeatureLevel + ", " + gpu.SwapEffect + ".");
            const Adapter* bigger = nullptr;
            for (const Adapter& other : gpu.Adapters)
            {
                if (!other.Software && other.VideoMemory > gpu.Device.VideoMemory * 2 && other.Name != gpu.Device.Name)
                    bigger = &other;
            }
            if (bigger && gpu.Device.Vendor == kVendorIntel)
                Add(checks, Level::Info, "Graphics", "Runs on the integrated graphics", "The window is drawn by " + gpu.Device.Name + " while " + bigger->Name + " is also installed. That is fine for this program; it saves power.",
                    "To use the other one: Windows Settings > System > Display > Graphics, add Manifold-Engine.exe and pick High performance.");
            if (!gpu.SwapEffect.empty() && gpu.SwapEffect.find("flip") == std::string::npos)
                Add(checks, Level::Warning, "Graphics", "Old swap model", "Tool windows use the blt model, which costs memory while they are resized.", {});
        }

        DisplayInfo display;
        {
            PROFILE_ZONE("Checks: display");
            display = ReadDisplay();
        }
        if (display.RefreshHz > 0)
            Add(checks, Level::Info, "Graphics", "Frames follow the display", "Drawing waits for the display, at most " + std::to_string(display.RefreshHz) + " frames per second while you work, and about 10 per second when nothing happens.");

        std::vector<LoadedModule> foreign;
        {
            PROFILE_ZONE("Checks: loaded libraries");
            foreign = ForeignModules();
        }
        std::string hooks;
        std::string others;
        int otherCount = 0;
        for (const LoadedModule& module : foreign)
        {
            if (const char* product = KnownHook(module.Name))
                hooks += (hooks.empty() ? "" : "; ") + module.Name + " (" + product + ") from " + module.Path;
            else if (module.Kind == "other")
            {
                if (otherCount < 8)
                    others += (others.empty() ? "" : ", ") + module.Name;
                ++otherCount;
            }
        }
        if (!hooks.empty())
            Add(checks, Level::Warning, "Environment", "Overlays or hooks are loaded into the program",
                "These libraries hook drawing inside Manifold: " + hooks + ". They can slow down drawing, stretch frame times and crash tool windows.",
                "Exclude Manifold-Engine.exe in the overlay's settings, or remove the replacement DLL from the program folder.");
        else
            Add(checks, Level::Ok, "Environment", "No overlays hook the program", "No known overlay, capture hook or replacement graphics DLL is loaded.");
        if (otherCount > 0)
            Add(checks, Level::Info, "Environment", std::to_string(otherCount) + (otherCount == 1 ? " library from outside Windows is loaded" : " libraries from outside Windows are loaded"),
                others + (otherCount > 8 ? ", ..." : "") + ". Antivirus programs, shell extensions and input tools load such libraries into every program; they are listed in case the program misbehaves.");

        CpuInfo cpu;
        {
            PROFILE_ZONE("Checks: processor");
            cpu = ReadCpu();
        }
        if (cpu.Hypervisor)
        {
            std::string detail = "Windows runs on a hypervisor (" + (cpu.HypervisorName.empty() ? std::string("unknown") : cpu.HypervisorName) + "), for example for virtualization-based security";
            if (const auto hvci = HvciRunning(); hvci && *hvci)
                detail += " with memory integrity on";
            detail += ". Hardware breakpoints set through Windows do not always stick there; the debugger arms them again, and the in-process debugger is the more reliable choice for watches.";
            Add(checks, Level::Info, "Debugger", "A hypervisor is active", detail);
        }
        if (cpu.Microcode && AffectedIntelDesktop(cpu.Brand))
        {
            if (*cpu.Microcode >= kVminFixMicrocode)
                Add(checks, Level::Ok, "Processor", "Processor microcode is up to date", cpu.Brand + " runs microcode " + Hex(*cpu.Microcode) + ", which includes Intel's fix for the 13th and 14th generation instability (0x12B or newer).");
            else
                Add(checks, Level::Warning, "Processor", "Processor microcode is out of date",
                    cpu.Brand + " runs microcode " + Hex(*cpu.Microcode) + ". Intel's fix for the instability of 13th and 14th generation desktop processors came with 0x12B; older microcode can cause random crashes of any program under load.",
                    "Update the mainboard BIOS to a version with microcode 0x12B or newer.");
        }

        const std::string plan = PowerPlan();
        if (plan == "Power saver")
            Add(checks, Level::Info, "Processor", "Power saver plan", "Scans and Lua scripts run slower with the power saver plan.", "Switch to Balanced or High performance in the Windows power settings.");

        MEMORYSTATUSEX memory{ sizeof(memory) };
        if (::GlobalMemoryStatusEx(&memory) && memory.dwMemoryLoad >= 90)
            Add(checks, Level::Warning, "Memory", "The system is low on memory", std::to_string(memory.dwMemoryLoad) + " % of " + Bytes(memory.ullTotalPhys) + " is in use. Scans that store many results and big tables can fail or page to disk.",
                "Close other programs, or scan for fewer value types at once.");

        const ProcessSample& sample = LastSample();
        if (sample.PrivateBytes > (3ull << 29))
            Add(checks, Level::Warning, "Memory", "The program uses a lot of memory", Bytes(sample.PrivateBytes) + " of private memory. Large scans, big Lua data or a leak in a script can cause this.",
                "Start a new scan, or look at the Lua memory under App info.");
        if (sample.Handles > 20000)
            Add(checks, Level::Warning, "Memory", "Many handles are open", std::to_string(sample.Handles) + " handles. A script or a tool that opens handles without closing them can cause this.");
        if (sample.GdiObjects > 5000 || sample.UserObjects > 5000)
            Add(checks, Level::Warning, "Memory", "Many window objects", std::to_string(sample.GdiObjects) + " GDI and " + std::to_string(sample.UserObjects) + " USER objects; Windows stops a program at 10000.");

        const Profiler::FrameSummary frames = Profiler::Summary();
        if (!Profiler::Enabled())
        {
            Add(checks, Level::Info, "Performance", "The profiler is off", "Frame times are only measured while the profiler is on. It costs a few microseconds per frame.", "Turn on the profiler.", Action::TurnOnProfiler);
        }
        else if (frames.Frames >= 30)
        {
            const std::string timing = "Average work per frame " + Milliseconds(frames.WorkMs) + ", 95 % of the frames under " + Milliseconds(frames.WorkP95) + ", slowest " + Milliseconds(frames.WorkMax) + ".";
            std::string slowest;
            double slowestMs = 0.0;
            for (const Profiler::ZoneView& zone : Profiler::Tree())
            {
                if (zone.Depth >= 1 && !zone.Wait && zone.SelfMs > slowestMs)
                {
                    slowestMs = zone.SelfMs;
                    slowest = zone.Name;
                }
            }
            if (frames.WorkP95 > 16.0)
                Add(checks, Level::Warning, "Performance", "Frames are slow", timing + (slowest.empty() ? std::string() : " Most of the time goes to " + slowest + " (" + Milliseconds(slowestMs) + " per frame)."),
                    "Open the Breakdown tab to see the zone that takes the time.");
            else
                Add(checks, Level::Ok, "Performance", "Frames are fast", timing);
            if (frames.Hitches > 0)
                Add(checks, Level::Warning, "Performance", std::to_string(frames.Hitches) + (frames.Hitches == 1 ? " stutter" : " stutters") + " in the last " + std::to_string(frames.Frames) + " frames",
                    "Frames that took longer than 50 ms. Loading a table, an AOB scan in a script or a slow Lua timer can cause them.", "Click the tall bars in the frame graph to see where the time went.");
        }

        if (sample.Valid && sample.CpuOfOneCore > 30.0 && !engine.Scanner().Busy() && !engine.Working())
        {
            std::string causes;
            int timers = 0;
            int fastest = 0;
            for (const auto& [id, timer] : engine.Lua().Timers())
            {
                if (timer.Destroyed || !timer.Enabled || timer.Callback <= 0)
                    continue;
                ++timers;
                fastest = fastest == 0 ? timer.Interval : std::min(fastest, timer.Interval);
            }
            if (timers)
                causes += std::to_string(timers) + " Lua timers (fastest every " + std::to_string(fastest) + " ms). ";
            if (engine.Debugger().Attached())
                causes += "The debugger is attached. ";
            if (settings.FreezeIntervalMs < 30)
                causes += "Frozen values are written every " + std::to_string(settings.FreezeIntervalMs) + " ms. ";
            Add(checks, Level::Warning, "Performance", "High processor use while idle", "The program uses " + Percent(sample.CpuOfOneCore) + " of one core without a scan running. " + causes,
                "Look at the Threads tab for the busy thread, and at Lua timers in the Breakdown tab.");
        }

        int fastTimers = 0;
        int fastest = 0;
        for (const auto& [id, timer] : engine.Lua().Timers())
        {
            if (timer.Destroyed || !timer.Enabled || timer.Callback <= 0 || timer.Interval >= 15)
                continue;
            ++fastTimers;
            fastest = fastest == 0 ? timer.Interval : std::min(fastest, timer.Interval);
        }
        if (fastTimers)
            Add(checks, Level::Info, "Lua", std::to_string(fastTimers) + (fastTimers == 1 ? " Lua timer is faster than a frame" : " Lua timers are faster than a frame"),
                "The fastest runs every " + std::to_string(fastest) + " ms, but timers are checked about every 15 ms; such a timer runs at most once per check and keeps the program awake.",
                "Use an interval of 16 ms or more, or a thread with createThread for tight loops.");
        if (lua_State* L = engine.Lua().State())
        {
            const int kilobytes = lua_gc(L, LUA_GCCOUNT, 0);
            if (kilobytes > 512 * 1024)
                Add(checks, Level::Warning, "Lua", "Lua uses a lot of memory", Bytes(static_cast<std::uint64_t>(kilobytes) * 1024) + " in the Lua state. A script may keep big tables or dumps alive.");
        }

        if (settings.UpdateIntervalMs < 50)
            Add(checks, Level::Info, "Settings", "The address list is read very often", "Every " + std::to_string(settings.UpdateIntervalMs) + " ms. With big tables this costs processor time.", "250 ms is the default, under Settings > General.", Action::OpenSettings);
        SYSTEM_INFO systemInfo{};
        ::GetSystemInfo(&systemInfo);
        if (settings.ScanThreads > static_cast<int>(systemInfo.dwNumberOfProcessors))
            Add(checks, Level::Info, "Settings", "More scan threads than processor threads", std::to_string(settings.ScanThreads) + " scan threads on " + std::to_string(systemInfo.dwNumberOfProcessors) + " processor threads; more do not scan faster.", "Set Scan threads to Auto under Settings > Scanner.", Action::OpenSettings);

        if (::IsDebuggerPresent())
            Add(checks, Level::Info, "Environment", "A debugger is attached to Manifold", "Drawing, Lua and exceptions run slower under a debugger.");

        const int crashes = CountFiles(Paths::CrashFolder());
        if (crashes > 0)
            Add(checks, Level::Info, "Files", std::to_string(crashes) + (crashes == 1 ? " crash report is stored" : " crash reports are stored"),
                "They are in " + WideToUtf8(Paths::CrashFolder().wstring()) + ". Each holds the moment the program crashed.", "Open the folder to look at them or to send them along with a bug report.", Action::OpenCrashFolder);
        std::error_code error;
        const auto logSize = std::filesystem::file_size(Paths::LogFile(), error);
        if (!error && logSize > (64ull << 20))
            Add(checks, Level::Info, "Files", "The log file is big", Bytes(logSize) + " in " + WideToUtf8(Paths::LogFile().wstring()) + ".", "Turn off Debug messages under Settings > Debugger if they are on.", Action::OpenSettings);

        const WindowsVersion windows = ReadWindowsVersion();
        if (windows.Build && windows.Build < 19041)
            Add(checks, Level::Warning, "Environment", "Old Windows version", windows.Name + " build " + std::to_string(windows.Build) + " is older than Windows 10 2004; the program is tested on Windows 10 2004 and newer.");

        std::stable_sort(checks.begin(), checks.end(), [](const Check& a, const Check& b) { return static_cast<int>(a.Severity) > static_cast<int>(b.Severity); });
        return checks;
    }

    std::string Report(Engine& engine, const std::vector<InfoGroup>& extra)
    {
        Sample(true);
        std::ostringstream out;
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[64];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
        out << "# Manifold Engine diagnostics\n\n";
        out << "Created " << stamp << " by Manifold Engine " << MANIFOLD_VERSION_TEXT << ".\n\n";

        out << "## Checks\n\n";
        for (const Check& check : RunChecks(engine))
        {
            out << "- [" << LevelName(check.Severity) << "] " << check.Area << ": " << check.Title << ". " << check.Detail << "\n";
            if (!check.Fix.empty() && check.Severity != Level::Ok)
                out << "  Fix: " << check.Fix << "\n";
        }

        std::vector<InfoGroup> groups = CollectInfo(engine);
        groups.insert(groups.end(), extra.begin(), extra.end());
        for (const InfoGroup& group : groups)
        {
            out << "\n## " << group.Title << "\n\n";
            for (const InfoRow& row : group.Rows)
                out << "- " << row.Key << ": " << row.Value << "\n";
        }

        if (Profiler::Frames() > 0)
        {
            const Profiler::FrameSummary frames = Profiler::Summary();
            out << "\n## Frames (last " << frames.Frames << ")\n\n";
            out << "- Frame period: " << Milliseconds(frames.PeriodMs) << " (" << Fixed(frames.FramesPerSecond, 1) << " frames per second), longest " << Milliseconds(frames.PeriodMax) << "\n";
            out << "- Work per frame: average " << Milliseconds(frames.WorkMs) << ", median " << Milliseconds(frames.WorkP50) << ", 95 % " << Milliseconds(frames.WorkP95) << ", 99 % "
                << Milliseconds(frames.WorkP99) << ", slowest " << Milliseconds(frames.WorkMax) << "\n";
            out << "- Stutters over 50 ms: " << frames.Hitches << "\n\n";
            out << "```\n";
            char line[256];
            std::snprintf(line, sizeof(line), "%-44s %10s %10s %10s %8s\n", "Zone", "avg ms", "self ms", "max ms", "calls");
            out << line;
            for (const Profiler::ZoneView& zone : Profiler::Tree())
            {
                if (zone.Depth > 0 && zone.MaxMs <= 0.0 && zone.CallsPerFrame <= 0.0)
                    continue;
                std::string name(static_cast<std::size_t>(zone.Depth) * 2, ' ');
                name += zone.Name;
                if (zone.Wait)
                    name += " (waiting)";
                std::snprintf(line, sizeof(line), "%-44s %10.3f %10.3f %10.3f %8.1f\n", name.c_str(), zone.AverageMs, zone.SelfMs, zone.MaxMs, zone.CallsPerFrame);
                out << line;
            }
            out << "```\n";
            const std::vector<Profiler::CounterView> counters = Profiler::Counters();
            if (!counters.empty())
            {
                out << "\n## Counters\n\n";
                for (const Profiler::CounterView& counter : counters)
                    out << "- " << counter.Name << ": " << Fixed(counter.Last, 0) << " (average " << Fixed(counter.Average, 1) << ", most " << Fixed(counter.Max, 0) << ")\n";
            }
        }

        const std::vector<ThreadSample>& threads = LastThreads();
        if (!threads.empty())
        {
            out << "\n## Threads\n\n```\n";
            char line[320];
            std::snprintf(line, sizeof(line), "%-8s %8s %10s  %-40s %s\n", "Id", "CPU %", "CPU s", "Start", "Description");
            out << line;
            for (std::size_t i = 0; i < threads.size() && i < 16; ++i)
            {
                const ThreadSample& thread = threads[i];
                std::snprintf(line, sizeof(line), "%-8lu %8.1f %10.2f  %-40s %s\n", thread.Id, thread.CpuOfOneCore, thread.CpuSeconds, thread.Start.c_str(), thread.Description.c_str());
                out << line;
            }
            if (threads.size() > 16)
                out << "... and " << threads.size() - 16 << " more threads\n";
            out << "```\n";
        }
        return out.str();
    }
}
