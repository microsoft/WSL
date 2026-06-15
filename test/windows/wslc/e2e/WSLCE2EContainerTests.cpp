/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "WSLCExecutor.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_InvalidCommand_DisplaysErrorMessage)
    {
        const auto result = RunWslc(L"container INVALID_CMD");
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unrecognized command: 'INVALID_CMD'"));
    }
};
} // namespace WSLCE2ETests
