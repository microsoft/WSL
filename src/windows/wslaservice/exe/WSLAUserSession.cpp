/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSession.cpp

Abstract:

    TODO

--*/

#include "WSLAUserSession.h"
#include "WSLASession.h"

using wsl::windows::service::wsla::WSLAUserSessionImpl;

WSLAUserSessionImpl::WSLAUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo) :
    m_tokenInfo(std::move(TokenInfo))
{
}

WSLAUserSessionImpl::~WSLAUserSessionImpl()
{
    // Manually signal the VM termination events. This prevents being stuck on an API call that holds the VM lock.
    {
        std::lock_guard lock(m_lock);

        for (auto* e : m_virtualMachines)
        {
            e->OnSessionTerminating();
        }
    }
}

void WSLAUserSessionImpl::OnVmTerminated(WSLAVirtualMachine* machine)
{
    std::lock_guard lock(m_lock);
    auto pred = [machine](const auto* e) { return machine == e; };

    // Remove any stale VM reference.
    m_virtualMachines.erase(std::remove_if(m_virtualMachines.begin(), m_virtualMachines.end(), pred), m_virtualMachines.end());
}

HRESULT WSLAUserSessionImpl::CreateVirtualMachine(const VIRTUAL_MACHINE_SETTINGS* Settings, IWSLAVirtualMachine** VirtualMachine)
{
    auto vm = wil::MakeOrThrow<WSLAVirtualMachine>(*Settings, GetUserSid(), this);

    {
        std::lock_guard lock(m_lock);
        m_virtualMachines.emplace_back(vm.Get());
    }

    vm->Start();
    THROW_IF_FAILED(vm.CopyTo(__uuidof(IWSLAVirtualMachine), (void**)VirtualMachine));

    return S_OK;
}

PSID WSLAUserSessionImpl::GetUserSid() const
{
    return m_tokenInfo->User.Sid;
}

HRESULT wsl::windows::service::wsla::WSLAUserSessionImpl::CreateSession(
    const WSLA_SESSION_SETTINGS* Settings, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession)
{
    ULONG id = m_nextSessionId++;
    auto session = wil::MakeOrThrow<WSLASession>(id, *Settings, *this, *VmSettings);

    {
        std::lock_guard lock(m_wslaSessionsLock);
        m_wslaSessions.emplace_back(session);
    }

    THROW_IF_FAILED(session.CopyTo(__uuidof(IWSLASession), (void**)WslaSession));

    return S_OK;
}

HRESULT wsl::windows::service::wsla::WSLAUserSessionImpl::ListSessions(_Out_ WSLA_SESSION_INFORMATION** Sessions, _Out_ ULONG* SessionsCount)
{
    auto output = wil::make_unique_cotaskmem<WSLA_SESSION_INFORMATION[]>(m_wslaSessions.size());
    std::lock_guard lock(m_wslaSessionsLock);
    for (size_t i = 0; i < m_wslaSessions.size(); ++i)
    {
        output[i].SessionId = m_wslaSessions[i]->GetId();
        m_wslaSessions[i]->GetDisplayName(&output[i].DisplayName);
    }
    *Sessions = output.release();
    *SessionsCount = static_cast<ULONG>(m_wslaSessions.size());
    return S_OK;
}

wsl::windows::service::wsla::WSLAUserSession::WSLAUserSession(std::weak_ptr<WSLAUserSessionImpl>&& Session) :
    m_session(std::move(Session))
{
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::GetVersion(_Out_ WSL_VERSION* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;

    return S_OK;
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::CreateVirtualMachine(const VIRTUAL_MACHINE_SETTINGS* Settings, IWSLAVirtualMachine** VirtualMachine)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateVirtualMachine(Settings, VirtualMachine);
}
CATCH_RETURN();

HRESULT wsl::windows::service::wsla::WSLAUserSession::CreateSession(
    const WSLA_SESSION_SETTINGS* Settings, const VIRTUAL_MACHINE_SETTINGS* VmSettings, IWSLASession** WslaSession)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateSession(Settings, VmSettings, WslaSession);
}
CATCH_RETURN();

HRESULT wsl::windows::service::wsla::WSLAUserSession::ListSessions(WSLA_SESSION_INFORMATION** Sessions, ULONG* SessionsCount)

{
    if (!Sessions || !SessionsCount)
    {
        return E_INVALIDARG;
    }

    // For now, return an empty list. We'll populate this from m_sessions later.
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ListSessions(Sessions, SessionsCount);
}

HRESULT wsl::windows::service::wsla::WSLAUserSession::OpenSession(ULONG Id, IWSLASession** Session)
{
    return E_NOTIMPL;
}
