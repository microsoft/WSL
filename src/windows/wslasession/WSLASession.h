/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.h

Abstract:

    TODO

--*/

#pragma once

#include "wslaservice.h"
#include "WSLAVirtualMachine.h"
#include "WSLAContainer.h"
#include "WSLAVhdVolume.h"
#include "WSLAVolumeMetadata.h"
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "IORelay.h"
#include <unordered_map>

namespace wsl::windows::service::wsla {

class WSLASession;

//
// WSLASession - Implements IWSLASession for container management.
// Runs in a per-user COM server process for security isolation.
// The SYSTEM service creates the VM and passes IWSLAVirtualMachine to Initialize().
//
class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLASession, IFastRundown, ISupportErrorInfo>
{
public:
    WSLASession() = default;

    ~WSLASession();

    // Sets a callback invoked when this object is destroyed.
    // Used by the COM server host to signal process exit.
    void SetDestructionCallback(std::function<void()>&& callback);

    // IWSLASession - initialization methods
    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;
    IFACEMETHOD(Initialize)(_In_ const WSLASessionInitSettings* Settings, _In_ IWSLAVirtualMachine* Vm) override;

    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
    IFACEMETHOD(GetState)(_Out_ WSLASessionState* State) override;

    // Image management.
    IFACEMETHOD(PullImage)(_In_ LPCSTR ImageUri, _In_opt_ const WslaRegistryAuthInformation* RegistryAuthenticationInformation, _In_opt_ IProgressCallback* ProgressCallback) override;
    IFACEMETHOD(BuildImage)(_In_ const WSLABuildImageOptions* Options, _In_opt_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(LoadImage)(_In_ ULONG ImageHandle, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(ImportImage)(_In_ ULONG ImageHandle, _In_ LPCSTR ImageName, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(SaveImage)(_In_ ULONG OutputHandle, _In_ LPCSTR ImageNameOrID, _In_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(ListImages)(_In_opt_ const WSLAListImageOptions* Options, _Out_ WSLAImageInformation** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(DeleteImage)(_In_ const WSLADeleteImageOptions* Options, _Out_ WSLADeletedImageInformation** DeletedImages, _Out_ ULONG* Count) override;
    IFACEMETHOD(TagImage)(_In_ const WSLATagImageOptions* Options) override;
    IFACEMETHOD(InspectImage)(_In_ LPCSTR ImageNameOrId, _Out_ LPSTR* Output) override;

    // Container management.
    IFACEMETHOD(CreateContainer)(_In_ const WSLAContainerOptions* Options, _Out_ IWSLAContainer** Container) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Id, _In_ IWSLAContainer** Container) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLAContainerEntry** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(PruneContainers)(_In_opt_ WSLAContainerPruneFilter* Filters, _In_ DWORD FiltersCount, _In_ ULONGLONG Until, _Out_ WSLAPruneContainersResults* Result) override;

    // VM management.
    IFACEMETHOD(CreateRootNamespaceProcess)(
        _In_ LPCSTR Executable, _In_ const WSLAProcessOptions* Options, _Out_ IWSLAProcess** VirtualMachine, _Out_ int* Errno) override;

    // Disk management.
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;

    // Volume management.
    IFACEMETHOD(CreateVolume)(_In_ const WSLAVolumeOptions* Options) override;
    IFACEMETHOD(DeleteVolume)(_In_ LPCSTR Name) override;

    IFACEMETHOD(Terminate()) override;

    // ISupportErrorInfo
    IFACEMETHOD(InterfaceSupportsErrorInfo)(_In_ REFIID riid) override;

    // Testing.
    IFACEMETHOD(MountWindowsFolder)(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly) override;
    IFACEMETHOD(UnmountWindowsFolder)(_In_ LPCSTR LinuxPath) override;
    IFACEMETHOD(MapVmPort)(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort) override;
    IFACEMETHOD(UnmapVmPort)(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort) override;

    common::relay::MultiHandleWait CreateIOContext(HANDLE CancelHandle = nullptr);

private:
    ULONG m_id = 0;

    void ConfigureStorage(const WSLASessionInitSettings& Settings, PSID UserSid);
    void OnContainerDeleted(const WSLAContainerImpl* Container);
    void OnDockerdLog(const gsl::span<char>& Data);
    void OnDockerdExited();
    void StartDockerd();
    void ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, ULONG InputHandle);
    void RecoverExistingContainers();
    void RecoverExistingVolumes();

    void SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& RequestCodePair, ULONG OutputHandle, HANDLE CancelEvent);

    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLAVirtualMachine> m_virtualMachine;
    std::optional<ContainerEventTracker> m_eventTracker;
    wil::unique_event m_containerdReadyEvent{wil::EventOptions::ManualReset};
    std::wstring m_displayName;
    std::filesystem::path m_storageVhdPath;

    // N.B. m_lock must be acquired before acquiring m_volumesLock or m_containersLock.
    // These locks protect m_volumes / m_containers without requiring an exclusive m_lock.
    // This allows independent operations to proceed while volume/container bookkeeping remains synchronized.
    std::mutex m_containersLock;
    std::mutex m_volumesLock;
    std::vector<std::unique_ptr<WSLAContainerImpl>> m_containers;
    std::unordered_map<std::string, std::unique_ptr<WSLAVhdVolumeImpl>> m_volumes;
    wil::unique_event m_sessionTerminatingEvent{wil::EventOptions::ManualReset};
    wil::srwlock m_lock;
    IORelay m_ioRelay;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    WSLAFeatureFlags m_featureFlags{};
    std::function<void()> m_destructionCallback;
    std::atomic<bool> m_terminated{false};

    // Used for testing only.
    std::mutex m_allocatedPortsLock;
    __guarded_by(m_allocatedPortsLock) std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>> m_allocatedPorts;
};

} // namespace wsl::windows::service::wsla
