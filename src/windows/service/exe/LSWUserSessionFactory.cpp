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

CoCreatableClassWithFactory(LSWUserSession, LSWUserSessionFactory);

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

        static std::mutex mutex;
        static std::vector<std::shared_ptr<LSWUserSessionImpl>> sessions;

        std::lock_guard lock{mutex};

        auto session = std::find_if(
            sessions.begin(), sessions.end(), [&tokenInfo](auto it) { return EqualSid(it->GetUserSid(), &tokenInfo->User.Sid); });

        if (session == sessions.end())
        {
            session = sessions.insert(sessions.end(), std::make_shared<LSWUserSessionImpl>(userToken.get(), std::move(tokenInfo)));
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