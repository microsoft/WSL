/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    HcsVirtualMachine.h

Abstract:

    Implementation of IWSLCVirtualMachine - represents a single HCS-based VM instance.
    This class encapsulates a VM and all operations on it.

--*/

#pragma once

#include <atomic>
#include "wslc.h"
#include "hcs.hpp"
#include "GuestDeviceManager.h"
#include "Dmesg.h"
#include "INetworkingEngine.h"
#include "WslCoreConfig.h"
#include <filesystem>
#include <map>
#include <optional>
#include <string>

#define MAX_VHD_COUNT 254

namespace wsl::windows::service::wslc {

class HcsVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCVirtualMachine, IFastRundown>
{
public:
    HcsVirtualMachine(_In_ const WSLCSessionSettings* Settings);
    ~HcsVirtualMachine();

    // IWSLCVirtualMachine implementation
    IFACEMETHOD(GetId)(_Out_ GUID* VmId) override;
    IFACEMETHOD(AcceptConnection)(_Out_ HANDLE* Socket) override;
    IFACEMETHOD(ConfigureNetworking)(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket) override;
    IFACEMETHOD(AttachDisk)(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun) override;
    IFACEMETHOD(DetachDisk)(_In_ ULONG Lun) override;
    IFACEMETHOD(AddShare)(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId) override;
    IFACEMETHOD(RemoveShare)(_In_ REFGUID ShareId) override;
    IFACEMETHOD(ApplyGuestCapabilities)(_In_ const WSLCGuestCapabilities* Capabilities) override;
    IFACEMETHOD(GetTerminationEvent)(_Out_ HANDLE* Event) override;
    IFACEMETHOD(MapVirtioNetPort)
    (_In_ USHORT HostPort, _In_ USHORT GuestPort, _In_ int Protocol, _In_ LPCSTR ListenAddress, _Out_ USHORT* AllocatedHostPort) override;
    IFACEMETHOD(UnmapVirtioNetPort)
    (_In_ USHORT HostPort, _In_ USHORT GuestPort, _In_ int Protocol, _In_ LPCSTR ListenAddress) override;
    IFACEMETHOD(GetTerminationReason)(_Out_ WSLCVirtualMachineTerminationReason* Reason, _Out_ LPWSTR* Details) override;

private:
    struct DiskInfo
    {
        std::wstring Path;
        bool AccessGranted = false;
    };

    bool FeatureEnabled(WSLCFeatureFlags Value) const;
    static void CALLBACK OnVmExitCallback(HCS_EVENT* Event, void* Context);
    void OnExit(const HCS_EVENT* Event);
    void OnCrash(const HCS_EVENT* Event);

    std::filesystem::path GetCrashDumpFolder();
    void CreateVmSavedStateFile(HANDLE UserToken);
    void EnforceVmSavedStateFileLimit();
    void WriteCrashLog(const std::wstring& crashLog);

    ULONG AllocateLun();
    void FreeLun(ULONG Lun);

    std::recursive_mutex m_lock;

    wsl::windows::common::hcs::unique_hcs_system m_computeSystem;
    GUID m_vmId{};
    std::wstring m_vmIdString;
    ULONG m_bootTimeoutMs{};

    wil::shared_handle m_userToken;
    WSLCFeatureFlags m_featureFlags{};
    WSLCNetworkingMode m_networkingMode{};

    bool m_swiotlbConfigured = false;

    wil::unique_socket m_listenSocket;
    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    std::shared_ptr<GuestDeviceManager> m_guestDeviceManager;
    std::optional<wsl::core::Config> m_natConfig;
    std::unique_ptr<wsl::core::INetworkingEngine> m_networkEngine;

    std::map<ULONG, DiskInfo> m_attachedDisks;
    std::bitset<MAX_VHD_COUNT> m_lunBitmap;

    // Shares: key is ShareId, value is nullopt for Plan9 or DeviceInstanceId for VirtioFS
    std::map<GUID, std::optional<GUID>, wsl::windows::common::helpers::GuidLess> m_shares;

    std::filesystem::path m_vmSavedStateFile;
    std::filesystem::path m_crashDumpFolder;
    std::atomic<bool> m_vmSavedStateCaptured = false;
    std::atomic<bool> m_crashLogCaptured = false;

    // Termination reason and details, cached in OnExit before m_vmExitEvent is signaled and never
    // modified afterward. Publication relies on the event: readers (GetTerminationReason) only access
    // these after observing m_vmExitEvent signaled, so no lock is needed. Keeping them lock-free also
    // avoids contending for m_lock from the HCS exit callback, which the destructor holds while the
    // callback is drained.
    WSLCVirtualMachineTerminationReason m_terminationReason{WSLCVirtualMachineTerminationReasonUnknown};
    std::wstring m_terminationDetails;
};

//
// WSLCVirtualMachineFactory - Implements IWSLCVirtualMachineFactory.
//
// Owns a deep copy of the WSLCSessionSettings needed to construct a VM and creates a
// fresh HcsVirtualMachine on demand. This lets the per-user session recreate a VM that
// was idle-terminated, without the SYSTEM service holding a VM up front.
//
class WSLCVirtualMachineFactory
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCVirtualMachineFactory, IFastRundown>
{
public:
    explicit WSLCVirtualMachineFactory(_In_ const WSLCSessionSettings* Settings);

    IFACEMETHOD(CreateVirtualMachine)(_Out_ IWSLCVirtualMachine** Vm) override;

private:
    // Rebuilds a WSLCSessionSettings that points at this factory's owned storage.
    // The returned struct is only valid while this factory is alive.
    WSLCSessionSettings BuildSettings();

    std::wstring m_displayName;
    std::wstring m_storagePath;
    std::optional<std::wstring> m_rootVhdOverride;
    std::optional<std::string> m_rootVhdTypeOverride;

    // Duplicated dmesg sink (best-effort): only the first VM is guaranteed a live sink;
    // subsequent VMs reuse this duplicate, whose writes simply fail if the sink is gone.
    wil::unique_handle m_dmesgOutput;

    ULONGLONG m_maximumStorageSizeMb{};
    ULONG m_cpuCount{};
    ULONG m_memoryMb{};
    ULONG m_bootTimeoutMs{};
    WSLCNetworkingMode m_networkingMode{};
    WSLCFeatureFlags m_featureFlags{};
    WSLCSessionStorageFlags m_storageFlags{};
};

} // namespace wsl::windows::service::wslc
