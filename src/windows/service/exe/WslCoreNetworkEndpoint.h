// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <memory>
#include <optional>
#include <hcs.hpp>

#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreTcpIpStateTracking.h"

namespace wsl::core::networking {

struct NetworkEndpoint
{
    NetworkEndpoint() = default;

    ~NetworkEndpoint() noexcept
    {
        if (Endpoint)
        {
            wil::unique_cotaskmem_string error;
            LOG_IF_FAILED_MSG(::HcnDeleteEndpoint(EndpointId, &error), "error message: %ls", error.get());
        }
    }

    NetworkEndpoint(NetworkEndpoint&&) = default;
    NetworkEndpoint& operator=(NetworkEndpoint&& source) = default;
    NetworkEndpoint(const NetworkEndpoint&) = delete;
    NetworkEndpoint& operator=(const NetworkEndpoint&) = delete;

    std::shared_ptr<NetworkSettings> Network;
    GUID NetworkId{};
    GUID EndpointId{};
    GUID InterfaceGuid{};
    NET_LUID InterfaceLuid{};
    windows::common::hcs::unique_hcn_endpoint Endpoint{};
    std::optional<IpStateTracking> StateTracking;

    void TraceLoggingRundown() const
    {
        if (Network)
        {
            WSL_LOG(
                "NetworkEndpoint::TraceLoggingRundown",
                TraceLoggingValue(NetworkId, "networkId"),
                TraceLoggingValue(EndpointId, "endpointId"),
                TraceLoggingValue(InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(InterfaceLuid.Value, "interfaceLuid"),
                TRACE_NETWORKSETTINGS_OBJECT(Network));
        }
        else
        {
            WSL_LOG(
                "NetworkEndpoint::TraceLoggingRundown",
                TraceLoggingValue(NetworkId, "networkId"),
                TraceLoggingValue(EndpointId, "endpointId"),
                TraceLoggingValue(InterfaceGuid, "interfaceGuid"),
                TraceLoggingValue(InterfaceLuid.Value, "interfaceLuid"),
                TraceLoggingValue("null", "Network"));
        }
    }
};
} // namespace wsl::core::networking
