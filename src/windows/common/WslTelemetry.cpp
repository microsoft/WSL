/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslTelemetry.cpp

Abstract:

    This file contains tracing function definitions.

--*/

#include "wslservice.h"
#include "ExecutionContext.h"
#include "WslTelemetry.h"

#define WSL_TELEMETRY_KEYWORDS (MICROSOFT_KEYWORD_CRITICAL_DATA | MICROSOFT_KEYWORD_MEASURES | MICROSOFT_KEYWORD_TELEMETRY)

TRACELOGGING_DEFINE_PROVIDER(
    LxssTelemetryProvider,
    "Microsoft.Windows.Subsystem.Lxss",
    // {d90b9468-67f0-5b3b-42cc-82ac81ffd960}
    (0xd90b9468, 0x67f0, 0x5b3b, 0x42, 0xcc, 0x82, 0xac, 0x81, 0xff, 0xd9, 0x60),
    TraceLoggingOptionMicrosoftTelemetry());

TRACELOGGING_DEFINE_PROVIDER(
    WslServiceTelemetryProvider,
    "Microsoft.Windows.Lxss.Manager",
    // {b99cdb5a-039c-5046-e672-1a0de0a40211}
    (0xb99cdb5a, 0x039c, 0x5046, 0xe6, 0x72, 0x1a, 0x0d, 0xe0, 0xa4, 0x02, 0x11),
    TraceLoggingOptionMicrosoftTelemetry());

#ifdef DEBUG
#define HRESULT_STRING_VALUE \
    , TraceLoggingValue(wsl::windows::common::wslutil::ErrorCodeToString(failure->hr).c_str(), "HRESULTString")
#else
#define HRESULT_STRING_VALUE
#endif

TraceLoggingHProvider g_hTraceLoggingProvider;

namespace WslTraceLoggingInternal {
static bool g_disableTelemetryByDefault = true;
static std::atomic<long> g_ClientsWithTelemetryEnabled = 0;
static std::atomic<long> g_ClientsWithTelemetryDisabled = 0;
} // namespace WslTraceLoggingInternal

WslTraceLoggingClient::WslTraceLoggingClient(bool TelemetryEnabled) : m_clientTelemetryEnabled(TelemetryEnabled)
{
    if (m_clientTelemetryEnabled)
    {
        ++WslTraceLoggingInternal::g_ClientsWithTelemetryEnabled;
    }
    else
    {
        ++WslTraceLoggingInternal::g_ClientsWithTelemetryDisabled;
    }
}

WslTraceLoggingClient::~WslTraceLoggingClient()
{
    if (m_clientTelemetryEnabled)
    {
        --WslTraceLoggingInternal::g_ClientsWithTelemetryEnabled;
        WI_ASSERT(WslTraceLoggingInternal::g_ClientsWithTelemetryEnabled >= 0);
    }
    else
    {
        --WslTraceLoggingInternal::g_ClientsWithTelemetryDisabled;
        WI_ASSERT(WslTraceLoggingInternal::g_ClientsWithTelemetryDisabled >= 0);
    }
}

