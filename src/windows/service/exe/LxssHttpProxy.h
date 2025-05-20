/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssHttpProxy.h

Abstract:

    This file contains HTTP proxy related classes and helper functions for proxy queries. Based on implementation in WSA.

--*/

#pragma once

#include <windows.h>
#include <winhttp.h>

#include <wil/resource.h>

#include <string>
#include <vector>
#include <optional>

#include "WslCoreConfig.h"
#include "WslCoreMessageQueue.h"

static constexpr auto c_winhttpModuleName = L"Winhttp.dll";
static constexpr auto c_httpProxyLower = "http_proxy";
static constexpr auto c_httpProxyUpper = "HTTP_PROXY";
static constexpr auto c_httpsProxyLower = "https_proxy";
static constexpr auto c_httpsProxyUpper = "HTTPS_PROXY";
static constexpr auto c_proxyBypassLower = "no_proxy";
static constexpr auto c_proxyBypassUpper = "NO_PROXY";
static constexpr auto c_pacProxy = "WSL_PAC_URL";
static constexpr auto c_loopback = L"loopback";
static constexpr auto c_localhost = L"localhost";

void FreeHttpProxySettings(WINHTTP_PROXY_SETTINGS_EX* proxySettings) noexcept;
using unique_winhttp_proxy_settings = wil::unique_struct<WINHTTP_PROXY_SETTINGS_EX, decltype(&FreeHttpProxySettings), FreeHttpProxySettings>;

// Function declarations used for dynamically loading in Winhttp APIs.
WINHTTPAPI
DWORD
WINAPI
RegisterProxyChangeNotification(_In_ ULONGLONG ullFlags, _In_ WINHTTP_PROXY_CHANGE_CALLBACK pfnCallback, _In_ PVOID pvContext, _Out_ WINHTTP_PROXY_CHANGE_REGISTRATION_HANDLE* hRegistration);

WINHTTPAPI
DWORD
WINAPI
UnregisterProxyChangeNotification(_In_ WINHTTP_PROXY_CHANGE_REGISTRATION_HANDLE hRegistration);

WINHTTPAPI
DWORD
WINAPI
GetProxySettingsEx(
    _In_ HINTERNET hResolver,
    _In_ WINHTTP_PROXY_SETTINGS_TYPE ProxySettingsType,
    _In_opt_ PWINHTTP_PROXY_SETTINGS_PARAM pProxySettingsParam,
    _In_opt_ DWORD_PTR pContext);

WINHTTPAPI
DWORD
WINAPI
GetProxySettingsResultEx(_In_ HINTERNET hResolver, _Out_ PVOID pProxySettingsEx);

WINHTTPAPI
DWORD
WINAPI
FreeProxySettingsEx(_In_ WINHTTP_PROXY_SETTINGS_TYPE ProxySettingsType, _In_ PVOID pProxySettingsEx);

enum class UnsupportedProxyReason
{
    Supported,
    LoopbackNotMirrored,
    Ipv6NotMirrored,
    LoopbackV6,
    UnsupportedError
};

constexpr auto ToString(UnsupportedProxyReason config) noexcept
{
    switch (config)
    {
    case UnsupportedProxyReason::Supported:
        return "Supported";
    case UnsupportedProxyReason::LoopbackNotMirrored:
        return "LoopbackNotMirrored";
    case UnsupportedProxyReason::Ipv6NotMirrored:
        return "Ipv6NotMirrored";
    case UnsupportedProxyReason::LoopbackV6:
        return "LoopbackV6";
    case UnsupportedProxyReason::UnsupportedError:
        return "UnsupportedError";
    default:
        return "<unknown UnsupportedProxyReason>";
    }
}

struct HttpProxySettings
{
    HttpProxySettings() = default;
    HttpProxySettings(const WINHTTP_PROXY_SETTINGS_EX& ProxySettings);
    HttpProxySettings(const HttpProxySettings&) = default;
    HttpProxySettings(HttpProxySettings&&) = default;
    HttpProxySettings& operator=(const HttpProxySettings&) = default;
    HttpProxySettings& operator=(HttpProxySettings&&) = default;

    std::string PacUrl{};
    std::string Proxy{};
    std::string SecureProxy{};
    std::vector<std::string> ProxyBypasses{};
    std::string ProxyBypassesComma{};
    UnsupportedProxyReason UnsupportedProxyDropReason = UnsupportedProxyReason::Supported;

    std::string ToString() const;
    bool HasSettingsConfigured() const;
};

class HttpProxyStateTracker
{
    enum class QueryState
    {
        NoQuery,
        Pending,
        PendingAndQueueAdditional
    };

public:
    HttpProxyStateTracker(int ProxyTimeout, HANDLE UserToken, wsl::core::NetworkingMode configuration);
    ~HttpProxyStateTracker();

    HttpProxyStateTracker(const HttpProxyStateTracker&) = delete;
    HttpProxyStateTracker(HttpProxyStateTracker&&) = delete;
    HttpProxyStateTracker& operator=(const HttpProxyStateTracker&) = delete;
    HttpProxyStateTracker& operator=(HttpProxyStateTracker&&) = delete;

    /// <summary>
    /// If no proxy queries have completed, wait for timeout for result.
    /// Otherwise, return the proxy settings.
    /// </summary>
    std::optional<HttpProxySettings> WaitForInitialProxySettings();

    /// <summary>
    /// This needs to be called after the VM is created so actual selected configuration is set.
    /// </summary>
    void ConfigureNetworkingMode(wsl::core::NetworkingMode mode) noexcept;

    /// <summary>
    /// Loads necessary WinHttpProxy APIs into static dynamic functions from DLL if they exist.
    /// </summary>
    static HRESULT s_LoadWinHttpProxyMethods() noexcept;

