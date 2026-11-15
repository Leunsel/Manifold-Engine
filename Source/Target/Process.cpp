#include "Target/Process.h"

#include "Core/RuntimeLog.h"
#include "Core/Text.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <unordered_map>

#include <windows.h>
#include <tlhelp32.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint64_t kNearRange = 0x7FF00000ull;
        constexpr std::uint64_t kGranularity = 0x10000ull;

        using NtProcessCall = LONG(NTAPI*)(HANDLE);

        NtProcessCall NtCall(const char* name)
        {
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            return ntdll ? reinterpret_cast<NtProcessCall>(reinterpret_cast<void*>(::GetProcAddress(ntdll, name))) : nullptr;
        }

        std::string LastErrorText(DWORD error)
        {
            wchar_t* buffer = nullptr;
            const DWORD length = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
            std::string text = length && buffer ? Trim(WideToUtf8(std::wstring_view(buffer, length))) : "error " + std::to_string(error);
            if (buffer)
                ::LocalFree(buffer);
            while (!text.empty() && text.back() == '.')
                text.pop_back();
            return text;
        }

        bool WritableProtect(DWORD protect)
        {
            const DWORD base = protect & 0xFF;
            return base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
        }

        DWORD WritableFor(DWORD protect)
        {
            const DWORD base = protect & 0xFF;
            if (base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY)
                return PAGE_EXECUTE_READWRITE;
            return PAGE_READWRITE;
        }

        RegionInfo FromQuery(const MEMORY_BASIC_INFORMATION& info)
        {
            RegionInfo region;
            region.Base = reinterpret_cast<std::uint64_t>(info.BaseAddress);
            region.Size = info.RegionSize;
            region.AllocationBase = reinterpret_cast<std::uint64_t>(info.AllocationBase);
            region.Protect = info.Protect;
            region.AllocationProtect = info.AllocationProtect;
            region.State = info.State;
            region.Type = info.Type;
            return region;
        }

        bool IsSystemPath(const std::wstring& path)
        {
            wchar_t windows[MAX_PATH] = L"";
            const UINT length = ::GetWindowsDirectoryW(windows, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return false;
            return StartsWithNoCase(WideToUtf8(path), WideToUtf8(windows));
        }

        std::wstring ImagePath(HANDLE process)
        {
            std::wstring path(MAX_PATH, L'\0');
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                DWORD size = static_cast<DWORD>(path.size());
                if (::QueryFullProcessImageNameW(process, 0, path.data(), &size))
                {
                    path.resize(size);
                    return path;
                }
                if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                    break;
                path.resize(path.size() * 2);
            }
            return {};
        }

        bool TargetIs64Bit(HANDLE process)
        {
            BOOL wow64 = FALSE;
            if (!::IsWow64Process(process, &wow64))
                return true;
            return wow64 == FALSE;
        }

        struct TitleSearch
        {
            std::unordered_map<DWORD, std::string>* Titles;
        };

        BOOL CALLBACK CollectTitle(HWND window, LPARAM parameter)
        {
            auto* search = reinterpret_cast<TitleSearch*>(parameter);
            if (!::IsWindowVisible(window) || ::GetWindow(window, GW_OWNER))
                return TRUE;
            wchar_t title[256] = L"";
            if (::GetWindowTextW(window, title, 256) <= 0)
                return TRUE;
            DWORD id = 0;
            ::GetWindowThreadProcessId(window, &id);
            auto& known = (*search->Titles)[id];
            if (known.empty())
                known = WideToUtf8(title);
            return TRUE;
        }
    }

    bool RegionInfo::Committed() const
    {
        return State == MEM_COMMIT;
    }

    bool RegionInfo::Readable() const
    {
        if (!Committed() || (Protect & PAGE_GUARD) || (Protect & PAGE_NOACCESS))
            return false;
        return (Protect & 0xFF) != 0;
    }

    bool RegionInfo::Writable() const
    {
        return Readable() && WritableProtect(Protect);
    }

    bool RegionInfo::Executable() const
    {
        const DWORD base = Protect & 0xFF;
        return Readable() && (base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY);
    }

    bool RegionInfo::CopyOnWrite() const
    {
        const DWORD base = Protect & 0xFF;
        return base == PAGE_WRITECOPY || base == PAGE_EXECUTE_WRITECOPY;
    }

    bool RegionInfo::Guarded() const
    {
        return (Protect & PAGE_GUARD) != 0;
    }

    std::vector<ProcessEntry> ListProcesses(bool withWindowTitles)
    {
        std::vector<ProcessEntry> list;
        std::unordered_map<DWORD, std::string> titles;
        if (withWindowTitles)
        {
            TitleSearch search{ &titles };
            ::EnumWindows(&CollectTitle, reinterpret_cast<LPARAM>(&search));
        }

        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return list;
        PROCESSENTRY32W entry{ sizeof(entry) };
        const DWORD self = ::GetCurrentProcessId();
        for (BOOL more = ::Process32FirstW(snapshot, &entry); more; more = ::Process32NextW(snapshot, &entry))
        {
            if (entry.th32ProcessID == 0 || entry.th32ProcessID == self)
                continue;
            ProcessEntry process;
            process.Id = entry.th32ProcessID;
            process.ParentId = entry.th32ParentProcessID;
            process.Name = WideToUtf8(entry.szExeFile);
            process.Threads = entry.cntThreads;
            if (const auto title = titles.find(entry.th32ProcessID); title != titles.end())
                process.WindowTitle = title->second;
            if (HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID))
            {
                process.Is64Bit = TargetIs64Bit(handle);
                process.Path = ImagePath(handle);
                ::CloseHandle(handle);
            }
            else
            {
                process.Accessible = false;
            }
            list.push_back(std::move(process));
        }
        ::CloseHandle(snapshot);
        return list;
    }

    std::optional<std::uint32_t> FindProcessId(std::string_view name)
    {
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return std::nullopt;
        PROCESSENTRY32W entry{ sizeof(entry) };
        std::optional<std::uint32_t> exact;
        std::optional<std::uint32_t> prefix;
        for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !name.empty(); more = ::Process32NextW(snapshot, &entry))
        {
            const std::string exe = WideToUtf8(entry.szExeFile);
            if (EqualsNoCase(exe, name))
                exact = entry.th32ProcessID;
            else if (exe.size() > name.size() && EqualsNoCase(std::string_view(exe).substr(0, name.size()), name))
                prefix = entry.th32ProcessID;
        }
        ::CloseHandle(snapshot);
        return exact ? exact : prefix;
    }

    bool IsElevated()
    {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool elevated = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
        ::CloseHandle(token);
        return elevated;
    }

    std::shared_ptr<Process> Process::Open(std::uint32_t id, Status& status)
    {
        constexpr DWORD kAccess = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
                                  PROCESS_SUSPEND_RESUME | SYNCHRONIZE;
        HANDLE handle = ::OpenProcess(kAccess, FALSE, id);
        if (!handle)
        {
            const DWORD error = ::GetLastError();
            const std::string detail = std::to_string(id) + ", " + LastErrorText(error);
            if (error == ERROR_ACCESS_DENIED)
                status = Status::Fail(ErrorCode::ProcessAccessDenied, detail);
            else if (error == ERROR_INVALID_PARAMETER)
                status = Status::Fail(ErrorCode::ProcessNotFound, std::to_string(id));
            else
                status = Status::Fail(ErrorCode::ProcessOpenFailed, detail);
            return nullptr;
        }

        std::shared_ptr<Process> process(new Process());
        process->m_id = id;
        process->m_handle = handle;
        process->m_path = ImagePath(handle);
        process->m_name = WideToUtf8(std::filesystem::path(process->m_path).filename().wstring());
        if (process->m_name.empty())
            process->m_name = "pid " + std::to_string(id);
        process->m_is64Bit = TargetIs64Bit(handle);
        if (!process->m_is64Bit)
        {
            process->m_highest = 0x7FFEFFFFull;
            for (const RegionInfo& region : process->Regions(0x80000000ull, 0xFFFFFFFFull))
            {
                if (region.Committed())
                {
                    process->m_highest = 0xFFFEFFFFull;
                    break;
                }
            }
        }
        process->RefreshModules();
        status = Status::Success();
        return process;
    }

    Process::~Process()
    {
        if (m_handle)
            ::CloseHandle(m_handle);
    }

    bool Process::Alive() const
    {
        return m_handle && ::WaitForSingleObject(m_handle, 0) == WAIT_TIMEOUT;
    }

    std::size_t Process::Read(std::uint64_t address, void* buffer, std::size_t size) const
    {
        if (!m_handle || size == 0)
            return 0;
        if (DryRun() && DryInsideAllocation(address, size))
        {
            DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
            return size;
        }
        SIZE_T done = 0;
        if (::ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(address), buffer, size, &done) && done == size)
        {
            if (DryRun())
                DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
            return size;
        }

        std::size_t total = 0;
        auto* out = static_cast<std::uint8_t*>(buffer);
        std::uint64_t at = address;
        const std::uint64_t end = address + size;
        bool anyRead = false;
        while (at < end)
        {
            const std::uint64_t pageEnd = (at & ~0xFFFull) + 0x1000;
            const std::size_t chunk = static_cast<std::size_t>(std::min(pageEnd, end) - at);
            SIZE_T got = 0;
            if (::ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(at), out + (at - address), chunk, &got) && got == chunk)
            {
                anyRead = true;
                total = static_cast<std::size_t>(at - address) + chunk;
            }
            else
            {
                std::fill(out + (at - address), out + (at - address) + chunk, std::uint8_t(0));
                if (!anyRead)
                    total = 0;
            }
            at += chunk;
        }
        if (anyRead && DryRun())
            DryOverlay(address, out, total);
        return anyRead ? total : 0;
    }

    bool Process::ReadExact(std::uint64_t address, void* buffer, std::size_t size) const
    {
        if (!m_handle || size == 0)
            return false;
        if (DryRun() && DryInsideAllocation(address, size))
        {
            DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
            return true;
        }
        SIZE_T done = 0;
        const bool read = ::ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(address), buffer, size, &done) && done == size;
        if (read && DryRun())
            DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
        return read;
    }

    namespace
    {
        std::atomic<bool> g_dryRun{ false };
    }

    void Process::SetDryRun(bool enabled)
    {
        g_dryRun = enabled;
    }

    bool Process::DryRun()
    {
        return g_dryRun.load();
    }

    DryRunActivity Process::DryActivity() const
    {
        std::lock_guard lock(m_dryLock);
        return m_dryActivity;
    }

    bool Process::DryInsideAllocation(std::uint64_t address, std::size_t size) const
    {
        std::lock_guard lock(m_dryLock);
        auto it = m_dryAllocations.upper_bound(address);
        if (it == m_dryAllocations.begin())
            return false;
        --it;
        return address >= it->first && address + size <= it->first + it->second.size();
    }

    void Process::DryOverlay(std::uint64_t address, std::uint8_t* out, std::size_t size) const
    {
        std::lock_guard lock(m_dryLock);
        const std::uint64_t end = address + size;
        auto allocation = m_dryAllocations.upper_bound(address);
        if (allocation != m_dryAllocations.begin())
            --allocation;
        for (; allocation != m_dryAllocations.end() && allocation->first < end; ++allocation)
        {
            const std::uint64_t first = std::max(address, allocation->first);
            const std::uint64_t last = std::min(end, allocation->first + allocation->second.size());
            for (std::uint64_t at = first; at < last; ++at)
                out[at - address] = allocation->second[static_cast<std::size_t>(at - allocation->first)];
        }
        for (auto it = m_dryBytes.lower_bound(address); it != m_dryBytes.end() && it->first < end; ++it)
            out[it->first - address] = it->second;
    }

    bool Process::DryReserve(std::uint64_t base, std::size_t size) const
    {
        std::lock_guard lock(m_dryLock);
        auto next = m_dryAllocations.lower_bound(base);
        if (next != m_dryAllocations.end() && next->first < base + size)
            return false;
        if (next != m_dryAllocations.begin())
        {
            auto previous = std::prev(next);
            if (previous->first + previous->second.size() > base)
                return false;
        }
        m_dryAllocations.emplace(base, std::vector<std::uint8_t>(size, 0));
        ++m_dryActivity.Allocations;
        return true;
    }

    std::optional<std::uint64_t> Process::ReadPointer(std::uint64_t address) const
    {
        if (m_is64Bit)
            return ReadValue<std::uint64_t>(address);
        if (const auto value = ReadValue<std::uint32_t>(address))
            return *value;
        return std::nullopt;
    }

    Status Process::Write(std::uint64_t address, const void* data, std::size_t size) const
    {
        if (!m_handle)
            return Status::Fail(ErrorCode::NoProcess);
        if (size == 0)
            return Status::Success();
        if (DryRun())
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            if (DryInsideAllocation(address, size))
            {
                std::lock_guard lock(m_dryLock);
                auto it = std::prev(m_dryAllocations.upper_bound(address));
                std::copy(bytes, bytes + size, it->second.begin() + static_cast<std::ptrdiff_t>(address - it->first));
            }
            else
            {
                for (std::uint64_t page = address & ~0xFFFull; page < address + size; page += 0x1000)
                {
                    MEMORY_BASIC_INFORMATION info{};
                    if (!::VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(page), &info, sizeof(info)) || info.State != MEM_COMMIT)
                        return Status::Fail(ErrorCode::WriteFailed, Hex(address));
                }
                std::lock_guard lock(m_dryLock);
                for (std::size_t i = 0; i < size; ++i)
                    m_dryBytes[address + i] = bytes[i];
            }
            std::lock_guard lock(m_dryLock);
            ++m_dryActivity.Writes;
            m_dryActivity.BytesWritten += size;
            return Status::Success();
        }

        std::vector<std::pair<std::uint64_t, DWORD>> changed;
        for (std::uint64_t page = address & ~0xFFFull; page < address + size; page += 0x1000)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!::VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(page), &info, sizeof(info)) || info.State != MEM_COMMIT)
                return Status::Fail(ErrorCode::WriteFailed, Hex(address));
            if (!WritableProtect(info.Protect) || (info.Protect & PAGE_GUARD))
            {
                DWORD old = 0;
                if (::VirtualProtectEx(m_handle, reinterpret_cast<LPVOID>(page), 0x1000, WritableFor(info.Protect), &old))
                    changed.emplace_back(page, old);
            }
        }

        SIZE_T done = 0;
        const bool written = ::WriteProcessMemory(m_handle, reinterpret_cast<LPVOID>(address), data, size, &done) && done == size;
        const DWORD error = written ? 0 : ::GetLastError();
        for (const auto& [page, old] : changed)
        {
            DWORD ignored = 0;
            ::VirtualProtectEx(m_handle, reinterpret_cast<LPVOID>(page), 0x1000, old, &ignored);
        }
        ::FlushInstructionCache(m_handle, reinterpret_cast<LPCVOID>(address), size);
        if (!written)
            return Status::Fail(ErrorCode::WriteFailed, Hex(address) + " (" + LastErrorText(error) + ")");
        return Status::Success();
    }

    std::uint64_t Process::Allocate(std::size_t size, std::uint32_t protect, std::uint64_t nearAddress, Status& status) const
    {
        if (!m_handle)
        {
            status = Status::Fail(ErrorCode::NoProcess);
            return 0;
        }
        const std::uint64_t wanted = (size + kGranularity - 1) & ~(kGranularity - 1);
        if (nearAddress != 0 && m_is64Bit)
        {
            const std::uint64_t low = nearAddress > kNearRange + LowestAddress() ? nearAddress - kNearRange : LowestAddress();
            const std::uint64_t high = std::min(nearAddress + kNearRange, m_highest);
            std::vector<std::uint64_t> candidates;
            for (const RegionInfo& region : Regions(low, high))
            {
                if (region.State != MEM_FREE)
                    continue;
                std::uint64_t start = (region.Base + kGranularity - 1) & ~(kGranularity - 1);
                const std::uint64_t end = region.End();
                if (start < low)
                    start = (low + kGranularity - 1) & ~(kGranularity - 1);
                if (start + wanted > end || start + wanted > high)
                    continue;
                if (start >= nearAddress)
                    candidates.push_back(start);
                else
                {
                    const std::uint64_t last = (end - wanted) & ~(kGranularity - 1);
                    candidates.push_back(std::max(start, last));
                }
            }
            std::sort(candidates.begin(), candidates.end(), [&](std::uint64_t a, std::uint64_t b)
                {
                    const std::uint64_t da = a > nearAddress ? a - nearAddress : nearAddress - a;
                    const std::uint64_t db = b > nearAddress ? b - nearAddress : nearAddress - b;
                    return da < db;
                });
            for (std::uint64_t candidate : candidates)
            {
                if (DryRun())
                {
                    for (std::uint64_t at = candidate; at + wanted <= high; at += kGranularity)
                    {
                        if (DryReserve(at, static_cast<std::size_t>(wanted)))
                        {
                            status = Status::Success();
                            return at;
                        }
                    }
                    continue;
                }
                if (void* memory = ::VirtualAllocEx(m_handle, reinterpret_cast<LPVOID>(candidate), size, MEM_RESERVE | MEM_COMMIT, protect))
                {
                    status = Status::Success();
                    return reinterpret_cast<std::uint64_t>(memory);
                }
            }
            status = Status::Fail(ErrorCode::AllocFailed, Hex(size) + " bytes near " + Hex(nearAddress));
            return 0;
        }

        if (DryRun())
        {
            for (const RegionInfo& region : Regions(LowestAddress(), m_highest))
            {
                if (region.State != MEM_FREE)
                    continue;
                for (std::uint64_t at = (region.Base + kGranularity - 1) & ~(kGranularity - 1); at + wanted <= region.End(); at += kGranularity)
                {
                    if (DryReserve(at, static_cast<std::size_t>(wanted)))
                    {
                        status = Status::Success();
                        return at;
                    }
                }
            }
            status = Status::Fail(ErrorCode::AllocFailed, Hex(size) + " bytes");
            return 0;
        }
        void* memory = ::VirtualAllocEx(m_handle, nullptr, size, MEM_RESERVE | MEM_COMMIT, protect);
        if (!memory)
        {
            status = Status::Fail(ErrorCode::AllocFailed, Hex(size) + " bytes, " + LastErrorText(::GetLastError()));
            return 0;
        }
        status = Status::Success();
        return reinterpret_cast<std::uint64_t>(memory);
    }

    Status Process::Free(std::uint64_t address) const
    {
        if (!m_handle)
            return Status::Fail(ErrorCode::NoProcess);
        if (DryRun())
        {
            std::lock_guard lock(m_dryLock);
            ++m_dryActivity.Frees;
            if (m_dryAllocations.erase(address) == 0)
                return Status::Fail(ErrorCode::FreeFailed, Hex(address));
            return Status::Success();
        }
        if (!::VirtualFreeEx(m_handle, reinterpret_cast<LPVOID>(address), 0, MEM_RELEASE))
            return Status::Fail(ErrorCode::FreeFailed, Hex(address));
        return Status::Success();
    }

    std::optional<std::uint32_t> Process::Protect(std::uint64_t address, std::size_t size, std::uint32_t protect) const
    {
        DWORD old = 0;
        if (m_handle && DryRun())
        {
            {
                std::lock_guard lock(m_dryLock);
                ++m_dryActivity.Protections;
            }
            if (DryInsideAllocation(address, std::max<std::size_t>(size, 1)))
                return static_cast<std::uint32_t>(PAGE_EXECUTE_READWRITE);
            MEMORY_BASIC_INFORMATION info{};
            if (!::VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) || info.State != MEM_COMMIT)
                return std::nullopt;
            return static_cast<std::uint32_t>(info.Protect);
        }
        if (!m_handle || !::VirtualProtectEx(m_handle, reinterpret_cast<LPVOID>(address), size, protect, &old))
            return std::nullopt;
        return old;
    }

    std::vector<RegionInfo> Process::Regions(std::uint64_t start, std::uint64_t stop) const
    {
        std::vector<RegionInfo> regions;
        if (!m_handle)
            return regions;
        std::uint64_t at = start;
        MEMORY_BASIC_INFORMATION info{};
        while (at < stop && ::VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) == sizeof(info))
        {
            RegionInfo region = FromQuery(info);
            if (region.Size == 0)
                break;
            regions.push_back(region);
            const std::uint64_t next = region.Base + region.Size;
            if (next <= at)
                break;
            at = next;
        }
        return regions;
    }

    std::optional<RegionInfo> Process::RegionAt(std::uint64_t address) const
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!m_handle || ::VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info))
            return std::nullopt;
        return FromQuery(info);
    }

    bool Process::IsReadable(std::uint64_t address) const
    {
        const auto region = RegionAt(address);
        return region && region->Readable();
    }

    ModuleList Process::Modules() const
    {
        std::lock_guard lock(m_moduleLock);
        return m_modules ? m_modules : std::make_shared<const std::vector<ModuleInfo>>();
    }

    void Process::RefreshModules()
    {
        auto modules = std::make_shared<std::vector<ModuleInfo>>();
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 5 && snapshot == INVALID_HANDLE_VALUE; ++attempt)
        {
            snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m_id);
            if (snapshot == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_BAD_LENGTH)
                break;
        }
        if (snapshot != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W entry{ sizeof(entry) };
            for (BOOL more = ::Module32FirstW(snapshot, &entry); more; more = ::Module32NextW(snapshot, &entry))
            {
                ModuleInfo module;
                module.Name = WideToUtf8(entry.szModule);
                module.Path = entry.szExePath;
                module.Base = reinterpret_cast<std::uint64_t>(entry.modBaseAddr);
                module.Size = entry.modBaseSize;
                module.System = IsSystemPath(module.Path);
                if (!m_is64Bit && module.Base > 0xFFFFFFFFull)
                    continue;
                modules->push_back(std::move(module));
            }
            ::CloseHandle(snapshot);
        }
        else
        {
            LOG_WARN("Process", ErrorMessage(ErrorCode::ModulesUnreadable));
        }
        std::lock_guard lock(m_moduleLock);
        m_modules = std::move(modules);
    }

    std::optional<ModuleInfo> Process::ModuleAt(std::uint64_t address) const
    {
        const ModuleList modules = Modules();
        for (const ModuleInfo& module : *modules)
        {
            if (module.Contains(address))
                return module;
        }
        return std::nullopt;
    }

    std::optional<ModuleInfo> Process::FindModule(std::string_view name) const
    {
        const ModuleList modules = Modules();
        for (const ModuleInfo& module : *modules)
        {
            if (EqualsNoCase(module.Name, name))
                return module;
        }
        for (const ModuleInfo& module : *modules)
        {
            const std::size_t dot = module.Name.rfind('.');
            if (dot != std::string::npos && EqualsNoCase(std::string_view(module.Name).substr(0, dot), name))
                return module;
        }
        return std::nullopt;
    }

    std::optional<ModuleInfo> Process::MainModule() const
    {
        const ModuleList modules = Modules();
        for (const ModuleInfo& module : *modules)
        {
            if (EqualsNoCase(module.Name, m_name))
                return module;
        }
        if (!modules->empty())
            return modules->front();
        return std::nullopt;
    }

    std::vector<std::uint32_t> Process::Threads() const
    {
        std::vector<std::uint32_t> threads;
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return threads;
        THREADENTRY32 entry{ sizeof(entry) };
        for (BOOL more = ::Thread32First(snapshot, &entry); more; more = ::Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID == m_id)
                threads.push_back(entry.th32ThreadID);
        }
        ::CloseHandle(snapshot);
        return threads;
    }

    bool Process::Suspend() const
    {
        static const NtProcessCall suspend = NtCall("NtSuspendProcess");
        if (DryRun())
            return m_handle != nullptr;
        return suspend && m_handle && suspend(m_handle) >= 0;
    }

    bool Process::Resume() const
    {
        static const NtProcessCall resume = NtCall("NtResumeProcess");
        if (DryRun())
            return m_handle != nullptr;
        return resume && m_handle && resume(m_handle) >= 0;
    }

    Status Process::CreateThread(std::uint64_t start, std::uint64_t parameter, bool wait, std::uint32_t timeoutMilliseconds) const
    {
        if (!m_handle)
            return Status::Fail(ErrorCode::NoProcess);
        if (DryRun())
        {
            std::lock_guard lock(m_dryLock);
            ++m_dryActivity.Threads;
            return Status::Success();
        }
        HANDLE thread = ::CreateRemoteThread(m_handle, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), reinterpret_cast<LPVOID>(parameter), 0, nullptr);
        if (!thread)
            return Status::Fail(ErrorCode::ThreadCreateFailed, Hex(start) + ", " + LastErrorText(::GetLastError()));
        if (wait)
            ::WaitForSingleObject(thread, timeoutMilliseconds);
        ::CloseHandle(thread);
        return Status::Success();
    }

    std::uint64_t Process::FindExport(std::string_view moduleName, std::string_view name) const
    {
        const auto module = FindModule(moduleName);
        if (!module)
            return 0;
        IMAGE_DOS_HEADER dos{};
        if (!ReadExact(module->Base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        DWORD exportRva = 0;
        DWORD exportSize = 0;
        if (m_is64Bit)
        {
            IMAGE_NT_HEADERS64 nt{};
            if (!ReadExact(module->Base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
                return 0;
            exportRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
            exportSize = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        }
        else
        {
            IMAGE_NT_HEADERS32 nt{};
            if (!ReadExact(module->Base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
                return 0;
            exportRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
            exportSize = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        }
        if (exportRva == 0 || exportSize < sizeof(IMAGE_EXPORT_DIRECTORY))
            return 0;
        IMAGE_EXPORT_DIRECTORY directory{};
        if (!ReadExact(module->Base + exportRva, &directory, sizeof(directory)))
            return 0;
        std::vector<DWORD> names(directory.NumberOfNames);
        std::vector<WORD> ordinals(directory.NumberOfNames);
        std::vector<DWORD> functions(directory.NumberOfFunctions);
        if (names.empty() || !ReadExact(module->Base + directory.AddressOfNames, names.data(), names.size() * sizeof(DWORD)) ||
            !ReadExact(module->Base + directory.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD)) ||
            functions.empty() || !ReadExact(module->Base + directory.AddressOfFunctions, functions.data(), functions.size() * sizeof(DWORD)))
            return 0;
        char buffer[256];
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            const std::size_t got = Read(module->Base + names[i], buffer, sizeof(buffer) - 1);
            if (got == 0)
                continue;
            buffer[sizeof(buffer) - 1] = 0;
            if (name == buffer && ordinals[i] < functions.size())
                return module->Base + functions[ordinals[i]];
        }
        return 0;
    }

    Status Process::InjectLibrary(const std::wstring& path) const
    {
        if (!m_handle)
            return Status::Fail(ErrorCode::NoProcess);
        if (DryRun())
        {
            std::lock_guard lock(m_dryLock);
            ++m_dryActivity.Libraries;
            return Status::Success();
        }
        const std::uint64_t loadLibrary = FindExport("kernel32.dll", "LoadLibraryW");
        if (loadLibrary == 0)
            return Status::Fail(ErrorCode::InjectFailed, "LoadLibraryW not found in the target");
        const std::size_t bytes = (path.size() + 1) * sizeof(wchar_t);
        Status status;
        const std::uint64_t memory = Allocate(bytes, PAGE_READWRITE, 0, status);
        if (!memory)
            return status;
        if (Status written = Write(memory, path.c_str(), bytes); !written)
        {
            Free(memory);
            return written;
        }
        HANDLE thread = ::CreateRemoteThread(m_handle, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), reinterpret_cast<LPVOID>(memory), 0, nullptr);
        if (!thread)
        {
            const DWORD error = ::GetLastError();
            Free(memory);
            return Status::Fail(ErrorCode::ThreadCreateFailed, LastErrorText(error));
        }
        ::WaitForSingleObject(thread, 15000);
        DWORD exitCode = 0;
        ::GetExitCodeThread(thread, &exitCode);
        ::CloseHandle(thread);
        Free(memory);
        const_cast<Process*>(this)->RefreshModules();
        if (exitCode == 0)
            return Status::Fail(ErrorCode::InjectFailed, WideToUtf8(path));
        return Status::Success();
    }

    std::string ProtectText(std::uint32_t protect)
    {
        std::string text;
        switch (protect & 0xFF)
        {
        case PAGE_NOACCESS: text = "---"; break;
        case PAGE_READONLY: text = "R--"; break;
        case PAGE_READWRITE: text = "RW-"; break;
        case PAGE_WRITECOPY: text = "RC-"; break;
        case PAGE_EXECUTE: text = "--X"; break;
        case PAGE_EXECUTE_READ: text = "R-X"; break;
        case PAGE_EXECUTE_READWRITE: text = "RWX"; break;
        case PAGE_EXECUTE_WRITECOPY: text = "RCX"; break;
        default: text = "?"; break;
        }
        if (protect & PAGE_GUARD)
            text += " guard";
        if (protect & PAGE_NOCACHE)
            text += " nocache";
        return text;
    }

    std::string RegionTypeText(std::uint32_t type)
    {
        switch (type)
        {
        case MEM_IMAGE: return "Image";
        case MEM_MAPPED: return "Mapped";
        case MEM_PRIVATE: return "Private";
        default: return "";
        }
    }
}
