/*++

    Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssIptables.h

Abstract:

    This file contains iptables-related function declarations.

--*/

#pragma once

#include <memory>
#include <mutex>
#include <mi.h>
#include <netfw.h>
#include "LxssUserCallback.h"

/// <summary>
/// Type to hold a MI_Session instance. Functions creating an instance of this
/// type will construct it in such a way as to also hold an implicit reference
/// to the global application instance.
/// </summary>
using unique_mi_session = std::unique_ptr<MI_Session, std::function<void(MI_Session*)>>;

/// <summary>
/// Type to hold a MI_Instance instance. Functions creating an instance of this
/// type will construct it in such a way as to also hold an implicit reference
/// to the global application instance.
/// </summary>
using unique_mi_instance = std::unique_ptr<MI_Instance, std::function<void(MI_Instance*)>>;

/// <summary>
/// Helper class for using Windows Management Interface.
/// </summary>
class LxssManagementInterface
{
public:
    /// <summary>
    /// Clone an instance.
    /// </summary>
    static unique_mi_instance CloneInstance(_In_ const MI_Instance* InstanceToClone);

    /// <summary>
    /// Closes a MI_Operation instance.
    /// </summary>
    static void CloseOperation(MI_Operation* Operation);

    /// <summary>
    /// Returns the global application instance.
    /// </summary>
    static std::shared_ptr<MI_Application> GetGlobalApplication();

    /// <summary>
    /// Create a new management interface MI_Instance instance.
    /// </summary>
    static unique_mi_instance NewInstance(_In_ const std::wstring& ClassName, _In_opt_ const MI_Class* Class);

    /// <summary>
    /// Create a new management interface session instance.
    /// </summary>
    static unique_mi_session NewSession();

    /// <summary>
    /// Local machine root instance.
    /// </summary>
    static const std::wstring& LocalRoot()
    {
        return s_localRoot;
    }

private:
    /// <summary>
    /// Called to destroy the global application instance. Called as a result
    /// of the last shared reference being deleted.
    /// </summary>
    static void CloseGlobalApplication(_Inout_ MI_Application* Application);

    /// <summary>
    /// Close a MI_Instance. This is implicitly called via the destruction of
    /// an unique_mi_instance instance.
    /// </summary>
    static void CloseInstance(_In_ std::shared_ptr<MI_Application>, _Inout_ MI_Instance* Instance);

    /// <summary>
    /// Close a session. This is implicitly called via the destruction of an
    /// unique_mi_session instance.
    /// </summary>
    static void CloseSession(_In_ std::shared_ptr<MI_Application>, _Inout_ MI_Session* Session);

    /// <summary>
    /// Lock for static members.
    /// </summary>
    static std::mutex s_lock;

    /// <summary>
    /// Global MI_Application instance.
    /// </summary>
    _Guarded_by_(LxssManagementInterface::s_lock) static std::weak_ptr<MI_Application> s_application;

    /// <summary>
    /// Local machine root instance.
    /// </summary>
    static const std::wstring s_localRoot;
};

/// <summary>
/// MI_Operation unique instance.
/// </summary>
using unique_mi_operation =
    wil::unique_struct<MI_Operation, decltype(LxssManagementInterface::CloseOperation), LxssManagementInterface::CloseOperation>;

class LxssNetworkingFirewallPort;
class LxssNetworkingNat;

/// <summary>
/// Emulate iptables functionality.
/// </summary>
class LxssIpTables
{
public:
    /// <summary>
    /// Constructor.
    /// </summary>
    LxssIpTables();

    /// <summary>
    /// Enable iptables emulation.
    /// </summary>
    void EnableIpTablesSupport(_In_ const wil::unique_handle& InstanceHandle);

    /// <summary>
    /// Cleanup any persistent data leftover from a non-clean shutdown.
    /// </summary>
    static void CleanupRemnants();

    /// <summary>
    /// Helper routine to convert an IP address into the string equivalent.
    /// </summary>
    static std::wstring AddressStringFromAddress(const IP_ADDRESS_PREFIX& Address, bool AddPrefixLength);

private:
    /// <summary>
    /// No copy constructor.
    /// </summary>
    LxssIpTables(const LxssIpTables&) = delete;

    /// <summary>
    /// Verify the input prefix address is supported.
    /// </summary>
    static bool IsAllowedInputPrefix(_In_ CONST IP_ADDRESS_PREFIX& InputPrefix);

