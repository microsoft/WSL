/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslInstallerFactory.cpp

Abstract:

    This file contains the WslInstallerFactory class implementation.

--*/

#include "precomp.h"

#include "WslInstallerFactory.h"

CoCreatableClassWithFactory(WslInstaller, WslInstallerFactory);

static std::vector<std::shared_ptr<WslInstaller>> g_sessions;
static wil::srwlock g_mutex;
extern wil::unique_event g_stopEvent;

void ClearSessions()
{
    auto lock = g_mutex.lock_exclusive();
    g_sessions.clear();
}

HRESULT WslInstallerFactory::CreateInstance(_In_ IUnknown* pUnkOuter, _In_ REFIID riid, _Out_ void** ppCreated)
{
    RETURN_HR_IF_NULL(E_POINTER, ppCreated);
    *ppCreated = nullptr;

    RETURN_HR_IF(CLASS_E_NOAGGREGATION, pUnkOuter != nullptr);

    WSL_LOG("WslInstallerCreateInstance", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    auto lock = g_mutex.lock_exclusive();

    if (g_stopEvent.is_signaled())
    {
        return S_FALSE;
    }

    const auto instance = wil::MakeOrThrow<WslInstaller>();
    instance.CopyTo(riid, ppCreated);

    return S_OK;
}