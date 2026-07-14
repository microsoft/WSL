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
        EnsureContainerDoesNotExist(WslcTargetContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcTargetContainerName);
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

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_WithEndpointFlags_RoundTrips)
    {
        const std::wstring subnet = L"172.72.0.0/16";
        const std::wstring ipAddress = L"172.72.0.42";
        const std::wstring alias1 = L"primary-alias";
        const std::wstring alias2 = L"secondary-alias";
        const std::wstring linkLocal = L"169.254.11.5";

        auto result = RunWslc(std::format(L"network create --driver bridge --subnet {} {}", subnet, TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"network connect --network-alias {} --network-alias {} --ip {} --link-local-ip {} {} {}", alias1, alias2, ipAddress, linkLocal, TestNetworkName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        const auto networkKey = string::WideToMultiByte(TestNetworkName);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkKey));
        const auto& endpoint = inspect.NetworkSettings.Networks.at(networkKey);
        VERIFY_ARE_EQUAL(string::WideToMultiByte(ipAddress), endpoint.IPAddress);
        VERIFY_IS_TRUE(endpoint.IPAMConfig.has_value());
        VERIFY_ARE_EQUAL(string::WideToMultiByte(ipAddress), endpoint.IPAMConfig->IPv4Address);
        VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, string::WideToMultiByte(alias1)) != endpoint.Aliases.end());
        VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, string::WideToMultiByte(alias2)) != endpoint.Aliases.end());
        VERIFY_IS_TRUE(
            std::ranges::find(endpoint.IPAMConfig->LinkLocalIPs, string::WideToMultiByte(linkLocal)) !=
            endpoint.IPAMConfig->LinkLocalIPs.end());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_InvalidIp_Rejected)
    {
        const std::wstring badIp = L"not-an-ip";

        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect --ip {} {} {}", badIp, TestNetworkName, WslcContainerName));
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VerifyPatternMatch(
            string::WideToMultiByte(result.Stderr.value()), std::format("*Invalid IP address '{}'*", string::WideToMultiByte(badIp)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_DriverOpt_RoundTrips)
    {
        const std::wstring driverOptKey = L"com.docker.network.endpoint.custom";
        const std::wstring driverOptValue = L"verify";

        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network connect --driver-opt {}={} {} {}", driverOptKey, driverOptValue, TestNetworkName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        const auto networkKey = string::WideToMultiByte(TestNetworkName);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkKey));
        const auto& endpoint = inspect.NetworkSettings.Networks.at(networkKey);
        const auto keyUtf8 = string::WideToMultiByte(driverOptKey);
        const auto valueUtf8 = string::WideToMultiByte(driverOptValue);
        const auto driverOptIt = endpoint.DriverOpts.find(keyUtf8);
        VERIFY_IS_TRUE(driverOptIt != endpoint.DriverOpts.end());
        VERIFY_ARE_EQUAL(valueUtf8, driverOptIt->second);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Connect_Link_RoundTrips)
    {
        const std::wstring targetAlias = L"db";

        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"container run -d --network {} --network-alias {} --name {} {} sleep infinity",
            TestNetworkName,
            targetAlias,
            WslcTargetContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const std::wstring linkEntry = std::format(L"{}:{}", WslcTargetContainerName, targetAlias);
        result = RunWslc(std::format(L"network connect --link {} {} {}", linkEntry, TestNetworkName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        const auto networkKey = string::WideToMultiByte(TestNetworkName);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkKey));
        const auto& endpoint = inspect.NetworkSettings.Networks.at(networkKey);
        const auto linkEntryUtf8 = string::WideToMultiByte(linkEntry);
        VERIFY_IS_TRUE(std::ranges::find(endpoint.Links, linkEntryUtf8) != endpoint.Links.end());
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
    const std::wstring WslcTargetContainerName = L"wslc-e2e-network-connect-target";
    const std::wstring TestNetworkName = L"wslc-e2e-network-connect";
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_NetworkCommandLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc network [<command>] [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::vector<std::pair<std::wstring_view, std::wstring>> entries = {
            {L"create", Localization::WSLCCLI_NetworkCreateDesc()},
            {L"remove", Localization::WSLCCLI_NetworkRemoveDesc()},
            {L"inspect", Localization::WSLCCLI_NetworkInspectDesc()},
            {L"list", Localization::WSLCCLI_NetworkListDesc()},
            {L"prune", Localization::WSLCCLI_NetworkPruneDesc()},
            {L"connect", Localization::WSLCCLI_NetworkConnectDesc()},
            {L"disconnect", Localization::WSLCCLI_NetworkDisconnectDesc()},
        };

        size_t maxLen = 0;
        for (const auto& [name, _] : entries)
        {
            maxLen = (std::max)(maxLen, name.size());
        }

        std::wstringstream commands;
        commands << Localization::WSLCCLI_AvailableSubcommands() << L"\r\n";
        for (const auto& [name, desc] : entries)
        {
            commands << L"  " << name << std::wstring(maxLen - name.size() + 2, L' ') << desc << L"\r\n";
        }
        commands << L"\r\n" << Localization::WSLCCLI_HelpForDetails() << L" [" << WSLC_CLI_HELP_ARG_STRING << L"]\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -?,--help  Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }

    std::wstring GetConnectHelpMessage() const
    {
        const std::vector<std::pair<std::wstring, std::wstring>> arguments = {
            {L"network-name", L"Network name"},
            {L"container-id", L"Container ID"},
        };
        const std::vector<std::pair<std::wstring, std::wstring>> options = {
            {L"--network-alias", Localization::WSLCCLI_NetworkAliasArgDescription()},
            {L"--ip", Localization::WSLCCLI_IpAddressArgDescription()},
            {L"--link", Localization::WSLCCLI_LinkArgDescription()},
            {L"--link-local-ip", Localization::WSLCCLI_LinkLocalIpArgDescription()},
            {L"--driver-opt", Localization::WSLCCLI_DriverOptArgDescription()},
            {L"-?,--help", L"Shows help about the selected command"},
        };

        std::wstringstream output;
        output << GetWslcHeader()                                                                  //
               << Localization::WSLCCLI_NetworkConnectLongDesc() + L"\r\n\r\n"                     //
               << L"Usage: wslc network connect [<options>] <network-name> <container-id>\r\n\r\n" //
               << RenderArgumentsAndOptions(arguments, options);
        return output.str();
    }

    std::wstring GetDisconnectHelpMessage() const
    {
        const std::vector<std::pair<std::wstring, std::wstring>> arguments = {
            {L"network-name", L"Network name"},
            {L"container-id", L"Container ID"},
        };
        const std::vector<std::pair<std::wstring, std::wstring>> options = {
            {L"-?,--help", L"Shows help about the selected command"},
        };

        std::wstringstream output;
        output << GetWslcHeader()                                                                     //
               << Localization::WSLCCLI_NetworkDisconnectLongDesc() + L"\r\n\r\n"                     //
               << L"Usage: wslc network disconnect [<options>] <network-name> <container-id>\r\n\r\n" //
               << RenderArgumentsAndOptions(arguments, options);
        return output.str();
    }

    // Mirrors Command::OutputHelp: maxLen is driven by GetUsageString() (prepends "--" for positional args).
    std::wstring RenderArgumentsAndOptions(
        const std::vector<std::pair<std::wstring, std::wstring>>& arguments, const std::vector<std::pair<std::wstring, std::wstring>>& options) const
    {
        size_t maxLen = 0;
        for (const auto& [name, _] : arguments)
        {
            maxLen = (std::max)(maxLen, name.size() + 2);
        }
        for (const auto& [name, _] : options)
        {
            maxLen = (std::max)(maxLen, name.size());
        }

        std::wstringstream out;
        out << L"The following arguments are available:\r\n";
        for (const auto& [name, desc] : arguments)
        {
            out << L"  " << name << std::wstring(maxLen - name.size() + 2, L' ') << desc << L"\r\n";
        }
        out << L"\r\n" << L"The following options are available:\r\n";
        for (const auto& [name, desc] : options)
        {
            out << L"  " << name << std::wstring(maxLen - name.size() + 2, L' ') << desc << L"\r\n";
        }
        out << L"\r\n";
        return out.str();
    }
};
} // namespace WSLCE2ETests
