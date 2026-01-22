/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LogsRelay.cpp

Abstract:

    Contains the implementation of the LogsRelay class.

--*/

#pragma once

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
    // Stop the relay thread.
    StopRelayThread();

    // Append the new handle
    // N.B. IgnoreErrors is set so the IO doesn't stop on individual handle errors.
    m_io.AddHandle(std::move(Handle), MultiHandleWait::Flags::IgnoreErrors);

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