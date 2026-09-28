/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    traceloggingconfig.h

Abstract:

    This file contains constants for ETL logging.

--*/

#pragma once

#include "defs.h"

static_assert(!wsl::shared::OfficialBuild);

#define MICROSOFT_KEYWORD_TELEMETRY 0
#define MICROSOFT_KEYWORD_MEASURES 0
#define MICROSOFT_KEYWORD_CRITICAL_DATA 0
#define PDT_ProductAndServicePerformance 0
#define PDT_ProductAndServiceUsage 0
#define TelemetryPrivacyDataTag(tag) TraceLoggingUInt64((tag), "PartA_PrivTags")
#define TraceLoggingOptionMicrosoftTelemetry() \
    TraceLoggingOptionGroup(0x9aa7a361, 0x583f, 0x4c09, 0xb1, 0xf1, 0xce, 0xa1, 0xef, 0x58, 0x63, 0xb0)