/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCVirtualMachineHost.h

Abstract:

    Implementation of IWSLCVirtualMachine - the service-side WSLC VM host.
    This class owns WSLC policy and an IVirtualMachineBackend.

--*/

#pragma once

#include "wslc.h"
#include "IVirtualMachineBackend.h"
#include "Dmesg.h"
#include "INetworkingEngine.h"
#include "RuntimeClass.h"
#include "WslCoreConfig.h"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace wsl::windows::service::wslc {

using WSLCVirtualMachineBackendFactory = std::function<std::unique_ptr<IVirtualMachineBackend>(const VmCreateRequest&)>;

class WSLCVirtualMachineHost
    : public common::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCVirtualMachine, IFastRundown>
{
public:
    WSLCVirtualMachineHost(_In_ const WSLCSessionSettings* Settings, WSLCVirtualMachineBackendFactory BackendFactory);
    ~WSLCVirtualMachineHost();

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
    bool FeatureEnabled(WSLCFeatureFlags Value) const;

    std::recursive_mutex m_lock;

    std::unique_ptr<IVirtualMachineBackend> m_backend;
    std::filesystem::path m_bootFilesPath;
    ULONG m_bootTimeoutMs{};

    WSLCFeatureFlags m_featureFlags{};
    WSLCNetworkingMode m_networkingMode{};
    std::string m_hostLoopback;

    bool m_swiotlbConfigured = false;

    VmGuestListener m_guestListener;
    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
    std::optional<wsl::core::Config> m_natConfig;
    std::unique_ptr<wsl::core::INetworkingEngine> m_networkingEngine;
};

//
// WSLCVirtualMachineFactory - Implements IWSLCVirtualMachineFactory.
//
// Owns a deep copy of the WSLCSessionSettings needed to construct a VM and creates a
// fresh WSLCVirtualMachineHost on demand. This lets the per-user session recreate a VM that
// was idle-terminated, without the SYSTEM service holding a VM up front.
//
class WSLCVirtualMachineFactory
    : public common::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLCVirtualMachineFactory, IFastRundown>
{
public:
    WSLCVirtualMachineFactory(_In_ const WSLCSessionSettings* Settings, WSLCVirtualMachineBackendFactory BackendFactory);

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
    std::string m_hostLoopback;
    WSLCSessionStorageFlags m_storageFlags{};
    WSLCVirtualMachineBackendFactory m_backendFactory;
};

} // namespace wsl::windows::service::wslc
