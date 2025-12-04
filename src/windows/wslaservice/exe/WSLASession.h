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

namespace wsl::windows::service::wsla {

class DECLSPEC_UUID("4877FEFC-4977-4929-A958-9F36AA1892A4") WSLASession
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IWSLASession, IFastRundown>
{
public:
    WSLASession(const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl);
    ~WSLASession();

    IFACEMETHOD(GetDisplayName)(LPWSTR* DisplayName) override;
    const std::wstring& DisplayName() const;

    // Image management.
    IFACEMETHOD(PullImage)(_In_ LPCWSTR Image, _In_ const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryInformation, _In_ IProgressCallback* ProgressCallback) override;
    IFACEMETHOD(ImportImage)(_In_ ULONG Handle, _In_ LPCWSTR Image, _In_ IProgressCallback* ProgressCallback) override;
    IFACEMETHOD(ListImages)(_Out_ WSLA_IMAGE_INFORMATION** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(DeleteImage)(_In_ LPCWSTR Image) override;

    // Container management.
    IFACEMETHOD(CreateContainer)(_In_ const WSLA_CONTAINER_OPTIONS* Options, _Out_ IWSLAContainer** Container) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Name, _In_ IWSLAContainer** Container) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLA_CONTAINER** Images, _Out_ ULONG* Count) override;

    // VM management.
    IFACEMETHOD(GetVirtualMachine)(IWSLAVirtualMachine** VirtualMachine) override;
    IFACEMETHOD(CreateRootNamespaceProcess)(_In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** VirtualMachine, _Out_ int* Errno) override;

    // Disk management.
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;

    IFACEMETHOD(Shutdown(_In_ ULONG)) override;

    void OnUserSessionTerminating();

private:
    static WSLAVirtualMachine::Settings CreateVmSettings(const WSLA_SESSION_SETTINGS& Settings);

    void ConfigureStorage(const WSLA_SESSION_SETTINGS& Settings);
    void Ext4Format(const std::string& Device);
    void ClearDeletedContainers();

    WSLA_SESSION_SETTINGS m_sessionSettings; // TODO: Revisit to see if we should have session settings as a member or not
    WSLAUserSessionImpl* m_userSession = nullptr;
    Microsoft::WRL::ComPtr<WSLAVirtualMachine> m_virtualMachine;
    std::wstring m_displayName;
    std::filesystem::path m_storageVhdPath;
    std::map<std::string, Microsoft::WRL::ComPtr<WSLAContainer>> m_containers;
    std::recursive_mutex m_lock;
};

} // namespace wsl::windows::service::wsla