/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAUserSessionFactory.cpp

Abstract:

    Contains the implementation for WSLASessionManagerFactory.

--*/

#include "precomp.h"

#include "WSLASessionManagerFactory.h"
#include "WSLASessionManager.h"

using wsl::windows::service::wsla::WSLASessionManagerFactory;
using wsl::windows::service::wsla::WSLASessionManager;

CoCreatableClassWithFactory(IWSLASessionManager, WSLASessionManagerFactory);

static std::mutex g_mutex;
static std::optional<std::shared_ptr<WSLASessionManager>> g_sessionManager = std::make_shared<WSLASessionManager>();

HRESULT WSLASessionManagerFactory::CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated)
{
    RETURN_HR_IF_NULL(E_POINTER, ppCreated);
    *ppCreated = nullptr;

    RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

    WSL_LOG("WSLASessionManagerFactory", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    try
    {
        std::lock_guard lock{g_mutex};

        THROW_HR_IF(CO_E_SERVER_STOPPING, !g_sessionManager.has_value());

        auto comInstance = wil::MakeOrThrow<WSLAUserSession>(std::weak_ptr<WSLAUserSessionImpl>(*session));

        THROW_IF_FAILED(comInstance.CopyTo(riid, ppCreated));
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();

        // Note: S_FALSE will cause COM to retry if the service is stopping.
        return result == CO_E_SERVER_STOPPING ? S_FALSE : result;
    }

    WSL_LOG("WSLAUserSessionFactory", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    return S_OK;
}

void wsl::windows::service::wsla::ClearWslaSessionsAndBlockNewInstances()
{
    std::lock_guard lock{g_mutex};
    g_sessionManager.reset();
}