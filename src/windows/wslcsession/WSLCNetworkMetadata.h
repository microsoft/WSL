/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCNetworkMetadata.h

Abstract:

    Constants and types for WSLC-managed Docker networks.

--*/

#pragma once

#include <string>

namespace wsl::windows::service::wslc {

// Label key used to identify WSLC-managed Docker networks.
constexpr auto WSLCNetworkManagedLabel = "com.microsoft.wsl.network.managed";
constexpr auto WSLCBridgeNetworkDriver = "bridge";

// Reserved Docker network names that cannot be used for custom networks.
inline bool IsReservedNetworkName(const std::string& name)
{
    return name == "bridge" || name == "host" || name == "none";
}

struct NetworkEntry
{
    std::string Id;
    std::string Driver;
};

} // namespace wsl::windows::service::wslc
