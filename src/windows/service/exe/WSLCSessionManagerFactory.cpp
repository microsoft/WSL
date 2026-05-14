/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionManagerFactory.cpp

Abstract:

    Contains the implementation for WSLCSessionManagerFactory.

--*/

#include "precomp.h"

#include "WSLCSessionManagerFactory.h"
#include "WSLCSessionManager.h"
#include "wslpolicies.h"

using wsl::windows::service::wslc::WSLCSessionManagerFactory;
using wsl::windows::service::wslc::WSLCSessionManagerImpl;

CoCreatableClassWithFactory(WSLCSessionManager, WSLCSessionManagerFactory);

static std::mutex g_mutex;
static std::optional<WSLCSessionManagerImpl> g_sessionManagerImpl = std::make_optional<WSLCSessionManagerImpl>();
static Microsoft::WRL::ComPtr<WSLCSessionManager> g_sessionManager;

HRESULT WSLCSessionManagerFactory::CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated)
{
    RETURN_HR_IF_NULL(E_POINTER, ppCreated);
    *ppCreated = nullptr;

    RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

    WSL_LOG("WSLCSessionManagerFactory", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    try
    {
        wsl::windows::common::COMServiceExecutionContext context;

        namespace policies = wsl::windows::policies;
        THROW_HR_WITH_USER_ERROR_IF(
            WSLC_E_CONTAINER_DISABLED,
            wsl::shared::Localization::MessageWSLContainerDisabled(),
            !policies::IsFeatureAllowed(policies::OpenPoliciesKey().get(), policies::c_allowWSLContainer));

        std::lock_guard lock{g_mutex};

        THROW_HR_IF(CO_E_SERVER_STOPPING, !g_sessionManagerImpl.has_value());

        if (!g_sessionManager)
        {
            g_sessionManager = wil::MakeOrThrow<WSLCSessionManager>(&g_sessionManagerImpl.value());
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

void wsl::windows::service::wslc::ClearWslcSessionsAndBlockNewInstances()
{
    std::lock_guard lock{g_mutex};

    // Disconnect the COM instance from its implementation.
    if (g_sessionManager)
    {
        g_sessionManager->Disconnect();
        g_sessionManager.Reset();

        // N.B. Callers might still have references to the COM instance. If that's the case, calls will all fail with RPC_E_DISCONNECTED.
    }

    g_sessionManagerImpl.reset();
}