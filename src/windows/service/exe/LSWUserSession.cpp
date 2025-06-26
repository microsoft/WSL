#include "LSWUserSession.h"

using wsl::windows::service::lsw::LSWUserSessionImpl;

wsl::windows::service::lsw::LSWUserSessionImpl::LSWUserSessionImpl(HANDLE Token)
{

}

PSID wsl::windows::service::lsw::LSWUserSessionImpl::GetUserSid() const
{
    return nullptr;
}

wsl::windows::service::lsw::LSWUserSession::LSWUserSession(std::weak_ptr<LSWUserSessionImpl>&& Session) : m_session(std::move(Session))
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
{

    return S_OK;
}
