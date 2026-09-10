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
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EImageSaveTests
{
    WSLC_TEST_CLASS(WSLCE2EImageSaveTests)

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        TestImageRegistry::Instance().Delete(DebianImage);
        TestImageRegistry::Instance().Delete(AlpineImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
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
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_MissingImageName)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\"", SavedArchivePath.wstring()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Required argument not provided: 'image'"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ImageNotFound)
    {
        const auto result = RunWslc(std::format(L"image save --output \"{}\" {}", SavedArchivePath.wstring(), InvalidImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = FormatErrorMessage(L"reference does not exist", L"WSLC_E_IMAGE_NOT_FOUND"), .ExitCode = 1});
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
        TestImageRegistry::Instance().Delete(DebianImage);

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
        // A pseudo console gives wslc a console stdout, which image save must reject.
        const auto commandLine = std::format(L"image save {}", DebianImage.NameAndTag());
        auto session = RunWslcInteractive(commandLine, ElevationType::Elevated, PseudoConsole{200, 50});

        WaitForPseudoConsoleOutput(session, string::WideToMultiByte(Localization::WSLCCLI_ImageSaveStdoutIsTerminalError()));
        VERIFY_ARE_EQUAL(1, session.Wait());
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_ToStdout_Load)
    {
        // Save source image
        auto saveResult = RunWslcAndRedirectToFile(std::format(L"image save {}", DebianImage.NameAndTag()), SavedArchivePath);
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Delete source image
        TestImageRegistry::Instance().Delete(DebianImage);

        // Load from saved archive
        auto loadResult = RunWslc(std::format(L"image load --input \"{}\"", SavedArchivePath.wstring()));
        loadResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Run a container from the loaded image to verify it works
        auto runResult = RunWslc(std::format(L"container run --rm {} echo Hello from saved image!", DebianImage.NameAndTag()));
        runResult.Verify({.Stdout = L"Hello from saved image!\n", .Stderr = L"", .ExitCode = 0});
    }
    WSLC_TEST_METHOD(WSLCE2E_Image_Save_MultipleImages_Load)
    {
        TestImageRegistry::Instance().EnsureLoaded(AlpineImage);

        // Force a pristine re-load of DebianImage at the end so subsequent tests (in fast mode)
        // see the same on-disk tar as DebianImage.Path. Without this, reloading from a
        // multi-image archive can produce a slightly different on-disk representation that
        // breaks byte-exact size checks in WSLCE2E_Image_Save_Success.
        auto restoreDebian = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { TestImageRegistry::Instance().Delete(DebianImage); });

        // Save both images into a single archive.
        const auto saveResult = RunWslc(std::format(
            L"image save --output \"{}\" {} {}", SavedArchivePath.wstring(), DebianImage.NameAndTag(), AlpineImage.NameAndTag()));
        saveResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Delete both source images.
        TestImageRegistry::Instance().Delete(DebianImage);
        TestImageRegistry::Instance().Delete(AlpineImage);

        // Load both images back from the single archive.
        const auto loadResult = RunWslc(std::format(L"image load --input \"{}\"", SavedArchivePath.wstring()));
        loadResult.Verify({.Stderr = L"", .ExitCode = 0});

        auto loadedImages = wsl::shared::string::Split(loadResult.Stdout.value(), L'\n');

        VERIFY_IS_TRUE(
            std::ranges::find(loadedImages, std::format(L"Loaded image: {}\r", DebianImage.NameAndTag())) != loadedImages.end());
        VERIFY_IS_TRUE(
            std::ranges::find(loadedImages, std::format(L"Loaded image: {}\r", AlpineImage.NameAndTag())) != loadedImages.end());

        // Run a container from each loaded image to confirm both are restored and runnable.
        const auto runDebian = RunWslc(std::format(L"container run --rm {} echo ok!", DebianImage.NameAndTag()));
        runDebian.Verify({.Stdout = std::format(L"ok!\n"), .Stderr = L"", .ExitCode = 0});

        const auto runAlpine = RunWslc(std::format(L"container run --rm {} echo ok!", AlpineImage.NameAndTag()));
        runAlpine.Verify({.Stdout = std::format(L"ok!\n"), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Image_Save_MultipleImages_InvalidImage)
    {
        const auto result = RunWslc(std::format(
            L"image save --output \"{}\" {} {}", SavedArchivePath.wstring(), DebianImage.NameAndTag(), InvalidImage.NameAndTag()));
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

private:
    const TestImage DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

    std::filesystem::path SavedArchivePath{};
};
} // namespace WSLCE2ETests
