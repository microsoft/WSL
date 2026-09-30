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
using wsl::windows::common::io::WriteHandle;
using wsl::windows::common::io::WriteNamedPipe;

DmesgCollector::DmesgCollector(
    GUID VmId, HANDLE ExitEvent, bool EnableTelemetry, bool EnableDebugConsole, const std::wstring& Com1PipeName, wil::unique_handle&& OutputHandle) :
    m_com1PipeName(Com1PipeName),
    m_vmExitEvent(ExitEvent),
    m_outputHandle(std::move(OutputHandle)),
    m_runtimeId(VmId),
    m_debugConsole(EnableDebugConsole),
    m_telemetry(EnableTelemetry)
{
}

DmesgCollector::~DmesgCollector()
{
    m_threadExitEvent.SetEvent();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

std::shared_ptr<DmesgCollector> DmesgCollector::Create(
    GUID VmId, HANDLE ExitEvent, bool EnableTelemetry, bool EnableDebugConsole, const std::wstring& Com1PipeName, bool EnableEarlyBootConsole, wil::unique_handle&& OutputHandle)
{
    auto dmesgCollector = std::shared_ptr<DmesgCollector>(
        new DmesgCollector(VmId, ExitEvent, EnableTelemetry, EnableDebugConsole, Com1PipeName, std::move(OutputHandle)));

    dmesgCollector->Start(EnableEarlyBootConsole);
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

    if (m_earlyConsolePipe)
    {
        io.AddHandle(
            std::make_unique<ReadNamedPipe>(
                HandleWrapper{std::move(m_earlyConsolePipe)},
                [this](const gsl::span<char>& Input) { ProcessInput(DmesgCollectorEarlyConsole, Input); }),
            MultiHandleWait::IgnoreErrors);
    }

    io.AddHandle(
        std::make_unique<ReadNamedPipe>(
            HandleWrapper{std::move(m_virtioConsolePipe)},
            [this](const gsl::span<char>& Input) { ProcessInput(DmesgCollectorConsole, Input); }),
        MultiHandleWait::IgnoreErrors);

    if (m_outputHandle)
    {
        auto output = std::make_unique<WriteHandle>(
            HandleWrapper{std::move(m_outputHandle), [this]() { m_outputWrite = nullptr; }}, std::vector<char>{}, false);
        m_outputWrite = output.get();
        io.AddHandle(std::move(output), MultiHandleWait::IgnoreErrors);
    }

    if (m_com1Pipe)
    {
        const bool reconnect = m_pipeServer && !m_debugConsole;

        auto com1 = std::make_unique<WriteNamedPipe>(
            HandleWrapper{std::move(m_com1Pipe), [this]() { m_com1Write = nullptr; }}, reconnect, !m_pipeServer);
        m_com1Write = com1.get();
        io.AddHandle(std::move(com1), MultiHandleWait::IgnoreErrors);
    }

    // The loop runs until either exit event is signaled.
    io.AddHandle(std::make_unique<EventHandle>(m_threadExitEvent.get()), MultiHandleWait::CancelOnCompleted);
    io.AddHandle(std::make_unique<EventHandle>(m_vmExitEvent), MultiHandleWait::CancelOnCompleted);

    io.Run({});
}
CATCH_LOG()

namespace {

template <typename TWriter>
void Push(TWriter& Writer, const gsl::span<char>& Input, const char* Target)
{
    constexpr size_t c_maxDmesgPendingBytes = 1024 * 1024;

    const auto pending = Writer.PendingBytes();

    // Don't fill the buffer past c_maxDmesgPendingBytes. If full, just drop the bytes with a warning.
    if (pending + Input.size() > c_maxDmesgPendingBytes)
    {
        WSL_LOG(
            "DmesgOutputDropped",
            TraceLoggingValue(Target, "target"),
            TraceLoggingValue(static_cast<uint64_t>(pending), "pendingBytes"));

        return;
    }

    Writer.Push(Input);
}

} // namespace

void DmesgCollector::ProcessInput(InputSource Source, const gsl::span<char>& Input)
{
    if (Input.empty())
    {
        return;
    }

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
            sendToComPipe = !m_debugConsole;
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

    if (sendToComPipe && m_com1Write)
    {
        Push(*m_com1Write, Input, "com1");
    }

    if (m_outputWrite != nullptr)
    {
        Push(*m_outputWrite, Input, "output");
    }
}

void DmesgCollector::Start(bool EnableEarlyBootConsole)
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
            }
        }

        THROW_LAST_ERROR_IF(!m_com1Pipe);
    }

    if (EnableEarlyBootConsole)
    {
        std::tie(m_earlyConsoleName, m_earlyConsolePipe) = CreateConsolePipe();
    }

    std::tie(m_virtioConsoleName, m_virtioConsolePipe) = CreateConsolePipe();

    m_thread = std::thread([this]() { Run(); });
}
