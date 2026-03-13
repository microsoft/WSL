/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssHttpProxy.cpp

Abstract:

    This file contains HTTP proxy related classes and helper functions for proxy queries.

--*/

#include "precomp.h"
#include "LxssHttpProxy.h"

#include <winhttp.h>
#include <notifications.h>

#include "WslCoreNetworkingSupport.h"

using namespace wsl::windows::common;

std::optional<LxssDynamicFunction<decltype(RegisterProxyChangeNotification)>> HttpProxyStateTracker::s_WinHttpRegisterProxyChangeNotification;
std::optional<LxssDynamicFunction<decltype(UnregisterProxyChangeNotification)>> HttpProxyStateTracker::s_WinHttpUnregisterProxyChangeNotification;
std::optional<LxssDynamicFunction<decltype(GetProxySettingsEx)>> HttpProxyStateTracker::s_WinHttpGetProxySettingsEx;
std::optional<LxssDynamicFunction<decltype(GetProxySettingsResultEx)>> HttpProxyStateTracker::s_WinHttpGetProxySettingsResultEx;
std::optional<LxssDynamicFunction<decltype(FreeProxySettingsEx)>> HttpProxyStateTracker::s_WinHttpFreeProxySettingsEx;

// Helpers for using Winhttp's APIs
HRESULT HttpProxyStateTracker::s_LoadWinHttpProxyMethods() noexcept
try
{
    static wil::shared_hmodule winHttpModule;
    static std::once_flag winHttpLoadFlag;

    // Load Winhttp dll only once
    std::call_once(winHttpLoadFlag, [&]() {
        winHttpModule.reset(LoadLibraryEx(c_winhttpModuleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
        THROW_LAST_ERROR_IF(!winHttpModule);
    });

    // Initialize dynamic functions for the WinHttp Proxy OS APIs
    // Not using the throwing constructor
    // as failures should not show up in the Error logs as they can falsely flag failure to start the container
    LxssDynamicFunction<decltype(RegisterProxyChangeNotification)> local_WinHttpRegisterProxyChangeNotification{DynamicFunctionErrorLogs::None};
    LxssDynamicFunction<decltype(UnregisterProxyChangeNotification)> local_WinHttpUnregisterProxyChangeNotification{DynamicFunctionErrorLogs::None};
    LxssDynamicFunction<decltype(GetProxySettingsEx)> local_WinHttpGetProxySettingsEx{DynamicFunctionErrorLogs::None};
    LxssDynamicFunction<decltype(GetProxySettingsResultEx)> local_WinHttpGetProxySettingsResultEx{DynamicFunctionErrorLogs::None};
    LxssDynamicFunction<decltype(FreeProxySettingsEx)> local_WinHttpFreeProxySettingsEx{DynamicFunctionErrorLogs::None};

    // try to load each function - only save if all succeed
    RETURN_IF_FAILED_EXPECTED(
        local_WinHttpRegisterProxyChangeNotification.load(winHttpModule, "WinHttpRegisterProxyChangeNotification"));
    RETURN_IF_FAILED_EXPECTED(
        local_WinHttpUnregisterProxyChangeNotification.load(winHttpModule, "WinHttpUnregisterProxyChangeNotification"));
    RETURN_IF_FAILED_EXPECTED(local_WinHttpGetProxySettingsEx.load(winHttpModule, "WinHttpGetProxySettingsEx"));
    RETURN_IF_FAILED_EXPECTED(local_WinHttpGetProxySettingsResultEx.load(winHttpModule, "WinHttpGetProxySettingsResultEx"));
    RETURN_IF_FAILED_EXPECTED(local_WinHttpFreeProxySettingsEx.load(winHttpModule, "WinHttpFreeProxySettingsEx"));

    s_WinHttpRegisterProxyChangeNotification.emplace(std::move(local_WinHttpRegisterProxyChangeNotification));
    s_WinHttpUnregisterProxyChangeNotification.emplace(std::move(local_WinHttpUnregisterProxyChangeNotification));
    s_WinHttpGetProxySettingsEx.emplace(std::move(local_WinHttpGetProxySettingsEx));
    s_WinHttpGetProxySettingsResultEx.emplace(std::move(local_WinHttpGetProxySettingsResultEx));
    s_WinHttpFreeProxySettingsEx.emplace(std::move(local_WinHttpFreeProxySettingsEx));
    return S_OK;
}
CATCH_RETURN()

void FreeHttpProxySettings(WINHTTP_PROXY_SETTINGS_EX* proxySettings) noexcept
try
{
    THROW_IF_WIN32_ERROR(HttpProxyStateTracker::s_WinHttpFreeProxySettingsEx.value()(WinHttpProxySettingsTypeWsl, proxySettings));
}
CATCH_LOG()

auto CallbackStatusToString(DWORD internetStatus) noexcept
{
    switch (internetStatus)
    {
    case WINHTTP_CALLBACK_STATUS_GETPROXYSETTINGS_COMPLETE:
        return "WINHTTP_CALLBACK_STATUS_GETPROXYSETTINGS_COMPLETE";
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        return "WINHTTP_CALLBACK_STATUS_REQUEST_ERROR";
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        return " WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING";
    default:
        return "Invalid status";
    }
}

// struct to contain proxy specific settings

void LogHttpProxySettings(const HttpProxySettings& settings) noexcept
try
{
    WSL_LOG("OnProxyRequestComplete", TraceLoggingValue(settings.ToString().c_str(), "newProxySettings"));
}
CATCH_LOG()

HttpProxySettings::HttpProxySettings(const WINHTTP_PROXY_SETTINGS_EX& proxySettings)
{
    if (WI_IsFlagSet(proxySettings.ullFlags, WINHTTP_PROXY_TYPE_PROXY))
    {
        Proxy = wsl::shared::string::WideToMultiByte(proxySettings.pcwszProxy);
        SecureProxy = wsl::shared::string::WideToMultiByte(proxySettings.pcwszSecureProxy);

        const auto proxyBypasses = wil::make_range(proxySettings.rgpcwszProxyBypasses, proxySettings.cProxyBypasses);
        std::transform(std::cbegin(proxyBypasses), std::cend(proxyBypasses), std::back_inserter(ProxyBypasses), [](const auto& proxyBypass) {
            return wsl::shared::string::WideToMultiByte(proxyBypass);
        });

        if (!ProxyBypasses.empty())
        {
            ProxyBypassesComma = std::accumulate(
                std::next(std::cbegin(ProxyBypasses)),
                std::cend(ProxyBypasses),
                ProxyBypasses.front(),
                [](std::string previous, const std::string& proxyBypass) { return std::move(previous) + "," + proxyBypass; });
        }
    }

    if (WI_IsFlagSet(proxySettings.ullFlags, WINHTTP_PROXY_TYPE_AUTO_PROXY_URL))
    {
        PacUrl = wsl::shared::string::WideToMultiByte(proxySettings.pcwszAutoconfigUrl);
    }
}

std::string HttpProxySettings::ToString() const
{
    std::ostringstream httpProxySettingsString{};
    httpProxySettingsString << "Proxy: " << Proxy << ", SecureProxy: " << SecureProxy << ", PacUrl: " << PacUrl
                            << ", ProxyBypasses: " << ProxyBypassesComma;
    return httpProxySettingsString.str();
}

bool HttpProxySettings::HasSettingsConfigured() const
{
    return !(Proxy.empty() && SecureProxy.empty() && PacUrl.empty());
}

void CALLBACK HttpProxyStateTracker::s_GetProxySettingsExCallback(
    _In_ HINTERNET resolver, _In_ DWORD_PTR context, _In_ DWORD internetStatus, _In_ PVOID statusInformation, _In_ DWORD) noexcept
try
{
    HttpProxyStateTracker* proxyTracker = reinterpret_cast<HttpProxyStateTracker*>(context);
    const WINHTTP_ASYNC_RESULT* pAsyncResult = static_cast<WINHTTP_ASYNC_RESULT*>(statusInformation);

    if (!proxyTracker)
    {
        return;
    }

    WSL_LOG(
        "s_GetProxySettingsExCallback-CallbackInfo", TraceLoggingValue(CallbackStatusToString(internetStatus), "internetStatus"));

    // This is the last WinHttp callback for this request, received after the request handles were closed.
    if (internetStatus == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
        proxyTracker->m_callbackQueue.submit([proxyTracker] { proxyTracker->RequestClosed(); });
        return;
    }

    DWORD error = ERROR_SUCCESS;
    PCSTR executionStep = "";
    unique_winhttp_proxy_settings proxySettings{};
    switch (internetStatus)
    {
    case WINHTTP_CALLBACK_STATUS_GETPROXYSETTINGS_COMPLETE:
    {
        executionStep = "WinHttpGetProxySettingsResultEx";
        error = s_WinHttpGetProxySettingsResultEx.value()(resolver, &proxySettings);
        break;
    }
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
        executionStep = "CallbackError";
        error = pAsyncResult->dwError;
        break;
    }

    default:
    {
        error = ERROR_INVALID_PARAMETER;
        break;
    }
    }

    if (SUCCEEDED_WIN32(error))
    {
        WSL_LOG(
            "s_GetProxySettingsExCallback-Results",
            TraceLoggingValue(proxySettings.pcwszProxy, "pcwszProxy"),
            TraceLoggingValue(proxySettings.pcwszSecureProxy, "pcwszSecureProxy"),
            TraceLoggingValue(proxySettings.pcwszAutoconfigUrl, "pcwszAutoconfigUrl"),
            TraceLoggingValue(proxySettings.cProxyBypasses, "cProxyBypasses"));
    }
    else
    {
        WSL_LOG(
            "WinHttpGetProxySettingsExCallbackFailed",
            TraceLoggingValue(error, "result"),
            TraceLoggingValue(executionStep, "executionStep"));
    }
    LOG_IF_WIN32_ERROR(error);

    HttpProxySettings newProxySettings{proxySettings};
    proxyTracker->m_callbackQueue.submit([proxyTracker, error, movedProxySettings = std::move(newProxySettings)]() mutable {
        proxyTracker->RequestCompleted(error, std::move(movedProxySettings));
    });
}
CATCH_LOG()

void HttpProxyStateTracker::RequestClosed() noexcept
try
{
    WI_ASSERT(m_callbackQueue.isRunningInQueue());
    const auto requery = m_queryState == QueryState::PendingAndQueueAdditional;
    m_queryState = QueryState::NoQuery;
    if (requery)
    {
        LOG_IF_FAILED(wil::ResultFromException([&] { QueryProxySettingsAsync(); }));
    }

    if (m_queryState == QueryState::NoQuery)
    {
        m_requestFinished.SetEvent();
    }
}
CATCH_LOG()

bool HttpProxyStateTracker::AreProxyStringsIdentical(const HttpProxySettings& newSettings) const
{
    if (!m_proxySettings.has_value())
    {
        return false;
    }
    // note that we do not include the UnsupportedProxyDropReason intentionally here as if that is only change we don't want to trigger a toast
    return (
        newSettings.Proxy == m_proxySettings->Proxy && newSettings.SecureProxy == m_proxySettings->SecureProxy &&
        newSettings.ProxyBypasses == m_proxySettings->ProxyBypasses && newSettings.PacUrl == m_proxySettings->PacUrl);
}

void HttpProxyStateTracker::RequestCompleted(_In_ DWORD error, _In_ HttpProxySettings&& newProxySettings) noexcept
try
{
    WI_ASSERT(m_callbackQueue.isRunningInQueue());
    if (SUCCEEDED_WIN32(error))
    {
        auto dataLock = m_proxySettingsLock.lock();

        FilterProxySettingsByNetworkConfiguration(newProxySettings, m_networkMode);

        if (!AreProxyStringsIdentical(newProxySettings))
        {
            LogHttpProxySettings(newProxySettings);
            m_proxySettings = std::move(newProxySettings);

            // If there was a setting changes, and this is not the initial proxy query, notify the user to restart WSL to get new proxy changes.
            if (m_initialProxyQueryCompleted.is_signaled())
            {
                notifications::DisplayProxyChangeNotification(m_localizedProxyChangeString);
            }
        }
        else
        {
            // note that the DropReason is not included in AreProxyStringsIdentical as we don't want to toast if that is only change,
            // but we still want to make sure the drop reason is updated; otherwise, we risk not reporting the correct drop reason to user
            if (newProxySettings.UnsupportedProxyDropReason != m_proxySettings->UnsupportedProxyDropReason)
            {
                m_proxySettings->UnsupportedProxyDropReason = newProxySettings.UnsupportedProxyDropReason;
            }
        }
        m_initialProxyQueryCompleted.SetEvent();
    }

    // It is guaranteed that after closing the handles, a callback with status WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING
    // will be issued to indicate that the callback has been cleaned up.
    m_resolver.reset();
    m_session.reset();
}
CATCH_LOG()

// Proxy query tracking.
void CALLBACK HttpProxyStateTracker::s_OnProxyChange(_In_ ULONGLONG flags, _In_ void* pContext) noexcept
try
{
    WSL_LOG("OnProxyChange", TraceLoggingValue(flags, "flags"));
    const auto proxyStateTracking = static_cast<HttpProxyStateTracker*>(pContext);

    // Ensure this is a change notification.
    if (WI_IsFlagClear(flags, WINHTTP_PROXY_NOTIFY_CHANGE))
    {
        return;
    }

    proxyStateTracking->m_callbackQueue.submit([proxyStateTracking] { proxyStateTracking->QueryProxySettingsAsync(); });
}
CATCH_LOG()

UnsupportedProxyReason HttpProxyStateTracker::IsUnsupportedProxy(LPCWSTR proxyString, wsl::core::NetworkingMode configuration) noexcept
try
{
    if (!proxyString)
    {
        return UnsupportedProxyReason::Supported;
    }

    URL_COMPONENTS url{};
    url.dwStructSize = sizeof(url); // Required for WinHttpCrackUrl
    url.dwHostNameLength = -1;      // Indicates what we want cracked.
    const DWORD proxyLength = static_cast<DWORD>(wcslen(proxyString));

    if (proxyLength == 0)
    {
        return UnsupportedProxyReason::Supported;
    }

    THROW_IF_WIN32_BOOL_FALSE(WinHttpCrackUrl(proxyString, proxyLength, 0, &url));

    // lpszHostName name will still include <proxy>:port portion of proxy string http://<proxy>:port, but the hostNameLength truncates the port
    std::wstring portRemoved{url.lpszHostName, url.dwHostNameLength};

    // IPv6 strings can come in format http://[<IPv6 address>]:port
    const auto openBracket = portRemoved.find_first_of(L"[");
    if (openBracket != ::std::wstring::npos)
    {
        const auto closeBracket = portRemoved.find_first_of(L"]");
        if (closeBracket == ::std::wstring::npos || (openBracket + 1 >= closeBracket))
        {
            // no other of below checks can contain brackets
            return UnsupportedProxyReason::Supported;
        }
        portRemoved = portRemoved.substr(openBracket + 1, closeBracket - openBracket - 1);
    }

    in6_addr addrV6{};
    PCWSTR pStringEnd{}; // not used by us but still required for *ToAddressW
    if (SUCCEEDED_WIN32(RtlIpv6StringToAddressW(portRemoved.c_str(), &pStringEnd, &addrV6)))
    {
        if (configuration != wsl::core::NetworkingMode::Mirrored)
        {
            return UnsupportedProxyReason::Ipv6NotMirrored; // v6 is only supported in mirrored mode
        }
        if (IN6_IS_ADDR_LOOPBACK(&addrV6))
        {
            return UnsupportedProxyReason::LoopbackV6; // v6 loopback is not supported in any network configuration
        }
        return UnsupportedProxyReason::Supported;
    }

    // v4 loopback is only supported in mirrored mode
    if (configuration != wsl::core::NetworkingMode::Mirrored)
    {
        in_addr addrV4{};
        if (SUCCEEDED_WIN32(RtlIpv4StringToAddressW(portRemoved.c_str(), true, &pStringEnd, &addrV4)))
        {
            if (IN4_IS_ADDR_LOOPBACK(&addrV4))
            {
                return UnsupportedProxyReason::LoopbackNotMirrored;
            }
            return UnsupportedProxyReason::Supported;
        }

        if (wsl::shared::string::IsEqual(portRemoved, c_loopback, true) || wsl::shared::string::IsEqual(portRemoved, c_localhost, true))
        {
            return UnsupportedProxyReason::LoopbackNotMirrored;
        }

        DWORD size = 0;
        std::wstring computerName{};

        if (!GetComputerNameW(nullptr, &size))
        {
            const DWORD err = GetLastError();
            THROW_WIN32_IF(err, err != ERROR_BUFFER_OVERFLOW);
        }
        computerName.resize(size, L'\0');

        THROW_IF_WIN32_BOOL_FALSE(GetComputerNameW(computerName.data(), &size));

        // remove any embedded null characters
        const auto offset = computerName.find_first_of(L'\0');
        if (offset != ::std::wstring::npos)
        {
            computerName.resize(offset);
        }

        if (wsl::shared::string::IsEqual(computerName, portRemoved, true))
        {
            return UnsupportedProxyReason::LoopbackNotMirrored;
        }
    }
    return UnsupportedProxyReason::Supported;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return UnsupportedProxyReason::UnsupportedError;
}

void HttpProxyStateTracker::FilterProxySettingsByNetworkConfiguration(HttpProxySettings& settings, wsl::core::NetworkingMode mode) noexcept
try
{
    const auto proxySupportState = IsUnsupportedProxy(wsl::shared::string::MultiByteToWide(settings.Proxy).c_str(), mode);
    const auto secureProxySupportState = IsUnsupportedProxy(wsl::shared::string::MultiByteToWide(settings.SecureProxy).c_str(), mode);
    if (proxySupportState != UnsupportedProxyReason::Supported)
    {
        settings.Proxy.clear();
        settings.UnsupportedProxyDropReason = proxySupportState;
    }

    if (secureProxySupportState != UnsupportedProxyReason::Supported)
    {
        settings.SecureProxy.clear();
        settings.UnsupportedProxyDropReason = secureProxySupportState;
    }

    // If we now have no proxy settings configured, we should clear the proxy bypasses too.
    // Note that if one setting was cleared, but other was not, the proxy bypasses are still valid.
    if (settings.Proxy.empty() && settings.SecureProxy.empty())
    {
        settings.ProxyBypasses.clear();
        settings.ProxyBypassesComma.clear();
    }

    if (proxySupportState != UnsupportedProxyReason::Supported || secureProxySupportState != UnsupportedProxyReason::Supported)
    {
        WSL_LOG(
            "AutoProxy-DropUnsupportedSetting",
            TraceLoggingValue(wsl::core::ToString(mode), "InvalidNetworkConfiguration"),
            TraceLoggingValue(ToString(proxySupportState), "DropHttpProxySetting"),
            TraceLoggingValue(ToString(secureProxySupportState), "DropHttpsProxySetting"));
    }
}
CATCH_LOG()

void HttpProxyStateTracker::QueryProxySettingsAsync()
{
    PCSTR executionStep = "";
    try
    {
        WI_ASSERT(m_callbackQueue.isRunningInQueue());
        if (m_queryState == QueryState::PendingAndQueueAdditional)
        {
            return;
        }

        if (m_queryState == QueryState::Pending)
        {
            m_queryState = QueryState::PendingAndQueueAdditional;
            WSL_LOG("Run another http proxy query after current completes");
            return;
        }

        executionStep = "impersonate_token";
        auto runAsUser = wil::impersonate_token(m_userToken.get());

        executionStep = "WinHttpOpen";
        //
        // Open session and setup resolver handle
        //
        wil::unique_winhttp_hinternet session(WinHttpOpen(
            nullptr, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
        THROW_LAST_ERROR_IF(!session.is_valid());

        executionStep = "WinHttpCreateProxyResolver";
        wil::unique_winhttp_hinternet resolver{};
        THROW_IF_WIN32_ERROR(WinHttpCreateProxyResolver(session.get(), &resolver));

        executionStep = "WinHttpSetStatusCallback";
        // We need to set flag WINHTTP_CALLBACK_FLAG_HANDLES in order to get the WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING
        // status callback when a handle is closed.
        // Using those flags will always result in 2 callbacks, 1 on success/failure, 1 when closing the requests.
        // Without these we risk race conditions while deconstructing the ProxyTracker.
        THROW_LAST_ERROR_IF(
            WinHttpSetStatusCallback(
                resolver.get(),
                s_GetProxySettingsExCallback,
                WINHTTP_CALLBACK_STATUS_GETPROXYSETTINGS_COMPLETE | WINHTTP_CALLBACK_STATUS_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES,
                0) == WINHTTP_INVALID_STATUS_CALLBACK);

        WINHTTP_PROXY_SETTINGS_PARAM ProxySettingsParam{0, nullptr, nullptr};

        executionStep = "WinHttpGetProxySettingsEx";
        // Query the proxy settings
        const DWORD dwError = s_WinHttpGetProxySettingsEx.value()(
            resolver.get(), WinHttpProxySettingsTypeWsl, &ProxySettingsParam, reinterpret_cast<DWORD_PTR>(this));

        if (dwError != ERROR_IO_PENDING && dwError != ERROR_SUCCESS)
        {
            THROW_WIN32(dwError);
        }

        // Transfer ownership of HTTP handles and track request
        m_resolver = std::move(resolver);
        m_session = std::move(session);
        m_requestFinished.ResetEvent();
        m_queryState = QueryState::Pending;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        WSL_LOG("QueryProxySettingsFailed", TraceLoggingHResult(hr, "result"), TraceLoggingValue(executionStep, "executionStep"));

        throw;
    }
}

HttpProxyStateTracker::HttpProxyStateTracker(int ProxyTimeout, HANDLE UserToken, wsl::core::NetworkingMode mode) :
    m_networkMode{mode},
    m_initialQueryTimeout{ProxyTimeout},
    m_localizedProxyChangeString{wsl::shared::Localization::MessageHttpProxyChangeDetected()}
{
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateTokenEx(UserToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, &m_userToken));
    m_callbackQueue.submit([this] { QueryProxySettingsAsync(); });
    THROW_IF_WIN32_ERROR(s_WinHttpRegisterProxyChangeNotification.value()(WINHTTP_PROXY_NOTIFY_CHANGE, s_OnProxyChange, this, &m_proxyRegistrationHandle));
}

HttpProxyStateTracker::~HttpProxyStateTracker()
{
    // cancel Proxy change notifications, preventing queries from being triggered.
    if (m_proxyRegistrationHandle != nullptr)
    {
        try
        {
            THROW_IF_WIN32_ERROR(s_WinHttpUnregisterProxyChangeNotification.value()(m_proxyRegistrationHandle));
        }
        CATCH_LOG()
    }
    // It is guaranteed that after closing the handles, a callback with status WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING
    // will be issued and it will be the last callback for this request, making it safe to delete the ProxyStateTracker.
    m_resolver.reset();
    m_session.reset();

    // Wait for all requests to complete. At this point no new requests can be started since we unregistered for proxy change
    // notifications. Without this we risk racing proxy callbacks and them calling into a cleaned up ProxyStateTracker.
    m_requestFinished.wait();

    // Cancel all pending work in the queue and prevent additional requests
    m_callbackQueue.cancel();
}

std::optional<HttpProxySettings> HttpProxyStateTracker::WaitForInitialProxySettings()
{
    m_initialProxyQueryCompleted.wait(m_initialQueryTimeout);
    auto lock = m_proxySettingsLock.lock();
    return m_proxySettings;
}

void HttpProxyStateTracker::ConfigureNetworkingMode(wsl::core::NetworkingMode mode) noexcept
{
    auto lock = m_proxySettingsLock.lock();
    // if we fall back to NAT mode need to strip bad settings
    if (m_proxySettings.has_value() && mode != m_networkMode)
    {
        FilterProxySettingsByNetworkConfiguration(m_proxySettings.value(), mode);
    }
    m_networkMode = mode;
}
