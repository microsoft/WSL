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
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_MissingName)
    {
        auto result = RunWslc(L"network create");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'network-name'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_DefaultDriver_Success)
    {
        auto result = RunWslc(std::format(L"network create {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestNetworkName, result.GetStdoutOneLine());

        VerifyNetworkIsListed(TestNetworkName);
        auto inspect = InspectNetwork(TestNetworkName);
        VERIFY_ARE_EQUAL("bridge", inspect.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_BridgeDriver_Success)
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

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_EmptyLabelKey_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge --label =foo {}", TestNetworkName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Label key cannot be empty\r\nError code: E_INVALIDARG"));

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_InvalidDriver_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver invalid_driver {}", TestNetworkName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            std::format(L"Unsupported network driver: 'invalid_driver'\r\nError code: E_INVALIDARG")));

        VerifyNetworkIsNotListed(TestNetworkName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Network_Create_Duplicate_Fail)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify(
            {.Stdout = L"", .Stderr = L"Cannot create a file when that file already exists. \r\nError code: ERROR_ALREADY_EXISTS\r\n", .ExitCode = 1});
    }

private:
    const std::wstring TestNetworkName = L"wslc-e2e-network-create";
};
} // namespace WSLCE2ETests