    /// <summary>
    /// Kernel-mode callback function for iptables operations.
    /// </summary>
    NTSTATUS
    KernelCallback(_In_ PVOID CallbackBuffer, _In_ ULONG_PTR CallbackBufferSize);

    /// <summary>
    /// Kernel-mode callback function to configure a port rule via the Windows
    /// firewall.
    /// </summary>
    NTSTATUS
    KernelCallbackFirewallPort(_In_ PLXBUS_USER_CALLBACK_IPTABLES_DATA CallbackData);

    /// <summary>
    /// Kernel-mode callback function to add a new masquerade entry.
    /// </summary>
    NTSTATUS
    KernelCallbackMasquerade(_In_ PLXBUS_USER_CALLBACK_IPTABLES_DATA CallbackData);

    /// <summary>
    /// Kernel-mode callback entrypoint function for iptables operations.
    /// </summary>
    static NTSTATUS KernelCallbackProxy(_Inout_ LxssIpTables* Self, _In_ PVOID CallbackBuffer, _In_ ULONG_PTR CallbackBufferSize);

    /// <summary>
    /// List of port rules.
    /// </summary>
    std::list<std::unique_ptr<LxssNetworkingFirewallPort>> m_firewallPorts;

    /// <summary>
    /// Lock to protect class members.
    /// </summary>
    std::mutex m_lock;

    /// <summary>
    /// List of NATs.
    /// </summary>
    std::list<std::unique_ptr<LxssNetworkingNat>> m_networkTranslators;

    /// <summary>
    /// Callback for the kernel-mode driver to make iptables requests.
    /// </summary>
    // N.B. This is the last member of the class because it needs to be
    //      destructed early as the asynchronous callback may rely on other
    //      members of the class being valid.
    std::unique_ptr<LxssUserCallback> m_kernelCallback;
};

/// <summary>
/// Class providing access to Windows firewall
/// </summary>
class LxssNetworkingFirewall
{
public:
    /// <summary>
    /// Default constructor.
    /// </summary>
    LxssNetworkingFirewall();

    /// <summary>
    /// Create a rule to allow the specified address and port combination.
    /// </summary>
    std::wstring AddPortRule(const IP_ADDRESS_PREFIX& Address) const;

    /// <summary>
    /// Cleanup any persistent data leftover from a non-clean shutdown.
    /// </summary>
    static void CleanupRemnants();

    /// <summary>
    /// Exclude a network adapter from the firewall's public profile.
    /// </summary>
    void ExcludeAdapter(const std::wstring& AdapterName);

    /// <summary>
    /// Remove a network adapter from the exclusion list.
    /// </summary>
    void RemoveExcludedAdapter(const std::wstring& AdapterName);

    /// <summary>
    /// Remove a port rule created by AddPortRule.
    /// </summary>
    void RemovePortRule(const std::wstring& RuleName) const;

private:
    /// <summary>
    /// No copy constructor.
    /// </summary>
    LxssNetworkingFirewall(const LxssNetworkingFirewall&) = delete;

    /// <summary>
    /// Copies part of a source array to a destination array.
    /// </summary>
    static void CopyPartialArray(SAFEARRAY* Destination, SAFEARRAY* Source, ULONG DestinationIndexStart, ULONG SourceIndexStart, ULONG ElementsToCopy);

    /// <summary>
    /// Creates the unique friendly name of the firewall port rule.
    /// </summary>
    static std::wstring GeneratePortRuleName(const IP_ADDRESS_PREFIX& Address);

    /// <summary>
    /// Returns the existing array of excluded adapters.
    /// </summary>
    wil::unique_variant GetExcludedAdapters(_Out_opt_ ULONG* AdapterCount) const;

    /// <summary>
    /// COM firewall instance.
    /// </summary>
    wil::com_ptr<INetFwPolicy2> m_firewall;

    /// <summary>
    /// Lock to protect class members.
    /// </summary>
    std::mutex m_lock;

    /// <summary>
    /// Firewall rule description.
    /// </summary>
    static const wil::unique_bstr s_DefaultRuleDescription;

    /// <summary>
    /// Prefix to uniquely identify WSL firewall rules.
    /// </summary>
    static const std::wstring s_FriendlyNamePrefix;
};

