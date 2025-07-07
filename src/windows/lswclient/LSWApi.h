#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

HRESULT GetWslVersion(WSL_VERSION* Version);

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
};

struct VirtualMachineSettings
{
    LPCWSTR DisplayName;
    struct Memory Memory;
    struct CPU CPU;
    struct Options Options;
};

typedef HANDLE LSWVirtualMachineHandle;

HRESULT CreateVirualMachine(const struct VirtualMachineSettings* Settings, struct LSWVirtualMachineHandle* VirtualMachine);

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

HRESULT AttachDisk(LSWVirtualMachineHandle* VirtualMachine, const struct DiskAttachSettings* Settings, struct AttachedDiskInformation* AttachedDisk);

struct MountSettings
{
    const char* Device;
    const char* Target;
    const char* Type;
    const char* Options;
    BOOL Chroot;
};

HRESULT Mount(LSWVirtualMachineHandle* VirtualMachine, const struct MountSettings* Settings);

struct ProcessFileDescriptorSettings
{
    uint32_t Number;
    BOOL Tty;
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

HRESULT CreateLinuxProcess(LSWVirtualMachineHandle* VirtualMachine, struct CreateProcessSettings* Settings, int32_t* Pid);

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

HRESULT WaitForLinuxProcess(LSWVirtualMachineHandle* VirtualMachine, int32_t Pid, uint64_t TimeoutMs, struct WaitResult* Result);

HRESULT SignalLinuxProcess(LSWVirtualMachineHandle* VirtualMachine, int32_t Pid, int32_t Signal);

#ifdef __cplusplus
}
#endif