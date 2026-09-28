// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#include <computenetwork.h>

namespace wsl::windows::common::stringify {

constexpr auto ToString(NL_NETWORK_CONNECTIVITY_LEVEL_HINT level) noexcept
{
    switch (level)
    {
    case NetworkConnectivityLevelHintNone:;
        return "None";
    case NetworkConnectivityLevelHintLocalAccess:
        return "LocalAccess";
    case NetworkConnectivityLevelHintInternetAccess:
        return "InternetAccess";
    case NetworkConnectivityLevelHintConstrainedInternetAccess:
        return "ConstrainedInternetAccess";
    case NetworkConnectivityLevelHintHidden:
        return "Hidden";
    default:
        return "Unknown";
    }
}

constexpr auto ToString(NL_NETWORK_CONNECTIVITY_COST_HINT cost) noexcept
{
    switch (cost)
    {
    case NetworkConnectivityCostHintUnrestricted:
        return "Unrestricted";
    case NetworkConnectivityCostHintFixed:
        return "Fixed";
    case NetworkConnectivityCostHintVariable:
        return "Variable";
    default:
        return "Unknown";
    }
}

constexpr auto ToString(ABI::Windows::Networking::Connectivity::NetworkConnectivityLevel connectivityLevel) noexcept
{
    using ABI::Windows::Networking::Connectivity::NetworkConnectivityLevel;

    switch (connectivityLevel)
    {
    case NetworkConnectivityLevel::NetworkConnectivityLevel_ConstrainedInternetAccess:
        return "ConstrainedInternetAccess";
    case NetworkConnectivityLevel::NetworkConnectivityLevel_InternetAccess:
        return "InternetAccess";
    case NetworkConnectivityLevel::NetworkConnectivityLevel_LocalAccess:
        return "LocalAccess";
    case NetworkConnectivityLevel::NetworkConnectivityLevel_None:
        return "None";
    default:
        return "<unknown NetworkConnectivityLevel>";
    }
}

constexpr auto HcnNotificationsToString(DWORD notification) noexcept
{
    switch (notification)
    {
    case HcnNotificationNetworkPreCreate:
        return "HcnNotificationNetworkPreCreate";
    case HcnNotificationNetworkCreate:
        return "HcnNotificationNetworkCreate";
    case HcnNotificationNetworkPreDelete:
        return "HcnNotificationNetworkPreDelete";
    case HcnNotificationNetworkDelete:
        return "HcnNotificationNetworkDelete";
    case HcnNotificationNamespaceCreate:
        return "HcnNotificationNamespaceCreate";
    case HcnNotificationNamespaceDelete:
        return "HcnNotificationNamespaceDelete";
    /// Notifications for HCN_SERVICE handles
    case 0x00000007:
        return "HcnNotificationGuestNetworkServiceCreate";
    case 0x00000008:
        return "HcnNotificationGuestNetworkServiceDelete";
    /// Notifications for HCN_NETWORK handles
    case 0x00000009:
        return "HcnNotificationNetworkEndpointAttached";
    case 0x00000010:
        return "HcnNotificationNetworkEndpointDetached";
    /// Notifications for HCN_GUESTNETWORKSERVICE handles
    case 0x00000011:
        return "HcnNotificationGuestNetworkServiceStateChanged";
    case 0x00000012:
        return "HcnNotificationGuestNetworkServiceInterfaceStateChanged";
    case HcnNotificationServiceDisconnect:
        return "HcnNotificationServiceDisconnect";
    default:
        return "<unknown>";
    }
}
} // namespace wsl::windows::common::stringify
