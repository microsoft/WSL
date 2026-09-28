/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssIptables.cpp

Abstract:

    This file contains iptables-related function definitions.

--*/

#include "precomp.h"
#include <mi.h>
#include "LxssIpTables.h"

using unique_safearray = wil::unique_any<SAFEARRAY*, decltype(&SafeArrayDestroy), SafeArrayDestroy>;

using namespace std::placeholders;

// LxssIpTables class functions.

LxssIpTables::LxssIpTables()
{
    return;
}

std::wstring LxssIpTables::AddressStringFromAddress(const IP_ADDRESS_PREFIX& Address, bool AddPrefixLength)
{
    std::wstringstream addressStream;
    addressStream << Address.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b1 << L"." << Address.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b2 << L"."
                  << Address.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b3 << L"." << Address.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b4;

    if (AddPrefixLength)
    {
        addressStream << L"/" << Address.PrefixLength;
    }

    return addressStream.str();
}

void LxssIpTables::CleanupRemnants()
{
    try
    {
        LxssNetworkingNat::CleanupRemnants();
    }
    CATCH_LOG()

    try
    {
        LxssNetworkingFirewall::CleanupRemnants();
    }
    CATCH_LOG()
}

void LxssIpTables::EnableIpTablesSupport(_In_ const wil::unique_handle& InstanceHandle)
{
    //
    // Passing 'this' is OK because unregistration will be done in the
    // destructor.
    //

    auto callback = std::bind(KernelCallbackProxy, this, _1, _2);
    m_kernelCallback =
        LxssUserCallback::Register(InstanceHandle.get(), LxBusUserCallbackTypeIptables, callback, sizeof(LXBUS_USER_CALLBACK_NETWORK_DATA));
}

bool LxssIpTables::IsAllowedInputPrefix(_In_ CONST IP_ADDRESS_PREFIX& InputPrefix)
{
    if (InputPrefix.Prefix.si_family != AF_INET)
    {
        LOG_HR_MSG(E_NOTIMPL, "IPv6 addresses for NAT not supported");
        return false;
    }

    if (InputPrefix.Prefix.Ipv4.sin_port != 0)
    {
        LOG_HR_MSG(E_NOTIMPL, "Specific ports for NAT not supported");
        return false;
    }

    // TODO_LX: Currently there is an agreement in place with HNS to restrict
    //          the NAT address range to 172.17.0.0/16.
    if ((InputPrefix.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b1 != 172) || (InputPrefix.Prefix.Ipv4.sin_addr.S_un.S_un_b.s_b2 != 17) ||
        (InputPrefix.PrefixLength < 16))
    {
        LOG_HR_MSG(E_NOTIMPL, "Address not supported for NAT: %ls", LxssIpTables::AddressStringFromAddress(InputPrefix, true).c_str());

        return false;
    }

    return true;
}

NTSTATUS
LxssIpTables::KernelCallback(_In_ PVOID CallbackBuffer, _In_ ULONG_PTR CallbackBufferSize)
{
    if (CallbackBufferSize < sizeof(LXBUS_USER_CALLBACK_IPTABLES_DATA))
    {
        WI_ASSERT_MSG(false, "Kernel provided unexpected data for user-mode callback.");

        return STATUS_INVALID_PARAMETER;
    }

    const auto callbackData = static_cast<PLXBUS_USER_CALLBACK_IPTABLES_DATA>(CallbackBuffer);

    switch (callbackData->IptablesDataType)
    {
    case LxBusUserCallbackIptablesDataTypeMasquerade:
        return KernelCallbackMasquerade(callbackData);
    case LxBusUserCallbackIptablesDataTypePort:
        return KernelCallbackFirewallPort(callbackData);
    default:
        WI_ASSERT_MSG(false, "Kernel provided unexpected data for user-mode callback.");

        return STATUS_INVALID_PARAMETER;
    }
}

