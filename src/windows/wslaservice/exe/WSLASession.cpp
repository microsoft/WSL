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
#include "ServiceProcessLauncher.h"

using wsl::windows::service::wsla::WSLASession;

WSLASession::WSLASession(const WSLA_SESSION_SETTINGS& Settings, WSLAUserSessionImpl& userSessionImpl, const VIRTUAL_MACHINE_SETTINGS& VmSettings) :
    m_sessionSettings(Settings),
    m_userSession(&userSessionImpl),
    m_virtualMachine(wil::MakeOrThrow<WSLAVirtualMachine>(VmSettings, userSessionImpl.GetUserSid(), &userSessionImpl)),
    m_displayName(Settings.DisplayName)
{
    WSL_LOG("SessionCreated", TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    if (Settings.TerminationCallback != nullptr)
    {
        m_virtualMachine->RegisterCallback(Settings.TerminationCallback);
    }

    m_virtualMachine->Start();
}

WSLASession::~WSLASession()
{
    WSL_LOG("SessionTerminated", TraceLoggingValue(m_displayName.c_str(), "DisplayName"));

    std::lock_guard lock{m_lock};

    if (m_virtualMachine)
    {
        m_virtualMachine->OnSessionTerminated();
        m_virtualMachine.Reset();
    }

    if (m_userSession != nullptr)
    {
        m_userSession->OnSessionTerminated(this);
    }
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
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

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
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    return m_virtualMachine->CreateLinuxProcess(Options, Process, Errno);
}
CATCH_RETURN();

HRESULT WSLASession::FormatVirtualDisk(LPCWSTR Path)
try
{
    THROW_HR_IF_MSG(E_INVALIDARG, !std::filesystem::path(Path).is_absolute(), "FormatVirtualDisk called with a relative path: %ls", Path);

    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    // Attach the disk to the VM (AttachDisk() performs the access check for the VHD file).
    auto [lun, device] = m_virtualMachine->AttachDisk(Path, false);

    // N.B. DetachDisk calls sync() before detaching.
    auto detachDisk = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this, lun]() { m_virtualMachine->DetachDisk(lun); });

    // Format it to ext4.
    constexpr auto mkfsPath = "/usr/sbin/mkfs.ext4";
    ServiceProcessLauncher launcher(mkfsPath, {mkfsPath, device});
    auto result = launcher.Launch(*m_virtualMachine.Get()).WaitAndCaptureOutput();

    THROW_HR_IF_MSG(E_FAIL, result.Code != 0, "%hs", launcher.FormatResult(result).c_str());

    return S_OK;
}
CATCH_RETURN();

void WSLASession::OnUserSessionTerminating()
{
    std::lock_guard lock{m_lock};
    WI_ASSERT(m_userSession != nullptr);

    m_userSession = nullptr;
    m_virtualMachine.Reset();
}

HRESULT WSLASession::Shutdown(ULONG Timeout)
try
{
    std::lock_guard lock{m_lock};
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_virtualMachine);

    THROW_IF_FAILED(m_virtualMachine->Shutdown(Timeout));

    m_virtualMachine.Reset();
    return S_OK;
}
CATCH_RETURN();