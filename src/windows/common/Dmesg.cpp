/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Dmesg.cpp

Abstract:

    This file contains logic to collect dmesg output.

--*/

#include "precomp.h"
#include "Dmesg.h"

using wsl::windows::common::io::EventHandle;
using wsl::windows::common::io::HandleWrapper;
using wsl::windows::common::io::MultiHandleWait;
using wsl::windows::common::io::ReadNamedPipe;

DmesgCollector::DmesgCollector(
    GUID VmId, const wil::unique_event& ExitEvent, bool EnableTelemetry, bool EnableDebugConsole, const std::wstring& Com1PipeName, wil::unique_handle&& OutputHandle) :
    m_com1PipeName(Com1PipeName),
    m_runtimeId(VmId),
    m_debugConsole(EnableDebugConsole),
    m_telemetry(EnableTelemetry),
    m_outputHandle(std::move(OutputHandle))
{
    m_exitEvent.reset(wsl::windows::common::wslutil::DuplicateHandle(ExitEvent.get()));
    m_overlappedEvent.create(wil::EventOptions::ManualReset);
    m_overlapped.hEvent = m_overlappedEvent.get();
    m_threadExit.create(wil::EventOptions::ManualReset);
    m_exitEvents = {m_threadExit.get(), m_exitEvent.get()};
}

DmesgCollector::~DmesgCollector()
{
    m_threadExit.SetEvent();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

std::shared_ptr<DmesgCollector> DmesgCollector::Create(
    GUID VmId,
    const wil::unique_event& ExitEvent,
    bool EnableTelemetry,
    bool EnableDebugConsole,
    const std::wstring& Com1PipeName,
    bool EnableEarlyBootConsole,
    wil::unique_handle&& OutputHandle)
{
    auto dmesgCollector = std::shared_ptr<DmesgCollector>(
        new DmesgCollector(VmId, ExitEvent, EnableTelemetry, EnableDebugConsole, Com1PipeName, std::move(OutputHandle)));

    if (FAILED(dmesgCollector->Start(EnableEarlyBootConsole)))
    {
        return {};
    }

    return dmesgCollector;
}

std::pair<std::wstring, wil::unique_hfile> DmesgCollector::CreateConsolePipe()
{
    std::wstring pipeName = wsl::windows::common::helpers::GetUniquePipeName();
    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(), (PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED), (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT), 1, LX_RELAY_BUFFER_SIZE, LX_RELAY_BUFFER_SIZE, 0, nullptr));

    THROW_LAST_ERROR_IF(!pipe);

    return {std::move(pipeName), std::move(pipe)};
}

void DmesgCollector::Run()
try
{
    wsl::windows::common::wslutil::SetThreadDescription(L"Dmesg");

    MultiHandleWait io;

    // N.B. The reads use IgnoreErrors so a failure on one console doesn't tear down collection on the other.
    if (m_earlyConsolePipe)
    {
        io.AddHandle(
            std::make_unique<ReadNamedPipe>(
                HandleWrapper{std::move(m_earlyConsolePipe)},
                [this](const gsl::span<char>& Input) {
                    if (!Input.empty())
                    {
                        ProcessInput(DmesgCollectorEarlyConsole, Input);
                    }
                }),
            MultiHandleWait::IgnoreErrors | MultiHandleWait::NeedNotComplete);
    }

    io.AddHandle(
        std::make_unique<ReadNamedPipe>(
            HandleWrapper{std::move(m_virtioConsolePipe)},
            [this](const gsl::span<char>& Input) {
                if (!Input.empty())
                {
                    ProcessInput(DmesgCollectorConsole, Input);
                }
            }),
        MultiHandleWait::IgnoreErrors | MultiHandleWait::NeedNotComplete);

    // The loop runs until either exit event is signaled.
    io.AddHandle(std::make_unique<EventHandle>(m_threadExit.get()), MultiHandleWait::CancelOnCompleted);
    io.AddHandle(std::make_unique<EventHandle>(m_exitEvent.get()), MultiHandleWait::CancelOnCompleted);

    io.Run({});
}
CATCH_LOG()

