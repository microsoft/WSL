/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkRemoveTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ENetworkRemoveTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkRemoveTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureNetworkDoesNotExist(TestNetworkName2);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureNetworkDoesNotExist(TestNetworkName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_HelpCommand)
    {
        auto result = RunWslc(L"network remove --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_MissingNetworkName)
    {
        auto result = RunWslc(L"network remove");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'network-name'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_Valid)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyNetworkIsListed(TestNetworkName);

        result = RunWslc(std::format(L"network remove {}", TestNetworkName));
        result.Verify({.Stdout = std::format(L"{}\r\n", TestNetworkName), .Stderr = L"", .ExitCode = 0});

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_Multiple_Valid)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyNetworkIsListed(TestNetworkName);
        VerifyNetworkIsListed(TestNetworkName2);

        result = RunWslc(std::format(L"network remove {} {}", TestNetworkName, TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyNetworkIsNotListed(TestNetworkName);
        VerifyNetworkIsNotListed(TestNetworkName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_NotFound)
    {
        auto result = RunWslc(std::format(L"network remove {}", TestNetworkName));
        result.Verify({.Stdout = L"", .Stderr = std::format(L"Network not found: '{}'\r\n", TestNetworkName), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_MixedFoundNotFound)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);

        result = RunWslc(std::format(L"network remove {} {}", TestNetworkName, TestNetworkName2));
        result.Verify(
            {.Stdout = std::format(L"{}\r\n", TestNetworkName),
             .Stderr = std::format(L"Network not found: '{}'\r\n", TestNetworkName2),
             .ExitCode = 1});
        VerifyNetworkIsNotListed(TestNetworkName);
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-remove";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-remove-2";

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableCommands()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_NetworkRemoveLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc network remove [<options>] <network-name>\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: delete rm\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  network-name    Network name\r\n"         //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                   //
                << L"  --session      Specify the session to use\r\n"            //
                << L"  -?,--help      Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
