/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachine.h

Abstract:

    Implementation of IWSLAVirtualMachine - represents a single HCS-based VM instance.
    This class encapsulates a VM and all operations on it.

--*/

#pragma once

#include "wslaservice.h"
#include "hcs.hpp"
#include "GuestDeviceManager.h"
#include "Dmesg.h"
#include "INetworkingEngine.h"
#include <filesystem>
#include <map>
#include <thread>

namespace wsl::windows::service::wsla {

class HcsVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLAVirtualMachine, IFastRundown>
{
public:
    HcsVirtualMachine(_In_ const WSLA_SESSION_SETTINGS* Settings);
    ~HcsVirtualMachine();

    // IWSLAVirtualMachine implementation
    IFACEMETHOD(GetId)(_Out_ GUID* VmId) override;
    IFACEMETHOD(AcceptConnection)(_Out_ HANDLE* Socket) override;
    IFACEMETHOD(ConfigureNetworking)(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket) override;
    IFACEMETHOD(AttachDisk)(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun) override;
    IFACEMETHOD(DetachDisk)(_In_ ULONG Lun) override;
    IFACEMETHOD(AddShare)(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId) override;
    IFACEMETHOD(RemoveShare)(_In_ REFGUID ShareId) override;

private:
    struct DiskInfo
    {
        std::wstring Path;
        std::string Device;
        bool AccessGranted = false;
    };

    bool FeatureEnabled(WSLAFeatureFlags Value) const;
    static void CALLBACK OnVmExitCallback(HCS_EVENT* Event, void* Context);
    void OnExit(const HCS_EVENT* Event);
    void OnCrash(const HCS_EVENT* Event);
    void SetExitEvent();

    std::filesystem::path GetCrashDumpFolder();
    void CreateVmSavedStateFile(HANDLE UserToken);
    void EnforceVmSavedStateFileLimit();
    void WriteCrashLog(const std::wstring& crashLog);
    void CollectCrashDumps(wil::unique_socket&& listenSocket) const;

    std::recursive_mutex m_lock;

    wsl::windows::common::hcs::unique_hcs_system m_computeSystem;
    GUID m_vmId{};
    std::wstring m_vmIdString;
    ULONG m_bootTimeoutMs{};

    wil::shared_handle m_userToken;
    GUID m_virtioFsClassId{};

    WSLAFeatureFlags m_featureFlags{};
    WSLANetworkingMode m_networkingMode{};

    wil::unique_socket m_listenSocket;
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
    std::unique_ptr<wsl::core::INetworkingEngine> m_networkEngine;

    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};

    std::map<ULONG, DiskInfo> m_attachedDisks;
    ULONG m_nextLun = 0;

    // Shares: key is ShareId, value is nullopt for Plan9 or DeviceInstanceId for VirtioFS
    std::map<GUID, std::optional<GUID>, wsl::windows::common::helpers::GuidLess> m_shares;

    std::thread m_crashDumpThread;
    std::filesystem::path m_vmSavedStateFile;
    std::filesystem::path m_crashDumpFolder;
    bool m_vmSavedStateCaptured = false;
    bool m_crashLogCaptured = false;

    wil::com_ptr<ITerminationCallback> m_terminationCallback;
};

} // namespace wsl::windows::service::wsla
