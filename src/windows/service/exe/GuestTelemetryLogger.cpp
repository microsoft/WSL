/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GuestTelemetryLogger.cpp

Abstract:

    This file contains logic to log guest telemetry.

--*/

#include "precomp.h"
#include "GuestTelemetryLogger.h"

GuestTelemetryLogger::GuestTelemetryLogger(GUID VmId) : m_runtimeId(VmId)
{
    m_threadExit.create(wil::EventOptions::ManualReset);
}

GuestTelemetryLogger::~GuestTelemetryLogger()
{
    m_threadExit.SetEvent();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

std::shared_ptr<GuestTelemetryLogger> GuestTelemetryLogger::Create(GUID VmId, const wil::unique_event& ExitEvent)
{
    auto logger = std::shared_ptr<GuestTelemetryLogger>(new GuestTelemetryLogger(VmId));
    logger->Start(ExitEvent);
    return logger;
}

LPCWSTR GuestTelemetryLogger::GetPipeName() const
{
    return m_pipeName.c_str();
}

void GuestTelemetryLogger::Start(const wil::unique_event& ExitEvent)
{
    THROW_HR_IF(E_UNEXPECTED, !m_pipeName.empty());

    m_pipeName = wsl::windows::common::helpers::GetUniquePipeName();
    wil::unique_hfile pipe(CreateNamedPipeW(
        m_pipeName.c_str(), (PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED), (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT), 1, LX_RELAY_BUFFER_SIZE, LX_RELAY_BUFFER_SIZE, 0, nullptr));

    THROW_LAST_ERROR_IF(!pipe);

    wil::unique_handle exitEvent(wsl::windows::common::wslutil::DuplicateHandle(ExitEvent.get()));
    m_thread = std::thread([Self = shared_from_this(), Pipe = std::move(pipe), ExitEvent = std::move(exitEvent)]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"GuestTelemetryLogger");

            // When the pipe connects, start reading data.
            const std::vector<HANDLE> exitEvents = {Self->m_threadExit.get(), ExitEvent.get()};
            wsl::windows::common::helpers::ConnectPipe(Pipe.get(), INFINITE, exitEvents);

            std::vector<gsl::byte> buffer(LX_RELAY_BUFFER_SIZE);
            OVERLAPPED overlapped = {};
            const wil::unique_event overlappedEvent(wil::EventOptions::ManualReset);
            overlapped.hEvent = overlappedEvent.get();
            for (;;)
            {
                overlappedEvent.ResetEvent();
                const auto bytesRead =
                    wsl::windows::common::relay::InterruptableRead(Pipe.get(), gsl::make_span(buffer), exitEvents, &overlapped);

                if (bytesRead == 0)
                {
                    break;
                }

                Self->ProcessInput(std::string_view{reinterpret_cast<const char*>(buffer.data()), bytesRead});
            }
        }
        CATCH_LOG()
    });
}

void GuestTelemetryLogger::ProcessInput(const std::string_view Input)
{
    m_ringBuffer.Insert(Input);

    const auto delimiterCount = std::count(Input.begin(), Input.end(), '\n');
    const auto newStrings = m_ringBuffer.GetLastDelimitedStrings('\n', delimiterCount);
    for (const auto& logLine : newStrings)
    {
        WSL_LOG("GuestTelemetry", TraceLoggingValue(logLine.c_str(), "text"), TraceLoggingValue(m_runtimeId, "vmId"));
    }
}