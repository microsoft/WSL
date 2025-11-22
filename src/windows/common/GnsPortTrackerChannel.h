// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <thread>
#include <functional>
#include <wil/resource.h>
#include <lxinitshared.h>
#include "SocketChannel.h"

namespace wsl::core {
class GnsPortTrackerChannel
{
public:
    GnsPortTrackerChannel(
        wil::unique_socket&& Socket,
        const std::function<int(const SOCKADDR_INET&, int, bool)>& Callback,
        const std::function<void(const std::string&, bool)>& InterfaceStateCallback);

    ~GnsPortTrackerChannel();

    GnsPortTrackerChannel(const GnsPortTrackerChannel&) = delete;
    GnsPortTrackerChannel(GnsPortTrackerChannel&&) = delete;

    GnsPortTrackerChannel& operator=(const GnsPortTrackerChannel&) = delete;
    GnsPortTrackerChannel& operator=(GnsPortTrackerChannel&&) = delete;

    void Run();

private:
    static SOCKADDR_INET ConvertPortRequestToSockAddr(_In_ const LX_GNS_PORT_ALLOCATION_REQUEST* portAllocationRequest);

    const std::function<int(const SOCKADDR_INET&, int, bool)> m_callback;
    const std::function<void(const std::string&, bool)> m_interfaceStateCallback;
    wil::unique_event m_stopEvent = wil::unique_event{wil::EventOptions::ManualReset};
    wsl::shared::SocketChannel m_channel;
    std::thread m_thread;
};
} // namespace wsl::core
