/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LSWUserSessionFactory.cpp

Abstract:

    TODO

--*/
#include "precomp.h"

#include "LSWUserSessionFactory.h"
#include "LSWUserSession.h"

using wsl::windows::service::lsw::LSWUserSessionFactory;
using wsl::windows::service::lsw::LSWUserSessionImpl;

CoCreatableClassWithFactory(LSWUserSession, LSWUserSessionFactory);

static std::mutex g_mutex;
static std::optional<std::vector<std::shared_ptr<LSWUserSessionImpl>>> g_sessions =
    std::make_optional<std::vector<std::shared_ptr<LSWUserSessionImpl>>>();

HRESULT LSWUserSessionFactory::CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated)
{
    RETURN_HR_IF_NULL(E_POINTER, ppCreated);
    *ppCreated = nullptr;

    RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

    WSL_LOG("LSWUserSessionFactory", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    try
    {
        const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

        // Get the session ID and SID of the client process.
        DWORD sessionId{};
        DWORD length = 0;
        THROW_IF_WIN32_BOOL_FALSE(::GetTokenInformation(userToken.get(), TokenSessionId, &sessionId, sizeof(sessionId), &length));

        auto tokenInfo = wil::get_token_information<TOKEN_USER>(userToken.get());

        std::lock_guard lock{g_mutex};

        THROW_HR_IF(CO_E_SERVER_STOPPING, !g_sessions.has_value());

        auto session = std::find_if(g_sessions->begin(), g_sessions->end(), [&tokenInfo](auto it) {
            return EqualSid(it->GetUserSid(), &tokenInfo->User.Sid);
        });

        if (session == g_sessions->end())
        {
            session = g_sessions->insert(g_sessions->end(), std::make_shared<LSWUserSessionImpl>(userToken.get(), std::move(tokenInfo)));
        }

        auto comInstance = wil::MakeOrThrow<LSWUserSession>(std::weak_ptr<LSWUserSessionImpl>(*session));

        THROW_IF_FAILED(comInstance.CopyTo(riid, ppCreated));
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();

        // Note: S_FALSE will cause COM to retry if the service is stopping.
        return result == CO_E_SERVER_STOPPING ? S_FALSE : result;
    }

    WSL_LOG("LSWUserSessionFactory", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    return S_OK;
}

void wsl::windows::service::lsw::ClearLswSessionsAndBlockNewInstances()
{
    std::lock_guard lock{g_mutex};
    g_sessions.reset();
}