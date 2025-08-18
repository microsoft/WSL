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

HRESULT WslGetVersion(struct WSL_VERSION_INFORMATION* Version);

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
    bool EnableEarlyBootDmesg;
};

enum NetworkingMode
{
    NetworkingModeNone,
    NetworkingModeNAT
};

struct Networking
{
    enum NetworkingMode Mode;
    bool DnsTunneling; // Not implemented yet.
};

struct GPU
{
    bool Enable;
};

struct VirtualMachineSettings
{
    LPCWSTR DisplayName;
    struct Memory Memory;
    struct CPU CPU;
    struct Options Options;
    struct Networking Networking;
    struct GPU GPU;
};

typedef void* LSWVirtualMachineHandle;

HRESULT WslCreateVirtualMachine(const struct VirtualMachineSettings* Settings, LSWVirtualMachineHandle* VirtualMachine);

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

HRESULT WslAttachDisk(LSWVirtualMachineHandle VirtualMachine, const struct DiskAttachSettings* Settings, struct AttachedDiskInformation* AttachedDisk);

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
    Default = 0,
    TerminalInput = 1,
    TerminalOutput = 2,
    LinuxFileInput = 4,
    LinuxFileOutput = 8,
    LinuxFileAppend = 16,
    LinuxFileCreate = 32,
};

struct ProcessFileDescriptorSettings
{
    int32_t Number;
    enum FileDescriptorType Type;
    const char* Path; // Required when 'Type' has LinuxFileInput or LinuxFileOutput
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

struct PortMappingSettings
{
    uint16_t WindowsPort;
    uint16_t LinuxPort;
    int AddressFamily;
};

HRESULT WslLaunchInteractiveTerminal(HANDLE Input, HANDLE Output, HANDLE* Process);

HRESULT WslWaitForLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, uint64_t TimeoutMs, struct WaitResult* Result);

HRESULT WslSignalLinuxProcess(LSWVirtualMachineHandle VirtualMachine, int32_t Pid, int32_t Signal);

HRESULT WslShutdownVirtualMachine(LSWVirtualMachineHandle VirtualMachine, uint64_t TimeoutMs);

void WslReleaseVirtualMachine(LSWVirtualMachineHandle VirtualMachine);

HRESULT WslLaunchDebugShell(LSWVirtualMachineHandle VirtualMachine, HANDLE* Process); // Used for development, might remove

HRESULT WslMapPort(LSWVirtualMachineHandle VirtualMachine, const struct PortMappingSettings* Settings);

HRESULT WslUnmapPort(LSWVirtualMachineHandle VirtualMachine, const struct PortMappingSettings* Settings);

HRESULT WslUnmount(LSWVirtualMachineHandle VirtualMachine, const char* Path);

HRESULT WslDetachDisk(LSWVirtualMachineHandle VirtualMachine, ULONG Lun);

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

HRESULT WslMountWindowsFolder(LSWVirtualMachineHandle VirtualMachine, LPCWSTR WindowsPath, const char* LinuxPath, BOOL ReadOnly);

HRESULT WslUnmountWindowsFolder(LSWVirtualMachineHandle VirtualMachine, const char* LinuxPath);

HRESULT WslMountGpuLibraries(LSWVirtualMachineHandle VirtualMachine, const char* LibrariesMountPoint, const char* DriversMountpoint);

#ifdef __cplusplus
}
#endif