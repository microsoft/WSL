/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GuestTelemetryLogger.h

Abstract:

    This file contains declarations used to log guest telemetry.

--*/

#pragma once

#include "RingBuffer.h"
#include "relay.hpp"

class GuestTelemetryLogger : public std::enable_shared_from_this<GuestTelemetryLogger>
{
public:
    GuestTelemetryLogger() = delete;
    ~GuestTelemetryLogger();

    static std::shared_ptr<GuestTelemetryLogger> Create(GUID VmId, const wil::unique_event& ExitEvent);

    LPCWSTR GetPipeName() const;

private:
    GuestTelemetryLogger(GUID VmId);

    void Start(const wil::unique_event& ExitEvent);
    void ProcessInput(const std::string_view Input);

    std::wstring m_pipeName;
    wil::unique_event m_threadExit;
    std::thread m_thread;
    GUID m_runtimeId{};
    RingBuffer m_ringBuffer{LX_RELAY_BUFFER_SIZE};
};
