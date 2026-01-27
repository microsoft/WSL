/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LogsRelay.cpp

Abstract:

    Contains the implementation of the LogsRelay class.

--*/

#include "LogsRelay.h"

using wsl::windows::common::relay::DockerIORelayHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::service::wsla::LogsRelay;

LogsRelay::~LogsRelay()
{
    StopRelayThread();
}

void LogsRelay::AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle)
{
    std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>> handles;
    handles.emplace_back(std::move(Handle));

    AddHandles(std::move(handles));
}

void LogsRelay::AddHandles(std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>>&& Handles)
{
    // Stop the relay thread.
    StopRelayThread();

    // Append the new handles
    // N.B. IgnoreErrors is set so the IO doesn't stop on individual handle errors.

    for (auto& e : Handles)
    {
        m_io.AddHandle(std::move(e), MultiHandleWait::IgnoreErrors);
    }

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
    m_io.AddHandle(std::make_unique<common::relay::EventHandle>(m_stopEvent.get()), MultiHandleWait::CancelOnCompleted);
    m_io.Run({});
}
CATCH_LOG();