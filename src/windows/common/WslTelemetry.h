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
#include <objbase.h>
#include <wil/resource.h>
#include <exception>
#include <type_traits>
#include <utility>
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

// Emit the Start (opcode=1) event for a TraceLogging Activity. The ActivityId is a GUID that
// correlates this Start with the matching Stop event; the backend joins on it instead of the
// event name (Start and Stop share the same name, distinguished only by Opcode).
#define WSL_LOG_TELEMETRY_ACTIVITY_START(ActivityIdGuid, Name, Tag, ...) \
    TraceLoggingWriteActivity( \
        g_hTraceLoggingProvider, \
        Name, \
        &(ActivityIdGuid), \
        nullptr, \
        TraceLoggingOpcode(WINEVENT_OPCODE_START), \
        TraceLoggingValue(WSL_PACKAGE_VERSION, "wslVersion"), \
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), \
        TelemetryPrivacyDataTag(Tag), \
        __VA_ARGS__);

// Emit the Stop (opcode=2) event for a TraceLogging Activity. Must use the same ActivityId and
// event Name as the matching Start.
#define WSL_LOG_TELEMETRY_ACTIVITY_STOP(ActivityIdGuid, Name, Tag, ...) \
    TraceLoggingWriteActivity( \
        g_hTraceLoggingProvider, \
        Name, \
        &(ActivityIdGuid), \
        nullptr, \
        TraceLoggingOpcode(WINEVENT_OPCODE_STOP), \
        TraceLoggingValue(WSL_PACKAGE_VERSION, "wslVersion"), \
        TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES), \
        TelemetryPrivacyDataTag(Tag), \
        __VA_ARGS__);

#ifdef __cplusplus

// RAII helper for TraceLogging Activities. Allocates an ActivityId on construction and invokes
// the Stop emitter on destruction with:
//   - hr == S_OK when the scope exits normally, or
//   - hr == E_FAIL when the scope is unwinding due to an uncaught exception.
//
// Used together with WSL_LOG_TELEMETRY_ACTIVITY_START / _STOP so Start/Stop share the same
// ActivityId and Opcode-based correlation carries the backend join without an application-level
// pairId field.
//
// Why E_FAIL on unwind instead of wil::ResultFromCaughtException(): RFCE must be called from
// inside a catch(...) handler; invoking it from a destructor during unwinding is undefined and
// can std::terminate. The outer CreateInstance scope captures the specific HRESULT via the
// idiomatic "result=E_UNEXPECTED; try { ...; result=S_OK; } catch(...) { result=RFCE(); throw; }"
// pattern (see LxssUserSession.cpp _CreateInstance). This scope only surfaces "did this step
// unwind?" for phase attribution.
//
// The backend state machine distinguishes three outcomes per inner step:
//   1. Normal success          -> Stop emitted with hr == S_OK
//   2. Step failed / unwound   -> Stop emitted with hr == E_FAIL
//   3. Real silent hang        -> No Stop ever emitted (feeds timeout categorization)
//
// Usage:
//   WslTelemetryActivityScope activity([&](const GUID& id, HRESULT hr) {
//       WSL_LOG_TELEMETRY_ACTIVITY_STOP(id, "Xxx", PDT_ProductAndServicePerformance,
//           TraceLoggingValue(vmId, "vmId"),
//           TraceLoggingHResult(hr, "hr"));
//   });
//   WSL_LOG_TELEMETRY_ACTIVITY_START(activity.ActivityId(), "Xxx",
//       PDT_ProductAndServicePerformance,
//       TraceLoggingValue(vmId, "vmId"));
//   // ...work, may throw...
template <typename TStopEmit>
class WslTelemetryActivityScope
{
public:
    explicit WslTelemetryActivityScope(TStopEmit stopEmit) :
        m_stopEmit(std::move(stopEmit)), m_uncaughtOnEntry(std::uncaught_exceptions())
    {
        // Throw on failure rather than falling back to GUID_NULL: if we can't allocate a unique
        // ActivityId, two unrelated activities on the same thread would both use GUID_NULL and
        // the backend could not distinguish their Start/Stop pairs, silently degrading the
        // "unmatched Start => timeout" classification. Callers already propagate exceptions.
        THROW_IF_FAILED(CoCreateGuid(&m_activityId));
    }

    ~WslTelemetryActivityScope() noexcept
    {
        const HRESULT hr = (std::uncaught_exceptions() > m_uncaughtOnEntry) ? E_FAIL : S_OK;

        try
        {
            m_stopEmit(m_activityId, hr);
        }
        CATCH_LOG();
    }

    const GUID& ActivityId() const noexcept
    {
        return m_activityId;
    }

    WslTelemetryActivityScope(const WslTelemetryActivityScope&) = delete;
    WslTelemetryActivityScope& operator=(const WslTelemetryActivityScope&) = delete;
    WslTelemetryActivityScope(WslTelemetryActivityScope&&) = delete;
    WslTelemetryActivityScope& operator=(WslTelemetryActivityScope&&) = delete;

private:
    GUID m_activityId{};
    TStopEmit m_stopEmit;
    int m_uncaughtOnEntry;
};

// Deduction guide so callers do not need to spell out the lambda type.
template <typename TStopEmit>
WslTelemetryActivityScope(TStopEmit) -> WslTelemetryActivityScope<TStopEmit>;

#endif // __cplusplus
