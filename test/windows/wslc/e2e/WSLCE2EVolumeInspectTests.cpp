/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumeInspectTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;
using namespace wsl::shared::string;

class WSLCE2EVolumeInspectTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeInspectTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName1);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName1);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Inspect_HelpCommand)
    {
        auto result = RunWslc(L"volume inspect --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Inspect_MissingVolumeName)
    {
        auto result = RunWslc(L"volume inspect");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'volume-name'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Inspect_Success)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName1, result.GetStdoutOneLine());

        result = RunWslc(std::format(L"volume inspect {}", TestVolumeName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        auto inspect = inspectData[0];

        VERIFY_ARE_EQUAL(WideToMultiByte(TestVolumeName1), inspect.Name);
        VERIFY_ARE_EQUAL("guest", inspect.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_InspectMultiple_Success)
    {
        // Create two volumes to inspect at the same time
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName1, result.GetStdoutOneLine());
        result = RunWslc(std::format(L"volume create {}", TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName2, result.GetStdoutOneLine());

        // Inspect both volumes in the same command
        result = RunWslc(std::format(L"volume inspect {} {}", TestVolumeName1, TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(2u, inspectData.size());

        auto inspect1 = inspectData[0];
        VERIFY_ARE_EQUAL(WideToMultiByte(TestVolumeName1), inspect1.Name);
        VERIFY_ARE_EQUAL("guest", inspect1.Driver);

        auto inspect2 = inspectData[1];
        VERIFY_ARE_EQUAL(WideToMultiByte(TestVolumeName2), inspect2.Name);
        VERIFY_ARE_EQUAL("guest", inspect2.Driver);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Inspect_NotFound)
    {
        auto result = RunWslc(std::format(L"volume inspect {}", TestVolumeName1));
        result.Verify({.Stdout = L"[]\r\n", .Stderr = std::format(L"Volume not found: '{}'\r\n", TestVolumeName1), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Inspect_MixedFoundNotFound)
    {
        // Create one volume but not the other
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName1));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName1, result.GetStdoutOneLine());

        // Inspect both volumes in the same command, expecting one to be found and the other to not be found
        result = RunWslc(std::format(L"volume inspect {} {}", TestVolumeName1, TestVolumeName2));
        result.Verify({.Stderr = std::format(L"Volume not found: '{}'\r\n", TestVolumeName2), .ExitCode = 1});

        // Verify found volume
        auto inspectData =
            wsl::shared::FromJson<std::vector<wsl::windows::common::wslc_schema::InspectVolume>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(1u, inspectData.size());
        auto inspect = inspectData[0];
        VERIFY_ARE_EQUAL(WideToMultiByte(TestVolumeName1), inspect.Name);
    }

private:
    const std::wstring TestVolumeName1 = L"wslc-e2e-volume-inspect-1";
    const std::wstring TestVolumeName2 = L"wslc-e2e-volume-inspect-2";
};
} // namespace WSLCE2ETests
