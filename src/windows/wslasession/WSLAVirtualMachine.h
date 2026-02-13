/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVirtualMachine.h

Abstract:

    WSLAVirtualMachine manages the client-side lifecycle of a WSLA virtual machine.

    The VM is created via IWSLAVirtualMachine (running in the SYSTEM service), and this class
    connects to the existing VM for unprivileged operations. Privileged operations
    like AttachDisk and AddShare are delegated back to IWSLAVirtualMachine.

--*/
#pragma once
#include "wslaservice.h"
#include "hcs.hpp"
#include "WSLAProcess.h"

namespace wsl::windows::service::wsla {

enum WSLAMountFlags
{
    WSLAMountFlagsNone = 0,
    WSLAMountFlagsReadOnly = 1,
    WSLAMountFlagsChroot = 2,
    WSLAMountFlagsWriteableOverlayFs = 4,
};

enum WSLAFdType
{
    WSLAFdTypeDefault = 0,
    WSLAFdTypeTty = 1,
    WSLAFdTypeTtyControl = 2,
};

struct WSLAProcessFd
{
    LONG Fd{};
    WSLAFdType Type{};
};

class WSLAVirtualMachine
{
public:
    struct ConnectedSocket
    {
        int Fd{};
        wil::unique_socket Socket;
    };

    using TPrepareCommandLine = std::function<void(const std::vector<ConnectedSocket>&)>;

    WSLAVirtualMachine(_In_ IWSLAVirtualMachine* Vm, _In_ const WSLA_SESSION_INIT_SETTINGS* Settings);
    ~WSLAVirtualMachine();

    void MapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort);
    void UnmapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort);
    void Unmount(_In_ const char* Path);

    HRESULT MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly);
    HRESULT UnmountWindowsFolder(_In_ LPCSTR LinuxPath);
    void Signal(_In_ LONG Pid, _In_ int Signal);

    void OnProcessReleased(int Pid);

    bool TryAllocatePort(uint16_t Port);
    std::set<uint16_t> AllocatePorts(uint16_t Count);
    void ReleasePorts(const std::set<uint16_t>& Ports);

    Microsoft::WRL::ComPtr<WSLAProcess> CreateLinuxProcess(
        _In_ LPCSTR Executable,
        _In_ const WSLA_PROCESS_OPTIONS& Options,
        int* Errno = nullptr,
        const TPrepareCommandLine& PrepareCommandLine = [](const auto&) {});

    std::pair<ULONG, std::string> AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly);
    void DetachDisk(_In_ ULONG Lun);
    void Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags);

    wil::unique_socket ConnectUnixSocket(_In_ const char* Path);
    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(enum WSLA_FORK::ForkType Type);

    // Returns an event that is signaled when the VM is being terminated.
    // Use this to cancel pending operations.
    HANDLE TerminatingEvent() const
    {
        return m_vmTerminatingEvent.get();
    }

    GUID VmId() const
    {
        return m_vmId;
    }

private:
    void MapPortImpl(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort, _In_ bool Remove);

    // Initial setup during Connect()
    void ConfigureInitialMounts();
    void ConfigureNetworking();

    static void Mount(wsl::shared::SocketChannel& Channel, LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags);
    void MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint);

    Microsoft::WRL::ComPtr<WSLAProcess> CreateLinuxProcessImpl(
        _In_ LPCSTR Executable,
        _In_ const WSLA_PROCESS_OPTIONS& Options,
        _In_ const std::vector<WSLAProcessFd>& Fds = {},
        int* Errno = nullptr,
        const TPrepareCommandLine& PrepareCommandLine = [](const auto&) {});

    bool FeatureEnabled(WSLAFeatureFlags Flag) const;

    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(
        wsl::shared::SocketChannel& Channel, enum WSLA_FORK::ForkType Type, ULONG TtyRows = 0, ULONG TtyColumns = 0);
    int32_t ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel);

    ConnectedSocket ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd);
    std::string GetVhdDevicePath(ULONG Lun);
    static void OpenLinuxFile(wsl::shared::SocketChannel& Channel, const char* Path, uint32_t Flags, int32_t Fd);
    void LaunchPortRelay();

    HRESULT MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ WSLAMountFlags Flags = WSLAMountFlagsNone);

    void WatchForExitedProcesses(wsl::shared::SocketChannel& Channel);

    struct AttachedDisk
    {
        std::filesystem::path Path;
        std::string Device;
    };

    // IWSLAVirtualMachine for privileged operations on this VM
    wil::com_ptr<IWSLAVirtualMachine> m_vm;

    WSLAFeatureFlags m_featureFlags{};
    WSLANetworkingMode m_networkingMode{};
    ULONG m_bootTimeoutMs{};

    std::string m_rootVhdType;

    std::thread m_processExitThread;

    std::set<uint16_t> m_allocatedPorts;

    GUID m_vmId{};

    std::mutex m_trackedProcessesLock;
    std::vector<VMProcessControl*> m_trackedProcesses;

    wil::unique_event m_vmTerminatingEvent{wil::EventOptions::ManualReset};

    wsl::shared::SocketChannel m_initChannel;
    wil::unique_handle m_portRelayChannelRead;
    wil::unique_handle m_portRelayChannelWrite;

    std::map<ULONG, AttachedDisk> m_attachedDisks;
    std::map<std::string, GUID> m_mountedWindowsFolders;
    std::recursive_mutex m_lock;
    std::mutex m_portRelaylock;
};
} // namespace wsl::windows::service::wsla