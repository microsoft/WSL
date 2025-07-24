/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    NetworkTests.cpp

Abstract:

    This file contains test cases for the networking logic.

--*/

#include "precomp.h"
#include "computenetwork.h"
#include "Common.h"
#include "wslpolicies.h"
#include "hns_schema.h"

#include <mstcpip.h>
#include <winhttp.h>
#include <winsock2.h>
#include <netlistmgr.h>

using wsl::shared::hns::GuestEndpointResourceType;
using wsl::shared::hns::ModifyGuestEndpointSettingRequest;
using wsl::shared::hns::ModifyRequestType;

bool TryLoadWinhttpProxyMethods() noexcept
{
    constexpr auto winhttpModuleName = L"Winhttp.dll";
    const wil::shared_hmodule winhttpModule{LoadLibraryEx(winhttpModuleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!winhttpModule)
    {
        return false;
    }

    try
    {
        // attempt to find the functions for the Winhttp proxy APIs.
        static LxssDynamicFunction<decltype(WinHttpRegisterProxyChangeNotification)> WinHttpRegisterProxyChangeNotification{
            winhttpModule, "WinHttpRegisterProxyChangeNotification"};
        static LxssDynamicFunction<decltype(WinHttpUnregisterProxyChangeNotification)> WinHttpUnregisterProxyChangeNotification{
            winhttpModule, "WinHttpUnregisterProxyChangeNotification"};
        static LxssDynamicFunction<decltype(WinHttpGetProxySettingsEx)> WinHttpGetProxySettingsEx{
            winhttpModule, "WinHttpGetProxySettingsEx"};
        static LxssDynamicFunction<decltype(WinHttpGetProxySettingsResultEx)> WinHttpGetProxySettingsResultEx{
            winhttpModule, "WinHttpGetProxySettingsResultEx"};
        static LxssDynamicFunction<decltype(WinHttpFreeProxySettingsEx)> WinHttpFreeProxySettingsEx{
            winhttpModule, "WinHttpFreeProxySettingsEx"};
    }
    catch (...)
    {
        return false;
    }
    return true;
}

#define HYPERV_FIREWALL_TEST_ONLY() \
    { \
        WSL2_TEST_ONLY(); \
        WINDOWS_11_TEST_ONLY(); \
        if (!AreExperimentalNetworkingFeaturesSupported() || !IsHyperVFirewallSupported()) \
        { \
            LogSkipped("Hyper-V Firewall not supported on this OS. Skipping test..."); \
            return; \
        } \
    }

#define MIRRORED_NETWORKING_TEST_ONLY() \
    { \
        WSL2_TEST_ONLY(); \
        WINDOWS_11_TEST_ONLY(); \
        if (!AreExperimentalNetworkingFeaturesSupported() || !IsHyperVFirewallSupported()) \
        { \
            LogSkipped("Mirrored networking not supported on this OS. Skipping test.."); \
            return; \
        } \
    }

#define DNS_TUNNELING_TEST_ONLY() \
    { \
        WSL2_TEST_ONLY(); \
        WINDOWS_11_TEST_ONLY(); \
        if (!AreExperimentalNetworkingFeaturesSupported()) \
        { \
            LogSkipped("DNS tunneling not supported on this OS. Skipping test..."); \
            return; \
        } \
        if (!TryLoadDnsResolverMethods()) \
        { \
            LogSkipped("DNS tunneling APIs not present on this OS. Skipping test..."); \
            return; \
        } \
    }

#define WINHTTP_PROXY_TEST_ONLY() \
    { \
        WSL2_TEST_ONLY(); \
        if (!TryLoadWinhttpProxyMethods()) \
        { \
            LogSkipped("Winhttp proxy APIs not present on this OS. Skipping test..."); \
            return; \
        } \
    }

static constexpr auto c_wslVmCreatorId = L"\'{40e0ac32-46a5-438a-A0B2-2B479E8F2E90}\'";
static constexpr auto c_wsaVmCreatorId = L"\'{9E288F02-CE00-4D9E-BE2B-14CE463B0298}\'";
static constexpr auto c_anyVmCreatorId = L"\'{00000000-0000-0000-0000-000000000000}\'";
static constexpr auto c_firewallRuleActionBlock = L"Block";
static constexpr auto c_firewallRuleActionAllow = L"Allow";
static constexpr auto c_firewallTrafficTestCmd = L"ping -c 3 -W 5 1.1.1.1";
static const std::wstring c_firewallTrafficTestPort = L"80";
static const std::wstring c_firewallTestOtherPort = L"443";
static const std::wstring c_dnsTunnelingDefaultIp = L"10.255.255.254";

namespace {

std::wstring GetMacAddress(const std::wstring& adapter = L"eth0")
{
    auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /sys/class/net/" + adapter + L"/address", 0);
    out.pop_back(); // remove LF
    return out;
}

template <class T>
class Stopwatch
{
private:
    LARGE_INTEGER m_startQpc;
    LARGE_INTEGER m_frequencyQpc;
    T m_timeoutInterval;

public:
    Stopwatch(_In_opt_ T TimeoutInterval = T::max()) : m_timeoutInterval(TimeoutInterval)
    {
        QueryPerformanceFrequency(&m_frequencyQpc);
        QueryPerformanceCounter(&m_startQpc);
    }

    T Elapsed()
    {
        LARGE_INTEGER End;
        UINT64 ElapsedQpc;

        QueryPerformanceCounter(&End);
        ElapsedQpc = End.QuadPart - m_startQpc.QuadPart;

        return T((ElapsedQpc * T::period::den) / T::period::num / m_frequencyQpc.QuadPart);
    }

    bool IsExpired()
    {
        return Elapsed() >= m_timeoutInterval;
    }
};

} // namespace

namespace NetworkTests {

class NetworkTests
{
    WSL_TEST_CLASS(NetworkTests)

    static std::wstring SockaddrToString(const SOCKADDR_INET* sockAddr)
    {
        constexpr auto ipv4AddressStringLength = 16;
        constexpr auto ipv6AddressStringLength = 48;

        std::wstring address(std::max(ipv4AddressStringLength, ipv6AddressStringLength), L'\0');

        switch (sockAddr->si_family)
        {
        case AF_INET:
        {
            RtlIpv4AddressToStringW(&sockAddr->Ipv4.sin_addr, address.data());
            break;
        }
        case AF_INET6:
        {
            RtlIpv6AddressToStringW(&sockAddr->Ipv6.sin6_addr, address.data());
            break;
        }
        default:
            break;
        }

        address.resize(std::wcslen(address.data()));
        return address;
    }

    struct IpAddress
    {
        std::wstring Address;
        uint8_t PrefixLength;
        bool Preferred = false;

        bool operator==(const IpAddress& other) const
        {
            return Address == other.Address && PrefixLength == other.PrefixLength;
        }

        std::wstring GetPrefix() const
        {
            DWORD status = ERROR_INVALID_FUNCTION;
            SOCKADDR_INET* address = nullptr;
            unsigned char* addressPointer{};

            NET_ADDRESS_INFO netAddrInfo{};
            status = ParseNetworkString(Address.c_str(), NET_STRING_IP_ADDRESS, &netAddrInfo, nullptr, nullptr);
            if (status != NO_ERROR)
            {
                return std::wstring(L"");
            }

            address = reinterpret_cast<SOCKADDR_INET*>(&netAddrInfo.IpAddress);
            addressPointer = (address->si_family == AF_INET) ? reinterpret_cast<unsigned char*>(&address->Ipv4.sin_addr)
                                                             : address->Ipv6.sin6_addr.u.Byte;

            constexpr int c_numBitsPerByte = 8;
            for (int i = 0, currPrefixLength = PrefixLength; i < INET_ADDR_LENGTH(address->si_family); i++, currPrefixLength -= c_numBitsPerByte)
            {
                if (currPrefixLength < c_numBitsPerByte)
                {
                    const int bitShiftAmt = (c_numBitsPerByte - std::max(currPrefixLength, 0));
                    addressPointer[i] &= (0xFF >> bitShiftAmt) << bitShiftAmt;
                }
            }

            return SockaddrToString(address) + L"/" + std::to_wstring(PrefixLength);
        }
    };

    struct InterfaceState
    {
        std::wstring Name;
        std::vector<IpAddress> V4Addresses;
        std::optional<std::wstring> Gateway;
        std::vector<IpAddress> V6Addresses;
        std::optional<std::wstring> V6Gateway;

        bool Up = false;
        int Mtu = 0;
        bool Rename = false;
    };

    struct Route
    {
        std::wstring Via;
        std::wstring Device;
        std::optional<std::wstring> Prefix;
        int Metric = 0;

        bool operator==(const Route& other) const
        {
            return Via == other.Via && Device == other.Device && Prefix == other.Prefix;
        }
    };

    struct RoutingTableState
    {
        std::optional<Route> DefaultRoute;
        std::vector<Route> Routes;
    };

    enum class FirewallType
    {
        Host,
        HyperV
    };

    struct FirewallRule
    {
        FirewallType Type;
        std::wstring Name;
        std::wstring RemotePorts;
        std::wstring Action;
        std::wstring VmCreatorId;
    };

    GUID AdapterId;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(false), TRUE);

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (LxsstuVmMode())
        {
            WslShutdown();
        }

        VERIFY_NO_THROW(LxsstuUninitialize(false));

        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        if (!LxsstuVmMode())
        {
            return true;
        }

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(
            L"readlink /sys/class/net/eth0 | grep -o -E '[[:xdigit:]]{8}(-[[:xdigit:]]{4}){3}-[[:xdigit:]]{12}'", 0);
        out.pop_back();

        const auto guid = wsl::shared::string::ToGuid(out);
        VERIFY_IS_TRUE(guid.has_value());

        AdapterId = guid.value();
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ln -f -s /init /gns"), (DWORD)0);

        return true;
    }

    TEST_METHOD(RemoveAndAddDefaultRoute)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});

        // Verify that the default routes are set
        auto state = GetIpv4RoutingTableState();
        VERIFY_IS_TRUE(state.DefaultRoute.has_value());
        VERIFY_ARE_EQUAL(state.DefaultRoute->Via, L"192.168.0.1");

        auto v6State = GetIpv6RoutingTableState();
        VERIFY_IS_TRUE(v6State.DefaultRoute.has_value());
        VERIFY_ARE_EQUAL(v6State.DefaultRoute->Via, L"fc00::1");

        // Now remove them
        wsl::shared::hns::Route route;
        route.NextHop = L"192.168.0.1";
        route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
        route.Family = AF_INET;
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Remove, GuestEndpointResourceType::Route);

        wsl::shared::hns::Route v6Route;
        v6Route.NextHop = L"fc00::1";
        v6Route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_V6_PREFIX;
        v6Route.Family = AF_INET6;
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Remove, GuestEndpointResourceType::Route);

        // Verify that the routes are removed
        state = GetIpv4RoutingTableState();
        VERIFY_IS_FALSE(state.DefaultRoute.has_value());

        v6State = GetIpv6RoutingTableState();
        VERIFY_IS_FALSE(v6State.DefaultRoute.has_value());

        // Add them again
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Add, GuestEndpointResourceType::Route);
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Add, GuestEndpointResourceType::Route);

        // Verify that the routes are restored
        state = GetIpv4RoutingTableState();
        VERIFY_IS_TRUE(state.DefaultRoute.has_value());
        VERIFY_ARE_EQUAL(state.DefaultRoute->Via, L"192.168.0.1");
        VERIFY_ARE_EQUAL(state.DefaultRoute->Device, L"eth0");

        v6State = GetIpv6RoutingTableState();
        VERIFY_IS_TRUE(v6State.DefaultRoute.has_value());
        VERIFY_ARE_EQUAL(v6State.DefaultRoute->Via, L"fc00::1");
        VERIFY_ARE_EQUAL(v6State.DefaultRoute->Device, L"eth0");
    }

    TEST_METHOD(SetInterfaceDownAndUp)
    {
        WSL2_TEST_ONLY();

        // Disconnect interface
        wsl::shared::hns::NetworkInterface link;
        link.Connected = false;
        RunGns(link, ModifyRequestType::Update, GuestEndpointResourceType::Interface);
        VERIFY_IS_FALSE(GetInterfaceState(L"eth0").Up);

        // Connect it again
        link.Connected = true;
        RunGns(link, ModifyRequestType::Update, GuestEndpointResourceType::Interface);
        VERIFY_IS_TRUE(GetInterfaceState(L"eth0").Up);
    }

    TEST_METHOD(SetMtu)
    {
        WSL2_TEST_ONLY();

        // Set MTU - must be 1280 bytes or above to meet IPv6 minimum MTU requirement
        wsl::shared::hns::NetworkInterface link;
        link.Connected = true;
        link.NlMtu = 1280;
        RunGns(link, ModifyRequestType::Update, GuestEndpointResourceType::Interface);
        VERIFY_ARE_EQUAL(GetInterfaceState(L"eth0").Mtu, 1280);
    }

    TEST_METHOD(AddAndRemoveCustomRoute)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});

        // Add custom routes, one per address family
        wsl::shared::hns::Route route;
        route.NextHop = L"192.168.0.12";
        route.DestinationPrefix = L"192.168.2.0/24";
        route.Family = AF_INET;
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        wsl::shared::hns::Route v6Route;
        v6Route.NextHop = L"fc00::12";
        v6Route.DestinationPrefix = L"fc00:abcd::/80";
        v6Route.Family = AF_INET6;
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        // Check that the routes are there
        const bool v4CustomRouteExists = RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24"});
        const bool v6CustomRouteExists = RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/80"});

        // Now remove them
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Remove, GuestEndpointResourceType::Route);
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Remove, GuestEndpointResourceType::Route);

        // Check that the routes are gone
        const bool v4CustomRouteGone = !RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24"});
        const bool v6CustomRouteGone = !RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/80"});

        VERIFY_IS_TRUE(v4CustomRouteExists);
        VERIFY_IS_TRUE(v6CustomRouteExists);

        VERIFY_IS_TRUE(v4CustomRouteGone);
        VERIFY_IS_TRUE(v6CustomRouteGone);
    }

    TEST_METHOD(AddRouteWithMetrics)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});

        // Add a custom route per address family
        wsl::shared::hns::Route route;
        route.NextHop = L"192.168.0.12";
        route.DestinationPrefix = L"192.168.2.0/24";
        route.Family = AF_INET;
        route.Metric = 12;
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        wsl::shared::hns::Route v6Route;
        v6Route.NextHop = L"fc00::12";
        v6Route.DestinationPrefix = L"fc00:abcd::/64";
        v6Route.Family = AF_INET6;
        v6Route.Metric = 12;
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        // Check that the routes are there
        const bool v4CustomRouteExists = RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24", 12});
        const bool v6CustomRouteExists = RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/64", 12});

        // Now remove them
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Remove, GuestEndpointResourceType::Route);
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Remove, GuestEndpointResourceType::Route);

        // Check that the routes are gone
        const bool v4CustomRouteGone = !RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24", 12});
        const bool v6CustomRouteGone = !RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/64", 12});

        VERIFY_IS_TRUE(v4CustomRouteExists);
        VERIFY_IS_TRUE(v6CustomRouteExists);

        VERIFY_IS_TRUE(v4CustomRouteGone);
        VERIFY_IS_TRUE(v6CustomRouteGone);
    }

    TEST_METHOD(ResetRoutes)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});

        // Add a custom route per address family
        wsl::shared::hns::Route route;
        route.NextHop = L"192.168.0.12";
        route.DestinationPrefix = L"192.168.2.0/24";
        route.Family = AF_INET;
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        wsl::shared::hns::Route v6Route;
        v6Route.NextHop = L"fc00::12";
        v6Route.DestinationPrefix = L"fc00:abcd::/80";
        v6Route.Family = AF_INET6;
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        // Check that the custom routes are there
        bool v4RouteExists = RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24"});
        bool v6RouteExists = RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/80"});

        // Reset the routing table
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Reset, GuestEndpointResourceType::Route);
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Reset, GuestEndpointResourceType::Route);

        // Check that both routes are gone, per address family
        bool v4RouteGoneAfterReset = !RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24"});
        auto state = GetIpv4RoutingTableState();
        bool v4GwGoneAfterReset = !state.DefaultRoute.has_value();

        bool v6RouteGoneAfterReset = !RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/80"});
        auto v6State = GetIpv6RoutingTableState();
        bool v6GwGoneAfterReset = !v6State.DefaultRoute.has_value();

        // Add the custom and default routes back
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Update, GuestEndpointResourceType::Route);
        route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
        route.NextHop = L"192.168.0.1";
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Update, GuestEndpointResourceType::Route);
        v6Route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_V6_PREFIX;
        v6Route.NextHop = L"fc00::1";
        SendDeviceSettingsRequest(L"eth0", v6Route, ModifyRequestType::Update, GuestEndpointResourceType::Route);

        // Verify that all the routes are there
        bool v4RouteRestored = RouteExists({L"192.168.0.12", L"eth0", L"192.168.2.0/24"});
        state = GetIpv4RoutingTableState();
        bool v4GwRestored = state.DefaultRoute.has_value();
        bool v4GwRestoredCorrectly = state.DefaultRoute->Via == L"192.168.0.1";

        bool v6RouteRestored = RouteExists({L"fc00::12", L"eth0", L"fc00:abcd::/80"});
        v6State = GetIpv6RoutingTableState();
        bool v6GwRestored = v6State.DefaultRoute.has_value();
        bool v6GwRestoredCorrectly = v6State.DefaultRoute->Via == L"fc00::1";

        VERIFY_IS_TRUE(v4RouteExists);
        VERIFY_IS_TRUE(v6RouteExists);

        VERIFY_IS_TRUE(v4RouteGoneAfterReset);
        VERIFY_IS_TRUE(v4GwGoneAfterReset);
        VERIFY_IS_TRUE(v6RouteGoneAfterReset);
        VERIFY_IS_TRUE(v6GwGoneAfterReset);

        VERIFY_IS_TRUE(v4RouteRestored);
        VERIFY_IS_TRUE(v4GwRestored);
        VERIFY_IS_TRUE(v4GwRestoredCorrectly);
        VERIFY_IS_TRUE(v6RouteRestored);
        VERIFY_IS_TRUE(v6GwRestored);
        VERIFY_IS_TRUE(v6GwRestoredCorrectly);
    }

    TEST_METHOD(ResetRoutesTwice)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});

        auto state = GetIpv4RoutingTableState();
        VERIFY_IS_TRUE(state.DefaultRoute.has_value());

        auto v6State = GetIpv6RoutingTableState();
        VERIFY_IS_TRUE(v6State.DefaultRoute.has_value());

        // Reset the IPv4 table twice
        wsl::shared::hns::Route route;
        route.Family = AF_INET;
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Reset, GuestEndpointResourceType::Route);
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Reset, GuestEndpointResourceType::Route);

        state = GetIpv4RoutingTableState();
        VERIFY_IS_FALSE(state.DefaultRoute.has_value());
        VERIFY_IS_TRUE(state.Routes.empty());

        // Then reset the IPv6 table twice
        route.Family = AF_INET6;
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Reset, GuestEndpointResourceType::Route);
        SendDeviceSettingsRequest(L"eth0", route, ModifyRequestType::Reset, GuestEndpointResourceType::Route);

        state = GetIpv6RoutingTableState();
        VERIFY_IS_FALSE(state.DefaultRoute.has_value());
        VERIFY_IS_TRUE(state.Routes.empty());
    }

    TEST_METHOD(UpdateIpAddress)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});

        // Verify that the IPs are in the preferred state
        auto interfaceState = GetInterfaceState(L"eth0");
        VERIFY_ARE_EQUAL(1, interfaceState.V4Addresses.size());
        VERIFY_ARE_EQUAL(L"192.168.0.2", interfaceState.V4Addresses[0].Address);
        VERIFY_IS_TRUE(interfaceState.V4Addresses[0].Preferred);

        VERIFY_ARE_EQUAL(1, interfaceState.V6Addresses.size());
        VERIFY_ARE_EQUAL(L"fc00::2", interfaceState.V6Addresses[0].Address);
        VERIFY_IS_TRUE(interfaceState.V6Addresses[0].Preferred);

        // Change current ip addresses to be deprecated
        wsl::shared::hns::IPAddress address;
        address.Address = L"192.168.0.2";
        address.OnLinkPrefixLength = 24;
        address.Family = AF_INET;
        address.PreferredLifetime = 0;
        SendDeviceSettingsRequest(L"eth0", address, ModifyRequestType::Update, GuestEndpointResourceType::IPAddress);

        wsl::shared::hns::IPAddress v6Address;
        v6Address.Address = L"fc00::2";
        v6Address.OnLinkPrefixLength = 64;
        v6Address.Family = AF_INET6;
        address.PreferredLifetime = 0;
        SendDeviceSettingsRequest(L"eth0", v6Address, ModifyRequestType::Update, GuestEndpointResourceType::IPAddress);

        // Validate that the IPs are no longer preferred
        interfaceState = GetInterfaceState(L"eth0");
        VERIFY_ARE_EQUAL(1, interfaceState.V4Addresses.size());
        VERIFY_ARE_EQUAL(L"192.168.0.2", interfaceState.V4Addresses[0].Address);
        VERIFY_IS_FALSE(interfaceState.V4Addresses[0].Preferred);

        VERIFY_ARE_EQUAL(1, interfaceState.V6Addresses.size());
        VERIFY_ARE_EQUAL(L"fc00::2", interfaceState.V6Addresses[0].Address);
        VERIFY_IS_FALSE(interfaceState.V6Addresses[0].Preferred);
    }

    enum IpPrefixOrigin
    {
        IpPrefixOriginOther = 0,
        IpPrefixOriginManual,
        IpPrefixOriginWellKnown,
        IpPrefixOriginDhcp,
        IpPrefixOriginRouterAdvertisement,
    };

    enum IpSuffixOrigin
    {
        IpSuffixOriginOther = 0,
        IpSuffixOriginManual,
        IpSuffixOriginWellKnown,
        IpSuffixOriginDhcp,
        IpSuffixOriginLinkLayerAddress,
        IpSuffixOriginRandom,
    };

    TEST_METHOD(TemporaryAddress)
    {
        WSL2_TEST_ONLY();

        TestCase({{L"eth0", {}, {}, {{L"fc00::2", 64}}, L"fc00::1"}});

        // Make the address public
        wsl::shared::hns::IPAddress v6Address;
        v6Address.Address = L"fc00::2";
        v6Address.OnLinkPrefixLength = 64;
        v6Address.Family = AF_INET6;
        v6Address.PrefixOrigin = IpPrefixOriginRouterAdvertisement;
        v6Address.SuffixOrigin = IpSuffixOriginLinkLayerAddress;
        v6Address.PreferredLifetime = 0xFFFFFFFF;
        SendDeviceSettingsRequest(L"eth0", v6Address, ModifyRequestType::Update, GuestEndpointResourceType::IPAddress);

        // Add a temporary address
        v6Address.Address = L"fc00::abcd:1234:5678:9999";
        v6Address.OnLinkPrefixLength = 64;
        v6Address.Family = AF_INET6;
        v6Address.PrefixOrigin = IpPrefixOriginRouterAdvertisement;
        v6Address.SuffixOrigin = IpSuffixOriginRandom;
        v6Address.PreferredLifetime = 0xFFFFFFFF;
        SendDeviceSettingsRequest(L"eth0", v6Address, ModifyRequestType::Add, GuestEndpointResourceType::IPAddress);

        // Wait for DAD to finish to avoid it being a factor in source address selection
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        VERIFY_ARE_EQUAL(2, GetInterfaceState(L"eth0").V6Addresses.size());

        // Ensure that the temporary address is preferred during source address selection
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"ip route get 2001::5");
        LogInfo("'ip route get 2001::5' - '%ls'", out.c_str());

        auto [out5, _5] = LxsstuLaunchWslAndCaptureOutput(L"ip addr show eth0");
        LogInfo("[TemporaryAddress] ip addr show output: '%ls'", out5.c_str());

        std::wsmatch match;
        std::wregex pattern(L"2001::5 from :: via fc00::1 dev eth0 proto kernel src ([a-f,A-F,0-9,:]+)");
        VERIFY_IS_TRUE(std::regex_search(out, match, pattern));
        VERIFY_ARE_EQUAL(2, match.size());
        VERIFY_ARE_EQUAL(L"fc00::abcd:1234:5678:9999", match.str(1));

        // Make another public address
        v6Address.Address = L"fc00::3";
        v6Address.OnLinkPrefixLength = 64;
        v6Address.Family = AF_INET6;
        v6Address.PrefixOrigin = IpPrefixOriginRouterAdvertisement;
        v6Address.SuffixOrigin = IpSuffixOriginLinkLayerAddress;
        v6Address.PreferredLifetime = 0xFFFFFFFF;
        SendDeviceSettingsRequest(L"eth0", v6Address, ModifyRequestType::Add, GuestEndpointResourceType::IPAddress);

        // Test source address selection again
        auto [out2, _2] = LxsstuLaunchWslAndCaptureOutput(L"ip route get 2001::6");
        LogInfo("'ip route get 2001::6' - '%ls'", out2.c_str());

        std::wregex pattern2(L"2001::6 from :: via fc00::1 dev eth0 proto kernel src ([a-f,A-F,0-9,:]+)");
        VERIFY_IS_TRUE(std::regex_search(out2, match, pattern2));
        VERIFY_ARE_EQUAL(2, match.size());
        VERIFY_ARE_EQUAL(L"fc00::abcd:1234:5678:9999", match.str(1));
    }

    TEST_METHOD(SimpleCase)
    {
        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});
    }

    TEST_METHOD(AddressChange)
    {
        TestCase(
            {{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"},
             {L"eth0", {{L"192.168.0.3", 24}}, L"192.168.0.1", {{L"fc00::3", 64}}, L"fc00::1"}});
    }

    TEST_METHOD(GatewayChange)
    {
        TestCase(
            {{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"},
             {L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.3", {{L"fc00::2", 64}}, L"fc00::3"}});
    }

    TEST_METHOD(NetworkChange)
    {
        TestCase(
            {{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"},
             {L"eth0", {{L"10.0.0.2", 16}}, L"10.0.0.1", {{L"fc00:abcd::5", 80}}, L"fc00:abcd::1"}});
    }

    TEST_METHOD(NetworkChangeAndBack)
    {
        TestCase(
            {{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"},
             {L"eth0", {{L"10.0.0.2", 16}}, L"10.0.0.1", {{L"fc00:abcd::5", 80}}, L"fc00:abcd::1"},
             {L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});
    }

    TEST_METHOD(NoChange)
    {
        TestCase(
            {{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"},
             {L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1"}});
    }

    TEST_METHOD(MultipleIps)
    {
        TestCase(
            {{L"eth0",
              {{L"192.168.0.2", 24}, {L"192.168.0.3", 24}},
              L"192.168.0.1",
              {{L"fc00::2", 64}, {L"fc00::3", 64}},
              L"fc00::1"}});
    }

    TEST_METHOD(MacAddressChangeAndBack)
    {
        WSL2_TEST_ONLY();

        const auto originalMac = GetMacAddress();

        wsl::shared::hns::MacAddress macAddress;
        macAddress.PhysicalAddress = "AA-AA-FF-FF-FF-FF";
        SendDeviceSettingsRequest(L"eth0", macAddress, ModifyRequestType::Update, GuestEndpointResourceType::MacAddress);
        VERIFY_ARE_EQUAL(GetMacAddress(), L"aa:aa:ff:ff:ff:ff");

        macAddress.PhysicalAddress = wsl::shared::string::WideToMultiByte(originalMac);
        std::replace(macAddress.PhysicalAddress.begin(), macAddress.PhysicalAddress.end(), ':', '-');
        SendDeviceSettingsRequest(L"eth0", macAddress, ModifyRequestType::Update, GuestEndpointResourceType::MacAddress);
        VERIFY_ARE_EQUAL(GetMacAddress(), originalMac);
    }

    static void VerifyDigDnsResolution(const std::wstring& digCommandLine)
    {
        // dig has exit code 0 when it receives a DNS response
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(digCommandLine.data(), 0);

        // Verify dig returned a non-empty output
        VERIFY_IS_TRUE(!out.empty());
    }

    static void VerifyDnsQueries()
    {
        // query for A/IPv4 records
        VerifyDigDnsResolution(L"dig +short +time=5 A bing.com");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 A bing.com");

        // query for AAAA/IPv6 records
        VerifyDigDnsResolution(L"dig +short +time=5 AAAA bing.com");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 AAAA bing.com");

        // query for MX records
        VerifyDigDnsResolution(L"dig +short +time=5 MX bing.com");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 MX bing.com");

        // query for NS records
        VerifyDigDnsResolution(L"dig +short +time=5 NS bing.com");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 NS bing.com");

        // reverse DNS lookup
        VerifyDigDnsResolution(L"dig +short +time=5 -x 8.8.8.8");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 -x 8.8.8.8");

        // query for SOA records
        VerifyDigDnsResolution(L"dig +short +time=5 SOA bing.com");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 SOA bing.com");

        // query for TXT records
        VerifyDigDnsResolution(L"dig +short +time=5 TXT bing.com");
        VerifyDigDnsResolution(L"dig +tcp +short +time=5 TXT bing.com");

        // query for CNAME records
        VerifyDigDnsResolution(L"dig +time=5 CNAME bing.com");
        VerifyDigDnsResolution(L"dig +tcp +time=5 CNAME bing.com");

        // query for SRV records
        VerifyDigDnsResolution(L"dig +time=5 SRV bing.com");
        VerifyDigDnsResolution(L"dig +tcp +time=5 SRV bing.com");

        // query for ANY - for this option dig expects a large response so it will query directly over TCP,
        // instead of trying UDP first and falling back to TCP.
        VerifyDigDnsResolution(L"dig +short ANY bing.com");
    }

    static void VerifyDnsSuffixes()
    {
        bool foundSuffix = false;

        // Verify global DNS suffixes are reflected in Linux
        auto [outGlobal, errGlobal] = LxsstuLaunchPowershellAndCaptureOutput(
            L"Get-DnsClientGlobalSetting | Select-Object -Property SuffixSearchList | ForEach-Object {$_.SuffixSearchList}");

        const std::wstring separators = L" \n\t\r";

        for (const auto& suffix : wsl::shared::string::SplitByMultipleSeparators(outGlobal, separators))
        {
            if (!suffix.empty())
            {
                foundSuffix = true;
                // use grep -F as suffixes can contain '.'
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"cat /etc/resolv.conf | grep search | grep -F " + suffix), static_cast<DWORD>(0));
            }
        }

        // Verify per-interface DNS suffixes are reflected in Linux
        auto [outPerInterface, errPerInterface] =
            LxsstuLaunchPowershellAndCaptureOutput(L"Get-DnsClient | ForEach-Object {$_.ConnectionSpecificSuffix}");

        for (const auto& suffix : wsl::shared::string::SplitByMultipleSeparators(outPerInterface, separators))
        {
            if (!suffix.empty())
            {
                foundSuffix = true;
                // use grep -F as suffixes can contain '.'
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"cat /etc/resolv.conf | grep search | grep -F " + suffix), static_cast<DWORD>(0));
            }
        }

        // No suffix was found - configure a dummy global suffix, verify it's reflected in Linux, then delete it
        if (!foundSuffix)
        {
            LxsstuLaunchPowershellAndCaptureOutput(L"Set-DnsClientGlobalSetting -SuffixSearchList @('test.com')");
            auto restoreGlobalSuffixes = wil::scope_exit(
                [&] { LxsstuLaunchPowershellAndCaptureOutput(L"Set-DnsClientGlobalSetting -SuffixSearchList @()"); });

            std::this_thread::sleep_for(std::chrono::seconds(1));

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"cat /etc/resolv.conf | grep search | grep -F test.com"), static_cast<DWORD>(0));

            LxsstuLaunchPowershellAndCaptureOutput(L"Set-DnsClientGlobalSetting -SuffixSearchList @()");
            std::this_thread::sleep_for(std::chrono::seconds(1));

            VERIFY_ARE_NOT_EQUAL(LxsstuLaunchWsl(L"cat /etc/resolv.conf | grep search | grep -F test.com"), static_cast<DWORD>(0));
        }
    }

    static void VerifyEtcHosts()
    {
        const auto windowsHostsPath = "C:\\Windows\\System32\\drivers\\etc\\hosts";

        // Save existing Windows /etc/hosts
        std::wifstream windowsHostsRead(windowsHostsPath);
        const auto oldWindowsHosts = std::wstring{std::istreambuf_iterator<wchar_t>(windowsHostsRead), {}};
        windowsHostsRead.close();

        auto restoreWindowsHosts = wil::scope_exit([&] {
            std::wofstream windowsHostsWrite(windowsHostsPath);
            windowsHostsWrite << oldWindowsHosts;
        });

        // Add dummy entry matching bing.com to IP 1.2.3.4
        std::wofstream windowsHostsWrite(windowsHostsPath, std::ios_base::app);
        windowsHostsWrite << "\n1.2.3.4 bing.com";
        windowsHostsWrite.close();

        // Verify Linux /etc/hosts does *not* contain 1.2.3.4
        VERIFY_ARE_NOT_EQUAL(LxsstuLaunchWsl(L"cat /etc/hosts | grep -F 1.2.3.4"), static_cast<DWORD>(0));

        // Verify bing.com gets resolved to 1.2.3.4 by dig
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"dig bing.com | grep -F 1.2.3.4"), static_cast<DWORD>(0));
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"dig +tcp bing.com | grep -F 1.2.3.4"), static_cast<DWORD>(0));
    }

    static void VerifyDnsTunneling(const std::wstring& dnsTunnelingIpAddress)
    {
        // Verify /etc/resolv.conf is configured with the expected nameserver
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"cat /etc/resolv.conf | grep nameserver | grep -F " + dnsTunnelingIpAddress), static_cast<DWORD>(0));

        // Verify that we have a working connection.
        GuestClient(L"tcp-connect:bing.com:80");

        // Verify multiple types of DNS queries
        VerifyDnsQueries();

        // Verify resolution via Windows /etc/hosts
        VerifyEtcHosts();

        // Verify DNS tunneling works with systemd enabled
        auto revert = EnableSystemd();

        GuestClient(L"tcp-connect:bing.com:80");
        VerifyDnsQueries();
    }

    TEST_METHOD(NatDnsTunneling)
    {
        DNS_TUNNELING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.dnsTunneling = true}));

        VerifyDnsTunneling(c_dnsTunnelingDefaultIp);
    }

    TEST_METHOD(NatDnsTunnelingWithSpecificIp)
    {
        DNS_TUNNELING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.dnsTunneling = true, .dnsTunnelingIpAddress = L"10.255.255.1"}));

        VerifyDnsTunneling(L"10.255.255.1");
    }

    TEST_METHOD(NatDnsTunnelingVerifySuffixes)
    {
        DNS_TUNNELING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.dnsTunneling = true}));

        VerifyDnsSuffixes();
    }

    TEST_METHOD(MirroredDnsTunneling)
    {
        DNS_TUNNELING_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .dnsTunneling = true}));
        WaitForMirroredStateInLinux();

        VerifyDnsTunneling(c_dnsTunnelingDefaultIp);
    }

    TEST_METHOD(MirroredDnsTunnelingWithSpecificIp)
    {
        DNS_TUNNELING_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig(
            {.networkingMode = wsl::core::NetworkingMode::Mirrored, .dnsTunneling = true, .dnsTunnelingIpAddress = L"10.255.255.1"}));
        WaitForMirroredStateInLinux();

        VerifyDnsTunneling(L"10.255.255.1");
    }

    TEST_METHOD(MirroredDnsTunnelingVerifySuffixes)
    {
        DNS_TUNNELING_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .dnsTunneling = true}));
        WaitForMirroredStateInLinux();

        VerifyDnsSuffixes();
    }

    TEST_METHOD(MirroredWithoutTunnelingVerifySuffixes)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .dnsTunneling = false}));
        WaitForMirroredStateInLinux();

        VerifyDnsSuffixes();
    }

    TEST_METHOD(NatWithoutIcsDnsProxy)
    {
        WSL2_TEST_ONLY();

        // Verify WSL has connectivity in NAT mode when the ICS DNS proxy is turned off (in which case the DNS servers
        // from Windows are mirrored in Linux)
        WslConfigChange config(LxssGenerateTestConfig({.dnsProxy = false}));

        GuestClient(L"tcp-connect:bing.com:80");
    }

    TEST_METHOD(DnsChange)
    {
        WSL2_TEST_ONLY();

        wsl::shared::hns::DNS dns;
        dns.ServerList = {L"1.1.1.1"};
        dns.Options = LX_INIT_RESOLVCONF_FULL_HEADER;
        RunGns(dns, ModifyRequestType::Update, GuestEndpointResourceType::DNS);

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /etc/resolv.conf", 0);
        const std::wstring expected = std::wstring(LX_INIT_RESOLVCONF_FULL_HEADER) + L"nameserver 1.1.1.1\n";
        VERIFY_ARE_EQUAL(expected, out.c_str());
    }

    TEST_METHOD(DnsChangeMultipleServerAndSearch)
    {
        WSL2_TEST_ONLY();

        wsl::shared::hns::DNS dns;
        dns.ServerList = L"1.1.1.1,1.1.1.2";
        dns.Domain = L"microsoft.com";
        dns.Search = L"foo.microsoft.com,bar.microsoft.com";
        dns.Options = LX_INIT_RESOLVCONF_FULL_HEADER;
        RunGns(dns, ModifyRequestType::Update, GuestEndpointResourceType::DNS);

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /etc/resolv.conf", 0);

        const std::wstring expected = std::wstring(LX_INIT_RESOLVCONF_FULL_HEADER) +
                                      L"nameserver 1.1.1.1\n"
                                      L"nameserver 1.1.1.2\n"
                                      L"domain microsoft.com\n"
                                      L"search foo.microsoft.com bar.microsoft.com\n";
        VERIFY_ARE_EQUAL(expected, out.c_str());
    }

    static void ClearHttpProxySettings(bool userScope)
    {
        auto command = L"Set-WinhttpProxy -SettingScope Machine -Proxy \\\"\\\"";
        if (userScope)
        {
            command = L"Set-WinhttpProxy -SettingScope User -Proxy \\\"\\\"";
        }
        LxsstuLaunchPowershellAndCaptureOutput(command);
    }

    static void SetHttpProxySettings(const std::wstring& proxyString, const std::wstring& bypasses, const std::wstring& autoconfigUrl, bool userScope)
    {
        std::wstringstream proxySettings{};
        if (userScope)
        {
            proxySettings << L" -SettingScope User";
        }
        else
        {
            proxySettings << L" -SettingScope Machine";
        }
        if (!proxyString.empty())
        {
            proxySettings << L" -Proxy " + proxyString;
        }
        if (!bypasses.empty())
        {
            proxySettings << L" -ProxyBypass \\\"" + bypasses + L"\\\"";
        }
        if (!autoconfigUrl.empty())
        {
            proxySettings << L" -AutoconfigUrl " + autoconfigUrl;
        }
        LogInfo("SetHttpProxySettings %ls", proxySettings.str().c_str());
        auto [out, _] = LxsstuLaunchPowershellAndCaptureOutput(L"Set-WinhttpProxy" + proxySettings.str());
        LogInfo("WinhttpProxy %ls", out.c_str());
    }

    static constexpr auto c_httpProxyLower = L"http_proxy";
    static constexpr auto c_httpProxyUpper = L"HTTP_PROXY";
    static constexpr auto c_httpsProxyLower = L"https_proxy";
    static constexpr auto c_httpsProxyUpper = L"HTTPS_PROXY";
    static constexpr auto c_proxyBypassLower = L"no_proxy";
    static constexpr auto c_proxyBypassUpper = L"NO_PROXY";
    static constexpr auto c_pacProxy = L"WSL_PAC_URL";
    static constexpr auto c_httpProxyString = L"http://test.com:8888";
    static constexpr auto c_httpProxyString2 = L"http://otherServer.com:1234";
    static constexpr auto c_httpProxyLocalhost = L"http://localhost:8888";
    static constexpr auto c_httpProxyLoopback = L"http://loopback:8888";
    static constexpr auto c_httpProxyLocalhostv4 = L"http://127.0.0.1:8888";
    static constexpr auto c_httpProxyLocalhostv6 = L"http://[::1]:8888";
    static constexpr auto c_httpProxyIpV4 = L"http://198.168.1.128:8888";
    static constexpr auto c_httpProxyIpV6 = L"http://[2001::1]:8888";
    static constexpr auto c_httpProxyBypassString = L"test";
    static constexpr auto c_httpProxyPACurl = L"testpac.pac";

    static void VerifyWslEnvVariable(const std::wstring& envVar, const std::wstring& proxyString)
    {
        auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"echo -n $" + envVar);
        VERIFY_ARE_EQUAL(proxyString, output);
    }

    static void VerifyHttpProxyBypassesMirrored(const std::wstring& bypassString)
    {
        VerifyWslEnvVariable(c_proxyBypassLower, bypassString);
        VerifyWslEnvVariable(c_proxyBypassUpper, bypassString);
    }

    static void VerifyHttpProxyPacUrlMirrored(const std::wstring& pacUrl)
    {
        VerifyWslEnvVariable(c_pacProxy, pacUrl);
    }

    static void VerifyHttpProxyStringMirrored(const std::wstring& proxyString)
    {
        VerifyWslEnvVariable(c_httpProxyLower, proxyString);
        VerifyWslEnvVariable(c_httpProxyUpper, proxyString);
        VerifyWslEnvVariable(c_httpsProxyLower, proxyString);
        VerifyWslEnvVariable(c_httpsProxyUpper, proxyString);
    }

    static void VerifyHttpProxyEnvVariables(const std::wstring& proxyString, const std::wstring& bypassString, const std::wstring& pacUrl)
    {
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"printenv");
        LogInfo("VerifyHttpProxyEnvVariables %ls", out.c_str());

        VerifyHttpProxyStringMirrored(proxyString);
        VerifyHttpProxyBypassesMirrored(bypassString);
        VerifyHttpProxyPacUrlMirrored(pacUrl);
    }

    static void VerifyHttpProxySimple(bool userScope = true)
    {
        auto restoreProxySettings = wil::scope_exit([&] { ClearHttpProxySettings(userScope); });

        SetHttpProxySettings(c_httpProxyString, L"", L"", userScope);
        VerifyHttpProxyEnvVariables(c_httpProxyString, L"", L"");
    }

    static void VerifyNoHttpProxyConfigured(bool userScope = true)
    {
        ClearHttpProxySettings(userScope);
        VerifyHttpProxyEnvVariables(L"", L"", L"");
    }

    static void VerifyHttpProxyWithBypassesConfigured(bool userScope = true)
    {
        auto restoreProxySettings = wil::scope_exit([&] { ClearHttpProxySettings(userScope); });

        SetHttpProxySettings(c_httpProxyString, c_httpProxyBypassString, L"", userScope);
        VerifyHttpProxyEnvVariables(c_httpProxyString, c_httpProxyBypassString, L"");
    }

    static void VerifyHttpProxyChange(bool userScope = true)
    {
        auto restoreProxySettings = wil::scope_exit([&] { ClearHttpProxySettings(userScope); });

        SetHttpProxySettings(c_httpProxyString, L"", L"", userScope);
        VerifyHttpProxyEnvVariables(c_httpProxyString, L"", L"");

        SetHttpProxySettings(c_httpProxyString2, L"", L"", userScope);
        VerifyHttpProxyEnvVariables(c_httpProxyString2, L"", L"");
    }

    static void VerifyHttpProxyAndWslEnv(bool userScope = true)
    {
        auto restoreProxySettings = wil::scope_exit([&] {
            ClearHttpProxySettings(userScope);
            THROW_LAST_ERROR_IF(!SetEnvironmentVariable(c_httpProxyLower, nullptr));
            THROW_LAST_ERROR_IF(!SetEnvironmentVariable(L"WSLENV", nullptr));
        });

        THROW_LAST_ERROR_IF(!SetEnvironmentVariable(c_httpProxyLower, c_httpProxyString));
        std::wstring wslEnvVal{c_httpProxyLower};
        THROW_LAST_ERROR_IF(!SetEnvironmentVariable(L"WSLENV", wslEnvVal.append(L"/u").c_str()));

        VerifyWslEnvVariable(c_httpProxyLower, c_httpProxyString);
        SetHttpProxySettings(c_httpProxyString2, L"", L"", true);
        // the user set environment variable should have priority over the proxy configured on host
        VerifyWslEnvVariable(c_httpProxyLower, c_httpProxyString);
        // this variable was not configured by user, so we use host configured proxy
        VerifyWslEnvVariable(c_httpProxyUpper, c_httpProxyString2);
    }

    static void VerifyHttpProxyFilterByNetworkConfiguration(bool isNatMode)
    {
        auto restoreProxySettings = wil::scope_exit([&] { ClearHttpProxySettings(true); });

        SetHttpProxySettings(c_httpProxyLocalhost, L"", L"", true);
        if (isNatMode)
        {
            VerifyHttpProxyEnvVariables(L"", L"", L"");
        }
        else
        {
            VerifyHttpProxyEnvVariables(c_httpProxyLocalhost, L"", L"");
        }

        ClearHttpProxySettings(true);

        SetHttpProxySettings(c_httpProxyLoopback, L"", L"", true);
        if (isNatMode)
        {
            VerifyHttpProxyEnvVariables(L"", L"", L"");
        }
        else
        {
            VerifyHttpProxyEnvVariables(c_httpProxyLoopback, L"", L"");
        }

        ClearHttpProxySettings(true);

        SetHttpProxySettings(c_httpProxyLocalhostv4, L"", L"", true);
        if (isNatMode)
        {
            VerifyHttpProxyEnvVariables(L"", L"", L"");
        }
        else
        {
            VerifyHttpProxyEnvVariables(c_httpProxyLocalhostv4, L"", L"");
        }

        ClearHttpProxySettings(true);

        SetHttpProxySettings(c_httpProxyLocalhostv4, c_httpProxyBypassString, L"", true);
        if (isNatMode)
        {
            VerifyHttpProxyEnvVariables(L"", L"", L"");
        }
        else
        {
            VerifyHttpProxyEnvVariables(c_httpProxyLocalhostv4, c_httpProxyBypassString, L"");
        }

        ClearHttpProxySettings(true);
        // validate nonloopback v4 still works
        SetHttpProxySettings(c_httpProxyIpV4, L"", L"", true);
        VerifyHttpProxyEnvVariables(c_httpProxyIpV4, L"", L"");

        ClearHttpProxySettings(true);

        SetHttpProxySettings(c_httpProxyIpV6, c_httpProxyBypassString, L"", true);
        // v6 addresses is only supported in mirrored mode
        if (isNatMode)
        {
            VerifyHttpProxyEnvVariables(L"", L"", L"");
        }
        else
        {
            VerifyHttpProxyEnvVariables(c_httpProxyIpV6, c_httpProxyBypassString, L"");
        }

        ClearHttpProxySettings(true);
        // v6 loopback is unsupported in both network modes
        SetHttpProxySettings(c_httpProxyLocalhostv6, L"", L"", true);
        VerifyHttpProxyEnvVariables(L"", L"", L"");
    }

    static void VerifyHttpProxyFilterByNetworkConfigurationNAT()
    {
        VerifyHttpProxyFilterByNetworkConfiguration(true);
    }

    static void VerifyHttpProxyFilterByNetworkConfigurationMirrored()
    {
        VerifyHttpProxyFilterByNetworkConfiguration(false);
    }

    TEST_METHOD(NatHttpProxyVerifyConfigDisabled)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = false}));

        auto restoreProxySettings = wil::scope_exit([&] { ClearHttpProxySettings(true); });
        SetHttpProxySettings(c_httpProxyString, L"", L"", true);
        VerifyHttpProxyEnvVariables(L"", L"", L"");
    }

    TEST_METHOD(NatHttpProxySimple)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));

        VerifyHttpProxySimple();
    }

    TEST_METHOD(NatHttpProxySimpleMachineScope)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));

        // verify with machine scope
        VerifyHttpProxySimple(false);
    }

    TEST_METHOD(NatNoHttpProxyConfigured)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));

        VerifyNoHttpProxyConfigured();
    }

    TEST_METHOD(NatHttpProxyWithBypassesConfigured)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));
        VerifyHttpProxyWithBypassesConfigured();
    }

    TEST_METHOD(NatHttpProxyChange)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));
        VerifyHttpProxyChange();
    }

    TEST_METHOD(NatHttpProxyAndWslEnv)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));
        VerifyHttpProxyAndWslEnv();
    }

    TEST_METHOD(NatHttpProxyFilterByNetworkConfiguration)
    {
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.autoProxy = true}));
        VerifyHttpProxyFilterByNetworkConfigurationNAT();
    }

    TEST_METHOD(MirroredHttpProxyVerifyConfigDisabled)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = false}));
        WaitForMirroredStateInLinux();

        auto restoreProxySettings = wil::scope_exit([&] { ClearHttpProxySettings(true); });
        SetHttpProxySettings(c_httpProxyString, L"", L"", true);
        VerifyHttpProxyEnvVariables(L"", L"", L"");
    }

    TEST_METHOD(MirroredHttpProxySimple)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));
        WaitForMirroredStateInLinux();
        VerifyHttpProxySimple();
    }

    TEST_METHOD(MirroredHttpProxySimpleMachineScope)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));
        WaitForMirroredStateInLinux();

        // verify with machine scope
        VerifyHttpProxySimple(false);
    }

    TEST_METHOD(MirroredNoHttpProxyConfigured)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));
        WaitForMirroredStateInLinux();
        VerifyNoHttpProxyConfigured();
    }

    TEST_METHOD(MirroredHttpProxyWithBypassesConfigured)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));
        WaitForMirroredStateInLinux();
        VerifyHttpProxyWithBypassesConfigured();
    }

    TEST_METHOD(MirroredHttpProxyChange)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));
        WaitForMirroredStateInLinux();
        VerifyHttpProxyChange();
    }

    TEST_METHOD(MirroredHttpProxyAndWslEnv)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));
        WaitForMirroredStateInLinux();
        VerifyHttpProxyAndWslEnv();
    }

    TEST_METHOD(MirroredHttpProxyFilterByNetworkConfiguration)
    {
        MIRRORED_NETWORKING_TEST_ONLY();
        WINHTTP_PROXY_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .autoProxy = true}));

        VerifyHttpProxyFilterByNetworkConfigurationMirrored();
    }

    TEST_METHOD(RenameInterface)
    {
        WSL2_TEST_ONLY();

        // Disconnect "eth0" interface so it can be renamed
        wsl::shared::hns::NetworkInterface link;
        link.Connected = false;
        RunGns(link, ModifyRequestType::Update, GuestEndpointResourceType::Interface);
        const bool eth0Disconnected = !GetInterfaceState(L"eth0").Up;

        TestCase({{L"myeth", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1", false, 1500, true}});
        const bool myethConnected = GetInterfaceState(L"myeth").Up;

        // Disconnect "myeth" interface so it can be restored
        link.Connected = false;
        RunGns(link, ModifyRequestType::Update, GuestEndpointResourceType::Interface);
        const bool myethDisconnected = !GetInterfaceState(L"myeth").Up;

        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1", false, 1500, true}});
        const bool eth0Connected = GetInterfaceState(L"eth0").Up;

        VERIFY_IS_TRUE(eth0Disconnected);
        VERIFY_IS_TRUE(myethConnected);
        VERIFY_IS_TRUE(myethDisconnected);
        VERIFY_IS_TRUE(eth0Connected);
    }

    TEST_METHOD(RenameWifiInterface)
    {
        WSL2_TEST_ONLY();

        std::wstring commandLine(L"wsl.exe bash -c \"zcat /proc/config.gz | grep CONFIG_PROXY_WIFI=y\"");
        const auto out = std::get<0>(LxsstuLaunchCommandAndCaptureOutputWithResult(commandLine.data()));
        if (out.empty())
        {
            LogSkipped("Kernel does not support PROXY_WIFI. Skipping test...");
            return;
        }

        // Disconnect "eth0" interface so it can be renamed
        wsl::shared::hns::NetworkInterface link;
        link.Connected = false;
        RunGns(link, ModifyRequestType::Update, GuestEndpointResourceType::Interface);
        const bool eth0Disconnected = !GetInterfaceState(L"eth0").Up;

        TestCase({{L"wlan0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1", false, 1500, true}});
        const bool _wlan0Connected = GetInterfaceState(L"_wlan0").Up;

        const bool _wlan0Deleted = LxsstuLaunchWsl(L"ip link del wlan0") == (DWORD)0;
        TestCase({{L"eth0", {{L"192.168.0.2", 24}}, L"192.168.0.1", {{L"fc00::2", 64}}, L"fc00::1", false, 1500, true}});
        const bool eth0Connected = GetInterfaceState(L"eth0").Up;

        VERIFY_IS_TRUE(eth0Disconnected);
        VERIFY_IS_TRUE(_wlan0Connected);
        VERIFY_IS_TRUE(_wlan0Deleted);
        VERIFY_IS_TRUE(eth0Connected);
    }

    TEST_METHOD(EnableLoopbackRouting)
    {
        WSL2_TEST_ONLY();

        // Enable accept_local and route_localnet settings for eth0
        wsl::shared::hns::VmNicCreatedNotification creationNotification{AdapterId};
        RunGns(creationNotification, LxGnsMessageVmNicCreatedNotification);

        // Verify the settings were enabled
        const bool acceptLocalEnabled = LxsstuLaunchWsl(L"sysctl net.ipv4.conf.eth0.accept_local | grep -w 1") == (DWORD)0;
        const bool routeLocalnetEnabled = LxsstuLaunchWsl(L"sysctl net.ipv4.conf.eth0.route_localnet | grep -w 1") == (DWORD)0;

        VERIFY_IS_TRUE(acceptLocalEnabled);
        VERIFY_IS_TRUE(routeLocalnetEnabled);
    }

    TEST_METHOD(InitializeLoopbackConfiguration)
    {
        WSL2_TEST_ONLY();

        // Assume eth0 is the GELNIC
        wsl::shared::hns::CreateDeviceRequest createDeviceRequest{wsl::shared::hns::DeviceType::Loopback, L"loopback", AdapterId};
        RunGns(createDeviceRequest, LxGnsMessageCreateDeviceRequest);

        // Verify the expected ip rules are present
        const bool gelnicRuleTcpExists =
            LxsstuLaunchWsl(L"ip rule show | grep \"from all iif eth0 ipproto tcp lookup local\" | grep ^0:") == (DWORD)0;
        const bool gelnicRuleUdpExists =
            LxsstuLaunchWsl(L"ip rule show | grep \"from all iif eth0 ipproto tcp lookup local\" | grep ^0:") == (DWORD)0;

        const bool table127RuleTcpExists =
            LxsstuLaunchWsl(L"ip rule show | grep \"from all ipproto tcp lookup 127\" | grep ^1:") == (DWORD)0;
        const bool table127RuleUdpExists =
            LxsstuLaunchWsl(L"ip rule show | grep \"from all ipproto udp lookup 127\" | grep ^1:") == (DWORD)0;
        const bool table128RuleTcpExists =
            LxsstuLaunchWsl(L"ip rule show | grep \"from all ipproto tcp lookup 128\" | grep ^1:") == (DWORD)0;
        const bool table128RuleUdpExists =
            LxsstuLaunchWsl(L"ip rule show | grep \"from all ipproto udp lookup 128\" | grep ^1:") == (DWORD)0;

        const bool localTableRuleExists = LxsstuLaunchWsl(L"ip rule show | grep \"from all lookup local\" | grep ^2:") == (DWORD)0;

        // Verify that the static neighbor entry was added for the gateway
        const bool gatewayArpEntryExists =
            LxsstuLaunchWsl(L"ip neigh show dev eth0 | grep \"169\\.254\\.73\\.152 lladdr 00:11:22:33:44:55 PERMANENT\"") == (DWORD)0;

        // Verify route was added for destination 127.0.0.1, with preferred source 127.0.0.1
        const bool routeToLoopbackRangeExists =
            LxsstuLaunchWsl(
                L"ip route show table 127 | grep \"127\\.0\\.0\\.1 via 169\\.254\\.73\\.152 dev eth0\" | grep "
                L"\"src 127\\.0\\.0\\.1\" | grep onlink") == (DWORD)0;

        const bool shutdownSuccessful = WslShutdown();

        VERIFY_IS_TRUE(gelnicRuleTcpExists);
        VERIFY_IS_TRUE(gelnicRuleUdpExists);
        VERIFY_IS_TRUE(table127RuleTcpExists);
        VERIFY_IS_TRUE(table127RuleUdpExists);
        VERIFY_IS_TRUE(table128RuleTcpExists);
        VERIFY_IS_TRUE(table128RuleUdpExists);
        VERIFY_IS_TRUE(localTableRuleExists);

        VERIFY_IS_TRUE(gatewayArpEntryExists);
        VERIFY_IS_TRUE(routeToLoopbackRangeExists);

        VERIFY_IS_TRUE(shutdownSuccessful);
    }

    TEST_METHOD(AddRemoveLoopbackRoutesv4)
    {
        WSL2_TEST_ONLY();

        const std::wstring interfaceName = L"eth0";
        const std::vector<std::wstring> ipAddresses = {L"127.0.0.1", L"127.0.0.2"};

        // Add routes on interface eth0 and verify that the routes were added in the custom local routing table (id 128)
        for (const auto address : ipAddresses)
        {
            wsl::shared::hns::LoopbackRoutesRequest addRequest{interfaceName, wsl::shared::hns::OperationType::Create, AF_INET, address};
            RunGns(addRequest, LxGnsMessageLoopbackRoutesRequest);
        }

        const bool firstRouteExists =
            LxsstuLaunchWsl(
                L"ip route show table 128 | grep \"127\\.0\\.0\\.1 via 169\\.254\\.73\\.152 dev eth0\" | grep \"src "
                L"127\\.0\\.0\\.1\" | grep onlink") == (DWORD)0;
        const bool secondRouteExists =
            LxsstuLaunchWsl(
                L"ip route show table 128 | grep \"127\\.0\\.0\\.2 via 169\\.254\\.73\\.152 dev eth0\" | grep \"src "
                L"127\\.0\\.0\\.2\" | grep onlink") == (DWORD)0;

        // Verify that the static neighbor entry was added for the gateway
        const bool gatewayArpEntryExists =
            LxsstuLaunchWsl(L"ip neigh show dev eth0 | grep \"169\\.254\\.73\\.152 lladdr 00:11:22:33:44:55 PERMANENT\"") == (DWORD)0;

        // Verify that the routes are deleted
        for (const auto address : ipAddresses)
        {
            wsl::shared::hns::LoopbackRoutesRequest removeRequest{interfaceName, wsl::shared::hns::OperationType::Remove, AF_INET, address};
            RunGns(removeRequest, LxGnsMessageLoopbackRoutesRequest);
        }

        const bool firstRouteDeleted = LxsstuLaunchWsl(L"ip route show table 128 | grep 127\\.0\\.0\\.1") == (DWORD)1;
        const bool secondRouteDeleted = LxsstuLaunchWsl(L"ip route show table 128 | grep 127\\.0\\.0\\.2") == (DWORD)1;

        const bool shutdownSuccessful = WslShutdown();

        VERIFY_IS_TRUE(firstRouteExists);
        VERIFY_IS_TRUE(secondRouteExists);

        VERIFY_IS_TRUE(gatewayArpEntryExists);

        VERIFY_IS_TRUE(firstRouteDeleted);
        VERIFY_IS_TRUE(secondRouteDeleted);

        VERIFY_IS_TRUE(shutdownSuccessful);
    }

    /*
        The test uses the "ip route get" command, which is equivalent to asking the OS what route it will take for a packet. It
        functions as a small integration test.
    */
    TEST_METHOD(LoopbackGetRoute)
    {
        WSL2_TEST_ONLY();

        // Verify that before configurations are applied, the route chosen for 127.0.0.1 tcp/udp uses the local routing table
        const bool loopbackTcpUsesLocalTable =
            LxsstuLaunchWsl(L"ip route get from 127.0.0.1 127.0.0.1 ipproto tcp | grep local") == (DWORD)0;
        const bool loopbackUdpUsesLocalTable =
            LxsstuLaunchWsl(L"ip route get from 127.0.0.1 127.0.0.1 ipproto udp | grep local") == (DWORD)0;

        // Assume eth0 is the GELNIC
        wsl::shared::hns::CreateDeviceRequest createDeviceRequest{wsl::shared::hns::DeviceType::Loopback, L"loopback", AdapterId};
        RunGns(createDeviceRequest, LxGnsMessageCreateDeviceRequest);

        // Verify that after configurations are applied, the route chosen for 127.0.0.1 tcp/udp is the desired one
        const bool loopbackTcpUsesCustomTable =
            LxsstuLaunchWsl(L"ip route get from 127.0.0.1 127.0.0.1 ipproto tcp | grep \"via 169\\.254\\.73\\.152 dev eth0\"") == (DWORD)0;
        const bool loopbackUdpUsesCustomTable =
            LxsstuLaunchWsl(L"ip route get from 127.0.0.1 127.0.0.1 ipproto udp | grep \"via 169\\.254\\.73\\.152 dev eth0\"") == (DWORD)0;

        const bool shutdownSuccessful = WslShutdown();

        VERIFY_IS_TRUE(loopbackTcpUsesLocalTable);
        VERIFY_IS_TRUE(loopbackUdpUsesLocalTable);

        VERIFY_IS_TRUE(loopbackTcpUsesCustomTable);
        VERIFY_IS_TRUE(loopbackUdpUsesCustomTable);

        VERIFY_IS_TRUE(shutdownSuccessful);
    }

    // Validate that adapter has an ip address, default route and DNS configuration in NAT mode
    TEST_METHOD(NatConfiguration)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig());

        const auto state = GetInterfaceState(L"eth0");
        VERIFY_IS_FALSE(state.V4Addresses.empty());
        VERIFY_IS_TRUE(state.Gateway.has_value());

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /etc/resolv.conf", 0);
        const std::wregex pattern(L"(.|\n)*nameserver [0-9\\. ]+(.|\n)*", std::regex::extended);

        VERIFY_IS_TRUE(std::regex_match(out, pattern));
    }

    static void WriteNatConfiguration(const std::wstring& network, const std::wstring& gateway, const std::wstring& ipAddress)
    {
        using namespace wsl::windows::common;
        const auto key = registry::OpenLxssMachineKey(KEY_SET_VALUE);

        if (gateway == L"delete")
        {
            registry::DeleteValue(key.get(), L"NatGatewayIpAddress");
        }
        else if (!gateway.empty())
        {
            registry::WriteString(key.get(), nullptr, L"NatGatewayIpAddress", gateway.c_str());
        }

        if (network == L"delete")
        {
            registry::DeleteValue(key.get(), L"NatNetwork");
        }
        else if (!network.empty())
        {
            registry::WriteString(key.get(), nullptr, L"NatNetwork", network.c_str());
        }

        const auto userKey = registry::OpenLxssUserKey();
        if (ipAddress == L"delete")
        {
            registry::DeleteValue(userKey.get(), L"NatIpAddress");
        }
        else if (!ipAddress.empty())
        {
            registry::WriteString(userKey.get(), nullptr, L"NatIpAddress", ipAddress.c_str());
        }
    }

    struct NatNetworkingConfiguration
    {
        std::wstring networkRange;
        std::wstring gatewayIpAddress;
        std::wstring ipAddress;
    };

    static NatNetworkingConfiguration GetNatConfiguration()
    {
        using namespace wsl::windows::common;
        const auto key = registry::OpenLxssMachineKey();

        const auto userKey = registry::OpenLxssUserKey();

        return {
            registry::ReadString(key.get(), nullptr, L"NatNetwork", L""),
            registry::ReadString(key.get(), nullptr, L"NatGatewayIpAddress", L""),
            registry::ReadString(userKey.get(), nullptr, L"NatIpAddress", L"")};
    }

    static void ResetWslNetwork()
    {
        // N.B. This must be kept in sync with the network IDs in NatNetworking.cpp.
        GUID natNetworkId;
        if (!AreExperimentalNetworkingFeaturesSupported() || !IsHyperVFirewallSupported())
        {
            natNetworkId = {0xb95d0c5e, 0x57d4, 0x412b, {0xb5, 0x71, 0x18, 0xa8, 0x1a, 0x16, 0xe0, 0x05}};
        }
        else
        {
            natNetworkId = {0x790e58b4, 0x7939, 0x4434, {0x93, 0x58, 0x89, 0xae, 0x7d, 0xdb, 0xe8, 0x7e}};
        }

        wil::unique_cotaskmem_string error;
        const auto hr = HcnDeleteNetwork(natNetworkId, &error);
        VERIFY_SUCCEEDED(hr, error.get());
    }

    TEST_METHOD(NatInvalidRange)
    {
        WSL2_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig());
        WriteNatConfiguration(L"InvalidRange", {}, {L"delete"});
        ResetWslNetwork();
        RestartWslService();

        const auto state = GetInterfaceState(
            L"eth0",
            L"wsl: Failed to create virtual network with address range: 'InvalidRange', created new network with range: "
            L"'*.*.*.*/*', *.*");

        VERIFY_IS_FALSE(state.V4Addresses.empty());
        VERIFY_IS_TRUE(state.Gateway.has_value());

        const auto networkConfiguration = GetNatConfiguration();
        VERIFY_IS_FALSE(networkConfiguration.networkRange.empty());
        VERIFY_ARE_EQUAL(state.V4Addresses[0].Address, networkConfiguration.ipAddress);
        VERIFY_ARE_EQUAL(state.Gateway.value_or(L""), networkConfiguration.gatewayIpAddress);
    }

    TEST_METHOD(NatInvalidGateway)
    {
        WSL2_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig());
        WriteNatConfiguration({}, L"InvalidGateway", {});
        ResetWslNetwork();
        RestartWslService();

        const auto state = GetInterfaceState(
            L"eth0",
            L"wsl: Failed to create virtual network with address range: '*.*.*.*/*', created new network with range: "
            L"'*.*.*.*/*', *.*");

        VERIFY_IS_FALSE(state.V4Addresses.empty());
        VERIFY_IS_TRUE(state.Gateway.has_value());

        const auto networkConfiguration = GetNatConfiguration();
        VERIFY_IS_FALSE(networkConfiguration.networkRange.empty());
        VERIFY_ARE_EQUAL(state.V4Addresses[0].Address, networkConfiguration.ipAddress);
        VERIFY_ARE_EQUAL(state.Gateway.value_or(L""), networkConfiguration.gatewayIpAddress);
    }

    TEST_METHOD(NatInvalidAddress)
    {
        WSL2_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig());

        const auto previousConfiguration = GetNatConfiguration();
        WriteNatConfiguration({}, {}, L"InvalidAddress");
        ResetWslNetwork();
        RestartWslService();

        const auto state = GetInterfaceState(
            L"eth0", L"wsl: Failed to create network endpoint with address: 'InvalidAddress', assigned new address: '*.*.*.*'*");
        VERIFY_IS_FALSE(state.V4Addresses.empty());
        VERIFY_IS_TRUE(state.Gateway.has_value());

        const auto networkConfiguration = GetNatConfiguration();
        // The network range should be the same
        VERIFY_ARE_EQUAL(networkConfiguration.networkRange, previousConfiguration.networkRange);

        VERIFY_IS_FALSE(networkConfiguration.networkRange.empty());
        VERIFY_ARE_EQUAL(state.V4Addresses[0].Address, networkConfiguration.ipAddress);
        VERIFY_ARE_EQUAL(state.Gateway.value_or(L""), networkConfiguration.gatewayIpAddress);
    }

    struct unique_kill_process
    {
        unique_kill_process()
        {
        }
        unique_kill_process(wil::unique_handle&& process) : m_process(std::move(process))
        {
        }

        unique_kill_process(unique_kill_process&&) = default;
        unique_kill_process& operator=(unique_kill_process&&) = default;

        unique_kill_process& operator=(const unique_kill_process&) = delete;
        unique_kill_process(const unique_kill_process&) = delete;

        ~unique_kill_process()
        {
            reset();
        }

        void reset()
        {
            if (m_process)
            {
                TerminateProcess(m_process.get(), 0);
                m_process.reset();
            }
        }

        wil::unique_handle m_process;
    };

    static void VerifyLoopbackHostToGuest(const std::wstring& address, int protocol, std::chrono::duration<int> timeout = std::chrono::minutes(5))
    {
        LogInfo("VerifyLoopbackHostToGuest(address=%ls, protocol=%d)", address.c_str(), protocol);

        SOCKADDR_INET addr = wsl::windows::common::string::StringToSockAddrInet(address);
        SS_PORT(&addr) = htons(1234);

        {
            // Create listener in guest
            std::optional<GuestListener> listener;

            // Note: If a previous test case had the same port bound it can take a bit of time for the port to be released on the host.
            auto createListener = [&]() { listener.emplace(addr, protocol); };
            try
            {
                wsl::shared::retry::RetryWithTimeout<void>(
                    createListener, std::chrono::seconds(1), timeout, []() { return wil::ResultFromCaughtException() == E_FAIL; });
            }
            catch (...)
            {
                LogError("Failed to bind %ls in the guest, 0x%x", address.c_str(), wil::ResultFromCaughtException());
                VERIFY_FAIL();
            }

            // If the guest is listening on any address, connect via loopback.
            const auto ipAddress = (addr.si_family == AF_INET) ? reinterpret_cast<const void*>(&addr.Ipv4.sin_addr)
                                                               : reinterpret_cast<const void*>(&addr.Ipv6.sin6_addr);
            if (INET_IS_ADDR_UNSPECIFIED(addr.si_family, ipAddress))
            {
                INETADDR_SETLOOPBACK(reinterpret_cast<PSOCKADDR>(&addr));
                SS_PORT(&addr) = htons(1234);
            }

            // Connect from a client on the host
            const wil::unique_socket clientSocket(socket(addr.si_family, (protocol == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM, protocol));
            VERIFY_ARE_NOT_EQUAL(clientSocket.get(), INVALID_SOCKET);
            // The WSL2 loopback relay may have a one second delay after creation.

            auto pred = [&]() {
                if (protocol == IPPROTO_UDP)
                {
                    const char buffer = 'A';
                    THROW_HR_IF(
                        E_FAIL,
                        sendto(clientSocket.get(), &buffer, sizeof(buffer), 0, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) !=
                            sizeof(buffer));
                }
                else
                {
                    THROW_HR_IF(E_FAIL, connect(clientSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR);
                }
            };

            try
            {
                wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::seconds(1), timeout);
            }
            catch (...)
            {
                LogError("Timed out trying to connect to %ls", address.c_str());
                VERIFY_FAIL();
            }

            // Verify the connection was accepted on the listener
            listener->AcceptConnection();
        }

        // Wait until the guest has released its port
        VerifyNotBound(addr, addr.si_family, protocol);
    }

    TEST_METHOD(HostToGuestLoopback)
    {
        BEGIN_TEST_METHOD_PROPERTIES()
            TEST_METHOD_PROPERTY(L"Data:NetConfig", L"{1, 2, 3, 4}")
        END_TEST_METHOD_PROPERTIES()

        // All networking modes for both WSL1/2 are expected to support TCP/IPv4 host to guest loopback by default.
        int networkingModeVal = 0;
        WEX::TestExecution::TestData::TryGetValue(L"NetConfig", networkingModeVal);
        auto networkingMode = static_cast<wsl::core::NetworkingMode>(networkingModeVal);
        switch (networkingMode)
        {
        case wsl::core::NetworkingMode::Bridged:
            WINDOWS_11_TEST_ONLY();
            __fallthrough;
        case wsl::core::NetworkingMode::Mirrored:
        case wsl::core::NetworkingMode::VirtioProxy:
            WSL2_TEST_ONLY();
            break;
        }

        LogInfo("HostToGuestLoopback (networkingMode=%hs)", ToString(networkingMode));
        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = networkingMode, .vmSwitch = L"Default Switch"}));
        VerifyLoopbackHostToGuest(L"127.0.0.1", IPPROTO_TCP);
        VerifyLoopbackHostToGuest(L"0.0.0.0", IPPROTO_TCP);
    }

    TEST_METHOD(MirroredSmokeTest)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        // Verify that we have a working connection
        GuestClient(L"tcp-connect:bing.com:80");
    }

    TEST_METHOD(MirroredInternetConnectivityV4)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        if (!HostHasInternetConnectivity(AF_INET))
        {
            LogSkipped("Host does not have IPv4 internet connectivity. Skipping...");
            return;
        }

        GuestClient(L"tcp4-connect:bing.com:80");
    }

    TEST_METHOD(MirroredInternetConnectivityV6)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        if (!HostHasInternetConnectivity(AF_INET6))
        {
            LogSkipped("Host does not have IPv6 internet connectivity. Skipping...");
            return;
        }

        GuestClient(L"tcp6-connect:bing.com:80");
    }

    static void VerifyLoopbackGuestToHost(const std::wstring& address, int protocol)
    {
        LogInfo("VerifyLoopbackGuestToHost(address=%ls, protocol=%d)", address.c_str(), protocol);

        SOCKADDR_INET addr = wsl::windows::common::string::StringToSockAddrInet(address);
        SS_PORT(&addr) = htons(1234);

        // Create a listener on the host
        const wil::unique_socket listenSocket(socket(addr.si_family, (protocol == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM, protocol));
        VERIFY_ARE_NOT_EQUAL(listenSocket.get(), INVALID_SOCKET);
        VERIFY_ARE_NOT_EQUAL(bind(listenSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)), SOCKET_ERROR);
        if (protocol == IPPROTO_TCP)
        {
            VERIFY_ARE_NOT_EQUAL(listen(listenSocket.get(), SOMAXCONN), SOCKET_ERROR);
        }

        // Connect from a client in the guest
        GuestClient client(addr, protocol);

        // Accept the connection on the listener
        SOCKADDR_INET remoteAddr{};
        int remoteAddrLen = sizeof(remoteAddr);
        if (protocol == IPPROTO_UDP)
        {
            char buffer[2048];
            int Timeout = 3000;
            VERIFY_ARE_NOT_EQUAL(setsockopt(listenSocket.get(), SOL_SOCKET, SO_RCVTIMEO, (char*)&Timeout, sizeof(Timeout)), SOCKET_ERROR);
            VERIFY_ARE_NOT_EQUAL(
                recvfrom(listenSocket.get(), buffer, sizeof(buffer), 0, reinterpret_cast<SOCKADDR*>(&remoteAddr), &remoteAddrLen), SOCKET_ERROR);
        }
        else
        {
            // TODO: this accept call needs to timeout to avoid indefinite wait
            const wil::unique_socket acceptSocket(accept(listenSocket.get(), reinterpret_cast<SOCKADDR*>(&remoteAddr), &remoteAddrLen));
            VERIFY_ARE_NOT_EQUAL(acceptSocket.get(), INVALID_SOCKET);
        }
    }

    static void VerifyLoopbackGuestToGuest(const std::wstring& address, int protocol)
    {
        LogInfo("VerifyLoopbackGuestToGuest(address=%ls, protocol=%d)", address.c_str(), protocol);

        SOCKADDR_INET addr = wsl::windows::common::string::StringToSockAddrInet(address);
        SS_PORT(&addr) = htons(1234);

        {
            std::optional<GuestListener> listener;

            auto createListener = [&]() { listener.emplace(addr, protocol); };
            try
            {
                wsl::shared::retry::RetryWithTimeout<void>(createListener, std::chrono::seconds(1), std::chrono::minutes(1), []() {
                    return wil::ResultFromCaughtException() == E_FAIL;
                });
            }
            catch (...)
            {
                LogError("Failed to bind %ls", address.c_str());
                VERIFY_FAIL();
            }

            // Create listener in guest

            // Connect from a client in the guest
            GuestClient client(addr, protocol);

            // Verify the connection was accepted on the listener
            listener->AcceptConnection();
        }

        // Wait until the guest has released its port
        VerifyNotBound(addr, addr.si_family, protocol);
    }

    static void VerifyLoopbackConnectivity(const std::wstring& address)
    {
        // Verify guest to host
        VerifyLoopbackGuestToHost(address, IPPROTO_UDP);
        VerifyLoopbackGuestToHost(address, IPPROTO_TCP);

        // Verify host to guest
        VerifyLoopbackHostToGuest(address, IPPROTO_UDP);
        VerifyLoopbackHostToGuest(address, IPPROTO_TCP);

        // Verify guest to guest
        VerifyLoopbackGuestToGuest(address, IPPROTO_UDP);
        VerifyLoopbackGuestToGuest(address, IPPROTO_TCP);
    }

    TEST_METHOD(MirroredLoopbackLocal)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored, .hostAddressLoopback = true}));
        WaitForMirroredStateInLinux();

        std::vector<InterfaceState> interfaceStates = GetAllInterfaceStates();

        // Verify loopback connectivity on assigned unicast addresses
        for (auto i = interfaceStates.begin(); i != interfaceStates.end(); ++i)
        {
            for (auto j = i->V4Addresses.begin(); j != i->V4Addresses.end(); ++j)
            {
                // The IP used for DNS tunneling is not intended for guest<->host communication
                if (j->Address != c_dnsTunnelingDefaultIp)
                {
                    VerifyLoopbackConnectivity(j->Address);
                }
            }
            for (auto j = i->V6Addresses.begin(); j != i->V6Addresses.end(); ++j)
            {
                // TODO: enable when v6 loopback is supported
                // VerifyLoopbackConnectivity(j->Address);
            }
        }
    }

    TEST_METHOD(MirroredLoopbackExplicit)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        // Verify loopback connectivity on loopback addresses
        VerifyLoopbackConnectivity(L"127.0.0.1");
        // TODO: enable when v6 loopback is supported
        // VerifyLoopbackConnectivity(L"::1");
    }

    TEST_METHOD(MirroredLoopbackSystemd)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        // Write a .conf file to conflict with loopback settings.
