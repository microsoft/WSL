/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerLogsTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container logs.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerLogsTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerLogsTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Logs_Tail)
    {
        // Run a container that outputs two lines
        auto result = RunWslc(std::format(
            L"container run --name {} {} sh -c \"echo line1 && echo line2\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"line1\nline2\n", .Stderr = L"", .ExitCode = 0});

        // Verify --tail 1 only shows the last line
        result = RunWslc(std::format(L"container logs --tail 1 {}", WslcContainerName));
        result.Verify({.Stdout = L"line2\n", .Stderr = L"", .ExitCode = 0});

        // Verify -n 2 shows both lines
        result = RunWslc(std::format(L"container logs -n 2 {}", WslcContainerName));
        result.Verify({.Stdout = L"line1\nline2\n", .Stderr = L"", .ExitCode = 0});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-logs";
    const TestImage& DebianImage = DebianTestImage();
};

} // namespace WSLCE2ETests
