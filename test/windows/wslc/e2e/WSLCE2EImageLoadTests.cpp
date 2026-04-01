/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageLoadTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image load command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EImageLoadTests
{
    WSLC_TEST_CLASS(WSLCE2EImageLoadTests)

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

    TEST_METHOD(WSLCE2E_Image_Load_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image load --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_Load_MissingInputPath)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image load");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'input'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Load_InputFileNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image load --input /nonexistent/path/to/image.tar");
        result.Verify({.ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Load_Success)
    {
        WSL2_TEST_ONLY();

        // Save an image first
        auto tempArchivePath = wsl::windows::common::filesystem::GetTempFilename();
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", tempArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Delete the image
        auto deleteResult = RunWslc(std::format(L"image delete --force {}", DebianImage.Name));
        deleteResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Load it back
        auto loadResult = RunWslc(std::format(L"image load --input \"{}\"", tempArchivePath.wstring()));
        loadResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify it's loaded
        auto listResult = RunWslc(L"image list --no-trunc");
        listResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto outputLines = listResult.GetStdoutLines();
        bool found = false;
        for (const auto& line : outputLines)
        {
            if (line.find(DebianImage.NameAndTag()) != std::wstring::npos)
            {
                found = true;
                break;
            }
        }
        VERIFY_IS_TRUE(found, L"Image should be loaded");

        // Cleanup
        DeleteFileW(tempArchivePath.c_str());
    }

private:
    const TestImage& DebianImage = DebianTestImage();

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
        return L"Loads images.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image load [<options>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  input    Provides path to the tar archive file containing the image\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -i,--input  Provides path to the tar archive file containing the image\r\n"
                << L"  -h,--help   Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
