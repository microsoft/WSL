/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkCreateTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ENetworkCreateTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkCreateTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(PythonImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(ClientContainerName);
        EnsureContainerDoesNotExist(ServerContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(ClientContainerName);
        EnsureContainerDoesNotExist(ServerContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_HelpCommand)
    {
        auto result = RunWslc(L"network create --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_MissingName)
    {
        auto result = RunWslc(L"network create");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'network-name'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_DefaultDriver_Success)
    {
        auto result = RunWslc(std::format(L"network create {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_IS_FALSE(inspect.EnableIPv6);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_BridgeDriver_Success)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_WithLabels_Success)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge --label env=test --label app=wslc {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_ARE_EQUAL("test", inspect.Labels["env"]);
        VERIFY_ARE_EQUAL("wslc", inspect.Labels["app"]);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_EmptyLabelKey_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge --label =foo {}", TestNetworkName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Label key cannot be empty\r\nError code: E_INVALIDARG"));

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_InvalidDriver_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver invalid_driver {}", TestNetworkName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            std::format(L"Unsupported network driver: 'invalid_driver'\r\nError code: E_INVALIDARG")));

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Duplicate_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = FormatErrorMessage(L"Cannot create a file when that file already exists. ", L"ERROR_ALREADY_EXISTS"),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Internal_Success)
    {
        auto result = RunWslc(std::format(L"network create --internal {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_IS_TRUE(inspect.Internal);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Ipv6_Success)
    {
        const std::wstring subnet = L"fd00:172:53::/64";
        auto result = RunWslc(std::format(L"network create --ipv6 --subnet {} {}", subnet, TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_IS_TRUE(inspect.EnableIPv6);
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        const auto ipv6Config = std::ranges::find_if(*inspect.IPAM.Config, [&](const auto& config) {
            return config.Subnet == wsl::shared::string::WideToMultiByte(subnet);
        });
        VERIFY_IS_TRUE(ipv6Config != inspect.IPAM.Config->end());
        if (ipv6Config == inspect.IPAM.Config->end())
        {
            return;
        }

        const auto serverScript = std::format(
            L"import socket;"
            L"s=socket.socket(socket.AF_INET6,socket.SOCK_STREAM);"
            L"s.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY,1);"
            L"s.bind(('::',{}));"
            L"s.listen(1);"
            L"print('SERVER READY',flush=True);"
            L"c,_=s.accept();"
            L"c.sendall(b'ipv6-ok');"
            L"c.close()",
            Ipv6TestPort);

        result = RunWslc(std::format(
            L"container run -d --network {} --name {} {} python3 -u -c \"{}\"", TestNetworkName, ServerContainerName, PythonImage.NameAndTag(), serverScript));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        WaitForContainerOutput(ServerContainerName, "SERVER READY");

        inspect = InspectNetwork(TestNetworkName);
        const auto serverEndpoint = std::ranges::find_if(inspect.Containers, [&](const auto& entry) {
            return entry.second.Name == string::WideToMultiByte(ServerContainerName);
        });
        VERIFY_IS_TRUE(serverEndpoint != inspect.Containers.end());
        if (serverEndpoint == inspect.Containers.end())
        {
            return;
        }

        auto serverAddress = serverEndpoint->second.IPv6Address;
        const auto prefixSeparator = serverAddress.find('/');
        VERIFY_IS_TRUE(prefixSeparator != std::string::npos);
        if (prefixSeparator == std::string::npos)
        {
            return;
        }

        serverAddress.resize(prefixSeparator);

        const auto clientCommand = std::format(
            L"python3 -c \"import socket;"
            L"s=socket.socket(socket.AF_INET6,socket.SOCK_STREAM);"
            L"s.settimeout(10);"
            L"s.connect(('{}',{}));"
            L"print(s.recv(16).decode())\"",
            string::MultiByteToWide(serverAddress),
            Ipv6TestPort);

        result = RunWslc(std::format(
            L"container run --rm --network {} --name {} {} {}", TestNetworkName, ClientContainerName, PythonImage.NameAndTag(), clientCommand));
        result.Verify({.Stdout = L"ipv6-ok\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Subnet_Success)
    {
        const std::wstring subnet = L"172.45.0.0/16";
        auto result = RunWslc(std::format(L"network create --subnet {} {}", subnet, TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(subnet), (*inspect.IPAM.Config)[0].Subnet);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_SubnetAndGateway_Success)
    {
        const std::wstring subnet = L"172.46.0.0/16";
        const std::wstring gateway = L"172.46.0.1";
        auto result = RunWslc(std::format(L"network create --subnet {} --gateway {} {}", subnet, gateway, TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(subnet), (*inspect.IPAM.Config)[0].Subnet);
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(gateway), (*inspect.IPAM.Config)[0].Gateway);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_GatewayWithoutSubnet_Fail)
    {
        auto result = RunWslc(std::format(L"network create --gateway 172.47.0.1 {}", TestNetworkName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = FormatErrorMessage(L"The '--gateway' option requires '--subnet' to also be specified.", L"E_INVALIDARG"),
             .ExitCode = 1});

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_SubnetAndIpRange_Success)
    {
        const std::wstring subnet = L"172.51.0.0/16";
        const std::wstring ipRange = L"172.51.10.0/24";
        auto result = RunWslc(std::format(L"network create --subnet {} --ip-range {} {}", subnet, ipRange, TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_IS_TRUE(inspect.IPAM.Config.has_value());
        VERIFY_ARE_EQUAL(1u, inspect.IPAM.Config->size());
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(subnet), (*inspect.IPAM.Config)[0].Subnet);
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(ipRange), (*inspect.IPAM.Config)[0].IPRange);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_IpRangeWithoutSubnet_Fail)
    {
        auto result = RunWslc(std::format(L"network create --ip-range 172.52.10.0/24 {}", TestNetworkName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = FormatErrorMessage(L"The '--ip-range' option requires '--subnet' to also be specified.", L"E_INVALIDARG"),
             .ExitCode = 1});

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_WithOpt_Success)
    {
        constexpr auto c_opts = L"--opt com.docker.network.bridge.enable_icc=true --opt com.docker.network.driver.mtu=1450";
        auto result = RunWslc(std::format(L"network create {} {}", c_opts, TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
        VERIFY_ARE_EQUAL("true", inspect.Options["com.docker.network.bridge.enable_icc"]);
        VERIFY_ARE_EQUAL("1450", inspect.Options["com.docker.network.driver.mtu"]);
    }

private:
    const std::wstring ClientContainerName = L"wslc-e2e-network-ipv6-client";
    const std::wstring ServerContainerName = L"wslc-e2e-network-ipv6-server";
    const std::wstring TestNetworkName = L"wslc-e2e-network-create";
    const uint16_t Ipv6TestPort = 18080;
    const TestImage& PythonImage = PythonTestImage();
};
} // namespace WSLCE2ETests
