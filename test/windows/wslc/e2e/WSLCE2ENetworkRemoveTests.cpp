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
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_MissingNetworkName)
    {
        auto result = RunWslc(L"network remove");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'network-name'"));
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

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_Force_NotFound)
    {
        auto result = RunWslc(std::format(L"network remove --force {}", TestNetworkName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_Force_Valid)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyNetworkIsListed(TestNetworkName);

        result = RunWslc(std::format(L"network remove --force {}", TestNetworkName));
        result.Verify({.Stdout = std::format(L"{}\r\n", TestNetworkName), .Stderr = L"", .ExitCode = 0});

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Remove_Force_MixedFoundNotFound)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsListed(TestNetworkName);

        result = RunWslc(std::format(L"network remove --force {} {}", TestNetworkName, TestNetworkName2));
        result.Verify({.Stdout = std::format(L"{}\r\n", TestNetworkName), .Stderr = L"", .ExitCode = 0});
        VerifyNetworkIsNotListed(TestNetworkName);
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-remove";
    const std::wstring TestNetworkName2 = L"wslc-e2e-network-remove-2";
};
} // namespace WSLCE2ETests
