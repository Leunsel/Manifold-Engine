#include <string>
#include <map>
#include <Windows.h>

#include "Syscall.h"

std::string GetNtdllPath()
{
	char systemDir[MAX_PATH];
	GetSystemDirectoryA(systemDir, MAX_PATH);

	return std::string(systemDir) + "\\ntdll.dll";
}

std::map<std::string, int> SysCalls;
DWORD RvaToOffset(DWORD rva, PIMAGE_NT_HEADERS nt, PIMAGE_SECTION_HEADER section)
{
	for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
	{
		if (rva >= section[i].VirtualAddress &&
			rva < section[i].VirtualAddress + section[i].Misc.VirtualSize)
		{
			return rva - section[i].VirtualAddress + section[i].PointerToRawData;
		}
	}
	return 0;
}

int GetSysCalls()
{
	HANDLE file = CreateFileA(
		//"C:\\Windows\\System32\\ntdll.dll",
		GetNtdllPath().c_str(),
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		0,
		nullptr);

	if (file == INVALID_HANDLE_VALUE)
		return 1;

	HANDLE mapping = CreateFileMapping(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	BYTE* base = (BYTE*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);

	auto dos = (PIMAGE_DOS_HEADER)base;
	auto nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
	auto section = IMAGE_FIRST_SECTION(nt);

	auto exportDirRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	auto exportDirOffset = RvaToOffset(exportDirRVA, nt, section);
	auto exportDir = (PIMAGE_EXPORT_DIRECTORY)(base + exportDirOffset);

	DWORD* names = (DWORD*)(base + RvaToOffset(exportDir->AddressOfNames, nt, section));
	DWORD* funcs = (DWORD*)(base + RvaToOffset(exportDir->AddressOfFunctions, nt, section));
	WORD* ordinals = (WORD*)(base + RvaToOffset(exportDir->AddressOfNameOrdinals, nt, section));

	for (DWORD i = 0; i < exportDir->NumberOfNames; i++)
	{
		char* name = (char*)(base + RvaToOffset(names[i], nt, section));

		if (strncmp(name, "Nt", 2) != 0)
			continue;

		DWORD funcRVA = funcs[ordinals[i]];
		DWORD funcOffset = RvaToOffset(funcRVA, nt, section);
		BYTE* funcBytes = base + funcOffset;

		// The service number lives in the "mov eax, imm32" that every ntdll
		// stub starts with, but the leading bytes differ per architecture, so
		// we have to read whichever ntdll this build actually maps.
#ifdef _WIN64
		// x64 stub:
		// mov r10, rcx       4C 8B D1
		// mov eax, XX XX XX XX   B8 ..
		// syscall            0F 05
		if (funcBytes[0] == 0x4C &&
			funcBytes[1] == 0x8B &&
			funcBytes[2] == 0xD1 &&
			funcBytes[3] == 0xB8)
		{
			DWORD syscallNumber = *(DWORD*)(funcBytes + 4);
			//std::cout << name << " -> " << syscallNumber << std::endl;
			SysCalls[name] = syscallNumber;
		}
#else
		// x86 (WoW64) stub: the service number is the very first instruction.
		// mov eax, XX XX XX XX   B8 ..
		// (followed by the WoW64 transition; its exact shape varies by build)
		if (funcBytes[0] == 0xB8)
		{
			DWORD syscallNumber = *(DWORD*)(funcBytes + 1);
			//std::cout << name << " -> " << syscallNumber << std::endl;
			SysCalls[name] = syscallNumber;
		}
#endif
	}

	UnmapViewOfFile(base);
	CloseHandle(mapping);
	CloseHandle(file);
	return 0;
}

void InitSysCalls()
{
	int Status = GetSysCalls();
	if (Status != 0)
	{
		// add error here
		return;
	}

	OpenSysCall = SysCalls["NtOpenProcess"];
	ReadVmSysCall = SysCalls["NtReadVirtualMemory"];
	WriteVmSysCall = SysCalls["NtWriteVirtualMemory"];
	QueryVmSysCall = SysCalls["NtQueryVirtualMemory"];
	ProtectVmSysCall = SysCalls["NtProtectVirtualMemory"];
	AllocVmSysCall = SysCalls["NtAllocateVirtualMemory"];
	FreeVmSysCall = SysCalls["NtFreeVirtualMemory"];
	QueryInfoProcSysCall = SysCalls["NtQueryInformationProcess"];
	// A number that stays 0 was not found in ntdll; callers fall back to the
	// Win32 API for that one instead of firing a guessed service number.
}

__attribute__((naked)) NTSTATUS NTAPI NtOpenProcess_direct(
	_Out_ PHANDLE ProcessHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ OBJECT_ATTRIBUTES* ObjectAttributes,
	_In_opt_ CLIENT_ID* ClientId
)
{
#ifdef _WIN64
	// x64: load the service number into eax and issue the syscall directly.
	__asm
	{
		mov r10,rcx
		mov eax, 26h
		cmp [OpenSysCall],0
		je Skip
		 mov eax,[OpenSysCall]
		Skip:
		syscall
		ret
	}
#else
	// x86 (WoW64): a 32-bit stub cannot issue `syscall`; it enters 64-bit mode
	// through the WoW64 transition thunk whose address the loader stores in the
	// TEB at fs:[0xC0]. eax carries the service number and the dispatcher reads
	// the arguments from the 32-bit stack. NtOpenProcess is __stdcall (NTAPI),
	// so the stub cleans its own four DWORD arguments with `ret 10h`.
	// The 26h fallback is the x64 number and is only reached if InitSysCalls
	// found nothing; on x86 the parser above supplies the correct number.
	__asm
	{
		mov eax, 26h
		cmp [OpenSysCall],0
		je Skip
		 mov eax,[OpenSysCall]
		Skip:
		call dword ptr fs:[0C0h]
		ret 10h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtReadVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_In_opt_ PVOID BaseAddress,
	_Out_ PVOID Buffer,
	_In_ SIZE_T NumberOfBytesToRead,
	_Out_opt_ PSIZE_T NumberOfBytesRead
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[ReadVmSysCall]
		syscall
		ret
	}
#else
	// Five __stdcall arguments of four bytes each -> ret 14h.
	__asm
	{
		mov eax,[ReadVmSysCall]
		call dword ptr fs:[0C0h]
		ret 14h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtWriteVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_In_opt_ PVOID BaseAddress,
	_In_ PVOID Buffer,
	_In_ SIZE_T NumberOfBytesToWrite,
	_Out_opt_ PSIZE_T NumberOfBytesWritten
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[WriteVmSysCall]
		syscall
		ret
	}
#else
	__asm
	{
		mov eax,[WriteVmSysCall]
		call dword ptr fs:[0C0h]
		ret 14h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtQueryVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_In_opt_ PVOID BaseAddress,
	_In_ int MemoryInformationClass,
	_Out_ PVOID MemoryInformation,
	_In_ SIZE_T MemoryInformationLength,
	_Out_opt_ PSIZE_T ReturnLength
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[QueryVmSysCall]
		syscall
		ret
	}
#else
	// Six __stdcall arguments of four bytes each -> ret 18h.
	__asm
	{
		mov eax,[QueryVmSysCall]
		call dword ptr fs:[0C0h]
		ret 18h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtProtectVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG NewProtect,
	_Out_ PULONG OldProtect
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[ProtectVmSysCall]
		syscall
		ret
	}
#else
	// Five __stdcall arguments of four bytes each -> ret 14h.
	__asm
	{
		mov eax,[ProtectVmSysCall]
		call dword ptr fs:[0C0h]
		ret 14h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtAllocateVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_In_ ULONG_PTR ZeroBits,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG AllocationType,
	_In_ ULONG Protect
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[AllocVmSysCall]
		syscall
		ret
	}
#else
	// Six __stdcall arguments of four bytes each (ZeroBits is one DWORD on x86) -> ret 18h.
	__asm
	{
		mov eax,[AllocVmSysCall]
		call dword ptr fs:[0C0h]
		ret 18h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtFreeVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG FreeType
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[FreeVmSysCall]
		syscall
		ret
	}
#else
	// Four __stdcall arguments of four bytes each -> ret 10h.
	__asm
	{
		mov eax,[FreeVmSysCall]
		call dword ptr fs:[0C0h]
		ret 10h
	}
#endif
}

__attribute__((naked)) NTSTATUS NTAPI NtQueryInformationProcess_direct(
	_In_ HANDLE ProcessHandle,
	_In_ int ProcessInformationClass,
	_Out_ PVOID ProcessInformation,
	_In_ ULONG ProcessInformationLength,
	_Out_opt_ PULONG ReturnLength
)
{
#ifdef _WIN64
	__asm
	{
		mov r10,rcx
		mov eax,[QueryInfoProcSysCall]
		syscall
		ret
	}
#else
	// Five __stdcall arguments of four bytes each -> ret 14h.
	__asm
	{
		mov eax,[QueryInfoProcSysCall]
		call dword ptr fs:[0C0h]
		ret 14h
	}
#endif
}