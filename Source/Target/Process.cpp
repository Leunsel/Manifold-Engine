#include "Target/Process.h"

#include "Core/RuntimeLog.h"
#include "Core/Handle.h"
#include "Core/Text.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <unordered_map>

#include <windows.h>
#include <tlhelp32.h>

#include "Syscalls/Syscall.h"

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

        DWORD DosErrorOf(NTSTATUS status)
        {
            using Translate = ULONG(WINAPI*)(NTSTATUS);
            static const Translate translate = []() -> Translate
                {
                    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
                    return ntdll ? reinterpret_cast<Translate>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlNtStatusToDosError"))) : nullptr;
                }();
            return translate ? translate(status) : ERROR_GEN_FAILURE;
        }

        bool Succeeded(NTSTATUS status)
        {
            if (status >= 0)
                return true;
            ::SetLastError(DosErrorOf(status));
            return false;
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

        // Target memory access goes through direct NtReadVirtualMemory /
        // NtWriteVirtualMemory system calls, matching the NtOpenProcess stub. If
        // the service number was not resolved from ntdll (number stays 0) these
        // fall back to the Win32 API. Both return the same "the call reported
        // success" bool as the Read/WriteProcessMemory they replace, with the
        // transferred count in `done`; callers still check `done == size`.
        bool TryReadVm(HANDLE handle, std::uint64_t address, void* buffer, std::size_t size, SIZE_T& done)
        {
            if (ReadVmSysCall == 0)
                return ::ReadProcessMemory(handle, reinterpret_cast<LPCVOID>(address), buffer, size, &done) != 0;
            done = 0;
            return Succeeded(NtReadVirtualMemory_direct(handle, reinterpret_cast<PVOID>(address), buffer, size, &done));
        }

        bool TryWriteVm(HANDLE handle, std::uint64_t address, const void* data, std::size_t size, SIZE_T& done)
        {
            if (WriteVmSysCall == 0)
                return ::WriteProcessMemory(handle, reinterpret_cast<LPVOID>(address), data, size, &done) != 0;
            done = 0;
            return Succeeded(NtWriteVirtualMemory_direct(handle, reinterpret_cast<PVOID>(address), const_cast<PVOID>(data), size, &done));
        }

        // Region query, protection, allocation and release go through the same
        // direct Nt* system calls when their service number resolved, and fall
        // back to the Win32 Ex-APIs otherwise. The Nt forms take in/out pointers
        // for base and size (rounded to pages), so the wrappers translate the
        // Win32-style arguments the call sites still use.
        SIZE_T QueryVm(HANDLE handle, std::uint64_t address, MEMORY_BASIC_INFORMATION& info)
        {
            if (QueryVmSysCall == 0)
                return ::VirtualQueryEx(handle, reinterpret_cast<LPCVOID>(address), &info, sizeof(info));
            SIZE_T returned = 0;
            const NTSTATUS status = NtQueryVirtualMemory_direct(
                handle, reinterpret_cast<PVOID>(address), 0 /*MemoryBasicInformation*/, &info, sizeof(info), &returned);
            return Succeeded(status) ? returned : 0;
        }

        bool ProtectVm(HANDLE handle, std::uint64_t address, std::size_t size, DWORD newProtect, DWORD& oldProtect)
        {
            if (ProtectVmSysCall == 0)
                return ::VirtualProtectEx(handle, reinterpret_cast<LPVOID>(address), size, newProtect, &oldProtect) != 0;
            PVOID base = reinterpret_cast<PVOID>(address);
            SIZE_T region = size;
            return Succeeded(NtProtectVirtualMemory_direct(handle, &base, &region, newProtect, &oldProtect));
        }

        void* AllocVm(HANDLE handle, std::uint64_t desired, std::size_t size, DWORD allocationType, DWORD protect)
        {
            if (AllocVmSysCall == 0)
                return ::VirtualAllocEx(handle, reinterpret_cast<LPVOID>(desired), size, allocationType, protect);
            PVOID base = reinterpret_cast<PVOID>(desired);
            SIZE_T region = size;
            const NTSTATUS status = NtAllocateVirtualMemory_direct(handle, &base, 0, &region, allocationType, protect);
            return Succeeded(status) ? base : nullptr;
        }

        bool FreeVm(HANDLE handle, std::uint64_t address)
        {
            if (FreeVmSysCall == 0)
                return ::VirtualFreeEx(handle, reinterpret_cast<LPVOID>(address), 0, MEM_RELEASE) != 0;
            PVOID base = reinterpret_cast<PVOID>(address);
            SIZE_T region = 0;
            return Succeeded(NtFreeVirtualMemory_direct(handle, &base, &region, MEM_RELEASE));
        }

        bool IsSystemPath(const std::wstring& path);

        std::wstring Wow64DiskPath(std::wstring path)
        {
            static const std::pair<std::wstring, std::wstring> folders = []
                {
                    wchar_t system[MAX_PATH] = L"";
                    wchar_t wow64[MAX_PATH] = L"";
                    const UINT systemLength = ::GetSystemDirectoryW(system, MAX_PATH);
                    const UINT wow64Length = ::GetSystemWow64DirectoryW(wow64, MAX_PATH);
                    if (systemLength == 0 || systemLength >= MAX_PATH || wow64Length == 0 || wow64Length >= MAX_PATH)
                        return std::pair<std::wstring, std::wstring>();
                    return std::pair<std::wstring, std::wstring>(std::wstring(system, systemLength), std::wstring(wow64, wow64Length));
                }();
            const std::wstring& system = folders.first;
            if (system.empty() || path.size() <= system.size() || (path[system.size()] != L'\\' && path[system.size()] != L'/'))
                return path;
            if (::CompareStringOrdinal(path.c_str(), static_cast<int>(system.size()), system.c_str(), static_cast<int>(system.size()), TRUE) != CSTR_EQUAL)
                return path;
            return folders.second + path.substr(system.size());
        }

        // Read a block / a POD value out of the target address space through
        // the existing TryReadVm wrapper (which goes through the direct
        // NtReadVirtualMemory system call when it resolved).
        bool ReadAt(HANDLE handle, std::uint64_t address, void* buffer, std::size_t size)
        {
            SIZE_T done = 0;
            return TryReadVm(handle, address, buffer, size, done) && done == size;
        }

        template <typename T>
        bool ReadStruct(HANDLE handle, std::uint64_t address, T& value)
        {
            return ReadAt(handle, address, &value, sizeof(value));
        }

        std::wstring ReadUnicodeString(HANDLE handle, std::uint16_t length, std::uint64_t buffer)
        {
            if (length == 0 || buffer == 0 || length > 0x8000)
                return {};
            std::wstring text(length / 2, L'\0');
            if (!ReadAt(handle, buffer, text.data(), length))
                return {};
            return text;
        }

        // Walk the target's module list through its PEB → Ldr → InLoadOrderModuleList,
        // read directly through NtReadVirtualMemory. The PEB base comes from
        // NtQueryInformationProcess(ProcessBasicInformation); for a WoW64 target the
        // 32-bit PEB comes from ProcessWow64Information. Returns false and leaves
        // `out` empty when anything the walk needs is unavailable, so the caller
        // can fall back to Toolhelp.
        constexpr int kProcessBasicInformation = 0;
        constexpr int kProcessWow64Information = 26;

        struct ProcessBasicInformation64
        {
            NTSTATUS ExitStatus;
            std::uint64_t PebBaseAddress;
            std::uint64_t AffinityMask;
            std::int32_t BasePriority;
            std::uint64_t UniqueProcessId;
            std::uint64_t InheritedFromUniqueProcessId;
        };

        bool TryModulesViaPeb64(HANDLE handle, std::vector<ModuleInfo>& out)
        {
            if (QueryInfoProcSysCall == 0)
                return false;

            ProcessBasicInformation64 pbi{};
            ULONG returned = 0;
            const NTSTATUS qs = NtQueryInformationProcess_direct(
                handle, kProcessBasicInformation, &pbi, sizeof(pbi), &returned);
            if (qs < 0 || pbi.PebBaseAddress == 0)
                return false;

            std::uint64_t ldr = 0;
            if (!ReadStruct(handle, pbi.PebBaseAddress + 0x18, ldr) || ldr == 0)
                return false;

            const std::uint64_t listHead = ldr + 0x10;
            std::uint64_t at = 0;
            if (!ReadStruct(handle, listHead, at) || at == 0)
                return false;

            for (int i = 0; i < 1024 && at != listHead; ++i)
            {
                std::uint64_t dllBase = 0;
                std::uint32_t sizeOfImage = 0;
                std::uint16_t fullLen = 0, baseLen = 0;
                std::uint64_t fullBuffer = 0, baseBuffer = 0;

                ReadStruct(handle, at + 0x30, dllBase);
                ReadStruct(handle, at + 0x40, sizeOfImage);
                ReadStruct(handle, at + 0x48, fullLen);
                ReadStruct(handle, at + 0x50, fullBuffer);
                ReadStruct(handle, at + 0x58, baseLen);
                ReadStruct(handle, at + 0x60, baseBuffer);

                if (dllBase != 0 && sizeOfImage != 0)
                {
                    ModuleInfo module;
                    module.Base = dllBase;
                    module.Size = sizeOfImage;
                    module.Path = ReadUnicodeString(handle, fullLen, fullBuffer);
                    const std::wstring base = ReadUnicodeString(handle, baseLen, baseBuffer);
                    if (!base.empty())
                        module.Name = WideToUtf8(base);
                    else if (!module.Path.empty())
                    {
                        const auto slash = module.Path.find_last_of(L"\\/");
                        module.Name = WideToUtf8(slash == std::wstring::npos ? module.Path : module.Path.substr(slash + 1));
                    }
                    module.System = !module.Path.empty() && IsSystemPath(module.Path);
                    out.push_back(std::move(module));
                }

                std::uint64_t next = 0;
                if (!ReadStruct(handle, at, next) || next == 0 || next == at)
                    break;
                at = next;
            }
            return !out.empty();
        }

        bool TryModulesViaPeb32(HANDLE handle, std::vector<ModuleInfo>& out)
        {
            if (QueryInfoProcSysCall == 0)
                return false;

            std::uint64_t peb32 = 0;
            ULONG returned = 0;
            const NTSTATUS qs = NtQueryInformationProcess_direct(
                handle, kProcessWow64Information, &peb32, sizeof(peb32), &returned);
            if (qs < 0 || peb32 == 0)
                return false;

            std::uint32_t ldr = 0;
            if (!ReadStruct(handle, peb32 + 0x0C, ldr) || ldr == 0)
                return false;

            const std::uint64_t listHead = static_cast<std::uint64_t>(ldr) + 0x0C;
            std::uint32_t at = 0;
            if (!ReadStruct(handle, listHead, at) || at == 0)
                return false;

            for (int i = 0; i < 1024 && static_cast<std::uint64_t>(at) != listHead; ++i)
            {
                std::uint32_t dllBase = 0, sizeOfImage = 0;
                std::uint16_t fullLen = 0, baseLen = 0;
                std::uint32_t fullBuffer = 0, baseBuffer = 0;

                ReadStruct(handle, at + 0x18, dllBase);
                ReadStruct(handle, at + 0x20, sizeOfImage);
                ReadStruct(handle, at + 0x24, fullLen);
                ReadStruct(handle, at + 0x28, fullBuffer);
                ReadStruct(handle, at + 0x2C, baseLen);
                ReadStruct(handle, at + 0x30, baseBuffer);

                if (dllBase != 0 && sizeOfImage != 0)
                {
                    ModuleInfo module;
                    module.Base = dllBase;
                    module.Size = sizeOfImage;
                    module.Path = Wow64DiskPath(ReadUnicodeString(handle, fullLen, fullBuffer));
                    const std::wstring base = ReadUnicodeString(handle, baseLen, baseBuffer);
                    if (!base.empty())
                        module.Name = WideToUtf8(base);
                    else if (!module.Path.empty())
                    {
                        const auto slash = module.Path.find_last_of(L"\\/");
                        module.Name = WideToUtf8(slash == std::wstring::npos ? module.Path : module.Path.substr(slash + 1));
                    }
                    module.System = !module.Path.empty() && IsSystemPath(module.Path);
                    out.push_back(std::move(module));
                }

                std::uint32_t next = 0;
                if (!ReadStruct(handle, at, next) || next == 0 || next == at)
                    break;
                at = next;
            }
            return !out.empty();
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

        // A single process entry as returned by the system-wide snapshot. We
        // keep only the four fields the engine actually uses; further fields
        // (handle count, memory usage, per-thread info) live past them in the
        // Nt data but are of no interest here.
        struct ProcSnapshotEntry
        {
            std::uint32_t Id;
            std::uint32_t ParentId;
            std::uint32_t Threads;
            std::string Name;
        };

        // Layout of SYSTEM_PROCESS_INFORMATION up to the fields we read. The
        // public winternl.h declaration hides the parent-pid slot behind a
        // reserved field, so we spell the full prefix out ourselves. Trailing
        // fields are not referenced; NextEntryOffset carries us past them.
        constexpr int kSystemProcessInformation = 5;
        struct SysProcInfoPrefix
        {
            ULONG NextEntryOffset;
            ULONG NumberOfThreads;
            LARGE_INTEGER WorkingSetPrivateSize;
            ULONG HardFaultCount;
            ULONG NumberOfThreadsHighWatermark;
            ULONGLONG CycleTime;
            LARGE_INTEGER CreateTime;
            LARGE_INTEGER UserTime;
            LARGE_INTEGER KernelTime;
            UNICODE_STRING ImageName;
            LONG BasePriority;
            HANDLE UniqueProcessId;
            HANDLE InheritedFromUniqueProcessId;
        };

        bool TryProcessSnapshotNt(std::vector<ProcSnapshotEntry>& out)
        {
            if (QuerySysInfoSysCall == 0)
                return false;

            // Grow the buffer until the call stops asking for more. On a busy
            // system the process list changes between calls, so we also cap
            // the retries to avoid an unbounded loop.
            std::vector<std::uint8_t> buffer(1 << 18);
            NTSTATUS status = static_cast<NTSTATUS>(0xC0000004L); // STATUS_INFO_LENGTH_MISMATCH
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                ULONG returned = 0;
                status = NtQuerySystemInformation_direct(
                    kSystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &returned);
                if (status >= 0)
                    break;
                if (status != static_cast<NTSTATUS>(0xC0000004L) /*STATUS_INFO_LENGTH_MISMATCH*/)
                    return false;
                buffer.resize(std::max<std::size_t>(buffer.size() * 2, returned + 0x10000));
            }
            if (status < 0)
                return false;

            const std::uint8_t* cursor = buffer.data();
            const std::uint8_t* end = cursor + buffer.size();
            while (cursor + sizeof(SysProcInfoPrefix) <= end)
            {
                const auto* entry = reinterpret_cast<const SysProcInfoPrefix*>(cursor);
                ProcSnapshotEntry snap;
                snap.Id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry->UniqueProcessId));
                snap.ParentId = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry->InheritedFromUniqueProcessId));
                snap.Threads = entry->NumberOfThreads;
                if (entry->ImageName.Buffer && entry->ImageName.Length > 0)
                {
                    const std::wstring name(entry->ImageName.Buffer, entry->ImageName.Length / sizeof(wchar_t));
                    snap.Name = WideToUtf8(name);
                }
                out.push_back(std::move(snap));

                if (entry->NextEntryOffset == 0)
                    break;
                cursor += entry->NextEntryOffset;
            }
            return !out.empty();
        }

        std::vector<ProcSnapshotEntry> ProcessSnapshot()
        {
            std::vector<ProcSnapshotEntry> out;
            if (TryProcessSnapshotNt(out))
                return out;

            const UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            if (!snapshot)
                return out;
            PROCESSENTRY32W entry{ sizeof(entry) };
            for (BOOL more = ::Process32FirstW(snapshot.Get(), &entry); more; more = ::Process32NextW(snapshot.Get(), &entry))
            {
                ProcSnapshotEntry snap;
                snap.Id = entry.th32ProcessID;
                snap.ParentId = entry.th32ParentProcessID;
                snap.Threads = entry.cntThreads;
                snap.Name = WideToUtf8(entry.szExeFile);
                out.push_back(std::move(snap));
            }
            return out;
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

    bool RegionInfo::NoCache() const
    {
        return (Protect & PAGE_NOCACHE) != 0;
    }

    bool RegionInfo::WriteCombine() const
    {
        return (Protect & PAGE_WRITECOMBINE) != 0;
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

        const DWORD self = ::GetCurrentProcessId();
        for (const ProcSnapshotEntry& entry : ProcessSnapshot())
        {
            if (entry.Id == 0 || entry.Id == self)
                continue;
            ProcessEntry process;
            process.Id = MakeProcessId(entry.Id);
            process.ParentId = MakeProcessId(entry.ParentId);
            process.Name = entry.Name;
            process.Threads = entry.Threads;
            if (const auto title = titles.find(entry.Id); title != titles.end())
                process.WindowTitle = title->second;
            if (const UniqueHandle handle(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.Id)); handle)
            {
                process.Is64Bit = TargetIs64Bit(handle.Get());
                process.Path = ImagePath(handle.Get());
            }
            else
            {
                process.Accessible = false;
            }
            list.push_back(std::move(process));
        }
        return list;
    }

    std::optional<ProcessId> FindProcessId(std::string_view name)
    {
        if (name.empty())
            return std::nullopt;
        std::optional<ProcessId> exact;
        std::optional<ProcessId> prefix;
        for (const ProcSnapshotEntry& entry : ProcessSnapshot())
        {
            if (EqualsNoCase(entry.Name, name))
                exact = MakeProcessId(entry.Id);
            else if (entry.Name.size() > name.size() && EqualsNoCase(std::string_view(entry.Name).substr(0, name.size()), name))
                prefix = MakeProcessId(entry.Id);
        }
        return exact ? exact : prefix;
    }

    bool IsElevated()
    {
        HANDLE raw = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
            return false;
        const UniqueHandle token(raw);
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        return ::GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    }

    Result<std::shared_ptr<Process>> Process::Open(ProcessId processId)
    {
        const DWORD id = Raw(processId);
        constexpr DWORD kAccess = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
                                  PROCESS_SUSPEND_RESUME | SYNCHRONIZE;
        HANDLE handle = nullptr;
        DWORD error = ERROR_SUCCESS;
        if (OpenSysCall == 0)
        {
            handle = ::OpenProcess(kAccess, FALSE, id);
            if (!handle)
                error = ::GetLastError();
        }
        else
        {
            OBJECT_ATTRIBUTES attributes;
            InitializeObjectAttributes(&attributes, nullptr, 0, nullptr, nullptr);
            CLIENT_ID cid{};
            cid.UniqueProcess = ::ULongToHandle(id);
            const NTSTATUS opened = NtOpenProcess_direct(&handle, kAccess, &attributes, &cid);
            if (opened < 0 || !handle)
            {
                error = opened < 0 ? DosErrorOf(opened) : ERROR_INVALID_HANDLE;
                handle = nullptr;
            }
        }

        if (!handle)
        {
            const std::string detail = std::to_string(id) + ", " + SystemErrorText(error);
            if (error == ERROR_ACCESS_DENIED)
                return Status::Fail(ErrorCode::ProcessAccessDenied, detail);
            if (error == ERROR_INVALID_PARAMETER)
                return Status::Fail(ErrorCode::ProcessNotFound, std::to_string(id));
            return Status::Fail(ErrorCode::ProcessOpenFailed, detail);
        }

        std::shared_ptr<Process> process(new Process());
        process->m_id = processId;
        process->m_handle.Reset(handle);
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
        return process;
    }

    bool Process::Alive() const
    {
        return m_handle.Get() && ::WaitForSingleObject(m_handle.Get(), 0) == WAIT_TIMEOUT;
    }

    std::size_t Process::Read(std::uint64_t address, void* buffer, std::size_t size) const
    {
        // ∑ A range that runs past the top of the address space is never readable. It used to reach the page loop
        //   below, where the end of the last page wrapped to 0 and one chunk zeroed up to 4 KB past the buffer:
        //   the pointer rescan in Crab followed a changing sentinel (0xFFFF...F0 plus an offset) and overran its stack.
        if (!m_handle.Get() || size == 0 || size > ~address)
            return 0;
        if (DryRun() && DryInsideAllocation(address, size))
        {
            DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
            return size;
        }
        SIZE_T done = 0;
        if (TryReadVm(m_handle.Get(), address, buffer, size, done) && done == size)
        {
            if (DryRun())
                DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
            ShowShadow(address, buffer, size);
            return size;
        }

        std::size_t total = 0;
        auto* out = static_cast<std::uint8_t*>(buffer);
        std::uint64_t at = address;
        const std::uint64_t end = address + size;
        bool anyRead = false;
        while (at < end)
        {
            const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(0x1000 - (at & 0xFFF), end - at));
            SIZE_T got = 0;
            if (TryReadVm(m_handle.Get(), at, out + (at - address), chunk, got) && got == chunk)
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
        if (anyRead)
            ShowShadow(address, out, total);
        return anyRead ? total : 0;
    }

    bool Process::ReadExact(std::uint64_t address, void* buffer, std::size_t size) const
    {
        if (!m_handle.Get() || size == 0)
            return false;
        if (DryRun() && DryInsideAllocation(address, size))
        {
            DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
            return true;
        }
        SIZE_T done = 0;
        const bool read = TryReadVm(m_handle.Get(), address, buffer, size, done) && done == size;
        if (read && DryRun())
            DryOverlay(address, static_cast<std::uint8_t*>(buffer), size);
        if (read)
            ShowShadow(address, buffer, size);
        return read;
    }

    bool Process::ReadRaw(std::uint64_t address, void* buffer, std::size_t size) const
    {
        SIZE_T done = 0;
        return m_handle.Get() && size != 0 && TryReadVm(m_handle.Get(), address, buffer, size, done) && done == size;
    }

    void Process::SetShadow(std::shared_ptr<const ByteShadow> shadow)
    {
        std::lock_guard lock(m_shadowLock);
        m_shadow = std::move(shadow);
        m_hasShadow = m_shadow != nullptr;
    }

    std::shared_ptr<const ByteShadow> Process::CurrentShadow() const
    {
        if (!m_hasShadow.load(std::memory_order_acquire))
            return nullptr;
        std::lock_guard lock(m_shadowLock);
        return m_shadow;
    }

    void Process::ShowShadow(std::uint64_t address, void* buffer, std::size_t size) const
    {
        if (const std::shared_ptr<const ByteShadow> shadow = CurrentShadow())
            shadow->Show(address, static_cast<std::uint8_t*>(buffer), size);
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
        if (!m_handle.Get())
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
                    if (!QueryVm(m_handle.Get(), page, info) || info.State != MEM_COMMIT)
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

        if (const std::shared_ptr<const ByteShadow> shadow = CurrentShadow())
            shadow->BeforeWrite(address, size);
        // ∑ Every page is checked before any protection changes: a range running past the end of its region used to return with
        //   the earlier pages left writable (a read-only or execute-only page stayed read-write).
        std::vector<std::pair<std::uint64_t, DWORD>> pages;
        for (std::uint64_t page = address & ~0xFFFull; page < address + size; page += 0x1000)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (!QueryVm(m_handle.Get(), page, info) || info.State != MEM_COMMIT)
                return Status::Fail(ErrorCode::WriteFailed, Hex(address));
            pages.emplace_back(page, info.Protect);
        }
        std::vector<std::pair<std::uint64_t, DWORD>> changed;
        for (const auto& [page, protect] : pages)
        {
            if (!WritableProtect(protect) || (protect & PAGE_GUARD))
            {
                DWORD old = 0;
                if (ProtectVm(m_handle.Get(), page, 0x1000, WritableFor(protect), old))
                    changed.emplace_back(page, old);
            }
        }

        SIZE_T done = 0;
        const bool written = TryWriteVm(m_handle.Get(), address, data, size, done) && done == size;
        const DWORD error = written ? 0 : ::GetLastError();
        for (const auto& [page, old] : changed)
        {
            DWORD ignored = 0;
            ProtectVm(m_handle.Get(), page, 0x1000, old, ignored);
        }
        ::FlushInstructionCache(m_handle.Get(), reinterpret_cast<LPCVOID>(address), size);
        if (!written)
            return Status::Fail(ErrorCode::WriteFailed, Hex(address) + " (" + SystemErrorText(error) + ")");
        return Status::Success();
    }

    Result<Address> Process::Allocate(std::size_t size, std::uint32_t protect, Address nearAddress) const
    {
        if (!m_handle.Get())
        {
            return Status::Fail(ErrorCode::NoProcess);
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
                            return at;
                        }
                    }
                    continue;
                }
                if (void* memory = AllocVm(m_handle.Get(), candidate, size, MEM_RESERVE | MEM_COMMIT, protect))
                {
                    return reinterpret_cast<std::uint64_t>(memory);
                }
            }
            return Status::Fail(ErrorCode::AllocFailed, Hex(size) + " bytes near " + Hex(nearAddress));
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
                        return at;
                    }
                }
            }
            return Status::Fail(ErrorCode::AllocFailed, Hex(size) + " bytes");
        }
        void* memory = AllocVm(m_handle.Get(), 0, size, MEM_RESERVE | MEM_COMMIT, protect);
        if (!memory)
        {
            return Status::Fail(ErrorCode::AllocFailed, Hex(size) + " bytes, " + SystemErrorText(::GetLastError()));
        }
        return reinterpret_cast<std::uint64_t>(memory);
    }

    Status Process::Free(std::uint64_t address) const
    {
        if (!m_handle.Get())
            return Status::Fail(ErrorCode::NoProcess);
        if (DryRun())
        {
            std::lock_guard lock(m_dryLock);
            ++m_dryActivity.Frees;
            if (m_dryAllocations.erase(address) == 0)
                return Status::Fail(ErrorCode::FreeFailed, Hex(address));
            return Status::Success();
        }
        if (!FreeVm(m_handle.Get(), address))
            return Status::Fail(ErrorCode::FreeFailed, Hex(address));
        return Status::Success();
    }

    std::optional<std::uint32_t> Process::Protect(std::uint64_t address, std::size_t size, std::uint32_t protect) const
    {
        DWORD old = 0;
        if (m_handle.Get() && DryRun())
        {
            {
                std::lock_guard lock(m_dryLock);
                ++m_dryActivity.Protections;
            }
            if (DryInsideAllocation(address, std::max<std::size_t>(size, 1)))
                return static_cast<std::uint32_t>(PAGE_EXECUTE_READWRITE);
            MEMORY_BASIC_INFORMATION info{};
            if (!QueryVm(m_handle.Get(), address, info) || info.State != MEM_COMMIT)
                return std::nullopt;
            return static_cast<std::uint32_t>(info.Protect);
        }
        if (!m_handle.Get() || !ProtectVm(m_handle.Get(), address, size, protect, old))
            return std::nullopt;
        return old;
    }

    std::vector<RegionInfo> Process::Regions(std::uint64_t start, std::uint64_t stop) const
    {
        std::vector<RegionInfo> regions;
        if (!m_handle.Get())
            return regions;
        std::uint64_t at = start;
        MEMORY_BASIC_INFORMATION info{};
        while (at < stop && QueryVm(m_handle.Get(), at, info) == sizeof(info))
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
        if (!m_handle.Get() || QueryVm(m_handle.Get(), address, info) != sizeof(info))
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

        // First try reading the module list straight out of the target's PEB
        // via NtReadVirtualMemory. It side-steps the Toolhelp cross-bitness bugs
        // (ERROR_BAD_LENGTH/ERROR_PARTIAL_COPY retries) and goes through the
        // direct system call path. On any failure we fall back to Toolhelp.
        const bool viaPeb = m_handle.Get() && (m_is64Bit
            ? TryModulesViaPeb64(m_handle.Get(), *modules)
            : TryModulesViaPeb32(m_handle.Get(), *modules));

        if (!viaPeb)
        {
            HANDLE snapshot = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 5 && snapshot == INVALID_HANDLE_VALUE; ++attempt)
            {
                snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Raw(m_id));
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
            else if (Alive())
            {
                LOG_WARN("Process", ErrorMessage(ErrorCode::ModulesUnreadable));
            }
        }

        // ∑ A process that ended has no module list any more; the last one it
        //   had still names the addresses of its crash, so it stays.
        if (modules->empty() && !Alive())
        {
            std::lock_guard lock(m_moduleLock);
            if (m_modules && !m_modules->empty())
                return;
        }
        auto spans = std::make_shared<std::vector<ModuleSpan>>();
        spans->reserve(modules->size());
        for (std::size_t i = 0; i < modules->size(); ++i)
            spans->push_back({ (*modules)[i].Base, (*modules)[i].Base + (*modules)[i].Size, i });
        std::sort(spans->begin(), spans->end(), [](const ModuleSpan& a, const ModuleSpan& b) { return a.Base < b.Base; });
        std::lock_guard lock(m_moduleLock);
        m_modules = std::move(modules);
        m_moduleSpans = std::move(spans);
    }

    std::optional<ModuleInfo> Process::ModuleAt(std::uint64_t address) const
    {
        ModuleList modules;
        std::shared_ptr<const std::vector<ModuleSpan>> spans;
        {
            std::lock_guard lock(m_moduleLock);
            modules = m_modules;
            spans = m_moduleSpans;
        }
        if (!modules || !spans)
            return std::nullopt;
        auto next = std::upper_bound(spans->begin(), spans->end(), address, [](std::uint64_t value, const ModuleSpan& span) { return value < span.Base; });
        if (next == spans->begin())
            return std::nullopt;
        --next;
        if (address >= next->End || next->Index >= modules->size())
            return std::nullopt;
        return (*modules)[next->Index];
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
        if (IsMainModuleName(name))
            return MainModule();
        return std::nullopt;
    }

    bool Process::IsMainModuleName(std::string_view name)
    {
        return EqualsNoCase(name, "$process") || EqualsNoCase(name, "mainmodule");
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
        const UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        if (!snapshot)
            return threads;
        THREADENTRY32 entry{ sizeof(entry) };
        for (BOOL more = ::Thread32First(snapshot.Get(), &entry); more; more = ::Thread32Next(snapshot.Get(), &entry))
        {
            if (entry.th32OwnerProcessID == Raw(m_id))
                threads.push_back(entry.th32ThreadID);
        }
        return threads;
    }

    bool Process::Suspend() const
    {
        static const NtProcessCall suspend = NtCall("NtSuspendProcess");
        if (DryRun())
            return m_handle.Get() != nullptr;
        return suspend && m_handle.Get() && suspend(m_handle.Get()) >= 0;
    }

    bool Process::Resume() const
    {
        static const NtProcessCall resume = NtCall("NtResumeProcess");
        if (DryRun())
            return m_handle.Get() != nullptr;
        return resume && m_handle.Get() && resume(m_handle.Get()) >= 0;
    }

    Status Process::CreateThread(std::uint64_t start, std::uint64_t parameter, bool wait, std::uint32_t timeoutMilliseconds) const
    {
        if (!m_handle.Get())
            return Status::Fail(ErrorCode::NoProcess);
        if (DryRun())
        {
            std::lock_guard lock(m_dryLock);
            ++m_dryActivity.Threads;
            return Status::Success();
        }
        const UniqueHandle thread(::CreateRemoteThread(m_handle.Get(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), reinterpret_cast<LPVOID>(parameter), 0, nullptr));
        if (!thread)
            return Status::Fail(ErrorCode::ThreadCreateFailed, Hex(start) + ", " + SystemErrorText(::GetLastError()));
        const DWORD waited = wait ? ::WaitForSingleObject(thread.Get(), timeoutMilliseconds) : WAIT_OBJECT_0;
        if (waited != WAIT_OBJECT_0)
            return Status::Fail(ErrorCode::ThreadTimedOut, Hex(start) + ", " + std::to_string(timeoutMilliseconds) + " ms");
        return Status::Success();
    }

    std::uint64_t Process::FindExport(std::string_view moduleName, std::string_view name) const
    {
        return ExportAddress(moduleName, name, 0);
    }

    std::uint64_t Process::ExportAddress(std::string_view moduleName, std::string_view name, int depth) const
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
        if (directory.NumberOfNames > kMaxModuleExports || directory.NumberOfFunctions > kMaxModuleExports)
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
            if (name != buffer || ordinals[i] >= functions.size())
                continue;
            const DWORD rva = functions[ordinals[i]];
            if (rva < exportRva || rva >= exportRva + exportSize)
                return module->Base + rva;
            char forward[256] = {};
            if (depth >= 4 || Read(module->Base + rva, forward, sizeof(forward) - 1) == 0)
                return 0;
            const std::string_view target(forward);
            const std::size_t dot = target.rfind('.');
            if (dot == std::string_view::npos || dot == 0 || dot + 1 >= target.size() || target[dot + 1] == '#')
                return 0;
            return ExportAddress(std::string(target.substr(0, dot)) + ".dll", target.substr(dot + 1), depth + 1);
        }
        return 0;
    }

    Status Process::InjectLibrary(const std::wstring& path) const
    {
        if (!m_handle.Get())
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
        const Result<Address> allocated = Allocate(bytes, PAGE_READWRITE);
        if (!allocated)
            return allocated.Error();
        const std::uint64_t memory = *allocated;
        if (Status written = Write(memory, path.c_str(), bytes); !written)
        {
            Free(memory);
            return written;
        }
        const UniqueHandle thread(::CreateRemoteThread(m_handle.Get(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), reinterpret_cast<LPVOID>(memory), 0, nullptr));
        if (!thread)
        {
            const DWORD error = ::GetLastError();
            Free(memory);
            return Status::Fail(ErrorCode::ThreadCreateFailed, SystemErrorText(error));
        }
        constexpr DWORD kLoadTimeout = 15000;
        const DWORD waited = ::WaitForSingleObject(thread.Get(), kLoadTimeout);
        DWORD exitCode = 0;
        const bool finished = waited == WAIT_OBJECT_0 && ::GetExitCodeThread(thread.Get(), &exitCode);
        if (waited != WAIT_OBJECT_0)
            return Status::Fail(ErrorCode::ThreadTimedOut, WideToUtf8(path) + ", " + std::to_string(kLoadTimeout) + " ms");
        Free(memory);
        const_cast<Process*>(this)->RefreshModules();
        if ((!finished || exitCode == 0) && !FindModule(WideToUtf8(std::filesystem::path(path).filename().wstring())))
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
