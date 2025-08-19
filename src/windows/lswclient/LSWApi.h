/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLA.h

Abstract:

    This file contains the WSLA api definitions.

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

HRESULT WslGetVersion(struct WSL_VERSION_INFORMATION* Version);

struct WslVmMemory
{
    uint64_t MemoryMb;
};

struct WslVmCPU
{
    uint32_t CpuCount;
};

enum WslVirtualMachineTerminationReason
{
    WslVirtualMachineTerminationReasonUnknown,
    WslVirtualMachineTerminationReasonShutdown,
    WslVirtualMachineTerminationReasonCrashed,
};

typedef HRESULT (*WslVirtualMachineTerminationCallback)(void*, enum WslVirtualMachineTerminationReason, LPCWSTR);

struct WslVmOptions
{
    uint32_t BootTimeoutMs;
    HANDLE Dmesg;
    WslVirtualMachineTerminationCallback TerminationCallback;
    void* TerminationContext;
    bool EnableDebugShell;
    bool EnableEarlyBootDmesg;
};

enum WslNetworkingMode
{
    WslNetworkingModeNone,
    WslNetworkingModeNAT
};

struct WslVmNetworking
{
    enum WslNetworkingMode Mode;
    bool DnsTunneling; // Not implemented yet.
};

struct WslVmGPU
{
    bool Enable;
};

struct WslVirtualMachineSettings
{
    LPCWSTR DisplayName;
    struct WslVmMemory Memory;
    struct WslVmCPU CPU;
    struct WslVmOptions Options;
    struct WslVmNetworking Networking;
    struct WslVmGPU GPU;
};

typedef void* WslVirtualMachineHandle;

HRESULT WslCreateVirtualMachine(const struct WslVirtualMachineSettings* Settings, WslVirtualMachineHandle* VirtualMachine);

struct WslDiskAttachSettings
{
    LPCWSTR WindowsPath;
    bool ReadOnly;
};

struct WslAttachedDiskInformation
{
    ULONG ScsiLun;
    char Device[10];
};

HRESULT WslAttachDisk(WslVirtualMachineHandle VirtualMachine, const struct WslDiskAttachSettings* Settings, struct WslAttachedDiskInformation* AttachedDisk);

enum WslMountFlags
{
    WslMountFlagsNone = 0,
    WslMountFlagsChroot = 1,
    WslMountFlagsWriteableOverlayFs = 2,
};

struct WslMountSettings
{
    const char* Device;
    const char* Target;
    const char* Type;
    const char* Options;
    uint32_t Flags;
};

HRESULT WslMount(WslVirtualMachineHandle VirtualMachine, const struct WslMountSettings* Settings);

enum WslFdType
{
    WslFdTypeDefault = 0,
    WslFdTypeTerminalInput = 1,
    WslFdTypeTerminalOutput = 2,
    WslFdTypeLinuxFileInput = 4,
    WslFdTypeLinuxFileOutput = 8,
    WslFdTypeLinuxFileAppend = 16,
    WslFdTypeLinuxFileCreate = 32,
};

struct WslProcessFileDescriptorSettings
{
    int32_t Number;
    enum WslFdType Type;
    const char* Path; // Required when 'Type' has LinuxFileInput or LinuxFileOutput
    HANDLE Handle;
};

struct WslCreateProcessSettings
{
    const char* Executable;
    char const** Arguments;
    char const** Environment;
    const char* CurrentDirectory;
    uint32_t FdCount;
    struct WslProcessFileDescriptorSettings* FileDescriptors;
};

HRESULT WslCreateLinuxProcess(WslVirtualMachineHandle VirtualMachine, struct WslCreateProcessSettings* Settings, int32_t* Pid);

enum WslProcessState
{
    WslProcessStateUnknown,
    WslProcessStateRunning,
    WslProcessStateExited,
    WslProcessStateSignaled
};

struct WslWaitResult
{
    enum ProcessState State;
    int32_t Code; // Signal number or exit code
};

struct WslPortMappingSettings
{
    uint16_t WindowsPort;
    uint16_t LinuxPort;
    int AddressFamily;
};

HRESULT WslLaunchInteractiveTerminal(HANDLE Input, HANDLE Output, HANDLE* Process);

HRESULT WslWaitForLinuxProcess(WslVirtualMachineHandle VirtualMachine, int32_t Pid, uint64_t TimeoutMs, struct WslWaitResult* Result);

HRESULT WslSignalLinuxProcess(WslVirtualMachineHandle VirtualMachine, int32_t Pid, int32_t Signal);

HRESULT WslShutdownVirtualMachine(WslVirtualMachineHandle VirtualMachine, uint64_t TimeoutMs);

void WslReleaseVirtualMachine(WslVirtualMachineHandle VirtualMachine);

HRESULT WslLaunchDebugShell(WslVirtualMachineHandle VirtualMachine, HANDLE* Process); // Used for development, might remove

HRESULT WslMapPort(WslVirtualMachineHandle VirtualMachine, const struct WslPortMappingSettings* Settings);

HRESULT WslUnmapPort(WslVirtualMachineHandle VirtualMachine, const struct WslPortMappingSettings* Settings);

HRESULT WslUnmount(WslVirtualMachineHandle VirtualMachine, const char* Path);

HRESULT WslDetachDisk(WslVirtualMachineHandle VirtualMachine, ULONG Lun);

enum WslInstallComponent
{
    WslInstallComponentNone = 0,
    WslInstallComponentVMPOC = 1,
    WslInstallComponentWslOC = 2,
    WslInstallComponentWslPackage = 4,
};

HRESULT WslQueryMissingComponents(enum WslInstallComponent* Components);

typedef void (*WslInstallCallback)(enum WslInstallComponent, uint64_t, uint64_t, void*);

HRESULT WslInstallComponents(enum WslInstallComponent Components, WslInstallCallback ProgressCallback, void* Context);

// Used for testing until the package is published.
HRESULT WslSetPackageUrl(LPCWSTR Url);

HRESULT WslMountWindowsFolder(WslVirtualMachineHandle VirtualMachine, LPCWSTR WindowsPath, const char* LinuxPath, BOOL ReadOnly);

HRESULT WslUnmountWindowsFolder(WslVirtualMachineHandle VirtualMachine, const char* LinuxPath);

HRESULT WslMountGpuLibraries(WslVirtualMachineHandle VirtualMachine, const char* LibrariesMountPoint, const char* DriversMountpoint);

HRESULT WslMountGpuLibraries(LSWVirtualMachineHandle VirtualMachine, const char* LibrariesMountPoint, const char* DriversMountpoint);

#ifdef __cplusplus
}
#endif