// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <Windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct WslOpenVmmConfig WslOpenVmmConfig;
typedef struct WslOpenVmmVm WslOpenVmmVm;

// Config and VM handles are opaque Rust-owned objects. Destroy each returned handle exactly once.
__declspec(dllimport) HRESULT WslOpenVmmCreateConfig(_Out_ WslOpenVmmConfig** Config);
__declspec(dllimport) void WslOpenVmmDestroyConfig(_In_opt_ WslOpenVmmConfig* Config);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetKernelPath(_Inout_ WslOpenVmmConfig* Config, _In_ LPCWSTR Path);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetInitrdPath(_Inout_ WslOpenVmmConfig* Config, _In_ LPCWSTR Path);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetKernelCmdLine(_Inout_ WslOpenVmmConfig* Config, _In_ LPCWSTR CommandLine);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetMemoryMb(_Inout_ WslOpenVmmConfig* Config, _In_ UINT64 MemoryMb);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetProcessorCount(_Inout_ WslOpenVmmConfig* Config, _In_ UINT32 Count);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetHvSocketPath(_Inout_ WslOpenVmmConfig* Config, _In_ LPCWSTR Path);
__declspec(dllimport) HRESULT WslOpenVmmConfigAddBootDisk(_Inout_ WslOpenVmmConfig* Config, _In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetConsommeNic(_Inout_ WslOpenVmmConfig* Config, _In_ LPCWSTR NicId, _In_ LPCWSTR MacAddress);
__declspec(dllimport) HRESULT WslOpenVmmConfigAddSerialPort(_Inout_ WslOpenVmmConfig* Config, _In_ UINT32 Port, _In_ LPCWSTR PipeName);
__declspec(dllimport) HRESULT WslOpenVmmConfigSetVirtioConsolePath(_Inout_ WslOpenVmmConfig* Config, _In_ LPCWSTR Path);
// On success, this consumes Config and sets it to nullptr. On failure, Config remains valid and Vm is nullptr.
__declspec(dllimport) HRESULT WslOpenVmmCreateVm(_Inout_ WslOpenVmmConfig** Config, _In_ LPCWSTR PipeName, _In_ UINT32 TimeoutMs, _Out_ WslOpenVmmVm** Vm);

__declspec(dllimport) void WslOpenVmmDestroyVm(_In_opt_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmResume(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmTeardown(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmQuit(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmAttachScsiDisk(_Inout_ WslOpenVmmVm* Vm, _In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly);
__declspec(dllimport) HRESULT WslOpenVmmVmDetachScsiDisk(_Inout_ WslOpenVmmVm* Vm, _In_ UINT32 Controller, _In_ UINT32 Lun);
__declspec(dllimport) HRESULT WslOpenVmmVmBindPort(_Inout_ WslOpenVmmVm* Vm, _In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32 Family);
__declspec(dllimport) HRESULT WslOpenVmmVmUnbindPort(_Inout_ WslOpenVmmVm* Vm, _In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32 Family);

#ifdef __cplusplus
}
#endif