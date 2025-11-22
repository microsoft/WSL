/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAVirtualMachine.h

Abstract:

    TODO

--*/
#pragma once
#include "wslaservice.h"
#include "INetworkingEngine.h"
#include "hcs.hpp"
#include "Dmesg.h"
#include "WSLAApi.h"
#include "WSLAProcess.h"
#include "DeviceHostProxy.h"
#include "DnsResolver.h"

namespace wsl::windows::service::wsla {

enum WSLAMountFlags
{
    WSLAMountFlagsNone = 0,
    WSLAMountFlagsChroot = 1,
    WSLAMountFlagsWriteableOverlayFs = 2,
};

class WSLAUserSessionImpl;

class DECLSPEC_UUID("0CFC5DC1-B6A7-45FC-8034-3FA9ED73CE30") WSLAVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLAVirtualMachine, IFastRundown>

{
public:
    struct ConnectedSocket
    {
        int Fd;
        wil::unique_socket Socket;
    };

    using TPrepareCommandLine = std::function<void(const std::vector<ConnectedSocket>&)>;

    WSLAVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID Sid, WSLAUserSessionImpl* UserSession);

    ~WSLAVirtualMachine();

    void Start();
    void OnSessionTerminated();

    IFACEMETHOD(CreateLinuxProcess(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno)) override;
    IFACEMETHOD(WaitPid(_In_ LONG Pid, _In_ ULONGLONG TimeoutMs, _Out_ ULONG* State, _Out_ int* Code)) override;
    IFACEMETHOD(Signal(_In_ LONG Pid, _In_ int Signal)) override;
    IFACEMETHOD(Shutdown(ULONGLONG _In_ TimeoutMs)) override;
    IFACEMETHOD(GetDebugShellPipe(_Out_ LPWSTR* pipePath)) override;
    IFACEMETHOD(MapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort, _In_ BOOL Remove)) override;
    IFACEMETHOD(Unmount(_In_ const char* Path)) override;
    IFACEMETHOD(MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)) override;
    IFACEMETHOD(UnmountWindowsFolder(_In_ LPCSTR LinuxPath)) override;
    void MountGpuLibraries(_In_ LPCSTR LibrariesMountPoint, _In_ LPCSTR DriversMountpoint, _In_ DWORD Flags);

    void OnProcessReleased(int Pid);
    void RegisterCallback(_In_ ITerminationCallback* callback);

    Microsoft::WRL::ComPtr<WSLAProcess> CreateLinuxProcess(
        _In_ const WSLA_PROCESS_OPTIONS& Options, int* Errno = nullptr, const TPrepareCommandLine& PrepareCommandLine = [](const auto&) {});

    std::pair<ULONG, std::string> AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly);
    void DetachDisk(_In_ ULONG Lun);

    // VirtioNetworking support
    GUID HandleVirtioAddGuestDevice(_In_ const GUID& Clsid, _In_ const GUID& DeviceId, _In_ PCWSTR Tag, _In_ PCWSTR Options);
    int HandleVirtioModifyOpenPorts(_In_ const GUID& Clsid, _In_ PCWSTR Tag, _In_ const SOCKADDR_INET& Addr, _In_ int Protocol, _In_ bool IsOpen);

private:
    static void Mount(wsl::shared::SocketChannel& Channel, LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags);
    static void CALLBACK s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context);
    static bool ParseTtyInformation(
        const WSLA_PROCESS_FD* Fds, ULONG FdCount, const WSLA_PROCESS_FD** TtyInput, const WSLA_PROCESS_FD** TtyOutput, const WSLA_PROCESS_FD** TtyControl);

    void ConfigureNetworking();
    void ConfigureMounts();
    void OnExit(_In_ const HCS_EVENT* Event);
    void OnCrash(_In_ const HCS_EVENT* Event);

    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(enum WSLA_FORK::ForkType Type);
    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(wsl::shared::SocketChannel& Channel, enum WSLA_FORK::ForkType Type);
    int32_t ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel);

    ConnectedSocket ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd);
    static void OpenLinuxFile(wsl::shared::SocketChannel& Channel, const char* Path, uint32_t Flags, int32_t Fd);
    void LaunchPortRelay();

    std::filesystem::path GetCrashDumpFolder();
    void CreateVmSavedStateFile();
    void EnforceVmSavedStateFileLimit();
    void WriteCrashLog(const std::wstring& crashLog);
    void CollectCrashDumps(wil::unique_socket&& listenSocket) const;

    Microsoft::WRL::ComPtr<WSLAProcess> CreateLinuxProcessImpl(
        _In_ const WSLA_PROCESS_OPTIONS& Options, int* Errno = nullptr, const TPrepareCommandLine& PrepareCommandLine = [](const auto&) {});

    HRESULT MountWindowsFolderImpl(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly, _In_ WSLAMountFlags Flags);

    void WatchForExitedProcesses(wsl::shared::SocketChannel& Channel);

    struct AttachedDisk
    {
        std::filesystem::path Path;
        std::string Device;
        bool AccessGranted = false;
    };

    VIRTUAL_MACHINE_SETTINGS m_settings;
    std::thread m_processExitThread;
    std::thread m_crashDumpCollectionThread;

    GUID m_vmId{};
    std::wstring m_vmIdString;
    wsl::windows::common::helpers::WindowsVersion m_windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
    int m_coldDiscardShiftSize{};
    bool m_running = false;
    PSID m_userSid{};
    wil::unique_handle m_userToken;
    std::wstring m_debugShellPipe;

    std::mutex m_trackedProcessesLock;
    std::vector<WSLAProcess*> m_trackedProcesses;

    wsl::windows::common::hcs::unique_hcs_system m_computeSystem;

    std::filesystem::path m_vmSavedStateFile;
    std::filesystem::path m_crashDumpFolder;
    bool m_vmSavedStateCaptured = false;
    bool m_crashLogCaptured = false;

    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_vmTerminatingEvent{wil::EventOptions::ManualReset};
    wil::com_ptr<ITerminationCallback> m_terminationCallback;
    std::unique_ptr<wsl::core::INetworkingEngine> m_networkEngine;

    wsl::shared::SocketChannel m_initChannel;
    wil::unique_handle m_portRelayChannelRead;
    wil::unique_handle m_portRelayChannelWrite;

    std::map<ULONG, AttachedDisk> m_attachedDisks;
    std::map<std::string, std::wstring> m_plan9Mounts;
    std::recursive_mutex m_lock;
    std::mutex m_portRelaylock;

    // VirtioNetworking support
    wil::srwlock m_guestDeviceLock;
    Microsoft::WRL::ComPtr<DeviceHostProxy> m_deviceHostSupport;
};
} // namespace wsl::windows::service::wsla