/// <summary>
/// Class representing a Windows firewall port open rule, removed on
/// destruction.
/// </summary>
class LxssNetworkingFirewallPort
{
public:
    /// <summary>
    /// Constructor.
    /// </summary>
    LxssNetworkingFirewallPort(const std::shared_ptr<LxssNetworkingFirewall>& Firewall, const IP_ADDRESS_PREFIX& Address);

    /// <summary>
    /// Constructor to take ownership of an existing rule.
    /// </summary>
    LxssNetworkingFirewallPort(const std::shared_ptr<LxssNetworkingFirewall>& Firewall, const wil::com_ptr<INetFwRule>& Existing);

    /// <summary>
    /// Destructor.
    /// </summary>
    ~LxssNetworkingFirewallPort();

    /// <summary>
    /// Returns the address and port of the firewall rule.
    /// </summary>
    const IP_ADDRESS_PREFIX& Address() const
    {
        return m_address;
    }

    /// <summary>
    /// Returns the underlying firewall instance.
    /// </summary>
    const std::shared_ptr<LxssNetworkingFirewall> Firewall() const
    {
        return m_firewall;
    }

private:
    /// <summary>
    /// No default constructor.
    /// </summary>
    LxssNetworkingFirewallPort() = delete;
    /// <summary>
    /// No copy constructor.
    /// </summary>
    LxssNetworkingFirewallPort(const LxssNetworkingFirewallPort&) = delete;

    /// <summary>
    /// Address information for the port rule.
    /// </summary>
    IP_ADDRESS_PREFIX m_address;

    /// <summary>
    /// Pointer to the firewall interface.
    /// </summary>
    std::shared_ptr<LxssNetworkingFirewall> m_firewall;

    /// <summary>
    /// The unique rule name.
    /// </summary>
    std::wstring m_name;
};

/// <summary>
/// Class representing a Windows NAT instance.
/// </summary>
class LxssNetworkingNat
{
public:
    /// <summary>
    /// Constructor.
    /// </summary>
    LxssNetworkingNat(const IP_ADDRESS_PREFIX& InputPrefix);

    /// <summary>
    /// Constructor to take ownership of an existing NAT.
    /// N.B. Not setting m_internalIpAddress as the only usage of this
    ///      constructor is to wrap an existing instance for cleanup/deletion.
    ///      If this constructor is to be used for other purposes, need to
    ///      fetch the s_WmiNatInternalIpAddress property and convert it.
    /// </summary>
    LxssNetworkingNat(const MI_Instance* ExistingInstance);

    /// <summary>
    /// Destructor.
    /// </summary>
    ~LxssNetworkingNat();

    /// <summary>
    /// The address being NAT'd.
    /// </summary>
    const IP_ADDRESS_PREFIX& Address() const
    {
        return m_internalIpAddress;
    }

    /// <summary>
    /// Cleanup any persistent data leftover from a non-clean shutdown.
    /// </summary>
    static void CleanupRemnants();

private:
    /// <summary>
    /// No default constructor.
    /// </summary>
    LxssNetworkingNat() = delete;
    /// <summary>
    /// No copy constructor.
    /// </summary>
    LxssNetworkingNat(const LxssNetworkingNat&) = delete;

    /// <summary>
    /// The NAT instance.
    /// </summary>
    unique_mi_instance m_natInstance;

    /// <summary>
    /// The session used to create/destroy the NAT.
    /// </summary>
    unique_mi_session m_session;

    /// <summary>
    /// Create a new WMI instance of the NAT type.
    /// </summary>
    static unique_mi_instance GetNatWmiInstance(const unique_mi_session& Session);

    /// <summary>
    /// The IP address prefix to NAT.
    /// </summary>
    IP_ADDRESS_PREFIX m_internalIpAddress;

    /// <summary>
    /// The string prefix for the friendly NAT name.
    /// </summary>
    static const std::wstring s_FriendlyNamePrefix;

    /// <summary>
    /// The string representing the NAT instance ID in WMI.
    /// </summary>
    static const std::wstring s_WmiNatInstanceId;

    /// <summary>
    /// The string representing the NAT internal IP address prefix in WMI.
    /// </summary>
    static const std::wstring s_WmiNatInternalIpAddress;

    /// <summary>
    /// The string representing the NAT name property in WMI.
    /// </summary>
    static const std::wstring s_WmiNatName;

    /// <summary>
    /// The string representing the NAT namespace in WMI.
    /// </summary>
    static const std::wstring s_WmiNatNamespace;
};
