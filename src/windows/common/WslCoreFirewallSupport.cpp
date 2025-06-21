// Copyright (C) Microsoft Corporation. All rights reserved.

#include "WslCoreFirewallSupport.h"

#include <ComputeNetwork.h>
#include <wil/com.h>

#include "string.hpp"
#include "WslCoreNetworkingSupport.h"

static constexpr auto c_hyperVFirewallLoopbackRuleIdPrefix_Old = L"WSA-IP-Loopback-Allow-Rule-1-";
static constexpr auto c_hyperVFirewallLoopbackRuleIdPrefix = L"WslCore-IP-Loopback-Allow-Rule-1-";
static constexpr auto c_hyperVFirewallLoopbackRuleName = L"WslCore Loopback Allow Rule";

static constexpr auto c_hyperVFirewallLocalSubnetRuleIdPrefix = L"WslCore-LocalSubnet-Allow-Rule-1-";
static constexpr auto c_hyperVFirewallLocalSubnetRuleName = L"WslCore LocalSubnet Allow Rule";

static constexpr auto c_hyperVFirewallIcmpV6RuleIdPrefix = L"WslCore-Allow-Inbound-ICMPv6-1-";
static constexpr auto c_hyperVFirewallIcmpV6RuleName = L"WslCore Inbound ICMPv6 Default Allow Rule";

static constexpr auto c_hyperVFirewallIcmpV4RuleIdPrefix = L"WslCore-Allow-Inbound-ICMPv4-1-";
static constexpr auto c_hyperVFirewallIcmpV4RuleName = L"WslCore Inbound ICMPv4 Default Allow Rule";

// Host Firewall rule to allow traffic to SharedAccess service.
static constexpr auto c_sharedAccessRuleId = L"WSLCore-SharedAccess-Allow-Rule";
static constexpr auto c_sharedAccessRuleName = L"WSLCore SharedAccess Allow Rule";
static constexpr auto c_sharedAccessService = L"SharedAccess";

static constexpr auto c_protocolUDP = L"UDP";
static constexpr auto c_svchostApplication = L"%SYSTEMROOT%\\System32\\svchost.exe";

// Regkey to control hyper-v firewall being disabled
static constexpr auto c_mpssvcRegPath = L"SYSTEM\\CurrentControlSet\\Services\\MpsSvc\\Parameters";
static constexpr auto c_mpssvcRegDisableKey = L"HyperVFirewallDisable";

// Constants corresponding to firewall WMI values
static constexpr auto c_directionInbound = 1;
static constexpr auto c_actionAllow = 2;
static constexpr auto c_ruleEnabled = 1;
static constexpr auto c_ruleDisabled = 0;
static constexpr auto c_true = 1;

// ICMP "port" constants
static constexpr auto c_icmpv6NeighborSolicitation = L"135";
static constexpr auto c_icmpv6NeighborAdvertisement = L"136";
static constexpr auto c_icmpv6PortDestinationUnreachable = L"1";
static constexpr auto c_icmpv6PortTimeExceeded = L"3";
static constexpr auto c_icmpv4PortDestinationUnreachable = L"3";
static constexpr auto c_icmpv4PortTimeExceeded = L"11";

// mDNS related constants
static constexpr auto c_hyperVFirewallMdnsIpv4RuleIdPrefix = L"WslCore-Allow-Inbound-mDNS-IPv4-1-";
static constexpr auto c_hyperVFirewallMdnsIpv4RuleName = L"WslCore Inbound IPv4 mDNS Default Allow Rule";

static constexpr auto c_hyperVFirewallMdnsIpv6RuleIdPrefix = L"WslCore-Allow-Inbound-mDNS-IPv6-1-";
static constexpr auto c_hyperVFirewallMdnsIpv6RuleName = L"WslCore Inbound IPv6 mDNS Default Allow Rule";

static constexpr auto c_mdnsPort = L"5353";
static constexpr auto c_mdnsIpv4Address = L"224.0.0.251";
static constexpr auto c_mdnsIpv6Address = L"ff02::fb";

namespace wsl::core::networking {
std::wstring MakeLoopbackFirewallRuleId(const GUID& guid)
{
    return c_hyperVFirewallLoopbackRuleIdPrefix +
           wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
}

std::wstring MakeLocalSubnetFirewallRuleId(const GUID& guid)
{
    return c_hyperVFirewallLocalSubnetRuleIdPrefix +
           wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
}

std::wstring MakeICMPv6FirewallRuleId(const GUID& guid)
{
    return c_hyperVFirewallIcmpV6RuleIdPrefix + wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
}

std::wstring MakeICMPv4FirewallRuleId(const GUID& guid)
{
    return c_hyperVFirewallIcmpV4RuleIdPrefix + wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
}

std::wstring MakeMdnsIpv4FirewallRuleId(const GUID& guid)
{
    return c_hyperVFirewallMdnsIpv4RuleIdPrefix +
           wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
}

std::wstring MakeMdnsIpv6FirewallRuleId(const GUID& guid)
{
    return c_hyperVFirewallMdnsIpv6RuleIdPrefix +
           wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None);
}

