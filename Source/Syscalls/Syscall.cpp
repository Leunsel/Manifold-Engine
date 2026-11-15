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

		// Expected pattern (x64):
		// mov r10, rcx       4C 8B D1
		// mov eax, XX XX XX XX
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
	// if OpenSysCall == 0 then error bla bla
}

__attribute__((naked)) NTSTATUS NtOpenProcess_direct(
	_Out_ PHANDLE ProcessHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ OBJECT_ATTRIBUTES* ObjectAttributes,
	_In_opt_ CLIENT_ID* ClientId
)
{
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
}