/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumeCreateTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "VolumeModel.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::windows::wslc::models;

class WSLCE2EVolumeCreateTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeCreateTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_HelpCommand)
    {
        auto result = RunWslc(L"volume create --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_EmptyName)
    {
        auto result = RunWslc(L"volume create");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto volumeName = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(volumeName.empty());

        auto deleteVolume = wil::scope_exit([&]() {
            auto deleteResult = RunWslc(std::format(L"volume rm {}", volumeName));
            deleteResult.Verify({.Stderr = L"", .ExitCode = 0});
        });
        VerifyVolumeIsListed(volumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_DefaultDriverIsGuest)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName, result.GetStdoutOneLine());

        VerifyVolumeIsListed(TestVolumeName);
        auto inspect = InspectVolume(TestVolumeName);
        VERIFY_ARE_EQUAL("guest", inspect.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_ExplicitDriver_Success)
    {
        auto result = RunWslc(std::format(L"volume create --driver vhd --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName, result.GetStdoutOneLine());

        VerifyVolumeIsListed(TestVolumeName);
        auto inspect = InspectVolume(TestVolumeName);
        VERIFY_ARE_EQUAL("vhd", inspect.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_Vhd_MissingOpts_Fail)
    {
        auto result = RunWslc(std::format(L"volume create --driver vhd {}", TestVolumeName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Missing required option: 'SizeBytes'\r\nError code: E_INVALIDARG"));

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_InvalidDriver_Fail)
    {
        auto result =
            RunWslc(std::format(L"volume create --driver invalid_driver --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Unsupported volume type: 'invalid_driver'\r\nError code: E_INVALIDARG"));

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_WithLabel_Success)
    {
        auto result = RunWslc(std::format(L"volume create --label A=1 --label B=2 {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName, result.GetStdoutOneLine());

        VerifyVolumeIsListed(TestVolumeName);
        auto inspect = InspectVolume(TestVolumeName);
        VERIFY_ARE_EQUAL("1", inspect.Labels["A"]);
        VERIFY_ARE_EQUAL("2", inspect.Labels["B"]);
    }

private:
    const std::wstring TestVolumeName = L"wslc-e2e-volume-create";
    const int DefaultVolumeSizeBytes = 3 * 1024 * 1024;
};
} // namespace WSLCE2ETests
