/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionProxy.h

Abstract:

    Session proxy that wraps IWSLASession and enables weak reference tracking
    for non-persistent sessions in the SYSTEM service.

    This proxy:
    - Implements IWeakReferenceSource for local weak reference support
    - Forwards all IWSLASession calls to the actual session in the per-user process
    - Enables the session manager to track session lifetime without holding strong refs

--*/

#pragma once
#include "wslaservice.h"
#include <wrl/implements.h>
#include <wil/token_helpers.h>
#include <string>
#include <atomic>

namespace wsl::windows::service::wsla {

class WSLASessionManagerImpl;

struct CallingProcessTokenInfo
{
    wil::unique_tokeninfo_ptr<TOKEN_USER> Info;
    bool Elevated;
};

class WSLASessionProxy
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>, IWSLASession, IFastRundown, Microsoft::WRL::FtmBase>
{
public:
    NON_COPYABLE(WSLASessionProxy);
    NON_MOVABLE(WSLASessionProxy);

    WSLASessionProxy(_In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ std::wstring DisplayName, _In_ CallingProcessTokenInfo TokenInfo, _In_ wil::com_ptr<IWSLASession> Session);

    ~WSLASessionProxy();

    // Accessors for session metadata
    ULONG GetId() const
    {
        return m_sessionId;
    }

    DWORD GetCreatorPid() const
    {
        return m_creatorPid;
    }

    const std::wstring& DisplayName() const
    {
        return m_displayName;
    }

    void CopyDisplayName(_Out_writes_z_(bufferLength) PWSTR buffer, size_t bufferLength) const;

    PSID GetSid() const
    {
        return m_tokenInfo.Info->User.Sid;
    }

    bool IsTokenElevated() const
    {
        return m_tokenInfo.Elevated;
    }

    bool IsTerminated() const
    {
        return m_terminated;
    }

    // IWSLASession
    IFACEMETHOD(GetProcessHandle)(_Out_ HANDLE* ProcessHandle) override;
    IFACEMETHOD(Initialize)(_In_ const WSLA_SESSION_INIT_SETTINGS*, _In_ IWSLAVirtualMachine*) override;
    IFACEMETHOD(GetId)(_Out_ ULONG* Id) override;
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
    IFACEMETHOD(CreateContainer)(_In_ const WSLA_CONTAINER_OPTIONS* Options, _Out_ IWSLAContainer** Container, _Inout_opt_ WSLA_ERROR_INFO* Error) override;
    IFACEMETHOD(OpenContainer)(_In_ LPCSTR Id, _In_ IWSLAContainer** Container) override;
    IFACEMETHOD(ListContainers)(_Out_ WSLA_CONTAINER** Images, _Out_ ULONG* Count) override;
    IFACEMETHOD(CreateRootNamespaceProcess)(
        _In_ LPCSTR Executable, _In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno) override;
    IFACEMETHOD(FormatVirtualDisk)(_In_ LPCWSTR Path) override;
    IFACEMETHOD(Terminate)() override;
    IFACEMETHOD(MountWindowsFolder)(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly) override;
    IFACEMETHOD(UnmountWindowsFolder)(_In_ LPCSTR LinuxPath) override;
    IFACEMETHOD(SaveImage)(_In_ ULONG OutputHandle, _In_ LPCSTR ImageNameOrID, _In_ IProgressCallback* ProgressCallback, _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;
    IFACEMETHOD(ExportContainer)(_In_ ULONG OutputHandle, _In_ LPCSTR ContainerID, _In_ IProgressCallback* ProgressCallback, _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo) override;
    IFACEMETHOD(MapVmPort)(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort) override;
    IFACEMETHOD(UnmapVmPort)(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort) override;

private:
    const ULONG m_sessionId;
    const DWORD m_creatorPid;
    const std::wstring m_displayName;
    const CallingProcessTokenInfo m_tokenInfo;
    std::atomic<bool> m_terminated{false};

    wil::com_ptr<IWSLASession> m_session;
};

} // namespace wsl::windows::service::wsla
