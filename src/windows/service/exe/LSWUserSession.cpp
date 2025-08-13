/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWUserSession.cpp

Abstract:

    TODO

--*/
#include "LSWUserSession.h"

using wsl::windows::service::lsw::LSWUserSessionImpl;

LSWUserSessionImpl::LSWUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo) :
    m_tokenInfo(std::move(TokenInfo))
{
}

LSWUserSessionImpl::~LSWUserSessionImpl()
{
    // Manually signal the VM termination events. This prevents being stuck on an API call that holds the VM lock.
    {
        std::lock_guard lock(m_virtualMachinesLock);

        for (auto* e : m_virtualMachines)
        {
            e->OnSessionTerminating();
        }
    }
}

void LSWUserSessionImpl::OnVmTerminated(LSWVirtualMachine* machine)
{
    std::lock_guard lock(m_virtualMachinesLock);
    auto pred = [machine](const auto* e) { return machine == e; };

    // Remove any stale VM reference.
    m_virtualMachines.erase(std::remove_if(m_virtualMachines.begin(), m_virtualMachines.end(), pred), m_virtualMachines.end());
}

HRESULT LSWUserSessionImpl::CreateVirtualMachine(const VIRTUAL_MACHINE_SETTINGS* Settings, ILSWVirtualMachine** VirtualMachine)
{
    auto vm = wil::MakeOrThrow<LSWVirtualMachine>(*Settings, GetUserSid(), this);

    {
        std::lock_guard lock(m_virtualMachinesLock);
        m_virtualMachines.emplace_back(vm.Get());
    }

    vm->Start();
    THROW_IF_FAILED(vm.CopyTo(__uuidof(ILSWVirtualMachine), (void**)VirtualMachine));

    return S_OK;
}

PSID LSWUserSessionImpl::GetUserSid() const
{
    return m_tokenInfo->User.Sid;
}

wsl::windows::service::lsw::LSWUserSession::LSWUserSession(std::weak_ptr<LSWUserSessionImpl>&& Session) :
    m_session(std::move(Session))
{
}

HRESULT wsl::windows::service::lsw::LSWUserSession::GetVersion(_Out_ WSL_VERSION* Version)
{
    Version->Major = WSL_PACKAGE_VERSION_MAJOR;
    Version->Minor = WSL_PACKAGE_VERSION_MINOR;
    Version->Revision = WSL_PACKAGE_VERSION_REVISION;

    return S_OK;
}

HRESULT wsl::windows::service::lsw::LSWUserSession::CreateVirtualMachine(const VIRTUAL_MACHINE_SETTINGS* Settings, ILSWVirtualMachine** VirtualMachine)
try
{
    auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateVirtualMachine(Settings, VirtualMachine);
}
CATCH_RETURN();