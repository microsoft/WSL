/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCNetworkingModeTests.cpp

Abstract:

    This file contains test cases for the WSLC session networking modes.

--*/

#include "precomp.h"
#include "WSLCTestBase.h"

class WSLCNetworkingModeTests : public WSLCTestBase
{
    WSLC_TEST_CLASS(WSLCNetworkingModeTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return BaseClassSetup();
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return BaseClassCleanup();
    }

    void ValidateNetworking(WSLCNetworkingMode mode, bool enableDnsTunneling = false)
    {
        // Reuse the default session if settings match (same networking mode and DNS tunneling setting).
        auto createNewSession = mode != m_defaultSessionSettings.NetworkingMode ||
                                enableDnsTunneling != WI_IsFlagSet(m_defaultSessionSettings.FeatureFlags, WslcFeatureFlagsDnsTunneling);

        auto settings = GetDefaultSessionSettings(L"networking-test", false, mode);
        WI_UpdateFlag(settings.FeatureFlags, WslcFeatureFlagsDnsTunneling, enableDnsTunneling);
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Validate that eth0 has an ip address
        ExpectCommandResult(
            session.get(),
            {"/bin/sh",
             "-c",
             "ip a  show dev eth0 | grep -iF 'inet ' |  grep -E '[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}\\.[0-9]{1,3}'"},
            0);

        ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver", "/etc/resolv.conf"}, 0);

        // Verify that /etc/resolv.conf is correctly configured.
        if (enableDnsTunneling)
        {
            auto result = ExpectCommandResult(session.get(), {"/bin/grep", "-iF", "nameserver ", "/etc/resolv.conf"}, 0);

            if (mode == WSLCNetworkingModeConsomme)
            {
                // Consomme points resolv.conf at the eth0 gateway.
                ExpectCommandResult(
                    session.get(),
                    {"/bin/sh",
                     "-c",
                     "ns=$(awk '/^nameserver/ {print $2; exit}' /etc/resolv.conf); "
                     "gw=$(ip route show default | awk '{print $3; exit}'); "
                     "[ -n \"$ns\" ] && [ -n \"$gw\" ] && [ \"$ns\" = \"$gw\" ]"},
                    0);
            }
            else
            {
                VERIFY_ARE_EQUAL(result.Output[1], std::format("nameserver {}\n", LX_INIT_DNS_TUNNELING_IP_ADDRESS));
            }
        }

        // Verify DNS resolution.
        // Note: without DNS tunneling, NAT mode uses the ICS SharedAccess DNS proxy which only supports UDP.
        // TCP DNS queries (dig +tcp) will time out without tunneling.
        VerifyDigDnsResolution(session.get(), "getent ahosts bing.com");
        VerifyDnsQueries(session.get(), mode, enableDnsTunneling);
    }

    TEST_METHOD(NATNetworking)
    {
        ValidateNetworking(WSLCNetworkingModeNAT);
    }

    TEST_METHOD(NATNetworkingWithDnsTunneling)
    {
        WINDOWS_11_TEST_ONLY();
        ValidateNetworking(WSLCNetworkingModeNAT, true);
    }

    TEST_METHOD(ConsommeNetworking)
    {
        ValidateNetworking(WSLCNetworkingModeConsomme);
    }

    TEST_METHOD(ConsommeNetworkingWithDnsTunneling)
    {
        WINDOWS_11_TEST_ONLY();
        ValidateNetworking(WSLCNetworkingModeConsomme, true);
    }

    void VerifyDigDnsResolution(IWSLCSession* session, const std::string& digCommandLine)
    {
        auto result = ExpectCommandResult(session, {"/bin/sh", "-c", digCommandLine}, 0);
        VERIFY_IS_FALSE(result.Output[1].empty());
    }