// if enabling Hyper-V Firewall, ensure the following rules are always added:
// a) ICMP rules for inbound responses, without these we risk breaking basic connectivity and/or app compat
// b) inbound rules to allow mDNS traffic. Note: Host firewall also has rules to allow inbound mDNS traffic but those
//    are scoped to the Windows dnscache service so they can't be automatically translated to Hyper-V firewall
std::vector<FirewallRuleConfiguration> MakeDefaultFirewallRuleConfiguration(const GUID& guid)
{
    std::vector<FirewallRuleConfiguration> firewallConfiguration;

    FirewallRuleConfiguration icmpV6AllowRule{MakeICMPv6FirewallRuleId(guid).c_str()};
    icmpV6AllowRule.RuleName = wil::make_bstr(c_hyperVFirewallIcmpV6RuleName);
    icmpV6AllowRule.Protocol = wil::make_bstr(L"ICMPv6");
    icmpV6AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_icmpv6NeighborSolicitation));
    icmpV6AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_icmpv6NeighborAdvertisement));
    icmpV6AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_icmpv6PortDestinationUnreachable));
    icmpV6AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_icmpv6PortTimeExceeded));
    icmpV6AllowRule.RemoteAddresses.clear(); // all remote addresses
    icmpV6AllowRule.RuleOperation = FirewallRuleOperation::Add;
    firewallConfiguration.emplace_back(icmpV6AllowRule);

    FirewallRuleConfiguration icmpV4AllowRule{MakeICMPv4FirewallRuleId(guid).c_str()};
    icmpV4AllowRule.RuleName = wil::make_bstr(c_hyperVFirewallIcmpV4RuleName);
    icmpV4AllowRule.Protocol = wil::make_bstr(L"ICMPv4");
    icmpV4AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_icmpv4PortDestinationUnreachable));
    icmpV4AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_icmpv4PortTimeExceeded));
    icmpV4AllowRule.RemoteAddresses.clear(); // all remote addresses
    icmpV4AllowRule.RuleOperation = FirewallRuleOperation::Add;
    firewallConfiguration.emplace_back(icmpV4AllowRule);

    FirewallRuleConfiguration mdnsIPv4AllowRule{MakeMdnsIpv4FirewallRuleId(guid).c_str()};
    mdnsIPv4AllowRule.RuleName = wil::make_bstr(c_hyperVFirewallMdnsIpv4RuleName);
    mdnsIPv4AllowRule.Protocol = wil::make_bstr(c_protocolUDP);
    mdnsIPv4AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_mdnsPort));
    mdnsIPv4AllowRule.LocalAddresses.emplace_back(wil::make_bstr(c_mdnsIpv4Address));
    mdnsIPv4AllowRule.RemoteAddresses.clear(); // all remote addresses
    mdnsIPv4AllowRule.RuleOperation = FirewallRuleOperation::Add;
    firewallConfiguration.emplace_back(mdnsIPv4AllowRule);

    FirewallRuleConfiguration mdnsIPv6AllowRule{MakeMdnsIpv6FirewallRuleId(guid).c_str()};
    mdnsIPv6AllowRule.RuleName = wil::make_bstr(c_hyperVFirewallMdnsIpv6RuleName);
    mdnsIPv6AllowRule.Protocol = wil::make_bstr(c_protocolUDP);
    mdnsIPv6AllowRule.LocalPorts.emplace_back(wil::make_bstr(c_mdnsPort));
    mdnsIPv6AllowRule.LocalAddresses.emplace_back(wil::make_bstr(c_mdnsIpv6Address));
    mdnsIPv6AllowRule.RemoteAddresses.clear(); // all remote addresses
    mdnsIPv6AllowRule.RuleOperation = FirewallRuleOperation::Add;
    firewallConfiguration.emplace_back(mdnsIPv6AllowRule);

    return firewallConfiguration;
}

FirewallRuleConfiguration MakeLoopbackFirewallRuleConfiguration(const std::wstring& ruleId)
{
    return {ruleId.c_str(), c_hyperVFirewallLoopbackRuleName};
}

FirewallRuleConfiguration MakeLocalSubnetFirewallRuleConfiguration(const std::wstring& ruleId)
{
    return {ruleId.c_str(), c_hyperVFirewallLocalSubnetRuleName};
}