void DmesgCollector::ProcessInput(InputSource Source, const gsl::span<char>& Input)
{
    RingBuffer* ringBuffer = nullptr;
    bool sendToComPipe = m_debugConsole;
    if (Source == DmesgCollectorEarlyConsole)
    {
        if (!m_earlyConsoleTransition)
        {
            ringBuffer = &m_dmesgEarlyBuffer;
        }
        else
        {
            sendToComPipe = !m_debugConsole && m_com1Pipe;
        }
    }
    else
    {
        WI_ASSERT(Source == DmesgCollectorConsole);
        ringBuffer = &m_dmesgBuffer;
        if (!m_earlyConsoleTransition)
        {
            // The transition is because COM1 may have some other purpose after boot, and that data should not be
            // captured into the dmesg log. Ideally we would flush all bytes for a clean transition, but since the
            // legacy serial device is essentially one byte at a time, there isn't a clean way to detect this.
            m_earlyConsoleTransition = true;
        }
    }

    if (ringBuffer != nullptr)
    {
        std::string_view inputString{Input.data(), Input.size()};
        ringBuffer->Insert(inputString);
        if (m_telemetry)
        {
            const auto delimiterCount = std::count(inputString.begin(), inputString.end(), '\n');
            const auto newStrings = ringBuffer->GetLastDelimitedStrings('\n', delimiterCount);
            for (const auto& logLine : newStrings)
            {
                WSL_LOG("GuestLog", TraceLoggingValue(logLine.c_str(), "text"), TraceLoggingValue(m_runtimeId, "vmId"));
            }
        }
    }

    if (sendToComPipe)
    {
        WriteToCom1(Input);
    }

    if (m_outputHandle != nullptr)
    {
        m_overlappedEvent.ResetEvent();
        if (wsl::windows::common::relay::InterruptableWrite(
                m_outputHandle.get(), gslhelpers::convert_span<gsl::byte>(Input), m_exitEvents, &m_overlapped) == 0)
        {
            m_outputHandle = nullptr;
        }
    }
}

void DmesgCollector::WriteToCom1(const gsl::span<char>& Input)
{
    if (!m_com1Pipe)
    {
        return;
    }

    // If this is not writing to the debug console, emulate the normal
    // serial pipe behavior of waiting for a pipe connection.
    if (m_waitForConnection)
    {
        if (FAILED(wil::ResultFromException(
                [&]() { wsl::windows::common::helpers::ConnectPipe(m_com1Pipe.get(), INFINITE, m_exitEvents); })))
        {
            return;
        }

        m_waitForConnection = false;
    }

    m_overlappedEvent.ResetEvent();
    const auto buffer = gslhelpers::convert_span<gsl::byte>(Input);
    if (wsl::windows::common::relay::InterruptableWrite(m_com1Pipe.get(), buffer, m_exitEvents, &m_overlapped) == 0)
    {
        if (m_debugConsole || !m_pipeServer)
        {
            // A disconnect from the debug console, or from a pipe that was acting as the server, doesn't have any
            // reconnect mechanism, so don't try to write anymore bytes.
            m_com1Pipe.reset();
        }
        else
        {
            // Emulate the normal serial behavior of waiting for a pipe connection to write.
            m_waitForConnection = true;
        }
    }
}

HRESULT DmesgCollector::Start(bool EnableEarlyBootConsole)
try
{
    if (!m_com1PipeName.empty())
    {
        // Check if the named pipe has already been created
        m_com1Pipe.reset(CreateFileW(
            m_com1PipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr));

        if (!m_com1Pipe)
        {
            m_com1Pipe.reset(CreateNamedPipeW(
                m_com1PipeName.c_str(),
                (PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED),
                (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT),
                1,
                LX_RELAY_BUFFER_SIZE,
                LX_RELAY_BUFFER_SIZE,
                0,
                nullptr));

            if (m_com1Pipe)
            {
                m_pipeServer = true;
                // If the debug console is not active, may have to wait for a connection.
                m_waitForConnection = !m_debugConsole;
            }
        }

        THROW_LAST_ERROR_IF(!m_com1Pipe);
    }

    if (EnableEarlyBootConsole)
    {
        std::tie(m_earlyConsoleName, m_earlyConsolePipe) = CreateConsolePipe();
    }

    std::tie(m_virtioConsoleName, m_virtioConsolePipe) = CreateConsolePipe();

    m_worker = std::thread([this]() { Run(); });
    return S_OK;
}
CATCH_RETURN()
