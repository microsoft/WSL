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
#include "ContainerEventTracker.h"
#include "DockerHTTPClient.h"
#include "IORelay.h"

namespace wsl::windows::service::wsla {

class WSLASession;

//
// WSLASession - Implements IWSLASession for container management.
// Runs in a per-user COM server process for security isolation.
// The SYSTEM service creates the VM and passes IWSLAVirtualMachine to Initialize().
//
class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLASession, IFastRundown>
{
public:
    WSLASession() = default;

    ~WSLASession();

    // Sets a callback invoked when this object is destroyed.
    // Used by the COM server host to signal process exit.
    void SetDestructionCallback(std::function<void()>&& callback);

    // IWSLASession - initialization methods
    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;
    IFACEMETHOD(Initialize)(_In_ const WSLA_SESSION_INIT_SETTINGS* Settings, _In_ IWSLAVirtualMachine* Vm) override;

    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
    IFACEMETHOD(GetState)(_Out_ WSLASessionState* State) override;

    // Image management.
    IFACEMETHOD(PullImage)(
        _In_ LPCSTR ImageUri,
        _In_ const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation,
        _In_ IProgressCallback* ProgressCallback,
        _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;
    IFACEMETHOD(LoadImage)(_In_ ULONG ImageHandle, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(ImportImage)(_In_ ULONG ImageHandle, _In_ LPCSTR ImageName, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(SaveImage)(_In_ ULONG OutputHandle, _In_ LPCSTR ImageNameOrID, _In_ IProgressCallback* ProgressCallback, _Inout_opt_ WSLA_ERROR_INFO* Error) override;
    IFACEMETHOD(ListImages)(_In_opt_ const WSLA_LIST_IMAGES_OPTIONS* Options, _Out_ WSLA_IMAGE_INFORMATION** Images, _Out_ ULONG* Count, _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;
    IFACEMETHOD(DeleteImage)(
        _In_ const WSLA_DELETE_IMAGE_OPTIONS* Options,
        _Out_ WSLA_DELETED_IMAGE_INFORMATION** DeletedImages,
        _Out_ ULONG* Count,
        _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;

    // Container management.
    IFACEMETHOD(CreateContainer)(_In_ const WSLA_CONTAINER_OPTIONS* Options, _Out_ IWSLAContainer** Container, _Inout_opt_ WSLA_ERROR_INFO* Error) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Id, _In_ IWSLAContainer** Container) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLA_CONTAINER** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(ExportContainer)(_In_ ULONG OutputHandle, _In_ LPCSTR ContainerID, _In_ IProgressCallback* ProgressCallback, _Inout_opt_ WSLA_ERROR_INFO* Error) override;

    // VM management.
    IFACEMETHOD(CreateRootNamespaceProcess)(
        _In_ LPCSTR Executable, _In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** VirtualMachine, _Out_ int* Errno) override;

    // Disk management.
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;

    IFACEMETHOD(Terminate()) override;

    // Testing.
    IFACEMETHOD(MountWindowsFolder)(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly) override;
    IFACEMETHOD(UnmountWindowsFolder)(_In_ LPCSTR LinuxPath) override;
    IFACEMETHOD(MapVmPort)(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort) override;
    IFACEMETHOD(UnmapVmPort)(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort) override;

private:
    ULONG m_id = 0;

    void ConfigureStorage(const WSLA_SESSION_INIT_SETTINGS& Settings, PSID UserSid);
    void Ext4Format(const std::string& Device);
    void OnContainerDeleted(const WSLAContainerImpl* Container);
    void OnDockerdLog(const gsl::span<char>& Data);
    void OnDockerdExited();
    void StartDockerd();
    void ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, ULONG InputHandle);
    void RecoverExistingContainers();

    void SaveImageImpl(std::pair<uint32_t, wil::unique_socket>& RequestCodePair, ULONG OutputHandle, WSLA_ERROR_INFO* Error);
    void ExportContainerImpl(std::pair<uint32_t, wil::unique_socket>& RequestCodePair, ULONG OutputHandle, WSLA_ERROR_INFO* Error);

    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLAVirtualMachine> m_virtualMachine;
    std::optional<ContainerEventTracker> m_eventTracker;
    wil::unique_event m_containerdReadyEvent{wil::EventOptions::ManualReset};
    std::wstring m_displayName;
    std::filesystem::path m_storageVhdPath;
    std::vector<std::unique_ptr<WSLAContainerImpl>> m_containers;
    wil::unique_event m_sessionTerminatingEvent{wil::EventOptions::ManualReset};
    std::recursive_mutex m_lock;
    IORelay m_ioRelay;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    WSLAFeatureFlags m_featureFlags{};
    std::function<void()> m_destructionCallback;
};

} // namespace wsl::windows::service::wsla
