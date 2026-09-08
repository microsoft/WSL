// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <memory>
#include <optional>
#include <utility>
#include <hcs.hpp>

#include "WslCoreNetworkEndpointSettings.h"
#include "WslCoreTcpIpStateTracking.h"

namespace wsl::core::networking {

struct NetworkEndpoint
{
    NetworkEndpoint() = default;

    ~NetworkEndpoint() noexcept
    {
        DeleteEndpoint();
    }

    NetworkEndpoint(NetworkEndpoint&&) = default;
    NetworkEndpoint& operator=(NetworkEndpoint&& source) noexcept
    {
        if (this != &source)
        {
            DeleteEndpoint();
            StateTracking.reset();

            Network = std::move(source.Network);
            NetworkId = source.NetworkId;
            EndpointId = source.EndpointId;
            InterfaceGuid = source.InterfaceGuid;
            InterfaceLuid = source.InterfaceLuid;
            Endpoint = std::move(source.Endpoint);
            StateTracking = std::move(source.StateTracking);
        }

        return *this;
    }

    NetworkEndpoint(const NetworkEndpoint&) = delete;
    NetworkEndpoint& operator=(const NetworkEndpoint&) = delete;

    std::shared_ptr<NetworkSettings> Network;
    GUID NetworkId{};
    GUID EndpointId{};
    GUID InterfaceGuid{};
    NET_LUID InterfaceLuid{};
    windows::common::hcs::unique_hcn_endpoint Endpoint{};
    std::optional<IpStateTracking> StateTracking;

    void DeleteEndpoint() noexcept
    {
        if (Endpoint)
        {
            wil::unique_cotaskmem_string error;
            LOG_IF_FAILED_MSG(::HcnDeleteEndpoint(EndpointId, &error), "error message: %ls", error.get());
            Endpoint.reset();
        }
    }

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
