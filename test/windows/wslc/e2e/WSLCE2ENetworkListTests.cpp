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
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_InvalidFormatOption)
    {
        auto result = RunWslc(L"network list --format invalid");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table."));
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
        VERIFY_ARE_EQUAL(2U, networks.size());

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
};
} // namespace WSLCE2ETests