    void VerifyDnsQueries(IWSLCSession* session, WSLCNetworkingMode mode, bool enableDnsTunneling)
    {
        // TCP DNS works except for NAT without tunneling (ICS SharedAccess DNS proxy is UDP-only).
        const bool includeTcp = (mode != WSLCNetworkingModeNAT) || enableDnsTunneling;

        // UDP queries for all record types
        VerifyDigDnsResolution(session, "dig +short +time=5 A bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 AAAA bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 MX bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 NS bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 -x 8.8.8.8");
        VerifyDigDnsResolution(session, "dig +short +time=5 SOA bing.com");
        VerifyDigDnsResolution(session, "dig +short +time=5 TXT bing.com");
        VerifyDigDnsResolution(session, "dig +time=5 CNAME bing.com");
        VerifyDigDnsResolution(session, "dig +time=5 SRV bing.com");

        if (includeTcp)
        {
            // ANY - dig expects a large response so it queries directly over TCP
            VerifyDigDnsResolution(session, "dig +short +time=5 ANY bing.com");

            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 A bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 AAAA bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 MX bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 NS bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 -x 8.8.8.8");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 SOA bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +short +time=5 TXT bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +time=5 CNAME bing.com");
            VerifyDigDnsResolution(session, "dig +tcp +time=5 SRV bing.com");
        }
    }

    void ValidatePortMapping(WSLCNetworkingMode networkingMode)
    {
        auto settings = GetDefaultSessionSettings(L"port-mapping-test");
        settings.NetworkingMode = networkingMode;

        // Reuse the default session if the networking mode matches.
        auto createNewSession = networkingMode != m_defaultSessionSettings.NetworkingMode;
        auto session = createNewSession ? CreateSession(settings) : m_defaultSession;

        // Install socat in the VM.
        {
            constexpr auto c_mountPoint = "/testdata";
            auto mountSource = std::filesystem::absolute(g_testDataPath);

            VERIFY_SUCCEEDED(session->MountWindowsFolder(mountSource.c_str(), c_mountPoint, true, TRUE));
            auto unmount = wil::scope_exit_log(
                WI_DIAGNOSTICS_INFO, [&]() { LOG_IF_FAILED(session->UnmountWindowsFolder(c_mountPoint, TRUE)); });

            const auto installCommand = std::format("tdnf install -y --disablerepo='*' --nogpgcheck {}/packages/*.rpm", c_mountPoint);
            auto installSocat = WSLCProcessLauncher("/bin/sh", {"/bin/sh", "-c", installCommand}).Launch(*session);
            ValidateProcessOutput(installSocat, {}, 0, 120 * 1000);
        }

        auto listen = [&](short port, const char* content, bool ipv6) {
            auto cmd = std::format("echo -n '{}' | /usr/bin/socat -dd TCP{}-LISTEN:{},reuseaddr -", content, ipv6 ? "6" : "", port);
            auto process = WSLCProcessLauncher("/bin/sh", {"/bin/sh", "-c", cmd}).Launch(*session);
            WaitForOutput(process.GetStdHandle(2), "listening on");

            return process;
        };

        auto connectAndRead = [&](short port, int family) -> std::string {
            SOCKADDR_INET addr{};
            addr.si_family = family;
            INETADDR_SETLOOPBACK((PSOCKADDR)&addr);
            SS_PORT(&addr) = htons(port);

            wil::unique_socket hostSocket{socket(family, SOCK_STREAM, IPPROTO_TCP)};
            THROW_LAST_ERROR_IF(!hostSocket);
            THROW_LAST_ERROR_IF(connect(hostSocket.get(), reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR);

            return ReadToString(hostSocket.get());
        };

        auto expectContent = [&](short port, int family, const char* expected) {
            auto content = connectAndRead(port, family);
            VERIFY_ARE_EQUAL(content, expected);
        };

        auto expectNotBound = [&](short port, int family) {
            auto result = wil::ResultFromException([&]() { connectAndRead(port, family); });

            VERIFY_ARE_EQUAL(result, HRESULT_FROM_WIN32(WSAECONNREFUSED));
        };

        // Map port
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, 1234, 80));

        // Validate that the same port can't be bound twice
        VERIFY_ARE_EQUAL(session->MapVmPort(AF_INET, 1234, 80), HRESULT_FROM_WIN32(WSAEADDRINUSE));

        // Check simple case
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that same port mapping can be reused
        listen(80, "port80", false);
        expectContent(1234, AF_INET, "port80");

        // Validate that the connection is immediately reset if the port is not bound on the linux side
        expectContent(1234, AF_INET, "");

        // Add a ipv6 binding
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET6, 1234, 80));

        // Validate that ipv6 bindings work as well.
        listen(80, "port80ipv6", true);
        expectContent(1234, AF_INET6, "port80ipv6");

        // Unmap the ipv4 port
        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, 1234, 80));

        // Verify that a proper error is returned if the mapping doesn't exist
        // TODO: update once virtionet error code is fixed.
        VERIFY_ARE_EQUAL(
            session->UnmapVmPort(AF_INET, 1234, 80), networkingMode == WSLCNetworkingModeNAT ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : E_INVALIDARG);

        // Unmap the v6 port
        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET6, 1234, 80));

        // Map another port as v6 only
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET6, 1235, 81));

        listen(81, "port81ipv6", true);
        expectContent(1235, AF_INET6, "port81ipv6");
        expectNotBound(1235, AF_INET);

        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET6, 1235, 81));
        VERIFY_ARE_EQUAL(session->UnmapVmPort(AF_INET6, 1235, 81), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
        expectNotBound(1235, AF_INET6);

        // Create a forking relay and stress test
        VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, 1234, 80));

        auto process =
            WSLCProcessLauncher{"/usr/bin/socat", {"/usr/bin/socat", "-dd", "TCP-LISTEN:80,fork,reuseaddr", "system:'echo -n OK'"}}
                .Launch(*session);

        WaitForOutput(process.GetStdHandle(2), "listening on");

        for (auto i = 0; i < 100; i++)
        {
            expectContent(1234, AF_INET, "OK");
        }

        VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, 1234, 80));

        // Validate the 63-port limit.
        // TODO: Remove the 63-port limit by switching the relay's AcceptThread from
        // WaitForMultipleObjects to IO completion ports or similar.
        constexpr int c_maxPorts = 63;
        for (int i = 0; i < c_maxPorts; i++)
        {
            VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + i), static_cast<uint16_t>(80 + i)));
        }

        if (networkingMode == WSLCNetworkingModeNAT)
        {
            // In NAT mode, the 64th port mapping should fail with ERROR_TOO_MANY_OPEN_FILES since the relay process uses a file handle for each mapping.
            VERIFY_ARE_EQUAL(
                session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)),
                HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES));
        }
        else
        {
            VERIFY_SUCCEEDED(session->MapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)));
            VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, static_cast<uint16_t>(20000 + c_maxPorts), static_cast<uint16_t>(80 + c_maxPorts)));
        }

        for (int i = 0; i < c_maxPorts; i++)
        {
            VERIFY_SUCCEEDED(session->UnmapVmPort(AF_INET, static_cast<uint16_t>(20000 + i), static_cast<uint16_t>(80 + i)));
        }
    }

    TEST_METHOD(PortMappingNat)
    {
        ValidatePortMapping(WSLCNetworkingModeNAT);
    }

    TEST_METHOD(PortMappingConsomme)
    {
        ValidatePortMapping(WSLCNetworkingModeConsomme);
    }

    WSLC_TEST_METHOD(StuckVmTermination)
    {
        // Create a 'stuck' process
        auto process = WSLCProcessLauncher{"/bin/cat", {"/bin/cat"}, {}, WSLCProcessFlagsStdin}.Launch(*m_defaultSession);

        // Stop the service
        StopWslService();

        ResetTestSession(); // Reopen the session since the service was stopped.
    }
};
