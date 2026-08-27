/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImagePushTests.cpp

Abstract:

    This file contains end-to-end tests for the WSLC image push command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImagePushTests
{
    WSLC_TEST_CLASS(WSLCE2EImagePushTests)

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsListedInHelp)
    {
        const auto result = RunWslc(L"image push --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsSubstring(L"--all-tags"));
        VERIFY_IS_TRUE(result.StdoutContainsSubstring(Localization::WSLCCLI_PushAllTagsArgDescription()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsRejectsTaggedReference)
    {
        const auto result = RunWslc(L"image push debian:latest --all-tags");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_AllTagsWithTagError()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Push_AllTagsRejectsDigestReference)
    {
        const auto result =
            RunWslc(L"image push debian@sha256:0000000000000000000000000000000000000000000000000000000000000000 -a");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(Localization::WSLCCLI_AllTagsWithTagError()));
    }
};
} // namespace WSLCE2ETests