void WslTraceLoggingInitialize(_In_ const TraceLoggingHProvider provider, _In_ BOOLEAN DisableTelemetryByDefault, _In_ std::optional<TLG_PENABLECALLBACK> callback)
{
    WI_ASSERT(!g_hTraceLoggingProvider);

    WslTraceLoggingInternal::g_disableTelemetryByDefault = (DisableTelemetryByDefault != FALSE);
    g_hTraceLoggingProvider = provider;

    if (callback.has_value())
    {
        TraceLoggingRegisterEx(g_hTraceLoggingProvider, callback.value(), nullptr);
    }
    else
    {
        TraceLoggingRegister(g_hTraceLoggingProvider);
    }

    wil::g_pfnResultLoggingCallback = [](wil::FailureInfo* failure, PWSTR, size_t) WI_NOEXCEPT {
        if (failure->type == wil::FailureType::Exception || failure->type == wil::FailureType::Return)
        {
            wsl::windows::common::ExecutionContext::CollectError(failure->hr);
        }

        if (g_hTraceLoggingProvider == LxssTelemetryProvider)
        {
            // Do not log telemetry for the internal invalid command line argument
            // error.

            if (failure->hr == WSL_E_INVALID_USAGE)
            {
                return;
            }

            switch (failure->type)
            {
            case wil::FailureType::Exception:
            case wil::FailureType::FailFast:
                WSL_LOG(
                    "LxssException",
                    TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    TraceLoggingValue(failure->pszFile, "File"),
                    TraceLoggingValue(failure->pszFunction, "FunctionName"),
                    TraceLoggingValue(failure->uLineNumber, "Line number"),
                    TraceLoggingValue(static_cast<DWORD>(failure->type), "Type"),
                    TraceLoggingHexUInt32(failure->hr, "HRESULT"),
                    TraceLoggingValue(failure->pszMessage, "Message"),
                    TraceLoggingValue(failure->pszCode, "Code") HRESULT_STRING_VALUE);

                break;

            default:
                WSL_LOG(
                    "LxssVerboseLog",
                    TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                    TraceLoggingValue(failure->pszFile, "File"),
                    TraceLoggingValue(failure->pszFunction, "FunctionName"),
                    TraceLoggingValue(failure->uLineNumber, "Line number"),
                    TraceLoggingValue(static_cast<DWORD>(failure->type), "Type"),
                    TraceLoggingHexUInt32(failure->hr, "HRESULT"),
                    TraceLoggingValue(failure->pszMessage, "Message"),
                    TraceLoggingValue(failure->pszCode, "Code") HRESULT_STRING_VALUE);

                break;
            }
        }
        else
        {
            switch (failure->type)
            {
            case wil::FailureType::Exception:
            case wil::FailureType::FailFast:
                WSL_LOG(
                    "Error",
                    TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                    TraceLoggingValue(failure->pszFile, "file"),
                    TraceLoggingValue(failure->uLineNumber, "linenumber"),
                    TraceLoggingValue(static_cast<DWORD>(failure->type), "type"),
                    TraceLoggingValue(failure->cFailureCount, "failurecount"),
                    TraceLoggingValue(GetCurrentThreadId(), "threadid"),
                    TraceLoggingHexUInt32(failure->hr, "hr"),
                    TraceLoggingValue(failure->pszMessage, "message"),
                    TraceLoggingValue(failure->pszCode, "code"),
                    TraceLoggingValue(failure->pszFunction, "function") HRESULT_STRING_VALUE);

                break;

            default:
                WSL_LOG(
                    "VerboseLog",
                    TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                    TraceLoggingValue(failure->pszFile, "file"),
                    TraceLoggingValue(failure->uLineNumber, "linenumber"),
                    TraceLoggingValue(failure->cFailureCount, "failurecount"),
                    TraceLoggingValue(GetCurrentThreadId(), "threadid"),
                    TraceLoggingHexUInt32(failure->hr, "hr"),
                    TraceLoggingValue(failure->pszMessage, "message"),
                    TraceLoggingValue(failure->pszCode, "code"),
                    TraceLoggingValue(failure->pszFunction, "function") HRESULT_STRING_VALUE);

                break;
            }
        }
    };
}

void WslTraceLoggingUninitialize()
{
    wil::g_pfnResultLoggingCallback = nullptr;
    TraceLoggingUnregister(g_hTraceLoggingProvider);
}

bool WslTraceLoggingShouldDisableTelemetry() noexcept
{
    return (WslTraceLoggingInternal::g_ClientsWithTelemetryDisabled > 0) ||
           (WslTraceLoggingInternal::g_disableTelemetryByDefault && (WslTraceLoggingInternal::g_ClientsWithTelemetryEnabled == 0));
}