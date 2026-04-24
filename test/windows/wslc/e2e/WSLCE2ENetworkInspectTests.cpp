/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::shared::string;

class WSLCE2ENetworkInspectTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkInspectTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName1);
        EnsureNetworkDoesNotExist(TestNetworkName2);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureNetworkDoesNotExist(TestNetworkName1);
        EnsureNetworkDoesNotExist(TestNetworkName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"network inspect --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Inspect_MissingNetworkName)
    {
        auto result = RunWslc(L"network inspect");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'network-name'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Inspect_Success)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName1, result.GetStdoutOneLine());

        result = RunWslc(std::format(L"network inspect {}", TestNetworkName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectNetwork>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        auto inspect = inspectData[0];

        VERIFY_ARE_EQUAL(WideToMultiByte(TestNetworkName1), inspect.Name);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_InspectMultiple_Success)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName1, result.GetStdoutOneLine());
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName2, result.GetStdoutOneLine());

        result = RunWslc(std::format(L"network inspect {} {}", TestNetworkName1, TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectNetwork>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(2u, inspectData.size());

        auto inspect1 = inspectData[0];
        VERIFY_ARE_EQUAL(WideToMultiByte(TestNetworkName1), inspect1.Name);
        VERIFY_ARE_EQUAL("bridge", inspect1.Driver);

        auto inspect2 = inspectData[1];
        VERIFY_ARE_EQUAL(WideToMultiByte(TestNetworkName2), inspect2.Name);
        VERIFY_ARE_EQUAL("bridge", inspect2.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Inspect_NotFound)
    {
        auto result = RunWslc(std::format(L"network inspect {}", TestNetworkName1));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Network not found: '{}'\r\n", TestNetworkName1), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Inspect_MixedFoundNotFound)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName1, result.GetStdoutOneLine());

        result = RunWslc(std::format(L"network inspect {} {}", TestNetworkName1, TestNetworkName2));
        result.Verify({.Stderr = std::format(L"Network not found: '{}'\r\n", TestNetworkName2), .ExitCode = 1});

        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectNetwork>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        auto inspect = inspectData[0];
        VERIFY_ARE_EQUAL(WideToMultiByte(TestNetworkName1), inspect.Name);
    }

private:
    const std::wstring TestNetworkName1 = L"wslc-e2e-network-inspect-1";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-inspect-2";

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
        return std::format(L"{}\r\n\r\n", Localization::WSLCCLI_NetworkInspectLongDesc());
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc network inspect [<options>] <network-name>\r\n\r\n";
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
