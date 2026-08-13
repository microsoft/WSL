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

    WSLC_TEST_METHOD(WSLCE2E_Network_List_QuietOption_OutputsIdsOnly)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"network list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        std::vector<std::wstring> expectedIds;
        for (const auto& network : ParseNdjsonOutput(result))
        {
            expectedIds.push_back(MultiByteToWide(network.at("ID").get<std::string>()));
        }

        result = RunWslc(L"network list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(expectedIds.size(), lines.size());
        for (const auto& id : expectedIds)
        {
            VerifyIdOutput(id, true);
            VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), id));
        }

        VERIFY_ARE_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestNetworkName));
        VERIFY_ARE_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestNetworkName2));

        result = RunWslc(L"network list --format json --no-trunc");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        expectedIds.clear();
        for (const auto& network : ParseNdjsonOutput(result))
        {
            expectedIds.push_back(MultiByteToWide(network.at("ID").get<std::string>()));
        }

        result = RunWslc(L"network list --quiet --no-trunc");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        lines = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(expectedIds.size(), lines.size());
        for (const auto& id : expectedIds)
        {
            VerifyIdOutput(id, false);
            VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), id));
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_List_JsonFormat)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"network list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto networks = ParseNdjsonOutput(result);

        std::vector<std::string> names;
        names.reserve(networks.size());
        for (const auto& network : networks)
        {
            VERIFY_ARE_EQUAL(3u, network.size());
            VERIFY_IS_TRUE(network.contains("ID"));
            VERIFY_IS_FALSE(network.contains("Id"));
            VERIFY_IS_TRUE(network.contains("Name"));
            VERIFY_IS_TRUE(network.contains("Driver"));
            VerifyIdOutput(MultiByteToWide(network.at("ID").get<std::string>()), true);
            names.push_back(network.at("Name").get<std::string>());
        }

        VERIFY_ARE_EQUAL(1u, static_cast<size_t>(std::count(names.begin(), names.end(), WideToMultiByte(TestNetworkName))));
        VERIFY_ARE_EQUAL(1u, static_cast<size_t>(std::count(names.begin(), names.end(), WideToMultiByte(TestNetworkName2))));

        auto quietResult = RunWslc(L"network list --format json --quiet");
        quietResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(result.Stdout.value(), quietResult.Stdout.value());

        auto fullResult = RunWslc(L"network list --format json --no-trunc");
        fullResult.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& network : ParseNdjsonOutput(fullResult))
        {
            VerifyIdOutput(MultiByteToWide(network.at("ID").get<std::string>()), false);
        }

        auto fullQuietResult = RunWslc(L"network list --format json --quiet --no-trunc");
        fullQuietResult.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(fullResult.Stdout.value(), fullQuietResult.Stdout.value());

        auto fullTableResult = RunWslc(L"network list --no-trunc");
        fullTableResult.Verify({.Stderr = L"", .ExitCode = 0});
        for (const auto& network : ParseNdjsonOutput(fullResult))
        {
            const auto id = MultiByteToWide(network.at("ID").get<std::string>());
            VERIFY_IS_TRUE(fullTableResult.StdoutContainsSubstring(id));
        }
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-list";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-list-2";
};
} // namespace WSLCE2ETests
