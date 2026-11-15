// Stand-alone self-test for the direct NtOpenProcess system call stub.
//
// It exercises both halves of Source/Syscalls/Syscall.cpp without pulling in
// the rest of the engine, so it can be built for x86 as well as x64 (the full
// app and the injected agent are x64-only). InitSysCalls() must read the
// correct service number out of whichever ntdll this build maps, and the naked
// stub must issue the call for the matching architecture; opening our own
// process is the simplest call that should always succeed.

#include <windows.h>
#include <winternl.h>
#include <cstdio>

#include "Syscalls/Syscall.h"

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN64)
    const char* arch = "x64";
#else
    const char* arch = "x86 (WoW64)";
#endif
    std::printf("architecture: %s\n", arch);

    InitSysCalls();
    std::printf("NtOpenProcess service number = 0x%X (%u)\n", OpenSysCall, OpenSysCall);
    if (OpenSysCall == 0)
    {
        std::printf("FAILED: the parser did not find a service number in ntdll\n");
        return 1;
    }

    HANDLE handle = nullptr;
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, nullptr, 0, nullptr, nullptr);
    CLIENT_ID cid;
    cid.UniqueProcess = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(::GetCurrentProcessId()));
    cid.UniqueThread = nullptr;

    const ACCESS_MASK access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    const NTSTATUS status = NtOpenProcess_direct(&handle, access, &attributes, &cid);
    std::printf("NtOpenProcess_direct        -> status = 0x%08lX, handle = %p\n",
                static_cast<unsigned long>(status), static_cast<void*>(handle));
    if (status != 0 || handle == nullptr)
    {
        std::printf("FAILED: direct system call did not open the process\n");
        return 1;
    }

    std::printf("NtReadVirtualMemory  service number = 0x%X\n", ReadVmSysCall);
    std::printf("NtWriteVirtualMemory service number = 0x%X\n", WriteVmSysCall);
    std::printf("NtQueryVirtualMemory service number = 0x%X\n", QueryVmSysCall);
    std::printf("NtProtectVirtualMemory  service number = 0x%X\n", ProtectVmSysCall);
    std::printf("NtAllocateVirtualMemory service number = 0x%X\n", AllocVmSysCall);
    std::printf("NtFreeVirtualMemory  service number = 0x%X\n", FreeVmSysCall);
    std::printf("NtQueryInformationProcess service number = 0x%X\n", QueryInfoProcSysCall);
    std::printf("NtGetContextThread service number = 0x%X\n", GetContextSysCall);
    std::printf("NtSetContextThread service number = 0x%X\n", SetContextSysCall);
    std::printf("NtSuspendThread    service number = 0x%X\n", SuspendThreadSysCall);
    std::printf("NtResumeThread     service number = 0x%X\n", ResumeThreadSysCall);

    int failures = 0;

    // Read a known value back out of our own address space (volatile so the
    // compiler cannot fold the value it never saw a normal store/load for).
    volatile unsigned int probe = 0xCAFEB0BAu;
    volatile unsigned int readBack = 0;
    SIZE_T moved = 0;
    const NTSTATUS rs = NtReadVirtualMemory_direct(
        handle, const_cast<unsigned int*>(&probe), const_cast<unsigned int*>(&readBack), sizeof(unsigned int), &moved);
    const bool readOk = rs >= 0 && moved == sizeof(unsigned int) && readBack == 0xCAFEB0BAu;
    std::printf("NtReadVirtualMemory_direct  -> status = 0x%08lX, value = 0x%08X -> %s\n",
                static_cast<unsigned long>(rs), readBack, readOk ? "OK" : "FAILED");
    failures += readOk ? 0 : 1;

    // Write a value into a second local and read it back with a normal load.
    volatile unsigned int sink = 0;
    const unsigned int source = 0x1234ABCDu;
    moved = 0;
    const NTSTATUS ws = NtWriteVirtualMemory_direct(
        handle, const_cast<unsigned int*>(&sink), const_cast<unsigned int*>(&source), sizeof(unsigned int), &moved);
    const bool writeOk = ws >= 0 && moved == sizeof(unsigned int) && sink == 0x1234ABCDu;
    std::printf("NtWriteVirtualMemory_direct -> status = 0x%08lX, value = 0x%08X -> %s\n",
                static_cast<unsigned long>(ws), sink, writeOk ? "OK" : "FAILED");
    failures += writeOk ? 0 : 1;

    // Allocate a page in our own address space through the direct call, round-trip
    // a marker through it, flip its protection, query it back, then release it —
    // every step a direct system call.
    PVOID region = nullptr;
    SIZE_T regionSize = 0x1000;
    const NTSTATUS as = NtAllocateVirtualMemory_direct(
        handle, &region, 0, &regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    const bool allocOk = as >= 0 && region != nullptr && regionSize >= 0x1000;
    std::printf("NtAllocateVirtualMemory_direct -> status = 0x%08lX, base = %p, size = 0x%llX -> %s\n",
                static_cast<unsigned long>(as), region,
                static_cast<unsigned long long>(regionSize), allocOk ? "OK" : "FAILED");
    failures += allocOk ? 0 : 1;

    if (allocOk)
    {
        const unsigned int marker = 0xFEEDFACEu;
        unsigned int echo = 0;
        SIZE_T n = 0;
        NtWriteVirtualMemory_direct(handle, region, const_cast<unsigned int*>(&marker), sizeof(marker), &n);
        NtReadVirtualMemory_direct(handle, region, &echo, sizeof(echo), &n);
        const bool roundTrip = echo == 0xFEEDFACEu;
        std::printf("  write+read back in the allocated page -> 0x%08X -> %s\n",
                    echo, roundTrip ? "OK" : "FAILED");
        failures += roundTrip ? 0 : 1;

        PVOID protBase = region;
        SIZE_T protSize = 0x1000;
        ULONG oldProtect = 0;
        const NTSTATUS ps = NtProtectVirtualMemory_direct(handle, &protBase, &protSize, PAGE_NOACCESS, &oldProtect);
        const bool protectOk = ps >= 0 && oldProtect == PAGE_READWRITE;
        std::printf("NtProtectVirtualMemory_direct  -> status = 0x%08lX, old = 0x%lX -> %s\n",
                    static_cast<unsigned long>(ps), static_cast<unsigned long>(oldProtect),
                    protectOk ? "OK" : "FAILED");
        failures += protectOk ? 0 : 1;

        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T returned = 0;
        const NTSTATUS qs = NtQueryVirtualMemory_direct(
            handle, region, 0 /*MemoryBasicInformation*/, &mbi, sizeof(mbi), &returned);
        const bool queryOk = qs >= 0 && returned >= sizeof(mbi) && mbi.State == MEM_COMMIT;
        std::printf("NtQueryVirtualMemory_direct    -> status = 0x%08lX, state = 0x%lX, protect = 0x%lX -> %s\n",
                    static_cast<unsigned long>(qs), static_cast<unsigned long>(mbi.State),
                    static_cast<unsigned long>(mbi.Protect), queryOk ? "OK" : "FAILED");
        failures += queryOk ? 0 : 1;

        PVOID freeBase = region;
        SIZE_T freeSize = 0;
        const NTSTATUS fs = NtFreeVirtualMemory_direct(handle, &freeBase, &freeSize, MEM_RELEASE);
        const bool freeOk = fs >= 0;
        std::printf("NtFreeVirtualMemory_direct     -> status = 0x%08lX -> %s\n",
                    static_cast<unsigned long>(fs), freeOk ? "OK" : "FAILED");
        failures += freeOk ? 0 : 1;
    }

    // Round-trip our own PEB base through the direct NtQueryInformationProcess
    // and compare against what the TEB gives via the GS/FS register trick — a
    // good smoke test for the service number and the stub. The struct is
    // natively sized per bitness: on x86 the PEB pointer is 32-bit, which puts
    // the whole thing at 0x18 bytes, not 0x30.
    struct PBI
    {
        NTSTATUS ExitStatus;
        PVOID PebBaseAddress;
        ULONG_PTR AffinityMask;
        LONG BasePriority;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR InheritedFromUniqueProcessId;
    } pbi{};
    ULONG returned = 0;
    const NTSTATUS qips = NtQueryInformationProcess_direct(
        handle, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), &returned);
#if defined(_WIN64)
    const unsigned long long ownPeb = __readgsqword(0x60);
#else
    const unsigned long long ownPeb = static_cast<unsigned long long>(__readfsdword(0x30));
#endif
    const unsigned long long reportedPeb = reinterpret_cast<unsigned long long>(pbi.PebBaseAddress);
    const bool qipsOk = qips >= 0 && reportedPeb != 0 && reportedPeb == ownPeb;
    std::printf("NtQueryInformationProcess_direct -> status = 0x%08lX, PEB = 0x%llX (ours 0x%llX) -> %s\n",
                static_cast<unsigned long>(qips), reportedPeb, ownPeb, qipsOk ? "OK" : "FAILED");
    failures += qipsOk ? 0 : 1;

    ::CloseHandle(handle);
    if (failures == 0)
    {
        std::printf("OK: open, read and write all went through direct system calls\n");
        return 0;
    }
    std::printf("FAILED: %d direct call(s) did not behave as expected\n", failures);
    return 1;
}
