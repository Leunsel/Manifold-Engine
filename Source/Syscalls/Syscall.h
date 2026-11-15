#pragma once
#include <winternl.h>

inline unsigned int OpenSysCall = 0;
inline unsigned int ReadVmSysCall = 0;
inline unsigned int WriteVmSysCall = 0;
inline unsigned int QueryVmSysCall = 0;
inline unsigned int ProtectVmSysCall = 0;
inline unsigned int AllocVmSysCall = 0;
inline unsigned int FreeVmSysCall = 0;

void InitSysCalls();

__attribute__((naked)) NTSTATUS NTAPI NtOpenProcess_direct(
	_Out_ PHANDLE ProcessHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ OBJECT_ATTRIBUTES* ObjectAttributes,
	_In_opt_ CLIENT_ID* ClientId
);

__attribute__((naked)) NTSTATUS NTAPI NtReadVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_In_opt_ PVOID BaseAddress,
	_Out_ PVOID Buffer,
	_In_ SIZE_T NumberOfBytesToRead,
	_Out_opt_ PSIZE_T NumberOfBytesRead
);

__attribute__((naked)) NTSTATUS NTAPI NtWriteVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_In_opt_ PVOID BaseAddress,
	_In_ PVOID Buffer,
	_In_ SIZE_T NumberOfBytesToWrite,
	_Out_opt_ PSIZE_T NumberOfBytesWritten
);

__attribute__((naked)) NTSTATUS NTAPI NtQueryVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_In_opt_ PVOID BaseAddress,
	_In_ int MemoryInformationClass,
	_Out_ PVOID MemoryInformation,
	_In_ SIZE_T MemoryInformationLength,
	_Out_opt_ PSIZE_T ReturnLength
);

__attribute__((naked)) NTSTATUS NTAPI NtProtectVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG NewProtect,
	_Out_ PULONG OldProtect
);

__attribute__((naked)) NTSTATUS NTAPI NtAllocateVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_In_ ULONG_PTR ZeroBits,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG AllocationType,
	_In_ ULONG Protect
);

__attribute__((naked)) NTSTATUS NTAPI NtFreeVirtualMemory_direct(
	_In_ HANDLE ProcessHandle,
	_Inout_ PVOID* BaseAddress,
	_Inout_ PSIZE_T RegionSize,
	_In_ ULONG FreeType
);