/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWVirtualMachine.h

Abstract:

    TODO

--*/
#pragma once
#include "wslservice.h"
#include "INetworkingEngine.h"
#include "hcs.hpp"
#include "Dmesg.h"

namespace wsl::windows::service::lsw {

class LSWUserSessionImpl;

class DECLSPEC_UUID("0CFC5DC1-B6A7-45FC-8034-3FA9ED73CE30") LSWVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ILSWVirtualMachine, IFastRundown>

{
public:
    LSWVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID Sid, LSWUserSessionImpl* UserSession);
    ~LSWVirtualMachine();

    void Start();
    void OnSessionTerminating();

    IFACEMETHOD(AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly, _Out_ LPSTR* Device, _Out_ ULONG* Lun)) override;
    IFACEMETHOD(Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags)) override;
    IFACEMETHOD(CreateLinuxProcess(
        _In_ const LSW_CREATE_PROCESS_OPTIONS* Options, _In_ ULONG FdCount, _In_ LSW_PROCESS_FD* Fd, _Out_ ULONG* Handles, _Out_ LSW_CREATE_PROCESS_RESULT* Result)) override;
    IFACEMETHOD(WaitPid(_In_ LONG Pid, _In_ ULONGLONG TimeoutMs, _Out_ ULONG* State, _Out_ int* Code)) override;
    IFACEMETHOD(Signal(_In_ LONG Pid, _In_ int Signal)) override;
    IFACEMETHOD(Shutdown(ULONGLONG _In_ TimeoutMs)) override;
    IFACEMETHOD(RegisterCallback(_In_ ITerminationCallback* callback)) override;
    IFACEMETHOD(GetDebugShellPipe(_Out_ LPWSTR* pipePath)) override;
    IFACEMETHOD(MapPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort, _In_ BOOL Remove)) override;
    IFACEMETHOD(Unmount(_In_ const char* Path)) override;
    IFACEMETHOD(DetachDisk(_In_ ULONG Lun)) override;
    IFACEMETHOD(MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)) override;
    IFACEMETHOD(UnmountWindowsFolder(_In_ LPCSTR LinuxPath)) override;

private:
    static int32_t MountImpl(wsl::shared::SocketChannel& Channel, LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ ULONG Flags);
    static void CALLBACK s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context);
    static bool ParseTtyInformation(const LSW_PROCESS_FD* Fds, ULONG FdCount, const LSW_PROCESS_FD** TtyInput, const LSW_PROCESS_FD** TtyOutput);

    void ConfigureNetworking();
    void OnExit(_In_ const HCS_EVENT* Event);

    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(enum LSW_FORK::ForkType Type);
    std::tuple<int32_t, int32_t, wsl::shared::SocketChannel> Fork(wsl::shared::SocketChannel& Channel, enum LSW_FORK::ForkType Type);
    int32_t ExpectClosedChannelOrError(wsl::shared::SocketChannel& Channel);

    wil::unique_socket ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd);
    void OpenLinuxFile(wsl::shared::SocketChannel& Channel, const char* Path, uint32_t Flags, int32_t Fd);
    void LaunchPortRelay();

    std::vector<wil::unique_socket> CreateLinuxProcessImpl(
        _In_ const LSW_CREATE_PROCESS_OPTIONS* Options, _In_ ULONG FdCount, _In_ LSW_PROCESS_FD* Fd, _Out_ LSW_CREATE_PROCESS_RESULT* Result);

    struct AttachedDisk
    {
        std::filesystem::path Path;
        std::string Device;
    };

    VIRTUAL_MACHINE_SETTINGS m_settings;
    GUID m_vmId{};
    std::wstring m_vmIdString;
    wsl::windows::common::helpers::WindowsVersion m_windowsVersion = wsl::windows::common::helpers::GetWindowsVersion();
    int m_coldDiscardShiftSize{};
    bool m_running = false;
    PSID m_userSid{};
    std::wstring m_debugShellPipe;

    wsl::windows::common::hcs::unique_hcs_system m_computeSystem;
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
    LSWUserSessionImpl* m_userSession;
};
} // namespace wsl::windows::service::lsw