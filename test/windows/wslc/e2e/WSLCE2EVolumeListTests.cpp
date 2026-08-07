/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVolumeListTests.cpp

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
using namespace wsl::shared::string;
using namespace wsl::windows::wslc::models;

class WSLCE2EVolumeListTests
{
    WSLC_TEST_CLASS(WSLCE2EVolumeListTests)

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureVolumeDoesNotExist(TestVolumeName);
        EnsureVolumeDoesNotExist(TestVolumeName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_HelpCommand)
    {
        auto result = RunWslc(L"volume list --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_InvalidFormatOption)
    {
        auto result = RunWslc(L"volume list --format invalid");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid format value: invalid is not a recognized format type. Supported format types are: json, table."));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_QuietOption_OutputsNamesOnly)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create {}", TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"volume list --quiet");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto lines = result.GetStdoutLines();
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestVolumeName));
        VERIFY_ARE_NOT_EQUAL(lines.end(), std::find(lines.begin(), lines.end(), TestVolumeName2));
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_List_JsonFormat)
    {
        auto result = RunWslc(std::format(L"volume create {}", TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create {}", TestVolumeName2));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(L"volume list --format json");
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto volumes = FromJson<std::vector<WSLCVolumeInformation>>(result.Stdout.value().c_str());
        VERIFY_ARE_EQUAL(2U, volumes.size());

        std::vector<std::string> names;
        names.reserve(volumes.size());
        for (const auto& volume : volumes)
        {
            names.push_back(volume.Name);
        }

        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestVolumeName)));
        VERIFY_ARE_NOT_EQUAL(names.end(), std::find(names.begin(), names.end(), WideToMultiByte(TestVolumeName2)));
    }

private:
    const std::wstring TestVolumeName = L"wslc-e2e-volume-list";
    const std::wstring TestVolumeName2 = L"wslc-e2e-volume-list-2";
};
} // namespace WSLCE2ETests
