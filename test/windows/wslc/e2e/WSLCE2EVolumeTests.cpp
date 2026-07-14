/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumeTests.cpp

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

class WSLCE2EVolumeTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeTests)

    WSLC_TEST_METHOD(WSLCE2E_Volume_HelpCommand)
    {
        auto result = RunWslc(L"volume --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_NoSubcommand_ShowsHelp)
    {
        auto result = RunWslc(L"volume");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_InvalidCommand_DisplaysErrorMessage)
    {
        auto result = RunWslc(L"volume INVALID_CMD");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unrecognized command: 'INVALID_CMD'"));
    }
};
} // namespace WSLCE2ETests