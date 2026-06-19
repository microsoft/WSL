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
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_MissingFile)
    {
        const auto result = RunWslc(L"image import");
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'file'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_Success)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import the tarball as a new image with a tag
        auto importResult = RunWslc(std::format(L"image import \"{}\" {}", SavedArchivePath.wstring(), ImportedImage.NameAndTag()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the imported image is listed
        VerifyImageIsListed(ImportedImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_WithoutTag)
    {
        // TODO: http://task.ms/62249460
        SKIP_TEST_UNSTABLE();

        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import without specifying an image name
        auto importResult = RunWslc(std::format(L"image import \"{}\"", SavedArchivePath.wstring()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_FromStdin_Success)
    {
        // TODO: http://task.ms/62246732
        SKIP_TEST_NOT_IMPL();
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_InvalidPath)
    {
        const auto result =
            RunWslc(std::format(L"image import \"{}\" {}", L"C:\\nonexistent\\path\\image.tar", ImportedImage.NameAndTag()));
        result.Verify({.ExitCode = 1});
    }

private:
    const TestImage DebianImage = DebianTestImage();
    const TestImage ImportedImage{L"wslc-test-imported", L"latest", L""};

    std::filesystem::path SavedArchivePath{};
};
} // namespace WSLCE2ETests
