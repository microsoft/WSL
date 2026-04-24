/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EImageImportTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC image import.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageImportTests
{
    WSLC_TEST_CLASS(WSLCE2EImageImportTests)

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(ImportedImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsDeleted(ImportedImage);
        SavedArchivePath = wsl::windows::common::filesystem::GetTempFilename();
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        DeleteFileW(SavedArchivePath.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_HelpCommand)
    {
        auto result = RunWslc(L"image import --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_MissingFile)
    {
        const auto result = RunWslc(L"image import");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'file'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_Success)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import the tarball as a new image with a tag
        auto importResult =
            RunWslc(std::format(L"image import \"{}\" {}", SavedArchivePath.wstring(), ImportedImage.NameAndTag()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the imported image is listed
        VerifyImageIsListed(ImportedImage.NameAndTag());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_WithoutTag)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import without specifying an image name
        auto importResult = RunWslc(std::format(L"image import \"{}\"", SavedArchivePath.wstring()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_InvalidPath)
    {
        const auto result =
            RunWslc(std::format(L"image import \"{}\" {}", L"C:\\nonexistent\\path\\image.tar", ImportedImage.NameAndTag()));
        result.Verify({.ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_FromRoot)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import using the root 'import' alias
        auto importResult =
            RunWslc(std::format(L"import \"{}\" {}", SavedArchivePath.wstring(), ImportedImage.NameAndTag()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the imported image is listed
        VerifyImageIsListed(ImportedImage.NameAndTag());
    }

private:
    const TestImage DebianImage = DebianTestImage();
    const TestImage ImportedImage{L"wslc-test-imported", L"latest", L""};

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
        return Localization::WSLCCLI_ImageImportLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc image import [<options>] <file> [<image>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"                                             //
                 << L"  file       " << Localization::WSLCCLI_ImportFileArgDescription() << L"\r\n"          //
                 << L"  image      " << Localization::WSLCCLI_ImageIdArgDescription() << L"\r\n"             //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"                                                 //
                << L"  --session  " << Localization::WSLCCLI_SessionIdArgDescription() << L"\r\n"             //
                << L"  -?,--help  " << Localization::WSLCCLI_HelpArgDescription() << L"\r\n"                  //
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
