/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageSaveTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image save.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EImageSaveTests
{
    WSLC_TEST_CLASS(WSLCE2EImageSaveTests)

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        SavedArchivePath = wsl::windows::common::filesystem::GetTempFilename();
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        DeleteFileW(SavedArchivePath.c_str());
        return true;
    }

    TEST_METHOD(WSLCE2E_Image_Save_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"image save --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Image_Save_MissingImageName)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(std::format(L"image save --output \"{}\"", SavedArchivePath.wstring()));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Save_MissingOutputPath)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(std::format(L"image save {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Required argument not provided: 'output'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Save_ImageNotFound)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"reference does not exist\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Image_Save_Success)
    {
        WSL2_TEST_ONLY();

        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(SavedArchivePath));
        auto sourceFileSize = std::filesystem::file_size(DebianImage.Path);
        auto archiveFileSize = std::filesystem::file_size(SavedArchivePath);
        VERIFY_ARE_EQUAL(sourceFileSize, archiveFileSize);
    }

    TEST_METHOD(WSLCE2E_Image_Save_Load)
    {
        WSL2_TEST_ONLY();

        // Save source image
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Delete source image
        EnsureImageIsDeleted(DebianImage);

        // Load from saved archive
        auto loadResult = RunWslc(std::format(L"image load --input \"{}\"", SavedArchivePath.wstring()));
        loadResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Run a container from the loaded image to verify it works
        auto runResult = RunWslc(std::format(L"container run --rm {} echo Hello from saved image!", DebianImage.NameAndTag()));
        runResult.Verify({.Stdout = L"Hello from saved image!\n", .Stderr = L"", .ExitCode = 0});
    }

private:
    const TestImage DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

    std::filesystem::path SavedArchivePath{};

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
        return L"Saves images.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image save [<options>] <image>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  image        Image name\r\n"              //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                 //
                << L"  -o,--output  Path for the saved image\r\n"              //
                << L"  --session    Specify the session to use\r\n"            //
                << L"  -h,--help    Shows help about the selected command\r\n" //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests