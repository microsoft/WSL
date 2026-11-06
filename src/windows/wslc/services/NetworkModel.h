/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkModel.h

Abstract:

    This file contains the NetworkModel definitions

--*/

#pragma once

#include "JsonUtils.h"
#include <string>

namespace wsl::windows::wslc::models {

struct CreateNetworkOptions
{
    std::string Name;
    std::optional<std::string> Driver;
    std::vector<std::pair<std::string, std::string>> DriverOpts{};
    std::vector<std::pair<std::string, std::string>> Labels{};
    bool Internal{false};
    bool EnableIPv6{false};
    std::optional<std::string> Subnet;
    std::optional<std::string> Gateway;
    std::optional<std::string> IpRange;
};

struct NetworkEndpointOptions
{
    std::vector<std::string> Aliases;
    std::optional<std::string> IpAddress;
    std::vector<std::string> Links;
    std::vector<std::string> LinkLocalIps;
    std::vector<std::string> DriverOpts;
};

struct ConnectNetworkOptions
{
    std::string NetworkName;
    std::string ContainerId;
    std::vector<std::string> Aliases;
    std::optional<std::string> IpAddress;
    std::vector<std::string> Links;
    std::vector<std::string> LinkLocalIps;
    std::vector<std::string> DriverOpts;
};

struct PruneNetworksResult
{
    std::vector<std::string> PrunedNetworks;
};

// The shape emitted by "network list --format json"; every value is reported as a string.
struct NetworkOutputInformation
{
    std::string CreatedAt;
    std::string Driver;
    std::string ID;
    std::string IPv4;
    std::string IPv6;
    std::string Internal;
    std::string Labels;
    std::string Name;
    std::string Scope;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(NetworkOutputInformation, CreatedAt, Driver, ID, IPv4, IPv6, Internal, Labels, Name, Scope);
};

} // namespace wsl::windows::wslc::models