    /// Static dynamic function for loading in necessary WinHttpProxy APIs
    static std::optional<LxssDynamicFunction<decltype(FreeProxySettingsEx)>> s_WinHttpFreeProxySettingsEx;

private:
    /// <summary>
    /// Invoked via WslCoreMessageQueue. Uses WinHttpProxy APIs to start proxy query.
    /// </summary>
    void QueryProxySettingsAsync();

    /// <summary>
    /// Invoked via WslCoreMessageQueue when a proxy request completes.
    /// </summary>
    /// <param name="error"> Error status of request. </param>
    /// <param name="proxySettings"> The current proxy settings. </param>
    void RequestCompleted(_In_ DWORD error, _In_ HttpProxySettings&& newProxySettings) noexcept;

    /// <summary>
    ///  Invoked via WslCoreMessageQueue when a proxy request closes.
    /// </summary>
    void RequestClosed() noexcept;

    /// <summary>
    /// Checks if two proxy settings are identical.
    /// </summary>
    bool AreProxyStringsIdentical(const HttpProxySettings& newSettings) const;

    /// <summary>
    /// Memory barrier for reading/writing to m_proxySettings.
    /// </summary>
    wil::critical_section m_proxySettingsLock{};

    /// <summary>
    /// Current http proxy settings. If no queries have completed it is std::nullopt.
    /// </summary>
    _Guarded_by_(m_proxySettingsLock) std::optional<HttpProxySettings> m_proxySettings {};

    /// <summary>
    /// Current network mode. Used to determine some cases when we should send toast notification.
    /// </summary>
    _Guarded_by_(m_proxySettingsLock) wsl::core::NetworkingMode m_networkMode = wsl::core::NetworkingMode::Nat;

    /// <summary>
    /// Indicates if we need to start another query after current one.
    /// </summary>
    QueryState m_queryState{QueryState::NoQuery};

    /// <summary>
    /// Used to impersonate user, as it is required for the proxy queries to run as the user; otherwise, the results will be incorrect.
    /// </summary>
    wil::unique_handle m_userToken{};

    /// <summary>
    /// Handle for tracking http proxy setting changes.
    /// </summary>
    WINHTTP_PROXY_CHANGE_REGISTRATION_HANDLE m_proxyRegistrationHandle{};

    /// <summary>
    /// Amount of time WSL will wait for proxy settings if no proxy settings have been detected by time we attempt to launch process.
    /// </summary>
    const int m_initialQueryTimeout = 1000;

    /// <summary>
    /// We resolve and store the localized proxy change string for notifications to this object, as we can't resolve it in callback from proxy query.
    /// </summary>
    const std::wstring m_localizedProxyChangeString{};

    /// <summary>
    /// Event that is set when m_proxySettings has a value.
    /// </summary>
    wil::slim_event_manual_reset m_initialProxyQueryCompleted{false};

    /// <summary>
    /// Event that is set when all tracked requests have completed.
    /// </summary>
    wil::slim_event_manual_reset m_requestFinished{true};

    // Handles associated with the request
    wil::unique_winhttp_hinternet m_session{};
    wil::unique_winhttp_hinternet m_resolver{};

    /// <summary>
    /// Single-threaded queue to trigger work from winhttp callbacks.
    /// </summary>
    wsl::core::WslCoreMessageQueue m_callbackQueue{};

    /// Static dynamic functions for loading in necessary WinHttpProxy APIs
    static std::optional<LxssDynamicFunction<decltype(GetProxySettingsEx)>> s_WinHttpGetProxySettingsEx;
    static std::optional<LxssDynamicFunction<decltype(GetProxySettingsResultEx)>> s_WinHttpGetProxySettingsResultEx;
    static std::optional<LxssDynamicFunction<decltype(RegisterProxyChangeNotification)>> s_WinHttpRegisterProxyChangeNotification;
    static std::optional<LxssDynamicFunction<decltype(UnregisterProxyChangeNotification)>> s_WinHttpUnregisterProxyChangeNotification;

    /// <summary>
    /// Callback that returns results from proxy queries.
    /// </summary>
    /// <param name="resolver"> Resolver associated with this callback. </param>
    /// <param name="context"> Pointer to ProxyCallbackContext associated with callback. </param>
    /// <param name="internetStatus"> Status of callback. </param>
    /// <param name="statusInformation"> Pointer to WINHTTP_ASYNC_RESULT used for error info. </param>
    static void CALLBACK s_GetProxySettingsExCallback(
        _In_ HINTERNET resolver, _In_ DWORD_PTR context, _In_ DWORD internetStatus, _In_ PVOID statusInformation, _In_ DWORD) noexcept;

    /// <summary>
    /// Callback that notifies that an Http proxy setting change has been detected.
    /// </summary>
    /// <param name="flags"> Flags used to verify the type of callback received. </param>
    /// <param name="pContext"> Pointer to ProxyStateTracker associated with callback. </param>
    static void CALLBACK s_OnProxyChange(_In_ ULONGLONG flags, _In_ void* pContext) noexcept;

    /// <summary>
    /// Determines if a proxy setting string is localhost.
    /// </summary>
    /// <param name="proxyString"> ProxyString to check if it is localhost. </param>
    /// <param name="configuration"> Current network configuration. </param>
    static UnsupportedProxyReason IsUnsupportedProxy(LPCWSTR proxyString, wsl::core::NetworkingMode mode) noexcept;

    /// <summary>
    /// Remove invalid proxy configurations depending on network mode.
    /// </summary>
    /// <param name="settings"> HttpProxySettings to be filtered. </param>
    /// <param name="configuration"> Current network configuration. </param>
    static void FilterProxySettingsByNetworkConfiguration(HttpProxySettings& settings, wsl::core::NetworkingMode mode) noexcept;
};
