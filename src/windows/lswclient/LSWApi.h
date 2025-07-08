/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWApi.h

Abstract:

    TODO

--*/
#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

HRESULT WslGetVersion(WSL_VERSION* Version);

struct Memory
{
    uint64_t MemoryMb;
};

struct CPU
{
    uint32_t CpuCount;
};

struct Options
{
    uint32_t BootTimeoutMs;
    HANDLE Dmesg;
};

struct VirtualMachineSettings
{
    LPCWSTR DisplayName;  // Not implemented yet
    struct Memory Memory; // Not implemented yet
    struct CPU CPU;       // Not implemented yet
    struct Options Options;
};

typedef void* LSWVirtualMachineHandle;

HRESULT WslCreateVirualMachine(const struct VirtualMachineSettings* Settings, LSWVirtualMachineHandle* VirtualMachine);

struct DiskAttachSettings
{
    LPCWSTR WindowsPath;
    bool ReadOnly;
};

struct AttachedDiskInformation
{
    ULONG ScsiLun;
    char Device[10];
};

HRESULT WslAttachDisk(LSWVirtualMachineHandle VirtualMachine, const struct DiskAttachSettings* Settings, AttachedDiskInformation* AttachedDisk);

struct MountSettings
{
    const char* Device;
    const char* Target;
    const char* Type;
    const char* Options;
    BOOL Chroot;
};

HRESULT WslMount(LSWVirtualMachineHandle VirtualMachine, const struct MountSettings* Settings);

struct ProcessFileDescriptorSettings
{
    uint32_t Number;
    BOOL Tty; // Not implemented yet
    HANDLE Handle;
};

struct CreateProcessSettings
{
    const char* Executable;
    char const** Arguments;
    char const** Environment;
    const char* CurrentDirectory;
    uint32_t FdCount;
    struct ProcessFileDescriptorSettings* FileDescriptors;
};

HRESULT WslCreateLinuxProcess(LSWVirtualMachineHandle VirtualMachine, struct CreateProcessSettings* Settings, int32_t* Pid);

enum ProcessState
{
    ProcessStateUnknown,
    ProcessStateRunning,
    ProcessStateExited,
    ProcessStateSignaled
};

struct WaitResult
{
    ProcessState State;
    int32_t Code; // Signal number or exit code
};

HRESULT WslWaitForLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, uint64_t TimeoutMs, struct WaitResult* Result);

HRESULT WslSignalLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, int32_t Signal);

HRESULT WslShutdownVirtualMachine(LSWVirtualMachineHandle VirtualMachine, uint64_t TimeoutMs);

void WslReleaseVirtualMachine(LSWVirtualMachineHandle VirtualMachine);

#ifdef __cplusplus
}
#endif