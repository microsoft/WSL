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
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_EmptyName)
    {
        auto result = RunWslc(std::format(L"volume create --opt SizeBytes={} ", DefaultVolumeSizeBytes));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto volumeName = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(volumeName.empty());

        auto deleteVolume = wil::scope_exit([&]()
        {
            auto deleteResult = RunWslc(std::format(L"volume rm {}", volumeName));
            deleteResult.Verify({.Stderr = L"", .ExitCode = 0});
        });
        VerifyVolumeIsListed(volumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_DefaultDriverIsVhd)
    {
        auto result = RunWslc(std::format(L"volume create --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_ARE_EQUAL(TestVolumeName, result.GetStdoutOneLine());

        VerifyVolumeIsListed(TestVolumeName);
        auto inspect = InspectVolume(TestVolumeName);
        VERIFY_ARE_EQUAL("vhd", inspect.Driver);
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
        result.Verify({.Stderr = L"Missing required option: 'SizeBytes'\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});

        VerifyVolumeIsNotListed(TestVolumeName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Volume_Create_InvalidDriver_Fail)
    {
        auto result =
            RunWslc(std::format(L"volume create --driver invalid_driver --opt SizeBytes={} {}", DefaultVolumeSizeBytes, TestVolumeName));
        result.Verify({.Stdout = L"", .Stderr = L"Unsupported volume type: 'invalid_driver'\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});

        VerifyVolumeIsNotListed(TestVolumeName);
    }

private:
    const std::wstring TestVolumeName = L"wslc-e2e-volume-create";
    const int DefaultVolumeSizeBytes = 3 * 1024 * 1024;

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_VolumeCreateLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc volume create [<options>] [<volume-name>]\r\n\r\n";
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
        options << L"The following options are available:\r\n"                      //
                << L"  -d,--driver    Specify volume driver name (default vhd)\r\n" //
                << L"  -o,--opt       Set driver specific options\r\n"              //
                << L"  --label        Volume metadata setting\r\n"                //
                << L"  --session      Specify the session to use\r\n"               //
                << L"  -?,--help      Shows help about the selected command\r\n"    //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
