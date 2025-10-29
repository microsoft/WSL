#include "C:/Users/trivedipooja/source/repos/WSL/src/windows/common/CMakeFiles/common.dir/Debug/cmake_pch.hxx"
#include "WSLASession.h"

using wsl::windows::service::wsla::WSLASessionImpl;
using wsl::windows::service::wsla::WSLASession;

WSLASessionImpl::WSLASessionImpl()
{
}


WSLASessionImpl::~WSLASessionImpl()
{
}

wsl::windows::service::wsla::WSLASession::WSLASession(std::weak_ptr<WSLASessionImpl>&& Session) :
    m_wslaSession(std::move(Session))
{
}

HRESULT WSLASession::GetDisplayName(LPWSTR* DisplayName)
{
    *DisplayName = wil::make_unique_string<wil::unique_cotaskmem_string>(m_sessionConfig.DisplayName).release();
    return S_OK;
}

WSLASession::WSLASession(const WSLA_SESSION_CONFIGURATION& SessionConfiguration) : m_sessionConfig(SessionConfiguration)
{
}