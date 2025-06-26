#include "LSWUserSession.h"
#include "LSWVirtualMachine.h"

using wsl::windows::service::lsw::LSWUserSessionImpl;

wsl::windows::service::lsw::LSWUserSessionImpl::LSWUserSessionImpl(HANDLE Token, wil::unique_tokeninfo_ptr<TOKEN_USER>&& TokenInfo) :
    m_tokenInfo(std::move(TokenInfo))
{
}

PSID wsl::windows::service::lsw::LSWUserSessionImpl::GetUserSid() const
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

    auto vm = wil::MakeOrThrow<LSWVirtualMachine>(*Settings, session->GetUserSid());;
    THROW_IF_FAILED(vm.CopyTo(__uuidof(ILSWVirtualMachine), (void**)VirtualMachine));
    return S_OK;
}
CATCH_RETURN();