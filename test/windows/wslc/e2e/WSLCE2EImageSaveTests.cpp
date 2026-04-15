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
using namespace wsl::shared;

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

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_HelpCommand)
    {
        auto result = RunWslc(L"image save --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_MissingImageName)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\"", SavedArchivePath.wstring()));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ImageNotFound)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"reference does not exist\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_Success)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(SavedArchivePath));
        auto sourceFileSize = std::filesystem::file_size(DebianImage.Path);
        auto archiveFileSize = std::filesystem::file_size(SavedArchivePath);
        VERIFY_ARE_EQUAL(sourceFileSize, archiveFileSize);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_Load)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToStdout_Success)
    {
        const auto result = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()), SavedArchivePath);
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(std::filesystem::exists(SavedArchivePath));
        auto sourceFileSize = std::filesystem::file_size(DebianImage.Path);
        auto archiveFileSize = std::filesystem::file_size(SavedArchivePath);
        VERIFY_ARE_EQUAL(sourceFileSize, archiveFileSize);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToTerminal_Fail)
    {
        const auto result = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"Cannot write image to terminal. Use the -o flag or redirect stdout.\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToStdout_Load)
    {
        // Save source image
        auto saveResult = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()), SavedArchivePath);
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
        return Localization::WSLCCLI_ImageSaveLongDesc() + L"\r\n\r\n";
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