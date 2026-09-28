/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssUserSessionFactory.h

Abstract:

    This file contains user session factory function declarations.

--*/

#pragma once
#include <wil/resource.h>
#include "LxssUserSession.h"

class LxssUserSessionImpl;

/// <summary>
/// WRL supports caching of class factories, but doesn't have a notion of a "singleton" COM object.
/// By providing our own class factory for creating LxssUserSession objects, we can control the lifetime
/// of the handed-out session object and ensure there's only one.
/// </summary>
class LxssUserSessionFactory : public Microsoft::WRL::ClassFactory<>
{
public:
    LxssUserSessionFactory() = default;

    /// <summary>
    /// IClassFactory::CreateInstance - creates or hands out the LxssUserSession object to use.
    /// </summary>
    STDMETHODIMP CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated) override;
};

/// <summary>
/// Clean shutdown - clear all active sessions and prevent new sessions from being created.
/// </summary>
void ClearSessionsAndBlockNewInstances();

/// <summary>
/// Create a new session for the current user.
/// </summary>
std::weak_ptr<LxssUserSessionImpl> CreateInstanceForCurrentUser();

/// <summary>
/// Set session creation policy. This is controlled by WSL enterprise policy keys.
/// </summary>
void SetSessionPolicy(_In_ bool enabled);

/// <summary>
/// Clean shutdown - terminate a specific session.
/// </summary>
void TerminateSession(_In_ DWORD sessionId);

/// <summary>
/// Find a session for a given user.
/// </summary>
std::shared_ptr<LxssUserSessionImpl> FindSession(PSID User);

/// <summary>
/// Find a session for a given cookie.
/// </summary>
std::shared_ptr<LxssUserSessionImpl> FindSessionByCookie(DWORD Cookie);