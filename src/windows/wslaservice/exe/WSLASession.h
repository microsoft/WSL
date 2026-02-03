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

// This private interface is used to get a WSLASession pointer from its weak reference. It's only used within the service.
class DECLSPEC_UUID("4559499B-4F07-4BD4-B098-9F4A432E9456") IWSLASessionImpl : public IInspectable
{
public:
    // N.B. The caller must maintain a reference to the COM object for the return pointer to be used safely.
    IFACEMETHOD(GetImplNoRef)(_Out_ WSLASession** Session) = 0;
};

class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLASession, IWSLASessionImpl, IFastRundown>
{
public:
    WSLASession(ULONG id, const WSLA_SESSION_SETTINGS& Settings, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo, bool Elevated);

    ~WSLASession();

    ULONG GetId() const noexcept;

    PSID GetSid() const noexcept;

    wil::unique_hlocal_string GetSidString() const;

    DWORD GetCreatorPid() const noexcept;

    bool IsTokenElevated() const noexcept;

    const std::wstring& DisplayName() const;

    void CopyDisplayName(_Out_writes_z_(bufferLength) PWSTR buffer, size_t bufferLength) const;

    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;

    // Image management.
    IFACEMETHOD(PullImage)(
        _In_ LPCSTR ImageUri,
        _In_ const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation,
        _In_ IProgressCallback* ProgressCallback,
        _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;
    IFACEMETHOD(LoadImage)(_In_ ULONG ImageHandle, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(ImportImage)(_In_ ULONG ImageHandle, _In_ LPCSTR ImageName, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength) override;
    IFACEMETHOD(ListImages)(_Out_ WSLA_IMAGE_INFORMATION** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(DeleteImage)(
        _In_ const WSLA_DELETE_IMAGE_OPTIONS* Options,
        _Out_ WSLA_DELETED_IMAGE_INFORMATION** DeletedImages,
        _Out_ ULONG* Count,
        _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;

    // Container management.
    IFACEMETHOD(CreateContainer)(_In_ const WSLA_CONTAINER_OPTIONS* Options, _Out_ IWSLAContainer** Container, _Inout_opt_ WSLA_ERROR_INFO* Error) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Id, _In_ IWSLAContainer** Container) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLA_CONTAINER** Images, _Out_ ULONG* Count) override;

    // VM management.
    IFACEMETHOD(CreateRootNamespaceProcess)(
        _In_ LPCSTR Executable, _In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** VirtualMachine, _Out_ int* Errno) override;

    // Disk management.
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;

    IFACEMETHOD(Terminate()) override;

    IFACEMETHOD(GetImplNoRef)(_Out_ WSLASession** Session) override;

    // Testing.
    IFACEMETHOD(MountWindowsFolder)(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly) override;
    IFACEMETHOD(UnmountWindowsFolder)(_In_ LPCSTR LinuxPath) override;
    IFACEMETHOD(MapVmPort)(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort) override;
    IFACEMETHOD(UnmapVmPort)(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort) override;

    void OnUserSessionTerminating();

    bool Terminated();

private:
    ULONG m_id = 0;

    static WSLAVirtualMachine::Settings CreateVmSettings(const WSLA_SESSION_SETTINGS& Settings);

    void ConfigureStorage(const WSLA_SESSION_SETTINGS& Settings, PSID UserSid);
    void Ext4Format(const std::string& Device);
    void OnContainerDeleted(const WSLAContainerImpl* Container);
    void OnDockerdLog(const gsl::span<char>& Data);
    void OnDockerdExited();
    void StartDockerd();
    void ImportImageImpl(DockerHTTPClient::HTTPRequestContext& Request, ULONG InputHandle);
    void RecoverExistingContainers();

    std::optional<DockerHTTPClient> m_dockerClient;
    std::optional<WSLAVirtualMachine> m_virtualMachine;
    std::optional<ContainerEventTracker> m_eventTracker;
    wil::unique_event m_containerdReadyEvent{wil::EventOptions::ManualReset};
    std::wstring m_displayName;
    std::filesystem::path m_storageVhdPath;
    std::vector<std::unique_ptr<WSLAContainerImpl>> m_containers;
    wil::unique_event m_sessionTerminatingEvent{wil::EventOptions::ManualReset};
    wil::unique_tokeninfo_ptr<TOKEN_USER> m_tokenInfo;
    bool m_elevatedToken{};
    DWORD m_creatorPid{};
    std::recursive_mutex m_lock;
    IORelay m_ioRelay;
    std::optional<ServiceRunningProcess> m_dockerdProcess;
    WSLAFeatureFlags m_featureFlags{};
};

} // namespace wsl::windows::service::wsla
