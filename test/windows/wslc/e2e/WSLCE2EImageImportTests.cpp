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
#include <ImageModel.h>

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
        result.Verify({.ExitCode = 1});
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Required argument not provided: 'file'") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_Success)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import the tarball as a new image with a tag
        auto importResult = RunWslc(std::format(L"image import \"{}\" {}", SavedArchivePath.wstring(), ImportedImage.NameAndTag()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the import outputs a truncated image ID (12 chars + \r\n)
        VERIFY_IS_TRUE(importResult.Stdout.has_value());
        VERIFY_ARE_EQUAL(importResult.Stdout->size(), static_cast<size_t>(12 + 2));

        // Verify the imported image is listed
        VerifyImageIsListed(ImportedImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_Success_NoTrunc)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Import with --no-trunc
        auto importResult =
            RunWslc(std::format(L"image import --no-trunc \"{}\" {}", SavedArchivePath.wstring(), ImportedImage.NameAndTag()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the import outputs a full-length image ID (> 12 chars)
        VERIFY_IS_TRUE(importResult.Stdout.has_value());
        VERIFY_IS_TRUE(importResult.Stdout->size() > 14);

        // Verify the imported image is listed
        VerifyImageIsListed(ImportedImage);
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Import_WithoutTag)
    {
        // Save image as a tarball
        auto saveResult = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), DebianImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        auto countUntaggedImages = [&]() {
            auto result = RunWslc(L"image list --format json");
            result.Verify({.Stderr = L"", .ExitCode = 0});
            auto images = FromJson<std::vector<wsl::windows::wslc::models::ImageInformation>>(result.Stdout.value().c_str());
            size_t count = 0;
            for (const auto& img : images)
            {
                if (!img.Repository.has_value() || img.Repository.value() == "<none>")
                {
                    count++;
                }
            }
            return count;
        };

        auto untaggedBefore = countUntaggedImages();

        // Import without specifying an image name — creates an untagged image
        auto importResult = RunWslc(std::format(L"image import \"{}\"", SavedArchivePath.wstring()));
        importResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the import outputs an image ID
        VERIFY_IS_TRUE(importResult.Stdout.has_value());
        VERIFY_IS_FALSE(importResult.Stdout->empty());

        // Verify that there is now one more untagged image
        VERIFY_ARE_EQUAL(countUntaggedImages(), untaggedBefore + 1);
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
