#pragma once
#include <winternl.h>

inline unsigned int OpenSysCall = 0;
inline unsigned int ReadVmSysCall = 0;
inline unsigned int WriteVmSysCall = 0;

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