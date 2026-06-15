/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <wslc_schema.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EImageInspectTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_MissingImageName)
    {
        const auto result = RunWslc(L"image inspect");
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value_or(0));
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'image'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_ImageNotFound)
    {
        auto result = RunWslc(std::format(L"image inspect {}", InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Image '{}' not found.\r\n", InvalidImage.NameAndTag()), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Inspect_Success)
    {
        auto result = RunWslc(std::format(L"image inspect {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectImage>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        VERIFY_IS_TRUE(inspectData[0].RepoTags.has_value());
        VERIFY_ARE_EQUAL(1u, inspectData[0].RepoTags.value().size());
        VERIFY_ARE_EQUAL(DebianImage.NameAndTag(), wsl::shared::string::MultiByteToWide(inspectData[0].RepoTags.value()[0]));
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
};
} // namespace WSLCE2ETests