/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCNetworkMetadata.h

Abstract:

    Constants and types for WSLC-managed Docker networks.

--*/

#pragma once

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
};

// Resolves a network name, full ID, or unique ID prefix against a name-keyed network map.
// Resolution order is exact name, exact ID, then unique ID prefix. Returns networks.end() if nothing matches.
template <typename TNetworkMap>
auto FindNetwork(TNetworkMap& networks, const std::string& nameOrId) -> decltype(networks.end())
{
    if (auto it = networks.find(nameOrId); it != networks.end())
    {
        return it;
    }

    auto match = networks.end();
    for (auto it = networks.begin(); it != networks.end(); ++it)
    {
        if (it->second.Id == nameOrId)
        {
            return it;
        }

        if (nameOrId.empty() || !it->second.Id.starts_with(nameOrId))
        {
            continue;
        }

        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG, wsl::shared::Localization::MessageWslcNetworkPrefixAmbiguous(nameOrId), match != networks.end());
        match = it;
    }

    return match;
}

} // namespace wsl::windows::service::wslc