// We can require the updated Firewall API be available (on all OS's that get the update)
// Thus we must indicate to the caller what version of Hyper-V Firewall is currently running.
HyperVFirewallSupport GetHyperVFirewallSupportVersion(const FirewallConfiguration& firewallConfig) noexcept
try
{
    // Check to see if Hyper-V firewall is disabled via the registry.
    DWORD localFirewallDisabled = 0;
    wil::ResultFromException([&] {
        localFirewallDisabled = windows::common::registry::ReadDword(HKEY_LOCAL_MACHINE, c_mpssvcRegPath, c_mpssvcRegDisableKey, 0);
    });
    if (localFirewallDisabled == 1)
    {
        WSL_LOG("GetHyperVFirewallSupportVersion: disabled by registry [HyperVFirewallSupport::None]");
        return HyperVFirewallSupport::None;
    }

    // There are no APIs to directly query which level of Hyper-V firewall support we have.
    // Instead, we check for availability of specific firewall objects/fields present to
    // determine if the requested functionality is supported or not.
    //
    // Currently, there are 3 possible levels of Hyper-V firewall OS support:
    // 1 - No Hyper-V firewall OS support.
    // 2 - Initial Hyper-V firewall support (Support for mirrored mode only).
    //     To check for this support, we query for the 'MSFT_NetFirewallHyperVVMCreator' object.
    // 3 - Enterprise Hyper-V firewall support (Support for NAT mode, configuring default settings values, and configuring per-profile configs).
    //     To check for this support, we query for the 'MSFT_NetFirewallHyperVProfile' object.

    // Connect to the root\standardcimv2 namespace with the current user and obtain pointer to make IWbemServices calls.
    const auto locator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();
    wil::com_ptr<IWbemServices> wbemService;
    THROW_IF_FAILED(locator->ConnectServer(
        wil::make_bstr(L"ROOT\\standardcimv2").get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &wbemService));

    // Set the IWbemServices proxy so that impersonation of the user (client) occurs.
    THROW_IF_FAILED(CoSetProxyBlanket(
        wbemService.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

    HRESULT hr{};
    wil::com_ptr<IWbemClassObject> baseObject;

    // Query for initial Hyper-V firewall OS support.
    hr = wbemService->GetObjectW(
        wil::make_bstr(L"MSFT_NetFirewallHyperVVMCreator").get(), WBEM_FLAG_RETURN_WBEM_COMPLETE, nullptr, &baseObject, nullptr);
    if (FAILED(hr))
    {
        WSL_LOG(
            "GetHyperVFirewallSupportVersion: MSFT_NetFirewallHyperVVMCreator failed to be instantiated "
            "[HyperVFirewallSupport::None]",
            TraceLoggingValue(hr));
        return HyperVFirewallSupport::None;
    }

    // Query for version 2 of the Hyper-V Firewall
    // We query for object instances instead of only getting the object class as this will return an error
    // if the OS changes are present but the Hyper-V Firewall feature is disabled.
    wil::com_ptr<IEnumWbemClassObject> enumObjects;
    hr = wbemService->ExecQuery(
        wil::make_bstr(L"WQL").get(), wil::make_bstr(L"SELECT * FROM MSFT_NetFirewallHyperVProfile").get(), WBEM_FLAG_RETURN_WBEM_COMPLETE, nullptr, &enumObjects);
    if (FAILED(hr))
    {
        WSL_LOG(
            "GetHyperVFirewallSupportVersion: Query MSFT_NetFirewallHyperVProfile instances failed "
            "[HyperVFirewallSupport::Version1]",
            TraceLoggingValue(hr));
        return HyperVFirewallSupport::Version1;
    }

    // If we reached here, we were able to query the Version2 objects
    WSL_LOG("GetHyperVFirewallSupportVersion [HyperVFirewallSupport::Version2]");
    return HyperVFirewallSupport::Version2;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    WSL_LOG(
        "wsl::core::networking::GetHyperVFirewallSupportVersion [HyperVFirewallSupport::None]",
        TraceLoggingValue(ToString(firewallConfig.DefaultLoopbackPolicy), "defaultLoopbackPolicy"));

    return HyperVFirewallSupport::None;
}

wil::com_ptr<IWbemClassObject> SpawnWbemObjectInstance(
    _In_ PCWSTR className, const wil::shared_bstr& instanceId, _In_opt_ IWbemContext* wbemContext, const wil::com_ptr<IWbemServices>& wbemService)
{
    // Fetch the class definition
    wil::com_ptr<IWbemClassObject> baseObject;
    THROW_IF_FAILED(wbemService->GetObject(wil::make_bstr(className).get(), WBEM_FLAG_RETURN_WBEM_COMPLETE, wbemContext, &baseObject, nullptr));

    // Create the new object instance
    wil::com_ptr<IWbemClassObject> newObject;
    THROW_IF_FAILED(baseObject->SpawnInstance(0, &newObject));

    // Non-RAII variant as we are not owning the resource here
    VARIANT v{};
    v.vt = VT_BSTR;
    v.bstrVal = instanceId.get();
    THROW_IF_FAILED(newObject->Put(L"InstanceID", 0, &v, 0));

    return newObject;
}

void WriteWMIInstance(_In_opt_ IWbemContext* wbemContext, const wil::com_ptr<IWbemServices>& wbemService, const wil::com_ptr<IWbemClassObject>& newObject)
{
    constexpr WBEM_CHANGE_FLAG_TYPE changeType = WBEM_FLAG_CREATE_OR_UPDATE;
    wil::com_ptr<IWbemCallResult> wmiResult;
    THROW_IF_FAILED_MSG(
        wbemService->PutInstance(newObject.get(), changeType, wbemContext, &wmiResult), "Failed to execute PutInstance WMI call");

    long callStatus;
    THROW_IF_FAILED_MSG(wmiResult->GetCallStatus(WBEM_INFINITE, &callStatus), "Failed to retrieve the WMI call status");
    THROW_IF_FAILED_MSG(callStatus, "Failed to create object instance");
}

HRESULT RegisterHyperVFirewallVmCreator(const GUID& vmCreatorId, const std::wstring& vmCreatorFriendlyName) noexcept
{
    PCSTR executionStep = "";
    try
    {
        executionStep = "CoCreateInstance";
        auto locator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

        executionStep = "ConnectServer";
        // Connect to the root\standardcimv2 namespace with the current user and obtain pointer to make IWbemServices calls.
        wil::com_ptr<IWbemServices> wbemService;
        THROW_IF_FAILED(locator->ConnectServer(
            wil::make_bstr(L"ROOT\\standardcimv2").get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &wbemService));

        executionStep = "CoSetProxyBlanket";
        // Set the IWbemServices proxy so that impersonation of the user (client) occurs.
        THROW_IF_FAILED(CoSetProxyBlanket(
            wbemService.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

        executionStep = "GetNetFirewallHyperVVMCreator";
        // Fetch the class definition
        wil::com_ptr<IWbemClassObject> baseObject;
        THROW_IF_FAILED(wbemService->GetObject(
            wil::make_bstr(L"MSFT_NetFirewallHyperVVMCreator").get(), WBEM_FLAG_RETURN_WBEM_COMPLETE, nullptr, &baseObject, nullptr));

        executionStep = "GetRegisterHyperVVMCreator";
        // Get the parameter object
        wil::com_ptr<IWbemClassObject> paramsObject;
        THROW_IF_FAILED(baseObject->GetMethod(wil::make_bstr(L"RegisterHyperVVMCreator").get(), 0, &paramsObject, nullptr));

        executionStep = "SpawnInstance";
        // Spawn instance for the parameter object
        wil::com_ptr<IWbemClassObject> paramsInstance;
        THROW_IF_FAILED(paramsObject->SpawnInstance(0, &paramsInstance));

        // Fill the parameter object
        wil::unique_variant v;

        executionStep = "PutVMCreatorId";
        // VM Creator Id
        std::wstring vmCreatorIdString =
            wsl::shared::string::GuidToString<wchar_t>(vmCreatorId, wsl::shared::string::GuidToStringFlags::AddBraces);
        v.vt = VT_BSTR;
        v.bstrVal = wil::make_bstr(vmCreatorIdString.c_str()).release();
        THROW_IF_FAILED(paramsInstance->Put(L"VMCreatorId", 0, &v, 0));
        v.reset();

        executionStep = "PutFriendlyName";
        // Friendly name
        v.vt = VT_BSTR;
        v.bstrVal = wil::make_bstr(vmCreatorFriendlyName.c_str()).release();
        THROW_IF_FAILED(paramsInstance->Put(L"FriendlyName", 0, &v, 0));
        v.reset();

        executionStep = "NetFirewallHyperVVMCreator::RegisterHyperVVMCreator";
        // making the recommended semi-synchronous call into WMI
        // which requires waiting for the completion with the resultObject
        wil::com_ptr<IWbemCallResult> resultObject;
        THROW_IF_FAILED(wbemService->ExecMethod(
            wil::make_bstr(L"MSFT_NetFirewallHyperVVMCreator").get(),
            wil::make_bstr(L"RegisterHyperVVMCreator").get(),
            WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            paramsInstance.get(),
            nullptr,
            &resultObject));

        executionStep = "GetResultObject";
        wil::com_ptr<IWbemClassObject> outParams;
        auto result = resultObject->GetResultObject(WBEM_INFINITE, &outParams);
        WSL_LOG(
            "RegisterHyperVFirewallVmCreator [GetResultObject]",
            TraceLoggingValue(
                result == WBEM_E_ALREADY_EXISTS ? "WBEM_E_ALREADY_EXISTS" : std::to_string(result).c_str(), "result"));

        if (result == WBEM_E_ALREADY_EXISTS)
        {
            // the method immediately returned already exists - we're fine to return now.
            result = S_OK;
        }
        THROW_IF_FAILED_MSG(result, "Failed to register hyper-v firewall vm creator");
        return S_OK;
    }
    catch (...)
    {
        auto hr = wil::ResultFromCaughtException();
        WSL_LOG(
            "RegisterHyperVFirewallVmCreatorFailed",
            TraceLoggingValue(hr, "result"),
            TraceLoggingValue(executionStep, "executionStep"));

        return hr;
    }
}

HRESULT ConfigureHyperVFirewallLoopbackAllow(const GUID& vmCreatorId) noexcept
{
    PCSTR executionStep = "";
    try
    {
        executionStep = "CoCreateInstance";
        auto locator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

        executionStep = "CoCreateInstanceWbemContext";
        // Create WbemContext for SystemDefaults
        // SystemDefaults are configured with lowest priority, so admin configuration can overwrite it
        auto wbemContext = wil::CoCreateInstance<WbemContext, IWbemContext>();
        wil::unique_variant v;
        v.vt = VT_BSTR;
        v.bstrVal = wil::make_bstr(L"SystemDefaults").release();
        executionStep = "SetPolicyStore";
        THROW_IF_FAILED(wbemContext->SetValue(L"PolicyStore", 0, &v));
        v.reset();

        executionStep = "ConnectServer";
        // Connect to the root\standardcimv2 namespace with the current user and obtain pointer to make IWbemServices calls.
        wil::com_ptr<IWbemServices> wbemService;
        THROW_IF_FAILED(locator->ConnectServer(
            wil::make_bstr(L"ROOT\\standardcimv2").get(), nullptr, nullptr, nullptr, 0, nullptr, wbemContext.get(), &wbemService));

        executionStep = "CoSetProxyBlanket";
        // Set the IWbemServices proxy so that impersonation of the user (client) occurs.
        THROW_IF_FAILED(CoSetProxyBlanket(
            wbemService.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

        executionStep = "SpawnNetFirewallHyperVVMSetting";
        // Spawn instance
        wil::shared_bstr vmCreatorIdString = wil::make_bstr(
            wsl::shared::string::GuidToString<wchar_t>(vmCreatorId, wsl::shared::string::GuidToStringFlags::AddBraces).c_str());
        wil::com_ptr<IWbemClassObject> settingsObject =
            SpawnWbemObjectInstance(L"MSFT_NetFirewallHyperVVMSetting", vmCreatorIdString, wbemContext.get(), wbemService);

        executionStep = "PutName";
        v.vt = VT_BSTR;
        v.bstrVal = vmCreatorIdString.get();
        const auto hr = settingsObject->Put(L"Name", 0, &v, 0);
        v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
        THROW_IF_FAILED(hr);

        executionStep = "PutLoopbackEnabled";
        v.vt = VT_I4;
        v.lVal = c_true;
        THROW_IF_FAILED(settingsObject->Put(L"LoopbackEnabled", 0, &v, 0));
        v.reset();

        executionStep = "WriteWMIInstance";
        WriteWMIInstance(wbemContext.get(), wbemService, settingsObject);

        return S_OK;
    }
    catch (...)
    {
        auto hr = wil::ResultFromCaughtException();
        WSL_LOG(
            "ConfigureHyperVFirewallLoopbackAllowFailed",
            TraceLoggingValue(hr, "result"),
            TraceLoggingValue(executionStep, "executionStep"));

        return hr;
    }
}

void ConfigureHyperVFirewall(const FirewallConfiguration& firewallConfig, const std::wstring& vmCreatorFriendlyName) noexcept
try
{
    if (!firewallConfig.Enabled())
    {
        return;
    }
    const auto coInit = InitializeCOMState();

    // Register the input ID with the firewall service.
    // If this fails, still proceed with rule creation, as the rules
    // will still be enforced without the vm creator registered
    LOG_IF_FAILED_MSG(
        RegisterHyperVFirewallVmCreator(firewallConfig.VmCreatorId.value(), vmCreatorFriendlyName),
        "RegisterHyperVFirewallVmCreator");

    // Configure firewall settings
    // The OS default is to block loopback. Configure the loopback setting only if the client requests a configuration different than OS default.
    if (FirewallAction::Allow == firewallConfig.DefaultLoopbackPolicy)
    {
        LOG_IF_FAILED_MSG(
            ConfigureHyperVFirewallLoopbackAllow(firewallConfig.VmCreatorId.value()), "ConfigureHyperVFirewallLoopbackAllow");
    }

    // Configure firewall rules
    for (const auto& firewallRule : firewallConfig.Rules)
    {
        if (firewallRule.RuleOperation == wsl::core::FirewallRuleOperation::Add)
        {
            if (FAILED_LOG(AddHyperVFirewallRule(firewallConfig.VmCreatorId.value(), firewallRule)))
            {
                // Due to a Windows bug, certain rules do not accept local ports.
                // If this error is encountered here, we try to instead add a less scoped
                // version of the rule to ensure necessary traffic is still allowed
                FirewallRuleConfiguration currFirewallRule = firewallRule;
                currFirewallRule.LocalPorts.clear();
                LOG_IF_FAILED_MSG(
                    AddHyperVFirewallRule(firewallConfig.VmCreatorId.value(), currFirewallRule),
                    "AddHyperVFirewallRule for %ls",
                    currFirewallRule.RuleId.get());
            }
        }
        else if (firewallRule.RuleOperation == wsl::core::FirewallRuleOperation::Delete)
        {
            LOG_IF_FAILED_MSG(
                RemoveHyperVFirewallRule(firewallRule.RuleId.get()), "RemoveHyperVFirewallRule for %ls", firewallRule.RuleId.get());
        }
        else
        {
            // Unexpected rule operation type
            WI_ASSERT(false);
        }
    }

    // WSL may have previously added this rule (which has since been renamed). Remove it if it is present.
    const auto oldLoopbackRuleId =
        c_hyperVFirewallLoopbackRuleIdPrefix_Old +
        wsl::shared::string::GuidToString<wchar_t>(firewallConfig.VmCreatorId.value(), wsl::shared::string::GuidToStringFlags::None);

    LOG_IF_FAILED_MSG(RemoveHyperVFirewallRule(oldLoopbackRuleId), "RemoveHyperVFirewallRule for %ls", oldLoopbackRuleId.c_str());
}
CATCH_LOG()

HRESULT AddHyperVFirewallRule(const GUID& vmCreatorId, const wsl::core::FirewallRuleConfiguration& firewallRule) noexcept
{
    PCSTR executionStep = "";
    try
    {
        executionStep = "CoCreateInstance";
        auto locator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

        executionStep = "ConnectServer";
        // Connect to the root\standardcimv2 namespace with the current user and obtain pointer to make IWbemServices calls.
        wil::com_ptr<IWbemServices> wbemService;
        THROW_IF_FAILED(locator->ConnectServer(
            wil::make_bstr(L"ROOT\\standardcimv2").get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &wbemService));

        executionStep = "CoSetProxyBlanket";
        // Set the IWbemServices proxy so that impersonation of the user (client) occurs.
        THROW_IF_FAILED(CoSetProxyBlanket(
            wbemService.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

        executionStep = "SpawnNetFirewallHyperVRule";
        wil::com_ptr<IWbemClassObject> ruleObject =
            SpawnWbemObjectInstance(L"MSFT_NetFirewallHyperVRule", firewallRule.RuleId, nullptr, wbemService);

        executionStep = "PutInstanceID";
        // Fill the object
        wil::unique_variant v;
        v.vt = VT_BSTR;
        v.bstrVal = firewallRule.RuleId.get();
        HRESULT hr = ruleObject->Put(L"InstanceID", 0, &v, 0);
        v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
        THROW_IF_FAILED(hr);

        executionStep = "PutElementName";
        v.vt = VT_BSTR;
        v.bstrVal = firewallRule.RuleName.get();
        hr = ruleObject->Put(L"ElementName", 0, &v, 0);
        v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
        THROW_IF_FAILED(hr);

        executionStep = "PutDirection";
        v.vt = VT_I4;
        v.lVal = c_directionInbound;
        THROW_IF_FAILED(ruleObject->Put(L"Direction", 0, &v, 0));
        v.reset();

        executionStep = "PutVMCreatorId";
        std::wstring vmCreatorIdString =
            wsl::shared::string::GuidToString<wchar_t>(vmCreatorId, wsl::shared::string::GuidToStringFlags::AddBraces);
        v.vt = VT_BSTR;
        v.bstrVal = wil::make_bstr(vmCreatorIdString.c_str()).release();
        THROW_IF_FAILED(ruleObject->Put(L"VMCreatorId", 0, &v, 0));
        v.reset();

        executionStep = "PutAction";
        v.vt = VT_I4;
        v.lVal = c_actionAllow;
        THROW_IF_FAILED(ruleObject->Put(L"Action", 0, &v, 0));
        v.reset();

        executionStep = "PutEnabled";
        v.vt = VT_I4;
        v.lVal = c_ruleEnabled;
        THROW_IF_FAILED(ruleObject->Put(L"Enabled", 0, &v, 0));
        v.reset();

        executionStep = "PutProtocol";
        if (firewallRule.Protocol.is_valid())
        {
            v.vt = VT_BSTR;
            v.bstrVal = firewallRule.Protocol.get();
            hr = ruleObject->Put(L"Protocol", 0, &v, 0);
            v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
            THROW_IF_FAILED(hr);
        }

        executionStep = "PutLocalPorts";
        if (!firewallRule.LocalPorts.empty())
        {
            // Convert to a safe array for usage in WMI
            CComSafeArray<BSTR> localPortsArray;
            THROW_IF_FAILED(localPortsArray.Create());
            for (const auto& localPort : firewallRule.LocalPorts)
            {
                THROW_IF_FAILED(localPortsArray.Add(localPort.get()));
            }
            v.vt = (VT_BSTR | VT_ARRAY);
            v.parray = localPortsArray.Detach();
            THROW_IF_FAILED(ruleObject->Put(L"LocalPorts", 0, &v, 0));
            v.reset();
        }

        executionStep = "PutLocalAddresses";
        if (!firewallRule.LocalAddresses.empty())
        {
            // Convert to a safe array for usage in WMI
            CComSafeArray<BSTR> localAddressesArray;
            THROW_IF_FAILED(localAddressesArray.Create());
            for (const auto& localAddress : firewallRule.LocalAddresses)
            {
                THROW_IF_FAILED(localAddressesArray.Add(localAddress.get()));
            }
            v.vt = (VT_BSTR | VT_ARRAY);
            v.parray = localAddressesArray.Detach();
            THROW_IF_FAILED(ruleObject->Put(L"LocalAddresses", 0, &v, 0));
            v.reset();
        }

        executionStep = "PutRemoteAddresses";
        if (!firewallRule.RemoteAddresses.empty())
        {
            // Convert to a safe array for usage in WMI
            CComSafeArray<BSTR> remoteAddressesArray;
            THROW_IF_FAILED(remoteAddressesArray.Create());
            for (const auto& remoteAddress : firewallRule.RemoteAddresses)
            {
                THROW_IF_FAILED(remoteAddressesArray.Add(remoteAddress.get()));
            }
            v.vt = (VT_BSTR | VT_ARRAY);
            v.parray = remoteAddressesArray.Detach();
            THROW_IF_FAILED(ruleObject->Put(L"RemoteAddresses", 0, &v, 0));
            v.reset();
        }

        executionStep = "WriteWMIInstance";
        WriteWMIInstance(nullptr, wbemService, ruleObject);

        return S_OK;
    }
    catch (...)
    {
        auto hr = wil::ResultFromCaughtException();
        WSL_LOG("AddHyperVFirewallRuleFailed", TraceLoggingValue(hr, "result"), TraceLoggingValue(executionStep, "executionStep"));
        return hr;
    }
}

HRESULT RemoveHyperVFirewallRule(const std::wstring& ruleId) noexcept
{
    PCSTR executionStep = "";
    try
    {
        executionStep = "CoCreateInstance";
        const auto locator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

        executionStep = "ConnectServer";
        // Connect to the root\standardcimv2 namespace with the current user and obtain pointer to make IWbemServices calls.
        wil::com_ptr<IWbemServices> wbemService;
        THROW_IF_FAILED(locator->ConnectServer(
            wil::make_bstr(L"ROOT\\standardcimv2").get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &wbemService));

        executionStep = "CoSetProxyBlanket";
        // Set the IWbemServices proxy so that impersonation of the user (client) occurs.
        THROW_IF_FAILED(CoSetProxyBlanket(
            wbemService.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

        // Create the rule deletion query string
        const std::wstring ruleDeletionString = std::format(L"MSFT_NetFirewallHyperVRule.InstanceId=\"{}\"", ruleId);
        const wil::unique_bstr ruleDeletionBstr = wil::make_bstr(ruleDeletionString.c_str());

        executionStep = "DeleteInstance";
        // Delete the instance to WMI
        wil::com_ptr<IWbemCallResult> wmiResult;
        THROW_IF_FAILED_MSG(
            wbemService->DeleteInstance(ruleDeletionBstr.get(), 0, nullptr, &wmiResult),
            "Failed to execute the WMI call for deleting the hyper-v firewall ruleId=%ws",
            ruleId.c_str());

        executionStep = "GetCallStatus";
        long callStatus;
        THROW_IF_FAILED_MSG(
            wmiResult->GetCallStatus(WBEM_INFINITE, &callStatus),
            "Failed to retrieve the WMI call status for deleting the hyper-v firewall ruleId=%ws",
            ruleId.c_str());

        // Ignore error not found, as this indicates the rule is already deleted
        if (callStatus == WBEM_E_NOT_FOUND)
        {
            callStatus = S_OK;
        }
        THROW_IF_FAILED_MSG(callStatus, "Failed to delete hyper-v firewall rule with ruleId=%ws", ruleId.c_str());
        return S_OK;
    }
    catch (...)
    {
        auto hr = wil::ResultFromCaughtException();
        WSL_LOG(
            "RemoveHyperVFirewallRuleFailed", TraceLoggingValue(hr, "result"), TraceLoggingValue(executionStep, "executionStep"));

        return hr;
    }
}

HRESULT AddHostFirewallRule(const wsl::core::FirewallRuleConfiguration& firewallRule) noexcept
try
{
    auto wbemLocator = wil::CoCreateInstance<WbemLocator, IWbemLocator>();

    // Create WbemContext for ActiveStore
    // ActiveStore is used so that the rules are not persisted and therefore not leaked upon uninstall
    auto wbemContext = wil::CoCreateInstance<WbemContext, IWbemContext>();
    wil::unique_variant v;
    v.vt = VT_BSTR;
    v.bstrVal = wil::make_bstr(L"ActiveStore").release();
    THROW_IF_FAILED(wbemContext->SetValue(L"PolicyStore", 0, &v));
    v.reset();

    // Connect to the root\standardcimv2 namespace with the current user and obtain pointer to make IWbemServices calls.
    wil::com_ptr<IWbemServices> wbemService;
    THROW_IF_FAILED(wbemLocator->ConnectServer(
        wil::make_bstr(L"ROOT\\standardcimv2").get(), nullptr, nullptr, nullptr, 0, nullptr, wbemContext.get(), &wbemService));

    // Set the IWbemServices proxy so that impersonation of the user (client) occurs.
    THROW_IF_FAILED(CoSetProxyBlanket(
        wbemService.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE));

    wil::com_ptr<IWbemClassObject> ruleObject =
        SpawnWbemObjectInstance(L"MSFT_NetFirewallRule", firewallRule.RuleId, wbemContext.get(), wbemService);

    v.vt = VT_BSTR;
    v.bstrVal = firewallRule.RuleName.get();
    auto hr = ruleObject->Put(L"ElementName", 0, &v, 0);
    v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
    THROW_IF_FAILED(hr);

    v.vt = VT_I4;
    v.lVal = c_directionInbound;
    THROW_IF_FAILED(ruleObject->Put(L"Direction", 0, &v, 0));
    v.reset();

    v.vt = VT_I4;
    v.lVal = c_actionAllow;
    THROW_IF_FAILED(ruleObject->Put(L"Action", 0, &v, 0));
    v.reset();

    // Create the rule initially in the disabled state so that we can add the proper associated objects with the correct scoping of the rule
    v.vt = VT_I4;
    v.lVal = c_ruleDisabled;
    THROW_IF_FAILED(ruleObject->Put(L"Enabled", 0, &v, 0));
    v.reset();

    v.vt = VT_BSTR;
    v.bstrVal = wil::make_bstr(L"ActiveStore").release();
    THROW_IF_FAILED(ruleObject->Put(L"PolicyStoreSource", 0, &v, 0));
    v.reset();

    WriteWMIInstance(wbemContext.get(), wbemService, ruleObject);

    // Firewall WMI uses associated instances for many rule conditions. These are created as separate objects (if the input parameter uses the fields)

    if (firewallRule.Protocol.is_valid() || !firewallRule.LocalPorts.empty())
    {
        wil::com_ptr<IWbemClassObject> protocolPortObject =
            SpawnWbemObjectInstance(L"MSFT_NetProtocolPortFilter", firewallRule.RuleId, wbemContext.get(), wbemService);

        if (firewallRule.Protocol.is_valid())
        {
            v.vt = VT_BSTR;
            v.bstrVal = firewallRule.Protocol.get();
            hr = protocolPortObject->Put(L"Protocol", 0, &v, 0);
            v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
            THROW_IF_FAILED(hr);
        }

        if (!firewallRule.LocalPorts.empty())
        {
            // Convert to a safe array for usage in WMI
            CComSafeArray<BSTR> localPortsArray;
            THROW_IF_FAILED(localPortsArray.Create());
            for (const auto& localPort : firewallRule.LocalPorts)
            {
                THROW_IF_FAILED(localPortsArray.Add(localPort.get()));
            }
            v.vt = (VT_BSTR | VT_ARRAY);
            v.parray = localPortsArray.Detach();
            THROW_IF_FAILED(protocolPortObject->Put(L"LocalPort", 0, &v, 0));
            v.reset();
        }

        WriteWMIInstance(wbemContext.get(), wbemService, protocolPortObject);
    }

    if (firewallRule.LocalApplication.is_valid())
    {
        wil::com_ptr<IWbemClassObject> applicationObject =
            SpawnWbemObjectInstance(L"MSFT_NetApplicationFilter", firewallRule.RuleId, wbemContext.get(), wbemService);

        v.vt = VT_BSTR;
        v.bstrVal = firewallRule.LocalApplication.get();
        hr = applicationObject->Put(L"AppPath", 0, &v, 0);
        v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
        THROW_IF_FAILED(hr);

        WriteWMIInstance(wbemContext.get(), wbemService, applicationObject);
    }

    if (firewallRule.LocalService.is_valid())
    {
        wil::com_ptr<IWbemClassObject> serviceObject =
            SpawnWbemObjectInstance(L"MSFT_NetServiceFilter", firewallRule.RuleId, wbemContext.get(), wbemService);

        v.vt = VT_BSTR;
        v.bstrVal = firewallRule.LocalService.get();
        hr = serviceObject->Put(L"ServiceName", 0, &v, 0);
        v.release(); // the variant should not free the bstr, it's owned by the wil::shared_bstr
        THROW_IF_FAILED(hr);

        WriteWMIInstance(wbemContext.get(), wbemService, serviceObject);
    }

    if (!firewallRule.RemoteAddresses.empty())
    {
        wil::com_ptr<IWbemClassObject> addressObject =
            SpawnWbemObjectInstance(L"MSFT_NetAddressFilter", firewallRule.RuleId, wbemContext.get(), wbemService);

        // Convert to a safe array for usage in WMI
        CComSafeArray<BSTR> remoteAddressesArray;
        THROW_IF_FAILED(remoteAddressesArray.Create());
        for (const auto& remoteAddress : firewallRule.RemoteAddresses)
        {
            THROW_IF_FAILED(remoteAddressesArray.Add(remoteAddress.get()));
        }
        v.vt = (VT_BSTR | VT_ARRAY);
        v.parray = remoteAddressesArray.Detach();
        THROW_IF_FAILED(addressObject->Put(L"RemoteAddress", 0, &v, 0));
        v.reset();

        WriteWMIInstance(wbemContext.get(), wbemService, addressObject);
    }

    // After necessary associated objects are created, we can now enable the rule
    ruleObject = SpawnWbemObjectInstance(L"MSFT_NetFirewallRule", firewallRule.RuleId, wbemContext.get(), wbemService);

    v.vt = VT_I4;
    v.lVal = c_ruleEnabled;
    THROW_IF_FAILED(ruleObject->Put(L"Enabled", 0, &v, 0));
    v.reset();

    WriteWMIInstance(wbemContext.get(), wbemService, ruleObject);
    return S_OK;
}
CATCH_RETURN()

void ConfigureSharedAccessFirewallRule() noexcept
{
    // Configures necessary host firewall rules:
    //    -Inbound rule to allow UDP traffic to port 53 for the SharedAccess service. This allows the proxied DNS requests to the host.
    LPCWSTR sharedAccessRulePorts[] = {L"53"};
    const wsl::core::FirewallRuleConfiguration sharedAccessRule(
        c_sharedAccessRuleId, c_sharedAccessRuleName, c_protocolUDP, 1, sharedAccessRulePorts, 0, nullptr, 0, nullptr, c_sharedAccessService, c_svchostApplication);
    LOG_IF_FAILED_MSG(AddHostFirewallRule(sharedAccessRule), "AddHostFirewallRule::sharedAccessRule");
}

} // namespace wsl::core::networking