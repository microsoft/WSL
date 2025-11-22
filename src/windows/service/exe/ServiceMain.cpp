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
#include "LxssIpTables.h"
#include "LxssUserSessionFactory.h"
#include <ctime>

using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::string;
using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::policies;

bool g_lxcoreInitialized{false};
wil::unique_event g_networkingReady{wil::EventOptions::ManualReset};

// Declare the LxssUserSession COM class.
CoCreatableClassWrlCreatorMapInclude(LxssUserSession);

struct WslServiceSecurityPolicy
{
    static LPCWSTR GetSDDLText()
    {
        // COM Access and Launch permissions allowed for authenticated user, principal self, and system.
        // 0xB = (COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL | COM_RIGHTS_ACTIVATE_LOCAL)
        // N.B. This should be kept in sync with the security descriptors in the appxmanifest and package.wix.
        return L"O:BAG:BAD:(A;;0xB;;;AU)(A;;0xB;;;PS)(A;;0xB;;;SY)";
    }
};

class WslService : public Windows::Internal::Service<WslService, Windows::Internal::ContinueRunningWithNoObjects, WslServiceSecurityPolicy>
{
public:
    static wchar_t* GetName()
    {
        return const_cast<LPWSTR>(L"WslService");
    }

    static void OnSessionChanged(DWORD eventType, DWORD sessionId);
    HRESULT OnServiceStarting();
    HRESULT ServiceStarted();
    void ServiceStopped();

private:
    static void __stdcall CheckForUpdates(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_ PVOID Context, _Inout_ PTP_TIMER);
    static void ApplyProcessPolicies();

    void CreateExplorerExtensions() noexcept;
    void EvaluateWslPolicy();
    void Initialize();
    static void InitializePlan9Redirector();
    void RegisterEventSource();
    void StartCheckingForUpdates();

    wil::unique_couninitialize_call m_coInit{false};
    wil::unique_registry_watcher m_watcher;
    wil::unique_threadpool_timer m_updateCheckTimer;
    wil::unique_any_handle_null<decltype(&::DeregisterEventSource), ::DeregisterEventSource> m_eventLog;
};

void WslService::EvaluateWslPolicy()
{
    // If WSL is disabled, terminate any sessions and block future sessions from being created.
    //
    // N.B. This is done instead of failing service start so a proper error can be returned to the user.
    const auto policiesKey = OpenPoliciesKey();
    const auto enabled = IsFeatureAllowed(policiesKey.get(), c_allowWSL);
    if (enabled)
    {
        Initialize();
    }

    SetSessionPolicy(enabled);
}

void WslService::Initialize()
{
    static std::once_flag flag{};
    std::call_once(flag, [&]() {
        // Initialize the connection to the LxCore driver.
        //
        // N.B. The WSL optional component is required on Windows 10. On Windows 11 and later,
        //      the lifted WSL service can run but will only support WSL2 distros.
        g_lxcoreInitialized = NT_SUCCESS(::LxssClientInitialize());

        try
        {
            // Initialize the Plan 9 redirector (can fail iff the OC is not enabled on Win10).
            // Failures here are silently ignored because we don't want the service to fail to start in that case
            // so it can return WSL_E_WSL_OPTIONAL_COMPONENT_REQUIRED in LxssUserSession
            InitializePlan9Redirector();
        }
        CATCH_LOG()

        RegisterEventSource();
    });
}

