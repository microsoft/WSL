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

IORelay::IORelay()
{
    m_thread = std::thread([this]() { Run(); });
}

IORelay::~IORelay()
{
    Stop();
}

void IORelay::AddHandle(std::unique_ptr<common::relay::OverlappedIOHandle>&& Handle)
{
    std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>> handles;
    handles.emplace_back(std::move(Handle));

    AddHandles(std::move(handles));
}

void IORelay::AddHandles(std::vector<std::unique_ptr<common::relay::OverlappedIOHandle>>&& Handles)
{
    WI_ASSERT(!m_exit);

    std::lock_guard lock(m_pendingHandlesLock);

    // Append the new handles
    // N.B. IgnoreErrors is set so the IO doesn't stop on individual handle errors.

    for (auto& e : Handles)
    {
        WI_ASSERT(!!e);
        m_pendingHandles.emplace_back(std::move(e));
    }

    // Restart the relay thread.
    m_refreshEvent.SetEvent();
}

void IORelay::Stop()
{
    m_exit = true;
    m_refreshEvent.SetEvent();

    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void IORelay::Run()
try
{
    common::wslutil::SetThreadDescription(L"IORelay");

    windows::common::relay::MultiHandleWait io;

    // N.B. All the IO must happen on the thread.
    // If the thread that scheduled the IO exits, the IO is cancelled.
    while (!m_exit)
    {
        {
            // Add any pending handles.
            std::lock_guard lock(m_pendingHandlesLock);
            for (auto& e : m_pendingHandles)
            {
                io.AddHandle(std::move(e), MultiHandleWait::IgnoreErrors);
            }

            m_pendingHandles.clear();
        }

        io.AddHandle(std::make_unique<common::relay::EventHandle>(m_refreshEvent.get()), MultiHandleWait::CancelOnCompleted);
        io.Run({});
    }
}
CATCH_LOG();