/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2ENetworkPruneTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC network prune command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2ENetworkPruneTests
{
    WSLC_TEST_CLASS(WSLCE2ENetworkPruneTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        CleanUpAllTestState();
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        CleanUpAllTestState();
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        CleanUpAllTestState();
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_HelpCommand)
    {
        const auto result = RunWslc(L"network prune --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_NoNetworks)
    {
        // Prune when no unused networks exist should succeed without reporting any of our test networks.
        const auto result = RunWslc(L"network prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_FALSE(result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)));
        VERIFY_IS_FALSE(result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_RemovesUnusedNetwork)
    {
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);

        auto cleanup = wil::scope_exit([&]() { EnsureNetworkDoesNotExist(TestNetworkName); });

        const auto result = RunWslc(L"network prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto output = result.GetStdoutLines();
        VERIFY_ARE_EQUAL(1u, output.size());
        VERIFY_ARE_NOT_EQUAL(std::wstring::npos, output[0].find(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)));

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_RemovesMultipleNetworks)
    {
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);
        VerifyNetworkIsListed(TestNetworkName2);

        auto cleanup = wil::scope_exit([&]() {
            EnsureNetworkDoesNotExist(TestNetworkName);
            EnsureNetworkDoesNotExist(TestNetworkName2);
        });

        const auto result = RunWslc(L"network prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName2)));

        VerifyNetworkIsNotListed(TestNetworkName);
        VerifyNetworkIsNotListed(TestNetworkName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_InUseNetwork_Preserved)
    {
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);

        // Start a container attached to the network so it is considered in use.
        RunWslc(std::format(
                    L"container run -d --name {} --network {} {} sleep infinity", WslcContainerName, TestNetworkName, DebianImage.NameAndTag()))
            .Verify({.Stderr = L"", .ExitCode = 0});

        auto cleanup = wil::scope_exit([&]() {
            EnsureContainerDoesNotExist(WslcContainerName);
            EnsureNetworkDoesNotExist(TestNetworkName);
        });

        const auto result = RunWslc(L"network prune");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_FALSE(
            result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)),
            L"Network in use by a running container must not be pruned");

        VerifyNetworkIsListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_LabelFilter_PreservesNonMatchingNetwork)
    {
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);

        auto cleanup = wil::scope_exit([&]() { EnsureNetworkDoesNotExist(TestNetworkName); });

        // A label filter that does not match the network should preserve it.
        const auto filteredPrune = RunWslc(L"network prune --filter label=wslc.test.never=present");
        filteredPrune.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(
            filteredPrune.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)),
            L"Filtered prune should not have deleted the non-matching network");
        VerifyNetworkIsListed(TestNetworkName);

        // A subsequent unfiltered prune should still remove it, proving the filter
        // was the reason it survived.
        const auto unfilteredPrune = RunWslc(L"network prune");
        unfilteredPrune.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(unfilteredPrune.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)));
        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_LabelFilter_MatchingValueIsDeleted)
    {
        RunWslc(std::format(L"network create --driver bridge --label wslc.test.prune=keep {}", TestNetworkName)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);
        VerifyNetworkIsListed(TestNetworkName2);

        auto cleanup = wil::scope_exit([&]() {
            EnsureNetworkDoesNotExist(TestNetworkName);
            EnsureNetworkDoesNotExist(TestNetworkName2);
        });

        const auto result = RunWslc(L"network prune --filter label=wslc.test.prune=keep");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)));
        VERIFY_IS_FALSE(
            result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName2)),
            L"Network without the matching label must not be deleted");

        VerifyNetworkIsNotListed(TestNetworkName);
        VerifyNetworkIsListed(TestNetworkName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_NegatedLabelFilter_PreservesLabeledNetwork)
    {
        RunWslc(std::format(L"network create --driver bridge --label wslc.test.keep=yes {}", TestNetworkName)).Verify({.Stderr = L"", .ExitCode = 0});
        RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName2)).Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);
        VerifyNetworkIsListed(TestNetworkName2);

        auto cleanup = wil::scope_exit([&]() {
            EnsureNetworkDoesNotExist(TestNetworkName);
            EnsureNetworkDoesNotExist(TestNetworkName2);
        });

        const auto result = RunWslc(L"network prune --filter label!=wslc.test.keep");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName2)));
        VERIFY_IS_FALSE(
            result.StdoutContainsLine(Localization::WSLCCLI_NetworkPruneDeleted(TestNetworkName)),
            L"Labeled network must be preserved when prune negates that label");

        VerifyNetworkIsListed(TestNetworkName);
        VerifyNetworkIsNotListed(TestNetworkName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_Filter_MalformedValue)
    {
        const auto result = RunWslc(L"network prune --filter label");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_InvalidFilterError(L"label")));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Prune_Filter_InvalidKey)
    {
        const auto result = RunWslc(L"network prune --filter color=red");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"invalid filter 'color'\r\nError code: E_INVALIDARG"));
    }

private:
    const TestImage& DebianImage = DebianTestImage();
    const std::wstring TestNetworkName = L"wslc-e2e-network-prune";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-prune-2";
    const std::wstring WslcContainerName = L"wslc-network-prune-test-container";

    void CleanUpAllTestState()
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureNetworkDoesNotExist(TestNetworkName);
        EnsureNetworkDoesNotExist(TestNetworkName2);
    }
};
} // namespace WSLCE2ETests
