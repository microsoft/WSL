// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "WslCoreTcpIpStateTracking.h"

#include "WslCoreConfig.h"
#include "WslCoreFirewallSupport.h"

// SyncFirewallState is only needed when running on a Windows build with the original Hyper-V Firewall API (shipped with Windows
// 11 22H2) later updates to the Hyper-V Firewall solve the below automatically
void wsl::core::networking::IpStateTracking::SyncFirewallState(const NetworkSettings& preferredNetwork) noexcept
try
{
    // This function is used to update rules which are IP address based. These
    // rules are updated whenever we detect IP address changes.
    // If we have tracked IP addresses, we add or update the rules.
    // If we no longer have any tracked IP addresses, we remove the rules.
    // Currently, we add the following rules:
    //     -My IP loopback rule - allows traffic from my IP addresses
    //     -Local subnet rule - allows traffic from the local subnet

    if (FirewallVmCreatorId.has_value())
    {
        // Obtain current set of IP addresses
        std::set<EndpointIpAddress> currentIpAddresses;
        if (!preferredNetwork.PreferredIpAddress.AddressString.empty())
        {
            currentIpAddresses.insert(preferredNetwork.PreferredIpAddress);
        }
        for (const auto& ipAddress : preferredNetwork.IpAddresses)
        {
            currentIpAddresses.insert(ipAddress);
        }

        // Only perform firewall update if the IP addresses have changed
        if (currentIpAddresses != FirewallTrackedIpAddresses)
        {
            // Ensure COM state is properly initialized
            const auto coInit = InitializeCOMState();
            const auto loopbackRuleId = MakeLoopbackFirewallRuleId(FirewallVmCreatorId.value());
            const auto localSubnetRuleId = MakeLocalSubnetFirewallRuleId(FirewallVmCreatorId.value());

            // If we have no IP addresses, remove any existing rules
            if (currentIpAddresses.empty())
            {
                WSL_LOG("IpStateTracking::SyncFirewallState removing rules");

                // Remove loopback rule
                RemoveHyperVFirewallRule(loopbackRuleId);

                // Remove local subnet rule
                RemoveHyperVFirewallRule(localSubnetRuleId);
            }
            else
            {
                // We have IP addresses - update the firewall rules
                FirewallRuleConfiguration myIpLoopbackRule{MakeLoopbackFirewallRuleConfiguration(loopbackRuleId)};
                FirewallRuleConfiguration localSubnetRule{MakeLocalSubnetFirewallRuleConfiguration(localSubnetRuleId)};

                // Iterate through IP Addresses to populate myIP loopback addresses and local subnet addresses
                std::set<std::wstring> localSubnetPrefixes;
                for (const auto& ipAddress : currentIpAddresses)
                {
                    myIpLoopbackRule.RemoteAddresses.emplace_back(wil::make_bstr(ipAddress.AddressString.c_str()));
                    localSubnetPrefixes.insert(ipAddress.GetPrefix());
                }

                // Convert from set of wstring to vector of unique_bstr
                for (const auto& subnet : localSubnetPrefixes)
                {
                    localSubnetRule.RemoteAddresses.emplace_back(wil::make_bstr(subnet.c_str()));
                }

                // Add my IP loopback rule
                WSL_LOG("IpStateTracking::SyncFirewallState Adding my IP loopback rule");
                AddHyperVFirewallRule(FirewallVmCreatorId.value(), myIpLoopbackRule);

                // Add local subnet rule
                WSL_LOG("IpStateTracking::SyncFirewallState Adding local subnet rule");
                AddHyperVFirewallRule(FirewallVmCreatorId.value(), localSubnetRule);
            }

            // Swap to the tracked set of IP addresses after we have performed all of the updates
            std::swap(FirewallTrackedIpAddresses, currentIpAddresses);
        }
        else
        {
            WSL_LOG(
                "IpStateTracking::SyncFirewallState - FirewallTrackedIpAddresses is synced with the preferredNetwork",
                TraceLoggingValue(FirewallTrackedIpAddresses.size(), "FirewallTrackedIpAddresses.size"));
        }
    }
    else
    {
        WSL_LOG("IpStateTracking::SyncFirewallState - no FirewallVmCreatorId");
    }
}
CATCH_LOG()
