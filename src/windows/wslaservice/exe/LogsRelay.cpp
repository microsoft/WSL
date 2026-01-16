#pragma once

#include "LogsRelay.h"

using wsl::windows::service::wsla::LogsRelay;

LogsRelay::~LogsRelay()
{
    StopRelayThread();
}

std::pair<wil::unique_hfile, wil::unique_hfile> LogsRelay::Add(wil::unique_socket&& socket)
{
    auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);
    auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

    auto handle = std::make_unique<common::relay::DockerIORelayHandle>(std::move(socket), std::move(stdoutWrite), std::move(stderrWrite));

    AddHandle(std::move(handle));

    return {std::move(stdoutRead), std::move(stderrRead)};
}

void LogsRelay::AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle)
{
    // Stop the relay thread.
    StopRelayThread();

    // Append the new handle
    m_io.AddHandle(std::move(Handle));

    // Restart the relay thread.
    StartRelayThread();
}

void LogsRelay::StartRelayThread()
{
    WI_ASSERT(!m_thread.joinable());
    m_stopEvent.ResetEvent();

    m_thread = std::thread([this]() { Run(); });
}

void LogsRelay::StopRelayThread()
{
    if (m_thread.joinable())
    {
        m_stopEvent.SetEvent();
        m_thread.join();
    }
}

void LogsRelay::Run()
try
{

    WSL_LOG("RelayStarting");
    // TODO: restart on IO errors.
    m_io.AddHandle(std::make_unique<common::relay::EventHandle>(m_stopEvent.get(), m_io.CancelRoutine()));
    m_io.Run({});

    WSL_LOG("RelayStopping");

}
CATCH_LOG();