// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <optional>
#include <string>
#include <vector>
#include <wbemcli.h>
#include <windows.h>
#include "WslCoreConfig.h"

namespace wsl::core::networking {

enum class HyperVFirewallSupport
{
    None,
    Version1, // initially shipped with SV2
    Version2  // updated from Version1 and backported down to Windows 11 22H2
};
HyperVFirewallSupport GetHyperVFirewallSupportVersion(const FirewallConfiguration& firewallConfig) noexcept;

std::wstring MakeLoopbackFirewallRuleId(const GUID& guid);
std::wstring MakeLocalSubnetFirewallRuleId(const GUID& guid);

std::vector<FirewallRuleConfiguration> MakeDefaultFirewallRuleConfiguration(const GUID& guid);
FirewallRuleConfiguration MakeLoopbackFirewallRuleConfiguration(const std::wstring& ruleId);
FirewallRuleConfiguration MakeLocalSubnetFirewallRuleConfiguration(const std::wstring& ruleId);

void ConfigureHyperVFirewall(const FirewallConfiguration& firewallConfig, const std::wstring& vmCreatorFriendlyName) noexcept;

void ConfigureSharedAccessFirewallRule() noexcept;

HRESULT AddHyperVFirewallRule(const GUID& vmCreatorId, const wsl::core::FirewallRuleConfiguration& firewallRule) noexcept;
HRESULT RemoveHyperVFirewallRule(const std::wstring& ruleId) noexcept;

} // namespace wsl::core::networking