void WslService::InitializePlan9Redirector()
{
    // Make sure that the Plan 9 redirector trigger start prefix is correct.
    try
    {
        // Acquire backup and restore privileges to modify the P9NP trigger start registry key.
        auto restore = wsl::windows::common::security::AcquirePrivileges({SE_BACKUP_NAME, SE_RESTORE_NAME});

        // Read the P9NP registry key and ensure it contains the correct value.
        constexpr auto* keyName = L"SYSTEM\\CurrentControlSet\\Services\\P9NP\\NetworkProvider";
        const auto key = CreateKey(HKEY_LOCAL_MACHINE, keyName, (KEY_READ | KEY_SET_VALUE), nullptr, REG_OPTION_BACKUP_RESTORE);
        constexpr auto* valueName = L"TriggerStartPrefix";
        DWORD valueType{};
        THROW_IF_WIN32_ERROR(RegGetValueW(key.get(), nullptr, valueName, (RRF_RT_ANY | RRF_NOEXPAND), &valueType, nullptr, nullptr));
        if (valueType != REG_MULTI_SZ)
        {
            // Because older Windows 10 builds won't have the p9rdr changes to support TriggerStartPrefix being a REG_MULTI_SZ,
            // make sure that this build has the updated AppIdFlags value (added to support vp9fs being called from packaged context),
            // which was added in the same commit.
            // This theoretically shouldn't happen since the package shouldn't install on Windows 10 builds that are too old to
            // support lifted, but if this block ran on such a build it would completely break p9rdr, so better safe than sorry.
            if (!wsl::windows::common::helpers::IsWindows11OrAbove())
            {
                auto appIdFlags = ReadDword(HKEY_CLASSES_ROOT, L"AppID\\{DFB65C4C-B34F-435D-AFE9-A86218684AA8}", L"AppIdFlags", 0);
                THROW_HR_IF_MSG(
                    E_UNEXPECTED,
                    WI_IsFlagClear(appIdFlags, APPIDREGFLAGS_AAA_NO_IMPLICIT_ACTIVATE_AS_IU),
                    "TriggerStartPrefix needs update, but AppIdFlags isn't up to date");
            }

            WSL_LOG("Updating TriggerStartPrefix", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

            constexpr wchar_t newValue[] = L"wsl.localhost\0wsl$\0";
            THROW_IF_WIN32_ERROR(RegSetValueEx(key.get(), valueName, 0, REG_MULTI_SZ, (BYTE*)newValue, sizeof(newValue)));
        }
    }
    CATCH_LOG()

    // Make sure the Plan 9 redirector driver is loaded.
    wsl::windows::common::redirector::EnsureRedirectorStarted();
}

HRESULT WslService::OnServiceStarting()
try
{
    ConfigureCrt();

    // Enable contextualized errors
    wsl::windows::common::EnableContextualizedErrors(true);

    // Initialize telemetry.
    WslTraceLoggingInitialize(WslServiceTelemetryProvider, !wsl::shared::OfficialBuild);

    WSL_LOG("Service starting", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    // Don't kill the process on unknown C++ exceptions.
    wil::g_fResultFailFastUnknownExceptions = false;

    wsl::windows::common::security::ApplyProcessMitigationPolicies();

    // Ensure that the OS has support for running lifted WSL.
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_SERVICE_DISABLED), !wsl::windows::common::helpers::IsWslSupportInterfacePresent());

    // Initialize Winsock.
    WSADATA Data;
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &Data));

    // Check if WSL is disabled via policy and set up a registry watcher to watch for changes.
    //
    // N.B. The registry watcher must be created before checking the policy to avoid missing notifications.
    m_watcher = wil::make_registry_watcher(HKEY_LOCAL_MACHINE, ROOT_POLICIES_KEY, true, [this](wil::RegistryChangeKind) {
        try
        {
            EvaluateWslPolicy();
        }
        CATCH_LOG()
    });

    EvaluateWslPolicy();
    return S_OK;
}
CATCH_RETURN()

void WslService::RegisterEventSource()
try
{
    m_eventLog.reset(::RegisterEventSource(nullptr, L"WSL"));
    THROW_LAST_ERROR_IF(!m_eventLog);

    wsl::windows::common::SetEventLog(m_eventLog.get());
}
CATCH_LOG();

HRESULT WslService::ServiceStarted()
{
    m_coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);

    // Cleanup any data from a previously aborted session (crash, power loss, etc).
    LxssIpTables::CleanupRemnants();
    g_networkingReady.SetEvent();

    if constexpr (wsl::shared::OfficialBuild)
    {
        StartCheckingForUpdates();
    }

    return S_OK;
}

