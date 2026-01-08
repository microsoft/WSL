/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAProcessIO.cpp

Abstract:

    Contains the different WSLAProcessIO implementations for process IO handling.

--*/

#include "precomp.h"

#include "WSLAProcessIO.h"

using wsl::windows::service::wsla::RelayedProcessIO;
using wsl::windows::service::wsla::TTYProcessIO;
using wsl::windows::service::wsla::VMProcessIO;

RelayedProcessIO::RelayedProcessIO(wil::unique_handle&& IoStream) : m_ioStream(std::move(IoStream))
{
}

RelayedProcessIO::~RelayedProcessIO()
{
    if (m_thread.joinable())
    {
        m_exitEvent.SetEvent();
        m_thread.join();
    }
}

void RelayedProcessIO::StartIORelay()
{
    WI_ASSERT(!m_thread.joinable() && !m_relayedHandles.has_value());

    m_relayedHandles.emplace();

    auto stdinPipe = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto stdoutPipe = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto stderrPipe = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

    m_relayedHandles->emplace(std::make_pair(WSLAFDStdin, stdinPipe.second.release()));
    m_relayedHandles->emplace(std::make_pair(WSLAFDStdout, stdoutPipe.first.release()));
    m_relayedHandles->emplace(std::make_pair(WSLAFDStderr, stderrPipe.first.release()));

    m_thread = std::thread([this,
                            stdinPipe = std::move(stdinPipe.first),
                            stdoutPipe = std::move(stdoutPipe.second),
                            stderrPipe = std::move(stderrPipe.second)]() mutable {
        RunIORelay(std::move(stdinPipe), std::move(stdoutPipe), std::move(stderrPipe));
    });
}

void RelayedProcessIO::RunIORelay(wil::unique_hfile&& stdinPipe, wil::unique_hfile&& stdoutPipe, wil::unique_hfile&& stderrPipe)
try
{
    common::relay::MultiHandleWait io;

    // This is required for docker to know when stdin is closed.
    auto onInputComplete = [&]() {
        LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(m_ioStream.get()), SD_SEND) == SOCKET_ERROR);
    };

    io.AddHandle(std::make_unique<common::relay::RelayHandle>(
        common::relay::HandleWrapper{std::move(stdinPipe), std::move(onInputComplete)}, m_ioStream.get()));

    io.AddHandle(std::make_unique<common::relay::EventHandle>(m_exitEvent.get(), [&]() { io.Cancel(); }));
    io.AddHandle(std::make_unique<common::relay::DockerIORelayHandle>(m_ioStream.get(), std::move(stdoutPipe), std::move(stderrPipe)));

    io.Run({});
}
CATCH_LOG();

wil::unique_handle RelayedProcessIO::OpenFd(ULONG Fd, WSLAFDFlags Flags)
{
    // TODO: Implement logs and non-stream FD's.
    THROW_HR_IF_MSG(E_INVALIDARG, Flags != WSLAFDFlagsStream, "Invalid flags for relayed process: %i", static_cast<int>(Flags));

    if (!m_relayedHandles.has_value())
    {
        StartIORelay();
    }

    auto it = m_relayedHandles->find(Fd);

    THROW_HR_IF_MSG(E_INVALIDARG, it == m_relayedHandles->end(), "Fd not found in relayed handles: %i", static_cast<int>(Fd));
    THROW_WIN32_IF_MSG(ERROR_INVALID_STATE, !it->second.is_valid(), "Fd already consumed: %i", static_cast<int>(Fd));

    return std::move(it->second);
}

TTYProcessIO::TTYProcessIO(wil::unique_handle&& IoStream) : m_ioStream(std::move(IoStream))
{
}

wil::unique_handle TTYProcessIO::OpenFd(ULONG Fd, WSLAFDFlags Flags)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Fd != WSLAFDTty, "Invalid fd type for TTY process: %i", static_cast<int>(Fd));
    THROW_HR_IF_MSG(E_INVALIDARG, Flags != WSLAFDFlagsStream, "Invalid flags for TTY process: %i", static_cast<int>(Flags));

    return std::move(m_ioStream);
}

VMProcessIO::VMProcessIO(std::map<ULONG, wil::unique_handle>&& handles) : m_handles(std::move(handles))
{
}

wil::unique_handle VMProcessIO::OpenFd(ULONG Fd, WSLAFDFlags Flags)
{
    auto it = m_handles.find(Fd);
    THROW_HR_IF_MSG(E_INVALIDARG, it == m_handles.end(), "Invalid fd type for VM process: %i", static_cast<int>(Fd));

    return std::move(it->second);
}