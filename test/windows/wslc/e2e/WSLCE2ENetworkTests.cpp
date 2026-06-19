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
};
} // namespace WSLCE2ETests
