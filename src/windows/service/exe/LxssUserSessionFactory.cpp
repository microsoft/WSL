/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssUserSessionFactory.cpp

Abstract:

    This file contains user session factory function definitions.

--*/

#include "precomp.h"
#include "LxssSecurity.h"
#include "LxssUserSessionFactory.h"
#include "PluginManager.h"

using namespace Microsoft::WRL;
using namespace Security;
using namespace wil;

bool g_disabledByPolicy{false};

// Note: g_sessionTerminationLock must always be acquired before g_sessionLock
std::recursive_mutex g_sessionTerminationLock;
srwlock g_sessionLock;

std::optional<std::vector<std::shared_ptr<LxssUserSessionImpl>>> g_sessions =
    std::make_optional<std::vector<std::shared_ptr<LxssUserSessionImpl>>>();

std::optional<wsl::windows::service::PluginManager> g_pluginManager;

extern unique_event g_networkingReady;
extern bool g_lxcoreInitialized;

_Requires_lock_held_(g_sessionLock)
void ClearSessionsAndBlockNewInstancesLockHeld(std::optional<std::vector<std::shared_ptr<LxssUserSessionImpl>>>& sessions)
{
    std::lock_guard lock(g_sessionTerminationLock);

    if (sessions)
    {
        // Shutdown the session and prevent new session creation.
        for (const auto& session : sessions.value())
        {
            // Because Shutdown() acquires the session inner lock, it shouldn't called while g_sessionLock is held,
            // since that could lead to a deadlock if FindSessionByCookie is called since that would try to lock g_sessionLock
            // while holding the session inner lock

            session->Shutdown(true, ShutdownBehavior::ForceAfter30Seconds);
        }

        sessions.reset();
    }

    // Unload plugins
    g_pluginManager.reset();
}

void ClearSessionsAndBlockNewInstances()
{
    std::optional<std::vector<std::shared_ptr<LxssUserSessionImpl>>> sessions;

    {
        auto sessionsLock = g_sessionLock.lock_exclusive();
        sessions = std::move(g_sessions);

        // This is required because the moved-from std::optional<T> isn't made empty, so this needs to be done explicitly.
        g_sessions.reset();
    }

    ClearSessionsAndBlockNewInstancesLockHeld(sessions);
}

void SetSessionPolicy(_In_ bool enabled)
{
    std::lock_guard lock(g_sessionTerminationLock);

    if (enabled)
    {
        auto sessionsLock = g_sessionLock.lock_exclusive();
        if (!g_sessions)
        {
            g_sessions = std::make_optional<std::vector<std::shared_ptr<LxssUserSessionImpl>>>();
        }

        if (!g_pluginManager.has_value())
        {
            g_pluginManager.emplace();
            g_pluginManager->LoadPlugins();
        }
    }
    else
    {
        ClearSessionsAndBlockNewInstances();
    }

    g_disabledByPolicy = !enabled;
}

std::shared_ptr<LxssUserSessionImpl> FindSessionByCookie(_In_ DWORD Cookie)
{
    // Find a session with a matching session ID and terminate it.
    //
    // N.B. Sessions launched from session zero will only be terminated when the
    //      service is stopped.
    auto lock = g_sessionLock.lock_exclusive();
    if (!g_sessions.has_value())
    {
        return {};
    }

    const auto found = std::find_if(std::begin(g_sessions.value()), std::end(g_sessions.value()), [Cookie](const auto& session) {
        return (session->GetSessionCookie() == Cookie);
    });

    return found == g_sessions->end() ? std::shared_ptr<LxssUserSessionImpl>() : *found;
}

void TerminateSession(_In_ DWORD sessionId)
{
    // Find a session with a matching session ID and terminate it.
    //
    // N.B. Sessions launched from session zero will only be terminated when the
    //      service is stopped.

    std::lock_guard lock(g_sessionTerminationLock);

    std::shared_ptr<LxssUserSessionImpl> session;

    {
        auto lock = g_sessionLock.lock_exclusive();
        if (!g_sessions.has_value())
        {
            return;
        }

        const auto found = std::find_if(std::begin(g_sessions.value()), std::end(g_sessions.value()), [&sessionId](const auto& session) {
            return (session->GetSessionId() == sessionId);
        });

        if (found != g_sessions->end())
        {
            // Shutdown the session and prevent new instance creation.
            session = std::move(*found);
            g_sessions->erase(found);
        }
    }

    if (session)
    {
        session->Shutdown(true);
    }
}

CoCreatableClassWithFactory(LxssUserSession, LxssUserSessionFactory);

HRESULT LxssUserSessionFactory::CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated)
{
    RETURN_HR_IF_NULL(E_POINTER, ppCreated);
    *ppCreated = nullptr;

    RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

    WSL_LOG("LxssUserSessionCreateInstanceBegin", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    // Wait for the network cleanup to be done before continuing.
    g_networkingReady.wait();

    try
    {
        auto instance = CreateInstanceForCurrentUser();
        const auto userSession = wil::MakeOrThrow<LxssUserSession>(instance);
        THROW_IF_FAILED(userSession.CopyTo(riid, ppCreated));
    }
    catch (...)
    {
        const auto result = wil::ResultFromCaughtException();

        // Note: S_FALSE will cause COM to retry if the service is stopping.
        return result == CO_E_SERVER_STOPPING ? S_FALSE : result;
    }

    WSL_LOG("LxssUserSessionCreateInstanceEnd", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    return S_OK;
}

_Requires_lock_held_(g_sessionLock)
std::shared_ptr<LxssUserSessionImpl> FindSessionLockHeld(PSID User)
{
    // Fail if the service is stopping (see ClearSessionsAndBlockNewInstances()).
    THROW_HR_IF(CO_E_SERVER_STOPPING, !g_sessions.has_value());

    const auto found = std::find_if(std::begin(g_sessions.value()), std::end(g_sessions.value()), [&](const auto& session) {
        return (::EqualSid(User, session->GetUserSid()));
    });

    if (found != g_sessions->end())
    {
        return *found;
    }
    else
    {
        return {};
    }
}

std::weak_ptr<LxssUserSessionImpl> CreateInstanceForCurrentUser()
{
    // Do not create sessions for localsystem.
    const unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    THROW_HR_IF(WSL_E_LOCAL_SYSTEM_NOT_SUPPORTED, wsl::windows::common::security::IsTokenLocalSystem(userToken.get()));

    // Get the session ID and SID of the client process.
    DWORD sessionId{};
    DWORD length = 0;
    THROW_IF_WIN32_BOOL_FALSE(::GetTokenInformation(userToken.get(), TokenSessionId, &sessionId, sizeof(sessionId), &length));

    const auto tokenInfo = wil::get_token_information<TOKEN_USER>(userToken.get());

    // Find an existing session or create a new one.
    std::shared_ptr<LxssUserSessionImpl> userSession;
    {
        std::lock_guard sessionLock(g_sessionTerminationLock);
        auto lock = g_sessionLock.lock_exclusive();

        // Do not allow session creation if WSL is disabled via policy.
        THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), g_disabledByPolicy);

        // Builds prior to Windows 10 require the WSL optional component.
        THROW_HR_IF(WSL_E_WSL_OPTIONAL_COMPONENT_REQUIRED, !g_lxcoreInitialized && !wsl::windows::common::helpers::IsWindows11OrAbove());

        userSession = FindSessionLockHeld(tokenInfo->User.Sid);

        if (!userSession)
        {
            userSession.reset(new LxssUserSessionImpl(tokenInfo->User.Sid, sessionId, *g_pluginManager));
            g_sessions->emplace_back(userSession);
        }
    }

    return userSession;
}
