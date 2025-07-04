#pragma once
#include "wslservice.h"
#include "hcs.hpp"
#include "Dmesg.h"

namespace wsl::windows::service::lsw {
class DECLSPEC_UUID("0CFC5DC1-B6A7-45FC-8034-3FA9ED73CE30") LSWVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ILSWVirtualMachine, IFastRundown>

{
public:
    LSWVirtualMachine(const VIRTUAL_MACHINE_SETTINGS& Settings, PSID Sid);

    void Start();

    IFACEMETHOD(GetState()) override;
    IFACEMETHOD(AttachDisk(_In_ PCWSTR Path, _In_ BOOL ReadOnly, _Out_ LPSTR* Device)) override;
    IFACEMETHOD(Mount(_In_ LPCSTR Source, _In_ LPCSTR Target, _In_ LPCSTR Type, _In_ LPCSTR Options, _In_ BOOL Chroot)) override;
    IFACEMETHOD(CreateLinuxProcess(_In_ const LSW_CREATE_PROCESS_OPTIONS* Options, _In_ ULONG FdCount, _In_ LSW_PROCESS_FD* Fd, _Out_ HANDLE* Handles, _Out_ LSW_CREATE_PROCESS_RESULT* Result)) override;


private:
    static void CALLBACK s_OnExit(_In_ HCS_EVENT* Event, _In_opt_ void* Context);

    std::pair<int32_t, wsl::shared::SocketChannel> Fork();
    wil::unique_socket ConnectSocket(wsl::shared::SocketChannel& Channel, int32_t Fd);

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
    PSID m_userSid{};

    wsl::windows::common::hcs::unique_hcs_system m_computeSystem;
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_vmTerminatingEvent{wil::EventOptions::ManualReset};

    wsl::shared::SocketChannel m_initChannel;

    std::map<ULONG, AttachedDisk> m_attachedDisks;
    std::mutex m_lock;
};
} // namespace wsl::windows::service::lsw