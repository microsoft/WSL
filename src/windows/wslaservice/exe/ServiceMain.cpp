/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceMain.cpp

Abstract:

    This file contains the entrypoint for the Lxss Manager service.

--*/

#include "precomp.h"
#include "comservicehelper.h"
#include "LxssSecurity.h"
#include "WslCoreFilesystem.h"
#include "WSLAUserSessionFactory.h"
#include <ctime>

using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::policies;

wil::unique_event g_networkingReady{wil::EventOptions::ManualReset};

// Declare the WSLAUserSession COM class.
CoCreatableClassWrlCreatorMapInclude(WSLAUserSession);

struct WslaServiceSecurityPolicy
{
    static LPCWSTR GetSDDLText()
    {
        // COM Access and Launch permissions allowed for authenticated user, principal self, and system.
        // 0xB = (COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL | COM_RIGHTS_ACTIVATE_LOCAL)
        // N.B. This should be kept in sync with the security descriptors in the appxmanifest and wslamsi.wix.
        return L"O:BAG:BAD:(A;;0xB;;;AU)(A;;0xB;;;PS)(A;;0xB;;;SY)";
    }
};

class WslaService : public Windows::Internal::Service<WslaService, Windows::Internal::ContinueRunningWithNoObjects, WslaServiceSecurityPolicy>
{
public:
    static wchar_t* GetName()
    {
        return const_cast<LPWSTR>(L"WSLAService");
    }

    static void OnSessionChanged(DWORD eventType, DWORD sessionId);
    HRESULT OnServiceStarting();
    HRESULT ServiceStarted();
    void ServiceStopped();

private:
    wil::unique_couninitialize_call m_coInit{false};
};

HRESULT WslaService::OnServiceStarting()
try
{
    ConfigureCrt();

    // Enable contextualized errors
    wsl::windows::common::EnableContextualizedErrors(true);

    // Initialize telemetry.
    // TODO-WSLA: Create a dedicated WSLA provider
    WslTraceLoggingInitialize(WslaTelemetryProvider, !wsl::shared::OfficialBuild);

    WSL_LOG("Service starting", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    // Don't kill the process on unknown C++ exceptions.
    wil::g_fResultFailFastUnknownExceptions = false;

    wsl::windows::common::security::ApplyProcessMitigationPolicies();

    // Initialize Winsock.
    WSADATA Data;
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

    return S_OK;
}
CATCH_RETURN()

HRESULT WslaService::ServiceStarted()
{
    m_coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    return S_OK;
}

void WslaService::OnSessionChanged(DWORD eventType, DWORD sessionId)
{
    if (eventType == WTS_SESSION_LOGOFF)
    {
        // TODO-WSLA: Implement for WSLA
        // TerminateSession(sessionId);
    }
}

void WslaService::ServiceStopped()
{
    WSL_LOG("Service stopping", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    // Terminate all user sessions.
    wsl::windows::service::wsla::ClearWslaSessionsAndBlockNewInstances();

    // There is a potential deadlock if CoUninitialize() is called before the LanguageChangeNotifyThread
    // isn't done initializing. Clearing the COM objects before calling CoUninitialize() works around the issue.
    winrt::clear_factory_cache();

    // Tear down telemetry.
    WslTraceLoggingUninitialize();

    // uninitialize COM. This must be done here because this call can cause cleanups that will be fail
    // if the CRT is shutting down.
    m_coInit.reset();
}

int __cdecl wmain()
{
    WslaService::ProcessMain();
    return 0;
}