#define CONFIG_FILE_PATH L"/etc/sysctl.d/MirroredLoopbackSystemd.conf"
        auto revertConfigFile = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] {
            const std::wstring deleteConfigFileCmd(L"-u root -e rm " CONFIG_FILE_PATH);
            LxsstuLaunchWsl(deleteConfigFileCmd.data());
        });
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"echo \"net.ipv4.conf.*.rp_filter=2\" > " CONFIG_FILE_PATH), static_cast<DWORD>(0));

        // Enable systemd which will apply the .conf file.
        auto revertSystemd = EnableSystemd();

        // Verify the settings configured in the systemd hardening logic.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"sysctl net.ipv4.conf.all.rp_filter | grep -w 0"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"sysctl net.ipv4.conf." TEXT(LX_INIT_LOOPBACK_DEVICE_NAME) L".rp_filter | grep -w 0"), 0);

        // Verify an E2E loopback scenario.
        VerifyLoopbackGuestToHost(L"127.0.0.1", IPPROTO_TCP);
    }

    static wil::unique_socket BindHostPort(uint16_t Port, int Type, int Protocol, bool ExpectSuccess, bool Ipv6 = false, bool Localhost = false)
    {
        int AddressFamily{};
        const SOCKADDR* Address{};
        int AddressSize{};
        SOCKADDR_IN Address4{};
        SOCKADDR_IN6 Address6{};
        if (Ipv6)
        {
            AddressFamily = AF_INET6;
            Address6.sin6_family = AF_INET6;
            Address6.sin6_port = htons(Port);
            if (Localhost)
            {
                Address6.sin6_addr = IN6ADDR_LOOPBACK_INIT;
            }
            Address = reinterpret_cast<SOCKADDR*>(&Address6);
            AddressSize = sizeof(Address6);
        }
        else
        {
            AddressFamily = AF_INET;
            Address4.sin_family = AF_INET;
            Address4.sin_port = htons(Port);
            if (Localhost)
            {
                Address4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            }
            Address = reinterpret_cast<SOCKADDR*>(&Address4);
            AddressSize = sizeof(Address4);
        }

        wil::unique_socket listenSocket(socket(AddressFamily, Type, Protocol));
        VERIFY_IS_TRUE(!!listenSocket);

        VERIFY_ARE_EQUAL(bind(listenSocket.get(), Address, AddressSize) != SOCKET_ERROR, ExpectSuccess);

        return listenSocket;
    }

    static std::tuple<unique_kill_process, bool, wil::unique_handle> BindGuestPortHelper(std::wstring_view BindSpec)
    {
        auto [stdErrRead, stdErrWrite] = CreateSubprocessPipe(false, true);
        auto [stdOutRead, stdOutWrite] = CreateSubprocessPipe(false, true);
        const std::wstring wslCmd = L"socat -dd " + std::wstring(BindSpec) + L" STDOUT";
        auto cmd = LxssGenerateWslCommandLine(wslCmd.data());

        auto process = LxsstuStartProcess(cmd.data(), nullptr, stdOutWrite.get(), stdErrWrite.get());
        stdErrWrite.reset();
        stdOutWrite.reset();

        const std::map<std::string_view, bool> patterns = {
            {"listening on", true},
            {"Address already in use", false},
        };

        bool success = false;
        bool finished = false;
        DWORD writeOffset = 0;
        constexpr DWORD readOffset = 0;
        std::string output(512, '\0');
        while (!finished)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(stdErrRead.get(), output.data() + writeOffset, static_cast<DWORD>(output.size() - writeOffset), &bytesRead, nullptr))
            {
                break;
            }

            writeOffset += bytesRead;
            LogInfo("output %hs", output.c_str());
            std::string_view outputView = output;
            for (const auto& pattern : patterns)
            {
                DWORD patternOffset = readOffset;
                auto matchString = pattern.first;
                while (!finished && (patternOffset + matchString.length() < writeOffset))
                {
                    if (outputView.substr(patternOffset).starts_with(matchString))
                    {
                        finished = true;
                        success = pattern.second;
                    }
                    patternOffset++;
                }
            }
        }

        VERIFY_IS_TRUE(finished);

        return std::tuple(std::move(process), success, std::move(stdOutRead));
    }

    static std::tuple<unique_kill_process, wil::unique_handle> BindGuestPort(std::wstring_view BindSpec, bool ExpectSuccess)
    {
        auto [process, success, read] = BindGuestPortHelper(BindSpec);

        VERIFY_ARE_EQUAL(ExpectSuccess, success);

        return std::tuple(std::move(process), std::move(read));
    }

    template <typename T>
    static void VerifyNotBound(T& Address, int AddressFamily, int Protocol)
    {
        const wil::unique_socket listenSocket(socket(AddressFamily, (Protocol == IPPROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM, Protocol));
        VERIFY_IS_TRUE(!!listenSocket);

        const auto timeout = std::chrono::steady_clock::now() + std::chrono::minutes(2);

        bool bound = false;
        while (!bound && std::chrono::steady_clock::now() < timeout)
        {
            bound = bind(listenSocket.get(), reinterpret_cast<SOCKADDR*>(&Address), sizeof(Address)) != SOCKET_ERROR;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        VERIFY_IS_TRUE(bound);
    }

    static void VerifyNotBoundLoopback(uint16_t port, bool Ipv6)
    {
        if (Ipv6)
        {
            SOCKADDR_IN6 Address{};
            Address.sin6_family = AF_INET6;
            Address.sin6_port = htons(port);
            Address.sin6_addr = IN6ADDR_LOOPBACK_INIT;

            VerifyNotBound(Address, Address.sin6_family, IPPROTO_TCP);
        }
        else
        {
            SOCKADDR_IN Address{};
            Address.sin_family = AF_INET;
            Address.sin_port = htons(port);
            Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            VerifyNotBound(Address, Address.sin_family, IPPROTO_TCP);
        }
    }

    struct
    {
        wchar_t const* const SocatServer = {};
        bool const Ipv6 = false;
        bool const expectRelay = true;
    } LoopbackBindTests[5] = {
        {
            .SocatServer = L"TCP4-LISTEN:1234,bind=127.0.0.1",
        },
        {
            .SocatServer = L"TCP4-LISTEN:1234,bind=127.0.0.2",
            .expectRelay = false,
        },
        {
            .SocatServer = L"TCP4-LISTEN:1234,bind=0.0.0.0",
        },
        {
            .SocatServer = L"TCP6-LISTEN:1234,bind=::1",
            .Ipv6 = true,
        },
        {
            .SocatServer = L"TCP6-LISTEN:1234,bind=::",
            .Ipv6 = true,
        },
    };

    void NatGuestPortIsReleased()
    {
        constexpr uint16_t port = 1234;
        for (auto const& test : LoopbackBindTests)
        {
            {
                auto guestProcess = BindGuestPort(test.SocatServer, true);
                std::this_thread::sleep_for(std::chrono::seconds(3));
                BindHostPort(port, SOCK_STREAM, IPPROTO_TCP, !test.expectRelay, test.Ipv6, true);
            }

            VerifyNotBoundLoopback(port, test.Ipv6);
        }
    }

    void NatHostPortCantBeBoundByGuest()
    {
        constexpr uint16_t port = 1234;
        for (auto const& test : LoopbackBindTests)
        {
            {
                auto hostPort = BindHostPort(port, SOCK_STREAM, IPPROTO_TCP, true, test.Ipv6, true);
                BindGuestPort(test.SocatServer, !test.expectRelay);
            }

            VerifyNotBoundLoopback(port, test.Ipv6);
        }
    }

    static void NatReusePortOnGuest()
    {
        constexpr uint16_t port = 1234;
        {
            auto [guestLocal, write] = BindGuestPort(L"TCP4-LISTEN:1234,bind=127.0.0.1,reuseport", true);
            BindHostPort(port, SOCK_STREAM, IPPROTO_TCP, false, false, true);
            auto guestWild = BindGuestPort(L"TCP4-LISTEN:1234,bind=0.0.0.0,reuseport", true);
            BindHostPort(port, SOCK_STREAM, IPPROTO_TCP, false, false, true);
            guestLocal.reset();
            BindHostPort(port, SOCK_STREAM, IPPROTO_TCP, false, false, true);
        }

        VerifyNotBoundLoopback(port, false);
    }

    static void ValidateLocalhostRelayTraffic(bool ipv6)
    {
        // Bind a port in the guest.
        auto [guestProcess, read] = BindGuestPort(ipv6 ? L"TCP6-LISTEN:1234,bind=::1" : L"TCP4-LISTEN:1234,bind=127.0.0.1", true);

        // Connect to the port via the localhost relay
        wil::unique_socket hostSocket;
        SOCKADDR_INET addr{};
        addr.si_family = ipv6 ? AF_INET6 : AF_INET;
        INETADDR_SETLOOPBACK((PSOCKADDR)&addr);
        SS_PORT(&addr) = htons(1234);

        auto pred = [&]() {
            hostSocket.reset(socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP));
            THROW_HR_IF(E_ABORT, !hostSocket);
            THROW_HR_IF(E_FAIL, connect(hostSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR);
        };

        try
        {
            wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::seconds(1), std::chrono::minutes(1));
        }
        catch (...)
        {
            LogError("Timed out trying to connect to relay, 0x%x", wil::ResultFromCaughtException());
            VERIFY_FAIL();
        }

        // Send data from host to guest.
        constexpr auto buffer = "test-relay-buffer";
        VERIFY_ARE_EQUAL(send(hostSocket.get(), buffer, static_cast<int>(strlen(buffer)), 0), strlen(buffer));

        {
            // Validate that the guest received the correct data.
            std::string content(strlen(buffer), '\0');

            DWORD totalRead{};
            while (totalRead < content.size())
            {
                DWORD bytesRead{};
                VERIFY_IS_TRUE(ReadFile(read.get(), content.data() + totalRead, static_cast<DWORD>(content.size()) - totalRead, &bytesRead, nullptr));
                LogInfo("Read %lu bytes", bytesRead);

                totalRead += bytesRead;
            }
            VERIFY_ARE_EQUAL(content, buffer);
        }
    }

    TEST_METHOD(NatLocalhostRelay)
    {
        WSL2_TEST_ONLY();
        WslKeepAlive keepAlive;

        ValidateLocalhostRelayTraffic(false);
        ValidateLocalhostRelayTraffic(true);
    }

    TEST_METHOD(NatLocalhostRelayNoIpv6)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.kernelCommandLine = L"ipv6.disable=1"}));
        WslKeepAlive keepAlive;

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"test -f /proc/net/tcp6"), 1L);
        ValidateLocalhostRelayTraffic(false);
    }

    TEST_METHOD(MirroredGuestPortCantBeBoundByHost)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        {
            auto guestProcess = BindGuestPort(L"TCP4-LISTEN:1234", true);
            BindHostPort(1234, SOCK_STREAM, IPPROTO_TCP, false);
        }

        {
            auto guestProcess = BindGuestPort(L"UDP4-LISTEN:1234", true);
            BindHostPort(1234, SOCK_DGRAM, IPPROTO_UDP, false);
        }
    }

    TEST_METHOD(MirroredGuestPortIsReleased)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        // Make sure the VM doesn't time out
        WslKeepAlive keepAlive;

        {
            auto guestProcess = BindGuestPort(L"TCP4-LISTEN:1234", true);
            BindHostPort(1234, SOCK_STREAM, IPPROTO_TCP, false);
        }

        const wil::unique_socket listenSocket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        VERIFY_IS_TRUE(!!listenSocket);

        SOCKADDR_IN Address{};
        Address.sin_family = AF_INET;
        Address.sin_port = htons(1234);

        const auto timeout = std::chrono::steady_clock::now() + std::chrono::minutes(2);

        bool bound = false;
        while (!bound && std::chrono::steady_clock::now() < timeout)
        {
            bound = bind(listenSocket.get(), reinterpret_cast<SOCKADDR*>(&Address), sizeof(Address)) != SOCKET_ERROR;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        VERIFY_IS_TRUE(bound);
    }

    TEST_METHOD(MirroredHostPortCantBeBoundByGuest)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        {
            auto hostPort = BindHostPort(1234, SOCK_STREAM, IPPROTO_TCP, true);
            BindGuestPort(L"TCP4-LISTEN:1234", false);
        }

        {
            auto hostPort = BindHostPort(1234, SOCK_DGRAM, IPPROTO_UDP, true);
            BindGuestPort(L"UDP4-LISTEN:1234", false);
        }
    }

    TEST_METHOD(MirroredUdpBindDoesNotPreventTcpBind)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        auto tcpPort = BindGuestPort(L"TCP4-LISTEN:1234", true);
        auto udpPort = BindGuestPort(L"UDP4-LISTEN:1234", true);
    }

    TEST_METHOD(MirroredHostUdpBindDoesNotPreventGuestTcpBind)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        auto tcpPort = BindHostPort(1234, SOCK_STREAM, IPPROTO_TCP, true);
        auto udpPort = BindGuestPort(L"UDP4-LISTEN:1234", true);
    }

    TEST_METHOD(MirroredMultipleGuestBindOnSameTuple)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        auto bind1 = BindGuestPort(L"TCP4-LISTEN:1234,bind=127.0.0.1", true);
        {
            auto bind2 = BindGuestPort(L"TCP6-LISTEN:1234,bind=::1", true);

            // Allow time for this second bind to be viewed as "in use" by the init port tracker
            // before closing the socket. If the socket is closed before the init port tracker sees
            // that the port allocation was in use, then the init port tracker will hold onto the
            // allocation for a considerable amount of time (through the duration of this test case)
            // before releasing it.
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }

        // Allow time for the init port tracker to detect the second port allocation as no longer in
        // use and perform its cleanup of the second port allocation.
        const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < timeout)
        {
            // {TCP, 1234} should still be reserved for the guest from the first bind.
            auto hostPort = BindHostPort(1234, SOCK_STREAM, IPPROTO_TCP, false);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    TEST_METHOD(MirroredEphemeralBind)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        auto tcpPort = BindGuestPort(L"TCP4-LISTEN:0", true);
        auto udpPort = BindGuestPort(L"UDP4-LISTEN:0", true);
    }

    TEST_METHOD(MirroredExplicitEphemeralBind)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        // Get ephemeral port range
        auto [start, err1] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_local_port_range | cut -f1", 0);
        start.pop_back();
        const auto ephemeralRangeStart = std::stoi(start);

        auto [end, err2] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_local_port_range | cut -f2", 0);
        end.pop_back();
        const auto ephemeralRangeEnd = std::stoi(end);

        // Walk the ephemeral port range and verify we can bind to at least one port (some might be already taken, but the test
        // assumes there should be at least one free).
        bool canBindTcp = false;
        bool canBindUdp = false;

        for (int port = ephemeralRangeStart; port <= ephemeralRangeEnd; port++)
        {
            auto [tcpListener, tcpSuccess, read] = BindGuestPortHelper(L"TCP4-LISTEN:" + std::to_wstring(port));
            if (tcpSuccess)
            {
                canBindTcp = true;
                break;
            }
        }

        for (int port = ephemeralRangeStart; port <= ephemeralRangeEnd; port++)
        {
            auto [udpListener, udpSuccess, read] = BindGuestPortHelper(L"UDP4-LISTEN:" + std::to_wstring(port));
            if (udpSuccess)
            {
                canBindUdp = true;
                break;
            }
        }

        VERIFY_IS_TRUE(canBindTcp);
        VERIFY_IS_TRUE(canBindUdp);
    }

    static void TestNonRootNamespaceEphemeralBind()
    {
        // Get the forwarding state.
        auto [oldIpForwardState, _1] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_forward", 0);
        std::wstring restoreIpForwardCommand = std::format(L"sysctl -w net.ipv4.ip_forward={}", oldIpForwardState.c_str());

        // Ensure the ephemeral port range configured in the non-root networking namespace does not
        // overlap with the ephemeral port range in the root networking namespace (use the 300 ports
        // preceding the root networking namespace ephemeral port range).
        auto [start, _2] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_local_port_range | cut -f1", 0);
        start.pop_back();
        int ephemeralRangeStart = std::stoi(start);

        int ephemeralRangeEnd = ephemeralRangeStart - 1;
        ephemeralRangeStart = ephemeralRangeEnd - 299;
        VERIFY_IS_GREATER_THAN(ephemeralRangeStart, 1024);
        VERIFY_IS_LESS_THAN_OR_EQUAL(ephemeralRangeEnd, UINT16_MAX);
        const std::wstring ephemeralRangeCommand =
            std::format(L"ip netns exec testns sysctl -w net.ipv4.ip_local_port_range=\"{} {}\"", ephemeralRangeStart, ephemeralRangeEnd);

        // Clean up the below configurations.
        auto revertConfig = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&restoreIpForwardCommand] {
            LxsstuLaunchWsl(restoreIpForwardCommand.c_str());
            LxsstuLaunchWsl(L"--system --user root nft flush chain nat POSTROUTING");
            LxsstuLaunchWsl(L"ip link delete veth-test-br");
            LxsstuLaunchWsl(L"ip link delete testbridge");
            LxsstuLaunchWsl(L"ip netns delete testns");
        });

        // Set up a networking namespace and provide it external network access via a bridge, veth
        // pair, SRCNAT iptables rule and forwarding.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip netns add testns"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(ephemeralRangeCommand.c_str()), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link add testbridge type bridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link add veth-test type veth peer name veth-test-br"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test netns testns"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test-br master testbridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns link set veth-test up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test-br up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set testbridge up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns addr add 192.168.15.2/24 dev veth-test"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip addr add 192.168.15.1/24 dev testbridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns route add default via 192.168.15.1 dev veth-test"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add table nat"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft \"add chain nat POSTROUTING { type nat hook postrouting priority srcnat; }\""), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add rule nat POSTROUTING ip saddr 192.168.15.0/24 oif != testbridge masquerade"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"sysctl -w net.ipv4.ip_forward=1"), 0);

        // Verify we have connectivity from the networking namespace when using ephemeral port selection.
        auto [output, warnings] =
            LxsstuLaunchWslAndCaptureOutput(L"ip netns exec testns socat -dd tcp-connect:bing.com:80 create:/tmp/nonexistent", 1);
        LogInfo("output %s", output.c_str());
        LogInfo("warnings %s", warnings.c_str());
        VERIFY_ARE_NOT_EQUAL(warnings.find(L"starting data transfer loop"), std::string::npos);
    }

    TEST_METHOD(NatNonRootNamespaceEphemeralBind)
    {
        WSL2_TEST_ONLY();

        // Because the test creates a new network namespace, the resolv.conf from the root network namespace
        // is copied in the resolv.conf of the new network namespace. The DNS tunneling listener running in the root namespace
        // needs to be accessible from the new namespace, so it can't use a 127* IP.
        WslConfigChange config(LxssGenerateTestConfig({
            .guiApplications = true,
            .dnsTunneling = true,
            .dnsTunnelingIpAddress = L"10.255.255.254",
        }));

        // Configure the root namespace ephemeral port range so we can guarantee a valid,
        // non-overlapping ephemeral port range in the non-root namespace using the very simple port
        // range selection logic in TestNonRootNamespaceEphemeralBind.
        auto [originalRange, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_local_port_range", 0);
        std::wstring restoreEphemeralPortRangeCommand =
            std::format(L"sysctl -w net.ipv4.ip_local_port_range=\"{}\"", originalRange.c_str());
        auto revertEphemeralPortRange = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&restoreEphemeralPortRangeCommand] {
            LxsstuLaunchWsl(restoreEphemeralPortRangeCommand.c_str());
        });

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"sysctl -w net.ipv4.ip_local_port_range=\"60400 60700\""), 0);

        TestNonRootNamespaceEphemeralBind();
    }

    TEST_METHOD(MirroredNonRootNamespaceEphemeralBind)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        // Because the test creates a new network namespace, the resolv.conf from the root network namespace
        // is copied in the resolv.conf of the new network namespace. The DNS tunneling listener running in the root namespace
        // needs to be accessible from the new namespace, so it can't use a 127* IP
        WslConfigChange config(LxssGenerateTestConfig(
            {.guiApplications = true, .networkingMode = wsl::core::NetworkingMode::Mirrored, .dnsTunneling = true, .dnsTunnelingIpAddress = L"10.255.255.254"}));
        WaitForMirroredStateInLinux();

        TestNonRootNamespaceEphemeralBind();
    }

    // Verifies that in mirrored mode, Windows can connect to a listener running in a Linux network namespace different from
    // the Linux root network namespace.
    TEST_METHOD(MirroredPortForwardingToNonRootNamespace)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig(
            {.guiApplications = true, .networkingMode = wsl::core::NetworkingMode::Mirrored, .hostAddressLoopback = true}));
        WaitForMirroredStateInLinux();

        // We list the IPv4 addresses mirrored in Linux and use the first one we find in the test
        std::vector<InterfaceState> interfaceStates = GetAllInterfaceStates();
        std::wstring ipAddress;

        for (auto i = interfaceStates.begin(); i != interfaceStates.end(); ++i)
        {
            for (auto j = i->V4Addresses.begin(); j != i->V4Addresses.end(); ++j)
            {
                // The IP used for DNS tunneling is not intended for guest<->host communication
                if (j->Address != c_dnsTunnelingDefaultIp)
                {
                    ipAddress = j->Address;
                    break;
                }
            }
        }

        // Get the forwarding state.
        auto [oldIpForwardState, _1] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_forward", 0);
        std::wstring restoreIpForwardCommand = std::format(L"sysctl -w net.ipv4.ip_forward={}", oldIpForwardState.c_str());

        // Clean up the below configurations.
        auto revertConfig = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&restoreIpForwardCommand] {
            LxsstuLaunchWsl(restoreIpForwardCommand.c_str());
            LxsstuLaunchWsl(L"--system --user root nft flush chain nat POSTROUTING");
            LxsstuLaunchWsl(L"--system --user root nft flush chain nat PREROUTING");
            LxsstuLaunchWsl(L"ip link delete veth-test-br");
            LxsstuLaunchWsl(L"ip link delete testbridge");
            LxsstuLaunchWsl(L"ip netns delete testns");
        });

        // Set up a networking namespace and provide it external network access via a bridge, veth
        // pair, SRCNAT iptables rule and forwarding.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip netns add testns"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link add testbridge type bridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link add veth-test type veth peer name veth-test-br"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test netns testns"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test-br master testbridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns link set veth-test up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test-br up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set testbridge up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns addr add 192.168.15.2/24 dev veth-test"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip addr add 192.168.15.1/24 dev testbridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns route add default via 192.168.15.1 dev veth-test"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add table nat"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft \"add chain nat POSTROUTING { type nat hook postrouting priority srcnat; }\""), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add rule nat POSTROUTING ip saddr 192.168.15.0/24 oif != testbridge masquerade"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"sysctl -w net.ipv4.ip_forward=1"), 0);

        // Add rule for port forwarding traffic with destination port 8080 to port 80 in the new namespace
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft \"add chain nat PREROUTING { type nat hook prerouting priority dstnat; }\""), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add rule nat PREROUTING tcp dport 8080 dnat to 192.168.15.2:80"), 0);

        // Start listeners in root namespace on port 8080 and new namespace on port 80
        SOCKADDR_INET rootListenerAddr = wsl::windows::common::string::StringToSockAddrInet(L"0.0.0.0");
        SS_PORT(&rootListenerAddr) = htons(8080);
        GuestListener rootListener(rootListenerAddr, IPPROTO_TCP);

        SOCKADDR_INET namespaceListenerAddr = wsl::windows::common::string::StringToSockAddrInet(L"0.0.0.0");
        SS_PORT(&namespaceListenerAddr) = htons(80);
        GuestListener namespaceListener(namespaceListenerAddr, IPPROTO_TCP, L"testns");

        // Verify Windows can connect to port 8080
        SOCKADDR_INET serverAddr = wsl::windows::common::string::StringToSockAddrInet(ipAddress);
        SS_PORT(&serverAddr) = htons(8080);

        wil::unique_socket clientSocket(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        VERIFY_ARE_NOT_EQUAL(clientSocket.get(), INVALID_SOCKET);

        VERIFY_ARE_EQUAL(connect(clientSocket.get(), reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)), 0);
    }

    TEST_METHOD(MirroredLinuxNonRootNamespaceConnectToWindowsHost)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig(
            {.guiApplications = true, .networkingMode = wsl::core::NetworkingMode::Mirrored, .hostAddressLoopback = true}));
        WaitForMirroredStateInLinux();

        // We list the IPv4 addresses mirrored in Linux and use the first one we find in the test
        std::vector<InterfaceState> interfaceStates = GetAllInterfaceStates();
        std::wstring ipAddress;

        for (auto i = interfaceStates.begin(); i != interfaceStates.end(); ++i)
        {
            for (auto j = i->V4Addresses.begin(); j != i->V4Addresses.end(); ++j)
            {
                // The IP used for DNS tunneling is not intended for guest<->host communication
                if (j->Address != c_dnsTunnelingDefaultIp)
                {
                    ipAddress = j->Address;
                    break;
                }
            }
        }

        // Get the forwarding state.
        auto [oldIpForwardState, _1] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv4/ip_forward", 0);
        std::wstring restoreIpForwardCommand = std::format(L"sysctl -w net.ipv4.ip_forward={}", oldIpForwardState.c_str());

        // Clean up the below configurations.
        auto revertConfig = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&restoreIpForwardCommand] {
            LxsstuLaunchWsl(restoreIpForwardCommand.c_str());
            LxsstuLaunchWsl(L"--system --user root nft flush chain nat POSTROUTING");
            LxsstuLaunchWsl(L"ip link delete veth-test-br");
            LxsstuLaunchWsl(L"ip link delete testbridge");
            LxsstuLaunchWsl(L"ip netns delete testns");
        });

        // Set up a networking namespace and provide it external network access via a bridge, veth
        // pair, SRCNAT iptables rule and forwarding.
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip netns add testns"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link add testbridge type bridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link add veth-test type veth peer name veth-test-br"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test netns testns"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test-br master testbridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns link set veth-test up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set veth-test-br up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip link set testbridge up"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns addr add 192.168.15.2/24 dev veth-test"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip addr add 192.168.15.1/24 dev testbridge"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"ip -n testns route add default via 192.168.15.1 dev veth-test"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add table nat"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft \"add chain nat POSTROUTING { type nat hook postrouting priority srcnat; }\""), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--system --user root nft add rule nat POSTROUTING ip saddr 192.168.15.0/24 oif != testbridge masquerade"), 0);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"sysctl -w net.ipv4.ip_forward=1"), 0);

        // Create a listener on the Windows host on port 1234
        SOCKADDR_INET addr = wsl::windows::common::string::StringToSockAddrInet(ipAddress);
        SS_PORT(&addr) = htons(1234);

        const wil::unique_socket listenSocket(socket(addr.si_family, SOCK_STREAM, IPPROTO_TCP));
        VERIFY_ARE_NOT_EQUAL(listenSocket.get(), INVALID_SOCKET);
        VERIFY_ARE_NOT_EQUAL(bind(listenSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)), SOCKET_ERROR);
        VERIFY_ARE_NOT_EQUAL(listen(listenSocket.get(), SOMAXCONN), SOCKET_ERROR);

        // Verify the new network namespace can connect to the Windows host listener
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(
            L"ip netns exec testns socat -dd tcp-connect:" + ipAddress + L":1234 create:/tmp/nonexistent", 1);
        LogInfo("output %s", output.c_str());
        LogInfo("warnings %s", warnings.c_str());
        VERIFY_ARE_NOT_EQUAL(warnings.find(L"starting data transfer loop"), std::string::npos);
    }

    TEST_METHOD(MirroredResolvConf)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /etc/resolv.conf", 0);
        const std::wregex pattern(L"(.|\n)*nameserver [0-9\\. ]+(.|\n)*", std::regex::extended);

        VERIFY_IS_TRUE(std::regex_match(out, pattern));
    }

    TEST_METHOD(MirroredNetworkSettings)
    {
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        struct NetworkSetting
        {
            const std::wstring Path;
            const std::wstring ExpectedValue;
        };

        std::vector<NetworkSetting> settings{
            {L"/proc/sys/net/ipv6/conf/all/accept_ra", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/default/accept_ra", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/all/dad_transmits", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/default/dad_transmits", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/all/autoconf", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/default/autoconf", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/all/addr_gen_mode", L"1\n"},
            {L"/proc/sys/net/ipv6/conf/default/addr_gen_mode", L"1\n"},
            {L"/proc/sys/net/ipv6/conf/all/use_tempaddr", L"0\n"},
            {L"/proc/sys/net/ipv6/conf/default/use_tempaddr", L"0\n"},
            {L"/proc/sys/net/ipv4/conf/all/arp_filter", L"1\n"},
            {L"/proc/sys/net/ipv4/conf/all/rp_filter", L"0\n"},
        };

        settings.push_back({L"/proc/sys/net/ipv4/conf/" + GetGelNicDeviceName() + L"/rp_filter", L"0\n"});

        for (const auto& setting : settings)
        {
            auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat " + setting.Path);
            LogInfo("%ls", (setting.Path + L" : " + out).c_str());
            VERIFY_ARE_EQUAL(setting.ExpectedValue, out);
        }
    }

    enum class FirewallObjects
    {
        Required,
        NotRequired
    };

    static void ValidateInitialFirewallState(FirewallObjects expectHyperVFirewallObjects)
    {
        // Verify that we have an initially working connection.
        // This also ensures that WSL is started to allow for
        // validating the initial Hyper-V port state
        GuestClient(L"tcp-connect:bing.com:80");

        if (expectHyperVFirewallObjects == FirewallObjects::Required)
        {
            // Query for Hyper-V objects. At least one Hyper-V port is expected
            auto [out, err] = LxsstuLaunchPowershellAndCaptureOutput(L"Get-NetFirewallHyperVPort");
            LogInfo("out:[%ls] err:[%ls]", out.c_str(), err.c_str());
            VERIFY_IS_TRUE(!out.empty());
        }
    }

    static auto AddFirewallRule(const FirewallRule& rule)
    {
        try
        {
            std::wstring cmdPrefix;
            if (rule.Type == FirewallType::HyperV)
            {
                cmdPrefix = L"New-NetFirewallHyperVRule -VmCreatorId " + rule.VmCreatorId + L" -RemotePorts " + rule.RemotePorts;
            }
            else
            {
                cmdPrefix = L"New-NetFirewallRule -Protocol TCP -RemotePort " + rule.RemotePorts;
            }

            auto [out, _] = LxsstuLaunchPowershellAndCaptureOutput(
                cmdPrefix + L" -Name " + rule.Name + L" -DisplayName " + rule.Name + L" -Action " + rule.Action +
                L" -Direction Outbound");

            LogInfo("AddRule output:[\n %ls]", out.c_str());

            // output what, if any, Hyper-V Firewall rules were created in response to the above
            auto [query_output, __] = LxsstuLaunchPowershellAndCaptureOutput(L"Get-NetFirewallHyperVRule -Name " + rule.Name);
            LogInfo("Get-NetFirewallHyperVRule output:[\n %ls]", query_output.c_str());
        }
        CATCH_LOG()

        return wil::scope_exit([rule]() {
            try
            {
                LogInfo("Removing the test rule %ls\n", rule.Name.c_str());
                std::wstring cmdPrefix;
                if (rule.Type == FirewallType::HyperV)
                {
                    cmdPrefix = L"Remove-NetFirewallHyperVRule";
                }
                else
                {
                    cmdPrefix = L"Remove-NetFirewallRule";
                }
                LxsstuLaunchPowershellAndCaptureOutput(cmdPrefix + L" -Name " + rule.Name);
            }
            CATCH_LOG()
        });
    }

    enum class FirewallTestConnectivity
    {
        Allowed,
        Blocked
    };

    static auto AddFirewallRuleAndValidateTraffic(const FirewallRule& rule, FirewallTestConnectivity expectedConnectivityAfterRule)
    {
        LogInfo(
            "Validating ruleType=[%ls] name=[%ls] and expectedConnectivity=[%ls]",
            (rule.Type == FirewallType::Host) ? L"Host" : L"HyperV",
            rule.Name.c_str(),
            expectedConnectivityAfterRule == FirewallTestConnectivity::Allowed ? L"Allowed" : L"Blocked");

        // Add rule and verify the connection is allowed/blocked as expected
        auto firewallRuleCleanup = AddFirewallRule(rule);

        GuestClient(L"tcp-connect:bing.com:80,connect-timeout=5", expectedConnectivityAfterRule);
        return firewallRuleCleanup;
    }

    static auto ConfigureFirewallEnabled(FirewallType firewallType, bool settingValue, std::wstring vmCreatorId = L"")
    {
        LogInfo(
            "Configure FirewallEnabled for Type=[%ls] enabled=[%ls]",
            (firewallType == FirewallType::Host) ? L"Host" : L"HyperV",
            settingValue ? L"True" : L"False");
        try
        {
            std::wstring prefix;
            if (firewallType == FirewallType::HyperV)
            {
                prefix = L"Set-NetFirewallHyperVProfile -VmCreatorId " + vmCreatorId;
            }
            else
            {
                prefix = L"Set-NetFirewallProfile";
            }
            LxsstuLaunchPowershellAndCaptureOutput(prefix + L" -Profile Public -Enabled " + (settingValue ? L"True" : L"False"));
            LxsstuLaunchPowershellAndCaptureOutput(prefix + L" -Profile Private -Enabled " + (settingValue ? L"True" : L"False"));
            LxsstuLaunchPowershellAndCaptureOutput(prefix + L" -Profile Domain -Enabled " + (settingValue ? L"True" : L"False"));
        }
        CATCH_LOG()

        return wil::scope_exit([vmCreatorId, firewallType]() {
            try
            {
                std::wstring prefix;
                if (firewallType == FirewallType::HyperV)
                {
                    prefix = L"Set-NetFirewallHyperVProfile -VmCreatorId " + vmCreatorId;
                }
                else
                {
                    prefix = L"Set-NetFirewallProfile";
                }

                LxsstuLaunchPowershellAndCaptureOutput(prefix + L" -Profile Public -Enabled NotConfigured");
                LxsstuLaunchPowershellAndCaptureOutput(prefix + L" -Profile Private -Enabled NotConfigured");
                LxsstuLaunchPowershellAndCaptureOutput(prefix + L" -Profile Domain -Enabled NotConfigured");
            }
            CATCH_LOG()
        });
    }

    static auto ConfigureHyperVFirewallLoopbackEnabled(bool settingValue, std::wstring vmCreatorId)
    {
        LogInfo("Configuring LoopbackEnabled=[%d]", settingValue);
        try
        {
            LxsstuLaunchPowershellAndCaptureOutput(
                L"Set-NetFirewallHyperVVMSetting -VmCreatorId " + vmCreatorId + L" -LoopbackEnabled " + (settingValue ? L"True" : L"False"));
        }
        CATCH_LOG()

        return wil::scope_exit([vmCreatorId]() {
            try
            {
                LxsstuLaunchPowershellAndCaptureOutput(
                    L"Set-NetFirewallHyperVVMSetting -VmCreatorId " + vmCreatorId + L" -LoopbackEnabled NotConfigured");
            }
            CATCH_LOG()
        });
    }

    static void FirewallRuleBlockedTests(FirewallTestConnectivity expectedConnectivity)
    {
        // Adding a block rule should result in traffic being blocked
        FirewallRule blockRule = {FirewallType::Host, L"WSLTestBlockRule", c_firewallTrafficTestPort, c_firewallRuleActionBlock};
        AddFirewallRuleAndValidateTraffic(blockRule, expectedConnectivity);

        // Adding both an allow and block rule should result in traffic being blocked
        FirewallRule allowRule = {FirewallType::Host, L"WSLTestAllowRule", c_firewallTrafficTestPort, c_firewallRuleActionAllow};
        auto allowRuleCleanup = AddFirewallRuleAndValidateTraffic(allowRule, FirewallTestConnectivity::Allowed);
        AddFirewallRuleAndValidateTraffic(blockRule, expectedConnectivity);
        allowRuleCleanup.reset();

        // Adding a block rule should result in traffic being blocked
        FirewallRule hyperVBlockRule = {
            FirewallType::HyperV, L"WSLTestBlockRuleHyperV", c_firewallTrafficTestPort, c_firewallRuleActionBlock, c_wslVmCreatorId};
        AddFirewallRuleAndValidateTraffic(hyperVBlockRule, expectedConnectivity);

        // Adding both an allow and block rule should result in traffic being blocked
        FirewallRule hyperVAllowRule = {
            FirewallType::HyperV, L"WSLTestAllowRuleHyperV", c_firewallTrafficTestPort, c_firewallRuleActionAllow, c_wslVmCreatorId};
        auto hyperVAllowRuleCleanup = AddFirewallRuleAndValidateTraffic(hyperVAllowRule, FirewallTestConnectivity::Allowed);
        AddFirewallRuleAndValidateTraffic(hyperVBlockRule, expectedConnectivity);
        hyperVAllowRuleCleanup.reset();

        // Adding a rule with vm creator 'any' should result in traffic being blocked
        FirewallRule anyHyperVBlockRule = {
            FirewallType::HyperV, L"WSLTestBlockRuleHyperVAny", c_firewallTrafficTestPort, c_firewallRuleActionBlock, c_wslVmCreatorId};
        AddFirewallRuleAndValidateTraffic(hyperVBlockRule, expectedConnectivity);
    }

    TEST_METHOD(NatFirewallRulesExpectedBlock)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.firewall = true}));

        ValidateInitialFirewallState(FirewallObjects::Required);
        FirewallRuleBlockedTests(FirewallTestConnectivity::Blocked);
    }

    TEST_METHOD(NatFirewallRulesExpectedBlockFirewallDisabled)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.firewall = false}));

        ValidateInitialFirewallState(FirewallObjects::NotRequired);
        FirewallRuleBlockedTests(FirewallTestConnectivity::Allowed);
    }

    TEST_METHOD(NatFirewallRulesExpectedBlockFirewallDisabledByPolicy)
    {
        HYPERV_FIREWALL_TEST_ONLY();

        RegistryKeyChange<DWORD> change(
            HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey, wsl::windows::policies::c_allowCustomFirewallUserSetting, 0);

        // the user tries to disable Hyper-V FW in the config file, but the admin disabled user control
        WslConfigChange config(LxssGenerateTestConfig({.firewall = false}));

        ValidateInitialFirewallState(FirewallObjects::NotRequired);
        FirewallRuleBlockedTests(FirewallTestConnectivity::Blocked);
    }

    TEST_METHOD(MirroredFirewallRulesExpectedBlock)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        ValidateInitialFirewallState(FirewallObjects::Required);
        FirewallRuleBlockedTests(FirewallTestConnectivity::Blocked);
    }

    static void FirewallRuleAllowedTests(FirewallTestConnectivity expectedConnectivity)
    {
        // A host rule with different IP address should not affect traffic
        FirewallRule differentIPRule = {FirewallType::Host, L"WSLTestDifferentIPRule", c_firewallTestOtherPort, c_firewallRuleActionBlock};
        AddFirewallRuleAndValidateTraffic(differentIPRule, expectedConnectivity);

        // A host rule with action allow should not affect traffic
        FirewallRule allowRule = {FirewallType::Host, L"WSLTestAllowRule", c_firewallTrafficTestPort, c_firewallRuleActionAllow};
        AddFirewallRuleAndValidateTraffic(allowRule, expectedConnectivity);

        // A hyperv- rule with a different VM creator ID should not affect this traffic
        FirewallRule differentVmCreatorRule = {
            FirewallType::HyperV, L"WSLTestDifferentVMCreatorIdRule", c_firewallTrafficTestPort, c_firewallRuleActionBlock, c_wsaVmCreatorId};
        AddFirewallRuleAndValidateTraffic(differentVmCreatorRule, expectedConnectivity);

        // A hyper-v rule with a different IP address should not affect this traffic
        FirewallRule differentIPHyperVRule = {
            FirewallType::HyperV, L"WSLTestDifferentIPRuleHyperV", c_firewallTestOtherPort, c_firewallRuleActionBlock, c_wslVmCreatorId};
        AddFirewallRuleAndValidateTraffic(differentIPHyperVRule, expectedConnectivity);

        // A hyper-v rule with action allow should not affect traffic
        FirewallRule allowHyperVRule = {
            FirewallType::HyperV, L"WSLTestAllowRuleHyperV", c_firewallTrafficTestPort, c_firewallRuleActionAllow, c_wslVmCreatorId};
        AddFirewallRuleAndValidateTraffic(allowHyperVRule, expectedConnectivity);
    }

    TEST_METHOD(NatFirewallRulesExpectedAllow)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.firewall = true}));

        ValidateInitialFirewallState(FirewallObjects::Required);
        FirewallRuleAllowedTests(FirewallTestConnectivity::Allowed);
    }

    TEST_METHOD(NatFirewallRulesExpectedAllowFirewallDisabled)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.firewall = false}));

        ValidateInitialFirewallState(FirewallObjects::NotRequired);
        FirewallRuleAllowedTests(FirewallTestConnectivity::Allowed);
    }

    TEST_METHOD(MirroredFirewallRulesExpectedAllow)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        ValidateInitialFirewallState(FirewallObjects::Required);
        FirewallRuleAllowedTests(FirewallTestConnectivity::Allowed);
    }

    static void FirewallSettingEnabledTests(bool isHyperVFirewallEnabled)
    {
        // Configure Firewall disabled
        auto hostDisabledCleanup = ConfigureFirewallEnabled(FirewallType::Host, false);

        // Add host block rule, which is expected to be enforced
        FirewallRule blockRule = {FirewallType::Host, L"WSLTestBlockRule", c_firewallTrafficTestPort, c_firewallRuleActionBlock, c_wslVmCreatorId};
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        blockRule.Type = FirewallType::HyperV;
        // Add hyper-v block rule, which is expected to be enforced
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        hostDisabledCleanup.reset();

        // Configure Hyper-V firewall disabled
        auto hyperVDisabledCleanup = ConfigureFirewallEnabled(FirewallType::HyperV, false, c_wslVmCreatorId);
        // Add host block rule, which is expected to be enforced
        blockRule.Type = FirewallType::Host;
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        // Add hyper-v block rule, which is expected to be enforced
        blockRule.Type = FirewallType::HyperV;
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        hyperVDisabledCleanup.reset();

        // host rules are propagated only if Hyper-V Firewall is enabled
        // Configure conflicting policy for host and hyper-v (hyper-v policy takes precedence)
        auto conflictingHostEnabledCleanup = ConfigureFirewallEnabled(FirewallType::Host, true);
        // Add host block rule, which is expected to be enforced
        blockRule.Type = FirewallType::Host;
        AddFirewallRuleAndValidateTraffic(
            blockRule, isHyperVFirewallEnabled ? FirewallTestConnectivity::Blocked : FirewallTestConnectivity::Allowed);
        // Add hyper-v block rule, which is expected to be enforced
        blockRule.Type = FirewallType::HyperV;
        AddFirewallRuleAndValidateTraffic(
            blockRule, isHyperVFirewallEnabled ? FirewallTestConnectivity::Blocked : FirewallTestConnectivity::Allowed);

        // Configure hyper-v disabled
        auto conflictingHyperVDisabledCleanup = ConfigureFirewallEnabled(FirewallType::HyperV, false, c_wslVmCreatorId);
        // Add host block rule, which is expected to be NOT enforced (firewall is disabled)
        blockRule.Type = FirewallType::Host;
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        // Add hyper-v block rule, which is expected to be NOT enforced (firewall is disabled)
        blockRule.Type = FirewallType::HyperV;
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        conflictingHostEnabledCleanup.reset();
        conflictingHyperVDisabledCleanup.reset();

        // Configure conflicting policy for host and hyper-v (hyper-v policy takes precedence)
        auto conflictingHyperVEnabledCleanup = ConfigureFirewallEnabled(FirewallType::HyperV, true, c_wslVmCreatorId);
        // Add host block rule, which is expected to be enforced
        blockRule.Type = FirewallType::Host;
        AddFirewallRuleAndValidateTraffic(
            blockRule, isHyperVFirewallEnabled ? FirewallTestConnectivity::Blocked : FirewallTestConnectivity::Allowed);
        // Add hyper-v block rule, which is expected to be enforced
        blockRule.Type = FirewallType::HyperV;
        AddFirewallRuleAndValidateTraffic(
            blockRule, isHyperVFirewallEnabled ? FirewallTestConnectivity::Blocked : FirewallTestConnectivity::Allowed);
        // Configure host firewall disabled. Hyper-V firewall is still expected to be enforced, but host firewall rules will not be
        auto conflictingHostDisabledCleanup = ConfigureFirewallEnabled(FirewallType::Host, false);
        // Add host block rule, which is NOT expected to be enforced (host firewall disabled)
        blockRule.Type = FirewallType::Host;
        AddFirewallRuleAndValidateTraffic(blockRule, FirewallTestConnectivity::Allowed);
        // Add hyper-v block rule, which is expected to be enforced (hyper-v firewall still enabled)
        blockRule.Type = FirewallType::HyperV;
        AddFirewallRuleAndValidateTraffic(
            blockRule, isHyperVFirewallEnabled ? FirewallTestConnectivity::Blocked : FirewallTestConnectivity::Allowed);
    }

    TEST_METHOD(NatFirewallRulesEnabledSetting)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        WslConfigChange config(LxssGenerateTestConfig({.firewall = true}));

        ValidateInitialFirewallState(FirewallObjects::Required);
        FirewallSettingEnabledTests(true);
    }

    TEST_METHOD(NatFirewallRulesEnabledSettingFirewallDisabled)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        SKIP_TEST_UNSTABLE();
        WslConfigChange config(LxssGenerateTestConfig({.firewall = false}));

        ValidateInitialFirewallState(FirewallObjects::NotRequired);
        FirewallSettingEnabledTests(false);
    }

    TEST_METHOD(MirroredFirewallRulesEnabledSetting)
    {
        HYPERV_FIREWALL_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        ValidateInitialFirewallState(FirewallObjects::Required);
        FirewallSettingEnabledTests(true);
    }

    /* Network Tests Helper Methods */

    static void RunGns(const std::string& input, const std::optional<GUID>& adapter = {}, const std::optional<LX_MESSAGE_TYPE>& messageType = {}, int expectedErrorCode = 0)
    {
        constexpr auto InheritOnReadHandle = true;
        constexpr auto DoNotEnableInheritOnWriteHandle = false;
        SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        auto [read, write] =
            CreateSubprocessPipe(InheritOnReadHandle, DoNotEnableInheritOnWriteHandle, static_cast<DWORD>(input.size()), &attributes);

        THROW_IF_WIN32_BOOL_FALSE(WriteFile(write.get(), input.data(), static_cast<DWORD>(input.size()), nullptr, nullptr));
        write.reset();

        LogInfo("GNS Input: '%S'", input.c_str());
        const auto adapterArg =
            adapter.has_value() ? L"--adapter " + wsl::shared::string::GuidToString<wchar_t>(adapter.value()) + std::wstring(L" ") : L"";
        const auto messageTypeArg =
            messageType.has_value() ? L"--msg_type " + std::to_wstring(static_cast<int>(messageType.value())) + std::wstring(L" ") : L"";
        LxsstuLaunchWslAndCaptureOutput(L"/gns " + adapterArg + messageTypeArg, expectedErrorCode, read.get());
    }

    template <typename T>
    void RunGns(T& input, ModifyRequestType action, GuestEndpointResourceType type)
    {
        ModifyGuestEndpointSettingRequest<T> request;
        request.RequestType = action;
        request.ResourceType = type;
        request.Settings = input;

        RunGns(wsl::shared::ToJson(request), AdapterId, LxGnsMessageNotification);
    }

    template <typename T>
    void RunGns(T& input, const LX_MESSAGE_TYPE messageType)
    {
        RunGns(wsl::shared::ToJson(input), AdapterId, messageType);
    }

    template <typename T>
    void SendDeviceSettingsRequest(std::wstring targetDevice, T& input, ModifyRequestType action, GuestEndpointResourceType type)
    {
        wsl::shared::hns::ModifyGuestEndpointSettingRequest<T> request;
        request.targetDeviceName = targetDevice;
        request.RequestType = action;
        request.ResourceType = type;
        request.Settings = input;

        RunGns(request, LxGnsMessageDeviceSettingRequest);
    }

    static RoutingTableState GetRoutingTableState(std::wstring& out, std::wregex& defaultRoutePattern, std::wregex& routePattern)
    {
        RoutingTableState state;
        std::wsmatch match;

        std::wistringstream input(out);
        std::wstring line;
        while (std::getline(input, line) && !line.empty())
        {
            if (std::regex_search(line, match, defaultRoutePattern) && match.size() >= 3)
            {
                VERIFY_IS_FALSE(state.DefaultRoute.has_value());

                state.DefaultRoute = {{match.str(1), match.str(2), {}, match.size() > 4 && match[4].matched ? std::stoi(match.str(4)) : 0}};
            }
            else if (std::regex_search(line, match, routePattern) && match.size() >= 4)
            {
                state.Routes.emplace_back(Route{
                    match.str(2), match.str(3), {match.str(1)}, match.size() > 5 && match[5].matched ? std::stoi(match.str(5)) : 0});
            }
        }

        return state;
    }

    static RoutingTableState GetIpv4RoutingTableState()
    {
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"ip route show");
        LogInfo("Ip route output: '%ls'", out.c_str());

        std::wregex defaultRoutePattern(L"default via ([0-9,.]+) dev ([a-zA-Z0-9]*) *(metric ([0-9]+))?");
        std::wregex routePattern(L"([0-9,.,/]+) via ([0-9,.]+) dev ([a-zA-Z0-9]*) *(metric ([0-9]+))?");

        return GetRoutingTableState(out, defaultRoutePattern, routePattern);
    }

    static RoutingTableState GetIpv6RoutingTableState()
    {
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"ip -6 route show");
        LogInfo("Ip -6 route output: '%ls'", out.c_str());

        RoutingTableState state;
        std::wregex defaultRoutePattern(L"default via ([a-f,A-F,0-9,:]+) dev ([a-zA-Z0-9]*) *(metric ([0-9]+))?");
        std::wregex routePattern(L"([a-f,A-F,0-9,:,/]+) via ([a-f,A-F,0-9,:]+) dev ([a-zA-Z0-9]*) *(metric ([0-9]+))?");

        return GetRoutingTableState(out, defaultRoutePattern, routePattern);
    }

    static InterfaceState GetInterfaceState(const std::wstring& name, const std::wstring& expectedWarnings = L"")
    {
        // Sample output from "ip addr show":
        // 4: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP group default qlen 1000
        // link/ether 00:12:34:56:78:9A brd ff:ff:ff:ff:ff:ff
        // inet 172.17.123.249/20 brd 172.17.127.255 scope global eth0
        // valid_lft forever preferred_lft forever
        // inet6 2001::1:2:3:4/64 scope global
        // valid_lft forever preferred_lft 0sec
        auto [out, warnings] = LxsstuLaunchWslAndCaptureOutput(L"ip addr show " + name);
        LogInfo("ip addr show output: '%ls'", out.c_str());

        if (expectedWarnings.empty())
        {
            VERIFY_IS_TRUE(warnings.empty());
        }
        else
        {
            if (!PathMatchSpec(warnings.c_str(), expectedWarnings.c_str()))
            {
                LogError("Warning '%ls' didn't match pattern '%ls'", warnings.c_str(), expectedWarnings.c_str());
                VERIFY_FAIL();
            }
        }

        std::wistringstream input(out);

        std::wstring line;

        InterfaceState state = {name};

        // Drop first two lines
        VERIFY_IS_TRUE(std::getline(input, line).good());
        VERIFY_IS_TRUE(std::getline(input, line).good());

        // Read the address lines
        while (std::getline(input, line).good())
        {
            std::wregex v4Pattern(L"inet ([0-9,.]+)\\/([0-9]+) brd ([0-9,.]+) scope global .*" + name);
            std::wregex v6Pattern(L"inet6 ([a-f,A-F,0-9,:]+)\\/([0-9]+) scope global");
            std::wregex v4LocalPattern(L"inet 169.254.([0-9,.]+)\\/([0-9]+) brd 169.254.255.255 scope link");
            std::wregex v6LocalPattern(L"inet6 ([a-f,A-F,0-9,:]+)\\/([0-9]+) scope link");
            std::wregex v4LoopbackPattern(L"inet 127.0.0.1/8 scope host");
            std::wregex v6LoopbackPattern(L"inet6 ::1/128 scope host");
            std::wregex deprecatedPattern(L"deprecated");

            std::wsmatch match, preferredStateMatch;
            if (std::regex_search(line, match, v4Pattern) && match.size() == 4)
            {
                bool preferred = !std::regex_search(line, preferredStateMatch, deprecatedPattern);
                state.V4Addresses.emplace_back(IpAddress{match.str(1), (uint8_t)std::stoul(match.str(2)), preferred});
            }
            else if (std::regex_search(line, match, v6Pattern) && match.size() == 3)
            {
                bool preferred = !std::regex_search(line, preferredStateMatch, deprecatedPattern);
                state.V6Addresses.emplace_back(IpAddress{match.str(1), (uint8_t)std::stoul(match.str(2)), preferred});
            }
            else if (std::regex_search(line, match, v4LocalPattern) && match.size() == 3)
            {
                LogInfo("Skipping ipv4 link local address");
            }
            else if (std::regex_search(line, match, v6LocalPattern) && match.size() == 3)
            {
                LogInfo("Skipping ipv6 link local address");
            }
            else if (std::regex_search(line, match, v4LoopbackPattern) && match.size() == 1)
            {
                LogInfo("Skipping ipv4 loopback");
            }
            else if (std::regex_search(line, match, v6LoopbackPattern) && match.size() == 1)
            {
                LogInfo("Skipping ipv6 loopback");
            }
            else
            {
                LogInfo("Ip addr output: '%ls'", out.c_str());
                LogInfo("Current line: \"%ls\"", line.c_str());
                VERIFY_FAIL(L"Failed to extract interface state");
            }

            // Skip the lifetimes line
            VERIFY_IS_TRUE(std::getline(input, line).good());
        }

        out = LxsstuLaunchWslAndCaptureOutput(L"cat /sys/class/net/" + name + L"/operstate").first;
        state.Up = false;
        if (out == L"up\n")
        {
            state.Up = true;
        }
        else if ((out != L"down\n") && ((name.substr(0, 4).compare(L"wlan") != 0) && (name != L"lo")))
        {
            LogInfo("Unexpected operstate: '%s'", out.c_str());
            VERIFY_FAIL();
        }

        out = LxsstuLaunchWslAndCaptureOutput(L"cat /sys/class/net/" + name + L"/mtu").first;
        state.Mtu = std::stoi(out);

        auto routingTableState = GetIpv4RoutingTableState();
        if (routingTableState.DefaultRoute.has_value())
        {
            state.Gateway = routingTableState.DefaultRoute->Via;
        }

        auto v6RoutingTableState = GetIpv6RoutingTableState();
        if (v6RoutingTableState.DefaultRoute.has_value())
        {
            state.V6Gateway = v6RoutingTableState.DefaultRoute->Via;
        }

        return state;
    }

    static std::vector<InterfaceState> GetAllInterfaceStates()
    {
        // Result output is a list of interface names with newline as the delimiter
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"ip -brief link show | awk -F '[@ ]' '{print $1}'");
        LogInfo("parsed ip link output:'%ls'", out.c_str());

        std::wistringstream input(out);

        std::vector<InterfaceState> interfaceStates;
        std::wstring line;

        while (std::getline(input, line).good())
        {
            interfaceStates.push_back(GetInterfaceState(line));
        }

        return interfaceStates;
    }

    void TestCase(const std::vector<InterfaceState>& interfaceStates)
    {
        WSL2_TEST_ONLY();

        for (const auto& state : interfaceStates)
        {
            if (state.Rename)
            {
                wsl::shared::hns::HNSEndpoint endpoint;
                endpoint.ID = AdapterId;
                endpoint.PortFriendlyName = state.Name;
                RunGns(wsl::shared::ToJson(endpoint));
            }

            // Remove existing addresses not in goal state
            auto currentInterfaceState = GetInterfaceState(state.Name);
            for (auto it = currentInterfaceState.V4Addresses.begin(); it != currentInterfaceState.V4Addresses.end(); ++it)
            {
                if (std::find(state.V4Addresses.begin(), state.V4Addresses.end(), *it) == state.V4Addresses.end())
                {
                    wsl::shared::hns::IPAddress address;
                    address.Address = it->Address;
                    address.OnLinkPrefixLength = it->PrefixLength;
                    address.Family = AF_INET;
                    SendDeviceSettingsRequest(state.Name, address, ModifyRequestType::Remove, GuestEndpointResourceType::IPAddress);
                }
            }

            for (auto it = currentInterfaceState.V6Addresses.begin(); it != currentInterfaceState.V6Addresses.end(); ++it)
            {
                if (std::find(state.V4Addresses.begin(), state.V4Addresses.end(), *it) == state.V4Addresses.end())
                {
                    wsl::shared::hns::IPAddress address;
                    address.Address = it->Address;
                    address.OnLinkPrefixLength = it->PrefixLength;
                    address.Family = AF_INET6;
                    SendDeviceSettingsRequest(state.Name, address, ModifyRequestType::Remove, GuestEndpointResourceType::IPAddress);
                }
            }

            // Add or update addresses
            for (auto it = state.V4Addresses.begin(); it != state.V4Addresses.end(); ++it)
            {
                wsl::shared::hns::IPAddress address;
                address.Address = it->Address;
                address.OnLinkPrefixLength = it->PrefixLength;
                address.Family = AF_INET;
                address.PreferredLifetime = 0xFFFFFFFF;
                bool updateAddress =
                    (std::find(currentInterfaceState.V4Addresses.begin(), currentInterfaceState.V4Addresses.end(), *it) !=
                     currentInterfaceState.V4Addresses.end());
                SendDeviceSettingsRequest(
                    state.Name, address, updateAddress ? ModifyRequestType::Update : ModifyRequestType::Add, GuestEndpointResourceType::IPAddress);

                Route prefixRoute{LX_INIT_UNSPECIFIED_ADDRESS, L"eth0", it->GetPrefix()};
                if (!RouteExists(prefixRoute))
                {
                    // Add the prefix route for the newly added/updated address
                    wsl::shared::hns::Route route;
                    route.NextHop = prefixRoute.Via;
                    route.DestinationPrefix = prefixRoute.Prefix.value();
                    route.Family = AF_INET;
                    SendDeviceSettingsRequest(state.Name, route, ModifyRequestType::Add, GuestEndpointResourceType::Route);
                }
            }

            for (auto it = state.V6Addresses.begin(); it != state.V6Addresses.end(); ++it)
            {
                wsl::shared::hns::IPAddress address;
                address.Address = it->Address;
                address.OnLinkPrefixLength = it->PrefixLength;
                address.Family = AF_INET6;
                address.PreferredLifetime = 0xFFFFFFFF;
                bool updateAddress =
                    (std::find(currentInterfaceState.V6Addresses.begin(), currentInterfaceState.V6Addresses.end(), *it) !=
                     currentInterfaceState.V6Addresses.end());
                SendDeviceSettingsRequest(
                    state.Name, address, updateAddress ? ModifyRequestType::Update : ModifyRequestType::Add, GuestEndpointResourceType::IPAddress);

                Route prefixRoute{LX_INIT_UNSPECIFIED_V6_ADDRESS, L"eth0", it->GetPrefix()};
                if (!RouteExists(prefixRoute))
                {
                    // Add the prefix route for the newly added/updated address
                    wsl::shared::hns::Route route;
                    route.NextHop = prefixRoute.Via;
                    route.DestinationPrefix = prefixRoute.Prefix.value();
                    route.Family = AF_INET6;
                    SendDeviceSettingsRequest(state.Name, route, ModifyRequestType::Add, GuestEndpointResourceType::Route);
                }
            }

            if (state.Gateway.has_value())
            {
                wsl::shared::hns::Route route;
                route.NextHop = state.Gateway.value();
                route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_PREFIX;
                route.Family = AF_INET;
                bool updateGw = currentInterfaceState.Gateway.has_value();
                SendDeviceSettingsRequest(
                    state.Name, route, updateGw ? ModifyRequestType::Update : ModifyRequestType::Add, GuestEndpointResourceType::Route);
            }

            if (state.V6Gateway.has_value())
            {
                wsl::shared::hns::Route route;
                route.NextHop = state.V6Gateway.value();
                route.DestinationPrefix = LX_INIT_DEFAULT_ROUTE_V6_PREFIX;
                route.Family = AF_INET6;
                bool updateGw = currentInterfaceState.V6Gateway.has_value();
                SendDeviceSettingsRequest(
                    state.Name, route, updateGw ? ModifyRequestType::Update : ModifyRequestType::Add, GuestEndpointResourceType::Route);
            }
        }

        // Validate that the addresses and routes are in the final goal state
        const auto& expectedInterfaceState = interfaceStates.back();

        auto interfaceState = GetInterfaceState(expectedInterfaceState.Name);
        for (auto it = expectedInterfaceState.V4Addresses.begin(); it != expectedInterfaceState.V4Addresses.end(); ++it)
        {
            VERIFY_IS_TRUE(
                std::find(interfaceState.V4Addresses.begin(), interfaceState.V4Addresses.end(), *it) != interfaceState.V4Addresses.end());
        }

        if (expectedInterfaceState.Gateway.has_value())
        {
            VERIFY_ARE_EQUAL(expectedInterfaceState.Gateway, interfaceState.Gateway);
        }

        for (auto it = expectedInterfaceState.V6Addresses.begin(); it != expectedInterfaceState.V6Addresses.end(); ++it)
        {
            VERIFY_IS_TRUE(
                std::find(interfaceState.V6Addresses.begin(), interfaceState.V6Addresses.end(), *it) != interfaceState.V6Addresses.end());
        }

        if (expectedInterfaceState.V6Gateway.has_value())
        {
            VERIFY_ARE_EQUAL(expectedInterfaceState.V6Gateway, interfaceState.V6Gateway);
        }
    }

    static bool RouteExists(const Route& route)
    {
        auto v4State = GetIpv4RoutingTableState();
        if (std::find(v4State.Routes.begin(), v4State.Routes.end(), route) != v4State.Routes.end())
        {
            return true;
        }

        auto v6State = GetIpv6RoutingTableState();
        return std::find(v6State.Routes.begin(), v6State.Routes.end(), route) != v6State.Routes.end();
    }

    // Reads from the file until the substring is found, a timeout is reached, or ReadFile returns an error
    // Returns true on success, false otherwise
    static bool FindSubstring(wil::unique_handle& file, const std::string& substr, std::string& output)
    {
        char buffer[256];
        DWORD bytesRead;
        const HANDLE readFileThread = OpenThread(THREAD_ALL_ACCESS, false, GetCurrentThreadId());
        const wil::unique_handle event(CreateEvent(nullptr, FALSE, FALSE, nullptr));
        VERIFY_ARE_NOT_EQUAL(event.get(), INVALID_HANDLE_VALUE);

        // ReadFile will block, so cancel the syscall if it is taking too long
        const auto watchdogThread = std::async(std::launch::async, [&] {
            if (WaitForSingleObject(event.get(), 30000) == WAIT_TIMEOUT)
            {
                LogInfo("Canceling synchronous IO", GetTickCount());
                CancelSynchronousIo(readFileThread);
            }
        });

        do
        {
            if (!ReadFile(file.get(), buffer, sizeof(buffer) - 1, &bytesRead, nullptr))
            {
                LogInfo("ReadFile failed with %d", GetLastError());
                break;
            }

            buffer[bytesRead] = '\0';
            output += std::string(buffer);

            if (output.find(substr) != std::string::npos)
            {
                break;
            }
        } while (true);

        SetEvent(event.get());
        watchdogThread.wait();

        LogInfo("output=\n %S", output.c_str());
        return (output.find(substr) != std::string::npos);
    }

    static std::wstring CreateSocatString(const SOCKADDR_INET& si, int protocol, bool listen)
    {
        return std::wstring(((protocol == IPPROTO_TCP) ? L"TCP" : L"UDP")) + std::wstring(((si.si_family == AF_INET) ? L"4" : L"6")) +
               std::wstring(L"-") + std::wstring((listen) ? L"LISTEN:" : ((IPPROTO_TCP) ? L"CONNECT:" : L"SENDTO:")) +
               std::wstring(
                   (listen) ? std::to_wstring(ntohs(SS_PORT(&si))) + std::wstring(L",bind=") +
                                  wsl::windows::common::string::SockAddrInetToWstring(si)
                            : wsl::windows::common::string::SockAddrInetToWstring(si) + std::wstring(L":") +
                                  std::to_wstring(ntohs(SS_PORT(&si))));
    }

    struct GuestListener
    {
        GuestListener(const SOCKADDR_INET& addr, int protocol)
        {
            THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &writePipe, nullptr, 0));
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(writePipe.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            const auto wslCmd = L"socat -dd " + CreateSocatString(addr, protocol, true) + L" STDOUT";
            auto cmd = LxssGenerateWslCommandLine(wslCmd.data());

            process = unique_kill_process(LxsstuStartProcess(cmd.data(), nullptr, nullptr, writePipe.get()));
            writePipe.reset();

            std::string output;
            THROW_HR_IF(E_FAIL, !NetworkTests::FindSubstring(readPipe, "listening on", output));
        }

        // Start a listener in a different network namespace
        GuestListener(const SOCKADDR_INET& addr, int protocol, const std::wstring& namespaceName)
        {
            THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &writePipe, nullptr, 0));
            THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(writePipe.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

            const auto wslCmd =
                L"ip netns exec " + namespaceName + L" socat -dd " + CreateSocatString(addr, protocol, true) + L" STDOUT";
            auto cmd = LxssGenerateWslCommandLine(wslCmd.data());

            process = unique_kill_process(LxsstuStartProcess(cmd.data(), nullptr, nullptr, writePipe.get()));
            writePipe.reset();

            std::string output;
            THROW_HR_IF(E_FAIL, !NetworkTests::FindSubstring(readPipe, "listening on", output));
        }

        void AcceptConnection()
        {
            std::string output;
            VERIFY_IS_TRUE(NetworkTests::FindSubstring(readPipe, "starting data transfer loop", output));
        }

        wil::unique_handle dmesgFile;
        unique_kill_process dmesg;
        unique_kill_process process;
        wil::unique_handle readPipe;
        wil::unique_handle writePipe;
    };

    struct GuestClient
    {
        GuestClient(const SOCKADDR_INET& addr, int protocol) : GuestClient(CreateSocatString(addr, protocol, false))
        {
        }

        GuestClient(const std::wstring& socatString, FirewallTestConnectivity expectedSuccess = FirewallTestConnectivity::Allowed)
        {
            const auto expectSuccess = expectedSuccess == FirewallTestConnectivity::Allowed;
            const auto wslCmd = L"echo A | socat -dd " + socatString + L" STDIN";
            auto cmd = LxssGenerateWslCommandLine(wslCmd.data());
            const auto* connectionString = expectSuccess ? "starting data transfer loop" : "Connection timed out";
            bool valueFound = false;
            for (int i = 0; i < 3; ++i)
            {
                wil::unique_handle readPipe;
                wil::unique_handle writePipe;
                THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &writePipe, nullptr, 0));
                THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(writePipe.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));

                unique_kill_process process = unique_kill_process(LxsstuStartProcess(cmd.data(), nullptr, nullptr, writePipe.get()));
                writePipe.reset();

                std::string output;
                valueFound = FindSubstring(readPipe, connectionString, output);

                if (expectSuccess && !valueFound && (output.find("Temporary failure") != std::string::npos))
                {
                    LogWarning("Temporary failure - retrying up to 3 times");
                    continue;
                }

                break;
            }

            VERIFY_IS_TRUE(valueFound, (expectSuccess) ? "Verifying connection succeeded" : "Verifying connection failed");
        }
    };

    static std::wstring GetGelNicDeviceName()
    {
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"ip route get from 127.0.0.1 127.0.0.1 | awk 'FNR <= 1 {print $7}'");
        out.pop_back();
        return out;
    }

    static bool HostHasInternetConnectivity(ADDRESS_FAMILY family)
    {
        using ABI::Windows::Foundation::Collections::IVectorView;
        using ABI::Windows::Networking::Connectivity::ConnectionProfile;
        using ABI::Windows::Networking::Connectivity::INetworkAdapter;
        using ABI::Windows::Networking::Connectivity::INetworkInformationStatics;
        using ABI::Windows::Networking::Connectivity::NetworkConnectivityLevel;

        // Get adapter addresses info.
        const auto adapterAddresses = GetAdapterAddresses(family);

        // Get connection profile info.
        const auto roInit = wil::RoInitialize();
        const auto networkInformationStatics =
            wil::GetActivationFactory<INetworkInformationStatics>(RuntimeClass_Windows_Networking_Connectivity_NetworkInformation);
        THROW_HR_IF_NULL_MSG(E_OUTOFMEMORY, networkInformationStatics.get(), "null INetworkInformationStatics");
        wil::com_ptr<IVectorView<ConnectionProfile*>> connectionList;
        THROW_IF_FAILED(networkInformationStatics->GetConnectionProfiles(&connectionList));

        // If we find a connection profile marked as having internet access and the associated
        // adapter has a <family> unicast address and a <family> default gateway, then conclude the
        // host has <family> internet connectivity.
        for (const auto& connectionProfile : wil::get_range(connectionList.get()))
        {
            NetworkConnectivityLevel connectivityLevel{};
            CONTINUE_IF_FAILED(connectionProfile->GetNetworkConnectivityLevel(&connectivityLevel));
            if (connectivityLevel != NetworkConnectivityLevel::NetworkConnectivityLevel_InternetAccess)
            {
                continue;
            }

            wil::com_ptr<INetworkAdapter> networkAdapter;
            CONTINUE_IF_FAILED(connectionProfile->get_NetworkAdapter(&networkAdapter));

            GUID interfaceGuid{};
            CONTINUE_IF_FAILED(networkAdapter->get_NetworkAdapterId(&interfaceGuid));

            NET_LUID interfaceLuid{};
            CONTINUE_IF_FAILED_WIN32(ConvertInterfaceGuidToLuid(&interfaceGuid, &interfaceLuid));

            for (const IP_ADAPTER_ADDRESSES* adapter = adapterAddresses.get(); adapter != nullptr; adapter = adapter->Next)
            {
                if (interfaceLuid.Value == adapter->Luid.Value && adapter->FirstUnicastAddress != nullptr && adapter->FirstGatewayAddress != nullptr)
                {
                    return true;
                }
            }
        }

        return false;
    }

    static std::unique_ptr<IP_ADAPTER_ADDRESSES> GetAdapterAddresses(ADDRESS_FAMILY family)
    {
        ULONG result;
        constexpr ULONG flags =
            (GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS);
        ULONG bufferSize = 0;
        std::unique_ptr<IP_ADAPTER_ADDRESSES> buffer;

        while ((result = GetAdaptersAddresses(family, flags, nullptr, buffer.get(), &bufferSize)) == ERROR_BUFFER_OVERFLOW)
        {
            buffer.reset(static_cast<IP_ADAPTER_ADDRESSES*>(malloc(bufferSize)));
            VERIFY_IS_NOT_NULL(buffer.get());
        }

        VERIFY_WIN32_SUCCEEDED(result);

        return buffer;
    }

    // Due to VM creation performance requirements, VM creation is allowed to finish even if all
    // networking state has not been mirrored yet. This introduces a race condition between the
    // mirroring of networking state and mirrored mode test case execution that relies on the
    // networking state being mirrored.
    //
    // This routine resolves the race condition by waiting for networking state to be mirrored into
    // the VM. Tracking all mirrored networking state is complicated, so we use a heuristic to
    // simplify: default routes have been observed to be mirrored last, so if they are present in
    // the VM then we consider mirroring to be completed.
    static void WaitForMirroredStateInLinux()
    {
        const bool hostConnectivityV4 = HostHasInternetConnectivity(AF_INET);
        const bool hostConnectivityV6 = HostHasInternetConnectivity(AF_INET6);

        Stopwatch<std::chrono::seconds> Watchdog(std::chrono::seconds(30));

        do
        {
            // Count how many interfaces have v4/v6 connectivity, as defined by having a gateway and at least 1 preferred address.
            int interfacesWithV4Connectivity = 0;
            int interfacesWithV6Connectivity = 0;

            // Get all interface info from the VM.
            for (const auto& i : GetAllInterfaceStates())
            {
                if (i.Gateway.has_value())
                {
                    for (const auto& j : i.V4Addresses)
                    {
                        if (j.Preferred)
                        {
                            interfacesWithV4Connectivity++;
                            break;
                        }
                    }
                }
                if (i.V6Gateway.has_value())
                {
                    for (const auto& j : i.V6Addresses)
                    {
                        if (j.Preferred)
                        {
                            interfacesWithV6Connectivity++;
                            break;
                        }
                    }
                }
            }

            // Consider mirroring to be complete if we have the same v4/v6 connectivity in the VM as the host.
            if ((!hostConnectivityV4 || interfacesWithV4Connectivity > 0) && (!hostConnectivityV6 || interfacesWithV6Connectivity > 0))
            {
                break;
            }

            LogInfo("Waiting for mirrored state...");
        } while (Sleep(1000), !Watchdog.IsExpired());

        VERIFY_IS_FALSE(Watchdog.IsExpired());
    }

    static void WaitForNATStateInLinux()
    {
        Stopwatch<std::chrono::seconds> Watchdog(std::chrono::seconds(30));

        // NAT only supports IPv4 connectivity
        // wait for the host to have v4 connectivity
        do
        {
            if (HostHasInternetConnectivity(AF_INET))
            {
                break;
            }

            LogInfo("Waiting for Windows network connectivity...");
        } while (Sleep(1000), !Watchdog.IsExpired());
        VERIFY_IS_FALSE(Watchdog.IsExpired());

        // reset the watchdog
        Watchdog = Stopwatch{std::chrono::seconds(30)};

        do
        {
            // Count how many interfaces have v4 connectivity, as defined by having a gateway and at least 1 preferred address.
            int interfacesWithV4Connectivity = 0;

            // Get all interface info from the VM.
            for (const auto& i : GetAllInterfaceStates())
            {
                if (i.Gateway.has_value())
                {
                    for (const auto& j : i.V4Addresses)
                    {
                        if (j.Preferred)
                        {
                            interfacesWithV4Connectivity++;
                            break;
                        }
                    }
                }
            }

            // Consider mirroring to be complete if we have the same v4 connectivity in the VM as the host.
            if (interfacesWithV4Connectivity > 0)
            {
                break;
            }

            LogInfo("Waiting for NAT state...");
        } while (Sleep(1000), !Watchdog.IsExpired());
        VERIFY_IS_FALSE(Watchdog.IsExpired());
    }

    // Set ManualConnectivityValidation to true to manually check stdout from the test to verify the correct calls are made in Linux/Init
    static constexpr bool ManualConnectivityValidation = false;
    TEST_METHOD(ConnectivityCheckTestMirroredDefaultSuccess)
    {
        WSL2_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        const auto coInit = wil::CoInitializeEx();
        const wil::com_ptr<INetworkListManager> networkListManager = wil::CoCreateInstance<NetworkListManager, INetworkListManager>();
        VERIFY_IS_NOT_NULL(networkListManager.get());
        NLM_CONNECTIVITY hostConnectivity{};
        VERIFY_SUCCEEDED(networkListManager->GetConnectivity(&hostConnectivity));

        // Windows
        const wsl::shared::conncheck::ConnCheckResult hostResult =
            wsl::shared::conncheck::CheckConnection("www.msftconnecttest.com", "ipv6.msftconnecttest.com", "80");

        if (hostConnectivity & NLM_CONNECTIVITY_IPV4_INTERNET)
        {
            VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::Success, hostResult.Ipv4Status);
        }
        else
        {
            // one of the 2 expected runtime failures
            VERIFY_IS_TRUE(
                hostResult.Ipv4Status == wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo ||
                hostResult.Ipv4Status == wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect);
        }

        if (hostConnectivity & NLM_CONNECTIVITY_IPV6_INTERNET)
        {
            VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::Success, hostResult.Ipv4Status);
        }
        else
        {
            // one of the 2 expected runtime failures
            VERIFY_IS_TRUE(
                hostResult.Ipv6Status == wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo ||
                hostResult.Ipv6Status == wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect);
        }

        // www.msftconnecttest.com will always fail IPv6 name resolution - it doesn't have any AAAA records registered for it
        const int expectedErrorCode = static_cast<int>(hostResult.Ipv4Status) |
                                      (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo) << 16);
        LogInfo("RunGns(www.msftconnecttest.com, 0x%x)", expectedErrorCode);
        // TODO: pass 'expectedErrorCode' instead of 1, once the pipeline is fixed from running Init back to wsl.exe
        // it returns 1 as that's the lowest 16 bit value (unknown where the upper 16 bits are trimmed)
        // if ManualConnectivityValidation is set true, one can confirm from the stdout captured that the correct result was determined and returned by init.
        constexpr auto testErrorCode = ManualConnectivityValidation ? expectedErrorCode : 1;
        RunGns("www.msftconnecttest.com", AdapterId, LxGnsMessageConnectTestRequest, testErrorCode);
    }

    TEST_METHOD(ConnectivityCheckTestNATDefaultSuccess)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig());
        WaitForNATStateInLinux();

        const auto coInit = wil::CoInitializeEx();
        const wil::com_ptr<INetworkListManager> networkListManager = wil::CoCreateInstance<NetworkListManager, INetworkListManager>();
        VERIFY_IS_NOT_NULL(networkListManager.get());
        NLM_CONNECTIVITY hostConnectivity{};
        VERIFY_SUCCEEDED(networkListManager->GetConnectivity(&hostConnectivity));

        // Windows
        const wsl::shared::conncheck::ConnCheckResult hostResult =
            wsl::shared::conncheck::CheckConnection("www.msftconnecttest.com", "ipv6.msftconnecttest.com", "80");

        if (hostConnectivity & NLM_CONNECTIVITY_IPV4_INTERNET)
        {
            VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::Success, hostResult.Ipv4Status);
        }
        else
        {
            // one of the 2 expected runtime failures
            VERIFY_IS_TRUE(
                hostResult.Ipv4Status == wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo ||
                hostResult.Ipv4Status == wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect);
        }
        if (hostConnectivity & NLM_CONNECTIVITY_IPV6_INTERNET)
        {
            VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::Success, hostResult.Ipv4Status);
        }
        else
        {
            // one of the 2 expected runtime failures (sometimes v6 name resolution will fail, depending on the configuration)
            VERIFY_IS_TRUE(
                hostResult.Ipv6Status == wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo ||
                hostResult.Ipv6Status == wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect);
        }

        // www.msftconnecttest.com will always fail IPv6 name resolution - it doesn't have any AAAA records registered for it
        const int expectedErrorCode = static_cast<int>(hostResult.Ipv4Status) |
                                      (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo) << 16);
        LogInfo("RunGns(www.msftconnecttest.com, 0x%x)", expectedErrorCode);
        // TODO: pass 'expectedErrorCode' instead of 1, once the pipeline is fixed from running Init back to wsl.exe
        // it returns 1 (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::Success)
        // as that's the lowest 16 bit value (unknown where the upper 16 bits are trimmed)
        // if ManualConnectivityValidation is set true, one can confirm from the stdout captured that the correct result was determined and returned by init.
        constexpr auto testErrorCode =
            ManualConnectivityValidation ? expectedErrorCode : static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::Success);
        RunGns("www.msftconnecttest.com", AdapterId, LxGnsMessageConnectTestRequest, testErrorCode);
    }

    TEST_METHOD(ConnectivityCheckTestMirroredNameResolutionFailure)
    {
        WSL2_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        // Windows
        const wsl::shared::conncheck::ConnCheckResult result =
            wsl::shared::conncheck::CheckConnection("asdlkfadsf.bbcxzncvb", nullptr, "80");

        VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo, result.Ipv4Status);
        VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo, result.Ipv6Status);

        constexpr int expectedErrorCode = static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo) |
                                          (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo) << 16);
        LogInfo("RunGns(asdlkfadsf.bbcxzncvb, 0x%x)", expectedErrorCode);
        // TODO: pass 'expectedErrorCode' instead of 1, once the pipeline is fixed from running Init back to wsl.exe
        // it returns 2 (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo))
        // as that's the lowest 16 bit value (unknown where the upper 16 bits are trimmed)
        // if temporarily change this back to expectedErrorCode, one can confirm from the stdout captured that the correct result was determined and returned by init.
        constexpr auto testErrorCode = ManualConnectivityValidation
                                           ? expectedErrorCode
                                           : static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo);
        RunGns("asdlkfadsf.bbcxzncvb", AdapterId, LxGnsMessageConnectTestRequest, testErrorCode);
    }

    TEST_METHOD(ConnectivityCheckTestNATNameResolutionFailure)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig());
        WaitForNATStateInLinux();

        // Windows
        const wsl::shared::conncheck::ConnCheckResult result =
            wsl::shared::conncheck::CheckConnection("asdlkfadsf.bbcxzncvb", nullptr, "80");

        VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo, result.Ipv4Status);
        VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo, result.Ipv6Status);

        constexpr int expectedErrorCode = static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo) |
                                          (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo) << 16);
        LogInfo("RunGns(asdlkfadsf.bbcxzncvb, 0x%x)", expectedErrorCode);
        // TODO: pass 'expectedErrorCode' instead of 1, once the pipeline is fixed from running Init back to wsl.exe
        // it returns 2 (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo))
        // as that's the lowest 16 bit value (unknown where the upper 16 bits are trimmed)
        // if temporarily change this back to expectedErrorCode, one can confirm from the stdout captured that the correct result was determined and returned by init.
        constexpr auto testErrorCode = ManualConnectivityValidation
                                           ? expectedErrorCode
                                           : static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo);
        RunGns("asdlkfadsf.bbcxzncvb", AdapterId, LxGnsMessageConnectTestRequest, testErrorCode);
    }

    TEST_METHOD(ConnectivityCheckTestMirroredNameResolvesButConnectivityFails)
    {
        WSL2_TEST_ONLY();
        MIRRORED_NETWORKING_TEST_ONLY();

        SKIP_TEST_UNSTABLE();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Mirrored}));
        WaitForMirroredStateInLinux();

        const auto* ncsiDnsOnlyName = "dns.msftncsi.com";
        // v4 and v6 should succeed to resolve the name, but fail to connect,
        // as this NCSI name is registered in global DNS, but there's not HTTP endpoint for it

        // Windows
        const wsl::shared::conncheck::ConnCheckResult result =
            wsl::shared::conncheck::CheckConnection(ncsiDnsOnlyName, nullptr, "80");

        VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect, result.Ipv4Status);
        // v6 name resolution might fail, depending on the configuration
        VERIFY_IS_TRUE(
            (wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo == result.Ipv6Status) ||
            (wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect == result.Ipv6Status));

        constexpr int expectedErrorCode = static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect) |
                                          (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect) << 16);
        LogInfo("RunGns(%hs, 0x%x)", ncsiDnsOnlyName, expectedErrorCode);
        // TODO: pass 'expectedErrorCode' instead of 1, once the pipeline is fixed from running Init back to wsl.exe
        // it returns 4 (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect))
        // as that's the lowest 16 bit value (unknown where the upper 16 bits are trimmed)
        // if ManualConnectivityValidation is set true, one can confirm from the stdout captured that the correct result was determined and returned by init.
        constexpr auto testErrorCode = ManualConnectivityValidation
                                           ? expectedErrorCode
                                           : static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect);
        RunGns(ncsiDnsOnlyName, AdapterId, LxGnsMessageConnectTestRequest, testErrorCode);
    }

    TEST_METHOD(ConnectivityCheckTestNATNameResolvesButConnectivityFails)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig());
        WaitForNATStateInLinux();

        const auto* ncsiDnsOnlyName = "dns.msftncsi.com";
        // v4 and v6 should succeed to resolve the name, but fail to connect,
        // as this NCSI name is registered in global DNS, but there's not HTTP endpoint for it

        // Windows
        const wsl::shared::conncheck::ConnCheckResult result =
            wsl::shared::conncheck::CheckConnection(ncsiDnsOnlyName, nullptr, "80");

        VERIFY_ARE_EQUAL(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect, result.Ipv4Status);
        // v6 name resolution might fail, depending on the configuration
        VERIFY_IS_TRUE(
            (wsl::shared::conncheck::ConnCheckStatus::FailureGetAddrInfo == result.Ipv6Status) ||
            (wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect == result.Ipv6Status));

        constexpr int expectedErrorCode = static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect) |
                                          (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect) << 16);
        LogInfo("RunGns(%hs, 0x%x)", ncsiDnsOnlyName, expectedErrorCode);
        // TODO: pass 'expectedErrorCode' instead of 1, once the pipeline is fixed from running Init back to wsl.exe
        // it returns 4 (static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect))
        // as that's the lowest 16 bit value (unknown where the upper 16 bits are trimmed)
        // if ManualConnectivityValidation is set true, one can confirm from the stdout captured that the correct result was determined and returned by init.
        constexpr auto testErrorCode = ManualConnectivityValidation
                                           ? expectedErrorCode
                                           : static_cast<int>(wsl::shared::conncheck::ConnCheckStatus::FailureSocketConnect);
        RunGns(ncsiDnsOnlyName, AdapterId, LxGnsMessageConnectTestRequest, testErrorCode);
    }
};

