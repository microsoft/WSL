/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumeRemoveTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EVolumeRemoveTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeRemoveTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_HelpCommand)
    {
        auto result = RunWslc(L"volume remove --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_MissingVolumeName)
    {
        auto result = RunWslc(L"volume remove");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'volume-name'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Valid)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsListed(TestVolumeName);

        result = RunWslc(std::format(L"volume remove {}", TestVolumeName));
        result.Verify({.Stdout = std::format(L"{}\r\n", TestVolumeName), .Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Multiple_Valid)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create {}", TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsListed(TestVolumeName);
        VerifyVolumeIsListed(TestVolumeName2);

        result = RunWslc(std::format(L"volume remove {} {}", TestVolumeName, TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsNotListed(TestVolumeName);
        VerifyVolumeIsNotListed(TestVolumeName2);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_NotFound)
    {
        auto result = RunWslc(std::format(L"volume remove {}", TestVolumeName));
        result.Verify({.Stdout = L"", .Stderr = std::format(L"Volume not found: '{}'\r\n", TestVolumeName), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_MixedFoundNotFound)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        result = RunWslc(std::format(L"volume remove {} {}", TestVolumeName, TestVolumeName2));
        result.Verify(
            {.Stdout = std::format(L"{}\r\n", TestVolumeName), .Stderr = std::format(L"Volume not found: '{}'\r\n", TestVolumeName2), .ExitCode = 1});
        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_VolumeInUse_Fail)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        // Create a container that uses the volume to ensure it's in use
        result = RunWslc(std::format(
            L"container run -d --name {} -v {}:/data {} sh -c \"echo -n 'WSLC Volume In Use Test' > /data/test.txt && sleep "
            L"infinity\"",
            WslcContainerName,
            TestVolumeName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Attempt to remove the volume while it's in use
        result = RunWslc(std::format(L"volume remove {}", TestVolumeName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = std::format(L"Volume '{}' is in use.\r\nError code: ERROR_SHARING_VIOLATION\r\n", TestVolumeName),
             .ExitCode = 1});

        VerifyVolumeIsListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Force_NotFound)
    {
        auto result = RunWslc(std::format(L"volume remove --force {}", TestVolumeName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Force_Valid)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsListed(TestVolumeName);

        result = RunWslc(std::format(L"volume remove --force {}", TestVolumeName));
        result.Verify({.Stdout = std::format(L"{}\r\n", TestVolumeName), .Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Force_MixedFoundNotFound)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        result = RunWslc(std::format(L"volume remove --force {} {}", TestVolumeName, TestVolumeName2));
        result.Verify({.Stdout = std::format(L"{}\r\n", TestVolumeName), .Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Force_VolumeInUse_Fail)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyVolumeIsListed(TestVolumeName);

        // Create a container that uses the volume to ensure it's in use
        result = RunWslc(std::format(
            L"container run -d --name {} -v {}:/data {} sh -c \"echo -n 'WSLC Volume In Use Test' > /data/test.txt && sleep "
            L"infinity\"",
            WslcContainerName,
            TestVolumeName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // --force does not bypass in-use checks, volume should still fail to be removed
        result = RunWslc(std::format(L"volume remove --force {}", TestVolumeName));
        result.Verify(
            {.Stdout = L"",
             .Stderr = std::format(L"Volume '{}' is in use.\r\nError code: ERROR_SHARING_VIOLATION\r\n", TestVolumeName),
             .ExitCode = 1});

        VerifyVolumeIsListed(TestVolumeName);
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const std::wstring TestVolumeName = L"wslc-e2e-volume-remove";
    const std::wstring TestVolumeName2 = L"wslc-e2e-volume-remove-2";
};
} // namespace WSLCE2ETests
