/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ServiceMain.cpp

Abstract:

    This file contains the entrypoint for WslInstallerService.

--*/

#include "precomp.h"
#include "comservicehelper.h"
#include "WslTelemetry.h"
#include "WslInstallerFactory.h"

wil::unique_event g_stopEvent{wil::EventOptions::ManualReset};

static constexpr auto c_serviceName = L"WslInstaller";

CoCreatableClassWrlCreatorMapInclude(WslInstaller);

struct WslInstallSecurityPolicy
{
    static LPCWSTR GetSDDLText()
    {
        // COM Access and Launch permissions allowed for authenticated user, principal self, and system.
        // 0xB = (COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL | COM_RIGHTS_ACTIVATE_LOCAL)
        // N.B. This should be kept in sync with the security descriptor in the appxmanifest.
        return L"O:BAG:BAD:(A;;0xB;;;AU)(A;;0xB;;;PS)(A;;0xB;;;SY)";
    }
};

class WslInstallerService
    : public Windows::Internal::Service<WslInstallerService, Windows::Internal::ShutdownAfterLastObjectReleased, WslInstallSecurityPolicy>
{
public:
    static wchar_t* GetName()
    {
        return const_cast<LPWSTR>(c_serviceName);
    }

    static HRESULT OnServiceStarting();
    HRESULT ServiceStarted();
    static void ServiceStopped();
    static bool AutoInstallEnabled();
};

bool WslInstallerService::AutoInstallEnabled()
try
{
    const auto key = wsl::windows::common::registry::OpenLxssMachineKey(KEY_READ);

    auto value = wsl::windows::common::registry::ReadDword(key.get(), L"MSI", L"AutoUpgradeViaMsix", 1);
    WSL_LOG("AutoUpgradeViaMsix", TraceLoggingLevel(WINEVENT_LEVEL_INFO), TraceLoggingValue(value, "setting"));

    return value == 1;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

HRESULT WslInstallerService::OnServiceStarting()
{
    wil::g_fResultFailFastUnknownExceptions = false;
    wsl::windows::common::wslutil::ConfigureCrt();

    WslTraceLoggingInitialize(WslServiceTelemetryProvider, !wsl::shared::OfficialBuild);
    wsl::windows::common::security::ApplyProcessMitigationPolicies();

    return S_OK;
}

void Stop()
{
    const wil::unique_schandle scm{OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, SC_MANAGER_ALL_ACCESS)};
    THROW_LAST_ERROR_IF(!scm);

    const wil::unique_schandle self{OpenServiceW(scm.get(), c_serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS)};
    THROW_LAST_ERROR_IF(!self);

    SERVICE_STATUS status{};
    THROW_IF_WIN32_BOOL_FALSE(ControlService(self.get(), SERVICE_CONTROL_STOP, &status));
}

HRESULT WslInstallerService::ServiceStarted()
{
    WSL_LOG("WslInstallServiceStarted", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    if (AutoInstallEnabled())
    {
        const auto install = LaunchInstall();
        if (!install)
        {
            ReportCurrentStatus();
            StopAsync();
            return S_OK;
        }

        THROW_LAST_ERROR_IF(WaitForSingleObject(install->Thread.get(), INFINITE) != WAIT_OBJECT_0);
    }

    return S_OK;
}

void WslInstallerService::ServiceStopped()
{
    WSL_LOG("WslInstallServiceStopping", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    g_stopEvent.SetEvent();
    ClearSessions();
}

int __cdecl wmain()
{
    WslInstallerService::ProcessMain();
    return 0;
}