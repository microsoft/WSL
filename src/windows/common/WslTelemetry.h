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
