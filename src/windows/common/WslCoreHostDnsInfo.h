// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once
#include <mutex>
#include <string>
#include <vector>

#include <iptypes.h>
#include <wil/registry.h>

#include "WslCoreNetworkingSupport.h"
#include "RegistryWatcher.h"

namespace wsl::core::networking {
struct DnsInfo
{
    std::vector<std::string> Servers;
    std::vector<std::string> Domains;
};

enum class DnsSettingsFlags
{
    None = 0x0,
    IncludeVpn = 0x1,
    IncludeIpv6Servers = 0x2,
    IncludeAllSuffixes = 0x4
};
DEFINE_ENUM_FLAG_OPERATORS(DnsSettingsFlags);

inline bool operator==(const DnsInfo& lhs, const DnsInfo& rhs) noexcept
{
    return lhs.Servers == rhs.Servers && lhs.Domains == rhs.Domains;
}
inline bool operator!=(const DnsInfo& lhs, const DnsInfo& rhs) noexcept
{
    return !(lhs == rhs);
}

std::string GenerateResolvConf(_In_ const DnsInfo& Info);

/// <summary>
/// Builds an hns::DNS notification from DnsInfo settings.
/// </summary>
/// <param name="settings">The DNS settings to convert</param>
/// <param name="useLinuxDomainEntry">If true, uses 'domain' entry for single suffix; otherwise uses 'search' for all
/// suffixes</param> <returns>The hns::DNS notification ready to send via GNS channel</returns>
wsl::shared::hns::DNS BuildDnsNotification(const DnsInfo& settings, bool useLinuxDomainEntry = false);

std::vector<std::string> GetAllDnsSuffixes(const std::vector<IpAdapterAddress>& AdapterAddresses);

DWORD GetBestInterface();

class HostDnsInfo
{
public:
    DnsInfo GetDnsSettings(_In_ DnsSettingsFlags Flags);

    void UpdateNetworkInformation();

    static DnsInfo GetDnsTunnelingSettings(const std::wstring& dnsTunnelingNameserver);

    const std::vector<IpAdapterAddress>& CurrentAddresses() const
    {
        return m_addresses;
    }

private:
    /// <summary>
    /// Internal function to retrieve the latest copy of interface information.
    /// </summary>
    std::vector<IpAdapterAddress> GetAdapterAddresses();

    /// <summary>
    /// Internal function to retrieve interface DNS servers.
    /// </summary>
    std::vector<std::string> GetInterfaceDnsServers(const std::vector<IpAdapterAddress>& AdapterAddresses, _In_ DnsSettingsFlags Flags);

    /// <summary>
    /// Internal function to retrieve all Windows DNS suffixes.
    /// </summary>
    static std::vector<std::string> GetInterfaceDnsSuffixes(const std::vector<IpAdapterAddress>& AdapterAddresses);

    /// <summary>
    /// Internal function to convert DNS server addresses into strings.
    /// </summary>
    static std::vector<std::string> GetDnsServerStrings(_In_ const PIP_ADAPTER_DNS_SERVER_ADDRESS& DnsServer, _In_ USHORT IpFamilyFilter, _In_ USHORT MaxValues);

    /// <summary>
    /// Stores latest copy of interface information.
    /// </summary>
    std::mutex m_lock;
    _Guarded_by_(m_lock) std::vector<IpAdapterAddress> m_addresses;
};

/// <summary>
/// Helper class that fetches current DNS settings from the host.
/// Callers are responsible for tracking changes if needed.
/// </summary>
class DnsUpdateHelper
{
public:
    /// <summary>
    /// Fetches current DNS settings from the host.
    /// </summary>
    /// <param name="flags">Flags controlling which DNS settings to include</param>
    /// <returns>Current DNS settings</returns>
    DnsInfo GetCurrentDnsSettings(DnsSettingsFlags flags);

private:
    HostDnsInfo m_hostDnsInfo;
};

using RegistryChangeCallback = std::function<void()>;

/// <summary>
/// Class used to get notifications when Windows DNS suffixes are updated in registry.
/// </summary>
class DnsSuffixRegistryWatcher
{
public:
    DnsSuffixRegistryWatcher(RegistryChangeCallback&& reportRegistryChange);
    ~DnsSuffixRegistryWatcher() noexcept = default;

private:
    RegistryChangeCallback m_reportRegistryChange;

    std::vector<wistd::unique_ptr<wsl::windows::common::slim_registry_watcher>> m_registryWatchers;
};

} // namespace wsl::core::networking