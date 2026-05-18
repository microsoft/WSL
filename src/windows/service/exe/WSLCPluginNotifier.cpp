// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WSLCPluginNotifier.h"

using wsl::windows::common::COMServiceExecutionContext;
using wsl::windows::service::wslc::WSLCPluginNotifier;

WSLCPluginNotifier::WSLCPluginNotifier(
    wsl::windows::service::PluginManager& Plugins,
    ULONG SessionId,
    DWORD CreatorPid,
    std::wstring DisplayName,
    wil::shared_handle UserToken,
    std::vector<BYTE>&& UserSid) :
    m_plugins(Plugins), m_displayName(std::move(DisplayName)), m_userToken(std::move(UserToken)), m_userSid(std::move(UserSid))
{
    m_sessionInfo.SessionId = static_cast<WSLCSessionId>(SessionId);
    m_sessionInfo.DisplayName = m_displayName.c_str();
    m_sessionInfo.ApplicationPid = CreatorPid;
    m_sessionInfo.UserToken = m_userToken.get();
    m_sessionInfo.UserSid = m_userSid.empty() ? nullptr : reinterpret_cast<PSID>(m_userSid.data());
}

HRESULT WSLCPluginNotifier::OnContainerStarted(LPCSTR InspectJson)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, InspectJson == nullptr);
    return m_plugins.OnWslcContainerStarted(&m_sessionInfo, InspectJson);
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::OnContainerStopping(LPCSTR ContainerId)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, ContainerId == nullptr);
    m_plugins.OnWslcContainerStopping(&m_sessionInfo, ContainerId);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::OnImageCreated(LPCSTR InspectJson)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, InspectJson == nullptr);
    m_plugins.OnWslcImageCreated(&m_sessionInfo, InspectJson);
    return S_OK;
}
CATCH_RETURN();

HRESULT WSLCPluginNotifier::OnImageDeleted(LPCSTR ImageId)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, ImageId == nullptr);
    m_plugins.OnWslcImageDeleted(&m_sessionInfo, ImageId);
    return S_OK;
}
CATCH_RETURN();
