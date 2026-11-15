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

    ::CloseHandle(handle);
    if (failures == 0)
    {
        std::printf("OK: open, read and write all went through direct system calls\n");
        return 0;
    }
    std::printf("FAILED: %d direct call(s) did not behave as expected\n", failures);
    return 1;
}
