// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    OpenVmmVirtualMachine.h

Abstract:

    Implementation of IWSLCVirtualMachine using OpenVMM as the VMM backend.

    This class spawns openvmm.exe as a child process, configures it via CLI
    arguments and (in future) gRPC, and implements the same IWSLCVirtualMachine
    interface as HcsVirtualMachine to allow the rest of WSLC to work unchanged.

--*/

#pragma once

#include "wslc.h"
#include "INetworkingEngine.h"
#include "TtrpcClient.h"
#include "Dmesg.h"
#include <filesystem>
#include <map>
#include <bitset>

#define MAX_VHD_COUNT 254

namespace wsl::windows::service::wslc {

class OpenVmmVirtualMachine
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCVirtualMachine, IFastRundown>
{
public:
    OpenVmmVirtualMachine(_In_ const WSLCSessionSettings* Settings);
    ~OpenVmmVirtualMachine();

    // IWSLCVirtualMachine implementation
    IFACEMETHOD(GetId)(_Out_ GUID* VmId) override;
    IFACEMETHOD(AcceptConnection)(_Out_ HANDLE* Socket) override;
    IFACEMETHOD(ConfigureNetworking)(_In_ HANDLE GnsSocket, _In_opt_ HANDLE* DnsSocket) override;
    IFACEMETHOD(AttachDisk)(_In_ LPCWSTR Path, _In_ BOOL ReadOnly, _Out_ ULONG* Lun) override;
    IFACEMETHOD(DetachDisk)(_In_ ULONG Lun) override;
    IFACEMETHOD(AddShare)(_In_ LPCWSTR WindowsPath, _In_ BOOL ReadOnly, _Out_ GUID* ShareId) override;
    IFACEMETHOD(RemoveShare)(_In_ REFGUID ShareId) override;
    IFACEMETHOD(GetTerminationEvent)(_Out_ HANDLE* Event) override;
    IFACEMETHOD(ConnectToVsockPort)(_In_ ULONG Port, _Out_ HANDLE* Socket) override;
    IFACEMETHOD(AcceptCrashDumpConnection)(_Out_ HANDLE* Socket) override;

private:
    struct DiskInfo
    {
        std::wstring Path;
        bool ReadOnly = false;
    };

    bool FeatureEnabled(WSLCFeatureFlags Value) const;

    // Build the openvmm.exe command line (ttrpc-only in orchestration mode).
    std::wstring BuildCommandLine() const;

    // Build a ttrpc CreateVM configuration from stored VM settings.
    TtrpcClient::VmConfig BuildVmConfig() const;

    // Spawn the openvmm.exe process, connect ttrpc, and create+resume the VM.
    void LaunchOpenVmm();

    // Monitor the openvmm process and signal m_vmExitEvent on exit.
    void WatchProcessExit();

    ULONG AllocateLun();
    void FreeLun(ULONG Lun);

    std::recursive_mutex m_lock;

    GUID m_vmId{};
    std::wstring m_vmIdString;

    WSLCFeatureFlags m_featureFlags{};
    WSLCNetworkingMode m_networkingMode{};
    ULONG m_bootTimeoutMs{};

    // OpenVMM process handle and management.
    wil::unique_handle m_processHandle;
    wil::unique_handle m_jobObject;
    std::thread m_processWatchThread;
    std::thread m_waitVmThread;

    // Paths for VM boot configuration.
    std::filesystem::path m_kernelPath;
    std::filesystem::path m_initrdPath;
    std::filesystem::path m_rootVhdPath;
    std::filesystem::path m_modulesVhdPath;
    std::filesystem::path m_openvmmPath;

    // Storage VHD for container data (pre-attached at boot).
    std::filesystem::path m_storageVhdPath;

    // Vsock bridge path for HvSocket emulation.
    std::filesystem::path m_vsockPath;

    // VM settings preserved from constructor for command line building.
    ULONG m_cpuCount{};
    ULONG m_memoryMb{};
    std::wstring m_kernelCmdLine;

    // Pre-created Unix domain socket listener for the init connection.
    // Must be listening BEFORE the VM boots so the guest can connect.
    SOCKET m_initListenSocket = INVALID_SOCKET;
    std::wstring m_initListenPath;

    // Pre-created Unix domain socket listener for crash dump collection.
    // Uses the hybrid_vsock bridge to receive crash dump connections from the guest.
    SOCKET m_crashDumpListenSocket = INVALID_SOCKET;
    std::wstring m_crashDumpListenPath;

    wil::unique_event m_vmExitEvent{wil::EventOptions::ManualReset};

    // Relay threads bridging AF_UNIX sockets to TCP loopback for overlapped I/O.
    std::thread m_initRelayThread;
    std::thread m_crashDumpRelayThread;
    std::mutex m_relayLock;
    std::vector<std::thread> m_relayThreads;

    std::map<ULONG, DiskInfo> m_attachedDisks;
    std::bitset<MAX_VHD_COUNT> m_lunBitmap;

    // Shares: key is ShareId, value is Windows path.
    std::map<GUID, std::wstring, wsl::windows::common::helpers::GuidLess> m_shares;

    // Networking engine (ConsommeNetworking for the OpenVMM backend).
    std::unique_ptr<wsl::core::INetworkingEngine> m_networkEngine;

    // ttrpc client for runtime VM management (disk hot-add/remove etc.).
    std::filesystem::path m_ttrpcSocketPath;
    std::unique_ptr<TtrpcClient> m_ttrpcClient;

    // Dmesg collector for early boot and virtio serial console output.
    std::shared_ptr<DmesgCollector> m_dmesgCollector;
};

} // namespace wsl::windows::service::wslc
