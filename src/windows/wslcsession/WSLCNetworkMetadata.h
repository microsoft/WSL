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
    std::map<std::string, std::string> Labels;
    NetworkIPAM IPAM;
};

} // namespace wsl::windows::service::wslc
