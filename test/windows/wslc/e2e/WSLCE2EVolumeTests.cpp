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

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EVolumeTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeTests)

    WSLC_TEST_METHOD(WSLCE2E_Volume_InvalidCommand_DisplaysErrorMessage)
    {
        const auto result = RunWslc(L"volume INVALID_CMD");
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unrecognized command: 'INVALID_CMD'"));
    }
};
} // namespace WSLCE2ETests
