/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    IORelay.cpp

Abstract:

    Contains the implementation of the IORelay class.

--*/

#include "IORelay.h"

using wsl::windows::common::relay::DockerIORelayHandle;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::service::wsla::IORelay;

IORelay::~IORelay()
{
    StopRelayThread();
}

void IORelay::AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle)
{
    std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>> handles;
    handles.emplace_back(std::move(Handle));

    AddHandles(std::move(handles));
}

void IORelay::AddHandles(std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>>&& Handles)
{
    // Stop the relay thread.
    StopRelayThread();

    // Append the new handles
    // N.B. IgnoreErrors is set so the IO doesn't stop on individual handle errors.

    for (auto& e : Handles)
    {
        WI_ASSERT(!!e);
        m_io.AddHandle(std::move(e), MultiHandleWait::IgnoreErrors);
    }

    // Restart the relay thread.
    StartRelayThread();
}

void IORelay::StartRelayThread()
{
    WI_ASSERT(!m_thread.joinable());
    m_stopEvent.ResetEvent();

    m_thread = std::thread([this]() { Run(); });
}

void IORelay::StopRelayThread()
{
    if (m_thread.joinable())
    {
        m_stopEvent.SetEvent();
        m_thread.join();
    }
}

void IORelay::Run()
try
{
    m_io.AddHandle(std::make_unique<common::relay::EventHandle>(m_stopEvent.get()), MultiHandleWait::CancelOnCompleted);
    m_io.Run({});
}
CATCH_LOG();