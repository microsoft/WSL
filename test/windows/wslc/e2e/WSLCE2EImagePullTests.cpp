/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImagePullTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC image pull command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImagePullTests
{
    WSLC_TEST_CLASS(WSLCE2EImagePullTests)

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsListedInHelp)
    {
        const auto result = RunWslc(L"image pull --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"--all-tags"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_AllTagsArgDescription()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsRejectsTaggedReference)
    {
        const auto result = RunWslc(L"image pull debian:latest --all-tags");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_PullAllTagsWithTagError()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Pull_AllTagsRejectsDigestReference)
    {
        const auto result =
            RunWslc(L"image pull debian@sha256:0000000000000000000000000000000000000000000000000000000000000000 -a");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_PullAllTagsWithTagError()));
    }
};
} // namespace WSLCE2ETests
