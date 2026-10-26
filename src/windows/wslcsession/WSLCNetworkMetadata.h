/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCNetworkMetadata.h

Abstract:

    Constants and types for WSLC-managed Docker networks.

--*/

#pragma once

#include "DockerHTTPClient.h"

namespace wsl::windows::service::wslc {

// Label key used to identify WSLC-managed Docker networks.
constexpr auto WSLCNetworkManagedLabel = "com.microsoft.wsl.network.managed";
constexpr auto WSLCBridgeNetworkDriver = "bridge";

// Reserved Docker network names that cannot be used for custom networks.
inline bool IsReservedNetworkName(const std::string& name)
{
    return name == "bridge" || name == "host" || name == "none";
}

struct NetworkIPAMConfig
{
    std::string Subnet;
    std::string Gateway;
    std::string IPRange;
};

struct NetworkIPAM
{
    std::string Driver;
    std::optional<std::vector<NetworkIPAMConfig>> Config;
};

struct NetworkEntry
{
    std::string Id;
    std::string Driver;
    std::string Scope;
    bool Internal{false};
    bool EnableIPv6{false};
    std::map<std::string, std::string> Labels;
    std::map<std::string, std::string> Options;
    NetworkIPAM IPAM;
};

struct ResolvedNetwork
{
    std::string Name;
    std::string Id;
    bool Managed{false};
};

// Finds a network by exact ID or exact name in a name-keyed network map. Returns networks.end() if nothing matches.
template <typename TNetworkMap>
auto FindNetwork(TNetworkMap& networks, const std::string& nameOrId) -> decltype(networks.end())
{
    for (auto it = networks.begin(); it != networks.end(); ++it)
    {
        if (it->second.Id == nameOrId)
        {
            return it;
        }
    }

    return networks.find(nameOrId);
}

// Resolves a network name, full ID, or unique ID prefix. Exact matches against the session-managed networks are
// resolved locally. Anything else is resolved by the engine, which tracks every network (including built-in and
// unmanaged networks) and rejects ID prefixes that match more than one of them.
template <typename TNetworkMap>
ResolvedNetwork ResolveNetworkReference(const TNetworkMap& networks, const std::string& nameOrId, DockerHTTPClient& dockerClient)
{
    if (const auto it = FindNetwork(networks, nameOrId); it != networks.end())
    {
        return ResolvedNetwork{.Name = it->first, .Id = it->second.Id, .Managed = true};
    }

    common::docker_schema::Network network;
    try
    {
        network = dockerClient.InspectNetwork(nameOrId);
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            WSLC_E_NETWORK_NOT_FOUND, wsl::shared::Localization::MessageWslcNetworkNotFound(nameOrId), e.StatusCode() == 404);
        THROW_HR_WITH_USER_ERROR_IF(
            WSLC_E_NETWORK_PREFIX_AMBIGUOUS, wsl::shared::Localization::MessageWslcNetworkPrefixAmbiguous(nameOrId), e.StatusCode() == 400);
        THROW_DOCKER_USER_ERROR_MSG(e, "Failed to inspect network '%hs'", nameOrId.c_str());
    }

    const auto managed = networks.find(network.Name);
    const bool isManaged = managed != networks.end() && managed->second.Id == network.Id;
    return ResolvedNetwork{.Name = std::move(network.Name), .Id = std::move(network.Id), .Managed = isManaged};
}

} // namespace wsl::windows::service::wslc
