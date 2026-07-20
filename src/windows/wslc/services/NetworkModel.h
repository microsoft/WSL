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
    std::optional<std::string> Subnet;
    std::optional<std::string> Gateway;
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

} // namespace wsl::windows::wslc::models
