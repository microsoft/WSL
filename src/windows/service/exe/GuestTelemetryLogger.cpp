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

std::shared_ptr<GuestTelemetryLogger> GuestTelemetryLogger::Create(GUID VmId, const wil::unique_event& ExitEvent, PSID PipeClientSid)
{
    auto logger = std::shared_ptr<GuestTelemetryLogger>(new GuestTelemetryLogger(VmId));
    logger->Start(ExitEvent, PipeClientSid);
    return logger;
}

LPCWSTR GuestTelemetryLogger::GetPipeName() const
{
    return m_pipeName.c_str();
}

void GuestTelemetryLogger::Start(const wil::unique_event& ExitEvent, PSID PipeClientSid)
{
    THROW_HR_IF(E_UNEXPECTED, !m_pipeName.empty());

    m_pipeName = wsl::windows::common::helpers::GetUniquePipeName();
    auto pipe = wsl::windows::common::helpers::CreatePipeServer(m_pipeName.c_str(), PipeClientSid);
    wil::unique_handle exitEvent(wsl::windows::common::wslutil::DuplicateHandle(ExitEvent.get()));
    m_thread = std::thread([this, Pipe = std::move(pipe), ExitEvent = std::move(exitEvent)]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"GuestTelemetryLogger");

            // When the pipe connects, start reading data.
            const std::vector<HANDLE> exitEvents = {m_threadExit.get(), ExitEvent.get()};
            wsl::windows::common::helpers::ConnectPipe(Pipe.get(), INFINITE, exitEvents);

            namespace io = wsl::windows::common::io;
            io::MultiHandleWait ioWait;
            ioWait.AddHandle(
                std::make_unique<io::ReadHandle>(
                    io::HandleWrapper{Pipe.get()},
                    [this](const gsl::span<char>& buffer) {
                        if (!buffer.empty())
                        {
                            ProcessInput(std::string_view{buffer.data(), static_cast<size_t>(buffer.size())});
                        }
                    }),
                io::MultiHandleWait::IgnoreErrors);

            ioWait.AddHandle(
                std::make_unique<io::EventHandle>(io::HandleWrapper{m_threadExit.get()}),
                io::MultiHandleWait::CancelOnCompleted | io::MultiHandleWait::NeedNotComplete);

            ioWait.AddHandle(
                std::make_unique<io::EventHandle>(io::HandleWrapper{ExitEvent.get()}),
                io::MultiHandleWait::CancelOnCompleted | io::MultiHandleWait::NeedNotComplete);

            ioWait.Run(std::nullopt);
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