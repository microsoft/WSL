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

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_HelpCommand)
    {
        auto result = RunWslc(L"volume remove --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_MissingVolumeName)
    {
        auto result = RunWslc(L"volume remove");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'volume-name'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Valid)
    {
        auto result = RunWslc(std::format(L"volume create --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsListed(TestVolumeName);

        result = RunWslc(std::format(L"volume remove {}", TestVolumeName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Remove_Multiple_Valid)
    {
        auto result = RunWslc(std::format(L"volume create --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        result = RunWslc(std::format(L"volume create --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName2));
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
        result.Verify(
            {.Stdout = L"",
             .Stderr = std::format(L"Volume not found: '{}'\r\nError code: WSLC_E_VOLUME_NOT_FOUND\r\n", TestVolumeName),
             .ExitCode = 1});
    }

private:
    const std::wstring TestVolumeName = L"wslc-e2e-volume-remove";
    const std::wstring TestVolumeName2 = L"wslc-e2e-volume-remove-2";
    const int DefaultVolumeSizeBytes = 3 * 1024 * 1024;

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()              //
               << GetDescription()             //
               << GetUsage()                   //
               << GetAvailableCommandAliases() //
               << GetAvailableCommands()       //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_VolumeRemoveLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc volume remove [<options>] <volume-name>\r\n\r\n";
    }

    std::wstring GetAvailableCommandAliases() const
    {
        return L"The following command aliases are available: delete rm\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  volume-name    Volume name\r\n"           //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                   //
                << L"  --session      Specify the session to use\r\n"            //
                << L"  -h,--help      Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
