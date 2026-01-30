// Copyright (C) Microsoft Corporation. All rights reserved.

#include <LxssDynamicFunction.h>
#include "precomp.h"

#include <algorithm>

#include <iphlpapi.h>
#include <ipifcons.h>

#include "WmiService.h"
#include "WslCoreHostDnsInfo.h"

static constexpr auto c_asciiNewLine = "\xa";

// Used for querying suffixes via WMI
static constexpr auto c_suffixSearchList = L"SuffixSearchList";
static constexpr auto c_connectionSpecificSuffix = L"ConnectionSpecificSuffix";
static constexpr auto c_connectionSpecificSuffixSearchList = L"ConnectionSpecificSuffixSearchList";
static constexpr auto c_interfaceIndex = L"InterfaceIndex";

static constexpr auto c_ipHelperModuleName = L"Iphlpapi.dll";

static std::optional<LxssDynamicFunction<decltype(GetInterfaceDnsSettings)>> g_getInterfaceDnsSettings;
static std::optional<LxssDynamicFunction<decltype(FreeInterfaceDnsSettings)>> g_freeInterfaceDnsSettings;

struct DnsRegistryPath
{
    const wchar_t* registryPath;
    const bool isRecursive;
};

// Registry paths that need to be monitored for DNS suffix changes
constexpr DnsRegistryPath c_dnsSuffixesRegistryPaths[] = {
    {L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\InterfaceSpecificParameters", true},
    {L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces", true},
    {L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters\\Interfaces", true},
    {L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", false},
    {L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters", false},
    {L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient", false},
    {L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters", false}};

// Load GetInterfaceDnsSettings and FreeInterfaceDnsSettings, if available
static bool LoadIpHelperMethods() noexcept
try
{
    static wil::shared_hmodule ipHelperModule;
    static std::once_flag loadFlag;

    std::call_once(loadFlag, [&]() {
        ipHelperModule.reset(LoadLibraryEx(c_ipHelperModuleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
        RETURN_LAST_ERROR_IF_EXPECTED(!ipHelperModule);

        LxssDynamicFunction<decltype(GetInterfaceDnsSettings)> local_getInterfaceDnsSettings{DynamicFunctionErrorLogs::None};
        RETURN_IF_FAILED_EXPECTED(local_getInterfaceDnsSettings.load(ipHelperModule, "GetInterfaceDnsSettings"));
        LxssDynamicFunction<decltype(FreeInterfaceDnsSettings)> local_freeInterfaceDnsSettings{DynamicFunctionErrorLogs::None};
        RETURN_IF_FAILED_EXPECTED(local_freeInterfaceDnsSettings.load(ipHelperModule, "FreeInterfaceDnsSettings"));

        g_getInterfaceDnsSettings.emplace(std::move(local_getInterfaceDnsSettings));
        g_freeInterfaceDnsSettings.emplace(std::move(local_freeInterfaceDnsSettings));
        return S_OK;
    });

    if (g_getInterfaceDnsSettings.has_value() && g_freeInterfaceDnsSettings.has_value())
    {
        return true;
    }

    WSL_LOG("LoadIpHelperMethods (false): GetInterfaceDnsSettings is not present");
    return false;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return false;
}

DWORD wsl::core::networking::GetBestInterface()
{
    DWORD bestInterface = 0;
    SOCKADDR_STORAGE address{};
    IN4ADDR_SETANY((SOCKADDR_IN*)&address);
    if (GetBestInterfaceEx((SOCKADDR*)&address, &bestInterface) != NO_ERROR)
    {
        IN6ADDR_SETANY((SOCKADDR_IN6*)&address);
        if (GetBestInterfaceEx((SOCKADDR*)&address, &bestInterface) != NO_ERROR)
        {
            bestInterface = 0;
        }
    }

    WSL_LOG("wsl::core::networking::GetBestInterface [GetBestInterfaceEx]", TraceLoggingValue(bestInterface, "bestInterface"));

    return bestInterface;
}

wsl::core::networking::DnsInfo wsl::core::networking::HostDnsInfo::GetDnsTunnelingSettings(const std::wstring& dnsTunnelingNameserver)
{
    DnsInfo dnsInfo;
    dnsInfo.Servers.push_back(wsl::shared::string::WideToMultiByte(dnsTunnelingNameserver));

    // All Windows DNS suffixes are configured in Linux when DNS tunneling is enabled
    dnsInfo.Domains = GetAllDnsSuffixes(AdapterAddresses::GetCurrent());

    return dnsInfo;
}

std::vector<wsl::core::networking::IpAdapterAddress> wsl::core::networking::HostDnsInfo::GetAdapterAddresses()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_addresses;
}

std::vector<std::string> wsl::core::networking::HostDnsInfo::GetDnsServerStrings(
    _In_ const PIP_ADAPTER_DNS_SERVER_ADDRESS& FirstDnsServer, _In_ USHORT IpFamilyFilter, _In_ USHORT MaxValues)
{
    std::vector<std::string> DnsServerStrings;
    CHAR IpBuffer[46];

    PIP_ADAPTER_DNS_SERVER_ADDRESS DnsServer = FirstDnsServer;
    while ((DnsServer != nullptr) && (DnsServerStrings.size() < MaxValues))
    {
        PVOID IpAddress = nullptr;
        const USHORT IpFamily = DnsServer->Address.lpSockaddr->sa_family;
        if (IpFamily == AF_INET)
        {
            IpAddress = &((sockaddr_in*)DnsServer->Address.lpSockaddr)->sin_addr;
        }
        else if (IpFamily == AF_INET6)
        {
            IpAddress = &((sockaddr_in6*)DnsServer->Address.lpSockaddr)->sin6_addr;
        }

        DnsServer = DnsServer->Next;
        if (IpFamily != IpFamilyFilter)
        {
            continue;
        }

        THROW_LAST_ERROR_IF_MSG(
            (InetNtopA(IpFamily, IpAddress, IpBuffer, ARRAYSIZE(IpBuffer)) == NULL), "Failed to convert IP address");

        DnsServerStrings.push_back(IpBuffer);
    }

    return DnsServerStrings;
}

std::vector<std::string> wsl::core::networking::HostDnsInfo::GetInterfaceDnsServers(const std::vector<IpAdapterAddress>& AdapterAddresses, _In_ DnsSettingsFlags Flags)
{
    std::vector<std::string> DnsServers;

    constexpr size_t MaxResolvConfDnsServers = 3;
    for (const IpAdapterAddress& NextAddress : AdapterAddresses)
    {
        WI_ASSERT(DnsServers.size() < MaxResolvConfDnsServers);

        USHORT MaxDnsServers = static_cast<USHORT>(MaxResolvConfDnsServers - DnsServers.size());

        // Include only primary DNS VPN server.
        if ((MaxDnsServers > 1) && (wsl::core::networking::IsInterfaceTypeVpn(NextAddress->IfType)))
        {
            MaxDnsServers = 1;
        }

        // Add DNS nameservers from the interface, with the IPv4 addresses first.
        std::vector<std::string> ipv4Servers = GetDnsServerStrings(NextAddress->FirstDnsServerAddress, AF_INET, MaxDnsServers);
        DnsServers.insert(DnsServers.end(), ipv4Servers.begin(), ipv4Servers.end());

        WI_ASSERT(DnsServers.size() <= MaxResolvConfDnsServers);

        if (WI_IsFlagSet(Flags, DnsSettingsFlags::IncludeIpv6Servers))
        {
            std::vector<std::string> ipv6Servers = GetDnsServerStrings(
                NextAddress->FirstDnsServerAddress, AF_INET6, static_cast<USHORT>(MaxResolvConfDnsServers - DnsServers.size()));

            DnsServers.insert(DnsServers.end(), ipv6Servers.begin(), ipv6Servers.end());
        }

        WI_ASSERT(DnsServers.size() <= MaxResolvConfDnsServers);

        // Only the first three nameserver entries are used.
        if (DnsServers.size() >= MaxResolvConfDnsServers)
        {
            break;
        }
    }

    return DnsServers;
}

std::vector<std::string> wsl::core::networking::HostDnsInfo::GetInterfaceDnsSuffixes(const std::vector<IpAdapterAddress>& AdapterAddresses)
{
    std::vector<std::string> DnsSuffixes;
    std::set<std::wstring> UniqueDnsSuffixes;

    PIP_ADAPTER_DNS_SUFFIX DnsSuffix;

    auto AppendSuffix = [&](const std::wstring& NewSuffix) {
        if (NewSuffix.empty())
        {
            return;
        }

        if (std::ranges::find_if(UniqueDnsSuffixes, [&](const std::wstring& Suffix) {
                return wsl::shared::string::IsEqual(Suffix, NewSuffix, true);
            }) == UniqueDnsSuffixes.end())
        {
            DnsSuffixes.emplace_back(wsl::shared::string::WideToMultiByte(NewSuffix));
            UniqueDnsSuffixes.insert(NewSuffix);
        }
    };

    for (const IpAdapterAddress& NextAddress : AdapterAddresses)
    {
        // Add any domain suffix information from the interface.
        if ((NextAddress->DnsSuffix != nullptr) && (wcslen(NextAddress->DnsSuffix) > 0))
        {
            AppendSuffix(NextAddress->DnsSuffix);
        }

        DnsSuffix = NextAddress->FirstDnsSuffix;
        while (DnsSuffix != nullptr)
        {
            AppendSuffix(DnsSuffix->String);

            DnsSuffix = DnsSuffix->Next;
        }
    }

    return DnsSuffixes;
}

wsl::core::networking::DnsInfo wsl::core::networking::HostDnsInfo::GetDnsSettings(_In_ DnsSettingsFlags Flags)
{
    std::vector<IpAdapterAddress> Addresses = GetAdapterAddresses();

    auto RemoveFilter = [&](const IpAdapterAddress& Address) {
        // Ignore interfaces that are not currently "up".
        // Ignore loopback and tunneling interfaces.
        // Ignore interfaces that have no IP address or no DNS addresses.
        // Ignore hidden interfaces
        if ((Address->OperStatus != IfOperStatusUp) || (Address->IfType == IF_TYPE_SOFTWARE_LOOPBACK) || (Address->IfType == IF_TYPE_TUNNEL) ||
            (!WI_IsFlagSet(Flags, DnsSettingsFlags::IncludeVpn) && wsl::core::networking::IsInterfaceTypeVpn(Address->IfType)) ||
            (Address->FirstUnicastAddress == nullptr) || (Address->FirstDnsServerAddress == nullptr) || IsInterfaceHidden(Address->IfIndex))
        {
            return true;
        }
        return false;
    };

    std::erase_if(Addresses, RemoveFilter);

    // Find the recommended internet interface if one exists.
    // First try VPN interface, then regular IPv4 and then IPv6.
    const auto BestInterface = GetBestInterface();

    // Sort the remaining network interfaces, with the most preferable at index 0.
    std::sort(Addresses.begin(), Addresses.end(), [&](const IpAdapterAddress& First, const IpAdapterAddress& Second) {
        // VPN interface takes precedence.
        const bool FirstIsVpn = wsl::core::networking::IsInterfaceTypeVpn(First->IfType);
        const bool SecondIsVpn = wsl::core::networking::IsInterfaceTypeVpn(Second->IfType);
        if (FirstIsVpn ^ SecondIsVpn)
        {
            // Give precedence to the first VPN interface.
            return FirstIsVpn;
        }

        // The identified 'best' internet connection interface should go right
        // after VPN. Or if both networking interfaces are VPN interfaces,
        // give preference to the one considered 'best'.
        if (First->IfIndex == BestInterface)
        {
            return true;
        }
        if (Second->IfIndex == BestInterface)
        {
            return false;
        }

        // Check the first interface for IPv4 DNS servers.
        bool FirstHasIpv4 = false;
        auto DnsServer = First->FirstDnsServerAddress;
        while (DnsServer != nullptr)
        {
            const USHORT IpFamily = DnsServer->Address.lpSockaddr->sa_family;
            DnsServer = DnsServer->Next;
            if (IpFamily == AF_INET)
            {
                FirstHasIpv4 = true;
                break;
            }
        }

        // Check the second interface for IPv4 DNS servers.
        bool SecondHasIpv4 = false;
        DnsServer = Second->FirstDnsServerAddress;
        while (DnsServer != nullptr)
        {
            const USHORT IpFamily = DnsServer->Address.lpSockaddr->sa_family;
            DnsServer = DnsServer->Next;
            if (IpFamily == AF_INET)
            {
                SecondHasIpv4 = true;
                break;
            }
        }

        // Give precedence to interfaces that have IPv4 DNS servers; otherwise, give precedence to the lower interface index.
        return (FirstHasIpv4 ^ SecondHasIpv4) ? FirstHasIpv4 : (First->IfIndex < Second->IfIndex);
    });

    DnsInfo DnsSettings{};

    DnsSettings.Servers = GetInterfaceDnsServers(Addresses, Flags);

    if (WI_IsFlagSet(Flags, DnsSettingsFlags::IncludeAllSuffixes))
    {
        DnsSettings.Domains = GetAllDnsSuffixes(Addresses);
    }
    else
    {
        DnsSettings.Domains = GetInterfaceDnsSuffixes(Addresses);
    }

    return DnsSettings;
}

void wsl::core::networking::HostDnsInfo::UpdateNetworkInformation()
{
    std::lock_guard<std::mutex> lock(m_lock);
    m_addresses = AdapterAddresses::GetCurrent();
}

std::string wsl::core::networking::GenerateResolvConf(_In_ const DnsInfo& Info)
{
    std::string contents{};
    if (!Info.Servers.empty())
    {
        // Add IP addresses of the DNS name servers.
        for (const std::string& ip : Info.Servers)
        {
            contents += "nameserver ";
            contents += ip;
            contents += c_asciiNewLine;
        }

        // Add domain information if it is available.
        if (!Info.Domains.empty())
        {
            contents += "search ";
            std::for_each(Info.Domains.begin(), (Info.Domains.end() - 1), [&contents](const std::string& NextDomain) {
                contents += NextDomain;
                contents += " ";
            });

            contents += Info.Domains.back();
            contents += c_asciiNewLine;
        }
    }

    WSL_LOG("wsl::core::networking::GenerateResolvConf", TraceLoggingValue(contents.c_str(), "resolvConf"));

    return contents;
}

std::vector<std::string> wsl::core::networking::GetAllDnsSuffixes(const std::vector<IpAdapterAddress>& AdapterAddresses)
{
    const auto com = wil::CoInitializeEx();
    wsl::core::WmiService service(L"ROOT\\StandardCimv2");

    // DNS suffixes will be configured in Linux in the following order, *similar* (not 100% the same) to the order in which Windows tries suffixes.
    //
    // 1) Global suffixes (can be configured manually or via group policy) - queried using WMI call equivalent with Get-DnsClientGlobalSetting
    // 2) Supplemental search list, queried using GetInterfaceDnsSettings()
    // 3) Per-interface suffixes, queried using WMI call equivalent with Get-DnsClient
    std::vector<std::string> dnsSuffixes;
    std::set<std::wstring> uniqueDnsSuffixes;

    auto AppendSuffix = [&](const std::wstring& newSuffix) {
        if (newSuffix.empty())
        {
            return;
        }

        if (std::ranges::find_if(uniqueDnsSuffixes, [&](const std::wstring& suffix) {
                return wsl::shared::string::IsEqual(suffix, newSuffix, true);
            }) == uniqueDnsSuffixes.end())
        {
            dnsSuffixes.emplace_back(wsl::shared::string::WideToMultiByte(newSuffix));
            uniqueDnsSuffixes.insert(newSuffix);
        }
    };

    // 1) Query global suffixes
    wsl::core::WmiEnumerate enumDnsClientGlobalSetting(service);

    for (const auto& instance : enumDnsClientGlobalSetting.query(L"SELECT * FROM MSFT_DnsClientGlobalSetting"))
    {
        std::vector<std::wstring> suffixSearchList;

        instance.get(c_suffixSearchList, &suffixSearchList);

        for (const auto& suffix : suffixSearchList)
        {
            AppendSuffix(suffix);
        }
    }

    // 2) Query supplemental search list. Skip this step if the OS does not support the required APIs
    if (LoadIpHelperMethods())
    {
        for (const auto& address : AdapterAddresses)
        {
            if (IsInterfaceHidden(address->IfIndex))
            {
                continue;
            }

            GUID interfaceGuid{};
            if (FAILED_WIN32_LOG(ConvertInterfaceLuidToGuid(&address->Luid, &interfaceGuid)))
            {
                continue;
            }

            DNS_INTERFACE_SETTINGS_EX settings{};
            settings.SettingsV1.Version = DNS_INTERFACE_SETTINGS_VERSION2;
            settings.SettingsV1.Flags = DNS_SETTING_SUPPLEMENTAL_SEARCH_LIST;

            if (FAILED_WIN32_LOG(g_getInterfaceDnsSettings.value()(interfaceGuid, reinterpret_cast<DNS_INTERFACE_SETTINGS*>(&settings))))
            {
                continue;
            }

            const auto freeSettings =
                wil::scope_exit([&] { g_freeInterfaceDnsSettings.value()(reinterpret_cast<DNS_INTERFACE_SETTINGS*>(&settings)); });

            if (settings.SupplementalSearchList != nullptr)
            {
                // The suffix list can be delimited by comma, space, tab
                std::wstring separators = L", \t";

                for (const auto& suffix :
                     wsl::shared::string::SplitByMultipleSeparators(std::wstring{settings.SupplementalSearchList}, separators))
                {
                    AppendSuffix(suffix);
                }
            }
        }
    }

    // 3) Query per-interface suffixes
    wsl::core::WmiEnumerate enumDnsClient(service);

    for (const auto& instance : enumDnsClient.query(L"SELECT * FROM MSFT_DnsClient"))
    {
        std::wstring connectionSuffix;
        std::vector<std::wstring> connectionSuffixSearchList;
        IF_INDEX interfaceIndex{};

        instance.get(c_interfaceIndex, &interfaceIndex);
        if (IsInterfaceHidden(interfaceIndex))
        {
            continue;
        }

        instance.get(c_connectionSpecificSuffix, &connectionSuffix);
        instance.get(c_connectionSpecificSuffixSearchList, &connectionSuffixSearchList);

        AppendSuffix(connectionSuffix);

        for (const auto& suffix : connectionSuffixSearchList)
        {
            AppendSuffix(suffix);
        }
    }

    return dnsSuffixes;
}

wsl::core::networking::DnsSuffixRegistryWatcher::DnsSuffixRegistryWatcher(RegistryChangeCallback&& reportRegistryChange) :
    m_reportRegistryChange(std::move(reportRegistryChange))
{
    std::vector<wistd::unique_ptr<wsl::windows::common::slim_registry_watcher>> localRegistryWatchers;

    for (const auto& path : c_dnsSuffixesRegistryPaths)
    {
        auto watcher = wil::make_unique_nothrow<wsl::windows::common::slim_registry_watcher>();
        THROW_HR_IF(E_OUTOFMEMORY, !watcher);

        THROW_IF_FAILED(watcher->create(HKEY_LOCAL_MACHINE, path.registryPath, path.isRecursive, [this](wil::RegistryChangeKind changeKind) {
            m_reportRegistryChange();
        }));
        localRegistryWatchers.emplace_back(std::move(watcher));
    }

    m_registryWatchers.swap(localRegistryWatchers);
}

wsl::shared::hns::DNS wsl::core::networking::BuildDnsNotification(const DnsInfo& settings, bool useLinuxDomainEntry)
{
    wsl::shared::hns::DNS dnsNotification{};
    dnsNotification.Options = LX_INIT_RESOLVCONF_FULL_HEADER;
    dnsNotification.ServerList = wsl::shared::string::MultiByteToWide(wsl::shared::string::Join(settings.Servers, ','));

    if (useLinuxDomainEntry && !settings.Domains.empty())
    {
        // Use 'domain' entry for single DNS suffix (typically used when mirroring host DNS without tunneling)
        dnsNotification.Domain = wsl::shared::string::MultiByteToWide(settings.Domains.front());
    }
    else
    {
        // Use 'search' entry for DNS suffix list
        dnsNotification.Search = wsl::shared::string::MultiByteToWide(wsl::shared::string::Join(settings.Domains, ','));
    }

    return dnsNotification;
}

wsl::core::networking::DnsInfo wsl::core::networking::DnsUpdateHelper::GetCurrentDnsSettings(DnsSettingsFlags flags)
{
    m_hostDnsInfo.UpdateNetworkInformation();
    return m_hostDnsInfo.GetDnsSettings(flags);
}
