/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWApi.h

Abstract:

    TODO

--*/
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WSL_VERSION_INFORMATION
{
    uint32_t Major;
    uint32_t Minor;
    uint32_t Revision;
};

HRESULT WslGetVersion(WSL_VERSION_INFORMATION* Version);

struct Memory
{
    uint64_t MemoryMb;
};

struct CPU
{
    uint32_t CpuCount;
};

enum VirtualMachineTerminationReason
{
    VirtualMachineTerminationReasonUnknown,
    VirtualMachineTerminationReasonShutdown,
    VirtualMachineTerminationReasonCrashed,
};

typedef HRESULT (*VirtualMachineTerminationCallback)(void*, enum VirtualMachineTerminationReason, LPCWSTR);

struct Options
{
    uint32_t BootTimeoutMs;
    HANDLE Dmesg;
    VirtualMachineTerminationCallback TerminationCallback;
    void* TerminationContext;
    bool EnableDebugShell;
};

enum NetworkingMode
{
    NetworkingModeNone,
    NetworkingModeNAT
};

struct Networking
{
    enum NetworkingMode Mode;
    bool DnsTunneling;
};

struct VirtualMachineSettings
{
    LPCWSTR DisplayName;  // Not implemented yet
    struct Memory Memory; // Not implemented yet
    struct CPU CPU;       // Not implemented yet
    struct Options Options;
    struct Networking Networking; // Not implemented yet
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

enum MountFlags
{
    MountFlagsNone = 0,
    MountFlagsChroot = 1,
    MountFlagsWriteableOverlayFs = 2,
};

struct MountSettings
{
    const char* Device;
    const char* Target;
    const char* Type;
    const char* Options;
    uint32_t Flags;
};

HRESULT WslMount(LSWVirtualMachineHandle VirtualMachine, const struct MountSettings* Settings);

enum FileDescriptorType
{
    Default,
    TerminalInput,
    TerminalOutput
};

struct ProcessFileDescriptorSettings
{
    int32_t Number;
    enum FileDescriptorType Type; // Not implemented yet
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
    enum ProcessState State;
    int32_t Code; // Signal number or exit code
};

HRESULT WslLaunchInteractiveTerminal(HANDLE Input, HANDLE Output, HANDLE* Process);

HRESULT WslWaitForLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, uint64_t TimeoutMs, struct WaitResult* Result);

HRESULT WslSignalLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, int32_t Signal);

HRESULT WslShutdownVirtualMachine(LSWVirtualMachineHandle VirtualMachine, uint64_t TimeoutMs);

void WslReleaseVirtualMachine(LSWVirtualMachineHandle VirtualMachine);

HRESULT WslLaunchDebugShell(LSWVirtualMachineHandle VirtualMachine, HANDLE* Process); // Used for development, might remove

#ifdef __cplusplus
}
#endif