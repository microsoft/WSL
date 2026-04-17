/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslTelemetry.h

Abstract:

    This file contains tracing function declarations.

--*/

#pragma once

#include <windows.h>
#include <winmeta.h>
#include <evntprov.h>
#include <wil/resource.h>
#include <type_traits>
#include <TraceLoggingProvider.h>
#include <TraceLoggingActivity.h>
#include "traceloggingconfig.h"

#ifdef __cplusplus
extern "C" {
#endif
TRACELOGGING_DECLARE_PROVIDER(LxssTelemetryProvider);
TRACELOGGING_DECLARE_PROVIDER(WslServiceTelemetryProvider);
#ifdef __cplusplus
}
#endif

extern TraceLoggingHProvider g_hTraceLoggingProvider;

// Initialize tracelogging for the binary.
//     DisableTelemetryByDefault - In scenarios where there are no active users, assume telemetry is not allowed.
//                                 This is used in conjunction with WslTraceLoggingClient to represent active clients.
//     ForceDropPII - Make sure all events are tagged with MICROSOFT_EVENTTAG_DROP_PII.
//                    WARNING: This will mean backend tools like devicedrill will not work. This should be a temporary setting.
void WslTraceLoggingInitialize(_In_ TraceLoggingHProvider, _In_ BOOLEAN DisableTelemetryByDefault, _In_ std::optional<TLG_PENABLECALLBACK> callback = {});

void WslTraceLoggingUninitialize();

bool WslTraceLoggingShouldDisableTelemetry() noexcept;

#ifdef __cplusplus
class WslTraceLoggingClient
{
public:
    WslTraceLoggingClient(bool TelemetryEnabled);
    ~WslTraceLoggingClient();

private:
    bool m_clientTelemetryEnabled;
};

#endif

#define WSL_LOG(Name, ...) TraceLoggingWrite(g_hTraceLoggingProvider, Name, __VA_ARGS__)

#define WSL_LOG_DEBUG(Name, ...) \
    if constexpr (wsl::shared::Debug) \
    { \
        WSL_LOG(Name, __VA_ARGS__); \
    }

#define WSL_LOG_TELEMETRY(Name, Tag, ...) \
    TraceLoggingWrite( \
        g_hTraceLoggingProvider, \
        Name, \
        TraceLoggingValue(WSL_PACKAGE_VERSION, "wslVersion"), \
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), \
        TelemetryPrivacyDataTag(Tag), \
        __VA_ARGS__);

#ifdef __cplusplus

// RAII helper that ensures a paired End telemetry event is always emitted,
// including when the enclosing scope exits via exception. On destruction:
//   - If the stack is unwinding due to an uncaught exception, the HRESULT
//     passed to the end lambda is populated from wil::ResultFromCaughtException().
//   - Otherwise, the HRESULT passed is S_OK.
//
// This lets the backend state machine distinguish between three outcomes:
//   1. Normal success     -> End event emitted with hr == S_OK
//   2. Exception / error  -> End event emitted with hr != S_OK
//   3. Real hang          -> No End event ever emitted
//
// Usage (caller emits Begin first, scope emits End on destruction):
//   WSL_LOG_TELEMETRY("XxxBegin", PDT_ProductAndServicePerformance, ...);
//   auto scope = WslTelemetryScope([&](HRESULT hr) {
//       WSL_LOG_TELEMETRY("XxxEnd", PDT_ProductAndServicePerformance,
//                          ..., TraceLoggingHResult(hr, "hr"));
//   });
//   // ...work, may throw...
//   // On scope exit (success or exception) End event fires with populated hr.
template <typename TEndEmit>
class WslTelemetryScope
{
public:
    explicit WslTelemetryScope(TEndEmit endEmit) :
        m_endEmit(std::move(endEmit)), m_uncaughtOnEntry(std::uncaught_exceptions())
    {
    }

    ~WslTelemetryScope() noexcept
    {
        HRESULT hr = S_OK;
        if (std::uncaught_exceptions() > m_uncaughtOnEntry)
        {
            hr = wil::ResultFromCaughtException();
        }

        try
        {
            m_endEmit(hr);
        }
        CATCH_LOG();
    }

    WslTelemetryScope(const WslTelemetryScope&) = delete;
    WslTelemetryScope& operator=(const WslTelemetryScope&) = delete;
    WslTelemetryScope(WslTelemetryScope&&) = delete;
    WslTelemetryScope& operator=(WslTelemetryScope&&) = delete;

private:
    TEndEmit m_endEmit;
    int m_uncaughtOnEntry;
};

// Deduction guide so callers do not need to spell out the lambda type.
template <typename TEndEmit>
WslTelemetryScope(TEndEmit) -> WslTelemetryScope<TEndEmit>;

#endif // __cplusplus
