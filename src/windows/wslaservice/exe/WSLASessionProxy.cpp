/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionProxy.cpp

Abstract:

    Implementation for WSLASessionProxy.

    This proxy wraps a remote IWSLASession and forwards all calls to it.
    It enables weak reference tracking for non-persistent sessions.

--*/

#include "WSLASessionProxy.h"

using wsl::windows::service::wsla::CallingProcessTokenInfo;
using wsl::windows::service::wsla::WSLASessionProxy;

WSLASessionProxy::WSLASessionProxy(
    _In_ ULONG SessionId, _In_ DWORD CreatorPid, _In_ std::wstring DisplayName, _In_ CallingProcessTokenInfo TokenInfo, _In_ wil::com_ptr<IWSLASession> Session) :
    m_sessionId(SessionId),
    m_creatorPid(CreatorPid),
    m_displayName(std::move(DisplayName)),
    m_tokenInfo(std::move(TokenInfo)),
    m_session(std::move(Session))
{
    WSL_LOG(
        "WSLASessionProxyCreated",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(m_sessionId, "SessionId"),
        TraceLoggingWideString(m_displayName.c_str(), "DisplayName"));
}

WSLASessionProxy::~WSLASessionProxy()
{
    WSL_LOG(
        "WSLASessionProxyDestroyed",
        TraceLoggingLevel(WINEVENT_LEVEL_INFO),
        TraceLoggingUInt32(m_sessionId, "SessionId"),
        TraceLoggingWideString(m_displayName.c_str(), "DisplayName"));

    // Terminate the underlying session when the proxy is destroyed.
    // For non-persistent sessions, this happens when all client refs are released.
    // For persistent sessions, this only happens during service shutdown.
    if (m_session && !m_terminated)
    {
        LOG_IF_FAILED(m_session->Terminate());
    }
}

void WSLASessionProxy::CopyDisplayName(_Out_writes_z_(bufferLength) PWSTR Buffer, size_t BufferLength) const
{
    THROW_HR_IF(E_BOUNDS, m_displayName.size() + 1 > BufferLength);
    wcscpy_s(Buffer, BufferLength, m_displayName.c_str());
}

// GetProcessHandle and Initialize are not supported on the proxy - they are only used during
// session creation and are called directly on the remote session, not through the proxy.
HRESULT WSLASessionProxy::GetProcessHandle(_Out_ HANDLE* ProcessHandle)
{
    RETURN_HR_IF_NULL(E_POINTER, ProcessHandle);
    *ProcessHandle = nullptr;
    return E_NOTIMPL;
}

HRESULT WSLASessionProxy::Initialize(_In_ const WSLA_SESSION_INIT_SETTINGS*, _In_ IWSLAVirtualMachine*)
{
    return E_NOTIMPL;
}

HRESULT WSLASessionProxy::GetId(_Out_ ULONG* Id)
{
    *Id = m_sessionId;
    return S_OK;
}

HRESULT WSLASessionProxy::PullImage(
    _In_ LPCSTR ImageUri,
    _In_ const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryAuthenticationInformation,
    _In_ IProgressCallback* ProgressCallback,
    _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo)
{
    return m_session->PullImage(ImageUri, RegistryAuthenticationInformation, ProgressCallback, ErrorInfo);
}

HRESULT WSLASessionProxy::LoadImage(_In_ ULONG ImageHandle, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength)
try
{
    wil::unique_handle localHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(ImageHandle))};
    return m_session->LoadImage(HandleToULong(localHandle.get()), ProgressCallback, ContentLength);
}
CATCH_RETURN()

HRESULT WSLASessionProxy::ImportImage(_In_ ULONG ImageHandle, _In_ LPCSTR ImageName, _In_ IProgressCallback* ProgressCallback, _In_ ULONGLONG ContentLength)
try
{
    wil::unique_handle localHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(ImageHandle))};
    return m_session->ImportImage(HandleToULong(localHandle.get()), ImageName, ProgressCallback, ContentLength);
}
CATCH_RETURN()

HRESULT WSLASessionProxy::ListImages(_Out_ WSLA_IMAGE_INFORMATION** Images, _Out_ ULONG* Count)
{
    return m_session->ListImages(Images, Count);
}

HRESULT WSLASessionProxy::DeleteImage(
    _In_ const WSLA_DELETE_IMAGE_OPTIONS* Options, _Out_ WSLA_DELETED_IMAGE_INFORMATION** DeletedImages, _Out_ ULONG* Count, _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo)
{
    return m_session->DeleteImage(Options, DeletedImages, Count, ErrorInfo);
}

HRESULT WSLASessionProxy::CreateContainer(_In_ const WSLA_CONTAINER_OPTIONS* Options, _Out_ IWSLAContainer** Container, _Inout_opt_ WSLA_ERROR_INFO* Error)
{
    return m_session->CreateContainer(Options, Container, Error);
}

HRESULT WSLASessionProxy::OpenContainer(_In_ LPCSTR Id, _In_ IWSLAContainer** Container)
{
    return m_session->OpenContainer(Id, Container);
}

HRESULT WSLASessionProxy::ListContainers(_Out_ WSLA_CONTAINER** Images, _Out_ ULONG* Count)
{
    return m_session->ListContainers(Images, Count);
}

HRESULT WSLASessionProxy::CreateRootNamespaceProcess(
    _In_ LPCSTR Executable, _In_ const WSLA_PROCESS_OPTIONS* Options, _Out_ IWSLAProcess** Process, _Out_ int* Errno)
{
    return m_session->CreateRootNamespaceProcess(Executable, Options, Process, Errno);
}

HRESULT WSLASessionProxy::FormatVirtualDisk(_In_ LPCWSTR Path)
{
    return m_session->FormatVirtualDisk(Path);
}

HRESULT WSLASessionProxy::Terminate()
{
    m_terminated = true;
    return m_session->Terminate();
}

HRESULT WSLASessionProxy::MountWindowsFolder(_In_ LPCWSTR WindowsPath, _In_ LPCSTR LinuxPath, _In_ BOOL ReadOnly)
{
    return m_session->MountWindowsFolder(WindowsPath, LinuxPath, ReadOnly);
}

HRESULT WSLASessionProxy::UnmountWindowsFolder(_In_ LPCSTR LinuxPath)
{
    return m_session->UnmountWindowsFolder(LinuxPath);
}

HRESULT WSLASessionProxy::SaveImage(_In_ ULONG OutputHandle, _In_ LPCSTR ImageNameOrID, _In_ IProgressCallback* ProgressCallback, _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo)
try
{
    wil::unique_handle localHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};
    return m_session->SaveImage(HandleToULong(localHandle.get()), ImageNameOrID, ProgressCallback, ErrorInfo);
}
CATCH_RETURN()

HRESULT WSLASessionProxy::ExportContainer(
    _In_ ULONG OutputHandle, _In_ LPCSTR ContainerID, _In_ IProgressCallback* ProgressCallback, _Inout_opt_ WSLA_ERROR_INFO* ErrorInfo)
try
{
    wil::unique_handle localHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutputHandle))};
    return m_session->ExportContainer(HandleToULong(localHandle.get()), ContainerID, ProgressCallback, ErrorInfo);
}
CATCH_RETURN()

HRESULT WSLASessionProxy::MapVmPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort)
{
    return m_session->MapVmPort(Family, WindowsPort, LinuxPort);
}

HRESULT WSLASessionProxy::UnmapVmPort(_In_ int Family, _In_ short WindowsPort, _In_ short LinuxPort)
{
    return m_session->UnmapVmPort(Family, WindowsPort, LinuxPort);
}