class BridgedTests
{
    WSL_TEST_CLASS(BridgedTests)

    std::optional<WslConfigChange> m_config;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(false), TRUE);

        if (LxsstuVmMode())
        {
            m_config.emplace(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Bridged, .vmSwitch = L"Default Switch"}));
        }

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        m_config.reset();

        VERIFY_NO_THROW(LxsstuUninitialize(false));

        return true;
    }

    TEST_METHOD(Basic)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY();

        // There's no way to guarantee that an external switch will work in the test environment
        // So this test just validates that the VM successfully starts.
        m_config->Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Bridged, .vmSwitch = L"Default Switch"}));

        // Verify that ipv6 is disabled by default.
        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv6/conf/all/disable_ipv6");
        VERIFY_ARE_EQUAL(L"1\n", out);
    }

    TEST_METHOD(CustomMac)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY();

        constexpr auto mac = L"aa:bb:cc:dd:ee:ff";
        m_config->Update(LxssGenerateTestConfig(
            {.networkingMode = wsl::core::NetworkingMode::Bridged, .vmSwitch = L"Default Switch", .macAddress = mac}));

        VERIFY_ARE_EQUAL(mac, GetMacAddress());
    }

    TEST_METHOD(CustomMacDashes)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY();

        // Note: The SynthNic fails to start if the first byte of the mac address is 0xff.

        std::wstring mac = L"ee-ee-dd-cc-bb-aa";
        m_config->Update(LxssGenerateTestConfig(
            {.networkingMode = wsl::core::NetworkingMode::Bridged, .vmSwitch = L"Default Switch", .macAddress = mac}));

        std::replace(mac.begin(), mac.end(), L'-', L':');
        VERIFY_ARE_EQUAL(mac, GetMacAddress());
    }

    TEST_METHOD(Ipv6)
    {
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY();

        m_config->Update(LxssGenerateTestConfig(
            {.networkingMode = wsl::core::NetworkingMode::Bridged, .vmSwitch = L"Default Switch", .ipv6 = true}));

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/sys/net/ipv6/conf/all/disable_ipv6");
        VERIFY_ARE_EQUAL(L"0\n", out);
    }
};
} // namespace NetworkTests
