/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSession.h

Abstract:

    TODO

--*/

#pragma once

#include "wslc.h"
#include "WSLCVirtualMachine.h"
#include "WSLCContainer.h"
#include "WSLCVhdVolume.h"
#include "WSLCVolumeMetadata.h"
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "IORelay.h"
#include <unordered_map>

namespace wsl::windows::service::wslc {

class WSLCSession;

class UserHandle
{
    NON_COPYABLE(UserHandle);

public:
    UserHandle(WSLCSession& Session, HANDLE handle);
    UserHandle(UserHandle&& Other);

    ~UserHandle();

    UserHandle& operator=(UserHandle&& Other);

    HANDLE Get() const noexcept;
    void Reset();

private:
    WSLCSession* m_session{};
    HANDLE m_handle{};
};

class UserCOMCallback
{
    NON_COPYABLE(UserCOMCallback);

public:
    UserCOMCallback(WSLCSession& Session) noexcept;
    UserCOMCallback(UserCOMCallback&& Other) noexcept;

    ~UserCOMCallback() noexcept;

    UserCOMCallback& operator=(UserCOMCallback&& Other) noexcept;
    void Reset() noexcept;

private:
    WSLCSession* m_session{};
    DWORD m_threadId{};
};

//
// WSLCSession - Implements IWSLCSession for container management.
// Runs in a per-user COM server process for security isolation.
// The SYSTEM service creates the VM and passes IWSLCVirtualMachine to Initialize().
//
class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLCSession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLCSession, IFastRundown, ISupportErrorInfo>
{
public:
    WSLCSession() = default;

    ~WSLCSession();

    // Sets a callback invoked when this object is destroyed.
    // Used by the COM server host to signal process exit.
    void SetDestructionCallback(std::function<void()>&& callback);

    // IWSLCSession - initialization methods
    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;
    IFACEMETHOD(Initialize)(_In_ const WSLCSessionInitSettings* Settings, _In_ IWSLCVirtualMachine* Vm) override;

    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
    IFACEMETHOD(GetState)(_Out_ WSLCSessionState* State) override;

