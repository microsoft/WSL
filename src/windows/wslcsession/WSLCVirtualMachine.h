/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVirtualMachine.h

Abstract:

    WSLCVirtualMachine manages the client-side lifecycle of a WSLC virtual machine.

    The VM is created via IWSLCVirtualMachine (running in the SYSTEM service), and this class
    connects to the existing VM for unprivileged operations. Privileged operations
    like AttachDisk and AddShare are delegated back to IWSLCVirtualMachine.

--*/
#pragma once
#include "wslc.h"
#include "hcs.hpp"
#include "WSLCProcess.h"
#include "WSLCContainerMetadata.h"
#include <thread>
#include <filesystem>

namespace wsl::windows::service::wslc {

enum WSLCMountFlags
{
    WSLCMountFlagsNone = 0,
    WSLCMountFlagsReadOnly = 1,
    WSLCMountFlagsChroot = 2,
    WSLCMountFlagsWriteableOverlayFs = 4,
};

enum WSLCFdType
{
    WSLCFdTypeDefault = 0,
    WSLCFdTypeTty = 1,
    WSLCFdTypeTtyControl = 2,
};

struct WSLCProcessFd
{
    LONG Fd{};
    WSLCFdType Type{};
};

class WSLCVirtualMachine;

struct VmPortAllocation
{
    NON_COPYABLE(VmPortAllocation);

    VmPortAllocation(uint16_t port, int Family, int Protocol, WSLCVirtualMachine& vm);
    VmPortAllocation(VmPortAllocation&& Other);
    ~VmPortAllocation();

    VmPortAllocation& operator=(VmPortAllocation&& Other);

    void Reset();
    void Release();
    uint16_t Port() const;
    int Family() const;
    int Protocol() const;

private:
    uint16_t m_port{};
    int m_family{};
    int m_protocol{};
    WSLCVirtualMachine* m_vm{};
};

struct VMPortMapping
{
    NON_COPYABLE(VMPortMapping);

    VMPortMapping(int Protocol, int Family, uint16_t Port, const char* Address);
    ~VMPortMapping();

    VMPortMapping(VMPortMapping&& Other);
    VMPortMapping& operator=(VMPortMapping&& Other);

    void AssignVmPort(const std::shared_ptr<VmPortAllocation>& Port);

    void Unmap();
    void Release();
    bool IsLocalhost() const;
    bool IsIpv6() const;
    std::string BindingAddressString() const;
    void Attach(WSLCVirtualMachine& Vm);
    void Detach();
    uint16_t HostPort() const;

    static VMPortMapping LocalhostTcpMapping(int Family, uint16_t WindowsPort);
    static VMPortMapping FromWSLCPortMapping(const ::WSLCPortMapping& Mapping);
    static VMPortMapping FromContainerMetaData(const wslc::WSLCPortMapping& Mapping);

    int Protocol{};
    std::shared_ptr<VmPortAllocation> VmPort;
    SOCKADDR_INET BindAddress{};

private:
    static SOCKADDR_INET ParseBindingAddress(int Family, uint16_t Port, const char* Address);

    WSLCVirtualMachine* Vm{};
};

class WSLCVirtualMachine
{
public:
    struct ConnectedSocket
    {
        int Fd{};
        wil::unique_socket Socket;
    };

    using TPrepareCommandLine = std::function<void(const std::vector<ConnectedSocket>&)>;

    WSLCVirtualMachine(_In_ IWSLCVirtualMachine* Vm, _In_ const WSLCSessionInitSettings* Settings);
    ~WSLCVirtualMachine();

    void Initialize();

    void MapPort(VMPortMapping& Mapping);
    void UnmapPort(VMPortMapping& Mapping);
    void Unmount(_In_ const char* Path);

    HRESULT MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly);
    HRESULT UnmountWindowsFolder(_In_ LPCSTR LinuxPath);
    void Signal(_In_ LONG Pid, _In_ int Signal);

    void OnProcessReleased(int Pid);

    std::shared_ptr<VmPortAllocation> TryAllocatePort(uint16_t Port, int Family, int Protocol);
    std::shared_ptr<VmPortAllocation> AllocatePort(int Family, int Protocol);
    void ReleasePort(VmPortAllocation& Port);

    Microsoft::WRL::ComPtr<WSLCProcess> CreateLinuxProcess(
        _In_ LPCSTR Executable,
        _In_ const WSLCProcessOptions& Options,
        int* Errno = nullptr,
        const TPrepareCommandLine& PrepareCommandLine = [](const auto&) {});

    std::pair<ULONG, std::string> AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly);
    void DetachDisk(_In_ ULONG Lun);
    void Ext4Format(_In_ const std::string& Device);
    void Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags);

    wil::unique_socket ConnectUnixSocket(_In_ const char* Path);
    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(enum WSLC_FORK::ForkType Type);

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
    void MapRelayPort(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort, _In_ bool Remove);

    // Initial setup during Connect()
    void ConfigureNetworking();

    static void Mount(wsl::shared::SocketChannel& Channel, LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags);
    void MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint);

    Microsoft::WRL::ComPtr<WSLCProcess> CreateLinuxProcessImpl(
        _In_ LPCSTR Executable,
        _In_ const WSLCProcessOptions& Options,
        _In_ const std::vector<WSLCProcessFd>& Fds = {},
        int* Errno = nullptr,
        const TPrepareCommandLine& PrepareCommandLine = [](const auto&) {});

    bool FeatureEnabled(WSLCFeatureFlags Flag) const;

    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(
        wsl::shared::SocketChannel& Channel, enum WSLC_FORK::ForkType Type, ULONG TtyRows = 0, ULONG TtyColumns = 0);
    int32_t ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel);

    ConnectedSocket ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd);
    std::string GetVhdDevicePath(ULONG Lun);
    void LaunchPortRelay();

    HRESULT MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ WSLCMountFlags Flags = WSLCMountFlagsNone);

    void WatchForExitedProcesses(wsl::shared::SocketChannel& Channel);

    void CollectCrashDumps(wil::unique_socket&& listenSocket);

    struct AttachedDisk
    {
        std::filesystem::path Path;
        std::string Device;
    };

    // IWSLCVirtualMachine for privileged operations on this VM
    wil::com_ptr<IWSLCVirtualMachine> m_vm;

    WSLCFeatureFlags m_featureFlags{};
    WSLCNetworkingMode m_networkingMode{};
    ULONG m_bootTimeoutMs{};

    std::string m_rootVhdType;

    std::thread m_processExitThread;
    std::thread m_crashDumpThread;

    std::set<uint16_t> m_allocatedPorts;

    GUID m_vmId{};

    std::mutex m_trackedProcessesLock;
    std::vector<std::weak_ptr<VMProcessControl>> m_trackedProcesses;

    wil::unique_event m_vmTerminatingEvent{wil::EventOptions::ManualReset};

    wsl::shared::SocketChannel m_initChannel;
    wil::unique_handle m_portRelayChannelRead;
    wil::unique_handle m_portRelayChannelWrite;

    std::map<ULONG, AttachedDisk> m_attachedDisks;
    std::map<std::string, GUID> m_mountedWindowsFolders;
    std::recursive_mutex m_lock;
    std::mutex m_portRelaylock;
};
} // namespace wsl::windows::service::wslc