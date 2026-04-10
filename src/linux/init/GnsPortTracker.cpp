// Copyright (C) Microsoft Corporation. All rights reserved.

#include <iostream>
#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include "common.h"
#include "GnsPortTracker.h"
#include "lxinitshared.h"
#include "bind_monitor.skel.h"

namespace {

extern "C" int OnBindMonitorEvent(void* ctx, void* data, size_t dataSz) noexcept
{
    auto* tracker = static_cast<GnsPortTracker*>(ctx);
    const auto* event = static_cast<const bind_event*>(data);

    try
    {
        tracker->RequestPort(*event);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION_MSG("Error processing bind monitor event");
    }

    return 0;
}

} // namespace

GnsPortTracker::GnsPortTracker(std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel) :
    m_hvSocketChannel(std::move(hvSocketChannel))
{
}

void GnsPortTracker::RequestPort(const bind_event& Event)
{
    LX_GNS_PORT_ALLOCATION_REQUEST request{};
    request.Header.MessageType = LxGnsMessagePortMappingRequest;
    request.Header.MessageSize = sizeof(request);
    request.Af = Event.family;
    request.Protocol = Event.protocol;
    request.Port = Event.port;
    request.Allocate = Event.is_bind;

    static_assert(sizeof(request.Address32) == 16);
    if (Event.family == AF_INET)
    {
        request.Address32[0] = Event.addr4;
    }
    else
    {
        memcpy(request.Address32, Event.addr6, sizeof(request.Address32));
    }

    const auto& response = m_hvSocketChannel->Transaction(request);

    GNS_LOG_INFO(
        "Port {} request: family ({}) port ({}) protocol ({}) result ({})",
        Event.is_bind ? "allocate" : "release",
        Event.family,
        Event.port,
        Event.protocol,
        response.Result);
}

void GnsPortTracker::Run()
{
    auto* skel = bind_monitor_bpf__open_and_load();
    THROW_LAST_ERROR_IF(!skel);

    auto destroySkel = wil::scope_exit([&] { bind_monitor_bpf__destroy(skel); });

    THROW_LAST_ERROR_IF(bind_monitor_bpf__attach(skel) != 0);

    auto* rb = ring_buffer__new(bpf_map__fd(skel->maps.events), OnBindMonitorEvent, this, nullptr);
    THROW_LAST_ERROR_IF(!rb);

    auto destroyRb = wil::scope_exit([&] { ring_buffer__free(rb); });

    GNS_LOG_INFO("BPF bind monitor attached and running");

    for (;;)
    {
        int err = ring_buffer__poll(rb, -1 /* block until event */);
        if (err == -EINTR)
        {
            continue;
        }

        THROW_LAST_ERROR_IF(err < 0);
    }
}