NTSTATUS
LxssIpTables::KernelCallbackFirewallPort(_In_ PLXBUS_USER_CALLBACK_IPTABLES_DATA CallbackData)
{
    CONST IP_ADDRESS_PREFIX& inputPrefix = CallbackData->Data.Port.InputPrefix;

    if (inputPrefix.Prefix.si_family != AF_INET)
    {
        LOG_HR_MSG(E_INVALIDARG, "IPv6 addresses for firewall ports not supported");

        return STATUS_INVALID_PARAMETER;
    }

    if (inputPrefix.Prefix.Ipv4.sin_port == 0)
    {
        LOG_HR_MSG(E_INVALIDARG, "No port specified");
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status;
    std::lock_guard<std::mutex> lock(m_lock);
    if (CallbackData->Data.Port.Enable == FALSE)
    {
        status = STATUS_NOT_FOUND;
        const auto foundEntry = std::find_if(m_firewallPorts.begin(), m_firewallPorts.end(), [&](const auto& next) {
            return (memcmp(&inputPrefix, &next->Address(), sizeof(inputPrefix)) == 0);
        });

        if (foundEntry != m_firewallPorts.end())
        {
            status = STATUS_INVALID_PARAMETER;
            try
            {
                m_firewallPorts.erase(foundEntry);
                status = STATUS_SUCCESS;
            }
            CATCH_LOG_MSG("Failed to remove firewall port rule.")
        }
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;
        try
        {
            std::shared_ptr<LxssNetworkingFirewall> firewall;
            if (m_firewallPorts.empty())
            {
                firewall = std::make_shared<LxssNetworkingFirewall>();
            }
            else
            {
                firewall = m_firewallPorts.front()->Firewall();
            }

            auto newPortRule = std::make_unique<LxssNetworkingFirewallPort>(firewall, inputPrefix);

            m_firewallPorts.emplace_back(std::move(newPortRule));
            status = STATUS_SUCCESS;
        }
        CATCH_LOG_MSG("Failed to create new firewall port rule.")
    }

    return status;
}

NTSTATUS
LxssIpTables::KernelCallbackMasquerade(_In_ PLXBUS_USER_CALLBACK_IPTABLES_DATA CallbackData)
{
    CONST IP_ADDRESS_PREFIX& inputPrefix = CallbackData->Data.Masquerade.InputPrefix;

    if (!IsAllowedInputPrefix(inputPrefix))
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS status;
    std::lock_guard<std::mutex> lock(m_lock);
    if (CallbackData->Data.Masquerade.Enable == FALSE)
    {
        status = STATUS_NOT_FOUND;
        const auto foundEntry = std::find_if(m_networkTranslators.begin(), m_networkTranslators.end(), [&](const auto& next) {
            return (memcmp(&inputPrefix, &next->Address(), sizeof(inputPrefix)) == 0);
        });

        if (foundEntry != m_networkTranslators.end())
        {
            status = STATUS_INVALID_PARAMETER;
            try
            {
                m_networkTranslators.erase(foundEntry);
                status = STATUS_SUCCESS;
            }
            CATCH_LOG_MSG("Failed to remove NAT.")
        }
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;
        try
        {
            auto newNat = std::make_unique<LxssNetworkingNat>(inputPrefix);
            m_networkTranslators.emplace_back(std::move(newNat));
            status = STATUS_SUCCESS;
        }
        CATCH_LOG_MSG("Failed to create new NAT.")
    }

    return status;
}

NTSTATUS
LxssIpTables::KernelCallbackProxy(_Inout_ LxssIpTables* Self, _In_ PVOID CallbackBuffer, _In_ ULONG_PTR CallbackBufferSize)
{
    return Self->KernelCallback(CallbackBuffer, CallbackBufferSize);
}

// LxssManagementInterface class functions.

std::weak_ptr<MI_Application> LxssManagementInterface::s_application;
const std::wstring LxssManagementInterface::s_localRoot(L"ROOT/StandardCimv2");
std::mutex LxssManagementInterface::s_lock;

unique_mi_instance LxssManagementInterface::CloneInstance(_In_ const MI_Instance* InstanceToClone)
{
    MI_Instance* rawInstance;
    const MI_Result result = MI_Instance_Clone(InstanceToClone, &rawInstance);
    THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

    return unique_mi_instance(rawInstance, std::bind(CloseInstance, GetGlobalApplication(), _1));
}

void LxssManagementInterface::CloseGlobalApplication(_Inout_ MI_Application* Application)
{
    WI_VERIFY(MI_Application_Close(Application) == MI_RESULT_OK);
    delete Application;
}

void LxssManagementInterface::CloseInstance(_In_ std::shared_ptr<MI_Application>, _Inout_ MI_Instance* Instance)
{
    WI_VERIFY(MI_Instance_Delete(Instance) == MI_RESULT_OK);
}

void LxssManagementInterface::CloseOperation(_Inout_ MI_Operation* Operation)
{
    // If an operation is in progress, close will wait for it to complete.
    // Always attempt to cancel the operation before closing it.
    (void)MI_Operation_Cancel(Operation, MI_REASON_NONE);
    WI_VERIFY(MI_Operation_Close(Operation) == MI_RESULT_OK);
}

void LxssManagementInterface::CloseSession(_In_ std::shared_ptr<MI_Application>, _Inout_ MI_Session* Session)
{
    WI_VERIFY(MI_Session_Close(Session, NULL, NULL) == MI_RESULT_OK);
    delete Session;
}

std::shared_ptr<MI_Application> LxssManagementInterface::GetGlobalApplication()
{
    std::shared_ptr<MI_Application> globalApplicationInstance;
    std::lock_guard<std::mutex> lock(s_lock);
    globalApplicationInstance = s_application.lock();
    if (!globalApplicationInstance)
    {
        std::unique_ptr<MI_Application> rawApplication(new MI_Application);
        const MI_Result result = MI_Application_Initialize(0, nullptr, nullptr, rawApplication.get());

        THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

        globalApplicationInstance = std::shared_ptr<MI_Application>(rawApplication.release(), CloseGlobalApplication);

        s_application = globalApplicationInstance;
    }

    return globalApplicationInstance;
}

unique_mi_instance LxssManagementInterface::NewInstance(_In_ const std::wstring& ClassName, _In_opt_ const MI_Class* Class)
{
    auto application = GetGlobalApplication();
    MI_Result result;
    MI_Instance* rawInstance;
    if (Class == nullptr)
    {
        result = MI_Application_NewInstance(application.get(), ClassName.c_str(), nullptr, &rawInstance);
    }
    else
    {
        result = MI_Application_NewInstanceFromClass(application.get(), ClassName.c_str(), Class, &rawInstance);
    }

    THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

    WI_ASSERT(rawInstance != nullptr);

    return unique_mi_instance(rawInstance, std::bind(CloseInstance, application, _1));
}

unique_mi_session LxssManagementInterface::NewSession()
{
    auto application = GetGlobalApplication();
    std::unique_ptr<MI_Session> rawSession(new MI_Session());
    const MI_Result result = MI_Application_NewSession(application.get(), nullptr, nullptr, nullptr, nullptr, nullptr, rawSession.get());

    THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

    return unique_mi_session(rawSession.release(), std::bind(CloseSession, application, _1));
}

// LxssNetworkingFirewall class functions

const wil::unique_bstr LxssNetworkingFirewall::s_DefaultRuleDescription(wil::make_bstr_failfast(L"WSL iptables entry"));

const std::wstring LxssNetworkingFirewall::s_FriendlyNamePrefix(L"WSLRULE_17774471984f_");

LxssNetworkingFirewall::LxssNetworkingFirewall()
{
    m_firewall = wil::CoCreateInstance<NetFwPolicy2, INetFwPolicy2>(CLSCTX_INPROC_SERVER);
}

void LxssNetworkingFirewall::CopyPartialArray(SAFEARRAY* Destination, SAFEARRAY* Source, ULONG DestinationIndexStart, ULONG SourceIndexStart, ULONG ElementsToCopy)
{
    if (ElementsToCopy == 0)
    {
        return;
    }

    // Sanity check destination
    THROW_HR_IF(E_INVALIDARG, SafeArrayGetDim(Destination) != 1);
    LONG firstIndex;
    // Only expecting arrays to start at 0, so enforce that for now.
    THROW_IF_FAILED(SafeArrayGetLBound(Destination, 1, &firstIndex));
    THROW_HR_IF(E_INVALIDARG, (firstIndex != 0));
    ULONG lastIndex;
    THROW_IF_FAILED(SafeArrayGetUBound(Destination, 1, (PLONG)&lastIndex));
    ULONG requestedLastIndex;
    THROW_IF_FAILED(ULongAdd(DestinationIndexStart, (ElementsToCopy - 1), &requestedLastIndex));

    THROW_HR_IF(E_INVALIDARG, (requestedLastIndex > lastIndex));
    // Sanity check source.
    THROW_HR_IF(E_INVALIDARG, SafeArrayGetDim(Source) != 1);
    // Only expecting arrays to start at 0, so enforce that for now.
    THROW_IF_FAILED(SafeArrayGetLBound(Source, 1, &firstIndex));
    THROW_HR_IF(E_INVALIDARG, (firstIndex != 0));
    THROW_IF_FAILED(SafeArrayGetUBound(Source, 1, (PLONG)&lastIndex));
    THROW_IF_FAILED(ULongAdd(SourceIndexStart, (ElementsToCopy - 1), &requestedLastIndex));

    THROW_HR_IF(E_INVALIDARG, (requestedLastIndex > lastIndex));
    // Perform the copy.
    ULONG curSourceIndex = SourceIndexStart;
    ULONG curDestIndex = DestinationIndexStart;
    THROW_IF_FAILED(SafeArrayLock(Source));
    auto releaseSource = wil::scope_exit([Source]() { WI_VERIFY(SUCCEEDED(SafeArrayUnlock(Source))); });

    for (ULONG index = 0; index < ElementsToCopy; index += 1)
    {
        VARIANT* nextAdapter;
        THROW_IF_FAILED(SafeArrayPtrOfIndex(Source, (PLONG)&curSourceIndex, (PVOID*)&nextAdapter));

        curSourceIndex += 1;
        THROW_IF_FAILED(SafeArrayPutElement(Destination, (PLONG)&curDestIndex, nextAdapter));

        curDestIndex += 1;
    }

    return;
}

std::wstring LxssNetworkingFirewall::AddPortRule(const IP_ADDRESS_PREFIX& Address) const
{
    auto newRule = wil::CoCreateInstance<NetFwRule, INetFwRule>(CLSCTX_INPROC_SERVER);

    // Open a port via the firewall by creating a rule that specifies the local
    // address and the local port to allow. Currently this rule only applies to
    // the public profile as that is the one invoked with traffic between
    // network compartments.
    THROW_IF_FAILED(newRule->put_Action(NET_FW_ACTION_ALLOW));
    THROW_IF_FAILED(newRule->put_Direction(NET_FW_RULE_DIR_IN));
    THROW_IF_FAILED(newRule->put_Profiles(NET_FW_PROFILE2_PUBLIC));
    THROW_IF_FAILED(newRule->put_Protocol(NET_FW_IP_PROTOCOL_TCP));
    const std::wstring addressString = LxssIpTables::AddressStringFromAddress(Address, false);

    const auto localAddress = wil::make_bstr_failfast(addressString.c_str());
    THROW_IF_FAILED(newRule->put_LocalAddresses(localAddress.get()));
    const auto localPort = wil::make_bstr_failfast(std::to_wstring(Address.Prefix.Ipv4.sin_port).c_str());

    THROW_IF_FAILED(newRule->put_LocalPorts(localPort.get()));
    std::wstring generatedName = GeneratePortRuleName(Address);
    const auto friendlyName = wil::make_bstr_failfast(generatedName.c_str());
    THROW_IF_FAILED(newRule->put_Name(friendlyName.get()));
    THROW_IF_FAILED(newRule->put_Description(s_DefaultRuleDescription.get()));
    THROW_IF_FAILED(newRule->put_Enabled(VARIANT_TRUE));
    // Add the rule to the existing set.
    wil::com_ptr<INetFwRules> rules;
    THROW_IF_FAILED(m_firewall->get_Rules(&rules));
    THROW_IF_FAILED(rules->Add(newRule.get()));
    // Return the unique rule name to the caller.
    return generatedName;
}

void LxssNetworkingFirewall::CleanupRemnants()
{
    auto firewall = std::make_shared<LxssNetworkingFirewall>();
    THROW_HR_IF(E_OUTOFMEMORY, !firewall);
    wil::com_ptr<INetFwRules> rules;
    THROW_IF_FAILED(firewall->m_firewall->get_Rules(&rules));
    wil::com_ptr<IUnknown> enumInterface;
    THROW_IF_FAILED(rules->get__NewEnum(enumInterface.addressof()));
    auto rulesEnum = enumInterface.query<IEnumVARIANT>();
    // Find any rules with the unique WSL prefix and destroy them.
    for (;;)
    {
        wil::unique_variant next;
        ULONG numEntries;
        THROW_IF_FAILED(rulesEnum->Next(1, &next, &numEntries));
        if (numEntries == 0)
        {
            break;
        }

        wil::com_ptr<INetFwRule> nextRule;
        THROW_IF_FAILED(next.pdispVal->QueryInterface(IID_PPV_ARGS(&nextRule)));
        wil::unique_bstr nextRuleName;
        THROW_IF_FAILED(nextRule->get_Name(nextRuleName.addressof()));
        if (wsl::shared::string::StartsWith(nextRuleName.get(), s_FriendlyNamePrefix.c_str(), true))
        {
            // The firewall port rule will be destroyed when it goes out of
            // scope.
            LxssNetworkingFirewallPort tempPort(firewall, nextRule);
        }
    }
}

std::wstring LxssNetworkingFirewall::GeneratePortRuleName(const IP_ADDRESS_PREFIX& Address)
{
    std::wstringstream nameStream;
    nameStream << s_FriendlyNamePrefix << LxssIpTables::AddressStringFromAddress(Address, false) << L":"
               << std::to_wstring(Address.Prefix.Ipv4.sin_port);

    return nameStream.str();
}

wil::unique_variant LxssNetworkingFirewall::GetExcludedAdapters(_Out_opt_ ULONG* AdapterCount) const
{
    wil::unique_variant excludedResult;
    THROW_IF_FAILED(m_firewall->get_ExcludedInterfaces(NET_FW_PROFILE2_PUBLIC, excludedResult.addressof()));

    if (excludedResult.vt == VT_EMPTY)
    {
        // If there are no entries create a 0-element array so that callers can
        // always assume the returned variant contains a safe array.
        excludedResult.parray = SafeArrayCreateVector(VT_VARIANT, 0, 0);
        THROW_HR_IF_NULL(E_OUTOFMEMORY, excludedResult.parray);
        excludedResult.vt = (VT_ARRAY | VT_VARIANT);
    }

    THROW_HR_IF_MSG(E_UNEXPECTED, (excludedResult.vt != (VT_ARRAY | VT_VARIANT)), "Unexpected type from get_ExcludedInterfaces");

    SAFEARRAY* existingAdapters = excludedResult.parray;
    THROW_HR_IF_MSG(E_UNEXPECTED, (SafeArrayGetDim(existingAdapters) != 1), "Unexpected array dim from get_ExcludedInterfaces");

    LONG firstIndex;
    THROW_IF_FAILED(SafeArrayGetLBound(existingAdapters, 1, &firstIndex));

    THROW_HR_IF_MSG(E_UNEXPECTED, (firstIndex != 0), "Unexpected array (l) from get_ExcludedInterfaces");

    LONG lastIndex;
    THROW_IF_FAILED(SafeArrayGetUBound(existingAdapters, 1, &lastIndex));

    if (ARGUMENT_PRESENT(AdapterCount))
    {
        // A zero-element array reports the upper bound as -1 because it
        // subtracts one from the element count (0) to get the upper bound.
        *AdapterCount = (static_cast<ULONG>(lastIndex) + 1);
    }

    return excludedResult;
}

void LxssNetworkingFirewall::ExcludeAdapter(const std::wstring& AdapterName)
{
    ULONG adapterCount;
    wil::unique_variant currentExcluded = GetExcludedAdapters(&adapterCount);
    THROW_HR_IF_MSG(E_BOUNDS, (adapterCount >= ULONG_MAX), "Unexpected array (u) from get_ExcludedInterfaces");

    SAFEARRAY* existingAdapters = currentExcluded.parray;
    unique_safearray adapters(SafeArrayCreateVector(VT_VARIANT, 0, (adapterCount + 1)));

    THROW_HR_IF(E_OUTOFMEMORY, !adapters);
    // Add existing entries
    CopyPartialArray(adapters.get(), existingAdapters, 0, 0, adapterCount);
    // Create new entry matching device name.
    wil::unique_variant interfaceName;
    interfaceName.bstrVal = SysAllocString(AdapterName.c_str());
    THROW_HR_IF_NULL(E_OUTOFMEMORY, interfaceName.bstrVal);
    THROW_IF_FAILED(SafeArrayPutElement(adapters.get(), (PLONG)&adapterCount, interfaceName.addressof()));

    wil::unique_variant excludedAdapters;
    excludedAdapters.vt = (VT_ARRAY | VT_VARIANT);
    excludedAdapters.parray = adapters.release();
    THROW_IF_FAILED(m_firewall->put_ExcludedInterfaces(NET_FW_PROFILE2_PUBLIC, excludedAdapters));
}

void LxssNetworkingFirewall::RemoveExcludedAdapter(const std::wstring& AdapterName)
{
    ULONG adapterCount;
    wil::unique_variant currentExcluded = GetExcludedAdapters(&adapterCount);
    SAFEARRAY* existingAdapters = currentExcluded.parray;
    ULONG index;
    for (index = 0; index < adapterCount; index += 1)
    {
        wil::unique_variant nextAdapter;
        THROW_IF_FAILED(SafeArrayGetElement(existingAdapters, (PLONG)&index, nextAdapter.addressof()));

        THROW_HR_IF(E_UNEXPECTED, (nextAdapter.vt != VT_BSTR));
        // Case-insensitive name comparison
        if (wsl::shared::string::IsEqual(AdapterName, nextAdapter.bstrVal, true))
        {
            break;
        }
    }

    THROW_HR_IF(E_INVALIDARG, (index >= adapterCount));
    unique_safearray adapters(SafeArrayCreateVector(VT_VARIANT, 0, (adapterCount - 1)));

    THROW_HR_IF(E_OUTOFMEMORY, !adapters);
    // Copy all of the elements except the one being removed.
    CopyPartialArray(adapters.get(), existingAdapters, 0, 0, index);
    CopyPartialArray(adapters.get(), existingAdapters, index, (index + 1), (adapterCount - (index + 1)));

    wil::unique_variant excludedAdapters;
    excludedAdapters.vt = (VT_ARRAY | VT_VARIANT);
    excludedAdapters.parray = adapters.release();
    THROW_IF_FAILED(m_firewall->put_ExcludedInterfaces(NET_FW_PROFILE2_PUBLIC, excludedAdapters));
}

void LxssNetworkingFirewall::RemovePortRule(const std::wstring& RuleName) const
{
    wil::com_ptr<INetFwRules> rules;
    THROW_IF_FAILED(m_firewall->get_Rules(&rules));
    THROW_IF_FAILED(rules->Remove(wil::make_bstr_failfast(RuleName.c_str()).get()));
}

// LxssNetworkingFirewallPort class functions

LxssNetworkingFirewallPort::LxssNetworkingFirewallPort(const std::shared_ptr<LxssNetworkingFirewall>& Firewall, const IP_ADDRESS_PREFIX& Address) :
    m_address(Address), m_firewall(Firewall)
{
    m_name = Firewall->AddPortRule(Address);
    return;
}

LxssNetworkingFirewallPort::LxssNetworkingFirewallPort(const std::shared_ptr<LxssNetworkingFirewall>& Firewall, const wil::com_ptr<INetFwRule>& Existing) :
    m_firewall(Firewall)
{
    wil::unique_bstr ruleName;
    THROW_IF_FAILED(Existing->get_Name(ruleName.addressof()));
    m_name = ruleName.get();
    return;
}

LxssNetworkingFirewallPort::~LxssNetworkingFirewallPort()
{
    try
    {
        m_firewall->RemovePortRule(m_name);
    }
    CATCH_LOG_MSG("Failed to remove firewall port rule.")
}

// LxssNetworkingNat class functions

// N.B. The name is internally limited by NAT to 39 characters.
const std::wstring LxssNetworkingNat::s_FriendlyNamePrefix(L"WSLNAT_17774471984f_");

const std::wstring LxssNetworkingNat::s_WmiNatInstanceId(L"InstanceID");
const std::wstring LxssNetworkingNat::s_WmiNatInternalIpAddress(L"InternalIPInterfaceAddressPrefix");

const std::wstring LxssNetworkingNat::s_WmiNatName(L"Name");
const std::wstring LxssNetworkingNat::s_WmiNatNamespace(L"MSFT_NetNat");

LxssNetworkingNat::LxssNetworkingNat(const IP_ADDRESS_PREFIX& InputPrefix) : m_internalIpAddress(InputPrefix)
{
    // Convert the address into a string of the form "172.17.0.0/16"
    const std::wstring inputAddress = LxssIpTables::AddressStringFromAddress(InputPrefix, true);

    const std::wstring friendlyName = s_FriendlyNamePrefix + inputAddress;
    m_session = LxssManagementInterface::NewSession();
    const auto instance = GetNatWmiInstance(m_session);
    MI_Value miValue;
    miValue.string = const_cast<MI_Char*>(friendlyName.c_str());
    MI_Result result = MI_Instance_SetElement(instance.get(), s_WmiNatName.c_str(), &miValue, MI_STRING, MI_FLAG_BORROW);

    THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

    miValue.string = const_cast<MI_Char*>(inputAddress.c_str());
    result = MI_Instance_SetElement(instance.get(), s_WmiNatInternalIpAddress.c_str(), &miValue, MI_STRING, MI_FLAG_BORROW);

    // TODO_LX: Might need a timeout value here, set via
    //          MI_OperationOptions_SetTimeout()
    unique_mi_operation operation;
    MI_Session_CreateInstance(
        m_session.get(), 0, nullptr, LxssManagementInterface::LocalRoot().c_str(), instance.get(), nullptr, operation.addressof());

    MI_Boolean moreResults;
    const MI_Instance* resultInstance;
    MI_Result innerResult = MI_RESULT_OK;
    result = MI_Operation_GetInstance(operation.addressof(), &resultInstance, &moreResults, &innerResult, nullptr, nullptr);

    WI_ASSERT(moreResults != MI_TRUE);

    THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

    THROW_HR_IF_MSG(E_FAIL, (innerResult != MI_RESULT_OK), "Operation failed with error %d", innerResult);

    m_natInstance = LxssManagementInterface::CloneInstance(resultInstance);
    return;
}

LxssNetworkingNat::LxssNetworkingNat(const MI_Instance* ExistingInstance)
{
    m_session = LxssManagementInterface::NewSession();
    m_natInstance = LxssManagementInterface::CloneInstance(ExistingInstance);
    return;
}

LxssNetworkingNat::~LxssNetworkingNat()
{
    unique_mi_operation operation;
    MI_Session_DeleteInstance(
        m_session.get(), 0, nullptr, LxssManagementInterface::LocalRoot().c_str(), m_natInstance.get(), nullptr, operation.addressof());

    MI_Boolean moreResults = MI_TRUE;
    while (moreResults != MI_FALSE)
    {
        const MI_Instance* resultInstance;
        MI_Result innerResult;
        const MI_Result result =
            MI_Operation_GetInstance(operation.addressof(), &resultInstance, &moreResults, &innerResult, nullptr, nullptr);

        if (result != MI_RESULT_OK)
        {
            LOG_HR_MSG(E_FAIL, "Failed with error %d", result);

            break;
        }

        LOG_HR_IF_MSG(E_FAIL, (innerResult != MI_RESULT_OK), "Failed with error %d", innerResult);
    }
}

void LxssNetworkingNat::CleanupRemnants()
{
    // Search through all NATs in the system looking for those that match the
    // WSL naming convention: WSL_<IP address>
    const auto session = LxssManagementInterface::NewSession();
    unique_mi_operation operation;
    MI_Session_EnumerateInstances(
        session.get(),
        0,
        nullptr,
        LxssManagementInterface::LocalRoot().c_str(),
        s_WmiNatNamespace.c_str(),
        false,
        nullptr,
        operation.addressof());

    MI_Boolean moreResults = MI_TRUE;
    while (moreResults != MI_FALSE)
    {
        const MI_Instance* resultInstance{};
        MI_Result innerResult{};
        MI_Result result = MI_Operation_GetInstance(operation.addressof(), &resultInstance, &moreResults, &innerResult, nullptr, nullptr);
        if (result != MI_RESULT_OK)
        {
            LOG_HR_MSG(E_FAIL, "Failed with error %d", result);
            break;
        }

        if (innerResult != MI_RESULT_OK)
        {
            LOG_HR_MSG(E_FAIL, "Failed with error %d", innerResult);
            continue;
        }

        if (resultInstance == nullptr)
        {
            // From: https://learn.microsoft.com/en-us/windows/win32/api/mi/nf-mi-mi_operation_getinstance
            // This value may be Null even if the operation succeeds.
            continue;
        }

        MI_Value miValue{};
        MI_Type miType{};
        result = MI_Instance_GetElement(resultInstance, s_WmiNatName.c_str(), &miValue, &miType, nullptr, nullptr);

        if (result != MI_RESULT_OK)
        {
            LOG_HR_MSG(E_FAIL, "Failed with error %d", result);
            continue;
        }

        if (miType != MI_STRING)
        {
            LOG_HR_MSG(E_UNEXPECTED, "Type is %d", miType);
            continue;
        }

        if (wsl::shared::string::StartsWith(miValue.string, s_FriendlyNamePrefix, true))
        {
            // Create a temporary NAT instance, to be immediately deleted when
            // it goes out of scope.
            LxssNetworkingNat natRemnant(resultInstance);
        }
    }
}

unique_mi_instance LxssNetworkingNat::GetNatWmiInstance(const unique_mi_session& Session)
{
    unique_mi_operation operation;
    MI_Session_GetClass(
        Session.get(), 0, nullptr, LxssManagementInterface::LocalRoot().c_str(), s_WmiNatNamespace.c_str(), nullptr, operation.addressof());

    const MI_Class* miClass;
    const MI_Result result = MI_Operation_GetClass(operation.addressof(), &miClass, nullptr, nullptr, nullptr, nullptr);

    THROW_HR_IF_MSG(E_FAIL, (result != MI_RESULT_OK), "Failed with error %d", result);

    return LxssManagementInterface::NewInstance(s_WmiNatNamespace.c_str(), miClass);
}
