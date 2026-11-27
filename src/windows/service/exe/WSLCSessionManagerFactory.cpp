/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionManagerFactory.cpp

Abstract:

    Contains the implementation for WSLCSessionManagerFactory.

--*/

#include "precomp.h"

#include <memory>
#include "WSLCSessionManagerFactory.h"
#include "WSLCSessionManager.h"
#include "wslpolicies.h"

using wsl::windows::service::wslc::WSLCSessionManagerFactory;
using wsl::windows::service::wslc::WSLCSessionManagerImpl;

CoCreatableClassWithFactory(WSLCSessionManager, WSLCSessionManagerFactory);

static std::mutex g_mutex;

static std::mutex g_pluginLookupLock;
static std::condition_variable g_pluginLookupCv;
_Guarded_by_(g_pluginLookupLock) static size_t g_activePluginLookups = 0;

// Read under either lock; moving ownership requires both g_mutex and g_pluginLookupLock.
static std::unique_ptr<WSLCSessionManagerImpl> g_sessionManagerImpl = std::make_unique<WSLCSessionManagerImpl>();
static Microsoft::WRL::ComPtr<WSLCSessionManager> g_sessionManager;

wil::com_ptr<IWSLCSession> wsl::windows::service::wslc::FindWslcSession(ULONG SessionId)
{
    WSLCSessionManagerImpl* sessionManagerImpl = nullptr;
    {
        std::lock_guard lock(g_pluginLookupLock);
        THROW_HR_IF(RPC_E_DISCONNECTED, !g_sessionManagerImpl);

        sessionManagerImpl = g_sessionManagerImpl.get();
        ++g_activePluginLookups;
    }

    auto release = wil::scope_exit([]() {
        std::lock_guard lock(g_pluginLookupLock);
        --g_activePluginLookups;
        g_pluginLookupCv.notify_all();
    });

    // Shutdown cannot destroy the session manager until all lookups have returned.
    return sessionManagerImpl->FindSession(SessionId);
}

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

        THROW_HR_IF(CO_E_SERVER_STOPPING, !g_sessionManagerImpl);

        if (!g_sessionManager)
        {
            g_sessionManager = wil::MakeOrThrow<WSLCSessionManager>(g_sessionManagerImpl.get());
        }

        THROW_IF_FAILED(g_sessionManager.CopyTo(riid, ppCreated));
    }
    CATCH_RETURN()

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

    std::unique_ptr<WSLCSessionManagerImpl> sessionManagerImpl;
    {
        std::unique_lock lock(g_pluginLookupLock);
        sessionManagerImpl = std::move(g_sessionManagerImpl);
        g_pluginLookupCv.wait(lock, []() { return g_activePluginLookups == 0; });
    }
}