    // Image management.
    IFACEMETHOD(PullImage)(_In_ LPCSTR Image, _In_opt_ LPCSTR RegistryAuthenticationInformation, _In_opt_ IProgressCallback* ProgressCallback) override;
    IFACEMETHOD(BuildImage)(_In_ const WSLCBuildImageOptions* Options, _In_opt_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(LoadImage)(_In_ const WSLCHandle ImageHandle, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(ImportImage)(_In_ const WSLCHandle ImageHandle, _In_ LPCSTR ImageName, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(SaveImage)(_In_ WSLCHandle OutputHandle, _In_ LPCSTR ImageNameOrID, _In_ IProgressCallback* ProgressCallback, _In_opt_ HANDLE CancelEvent) override;
    IFACEMETHOD(ListImages)(_In_opt_ const WSLCListImageOptions* Options, _Out_ WSLCImageInformation** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(DeleteImage)(_In_ const WSLCDeleteImageOptions* Options, _Out_ WSLCDeletedImageInformation** DeletedImages, _Out_ ULONG* Count) override;
    IFACEMETHOD(TagImage)(_In_ const WSLCTagImageOptions* Options) override;
    IFACEMETHOD(PushImage)(_In_ LPCSTR Image, _In_ LPCSTR RegistryAuthenticationInformation, _In_opt_ IProgressCallback* ProgressCallback) override;
    IFACEMETHOD(InspectImage)(_In_ LPCSTR ImageNameOrId, _Out_ LPSTR* Output) override;
    IFACEMETHOD(Authenticate)(_In_ LPCSTR ServerAddress, _In_ LPCSTR Username, _In_ LPCSTR Password, _Out_ LPSTR* IdentityToken) override;
    IFACEMETHOD(PruneImages)(
        _In_opt_ const WSLCPruneImagesOptions* Options,
        _Out_ WSLCDeletedImageInformation** DeletedImages,
        _Out_ ULONG* DeletedImagesCount,
        _Out_ ULONGLONG* SpaceReclaimed) override;

    // Container management.
    IFACEMETHOD(CreateContainer)(_In_ const WSLCContainerOptions* Options, _Out_ IWSLCContainer** Container) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Id, _In_ IWSLCContainer** Container) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLCContainerEntry** Containers, _Out_ ULONG* Count, _Out_ WSLCContainerPortMapping** Ports, _Out_ ULONG* PortsCount) override;
    IFACEMETHOD(PruneContainers)(_In_opt_ WSLCPruneLabelFilter* Filters, _In_ DWORD FiltersCount, _In_ ULONGLONG Until, _Out_ WSLCPruneContainersResults* Result) override;

    // VM management.
    IFACEMETHOD(CreateRootNamespaceProcess)(
        _In_ LPCSTR Executable, _In_ const WSLCProcessOptions* Options, _Out_ IWSLCProcess** VirtualMachine, _Out_ int* Errno) override;

    // Disk management.
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;

    // Volume management.
    IFACEMETHOD(CreateVolume)(_In_ const WSLCVolumeOptions* Options, _Out_ WSLCVolumeInformation* VolumeInfo) override;
    IFACEMETHOD(DeleteVolume)(_In_ LPCSTR Name) override;
    IFACEMETHOD(ListVolumes)(_Out_ WSLCVolumeInformation** Volumes, _Out_ ULONG* Count) override;
    IFACEMETHOD(InspectVolume)(_In_ LPCSTR Name, _Out_ LPSTR* Output) override;
    IFACEMETHOD(PruneVolumes)(_In_opt_ const WSLCPruneVolumesOptions* Options, _Out_ WSLCPruneVolumesResults* Results) override;

    IFACEMETHOD(Terminate()) override;

    // ISupportErrorInfo
    IFACEMETHOD(InterfaceSupportsErrorInfo)(_In_ REFIID riid) override;

    // Testing.
    IFACEMETHOD(MountWindowsFolder)(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly) override;
    IFACEMETHOD(UnmountWindowsFolder)(_In_ LPCSTR LinuxPath) override;
    IFACEMETHOD(MapVmPort)(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort) override;
    IFACEMETHOD(UnmapVmPort)(_In_ int Family, _In_ unsigned short WindowsPort, _In_ unsigned short LinuxPort) override;

    common::relay::MultiHandleWait CreateIOContext(HANDLE CancelHandle = nullptr);

    UserHandle OpenUserHandle(WSLCHandle Handle);
    void ReleaseUserHandle(HANDLE Handle);

    UserCOMCallback RegisterUserCOMCallback();
    void UnregisterUserCOMCallback(DWORD ThreadId);

private:
    ULONG m_id = 0;

    __requires_lock_held(m_userHandlesLock) void CancelUserHandleIO();
    __requires_lock_held(m_userCOMCallbacksLock) void CancelUserCOMCallbacks();
    void ConfigureStorage(const WSLCSessionInitSettings& Settings, PSID UserSid);
    void Ext4Format(const std::string& Device);
    void OnContainerDeleted(const WSLCContainerImpl* Container);
    void OnContainerdLog(const gsl::span<char>& Data);
    void OnContainerdExited();
    void OnDockerdLog(const gsl::span<char>& Data);
    void OnDockerdExited();
    void StartContainerd();
    void StartDockerd();
    int StopProcess(std::optional<ServiceRunningProcess>& Process, DWORD TerminateTimeoutMs, DWORD KillTimeoutMs);
    void ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, const WSLCHandle ImageHandle);
    void RecoverExistingContainers();
    void RecoverExistingVolumes();

    void SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& RequestCodePair, WSLCHandle OutputHandle, HANDLE CancelEvent);
    void StreamImageOperation(DockerHTTPClient::HTTPRequestContext& requestContext, LPCSTR Image, LPCSTR OperationName, IProgressCallback* ProgressCallback);

    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLCVirtualMachine> m_virtualMachine;
    std::optional<ContainerEventTracker> m_eventTracker;
    wil::unique_event m_containerdReadyEvent{wil::EventOptions::ManualReset};
    std::wstring m_displayName;
    std::filesystem::path m_storageVhdPath;

    // N.B. m_lock must be acquired before acquiring m_volumesLock or m_containersLock.
    // These locks protect m_volumes / m_containers without requiring an exclusive m_lock.
    // This allows independent operations to proceed while volume/container bookkeeping remains synchronized.
    std::mutex m_containersLock;
    std::mutex m_volumesLock;
    std::vector<std::unique_ptr<WSLCContainerImpl>> m_containers;
    std::unordered_map<std::string, std::unique_ptr<WSLCVhdVolumeImpl>> m_volumes;
    std::unordered_set<std::string> m_anonymousVolumes; // TODO: Implement proper anonymous volume support.
    wil::unique_event m_sessionTerminatingEvent{wil::EventOptions::ManualReset};
    wil::srwlock m_lock;
    IORelay m_ioRelay;
    std::optional<ServiceRunningProcess> m_containerdProcess;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    WSLCFeatureFlags m_featureFlags{};
    std::function<void()> m_destructionCallback;
    std::atomic<bool> m_terminated{false};

    // User-provided handles that the session is currently doing IO on.
    std::mutex m_userHandlesLock;
    __guarded_by(m_userHandlesLock) std::vector<HANDLE> m_userHandles;

    // Threads currently inside an outgoing COM callback (e.g. IProgressCallback::OnProgress).
    std::recursive_mutex m_userCOMCallbacksLock;
    __guarded_by(m_userCOMCallbacksLock) std::set<DWORD> m_userCOMCallbackThreads;

    // Used for testing only.
    std::mutex m_allocatedPortsLock;
    __guarded_by(m_allocatedPortsLock) std::map<uint16_t, std::pair<std::shared_ptr<VmPortAllocation>, size_t>> m_allocatedPorts;
};

} // namespace wsl::windows::service::wslc
