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
// Destruction (and successful CreateVm consumption of Config) must not race any use of that handle.
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
// TimeoutMs must be nonzero and bounds lock acquisition, connection establishment, and CreateVm together.
// After an ambiguous CreateVm failure, discard both the config and the owning process before retrying.
__declspec(dllimport) HRESULT WslOpenVmmCreateVm(_Inout_ WslOpenVmmConfig** Config, _In_ LPCWSTR SocketPath, _In_ UINT32 TimeoutMs, _Out_ WslOpenVmmVm** Vm);

__declspec(dllimport) void WslOpenVmmDestroyVm(_In_opt_ WslOpenVmmVm* Vm);
// VM calls use TimeoutMs from creation, including time waiting for the per-VM lock.
// Ambiguous RPC failures permanently invalidate mutations; teardown/quit remain available with deadlines.
// Cancellation is nonblocking and may race an RPC, but not destruction. It also requires VM recreation.
__declspec(dllimport) HRESULT WslOpenVmmVmCancelRequests(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmResume(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmTeardown(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmQuit(_Inout_ WslOpenVmmVm* Vm);
__declspec(dllimport) HRESULT WslOpenVmmVmAttachScsiDisk(_Inout_ WslOpenVmmVm* Vm, _In_ UINT32 Controller, _In_ UINT32 Lun, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly);
__declspec(dllimport) HRESULT WslOpenVmmVmDetachScsiDisk(_Inout_ WslOpenVmmVm* Vm, _In_ UINT32 Controller, _In_ UINT32 Lun);
__declspec(dllimport) HRESULT WslOpenVmmVmBindPort(_Inout_ WslOpenVmmVm* Vm, _In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32 Family);
__declspec(dllimport) HRESULT WslOpenVmmVmUnbindPort(_Inout_ WslOpenVmmVm* Vm, _In_ UINT16 HostPort, _In_ UINT16 GuestPort, _In_ BOOL Tcp, _In_ INT32 Family);
__declspec(dllimport) HRESULT WslOpenVmmVmAddShare(_Inout_ WslOpenVmmVm* Vm, _In_ LPCWSTR Tag, _In_ LPCWSTR HostPath, _In_ BOOL ReadOnly);
__declspec(dllimport) HRESULT WslOpenVmmVmRemoveShare(_Inout_ WslOpenVmmVm* Vm, _In_ LPCWSTR Tag);

#ifdef __cplusplus
}
#endif