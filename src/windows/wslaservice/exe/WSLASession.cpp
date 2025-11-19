/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASession.cpp

Abstract:

    This file contains the implementation of the WSLASession COM class.

--*/

#include "precomp.h"
#include "WSLASession.h"
#include "WSLAUserSession.h"
#include "WSLAContainer.h"

using wsl::windows::service::wsla::WSLASession;

WSLASession::WSLASession(const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl, const VIRTUAL_MACHINE_SETTINGS& VmSettings) :
    m_sessionSettings(Settings),
    m_userSession(userSessionImpl),
    m_virtualMachine(std::make_optional<WSLAVirtualMachine>(VmSettings, userSessionImpl.GetUserSid(), &userSessionImpl)),
    m_displayName(Settings.DisplayName)
{
    if (Settings.TerminationCallback != nullptr)
    {
        m_virtualMachine->RegisterCallback(Settings.TerminationCallback);
    }

    m_virtualMachine->Start();
}

HRESULT WSLASession::GetDisplayName(LPWSTR* DisplayName)
{
    *DisplayName = wil::make_unique_string<wil::unique_cotaskmem_string>(m_displayName.c_str()).release();
    return S_OK;
}

HRESULT WSLASession::PullImage(LPCWSTR Image, const WSLA_REGISTRY_AUTHENTICATION_INFORMATION* RegistryInformation, IProgressCallback* ProgressCallback)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::ImportImage(ULONG Handle, LPCWSTR Image, IProgressCallback* ProgressCallback)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::ListImages(WSLA_IMAGE_INFORMATION** Images, ULONG* Count)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::DeleteImage(LPCWSTR Image)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::CreateContainer(const WSLA_CONTAINER_OPTIONS* Options, IWSLAContainer** Container)
try
{
    // Basic instanciation for testing.
    // TODO: Implement.

    auto container = wil::MakeOrThrow<WSLAContainer>();
    container.CopyTo(__uuidof(IWSLAContainer), (void**)Container);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLASession::OpenContainer(LPCWSTR Name, IWSLAContainer** Container)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::ListContainers(WSLA_CONTAINER** Images, ULONG* Count)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::GetVirtualMachine(IWSLAVirtualMachine** VirtualMachine)
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine.has_value());

    THROW_IF_FAILED(m_virtualMachine->QueryInterface(__uuidof(IWSLAVirtualMachine), (void**)VirtualMachine));
    return S_OK;
}

HRESULT WSLASession::CreateRootNamespaceProcess(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
try
{
    if (Errno != nullptr)
    {
        *Errno = -1; // Make sure not to return 0 if something fails.
    }

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine.has_value());

    return m_virtualMachine->CreateLinuxProcess(Options, Process, Errno);
}
CATCH_RETURN();

HRESULT WSLASession::FormatVirtualDisk(LPCWSTR Path)
{
    return E_NOTIMPL;
}

HRESULT WSLASession::Shutdown(ULONG Timeout)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine.has_value());

    THROW_IF_FAILED(m_virtualMachine->Shutdown(Timeout));

    m_virtualMachine.reset();
    return S_OK;
}
CATCH_RETURN();