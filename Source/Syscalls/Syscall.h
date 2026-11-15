#pragma once
#include <winternl.h>

inline unsigned int OpenSysCall = 0;

void InitSysCalls();

__attribute__((naked)) NTSTATUS NtOpenProcess_direct(
	_Out_ PHANDLE ProcessHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ OBJECT_ATTRIBUTES* ObjectAttributes,
	_In_opt_ CLIENT_ID* ClientId
);