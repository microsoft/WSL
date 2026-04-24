/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkListTests.cpp

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

class WSLCE2ENetworkListTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkListTests)

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

    WSLC_TEST_METHOD(WSLCE2E_Network_List_HelpCommand)
    {
        auto result = RunWslc(L"network list --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_InvalidFormatOption)
    {
        auto result = RunWslc(L"network list --format invalid");
        result.Verify({.Stderr = L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table.\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_QuietOption_OutputsNamesOnly)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"network list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestNetworkName));
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestNetworkName2));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_JsonFormat)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"network list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto networks = FromJson<std::vector<WSLCNetworkInformation>>(result.Stdout.value().c_str());
        VERIFY_IS_TRUE(networks.size() >= 2U);

        std::vector<std::string> names;
        names.reserve(networks.size());
        for (const auto& network : networks)
        {
            names.push_back(network.Name);
        }

        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestNetworkName)));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestNetworkName2)));
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-list";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-list-2";

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return std::format(L"{}\r\n\r\n", Localization::WSLCCLI_NetworkListLongDesc());
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc network list [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: ls\r\n\r\n";
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                            //
                << L"  --format    Output formatting (json or table) (Default: table)\r\n" //
                << L"  -q,--quiet  Outputs the network names only\r\n"                   //
                << L"  --session   Specify the session to use\r\n"                       //
                << L"  -?,--help   Shows help about the selected command\r\n"            //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
