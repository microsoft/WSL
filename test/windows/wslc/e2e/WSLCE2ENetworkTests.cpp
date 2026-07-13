/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "Argument.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ENetworkTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_HelpCommand)
    {
        auto result = RunWslc(L"network --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_NoSubcommand_ShowsHelp)
    {
        auto result = RunWslc(L"network");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_InvalidCommand_DisplaysErrorMessage)
    {
        auto result = RunWslc(L"network INVALID_CMD");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unrecognized command: 'INVALID_CMD'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_HelpCommand)
    {
        auto result = RunWslc(L"network connect --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_HelpCommand)
    {
        auto result = RunWslc(L"network disconnect --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_MissingNetworkName)
    {
        auto result = RunWslc(L"network connect");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'network-name'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_MissingContainerId)
    {
        auto result = RunWslc(std::format(L"network connect {}", TestNetworkName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'container-id'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_MissingNetworkName)
    {
        auto result = RunWslc(L"network disconnect");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'network-name'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_MissingContainerId)
    {
        auto result = RunWslc(std::format(L"network disconnect {}", TestNetworkName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'container-id'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_Valid)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect {} {}", TestNetworkName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(string::WideToMultiByte(TestNetworkName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_UnknownNetwork)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect {} {}", TestNetworkName, WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Network not found: '{}'\r\nError code: WSLC_E_NETWORK_NOT_FOUND\r\n", TestNetworkName), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_UnknownContainer)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect {} {}", TestNetworkName, WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_NoneMode_Rejected)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(
            std::format(L"container run -d --network none --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect {} {}", TestNetworkName, WslcContainerName));
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VerifyPatternMatch(string::WideToMultiByte(result.Stderr.value()), "*'host' or 'none'*Error code: *\r\n");
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_AlreadyConnected_DockerErrorPropagated)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"container run -d --network {} --name {} {} sleep infinity", TestNetworkName, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect {} {}", TestNetworkName, WslcContainerName));
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VerifyPatternMatch(string::WideToMultiByte(result.Stderr.value()), "*already exists*Error code: *\r\n");
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_Valid)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"container run -d --network {} --name {} {} sleep infinity", TestNetworkName, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        {
            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(string::WideToMultiByte(TestNetworkName)));
        }

        result = RunWslc(std::format(L"network disconnect {} {}", TestNetworkName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_IS_FALSE(inspect.NetworkSettings.Networks.contains(string::WideToMultiByte(TestNetworkName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_UnknownNetwork)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network disconnect {} {}", TestNetworkName, WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Network not found: '{}'\r\nError code: WSLC_E_NETWORK_NOT_FOUND\r\n", TestNetworkName), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_UnknownContainer)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network disconnect {} {}", TestNetworkName, WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

private:
    const std::wstring WslcContainerName = L"wslc-e2e-network-connect-container";
    const std::wstring TestNetworkName = L"wslc-e2e-network-connect";
    const TestImage& DebianImage = DebianTestImage();
};
} // namespace WSLCE2ETests
