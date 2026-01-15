/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLASessionManagerFactory.cpp

Abstract:

    Contains the implementation for WSLASessionManagerFactory.

--*/

#include "precomp.h"

#include "WSLASessionManagerFactory.h"
#include "WSLASessionManager.h"

using wsl::windows::service::wsla::WSLASessionManagerFactory;
using wsl::windows::service::wsla::WSLASessionManagerImpl;

CoCreatableClassWithFactory(WSLASessionManager, WSLASessionManagerFactory);

static std::mutex g_mutex;
static std::optional<WSLASessionManagerImpl> g_sessionManagerImpl = std::make_optional<WSLASessionManagerImpl>();
static Microsoft::WRL::ComPtr<WSLASessionManager> g_sessionManager;

HRESULT WSLASessionManagerFactory::CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated)
{
    RETURN_HR_IF_NULL(E_POINTER, ppCreated);
    *ppCreated = nullptr;

    RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

    WSL_LOG("WSLASessionManagerFactory", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    try
    {
        std::lock_guard lock{g_mutex};

        THROW_HR_IF(CO_E_SERVER_STOPPING, !g_sessionManagerImpl.has_value());

        if (!g_sessionManager)
        {
            g_sessionManager = wil::MakeOrThrow<WSLASessionManager>(&g_sessionManagerImpl.value());
        }

        THROW_IF_FAILED(g_sessionManager.CopyTo(riid, ppCreated));
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();

        // Note: S_FALSE will cause COM to retry if the service is stopping.
        return result == CO_E_SERVER_STOPPING ? S_FALSE : result;
    }

    return S_OK;
}

void wsl::windows::service::wsla::ClearWslaSessionsAndBlockNewInstances()
{
    std::lock_guard lock{g_mutex};

    // Disconnect the COM instance from its implementation.
    if (g_sessionManager)
    {
       // g_sessionManager->Disconnect();
        g_sessionManager.Reset();

        // N.B. Callers might still have references to the COM instance. If that's the case, calls will all fail with RPC_E_DISCONNECTED.
    }

    g_sessionManagerImpl.reset();
}