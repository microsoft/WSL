/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkConnectDisconnectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC network connect/disconnect commands.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ENetworkConnectDisconnectTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkConnectDisconnectTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_HelpCommand)
    {
        auto result = RunWslc(L"network connect --help");
        result.Verify({.Stdout = GetConnectHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_HelpCommand)
    {
        auto result = RunWslc(L"network disconnect --help");
        result.Verify({.Stdout = GetDisconnectHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_MissingNetworkName)
    {
        auto result = RunWslc(L"network connect");
        result.Verify({.Stdout = GetConnectHelpMessage(), .Stderr = L"Required argument not provided: 'network-name'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_MissingContainerId)
    {
        auto result = RunWslc(std::format(L"network connect {}", TestNetworkName));
        result.Verify({.Stdout = GetConnectHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_MissingNetworkName)
    {
        auto result = RunWslc(L"network disconnect");
        result.Verify({.Stdout = GetDisconnectHelpMessage(), .Stderr = L"Required argument not provided: 'network-name'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_MissingContainerId)
    {
        auto result = RunWslc(std::format(L"network disconnect {}", TestNetworkName));
        result.Verify({.Stdout = GetDisconnectHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
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
        VERIFY_IS_TRUE(result.Stderr.value().find(L"'host' or 'none'") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_AlreadyConnected_DockerErrorPropagated)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(
            std::format(
                L"container run -d --network {} --name {} {} sleep infinity", TestNetworkName, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect {} {}", TestNetworkName, WslcContainerName));
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr.value().find(L"Error code:") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Disconnect_Valid)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(
            std::format(
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

    std::wstring GetConnectHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()                                                                  //
               << Localization::WSLCCLI_NetworkConnectLongDesc() + L"\r\n\r\n"                     //
               << L"Usage: wslc network connect [<options>] <network-name> <container-id>\r\n\r\n" //
               << GetAvailableArguments()                                                          //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDisconnectHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()                                                                     //
               << Localization::WSLCCLI_NetworkDisconnectLongDesc() + L"\r\n\r\n"                     //
               << L"Usage: wslc network disconnect [<options>] <network-name> <container-id>\r\n\r\n" //
               << GetAvailableArguments()                                                             //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetAvailableArguments() const
    {
        std::wstringstream args;
        args << L"The following arguments are available:\r\n" //
             << L"  network-name    Network name\r\n"         //
             << L"  container-id    Container ID\r\n"         //
             << L"\r\n";
        return args.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                    //
                << L"  -?,--help       Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
