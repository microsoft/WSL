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

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ENetworkCreateTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkCreateTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_HelpCommand)
    {
        auto result = RunWslc(L"network create --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_MissingName)
    {
        auto result = RunWslc(L"network create");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'network-name'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Success)
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

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_InvalidDriver_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver invalid_driver {}", TestNetworkName));
        result.Verify(
            {.Stdout = L"", .Stderr = std::format(L"Unsupported network driver: 'invalid_driver'\r\nError code: E_INVALIDARG\r\n"), .ExitCode = 1});

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Duplicate_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = std::format(L"Network '{}' already exists.\r\nError code: ERROR_ALREADY_EXISTS\r\n", TestNetworkName),
             .ExitCode = 1});
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-create";

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
        return std::format(L"{}\r\n\r\n", Localization::WSLCCLI_NetworkCreateLongDesc());
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc network create [<options>] <network-name>\r\n\r\n";
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
        options << L"The following options are available:\r\n"                        //
                << L"  -d,--driver    Specify volume driver name (default guest)\r\n" //
                << L"  -o,--opt       Set driver specific options\r\n"                //
                << L"  --label        Volume metadata setting\r\n"                    //
                << L"  --session      Specify the session to use\r\n"                 //
                << L"  -?,--help      Shows help about the selected command\r\n"      //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