void WslService::OnSessionChanged(DWORD eventType, DWORD sessionId)
{
    if (eventType == WTS_SESSION_LOGOFF)
    {
        TerminateSession(sessionId);
    }
}

void WslService::ServiceStopped()
{
    WSL_LOG("Service stopping", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

    // Stop checking for updates.
    m_updateCheckTimer.reset();

    // Stop watching the WSL policy registry keys.
    m_watcher.reset();

    // Terminate all user sessions.
    ClearSessionsAndBlockNewInstances();

    // Disconnect from the LxCore driver.
    if (g_lxcoreInitialized)
    {
        LxssClientUninitialize();
    }

    // There is a potential deadlock if CoUninitialize() is called before the LanguageChangeNotifyThread
    // isn't done initializing. Clearing the COM objects before calling CoUninitialize() works around the issue.
    winrt::clear_factory_cache();

    // Tear down telemetry.
    WslTraceLoggingUninitialize();

    // uninitialize COM. This must be done here because this call can cause cleanups that will be fail
    // if the CRT is shutting down.
    m_coInit.reset();
}

void WslService::StartCheckingForUpdates()
try
{
    const auto lxssKey = OpenLxssMachineKey(KEY_QUERY_VALUE);
    constexpr std::uint64_t c_updateCheckPeriodDefaultMs = 24 * 60 * 60 * 1000; // 24h
    const auto period =
        wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, L"UpdateCheckPeriodMs", c_updateCheckPeriodDefaultMs);

    if (period <= 0)
    {
        WSL_LOG("Update check is disabled via the registry", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

        return;
    }

    m_updateCheckTimer.reset(CreateThreadpoolTimer(WslService::CheckForUpdates, this, nullptr));
    THROW_LAST_ERROR_IF_NULL(m_updateCheckTimer);

    // Check for updates at the configured period, starting one minute after the service starts.
    auto dueTime = wil::filetime::from_int64(static_cast<ULONGLONG>(-1 * wil::filetime_duration::one_minute));
    SetThreadpoolTimer(m_updateCheckTimer.get(), &dueTime, period, 60 * 1000);
}
CATCH_LOG()

void WslService::CheckForUpdates(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_ PVOID Context, _Inout_ PTP_TIMER)
try
{
    auto [version, _] = GetLatestGitHubRelease(false);
    if (ParseWslPackageVersion(version) > ParseWslPackageVersion(TEXT(WSL_PACKAGE_VERSION)))
    {
        WSL_LOG("WSL Package update is available", TraceLoggingLevel(WINEVENT_LEVEL_INFO));

        // Reset the timer since there's no reason to check for updates anymore.
        SetThreadpoolTimer(static_cast<WslService*>(Context)->m_updateCheckTimer.get(), nullptr, 0, 0);

        // Get current release date
        std::wstring currentReleaseCreatedAtDate = GetGitHubReleaseByTag(TEXT(WSL_PACKAGE_VERSION)).created_at;

        std::tm tm = {};
        std::wstring dateTimeFormat = L"%Y-%m-%dT%H:%M:%SZ";
        std::wistringstream ss(currentReleaseCreatedAtDate);
        ss >> std::get_time(&tm, dateTimeFormat.c_str());
        auto tp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

        // If their release of WSL is older than 30 days, then show a notification to update
        if (std::chrono::system_clock::now() - std::chrono::days(30) > tp)
        {
            // Create a notification to inform the user that an update is available
            THROW_IF_FAILED(wsl::windows::common::notifications::DisplayUpdateNotification(version));

            WSL_LOG("WSL Package update notification displayed", TraceLoggingLevel(WINEVENT_LEVEL_INFO));
        }
    }
}
CATCH_LOG()

int __cdecl wmain()
{
    WslService::ProcessMain();
    return 0;
}
